#include "environment_map.h"

#include "source_image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace pvt::detail {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTau = 2.0 * kPi;
constexpr double kInvSqrtTwo = 0.707106781186547524400844362104849039;

bool fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
}

struct Direction {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

Direction cross(Direction left, Direction right) noexcept {
    return {left.y * right.z - left.z * right.y,
            left.z * right.x - left.x * right.z,
            left.x * right.y - left.y * right.x};
}

Direction normalize(Direction value) noexcept {
    const double length_squared = value.x * value.x + value.y * value.y
                                  + value.z * value.z;
    if (!std::isfinite(length_squared) || length_squared <= 0.0) return {};
    const double inverse_length = 1.0 / std::sqrt(length_squared);
    return {value.x * inverse_length, value.y * inverse_length,
            value.z * inverse_length};
}

Direction offset_direction(Direction normal, Direction axis,
                           double sign) noexcept {
    return normalize({normal.x * kInvSqrtTwo
                          + axis.x * (sign * kInvSqrtTwo),
                      normal.y * kInvSqrtTwo
                          + axis.y * (sign * kInvSqrtTwo),
                      normal.z * kInvSqrtTwo
                          + axis.z * (sign * kInvSqrtTwo)});
}

int wrap_index(int value, int extent) noexcept {
    const int wrapped = value % extent;
    return wrapped < 0 ? wrapped + extent : wrapped;
}

EnvironmentMapRgb sample_direction(const PreparedEnvironmentMap& environment,
                                   Direction direction) noexcept {
    const Image& image = *environment.image;
    const double longitude = std::atan2(direction.x, direction.z);
    double u = 0.5 + longitude / kTau + environment.rotation_turns;
    u -= std::floor(u);
    const double v = std::clamp(
        0.5 - std::asin(std::clamp(direction.y, -1.0, 1.0)) / kPi,
        0.0, 1.0);

    // Pixel-center coordinates make bilinear sampling continuous across U=0
    // while correctly clamping the first and last rows at the poles.
    const double x = u * static_cast<double>(image.width) - 0.5;
    const double y = v * static_cast<double>(image.height) - 0.5;
    const int x0_unwrapped = static_cast<int>(std::floor(x));
    const int y0_unclamped = static_cast<int>(std::floor(y));
    const int x0 = wrap_index(x0_unwrapped, image.width);
    const int x1 = wrap_index(x0_unwrapped + 1, image.width);
    const int y0 = std::clamp(y0_unclamped, 0, image.height - 1);
    const int y1 = std::clamp(y0_unclamped + 1, 0, image.height - 1);
    const double tx = x - std::floor(x);
    const double ty = std::clamp(y - std::floor(y), 0.0, 1.0);
    const auto channel = [&](std::size_t component) noexcept {
        const auto at = [&](int px, int py) noexcept {
            const std::size_t index =
                (static_cast<std::size_t>(py)
                     * static_cast<std::size_t>(image.width)
                 + static_cast<std::size_t>(px))
                    * 4U
                + component;
            return std::max(0.0, static_cast<double>(image.pixels[index]));
        };
        const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
        const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
        return top + (bottom - top) * ty;
    };
    return {static_cast<float>(channel(0U)),
            static_cast<float>(channel(1U)),
            static_cast<float>(channel(2U))};
}

float saturated_radiance(double value, double scale) noexcept {
    if (!(value > 0.0) || !(scale > 0.0)) return 0.0F;
    const double maximum = (std::numeric_limits<float>::max)();
    return static_cast<float>(std::min(value * scale, maximum));
}

} // namespace

bool prepare_environment_map(const EnvironmentMapConfig& config,
                             PreparedEnvironmentMap& prepared,
                             const std::atomic_bool* cancel,
                             std::string* error) {
    PreparedEnvironmentMap candidate;
    if (!config.enabled) {
        prepared = std::move(candidate);
        if (error != nullptr) error->clear();
        return true;
    }
    if (!std::isfinite(config.rotation_degrees)
        || !std::isfinite(config.exposure_stops)
        || !std::isfinite(config.intensity) || config.intensity < 0.0
        || !std::isfinite(config.mix) || config.mix < 0.0
        || config.mix > 1.0) {
        return fail(error,
                    "Environment-map rotation, exposure, intensity, or mix is invalid.");
    }
    switch (config.encoding) {
        case EnvironmentMapEncoding::Auto:
        case EnvironmentMapEncoding::Srgb:
        case EnvironmentMapEncoding::Linear:
            break;
        default:
            return fail(error, "Environment-map encoding is invalid.");
    }
    if (!load_environment_map_source(config.path, config.encoding,
                                     candidate.image, cancel, error)) {
        return false;
    }
    if (!candidate.image || candidate.image->width <= 0
        || candidate.image->height <= 0
        || candidate.image->pixels.size()
               != static_cast<std::size_t>(candidate.image->width)
                      * static_cast<std::size_t>(candidate.image->height) * 4U) {
        return fail(error, "Decoded environment map has invalid storage.");
    }

    candidate.rotation_turns = std::remainder(
        config.rotation_degrees / 360.0, 1.0);
    candidate.mix = config.mix;
    if (config.intensity == 0.0) {
        candidate.radiance_scale = 0.0;
    } else {
        const double exponent = config.exposure_stops
                                + std::log2(config.intensity);
        const double maximum_exponent = std::log2(
            static_cast<double>((std::numeric_limits<float>::max)()));
        const double minimum_exponent = std::log2(
            static_cast<double>((std::numeric_limits<float>::denorm_min)()));
        if (!std::isfinite(exponent) || exponent >= maximum_exponent) {
            candidate.radiance_scale = exponent < 0.0
                ? 0.0
                : static_cast<double>((std::numeric_limits<float>::max)());
        } else if (exponent <= minimum_exponent) {
            candidate.radiance_scale = 0.0;
        } else {
            candidate.radiance_scale = std::exp2(exponent);
        }
    }
    prepared = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

EnvironmentMapRgb sample_environment_map_diffuse(
    const PreparedEnvironmentMap& environment,
    double normal_x, double normal_y, double normal_z) noexcept {
    if (!environment.image || environment.image->width <= 0
        || environment.image->height <= 0) {
        return {};
    }
    const std::size_t width = static_cast<std::size_t>(environment.image->width);
    const std::size_t height = static_cast<std::size_t>(environment.image->height);
    if (width > (std::numeric_limits<std::size_t>::max)() / height
        || width * height
               > (std::numeric_limits<std::size_t>::max)() / 4U
        || environment.image->pixels.size() != width * height * 4U) {
        return {};
    }

    const Direction normal = normalize({normal_x, normal_y, normal_z});
    const double normal_length_squared = normal.x * normal.x
                                         + normal.y * normal.y
                                         + normal.z * normal.z;
    if (!(normal_length_squared > 0.0)) return {};
    const Direction reference = std::abs(normal.y) < 0.999
        ? Direction{0.0, 1.0, 0.0}
        : Direction{1.0, 0.0, 0.0};
    const Direction tangent = normalize(cross(reference, normal));
    const Direction bitangent = cross(normal, tangent);
    const std::array<Direction, 5U> directions{{
        normal,
        offset_direction(normal, tangent, 1.0),
        offset_direction(normal, tangent, -1.0),
        offset_direction(normal, bitangent, 1.0),
        offset_direction(normal, bitangent, -1.0),
    }};
    // The center receives twice the weight of each 45-degree ring sample.
    // These positive normalized weights preserve a constant environment exactly.
    constexpr std::array<double, 5U> weights{{
        1.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0,
    }};
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (std::size_t index = 0U; index < directions.size(); ++index) {
        const EnvironmentMapRgb sample = sample_direction(
            environment, directions[index]);
        red += static_cast<double>(sample.red) * weights[index];
        green += static_cast<double>(sample.green) * weights[index];
        blue += static_cast<double>(sample.blue) * weights[index];
    }
    return {saturated_radiance(red, environment.radiance_scale),
            saturated_radiance(green, environment.radiance_scale),
            saturated_radiance(blue, environment.radiance_scale)};
}

} // namespace pvt::detail
