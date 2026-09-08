#pragma once

#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace pvt::palette_remix {

struct Settings {
    double hue_degrees = 0.0;
    double saturation = 1.0;
    double exposure_stops = 0.0;
    std::size_t offset = 0;
    bool reverse = false;
};

// Signed extensions let imported negative/HDR linear values participate without
// forcing them through QColor's bounded, quantized display representation.
inline double encode(double linear) {
    const double magnitude = std::abs(linear);
    return std::copysign(magnitude <= 0.0031308 ? 12.92 * magnitude
        : 1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055, linear);
}

inline double decode(double encoded) {
    const double magnitude = std::abs(encoded);
    return std::copysign(magnitude <= 0.04045 ? magnitude / 12.92
        : std::pow((magnitude + 0.055) / 1.055, 2.4), encoded);
}

inline PaletteConfig apply(const PaletteConfig& original, const Settings& settings) {
    // Always derive from the opening palette, never from a previous slider tick.
    PaletteConfig result = original;
    if (!std::isfinite(settings.hue_degrees)
        || !std::isfinite(settings.saturation)
        || !std::isfinite(settings.exposure_stops)) return result;
    const double hue = std::fmod(settings.hue_degrees, 360.0);
    const double saturation = std::clamp(settings.saturation, 0.0, 2.0);
    const double gain = std::exp2(std::clamp(settings.exposure_stops, -2.0, 2.0));
    if (hue != 0.0 || saturation != 1.0 || gain != 1.0) {
        for (auto& color : result.colors) {
            const bool linear = color.encoding == PaletteColorEncoding::Linear;
            std::array<double, 3> rgb = {color.red, color.green, color.blue};
            if (hue != 0.0 || saturation != 1.0) {
                if (linear) for (auto& channel : rgb) channel = encode(channel);
                const double high = *std::max_element(rgb.begin(), rgb.end());
                const double low = *std::min_element(rgb.begin(), rgb.end());
                const double chroma = high - low;
                if (chroma > 0.0) {
                    double sector = rgb[0] == high ? (rgb[1] - rgb[2]) / chroma
                        : rgb[1] == high ? 2.0 + (rgb[2] - rgb[0]) / chroma
                                         : 4.0 + (rgb[0] - rgb[1]) / chroma;
                    sector = std::fmod(sector + hue / 60.0 + 12.0, 6.0);
                    const double c = chroma * saturation;
                    const double x = c * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
                    const double m = high - c;
                    if (sector < 1.0) rgb = {c, x, 0.0};
                    else if (sector < 2.0) rgb = {x, c, 0.0};
                    else if (sector < 3.0) rgb = {0.0, c, x};
                    else if (sector < 4.0) rgb = {0.0, x, c};
                    else if (sector < 5.0) rgb = {x, 0.0, c};
                    else rgb = {c, 0.0, x};
                    for (auto& channel : rgb) channel += m;
                }
                if (linear) for (auto& channel : rgb) channel = decode(channel);
            }
            for (auto& channel : rgb) {
                if (linear) {
                    constexpr double limit = (std::numeric_limits<float>::max)();
                    channel = std::clamp(channel * gain, -limit, limit);
                } else {
                    if (gain != 1.0) channel = encode(decode(channel) * gain);
                    channel = std::clamp(channel, 0.0, 1.0);
                }
            }
            color.red = rgb[0];
            color.green = rgb[1];
            color.blue = rgb[2];
        }
    }
    if (settings.reverse) std::reverse(result.colors.begin(), result.colors.end());
    if (!result.colors.empty()) {
        const auto offset = settings.offset % result.colors.size();
        std::rotate(result.colors.begin(), result.colors.begin()
            + static_cast<std::ptrdiff_t>(offset), result.colors.end());
    }
    return result;
}

} // namespace pvt::palette_remix
