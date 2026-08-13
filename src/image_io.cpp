#include "procedural_visualizer_tool.h"
#include "path_utf8.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <io.h>
#  include <process.h>
#  include <windows.h>
#  include "windows_file_install.h"
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#  if defined(__APPLE__)
#    include <sys/stdio.h>
#  endif
#endif

namespace pvt {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kSequenceDitherSeed = 0x50565431U; // "PVT1"
constexpr std::size_t kMaximumOutputPathBytes = 64U * 1024U;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

void clear_error(std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t* result) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool checked_add_u64(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

std::string system_error_message(const char* action, const fs::path& path, int code) {
    std::string message(action);
    message += " '";
    message += path.string();
    message += "': ";
    // generic_category().message() does not expose strerror's potentially
    // shared static buffer, so concurrent exports retain reliable diagnostics.
    message += std::generic_category().message(code);
    return message;
}

bool valid_dither_method(DitherMethod method) {
    switch (method) {
        case DitherMethod::BlueNoise:
        case DitherMethod::OrderedBayer:
        case DitherMethod::FloydSteinberg:
            return true;
    }
    return false;
}

bool writes_alpha_channel(const RenderConfig& config) {
    // AlphaConfig::enabled remains an RGBA request for legacy single-render
    // callers. Project rendering uses the independent global export flag.
    return config.alpha.enabled || config.output.write_alpha;
}

bool valid_prefix(const std::string& prefix) {
    if (prefix.empty()) {
        return false;
    }
    for (char raw_character : prefix) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '/' || character == '\\' || character == '<'
            || character == '>' || character == ':' || character == '"'
            || character == '|' || character == '?' || character == '*'
            || character < 0x20U || character == 0x7fU) {
            return false;
        }
    }
    return true;
}

bool validate_image_for_export(const Image& image,
                               const RenderConfig& config,
                               std::string* error) {
    if (image.width <= 0 || image.height <= 0) {
        return fail(error, "The image dimensions must be positive.");
    }
    if (image.width != config.width || image.height != config.height) {
        return fail(error, "The image dimensions do not match the render configuration.");
    }

    std::size_t pixel_count = 0;
    std::size_t component_count = 0;
    if (!checked_multiply(static_cast<std::size_t>(image.width),
                          static_cast<std::size_t>(image.height), &pixel_count)
        || !checked_multiply(pixel_count, 4U, &component_count)) {
        return fail(error, "The image dimensions overflow the addressable pixel buffer size.");
    }
    if (image.pixels.size() != component_count) {
        return fail(error, "The image must contain exactly four float components per pixel.");
    }

    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::size_t offset = pixel * 4U;
        if (!std::isfinite(image.pixels[offset])
            || !std::isfinite(image.pixels[offset + 1U])
            || !std::isfinite(image.pixels[offset + 2U])) {
            return fail(error, "The image contains a non-finite RGB component.");
        }
        const float alpha = image.pixels[offset + 3U];
        if (!std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
            return fail(error, "The image contains alpha outside the finite [0, 1] range.");
        }
    }

    if (config.output.bit_depth != 8 && config.output.bit_depth != 16
        && config.output.bit_depth != 32) {
        return fail(error, "Export bit depth must be 8, 16, or 32.");
    }
    if (!valid_dither_method(config.output.dither_method)) {
        return fail(error, "The selected dithering method is invalid.");
    }
    return true;
}

double clamp_unit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double linear_to_srgb(double value) {
    value = clamp_unit(value);
    if (value <= 0.0031308) {
        return value * 12.92;
    }
    return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

double encoded_component(const float* pixel, int channel) {
    // Alpha is linear coverage. RGB is converted to display-referred sRGB only
    // at PNG export; EXR retains the original linear float values.
    return channel == 3 ? clamp_unit(pixel[3]) : linear_to_srgb(pixel[channel]);
}

constexpr std::array<std::uint8_t, 64> kBayer8x8 = {{
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
}};

double blue_noise_like(int x, int y, int channel, std::uint32_t seed) {
    // Interleaved gradient noise is a compact, low-clumping spatial sequence.
    // Seed and channel alter only its fixed spatial offset; render_sequence uses
    // one seed for every frame so the quantization texture cannot flicker.
    const double shifted_x = static_cast<double>(x)
                             + static_cast<double>(seed & 0x0fffU)
                             + static_cast<double>(channel * 37);
    const double shifted_y = static_cast<double>(y)
                             + static_cast<double>((seed >> 12U) & 0x0fffU)
                             + static_cast<double>(channel * 53);
    const double inner = std::fmod(0.06711056 * shifted_x
                                   + 0.00583715 * shifted_y, 1.0);
    return std::fmod(52.9829189 * inner, 1.0) - 0.5;
}

double ordered_bayer(int x, int y, int channel) {
    const int shifted_x = (x + channel * 3) & 7;
    const int shifted_y = (y + channel * 5) & 7;
    const std::uint8_t rank = kBayer8x8[static_cast<std::size_t>(shifted_y * 8
                                                                + shifted_x)];
    return (static_cast<double>(rank) + 0.5) / 64.0 - 0.5;
}

double spatial_dither(DitherMethod method,
                      int x,
                      int y,
                      int channel,
                      std::uint32_t seed) {
    switch (method) {
        case DitherMethod::BlueNoise:
            return blue_noise_like(x, y, channel, seed);
        case DitherMethod::OrderedBayer:
            return ordered_bayer(x, y, channel);
        case DitherMethod::FloydSteinberg:
            return 0.0;
    }
    return 0.0;
}

std::uint32_t quantize_component(double encoded,
                                 std::uint32_t maximum,
                                 double dither) {
    const double scaled = clamp_unit(encoded) * static_cast<double>(maximum) + dither;
    const double bounded = std::max(0.0, std::min(static_cast<double>(maximum), scaled));
    return static_cast<std::uint32_t>(std::llround(bounded));
}

struct PngErrorContext {
    char message[256];
};

void png_error_callback(png_structp png, png_const_charp message) {
    auto* context = static_cast<PngErrorContext*>(png_get_error_ptr(png));
    if (context != nullptr) {
        const char* source = message != nullptr ? message : "unknown libpng error";
        std::snprintf(context->message, sizeof(context->message), "%s", source);
    }
    png_longjmp(png, 1);
}

bool write_png_stream(std::FILE* file,
                      const Image& image,
                      const RenderConfig& config,
                      std::uint32_t seed,
                      const std::atomic_bool* cancel,
                      std::string* error) {
    const int channels = writes_alpha_channel(config) ? 4 : 3;
    const int bit_depth = config.output.bit_depth;
    if (image.width <= 0 || image.height <= 0) {
        return fail(error, "PNG dimensions must be positive.");
    }
    if (bit_depth != 8 && bit_depth != 16) {
        return fail(error, "PNG bit depth must be either 8 or 16.");
    }
    const std::size_t bytes_per_component = bit_depth == 16 ? 2U : 1U;
    std::size_t row_components = 0;
    std::size_t row_bytes = 0;
    std::size_t error_values = 0;
    if (!checked_multiply(static_cast<std::size_t>(image.width),
                          static_cast<std::size_t>(channels), &row_components)
        || !checked_multiply(row_components, bytes_per_component, &row_bytes)
        || !checked_multiply(static_cast<std::size_t>(image.width) + 2U,
                             static_cast<std::size_t>(channels), &error_values)) {
        return fail(error, "The PNG row buffer size overflows addressable memory.");
    }
    if (row_bytes == 0 || error_values == 0) {
        return fail(error, "The PNG row buffer size must be positive.");
    }

    auto* context = static_cast<PngErrorContext*>(std::calloc(1U, sizeof(PngErrorContext)));
    auto* row = static_cast<png_bytep>(std::malloc(row_bytes));
    double* diffusion_a = nullptr;
    double* diffusion_b = nullptr;
    const bool use_floyd = config.output.dither_enabled
                           && config.output.dither_method == DitherMethod::FloydSteinberg;
    if (use_floyd) {
        diffusion_a = static_cast<double*>(std::calloc(error_values, sizeof(double)));
        diffusion_b = static_cast<double*>(std::calloc(error_values, sizeof(double)));
    }
    if (context == nullptr || row == nullptr
        || (use_floyd && (diffusion_a == nullptr || diffusion_b == nullptr))) {
        std::free(context);
        std::free(row);
        std::free(diffusion_a);
        std::free(diffusion_b);
        return fail(error, "Could not allocate PNG encoding buffers.");
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, context,
                                               png_error_callback, nullptr);
    if (png == nullptr) {
        std::free(context);
        std::free(row);
        std::free(diffusion_a);
        std::free(diffusion_b);
        return fail(error, "Could not initialize the PNG writer.");
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        std::free(context);
        std::free(row);
        std::free(diffusion_a);
        std::free(diffusion_b);
        return fail(error, "Could not initialize PNG metadata.");
    }

    if (setjmp(png_jmpbuf(png)) != 0) {
        const std::string message = context->message[0] != '\0'
                                        ? context->message
                                        : "unknown libpng error";
        png_destroy_write_struct(&png, &info);
        std::free(context);
        std::free(row);
        std::free(diffusion_a);
        std::free(diffusion_b);
        return fail(error, "PNG encoding failed: " + message);
    }

    png_init_io(png, file);
    png_set_IHDR(png, info,
                 static_cast<png_uint_32>(image.width),
                 static_cast<png_uint_32>(image.height),
                 bit_depth,
                 channels == 4 ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL);
    png_set_compression_level(png, config.output.png_compression_level);
    png_write_info(png, info);

    const std::uint32_t maximum = bit_depth == 16 ? 65535U : 255U;
    for (int y = 0; y < image.height; ++y) {
        if (cancelled(cancel)) {
            png_destroy_write_struct(&png, &info);
            std::free(context);
            std::free(row);
            std::free(diffusion_a);
            std::free(diffusion_b);
            return fail(error, "PNG encoding was cancelled.");
        }
        double* current_error = nullptr;
        double* next_error = nullptr;
        if (use_floyd) {
            current_error = (y & 1) == 0 ? diffusion_a : diffusion_b;
            next_error = (y & 1) == 0 ? diffusion_b : diffusion_a;
            std::memset(next_error, 0, error_values * sizeof(double));
        }

        std::size_t output_offset = 0;
        for (int x = 0; x < image.width; ++x) {
            const std::size_t input_offset =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)
                 + static_cast<std::size_t>(x)) * 4U;
            const float* pixel = image.pixels.data() + input_offset;

            for (int channel = 0; channel < channels; ++channel) {
                const double encoded = encoded_component(pixel, channel);
                std::uint32_t quantized = 0;

                if (use_floyd) {
                    const std::size_t diffusion_index =
                        (static_cast<std::size_t>(x) + 1U)
                            * static_cast<std::size_t>(channels)
                        + static_cast<std::size_t>(channel);
                    const double scaled = encoded * static_cast<double>(maximum)
                                          + current_error[diffusion_index];
                    const double bounded = std::max(
                        0.0, std::min(static_cast<double>(maximum), scaled));
                    quantized = static_cast<std::uint32_t>(std::llround(bounded));
                    const double quantization_error = bounded
                                                      - static_cast<double>(quantized);
                    current_error[diffusion_index
                                  + static_cast<std::size_t>(channels)]
                        += quantization_error * (7.0 / 16.0);
                    next_error[diffusion_index
                               - static_cast<std::size_t>(channels)]
                        += quantization_error * (3.0 / 16.0);
                    next_error[diffusion_index] += quantization_error * (5.0 / 16.0);
                    next_error[diffusion_index
                               + static_cast<std::size_t>(channels)]
                        += quantization_error * (1.0 / 16.0);
                } else {
                    const double dither = config.output.dither_enabled
                                              ? spatial_dither(config.output.dither_method,
                                                               x, y, channel, seed)
                                              : 0.0;
                    quantized = quantize_component(encoded, maximum, dither);
                }

                if (bit_depth == 16) {
                    // PNG stores 16-bit samples in network byte order.
                    row[output_offset++] = static_cast<png_byte>(quantized >> 8U);
                    row[output_offset++] = static_cast<png_byte>(quantized & 0xffU);
                } else {
                    row[output_offset++] = static_cast<png_byte>(quantized);
                }
            }
        }
        png_write_row(png, row);
    }

    if (cancelled(cancel)) {
        png_destroy_write_struct(&png, &info);
        std::free(context);
        std::free(row);
        std::free(diffusion_a);
        std::free(diffusion_b);
        return fail(error, "PNG encoding was cancelled.");
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    std::free(context);
    std::free(row);
    std::free(diffusion_a);
    std::free(diffusion_b);

    if (std::ferror(file) != 0) {
        return fail(error, "Writing the PNG stream failed.");
    }
    return true;
}

void append_u32(std::vector<unsigned char>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

void append_u64(std::vector<unsigned char>& bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void append_float(std::vector<unsigned char>& bytes, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "OpenEXR requires 32-bit floats");
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

void append_c_string(std::vector<unsigned char>& bytes, const char* text) {
    const std::size_t length = std::strlen(text);
    bytes.insert(bytes.end(), text, text + length);
    bytes.push_back(0U);
}

bool append_attribute(std::vector<unsigned char>& header,
                      const char* name,
                      const char* type,
                      const std::vector<unsigned char>& value,
                      std::string* error) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, "An OpenEXR header attribute is too large.");
    }
    append_c_string(header, name);
    append_c_string(header, type);
    append_u32(header, static_cast<std::uint32_t>(value.size()));
    header.insert(header.end(), value.begin(), value.end());
    return true;
}

bool write_all(std::FILE* file, const void* data, std::size_t size) {
    return size == 0U || std::fwrite(data, 1U, size, file) == size;
}

bool write_vector(std::FILE* file, const std::vector<unsigned char>& bytes) {
    return write_all(file, bytes.data(), bytes.size());
}

struct ExrChannel {
    const char* name;
    int rgba_index;
};

bool write_exr_stream(std::FILE* file,
                      const Image& image,
                      const RenderConfig& config,
                      const std::atomic_bool* cancel,
                      std::string* error) {
    // OpenEXR scanline files store channels alphabetically. Each scanline chunk
    // contains one planar run per channel in the same order as the chlist.
    const std::array<ExrChannel, 4> rgba_channels = {{
        {"A", 3}, {"B", 2}, {"G", 1}, {"R", 0}
    }};
    const std::array<ExrChannel, 3> rgb_channels = {{
        {"B", 2}, {"G", 1}, {"R", 0}
    }};
    const bool include_alpha = writes_alpha_channel(config);
    const ExrChannel* channels = include_alpha ? rgba_channels.data()
                                               : rgb_channels.data();
    const std::size_t channel_count = include_alpha ? rgba_channels.size()
                                                    : rgb_channels.size();

    std::vector<unsigned char> header;
    header.reserve(384U);
    append_u32(header, 20000630U); // OpenEXR magic number.
    append_u32(header, 2U);        // Version 2, single-part scanline image.

    std::vector<unsigned char> value;
    for (std::size_t channel = 0; channel < channel_count; ++channel) {
        append_c_string(value, channels[channel].name);
        append_u32(value, 2U); // FLOAT, never HALF.
        value.push_back(0U);   // pLinear is only a hint; samples are linear here.
        value.push_back(0U);
        value.push_back(0U);
        value.push_back(0U);
        append_u32(value, 1U); // xSampling
        append_u32(value, 1U); // ySampling
    }
    value.push_back(0U); // End of chlist.
    if (!append_attribute(header, "channels", "chlist", value, error)) {
        return false;
    }

    value.assign(1U, 0U); // NO_COMPRESSION
    if (!append_attribute(header, "compression", "compression", value, error)) {
        return false;
    }

    value.clear();
    append_u32(value, 0U);
    append_u32(value, 0U);
    append_u32(value, static_cast<std::uint32_t>(image.width - 1));
    append_u32(value, static_cast<std::uint32_t>(image.height - 1));
    if (!append_attribute(header, "dataWindow", "box2i", value, error)
        || !append_attribute(header, "displayWindow", "box2i", value, error)) {
        return false;
    }

    value.assign(1U, 0U); // INCREASING_Y
    if (!append_attribute(header, "lineOrder", "lineOrder", value, error)) {
        return false;
    }

    value.clear();
    append_float(value, 1.0F);
    if (!append_attribute(header, "pixelAspectRatio", "float", value, error)) {
        return false;
    }

    value.clear();
    append_float(value, 0.0F);
    append_float(value, 0.0F);
    if (!append_attribute(header, "screenWindowCenter", "v2f", value, error)) {
        return false;
    }

    value.clear();
    append_float(value, 1.0F);
    if (!append_attribute(header, "screenWindowWidth", "float", value, error)) {
        return false;
    }
    header.push_back(0U); // End of header.

    std::size_t scanline_samples = 0;
    std::size_t scanline_bytes = 0;
    if (!checked_multiply(static_cast<std::size_t>(image.width), channel_count,
                          &scanline_samples)
        || !checked_multiply(scanline_samples, sizeof(float), &scanline_bytes)
        || scanline_bytes > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return fail(error, "An OpenEXR scanline is too large.");
    }

    std::uint64_t offset_table_bytes = 0;
    if (static_cast<std::uint64_t>(image.height)
        > std::numeric_limits<std::uint64_t>::max() / 8U) {
        return fail(error, "The OpenEXR offset table size overflows.");
    }
    offset_table_bytes = static_cast<std::uint64_t>(image.height) * 8U;
    std::uint64_t first_chunk = 0;
    if (!checked_add_u64(static_cast<std::uint64_t>(header.size()), offset_table_bytes,
                         &first_chunk)) {
        return fail(error, "The OpenEXR file offset table overflows.");
    }
    const std::uint64_t chunk_bytes = 8U + static_cast<std::uint64_t>(scanline_bytes);
    if (image.height > 0
        && chunk_bytes > (std::numeric_limits<std::uint64_t>::max() - first_chunk)
                             / static_cast<std::uint64_t>(image.height)) {
        return fail(error, "The OpenEXR file size overflows 64-bit offsets.");
    }

    if (!write_vector(file, header)) {
        return fail(error, "Writing the OpenEXR header failed.");
    }
    std::vector<unsigned char> little_endian;
    little_endian.reserve(8U);
    for (int y = 0; y < image.height; ++y) {
        if (cancelled(cancel)) {
            return fail(error, "OpenEXR encoding was cancelled.");
        }
        little_endian.clear();
        append_u64(little_endian,
                   first_chunk + static_cast<std::uint64_t>(y) * chunk_bytes);
        if (!write_vector(file, little_endian)) {
            return fail(error, "Writing the OpenEXR scanline offset table failed.");
        }
    }

    std::vector<unsigned char> scanline(scanline_bytes);
    for (int y = 0; y < image.height; ++y) {
        if (cancelled(cancel)) {
            return fail(error, "OpenEXR encoding was cancelled.");
        }
        std::size_t output = 0;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            for (int x = 0; x < image.width; ++x) {
                const std::size_t input =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)
                     + static_cast<std::size_t>(x)) * 4U
                    + static_cast<std::size_t>(channels[channel].rgba_index);
                std::uint32_t bits = 0;
                const float sample = image.pixels[input];
                std::memcpy(&bits, &sample, sizeof(bits));
                scanline[output++] = static_cast<unsigned char>(bits & 0xffU);
                scanline[output++] = static_cast<unsigned char>((bits >> 8U) & 0xffU);
                scanline[output++] = static_cast<unsigned char>((bits >> 16U) & 0xffU);
                scanline[output++] = static_cast<unsigned char>((bits >> 24U) & 0xffU);
            }
        }

        little_endian.clear();
        append_u32(little_endian, static_cast<std::uint32_t>(y));
        append_u32(little_endian, static_cast<std::uint32_t>(scanline_bytes));
        if (!write_vector(file, little_endian) || !write_vector(file, scanline)) {
            return fail(error, "Writing OpenEXR scanline data failed.");
        }
    }

    if (std::ferror(file) != 0) {
        return fail(error, "Writing the OpenEXR stream failed.");
    }
    return true;
}

std::uint64_t process_id() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

#if !defined(_WIN32)
int fsync_retry(int descriptor) {
    int result = 0;
    do {
        result = ::fsync(descriptor);
    } while (result != 0 && errno == EINTR);
    return result;
}

void sync_directory_best_effort(const fs::path& directory) {
#  if defined(O_DIRECTORY)
    constexpr int directory_flag = O_DIRECTORY;
#  else
    constexpr int directory_flag = 0;
#  endif
    const int descriptor = ::open(directory.c_str(), O_RDONLY | directory_flag);
    if (descriptor >= 0) {
        (void)fsync_retry(descriptor);
        (void)::close(descriptor);
    }
}
#endif

class TemporaryOutput {
public:
    TemporaryOutput() = default;
    TemporaryOutput(const TemporaryOutput&) = delete;
    TemporaryOutput& operator=(const TemporaryOutput&) = delete;

    ~TemporaryOutput() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
        if (!path_.empty()) {
            std::error_code ignored;
            fs::remove(path_, ignored);
        }
    }

    bool open_next_to(const fs::path& destination, std::string* error) {
        static std::atomic<std::uint64_t> counter{0U};
        const std::uint64_t first = counter.fetch_add(128U, std::memory_order_relaxed);

        for (std::uint64_t attempt = 0; attempt < 128U; ++attempt) {
            fs::path candidate = destination;
            candidate += ".tmp." + std::to_string(process_id()) + "."
                         + std::to_string(first + attempt);
            errno = 0;
#if defined(_WIN32)
            std::FILE* opened = nullptr;
            const errno_t result = _wfopen_s(&opened, candidate.wstring().c_str(), L"wbx");
            if (result == 0 && opened != nullptr) {
                path_ = std::move(candidate);
                file_ = opened;
                return true;
            }
            const int open_error = result != 0 ? static_cast<int>(result) : errno;
#else
            std::FILE* opened = std::fopen(candidate.c_str(), "wbx");
            if (opened != nullptr) {
                path_ = std::move(candidate);
                file_ = opened;
                return true;
            }
            const int open_error = errno;
#endif
            if (open_error != EEXIST) {
                return fail(error, system_error_message("Could not create temporary output",
                                                        candidate, open_error));
            }
        }
        return fail(error, "Could not allocate a unique temporary output filename.");
    }

    std::FILE* file() const {
        return file_;
    }

    const fs::path& path() const {
        return path_;
    }

#if !defined(_WIN32)
    bool set_permissions(mode_t permissions, std::string* error) {
        if (file_ == nullptr) {
            return fail(error, "The temporary output file is not open.");
        }
        if (::fchmod(::fileno(file_), permissions) != 0) {
            const int permission_error = errno;
            return fail(error, system_error_message(
                                   "Could not preserve output file permissions",
                                   path_, permission_error));
        }
        return true;
    }
#endif

    bool close_and_sync(std::string* error) {
        if (file_ == nullptr) {
            return fail(error, "The temporary output file is not open.");
        }
        bool ok = true;
        int saved_error = 0;
        if (std::fflush(file_) != 0) {
            ok = false;
            saved_error = errno;
        }
#if defined(_WIN32)
        if (ok && _commit(_fileno(file_)) != 0) {
            ok = false;
            saved_error = errno;
        }
#else
        if (ok && fsync_retry(fileno(file_)) != 0) {
            ok = false;
            saved_error = errno;
        }
#endif
        if (std::fclose(file_) != 0 && ok) {
            ok = false;
            saved_error = errno;
        }
        file_ = nullptr;
        if (!ok) {
            return fail(error, system_error_message("Could not finalize temporary output",
                                                    path_, saved_error));
        }
        return true;
    }

    void dismiss() {
        path_.clear();
    }

private:
    fs::path path_;
    std::FILE* file_ = nullptr;
};

bool path_entry_exists(const fs::path& path, bool* exists, std::string* error) {
    std::error_code code;
    const fs::file_status status = fs::symlink_status(path, code);
    if (code) {
        if (code == std::errc::no_such_file_or_directory) {
            *exists = false;
            return true;
        }
        return fail(error, "Could not inspect output path '" + path.string()
                           + "': " + code.message());
    }
    *exists = status.type() != fs::file_type::not_found;
    return true;
}

bool ensure_directory(const fs::path& directory, std::string* error) {
    if (directory.empty()) {
        return fail(error, "The output directory cannot be empty.");
    }
    std::error_code code;
    const fs::file_status status = fs::status(directory, code);
    if (!code && fs::exists(status)) {
        if (!fs::is_directory(status)) {
            return fail(error, "Output path '" + directory.string()
                               + "' exists but is not a directory.");
        }
        return true;
    }
    if (code && code != std::errc::no_such_file_or_directory) {
        return fail(error, "Could not inspect output directory '" + directory.string()
                           + "': " + code.message());
    }
    code.clear();
    if (!fs::create_directories(directory, code) && code) {
        return fail(error, "Could not create output directory '" + directory.string()
                           + "': " + code.message());
    }
    return true;
}

bool install_temporary(const fs::path& temporary,
                       const fs::path& destination,
                       bool overwrite,
                       std::string* error) {
#if defined(_WIN32)
    DWORD code = ERROR_SUCCESS;
    if (!detail::install_windows_temporary(temporary, destination,
                                           overwrite, &code)) {
        return fail(error, "Could not install output file '" + destination.string()
                           + "' (Windows error " + std::to_string(code) + ").");
    }
#else
    if (overwrite) {
        if (::rename(temporary.c_str(), destination.c_str()) != 0) {
            return fail(error, system_error_message("Could not install output",
                                                    destination, errno));
        }
    } else {
#if defined(__APPLE__)
        // macOS supplies a true atomic, no-replace rename. Besides matching the
        // normal rename semantics, this continues to work on volumes that do
        // not implement hard links.
        if (::renameatx_np(AT_FDCWD, temporary.c_str(),
                           AT_FDCWD, destination.c_str(), RENAME_EXCL) != 0) {
            return fail(error, system_error_message("Could not install output without replacing it",
                                                    destination, errno));
        }
#else
        // A same-directory hard link provides atomic no-clobber semantics on
        // POSIX. The temporary name is then removed, leaving the linked inode.
        if (::link(temporary.c_str(), destination.c_str()) != 0) {
            return fail(error, system_error_message("Could not install output without replacing it",
                                                    destination, errno));
        }
        if (::unlink(temporary.c_str()) != 0) {
            return fail(error, system_error_message("Output was installed, but its temporary name could not be removed",
                                                    temporary, errno));
        }
#endif
    }
#endif
    return true;
}

struct PreparedOutput {
    TemporaryOutput temporary;
    fs::path destination;
    fs::path parent;
    bool overwrite = false;
};

bool prepare_image_output(const std::string& path,
                          const Image& image,
                          const RenderConfig& config,
                          std::uint32_t deterministic_seed,
                          const std::atomic_bool* cancel,
                          PreparedOutput* prepared,
                          std::string* error) {
    if (prepared == nullptr) {
        return fail(error, "The prepared output destination is missing.");
    }
    const ValidationResult validation = validate(config);
    if (!validation.ok) {
        return fail(error, validation.message.empty()
                               ? "The export configuration is invalid."
                               : validation.message);
    }
    if (!validate_image_for_export(image, config, error)) {
        return false;
    }
    if (path.empty() || path.size() > kMaximumOutputPathBytes
        || path.find('\0') != std::string::npos) {
        return fail(error,
                    "The output image path is empty, contains a NUL byte, or exceeds 64 KiB.");
    }

    const fs::path destination = detail::path_from_utf8(path);
    const fs::path parent = destination.has_parent_path() ? destination.parent_path()
                                                          : fs::path(".");
    if (!ensure_directory(parent, error)) {
        return false;
    }

    bool destination_exists = false;
    if (!path_entry_exists(destination, &destination_exists, error)) {
        return false;
    }
    if (destination_exists && !config.output.overwrite_existing) {
        return fail(error, "Output file already exists: '" + destination.string() + "'.");
    }
#if !defined(_WIN32)
    mode_t preserved_permissions = 0;
    bool preserve_permissions = false;
#endif
    if (destination_exists) {
#if defined(_WIN32)
        std::error_code code;
        if (fs::is_directory(fs::symlink_status(destination, code))) {
            return fail(error, "Output destination is a directory: '"
                               + destination.string() + "'.");
        }
        if (code) {
            return fail(error, "Could not inspect output destination '"
                               + destination.string() + "': " + code.message());
        }
#else
        // Inspect without following a destination symlink: overwrite replaces
        // the entry itself. Preserve permission bits only for a regular file.
        struct stat destination_status {};
        if (::lstat(destination.c_str(), &destination_status) != 0) {
            return fail(error, system_error_message(
                                   "Could not inspect output destination",
                                   destination, errno));
        }
        if (S_ISDIR(destination_status.st_mode)) {
            return fail(error, "Output destination is a directory: '"
                               + destination.string() + "'.");
        }
        if (S_ISREG(destination_status.st_mode)) {
            preserved_permissions = destination_status.st_mode & 07777;
            preserve_permissions = true;
        }
#endif
    }

    prepared->destination = destination;
    prepared->parent = parent;
    prepared->overwrite = config.output.overwrite_existing;
    if (!prepared->temporary.open_next_to(destination, error)) {
        return false;
    }

    const bool encoded = config.output.bit_depth == 32
                             ? write_exr_stream(prepared->temporary.file(), image,
                                                config, cancel, error)
                             : write_png_stream(prepared->temporary.file(), image, config,
                                                deterministic_seed, cancel, error);
    if (!encoded) {
        return false;
    }
#if !defined(_WIN32)
    // Apply the final mode after encoding: on some POSIX systems, writing a
    // file can clear its set-user-ID or set-group-ID bits.
    if (preserve_permissions
        && !prepared->temporary.set_permissions(preserved_permissions, error)) {
        return false;
    }
#endif
    if (!prepared->temporary.close_and_sync(error)) {
        return false;
    }
    return true;
}

bool install_prepared_output(PreparedOutput& prepared, std::string* error) {
    if (!install_temporary(prepared.temporary.path(), prepared.destination,
                           prepared.overwrite, error)) {
        return false;
    }
    prepared.temporary.dismiss();
#if !defined(_WIN32)
    // The file data was synced before installation. Sync the containing
    // directory as well so the new or replaced name is durable across a crash.
    sync_directory_best_effort(prepared.parent);
#endif
    return true;
}

bool write_image_impl(const std::string& path,
                      const Image& image,
                      const RenderConfig& config,
                      std::uint32_t deterministic_seed,
                      std::string* error) {
    PreparedOutput prepared;
    return prepare_image_output(path, image, config, deterministic_seed,
                                nullptr, &prepared, error)
           && install_prepared_output(prepared, error);
}

const char* extension_for_bit_depth(int bit_depth) {
    if (bit_depth == 8 || bit_depth == 16) {
        return ".png";
    }
    if (bit_depth == 32) {
        return ".exr";
    }
    return nullptr;
}

bool build_frame_path(const RenderConfig& config,
                      int frame_index,
                      fs::path* result,
                      std::string* error) {
    const char* extension = extension_for_bit_depth(config.output.bit_depth);
    if (extension == nullptr) {
        return fail(error, "Export bit depth must be 8, 16, or 32.");
    }
    if (!valid_prefix(config.output.filename_prefix)) {
        return fail(error, "The filename prefix cannot be empty or contain characters forbidden in portable filenames.");
    }
    if (config.output.filename_digits < 1 || config.output.filename_digits > 12) {
        return fail(error, "Filename zero-padding must be between 1 and 12 digits.");
    }
    if (frame_index < 0 || frame_index >= config.total_frames) {
        return fail(error, "The requested frame index is outside the sequence.");
    }

    const std::int64_t number = static_cast<std::int64_t>(config.output.first_frame_number)
                                + static_cast<std::int64_t>(frame_index);
    if (number < 0) {
        return fail(error, "Frame numbering cannot be negative.");
    }
    std::string digits = std::to_string(number);
    if (digits.size() < static_cast<std::size_t>(config.output.filename_digits)) {
        digits.insert(0U,
                      static_cast<std::size_t>(config.output.filename_digits) - digits.size(),
                      '0');
    }
    const std::string filename = config.output.filename_prefix + digits + extension;
    *result = detail::path_from_utf8(config.output.output_directory)
              / detail::path_from_utf8(filename);
    return true;
}

bool report_progress(const ProgressCallback& progress,
                     int completed,
                     int total,
                     std::string* error) {
    if (!progress) {
        return true;
    }
    try {
        if (!progress(completed, total)) {
            return fail(error, "Rendering was cancelled by the progress callback.");
        }
    } catch (const std::exception& exception) {
        return fail(error, "The progress callback failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "The progress callback failed with an unknown exception.");
    }
    return true;
}

bool select_sequence_worker_count(const SequenceRenderOptions& options,
                                  int total_frames,
                                  std::size_t estimated_peak_bytes,
                                  std::size_t* worker_count,
                                  std::string* error) {
    if (options.worker_count > kMaximumSequenceWorkers) {
        return fail(error, "Sequence worker count cannot exceed "
                           + std::to_string(kMaximumSequenceWorkers) + ".");
    }
    const std::size_t hardware_workers =
        std::max<std::size_t>(1U, std::thread::hardware_concurrency());
    const std::size_t requested = options.worker_count == 0U
                                      ? hardware_workers
                                      : options.worker_count;
    const std::size_t budget = options.memory_budget_bytes == 0U
                                   ? kDefaultSequenceMemoryBudgetBytes
                                   : options.memory_budget_bytes;
    const std::size_t memory_limited = estimated_peak_bytes == 0U
                                           ? requested
                                           : std::max<std::size_t>(
                                                 1U, budget / estimated_peak_bytes);
    *worker_count = std::max<std::size_t>(
        1U, std::min({requested, memory_limited,
                      static_cast<std::size_t>(total_frames),
                      kMaximumSequenceWorkers}));
    return true;
}

std::size_t backend_adjusted_peak_bytes(
    std::size_t reference_peak_bytes,
    const SequenceRenderOptions& options) {
    // Hybrid project rendering can hold one CPU and one Metal layer working
    // set concurrently. Count that pair in the existing aggregate sequence
    // budget so frame-level workers cannot multiply hidden per-layer memory.
    if (options.frame.backend != RenderBackend::CpuAndGpu) {
        return reference_peak_bytes;
    }
    std::size_t adjusted = 0U;
    if (!checked_multiply(reference_peak_bytes, 2U, &adjusted)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return adjusted;
}

enum class FrameFailureStage {
    None,
    Render,
    Encode
};

struct FrameWorkResult {
    bool ok = false;
    FrameFailureStage failure_stage = FrameFailureStage::None;
    std::string error;
    std::exception_ptr exception;
    std::unique_ptr<PreparedOutput> prepared;
};

struct FrameWorkerSlot {
    bool ready = false;
    int frame_index = -1;
    FrameWorkResult result;
};

class SequenceWorkerJoiner {
public:
    SequenceWorkerJoiner(std::vector<std::thread>& threads,
                         std::atomic_bool& stop,
                         std::condition_variable& wake)
        : threads_(threads), stop_(stop), wake_(wake) {}

    SequenceWorkerJoiner(const SequenceWorkerJoiner&) = delete;
    SequenceWorkerJoiner& operator=(const SequenceWorkerJoiner&) = delete;

    ~SequenceWorkerJoiner() {
        stop_.store(true, std::memory_order_relaxed);
        wake_.notify_all();
        for (std::thread& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    std::vector<std::thread>& threads_;
    std::atomic_bool& stop_;
    std::condition_variable& wake_;
};

std::string worker_exception_message(const std::exception_ptr& exception) {
    try {
        if (exception != nullptr) {
            std::rethrow_exception(exception);
        }
    } catch (const std::bad_alloc&) {
        return "worker ran out of memory";
    } catch (const std::exception& value) {
        return value.what();
    } catch (...) {
        return "worker failed with an unknown exception";
    }
    return "worker failed without an error";
}

template <typename RenderFrame>
bool render_prepared_sequence(int total_frames,
                              const RenderConfig& output_config,
                              std::size_t estimated_peak_bytes,
                              const SequenceRenderOptions& options,
                              const ProgressCallback& progress,
                              const std::atomic_bool* cancel,
                              const char* sequence_name,
                              const char* frame_name,
                              RenderFrame render_frame,
                              std::string* error) {
    std::size_t worker_count = 0U;
    if (!select_sequence_worker_count(options, total_frames,
                                      estimated_peak_bytes, &worker_count, error)) {
        return false;
    }

    std::atomic_bool stop {false};
    std::atomic<int> next_frame {0};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<FrameWorkerSlot> slots(worker_count);
    std::vector<std::thread> threads;
    threads.reserve(worker_count);
    std::exception_ptr scheduler_exception;
    SequenceWorkerJoiner joiner(threads, stop, wake);

    for (std::size_t worker = 0U; worker < worker_count; ++worker) {
        threads.emplace_back([&, worker] {
            try {
                Image image;
                for (;;) {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [&] {
                            return stop.load(std::memory_order_relaxed)
                                   || !slots[worker].ready;
                        });
                    }
                    if (stop.load(std::memory_order_relaxed)) {
                        return;
                    }

                    const int frame_index =
                        next_frame.fetch_add(1, std::memory_order_relaxed);
                    if (frame_index >= total_frames) {
                        return;
                    }

                    FrameWorkResult result;
                    result.failure_stage = FrameFailureStage::Render;
                    try {
                        if (render_frame(frame_index, image, &stop, &result.error)) {
                            result.failure_stage = FrameFailureStage::Encode;
                            result.prepared = std::make_unique<PreparedOutput>();
                            fs::path frame_path;
                            if (build_frame_path(output_config, frame_index,
                                                 &frame_path, &result.error)
                                && prepare_image_output(
                                    detail::path_to_utf8(frame_path), image,
                                    output_config, kSequenceDitherSeed,
                                    &stop, result.prepared.get(), &result.error)) {
                                result.ok = true;
                                result.failure_stage = FrameFailureStage::None;
                            }
                        }
                    } catch (...) {
                        result.exception = std::current_exception();
                    }

                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        slots[worker].frame_index = frame_index;
                        slots[worker].result = std::move(result);
                        slots[worker].ready = true;
                    }
                    wake.notify_all();
                }
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (scheduler_exception == nullptr) {
                        scheduler_exception = std::current_exception();
                    }
                }
                stop.store(true, std::memory_order_relaxed);
                wake.notify_all();
            }
        });
    }

    for (int expected_frame = 0; expected_frame < total_frames; ++expected_frame) {
        FrameWorkResult result;
        {
            std::unique_lock<std::mutex> lock(mutex);
            for (;;) {
                if (scheduler_exception != nullptr) {
                    return fail(error, std::string(sequence_name)
                                           + " worker failed: "
                                           + worker_exception_message(
                                                 scheduler_exception) + ".");
                }
                if (cancelled(cancel)) {
                    stop.store(true, std::memory_order_relaxed);
                    wake.notify_all();
                    return fail(error, std::string(sequence_name)
                                           + " was cancelled.");
                }

                auto ready = std::find_if(
                    slots.begin(), slots.end(),
                    [expected_frame](const FrameWorkerSlot& slot) {
                        return slot.ready && slot.frame_index == expected_frame;
                    });
                if (ready != slots.end()) {
                    result = std::move(ready->result);
                    ready->ready = false;
                    ready->frame_index = -1;
                    break;
                }
                wake.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
        wake.notify_all();

        if (result.exception != nullptr) {
            return fail(error, "Could not process " + std::string(frame_name) + " "
                                   + std::to_string(expected_frame) + ": "
                                   + worker_exception_message(result.exception) + ".");
        }
        if (!result.ok) {
            const char* action = result.failure_stage == FrameFailureStage::Encode
                                     ? "export"
                                     : "render";
            return fail(error, "Could not " + std::string(action) + " "
                                   + frame_name + " "
                                   + std::to_string(expected_frame) + ": "
                                   + result.error);
        }
        if (cancelled(cancel)) {
            return fail(error, std::string(sequence_name) + " was cancelled.");
        }

        std::string install_error;
        if (result.prepared == nullptr
            || !install_prepared_output(*result.prepared, &install_error)) {
            return fail(error, "Could not export " + std::string(frame_name) + " "
                                   + std::to_string(expected_frame) + ": "
                                   + (install_error.empty()
                                          ? "the prepared output is missing"
                                          : install_error));
        }
        if (!report_progress(progress, expected_frame + 1, total_frames, error)) {
            return false;
        }
    }
    return true;
}

bool render_sequence_impl(const RenderConfig& config,
                          const SequenceRenderOptions& options,
                          const ProgressCallback& progress,
                          const std::atomic_bool* cancel,
                          std::string* error) {
    const ValidationResult validation = validate(config);
    if (!validation.ok) {
        return fail(error, validation.message.empty() ? "The render configuration is invalid."
                                                      : validation.message);
    }
    if (cancelled(cancel)) {
        return fail(error, "Rendering was cancelled.");
    }
    std::string frame_count_error;
    const int total_frames = effective_frame_count(config, &frame_count_error);
    if (total_frames < 1) {
        return fail(error, frame_count_error.empty()
                               ? "The synchronized clock has no renderable frames."
                               : frame_count_error);
    }
    RenderConfig output_config = config;
    output_config.total_frames = total_frames;

    const fs::path directory = detail::path_from_utf8(config.output.output_directory);
    if (!ensure_directory(directory, error)) {
        return false;
    }

    // Inspect every final name before rendering. Installation repeats the
    // check atomically so another process cannot exploit the gap.
    for (int frame_index = 0; frame_index < total_frames; ++frame_index) {
        if (cancelled(cancel)) {
            return fail(error, "Rendering was cancelled during output preflight.");
        }
        fs::path path;
        if (!build_frame_path(output_config, frame_index, &path, error)) {
            return false;
        }
        bool exists = false;
        if (!path_entry_exists(path, &exists, error)) {
            return false;
        }
        if (exists && !config.output.overwrite_existing) {
            return fail(error, "Output file already exists: '" + path.string()
                               + "'. No frames were rendered.");
        }
        if (exists) {
            std::error_code code;
            if (fs::is_directory(fs::symlink_status(path, code))) {
                return fail(error, "Output destination is a directory: '"
                                   + path.string() + "'. No frames were rendered.");
            }
            if (code) {
                return fail(error, "Could not inspect output destination '"
                                   + path.string() + "': " + code.message());
            }
        }
    }

    return render_prepared_sequence(
        total_frames, output_config,
        backend_adjusted_peak_bytes(validation.estimated_peak_bytes, options),
        options, progress, cancel, "Rendering", "frame",
        [&config, &options](int frame_index, Image& image,
                  const std::atomic_bool* worker_cancel,
                  std::string* frame_error) {
            return render_frame(config, frame_index, options.frame, image,
                                worker_cancel, frame_error);
        },
        error);
}

bool render_project_sequence_impl(const ProjectConfig& project,
                                  const SequenceRenderOptions& options,
                                  const ProgressCallback& progress,
                                  const std::atomic_bool* cancel,
                                  std::string* error) {
    const ValidationResult validation = validate(project);
    if (!validation.ok) {
        return fail(error, validation.message.empty()
                               ? "The project configuration is invalid."
                               : validation.message);
    }
    if (cancelled(cancel)) {
        return fail(error, "Project rendering was cancelled.");
    }

    // Image encoding and naming remain centralized in the legacy-safe export
    // path, but the final alpha channel comes from project-global output.
    const RenderConfig defaults = default_config();
    RenderConfig output_config = apply_global_config(
        project.canvas, project.output,
        static_cast<const RenderData&>(defaults));
    output_config.alpha.enabled = false;
    std::string frame_count_error;
    const int total_frames = effective_frame_count(project.canvas,
                                                   &frame_count_error);
    if (total_frames < 1) {
        return fail(error, frame_count_error.empty()
                               ? "The project clock has no renderable frames."
                               : frame_count_error);
    }
    output_config.total_frames = total_frames;

    const fs::path directory =
        detail::path_from_utf8(project.output.output_directory);
    if (!ensure_directory(directory, error)) {
        return false;
    }

    // Preflight every destination before rendering so a late collision cannot
    // leave an unintentionally partial sequence.
    for (int frame_index = 0; frame_index < total_frames;
         ++frame_index) {
        if (cancelled(cancel)) {
            return fail(error,
                        "Project rendering was cancelled during output preflight.");
        }
        fs::path path;
        if (!build_frame_path(output_config, frame_index, &path, error)) {
            return false;
        }
        bool exists = false;
        if (!path_entry_exists(path, &exists, error)) {
            return false;
        }
        if (exists && !project.output.overwrite_existing) {
            return fail(error, "Output file already exists: '" + path.string()
                                   + "'. No frames were rendered.");
        }
        if (exists) {
            std::error_code code;
            if (fs::is_directory(fs::symlink_status(path, code))) {
                return fail(error, "Output destination is a directory: '"
                                       + path.string()
                                       + "'. No frames were rendered.");
            }
            if (code) {
                return fail(error, "Could not inspect output destination '"
                                       + path.string() + "': " + code.message());
            }
        }
    }

    return render_prepared_sequence(
        total_frames, output_config,
        backend_adjusted_peak_bytes(validation.estimated_peak_bytes, options),
        options, progress, cancel,
        "Project rendering", "project frame",
        [&project, &options](int frame_index, Image& image,
                   const std::atomic_bool* worker_cancel,
                   std::string* frame_error) {
            return render_project_frame(project, frame_index, options.frame,
                                        image, worker_cancel, frame_error);
        },
        error);
}

} // namespace

bool write_image(const std::string& path,
                 const Image& image,
                 const RenderConfig& config,
                 std::uint32_t deterministic_seed,
                 std::string* error) {
    clear_error(error);
    try {
        return write_image_impl(path, image, config, deterministic_seed, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Image export ran out of memory.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, "Image export filesystem error: " + std::string(exception.what()));
    } catch (const std::exception& exception) {
        return fail(error, "Image export failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Image export failed with an unknown exception.");
    }
}

bool render_sequence(const RenderConfig& config,
                     const ProgressCallback& progress,
                     const std::atomic_bool* cancel,
                     std::string* error) {
    return render_sequence(config, SequenceRenderOptions{}, progress, cancel, error);
}

bool render_sequence(const RenderConfig& config,
                     const SequenceRenderOptions& options,
                     const ProgressCallback& progress,
                     const std::atomic_bool* cancel,
                     std::string* error) {
    clear_error(error);
    try {
        return render_sequence_impl(config, options, progress, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Sequence rendering ran out of memory.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, "Sequence rendering filesystem error: "
                           + std::string(exception.what()));
    } catch (const std::exception& exception) {
        return fail(error, "Sequence rendering failed: " + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Sequence rendering failed with an unknown exception.");
    }
}

bool render_project_sequence(const ProjectConfig& project,
                             const ProgressCallback& progress,
                             const std::atomic_bool* cancel,
                             std::string* error) {
    return render_project_sequence(project, SequenceRenderOptions{}, progress,
                                   cancel, error);
}

bool render_project_sequence(const ProjectConfig& project,
                             const SequenceRenderOptions& options,
                             const ProgressCallback& progress,
                             const std::atomic_bool* cancel,
                             std::string* error) {
    clear_error(error);
    try {
        return render_project_sequence_impl(project, options, progress, cancel,
                                            error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Project sequence rendering ran out of memory.");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(error, "Project sequence rendering filesystem error: "
                           + std::string(exception.what()));
    } catch (const std::exception& exception) {
        return fail(error, "Project sequence rendering failed: "
                           + std::string(exception.what()));
    } catch (...) {
        return fail(error,
                    "Project sequence rendering failed with an unknown exception.");
    }
}

} // namespace pvt
