#include "procedural_visualizer_tool.h"
#include "path_utf8.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
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
                      std::string* error) {
    const int channels = config.alpha.enabled ? 4 : 3;
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
    png_set_compression_level(png, 6);
    png_write_info(png, info);

    const std::uint32_t maximum = bit_depth == 16 ? 65535U : 255U;
    for (int y = 0; y < image.height; ++y) {
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
                      std::string* error) {
    // OpenEXR scanline files store channels alphabetically. Each scanline chunk
    // contains one planar run per channel in the same order as the chlist.
    const std::array<ExrChannel, 4> rgba_channels = {{
        {"A", 3}, {"B", 2}, {"G", 1}, {"R", 0}
    }};
    const std::array<ExrChannel, 3> rgb_channels = {{
        {"B", 2}, {"G", 1}, {"R", 0}
    }};
    const ExrChannel* channels = config.alpha.enabled ? rgba_channels.data()
                                                       : rgb_channels.data();
    const std::size_t channel_count = config.alpha.enabled ? rgba_channels.size()
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
        little_endian.clear();
        append_u64(little_endian,
                   first_chunk + static_cast<std::uint64_t>(y) * chunk_bytes);
        if (!write_vector(file, little_endian)) {
            return fail(error, "Writing the OpenEXR scanline offset table failed.");
        }
    }

    std::vector<unsigned char> scanline(scanline_bytes);
    for (int y = 0; y < image.height; ++y) {
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

bool write_image_impl(const std::string& path,
                      const Image& image,
                      const RenderConfig& config,
                      std::uint32_t deterministic_seed,
                      std::string* error) {
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

    TemporaryOutput temporary;
    if (!temporary.open_next_to(destination, error)) {
        return false;
    }

    const bool encoded = config.output.bit_depth == 32
                             ? write_exr_stream(temporary.file(), image, config, error)
                             : write_png_stream(temporary.file(), image, config,
                                                deterministic_seed, error);
    if (!encoded) {
        return false;
    }
#if !defined(_WIN32)
    // Apply the final mode after encoding: on some POSIX systems, writing a
    // file can clear its set-user-ID or set-group-ID bits.
    if (preserve_permissions
        && !temporary.set_permissions(preserved_permissions, error)) {
        return false;
    }
#endif
    if (!temporary.close_and_sync(error)) {
        return false;
    }
    if (!install_temporary(temporary.path(), destination,
                           config.output.overwrite_existing, error)) {
        return false;
    }
    temporary.dismiss();
#if !defined(_WIN32)
    // The file data was synced before installation. Sync the containing
    // directory as well so the new or replaced name is durable across a crash.
    sync_directory_best_effort(parent);
#endif
    return true;
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

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
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

bool render_sequence_impl(const RenderConfig& config,
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

    const fs::path directory = detail::path_from_utf8(config.output.output_directory);
    if (!ensure_directory(directory, error)) {
        return false;
    }

    // Inspect every final name before rendering. write_image repeats the check
    // atomically at install time so another process cannot exploit the gap.
    for (int frame_index = 0; frame_index < config.total_frames; ++frame_index) {
        if (cancelled(cancel)) {
            return fail(error, "Rendering was cancelled during output preflight.");
        }
        fs::path path;
        if (!build_frame_path(config, frame_index, &path, error)) {
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

    Image image;
    for (int frame_index = 0; frame_index < config.total_frames; ++frame_index) {
        if (cancelled(cancel)) {
            return fail(error, "Rendering was cancelled.");
        }

        std::string frame_error;
        if (!render_frame(config, frame_index, image, &frame_error)) {
            return fail(error, "Could not render frame " + std::to_string(frame_index)
                               + ": " + frame_error);
        }
        fs::path path;
        if (!build_frame_path(config, frame_index, &path, error)) {
            return false;
        }
        if (!write_image(detail::path_to_utf8(path), image, config, kSequenceDitherSeed,
                         &frame_error)) {
            return fail(error, "Could not export frame " + std::to_string(frame_index)
                               + ": " + frame_error);
        }
        if (!report_progress(progress, frame_index + 1, config.total_frames, error)) {
            return false;
        }
    }
    return true;
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
    clear_error(error);
    try {
        return render_sequence_impl(config, progress, cancel, error);
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

} // namespace pvt
