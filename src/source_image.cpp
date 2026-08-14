#include "source_image.h"

#include "path_utf8.h"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

namespace fs = std::filesystem;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

struct CachedSource {
    std::string path;
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
        return fail(error, "Starting image path is empty or contains a NUL byte.");
    }
    std::error_code code;
    const fs::path native = path_from_utf8(path);
    const fs::file_status status = fs::symlink_status(native, code);
    if (code || !fs::is_regular_file(status) || fs::is_symlink(status)) {
        return fail(error,
                    "Starting image must be a readable regular file, not a link or special file.");
    }
    file_size = fs::file_size(native, code);
    if (code || file_size == 0U || file_size > kMaximumEmbeddedAssetBytes) {
        return fail(error, "Starting image file is empty, unreadable, or exceeds the signed-int bundle-entry limit.");
    }
    modified = fs::last_write_time(native, code);
    return !code || fail(error, "Could not inspect the starting image timestamp.");
}

std::FILE* open_source(const fs::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.wstring().c_str(), L"rb") == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

bool decode_png(const std::string& path, std::shared_ptr<const Image>& decoded,
                const std::atomic_bool* cancel, std::string* error) {
    if (cancelled(cancel)) return fail(error, "Starting image decoding was cancelled.");
    const fs::path native = path_from_utf8(path);
    std::FILE* file = open_source(native);
    if (file == nullptr) return fail(error, "Could not open the starting image.");

    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_stdio(&png, file) == 0) {
        const std::string message = png.message;
        std::fclose(file);
        return fail(error, "Could not read starting PNG metadata: " + message);
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
                    "Starting PNG dimensions are invalid or exceed addressable decoded storage.");
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
        return fail(error, "Could not decode starting PNG: " + message);
    }
    png_image_free(&png);
    std::fclose(file);
    if (cancelled(cancel)) return fail(error, "Starting image decoding was cancelled.");

    auto result = std::make_shared<Image>();
    result->width = decoded_width;
    result->height = decoded_height;
    result->pixels.resize(components);
    constexpr float scale = 1.0F / 65535.0F;
    for (std::size_t index = 0U; index < components; ++index) {
        if ((index & 65535U) == 0U && cancelled(cancel)) {
            return fail(error, "Starting image decoding was cancelled.");
        }
        result->pixels[index] = static_cast<float>(linear[index]) * scale;
    }
    decoded = std::move(result);
    return true;
}

bool load_cached(const std::string& path, std::shared_ptr<const Image>& image,
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
    if (!decode_png(path, decoded, cancel, error)) return false;
    {
        const std::lock_guard<std::mutex> lock(source_cache_mutex);
        const auto existing = std::find_if(
            source_cache.begin(), source_cache.end(),
            [&path](const CachedSource& candidate) {
                return candidate.path == path;
            });
        if (existing != source_cache.end()) {
            source_cache_bytes -= existing->decoded_bytes;
            source_cache.erase(existing);
        }
        const std::size_t decoded_bytes =
            decoded->pixels.size() * sizeof(float);
        source_cache.push_back({path, file_size, modified, decoded,
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
    const bool ok = load_cached(path, image, cancel, error);
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
