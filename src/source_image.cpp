#include "source_image.h"

#include "path_utf8.h"

#include <png.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

namespace fs = std::filesystem;

enum class DecodeIntent {
    Color,
    Data,
};

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

struct CachedSource {
    std::string path;
    DecodeIntent intent = DecodeIntent::Color;
    std::uintmax_t file_size = 0U;
    fs::file_time_type modified{};
    std::shared_ptr<const Image> image;
    std::size_t decoded_bytes = 0U;
    std::uint64_t last_used = 0U;
};

std::mutex source_cache_mutex;
std::vector<CachedSource> source_cache;
std::size_t source_cache_bytes = 0U;
std::uint64_t source_cache_clock = 0U;
constexpr std::size_t kMaximumCachedStartingImageBytes =
    std::size_t{512} * 1024U * 1024U;
constexpr std::size_t kMaximumCachedStartingImages = 64U;

bool inspect_source(const std::string& path, std::uintmax_t& file_size,
                    fs::file_time_type& modified, std::string* error) {
    if (path.empty() || path.find('\0') != std::string::npos) {
        return fail(error, "Image source path is empty or contains a NUL byte.");
    }
    std::error_code code;
    const fs::path native = path_from_utf8(path);
    const fs::file_status status = fs::symlink_status(native, code);
    if (code || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        return fail(error,
                    "Image source must be a readable regular file, not a link or special file.");
    }
    file_size = fs::file_size(native, code);
    if (code || file_size == 0U || file_size > kMaximumEmbeddedAssetBytes) {
        return fail(error, "Image source file is empty, unreadable, or exceeds the signed-int bundle-entry limit.");
    }
    modified = fs::last_write_time(native, code);
    return !code || fail(error, "Could not inspect the image source timestamp.");
}

std::FILE* open_source(const fs::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.wstring().c_str(), L"rb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool decode_png_color(const std::string& path,
                      std::shared_ptr<const Image>& decoded,
                      const std::atomic_bool* cancel, std::string* error) {
    if (cancelled(cancel)) return fail(error, "PNG source decoding was cancelled.");
    const fs::path native = path_from_utf8(path);
    std::FILE* file = open_source(native);
    if (file == nullptr) return fail(error, "Could not open the PNG source.");

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_stdio(&png, file) == 0) {
        const std::string message = png.message;
        std::fclose(file);
        return fail(error, "Could not read PNG source metadata: " + message);
    }
    const std::uint64_t decoded_pixels =
        static_cast<std::uint64_t>(png.width) * png.height;
    if (png.width == 0U || png.height == 0U
        || png.width > static_cast<png_uint_32>(std::numeric_limits<int>::max())
        || png.height > static_cast<png_uint_32>(std::numeric_limits<int>::max())
        || decoded_pixels
               > (std::numeric_limits<std::size_t>::max)()
                     / (4U * sizeof(png_uint_16))) {
        png_image_free(&png);
        std::fclose(file);
        return fail(error,
                    "PNG source dimensions are invalid or exceed addressable decoded storage.");
    }
    png.format = PNG_FORMAT_LINEAR_RGB_ALPHA;
    const int decoded_width = static_cast<int>(png.width);
    const int decoded_height = static_cast<int>(png.height);
    const std::size_t components = static_cast<std::size_t>(decoded_width)
                                   * static_cast<std::size_t>(decoded_height) * 4U;
    std::vector<png_uint_16> linear(components);
    if (png_image_finish_read(&png, nullptr, linear.data(), 0, nullptr) == 0) {
        const std::string message = png.message;
        png_image_free(&png);
        std::fclose(file);
        return fail(error, "Could not decode PNG source: " + message);
    }
    png_image_free(&png);
    std::fclose(file);
    if (cancelled(cancel)) return fail(error, "PNG source decoding was cancelled.");

    auto result = std::make_shared<Image>();
    result->width = decoded_width;
    result->height = decoded_height;
    result->pixels.resize(components);
    constexpr float scale = 1.0F / 65535.0F;
    for (std::size_t index = 0U; index < components; ++index) {
        if ((index & 65535U) == 0U && cancelled(cancel)) {
            return fail(error, "PNG source decoding was cancelled.");
        }
        result->pixels[index] = static_cast<float>(linear[index]) * scale;
    }
    decoded = std::move(result);
    return true;
}

struct PngReadError {
    char message[256]{};
};

void png_read_error(png_structp png, png_const_charp message) {
    auto* context = static_cast<PngReadError*>(png_get_error_ptr(png));
    if (context != nullptr) {
        const char* source = message != nullptr ? message : "unknown libpng error";
        std::strncpy(context->message, source, sizeof(context->message) - 1U);
        context->message[sizeof(context->message) - 1U] = '\0';
    }
    png_longjmp(png, 1);
}

bool decode_png_data(const std::string& path,
                     std::shared_ptr<const Image>& decoded,
                     const std::atomic_bool* cancel, std::string* error) {
    if (cancelled(cancel)) return fail(error, "PNG data decoding was cancelled.");
    std::FILE* file = open_source(path_from_utf8(path));
    if (file == nullptr) return fail(error, "Could not open the PNG data image.");

    PngReadError read_error;
    png_structp png = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, &read_error, png_read_error, nullptr);
    if (png == nullptr) {
        std::fclose(file);
        return fail(error, "Could not initialize the PNG data decoder.");
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(file);
        return fail(error, "Could not allocate PNG data metadata.");
    }

    unsigned char* raw = nullptr;
    if (setjmp(png_jmpbuf(png)) != 0) {
        std::free(raw);
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(file);
        return fail(error, "Could not decode PNG data image: "
                               + std::string(read_error.message[0] != '\0'
                                                 ? read_error.message
                                                 : "unknown libpng error"));
    }

    png_init_io(png, file);
    png_read_info(png, info);
    const png_uint_32 width = png_get_image_width(png, info);
    const png_uint_32 height = png_get_image_height(png, info);
    const int source_depth = png_get_bit_depth(png, info);
    const int color_type = png_get_color_type(png, info);
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U
        || width > static_cast<png_uint_32>((std::numeric_limits<int>::max)())
        || height > static_cast<png_uint_32>((std::numeric_limits<int>::max)())
        || pixel_count > (std::numeric_limits<std::size_t>::max)() / 8U) {
        png_error(png, "PNG data dimensions exceed addressable float storage");
    }
    if (source_depth != 1 && source_depth != 2 && source_depth != 4
        && source_depth != 8 && source_depth != 16) {
        png_error(png, "PNG data uses an unsupported sample depth");
    }

    const bool source_has_alpha = (color_type & PNG_COLOR_MASK_ALPHA) != 0
        || png_get_valid(png, info, PNG_INFO_tRNS) != 0;
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && source_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS) != 0) {
        png_set_tRNS_to_alpha(png);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY
        || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (!source_has_alpha) {
        png_set_add_alpha(png, source_depth == 16 ? 65535U : 255U,
                          PNG_FILLER_AFTER);
    }
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    const int output_depth = png_get_bit_depth(png, info);
    const int output_channels = png_get_channels(png, info);
    const png_size_t row_bytes = png_get_rowbytes(png, info);
    const std::size_t bytes_per_sample = output_depth == 16 ? 2U : 1U;
    const std::size_t expected_row = static_cast<std::size_t>(width)
                                     * 4U * bytes_per_sample;
    if ((output_depth != 8 && output_depth != 16)
        || output_channels != 4 || row_bytes != expected_row
        || static_cast<std::size_t>(height)
               > (std::numeric_limits<std::size_t>::max)() / expected_row) {
        png_error(png, "PNG data transforms produced an invalid row layout");
    }
    const std::size_t raw_bytes = expected_row * static_cast<std::size_t>(height);
    raw = static_cast<unsigned char*>(std::malloc(raw_bytes));
    if (raw == nullptr) png_error(png, "PNG data buffer allocation failed");
    for (int pass = 0; pass < passes; ++pass) {
        for (png_uint_32 row = 0U; row < height; ++row) {
            png_read_row(png,
                         raw + static_cast<std::size_t>(row) * expected_row,
                         nullptr);
        }
    }
    png_read_end(png, info);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(file);
    if (cancelled(cancel)) {
        std::free(raw);
        return fail(error, "PNG data decoding was cancelled.");
    }

    auto result = std::make_shared<Image>();
    result->width = static_cast<int>(width);
    result->height = static_cast<int>(height);
    const std::size_t components = static_cast<std::size_t>(pixel_count) * 4U;
    result->pixels.resize(components);
    const float scale = output_depth == 16 ? 1.0F / 65535.0F : 1.0F / 255.0F;
    std::size_t input = 0U;
    for (std::size_t index = 0U; index < components; ++index) {
        if ((index & 65535U) == 0U && cancelled(cancel)) {
            std::free(raw);
            return fail(error, "PNG data decoding was cancelled.");
        }
        const std::uint32_t sample = output_depth == 16
            ? (static_cast<std::uint32_t>(raw[input]) << 8U)
                  | static_cast<std::uint32_t>(raw[input + 1U])
            : static_cast<std::uint32_t>(raw[input]);
        input += bytes_per_sample;
        result->pixels[index] = static_cast<float>(sample) * scale;
    }
    std::free(raw);
    decoded = std::move(result);
    return true;
}

bool checked_size_multiply(std::size_t left, std::size_t right,
                           std::size_t& result) {
    if (left != 0U
        && right > (std::numeric_limits<std::size_t>::max)() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool read_exr_u32(const std::vector<unsigned char>& bytes,
                  std::size_t& position, std::uint32_t& value) {
    if (position > bytes.size() || bytes.size() - position < 4U) return false;
    value = static_cast<std::uint32_t>(bytes[position])
            | (static_cast<std::uint32_t>(bytes[position + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes[position + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes[position + 3U]) << 24U);
    position += 4U;
    return true;
}

bool read_exr_u64(const std::vector<unsigned char>& bytes,
                  std::size_t& position, std::uint64_t& value) {
    if (position > bytes.size() || bytes.size() - position < 8U) return false;
    value = 0U;
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[position++]) << shift;
    }
    return true;
}

bool read_exr_string(const std::vector<unsigned char>& bytes,
                     std::size_t& position, std::string& value,
                     std::size_t maximum) {
    value.clear();
    while (position < bytes.size() && bytes[position] != 0U) {
        if (value.size() >= maximum) return false;
        value.push_back(static_cast<char>(bytes[position++]));
    }
    if (position >= bytes.size()) return false;
    ++position;
    return true;
}

std::int32_t exr_signed(std::uint32_t bits) {
    std::int32_t result = 0;
    static_assert(sizeof(result) == sizeof(bits),
                  "OpenEXR coordinates require 32 bits");
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

float exr_float(std::uint32_t bits) {
    float result = 0.0F;
    static_assert(sizeof(result) == sizeof(bits),
                  "OpenEXR FLOAT requires 32 bits");
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

float exr_half(std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U)
                               << 16U;
    std::uint32_t exponent = (half >> 10U) & 0x1fU;
    std::uint32_t mantissa = half & 0x03ffU;
    std::uint32_t bits = 0U;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            bits = sign;
        } else {
            int shift = 0;
            while ((mantissa & 0x0400U) == 0U) {
                mantissa <<= 1U;
                ++shift;
            }
            mantissa &= 0x03ffU;
            const std::uint32_t float_exponent = static_cast<std::uint32_t>(
                127 - 15 + 1 - shift);
            bits = sign | (float_exponent << 23U) | (mantissa << 13U);
        }
    } else if (exponent == 0x1fU) {
        bits = sign | 0x7f800000U | (mantissa << 13U);
    } else {
        exponent += 127U - 15U;
        bits = sign | (exponent << 23U) | (mantissa << 13U);
    }
    return exr_float(bits);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return static_cast<char>(byte >= 'A' && byte <= 'Z'
                                     ? byte - 'A' + 'a' : byte);
    });
    return value;
}

struct ExrChannel {
    std::string name;
    std::string folded;
    std::uint32_t pixel_type = 0U;
    std::size_t bytes_per_sample = 0U;
};

struct ExrHeader {
    std::vector<ExrChannel> channels;
    std::uint8_t compression = 0U;
    std::int32_t minimum_x = 0;
    std::int32_t minimum_y = 0;
    std::int32_t maximum_x = -1;
    std::int32_t maximum_y = -1;
    std::uint8_t line_order = 0U;
    bool has_channels = false;
    bool has_compression = false;
    bool has_data_window = false;
};

bool parse_exr_channels(const std::vector<unsigned char>& bytes,
                        std::size_t begin, std::size_t size,
                        ExrHeader& header, std::string* error) {
    if (header.has_channels) {
        return fail(error, "OpenEXR has duplicate channel attributes.");
    }
    const std::size_t end = begin + size;
    std::size_t position = begin;
    std::set<std::string> names;
    while (position < end) {
        if (bytes[position] == 0U) {
            ++position;
            if (position != end) {
                return fail(error, "OpenEXR channel list has trailing data.");
            }
            header.has_channels = true;
            return true;
        }
        std::string name;
        const std::size_t start = position;
        while (position < end && bytes[position] != 0U
               && position - start <= 255U) {
            name.push_back(static_cast<char>(bytes[position++]));
        }
        if (position >= end || name.empty() || position - start > 255U) {
            return fail(error, "OpenEXR channel list is malformed.");
        }
        ++position;
        if (end - position < 16U) {
            return fail(error, "OpenEXR channel record is truncated.");
        }
        std::size_t field = position;
        std::uint32_t pixel_type = 0U;
        std::uint32_t x_sampling = 0U;
        std::uint32_t y_sampling = 0U;
        if (!read_exr_u32(bytes, field, pixel_type)) return false;
        field += 4U;
        if (!read_exr_u32(bytes, field, x_sampling)
            || !read_exr_u32(bytes, field, y_sampling)) {
            return fail(error, "OpenEXR channel record is truncated.");
        }
        position = field;
        if (pixel_type > 2U) {
            return fail(error, "OpenEXR channel uses an unknown pixel type.");
        }
        if (x_sampling != 1U || y_sampling != 1U) {
            return fail(error, "Subsampled OpenEXR channels are not supported.");
        }
        const std::string folded = lower_ascii(name);
        if (!names.insert(folded).second) {
            return fail(error, "OpenEXR has duplicate channel names.");
        }
        header.channels.push_back(
            {std::move(name), folded, pixel_type, pixel_type == 1U ? 2U : 4U});
        if (header.channels.size() > 64U) {
            return fail(error, "OpenEXR has too many channels.");
        }
    }
    return fail(error, "OpenEXR channel list has no terminator.");
}

bool parse_exr_header(const std::vector<unsigned char>& bytes,
                      std::size_t& position, ExrHeader& header,
                      std::string* error) {
    std::uint32_t magic = 0U;
    std::uint32_t version = 0U;
    if (!read_exr_u32(bytes, position, magic)
        || !read_exr_u32(bytes, position, version)
        || magic != 20000630U) {
        return fail(error, "Input is not an OpenEXR file.");
    }
    const std::uint32_t flags = version & 0xffffff00U;
    if ((version & 0xffU) != 2U || (flags & ~0x00000400U) != 0U) {
        return fail(error,
                    "Only single-part, non-tiled OpenEXR version 2 scanline images are supported.");
    }
    std::set<std::string> attributes;
    bool terminated = false;
    while (position < bytes.size()) {
        if (bytes[position] == 0U) {
            ++position;
            terminated = true;
            break;
        }
        std::string name;
        std::string type;
        if (!read_exr_string(bytes, position, name, 255U)
            || !read_exr_string(bytes, position, type, 255U)) {
            return fail(error, "OpenEXR header has an invalid attribute.");
        }
        if (!attributes.insert(name).second) {
            return fail(error, "OpenEXR header has duplicate attributes.");
        }
        std::uint32_t size = 0U;
        if (!read_exr_u32(bytes, position, size)
            || size > bytes.size() - position) {
            return fail(error, "OpenEXR header attribute exceeds the file bounds.");
        }
        const std::size_t begin = position;
        if (name == "channels") {
            if (type != "chlist"
                || !parse_exr_channels(bytes, begin, size, header, error)) {
                return false;
            }
        } else if (name == "compression") {
            if (type != "compression" || size != 1U
                || header.has_compression) {
                return fail(error, "OpenEXR compression attribute is malformed.");
            }
            header.compression = bytes[begin];
            header.has_compression = true;
        } else if (name == "dataWindow") {
            if (type != "box2i" || size != 16U || header.has_data_window) {
                return fail(error, "OpenEXR dataWindow attribute is malformed.");
            }
            std::size_t field = begin;
            std::uint32_t values[4]{};
            for (std::uint32_t& value : values) {
                if (!read_exr_u32(bytes, field, value)) return false;
            }
            header.minimum_x = exr_signed(values[0U]);
            header.minimum_y = exr_signed(values[1U]);
            header.maximum_x = exr_signed(values[2U]);
            header.maximum_y = exr_signed(values[3U]);
            header.has_data_window = true;
        } else if (name == "lineOrder") {
            if (type != "lineOrder" || size != 1U) {
                return fail(error, "OpenEXR lineOrder attribute is malformed.");
            }
            header.line_order = bytes[begin];
        }
        position += size;
    }
    if (!terminated || !header.has_channels || !header.has_compression
        || !header.has_data_window) {
        return fail(error,
                    "OpenEXR is missing its header terminator, channels, compression, or dataWindow.");
    }
    if (header.line_order != 0U) {
        return fail(error,
                    "Only increasing-Y OpenEXR scanline order is supported.");
    }
    if (header.compression > 3U) {
        return fail(error,
                    "OpenEXR compression is unsupported; use NONE, RLE, ZIPS, or ZIP.");
    }
    return true;
}

void undo_exr_predictor_and_shuffle(std::vector<unsigned char>& bytes) {
    for (std::size_t index = 1U; index < bytes.size(); ++index) {
        bytes[index] = static_cast<unsigned char>(
            static_cast<int>(bytes[index - 1U])
            + static_cast<int>(bytes[index]) - 128);
    }
    std::vector<unsigned char> interleaved(bytes.size());
    const unsigned char* even = bytes.data();
    const unsigned char* odd = bytes.data() + (bytes.size() + 1U) / 2U;
    for (std::size_t index = 0U; index < interleaved.size(); ++index) {
        interleaved[index] = (index & 1U) == 0U ? *even++ : *odd++;
    }
    bytes.swap(interleaved);
}

bool decode_exr_compressed(const unsigned char* source,
                           std::size_t source_size,
                           std::uint8_t compression,
                           std::vector<unsigned char>& decoded,
                           std::string* error) {
    if (source_size == decoded.size()) {
        std::copy_n(source, source_size, decoded.begin());
        return true;
    }
    if (compression == 1U) {
        std::vector<unsigned char> unpacked(decoded.size());
        std::size_t input = 0U;
        std::size_t output = 0U;
        while (input < source_size && output < unpacked.size()) {
            const int count = source[input] > 127U
                ? static_cast<int>(source[input++]) - 256
                : static_cast<int>(source[input++]);
            if (count < 0) {
                const std::size_t literal = static_cast<std::size_t>(-count);
                if (literal > source_size - input
                    || literal > unpacked.size() - output) {
                    return fail(error, "OpenEXR RLE literal exceeds its block.");
                }
                std::copy_n(source + input, literal, unpacked.data() + output);
                input += literal;
                output += literal;
            } else {
                const std::size_t run = static_cast<std::size_t>(count) + 1U;
                if (input >= source_size || run > unpacked.size() - output) {
                    return fail(error, "OpenEXR RLE run exceeds its block.");
                }
                std::fill_n(unpacked.data() + output, run, source[input++]);
                output += run;
            }
        }
        if (input != source_size || output != unpacked.size()) {
            return fail(error, "OpenEXR RLE block has an invalid decoded size.");
        }
        decoded.swap(unpacked);
    } else {
        if (compression != 2U && compression != 3U) {
            return fail(error, "OpenEXR compression method is unsupported.");
        }
        uLongf decoded_size = static_cast<uLongf>(decoded.size());
        const int result = ::uncompress(
            decoded.data(), &decoded_size, source,
            static_cast<uLong>(source_size));
        if (result != Z_OK || decoded_size != decoded.size()) {
            return fail(error, "Could not inflate OpenEXR ZIP scanline block.");
        }
    }
    undo_exr_predictor_and_shuffle(decoded);
    return true;
}

int exr_named_channel(const ExrHeader& header, const char* name,
                      bool exact_only) {
    const std::string target(name);
    for (std::size_t index = 0U; index < header.channels.size(); ++index) {
        const ExrChannel& channel = header.channels[index];
        if (channel.pixel_type != 1U && channel.pixel_type != 2U) continue;
        if (channel.folded == target) return static_cast<int>(index);
    }
    if (exact_only) return -1;
    const std::string suffix = "." + target;
    for (std::size_t index = 0U; index < header.channels.size(); ++index) {
        const ExrChannel& channel = header.channels[index];
        if (channel.pixel_type != 1U && channel.pixel_type != 2U) continue;
        if (channel.folded.size() > suffix.size()
            && channel.folded.compare(channel.folded.size() - suffix.size(),
                                      suffix.size(), suffix) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

bool read_exr_file(const std::string& path, std::uintmax_t file_size,
                   std::vector<unsigned char>& bytes, std::string* error) {
    if (file_size > (std::numeric_limits<std::size_t>::max)()) {
        return fail(error, "OpenEXR file exceeds addressable memory.");
    }
    std::FILE* file = open_source(path_from_utf8(path));
    if (file == nullptr) return fail(error, "Could not open the OpenEXR source.");
    bytes.resize(static_cast<std::size_t>(file_size));
    const std::size_t read = std::fread(bytes.data(), 1U, bytes.size(), file);
    const bool input_error = std::ferror(file) != 0;
    std::fclose(file);
    if (input_error || read != bytes.size()) {
        return fail(error, "Could not read the complete OpenEXR source.");
    }
    return true;
}

bool decode_exr(const std::string& path, std::uintmax_t file_size,
                std::shared_ptr<const Image>& decoded,
                const std::atomic_bool* cancel, std::string* error) {
    if (cancelled(cancel)) return fail(error, "OpenEXR decoding was cancelled.");
    std::vector<unsigned char> bytes;
    if (!read_exr_file(path, file_size, bytes, error)) return false;
    std::size_t position = 0U;
    ExrHeader header;
    if (!parse_exr_header(bytes, position, header, error)) return false;

    const std::int64_t signed_width = static_cast<std::int64_t>(header.maximum_x)
                                      - header.minimum_x + 1;
    const std::int64_t signed_height = static_cast<std::int64_t>(header.maximum_y)
                                       - header.minimum_y + 1;
    if (signed_width <= 0 || signed_height <= 0
        || signed_width > (std::numeric_limits<int>::max)()
        || signed_height > (std::numeric_limits<int>::max)()) {
        return fail(error, "OpenEXR dataWindow dimensions are invalid.");
    }
    const std::size_t width = static_cast<std::size_t>(signed_width);
    const std::size_t height = static_cast<std::size_t>(signed_height);
    std::size_t pixel_count = 0U;
    std::size_t components = 0U;
    if (!checked_size_multiply(width, height, pixel_count)
        || !checked_size_multiply(pixel_count, 4U, components)) {
        return fail(error, "OpenEXR decoded image dimensions overflow.");
    }

    std::array<int, 4U> selected{{-1, -1, -1, -1}};
    const std::array<const char*, 4U> names{{"r", "g", "b", "a"}};
    for (std::size_t semantic = 0U; semantic < selected.size(); ++semantic) {
        selected[semantic] = exr_named_channel(header, names[semantic], true);
        if (selected[semantic] < 0) {
            selected[semantic] = exr_named_channel(header, names[semantic], false);
        }
    }
    int scalar = exr_named_channel(header, "y", true);
    if (scalar < 0) scalar = exr_named_channel(header, "y", false);
    if (scalar < 0) {
        for (std::size_t semantic = 0U; semantic < 3U && scalar < 0;
             ++semantic) {
            scalar = selected[semantic];
        }
    }
    if (scalar < 0) {
        for (std::size_t index = 0U; index < header.channels.size(); ++index) {
            const ExrChannel& channel = header.channels[index];
            if ((channel.pixel_type == 1U || channel.pixel_type == 2U)
                && static_cast<int>(index) != selected[3U]) {
                scalar = static_cast<int>(index);
                break;
            }
        }
    }
    if ((selected[0U] < 0 || selected[1U] < 0 || selected[2U] < 0)
        && scalar < 0) {
        return fail(error,
                    "OpenEXR image has no HALF/FLOAT color or data channel.");
    }

    const std::size_t lines_per_block = header.compression == 3U ? 16U : 1U;
    const std::size_t chunk_count =
        (height + lines_per_block - 1U) / lines_per_block;
    std::vector<std::uint64_t> offsets(chunk_count);
    for (std::uint64_t& offset : offsets) {
        if (!read_exr_u64(bytes, position, offset) || offset > bytes.size()) {
            return fail(error,
                        "OpenEXR scanline offset table is truncated or invalid.");
        }
    }

    auto result = std::make_shared<Image>();
    result->width = static_cast<int>(width);
    result->height = static_cast<int>(height);
    result->pixels.assign(components, 0.0F);
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        result->pixels[pixel * 4U + 3U] = 1.0F;
    }
    std::vector<float> scalar_samples;
    if (scalar >= 0) scalar_samples.assign(pixel_count, 0.0F);
    std::vector<bool> rows_seen(height, false);

    std::size_t bytes_per_row = 0U;
    for (const ExrChannel& channel : header.channels) {
        std::size_t channel_bytes = 0U;
        if (!checked_size_multiply(width, channel.bytes_per_sample,
                                   channel_bytes)
            || channel_bytes
                   > (std::numeric_limits<std::size_t>::max)()
                         - bytes_per_row) {
            return fail(error, "OpenEXR scanline byte size overflows.");
        }
        bytes_per_row += channel_bytes;
    }

    for (const std::uint64_t offset : offsets) {
        if (cancelled(cancel)) {
            return fail(error, "OpenEXR decoding was cancelled.");
        }
        std::size_t chunk = static_cast<std::size_t>(offset);
        std::uint32_t encoded_y = 0U;
        std::uint32_t encoded_size = 0U;
        if (!read_exr_u32(bytes, chunk, encoded_y)
            || !read_exr_u32(bytes, chunk, encoded_size)
            || encoded_size > bytes.size() - chunk) {
            return fail(error,
                        "OpenEXR scanline block is outside the file bounds.");
        }
        const std::int32_t y = exr_signed(encoded_y);
        if (y < header.minimum_y || y > header.maximum_y) {
            return fail(error,
                        "OpenEXR scanline block has an invalid Y coordinate.");
        }
        const std::size_t first_row = static_cast<std::size_t>(
            y - header.minimum_y);
        const std::size_t row_count =
            (std::min)(lines_per_block, height - first_row);
        std::size_t decoded_bytes = 0U;
        if (!checked_size_multiply(bytes_per_row, row_count, decoded_bytes)) {
            return fail(error, "OpenEXR scanline block size overflows.");
        }
        std::vector<unsigned char> block(decoded_bytes);
        if (header.compression == 0U) {
            if (encoded_size != decoded_bytes) {
                return fail(error,
                            "Uncompressed OpenEXR scanline size is invalid.");
            }
            std::copy_n(bytes.data() + chunk, decoded_bytes, block.begin());
        } else if (!decode_exr_compressed(bytes.data() + chunk, encoded_size,
                                          header.compression, block, error)) {
            return false;
        }

        std::size_t input = 0U;
        for (std::size_t row = 0U; row < row_count; ++row) {
            const std::size_t destination_row = first_row + row;
            if (rows_seen[destination_row]) {
                return fail(error, "OpenEXR scanline blocks overlap.");
            }
            rows_seen[destination_row] = true;
            for (std::size_t channel_index = 0U;
                 channel_index < header.channels.size(); ++channel_index) {
                const ExrChannel& channel = header.channels[channel_index];
                for (std::size_t x = 0U; x < width; ++x) {
                    float sample = 0.0F;
                    if (channel.pixel_type == 1U) {
                        const std::uint16_t bits =
                            static_cast<std::uint16_t>(block[input])
                            | static_cast<std::uint16_t>(
                                  static_cast<std::uint16_t>(block[input + 1U])
                                  << 8U);
                        sample = exr_half(bits);
                    } else if (channel.pixel_type == 2U) {
                        const std::uint32_t bits =
                            static_cast<std::uint32_t>(block[input])
                            | (static_cast<std::uint32_t>(block[input + 1U])
                               << 8U)
                            | (static_cast<std::uint32_t>(block[input + 2U])
                               << 16U)
                            | (static_cast<std::uint32_t>(block[input + 3U])
                               << 24U);
                        sample = exr_float(bits);
                    }
                    input += channel.bytes_per_sample;
                    const std::size_t pixel = destination_row * width + x;
                    bool used = static_cast<int>(channel_index) == scalar;
                    if (used) scalar_samples[pixel] = sample;
                    for (std::size_t semantic = 0U;
                         semantic < selected.size(); ++semantic) {
                        if (selected[semantic]
                            == static_cast<int>(channel_index)) {
                            result->pixels[pixel * 4U + semantic] = sample;
                            used = true;
                        }
                    }
                    if (used && !std::isfinite(sample)) {
                        return fail(error,
                                    "OpenEXR image contains NaN or infinity in a selected channel.");
                    }
                }
            }
        }
        if (input != block.size()) {
            return fail(error,
                        "OpenEXR scanline block has trailing decoded data.");
        }
    }
    if (std::find(rows_seen.begin(), rows_seen.end(), false)
        != rows_seen.end()) {
        return fail(error,
                    "OpenEXR scanline table does not cover every row.");
    }
    for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel) {
        for (std::size_t semantic = 0U; semantic < 3U; ++semantic) {
            if (selected[semantic] < 0) {
                result->pixels[pixel * 4U + semantic] = scalar_samples[pixel];
            }
        }
        const float alpha = result->pixels[pixel * 4U + 3U];
        if (!std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
            return fail(error, "OpenEXR image alpha is outside [0, 1].");
        }
    }
    decoded = std::move(result);
    return true;
}

bool decode_source(const std::string& path, std::uintmax_t file_size,
                   DecodeIntent intent,
                   std::shared_ptr<const Image>& decoded,
                   const std::atomic_bool* cancel, std::string* error) {
    std::FILE* file = open_source(path_from_utf8(path));
    if (file == nullptr) return fail(error, "Could not open the image source.");
    std::array<unsigned char, 8U> signature{};
    const std::size_t bytes_read = std::fread(
        signature.data(), 1U, signature.size(), file);
    std::fclose(file);
    if (bytes_read == signature.size()
        && png_sig_cmp(signature.data(), 0U, signature.size()) == 0) {
        return intent == DecodeIntent::Data
            ? decode_png_data(path, decoded, cancel, error)
            : decode_png_color(path, decoded, cancel, error);
    }
    const std::uint32_t magic = static_cast<std::uint32_t>(signature[0U])
        | (static_cast<std::uint32_t>(signature[1U]) << 8U)
        | (static_cast<std::uint32_t>(signature[2U]) << 16U)
        | (static_cast<std::uint32_t>(signature[3U]) << 24U);
    if (bytes_read >= 4U && magic == 20000630U) {
        return decode_exr(path, file_size, decoded, cancel, error);
    }
    return fail(error,
                "Image source is neither a valid PNG nor a scanline OpenEXR file.");
}

bool load_cached(const std::string& path, DecodeIntent intent,
                 std::shared_ptr<const Image>& image,
                 const std::atomic_bool* cancel, std::string* error) {
    std::uintmax_t file_size = 0U;
    fs::file_time_type modified{};
    if (!inspect_source(path, file_size, modified, error)) return false;
    {
        const std::lock_guard<std::mutex> lock(source_cache_mutex);
        const auto found = std::find_if(
            source_cache.begin(), source_cache.end(),
            [&](const CachedSource& candidate) {
                return candidate.path == path
                       && candidate.intent == intent
                       && candidate.file_size == file_size
                       && candidate.modified == modified
                       && candidate.image;
            });
        if (found != source_cache.end()) {
            found->last_used = ++source_cache_clock;
            image = found->image;
            return true;
        }
    }
    std::shared_ptr<const Image> decoded;
    if (!decode_source(path, file_size, intent, decoded, cancel, error)) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(source_cache_mutex);
        const auto existing = std::find_if(
            source_cache.begin(), source_cache.end(),
            [&path, intent](const CachedSource& candidate) {
                return candidate.path == path && candidate.intent == intent;
            });
        if (existing != source_cache.end()) {
            source_cache_bytes -= existing->decoded_bytes;
            source_cache.erase(existing);
        }
        const std::size_t decoded_bytes =
            decoded->pixels.size() * sizeof(float);
        source_cache.push_back({path, intent, file_size, modified, decoded,
                                decoded_bytes, ++source_cache_clock});
        source_cache_bytes += decoded_bytes;
        while ((source_cache.size() > kMaximumCachedStartingImages
                || source_cache_bytes > kMaximumCachedStartingImageBytes)
               && source_cache.size() > 1U) {
            const auto oldest = std::min_element(
                source_cache.begin(), source_cache.end(),
                [](const CachedSource& left, const CachedSource& right) {
                    return left.last_used < right.last_used;
                });
            source_cache_bytes -= oldest->decoded_bytes;
            source_cache.erase(oldest);
        }
    }
    image = std::move(decoded);
    return true;
}

float sample_channel(const Image& image, double x, double y,
                     std::size_t channel, bool tile,
                     bool transparent_outside) {
    if (tile) {
        x = std::fmod(x, static_cast<double>(image.width));
        y = std::fmod(y, static_cast<double>(image.height));
        if (x < 0.0) x += image.width;
        if (y < 0.0) y += image.height;
    } else if (x < 0.0 || y < 0.0 || x > image.width - 1.0
               || y > image.height - 1.0) {
        if (transparent_outside) return 0.0F;
        x = std::clamp(x, 0.0, static_cast<double>(image.width - 1));
        y = std::clamp(y, 0.0, static_cast<double>(image.height - 1));
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, image.width - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, image.height - 1);
    const int x1 = tile ? (x0 + 1) % image.width : std::min(x0 + 1, image.width - 1);
    const int y1 = tile ? (y0 + 1) % image.height : std::min(y0 + 1, image.height - 1);
    const double tx = x - std::floor(x);
    const double ty = y - std::floor(y);
    const auto at = [&](int px, int py) {
        return image.pixels[(static_cast<std::size_t>(py)
                             * static_cast<std::size_t>(image.width)
                             + static_cast<std::size_t>(px)) * 4U + channel];
    };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return static_cast<float>(top + (bottom - top) * ty);
}

} // namespace

bool validate_starting_image_source(const std::string& path,
                                    std::string* error) {
    std::shared_ptr<const Image> decoded;
    const bool ok = load_starting_image_source(
        path, decoded, nullptr, error);
    if (ok && error != nullptr) error->clear();
    return ok;
}

bool load_starting_image_source(const std::string& path,
                                std::shared_ptr<const Image>& image,
                                const std::atomic_bool* cancel,
                                std::string* error) {
    const bool ok = load_cached(path, DecodeIntent::Color, image, cancel, error);
    if (ok && error != nullptr) error->clear();
    return ok;
}

bool validate_data_image_source(const std::string& path,
                                std::string* error) {
    std::shared_ptr<const Image> decoded;
    const bool ok = load_data_image_source(path, decoded, nullptr, error);
    if (ok && error != nullptr) error->clear();
    return ok;
}

bool load_data_image_source(const std::string& path,
                            std::shared_ptr<const Image>& image,
                            const std::atomic_bool* cancel,
                            std::string* error) {
    const bool ok = load_cached(path, DecodeIntent::Data, image, cancel, error);
    if (ok && error != nullptr) error->clear();
    return ok;
}

bool render_starting_image(const StartingImageConfig& source,
                           int destination_width, int destination_height,
                           Image& destination, const std::atomic_bool* cancel,
                           std::string* error) {
    std::shared_ptr<const Image> decoded;
    if (!load_starting_image_source(
            source.path, decoded, cancel, error)) {
        return false;
    }
    Image result;
    result.width = destination_width;
    result.height = destination_height;
    result.pixels.assign(static_cast<std::size_t>(destination_width)
                             * static_cast<std::size_t>(destination_height) * 4U,
                         0.0F);
    const double sx = static_cast<double>(destination_width) / decoded->width;
    const double sy = static_cast<double>(destination_height) / decoded->height;
    const double fit_scale = source.fit == StartingImageFit::Contain
                                 ? std::min(sx, sy) : std::max(sx, sy);
    for (int y = 0; y < destination_height; ++y) {
        if (cancelled(cancel)) return fail(error, "Starting image rendering was cancelled.");
        for (int x = 0; x < destination_width; ++x) {
            double source_x = 0.0;
            double source_y = 0.0;
            bool tile = source.fit == StartingImageFit::Tile;
            const bool transparent_outside =
                source.fit == StartingImageFit::Contain;
            if (source.fit == StartingImageFit::Stretch) {
                source_x = (x + 0.5) / sx - 0.5;
                source_y = (y + 0.5) / sy - 0.5;
            } else if (tile) {
                source_x = static_cast<double>(x);
                source_y = static_cast<double>(y);
            } else {
                source_x = (x - destination_width * 0.5) / fit_scale
                           + decoded->width * 0.5;
                source_y = (y - destination_height * 0.5) / fit_scale
                           + decoded->height * 0.5;
            }
            const std::size_t offset =
                (static_cast<std::size_t>(y)
                 * static_cast<std::size_t>(destination_width)
                 + static_cast<std::size_t>(x)) * 4U;
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                result.pixels[offset + channel] =
                    sample_channel(*decoded, source_x, source_y, channel, tile,
                                   transparent_outside);
            }
        }
    }
    destination = std::move(result);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace pvt::detail
