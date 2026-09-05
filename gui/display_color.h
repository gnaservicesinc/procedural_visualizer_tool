#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pvt::display {

// Display previews are already quantized to eight bits. Find the transitions
// of the existing float sRGB formula once, rather than evaluating pow three
// times per pixel on every frame. Computing the transitions on this host also
// retains its libm rounding at the boundaries of adjacent display codes.
class Srgb8Converter {
public:
    Srgb8Converter() {
        thresholds_[0] = 0.0F;
        thresholds_[256] = std::numeric_limits<float>::infinity();
        for (std::size_t code = 1; code < 256U; ++code) {
            float lower = 0.0F;
            float upper = 1.0F;
            for (;;) {
                const float midpoint = lower + (upper - lower) * 0.5F;
                if (midpoint == lower || midpoint == upper) break;
                if (reference(midpoint) < code) lower = midpoint;
                else upper = midpoint;
            }
            thresholds_[code] = upper;
        }
        std::size_t code = 0;
        for (std::size_t bucket = 0; bucket < kBucketCount; ++bucket) {
            const float lower = static_cast<float>(bucket)
                                / static_cast<float>(kBucketCount);
            while (lower >= thresholds_[code + 1U]) ++code;
            lower_codes_[bucket] = static_cast<unsigned char>(code);
        }
    }

    unsigned char operator()(float value) const {
        if (!std::isfinite(value) || value <= 0.0F) return 0;
        if (value >= 1.0F) return 255;
        const auto bucket = static_cast<std::size_t>(
            value * static_cast<float>(kBucketCount));
        const auto code = static_cast<std::size_t>(lower_codes_[bucket]);
        // The steepest sRGB segment spans less than one display code per
        // bucket (12.92 * 255 / 4096 < 1), so at most one transition remains.
        return static_cast<unsigned char>(
            code + static_cast<std::size_t>(value >= thresholds_[code + 1U]));
    }

private:
    static unsigned char reference(float value) {
        const float encoded = value <= 0.0031308F
            ? value * 12.92F
            : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
        return static_cast<unsigned char>(std::lround(encoded * 255.0F));
    }

    static constexpr std::size_t kBucketCount = 4096U;
    std::array<float, 257> thresholds_{};
    std::array<unsigned char, kBucketCount> lower_codes_{};
};

inline const Srgb8Converter& srgb8_converter() {
    // C++ guarantees initialization once even when editor and Live rendering
    // first request a display frame concurrently. The table is then read-only.
    static const Srgb8Converter converter;
    return converter;
}

inline void convert_rgba_row(const float* input, unsigned char* output,
                             std::size_t pixels) {
    const auto& convert = srgb8_converter();
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::size_t offset = pixel * 4U;
        output[offset] = convert(input[offset]);
        output[offset + 1U] = convert(input[offset + 1U]);
        output[offset + 2U] = convert(input[offset + 2U]);
        // Alpha remains linear, straight coverage, using the original rounding.
        output[offset + 3U] = static_cast<unsigned char>(std::lround(
            std::clamp(input[offset + 3U], 0.0F, 1.0F) * 255.0F));
    }
}

} // namespace pvt::display
