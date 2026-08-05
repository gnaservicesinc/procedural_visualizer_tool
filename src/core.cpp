#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace pvt {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTau = 2.0 * kPi;
constexpr int kMaximumDimension = 16384;
constexpr int kMaximumFrames = 1000000;
constexpr std::size_t kMaximumPeakBytes = std::size_t{1} << 30;
constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4096;
constexpr std::size_t kMaximumPrefixBytes = 128;

struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 0.0;
};

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

double clamp_value(double value, double minimum, double maximum) {
    return std::max(minimum, std::min(maximum, value));
}

double mix_value(double first, double second, double amount) {
    return first + (second - first) * amount;
}

double smoothstep(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value * value * (3.0 - 2.0 * value);
}

double radians(double degrees) {
    return degrees * kPi / 180.0;
}

double wrap_unit(double value) {
    value = std::fmod(value, 1.0);
    return value < 0.0 ? value + 1.0 : value;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

ValidationResult invalid_result(const std::string& message,
                                std::size_t estimated_peak_bytes = 0) {
    ValidationResult result;
    result.ok = false;
    result.message = message;
    result.estimated_peak_bytes = estimated_peak_bytes;
    return result;
}

bool finite_in_range(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

bool valid_name(const std::string& value) {
    if (value.size() > kMaximumNameBytes) {
        return false;
    }
    for (char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0 || character == 0x7f
            || (character < 0x20 && character != '\t')) {
            return false;
        }
    }
    return true;
}

bool valid_path_text(const std::string& value, std::size_t maximum_size, bool prefix) {
    if (value.empty() || value.size() >= maximum_size) {
        return false;
    }
    for (char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0 || character < 0x20 || character == 0x7f) {
            return false;
        }
        if (prefix) {
            switch (character) {
                case '<':
                case '>':
                case ':':
                case '"':
                case '/':
                case '\\':
                case '|':
                case '?':
                case '*':
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

bool valid_enum(EdgeMode value) {
    switch (value) {
        case EdgeMode::Alpha:
        case EdgeMode::Black:
        case EdgeMode::White:
        case EdgeMode::Reflect:
            return true;
    }
    return false;
}

bool valid_enum(EffectType value) {
    switch (value) {
        case EffectType::EndlessZoom:
        case EffectType::Ripple:
        case EffectType::Shake:
        case EffectType::FlagWave:
        case EffectType::Glow:
            return true;
    }
    return false;
}

bool valid_enum(DitherMethod value) {
    switch (value) {
        case DitherMethod::BlueNoise:
        case DitherMethod::OrderedBayer:
        case DitherMethod::FloydSteinberg:
            return true;
    }
    return false;
}

bool valid_enum(SurfaceMapping value) {
    switch (value) {
        case SurfaceMapping::Plane:
        case SurfaceMapping::Cylinder:
        case SurfaceMapping::Sphere:
        case SurfaceMapping::Cube:
            return true;
    }
    return false;
}

bool valid_enum(Waveform value) {
    switch (value) {
        case Waveform::Sine:
        case Waveform::Triangle:
        case Waveform::SmoothPulse:
        case Waveform::Bounce:
            return true;
    }
    return false;
}

bool valid_enum(QuantizationMode value) {
    switch (value) {
        case QuantizationMode::Rgb:
        case QuantizationMode::Luminance:
        case QuantizationMode::Hue:
            return true;
    }
    return false;
}

bool effect_has_render_work(const EffectConfig& effect) {
    if (!effect.enabled || effect.intensity <= 0.0) {
        return false;
    }
    return effect.type == EffectType::Glow ? effect.radius_pixels > 0.0
                                           : effect.magnitude > 0.0;
}

bool surface_has_render_work(const SurfaceConfig& surface) {
    if (!surface.enabled) {
        return false;
    }
    if (surface.mapping != SurfaceMapping::Plane) {
        return surface.curvature > 0.0;
    }
    const bool identity_rotation = surface.rotations_per_loop == 0
                                   && std::fmod(surface.phase_degrees, 360.0) == 0.0;
    return !identity_rotation;
}

double srgb_to_linear(double value) {
    value = clamp_value(value, 0.0, 1.0);
    return value <= 0.04045
               ? value / 12.92
               : std::pow((value + 0.055) / 1.055, 2.4);
}

Color hsl_to_linear_rgb(double hue, double saturation, double lightness) {
    hue = std::fmod(hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    saturation = clamp_value(saturation, 0.0, 1.0);
    lightness = clamp_value(lightness, 0.0, 1.0);

    const double chroma = (1.0 - std::fabs(2.0 * lightness - 1.0)) * saturation;
    const double hue_prime = hue / 60.0;
    const double x = chroma * (1.0 - std::fabs(std::fmod(hue_prime, 2.0) - 1.0));
    const double match = lightness - chroma / 2.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;

    if (hue_prime < 1.0) {
        red = chroma;
        green = x;
    } else if (hue_prime < 2.0) {
        red = x;
        green = chroma;
    } else if (hue_prime < 3.0) {
        green = chroma;
        blue = x;
    } else if (hue_prime < 4.0) {
        green = x;
        blue = chroma;
    } else if (hue_prime < 5.0) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }

    return {srgb_to_linear(red + match),
            srgb_to_linear(green + match),
            srgb_to_linear(blue + match),
            1.0};
}

double evaluate_waveform(Waveform waveform, double phase, double shape) {
    const double sine = std::sin(phase);
    switch (waveform) {
        case Waveform::Sine:
            return sine;
        case Waveform::Triangle:
            return (2.0 / kPi) * std::asin(clamp_value(sine, -1.0, 1.0));
        case Waveform::SmoothPulse: {
            const double sharpness = 0.25 + 8.0 * clamp_value(shape, 0.0, 1.0);
            return std::tanh(sharpness * sine) / std::tanh(sharpness);
        }
        case Waveform::Bounce:
            return 1.0 - 2.0 * std::fabs(std::sin(phase * 0.5));
    }
    return 0.0;
}

double master_motion_phase(const RenderConfig& config, double loop_phase) {
    double result = loop_phase + config.phrase_warp * std::sin(loop_phase);
    for (const SwingConfig& swing : config.swings) {
        if (!swing.enabled) {
            continue;
        }
        const double swing_phase = static_cast<double>(swing.cycles_per_loop) * loop_phase
                                   + radians(swing.phase_degrees);
        result += swing.amount * evaluate_waveform(swing.waveform, swing_phase, swing.shape);
    }
    return result;
}

double wave_coordinate(const WaveConfig& wave, double x, double y,
                       const RenderConfig& config) {
    const double short_side = static_cast<double>(std::min(config.width, config.height));
    const double source_x = wave.x_percent * 0.01 * static_cast<double>(config.width);
    const double source_y = wave.y_percent * 0.01 * static_cast<double>(config.height);
    const double dx = (x - source_x) / short_side;
    const double dy = (y - source_y) / short_side;
    const double radial = std::hypot(dx, dy);

    if (wave.direction < 0.5) {
        return mix_value(radial, dx, 1.0 - 2.0 * wave.direction);
    }
    return mix_value(radial, dy, 2.0 * wave.direction - 1.0);
}

double wave_height(const RenderConfig& config, double x, double y,
                   double loop_phase, double motion_phase) {
    double height = 0.0;
    for (const WaveConfig& wave : config.waves) {
        if (!wave.enabled) {
            continue;
        }
        const double clock = wave.synchronized ? motion_phase : loop_phase;
        const double phase = static_cast<double>(wave.cycles_per_loop) * clock;
        height += wave.amplitude
                  * std::sin(kTau * wave.spatial_frequency
                                 * wave_coordinate(wave, x, y, config)
                             - phase + radians(wave.phase_degrees));
    }
    return height;
}

void ensure_image(Image& image, int width, int height) {
    image.width = width;
    image.height = height;
    const std::size_t count = static_cast<std::size_t>(width)
                              * static_cast<std::size_t>(height) * 4U;
    image.pixels.resize(count);
}

float stored_channel(double value) {
    // Valid configurations are bounded so normal rendering never reaches this
    // guard. Keep the final double-to-float conversion defensive as well: a
    // malformed future effect must not leak NaN/Inf into PNG or EXR writers.
    if (std::isnan(value)) {
        return 0.0F;
    }
    constexpr double maximum = static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(clamp_value(value, -maximum, maximum));
}

std::size_t pixel_offset_unchecked(const Image& image, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width)
            + static_cast<std::size_t>(x)) * 4U;
}

void store_color(Image& image, int x, int y, const Color& color) {
    float* pixel = image.pixels.data() + pixel_offset_unchecked(image, x, y);
    pixel[0] = stored_channel(color.r);
    pixel[1] = stored_channel(color.g);
    pixel[2] = stored_channel(color.b);
    pixel[3] = static_cast<float>(clamp_value(color.a, 0.0, 1.0));
}

Color load_color(const Image& image, int x, int y) {
    const float* pixel = image.pixels.data() + pixel_offset_unchecked(image, x, y);
    return {pixel[0], pixel[1], pixel[2], pixel[3]};
}

int reflected_index(long long index, int size) {
    if (size <= 1) {
        return 0;
    }
    const long long period = 2LL * static_cast<long long>(size - 1);
    index %= period;
    if (index < 0) {
        index += period;
    }
    if (index >= size) {
        index = period - index;
    }
    return static_cast<int>(index);
}

Color edge_color(EdgeMode mode) {
    switch (mode) {
        case EdgeMode::Alpha:
            return {0.0, 0.0, 0.0, 0.0};
        case EdgeMode::Black:
            return {0.0, 0.0, 0.0, 1.0};
        case EdgeMode::White:
            return {1.0, 1.0, 1.0, 1.0};
        case EdgeMode::Reflect:
            break;
    }
    return {0.0, 0.0, 0.0, 0.0};
}

Color sample_texel(const Image& image, long long x, long long y, EdgeMode mode) {
    if (x >= 0 && x < image.width && y >= 0 && y < image.height) {
        return load_color(image, static_cast<int>(x), static_cast<int>(y));
    }
    if (mode != EdgeMode::Reflect) {
        return edge_color(mode);
    }
    return load_color(image, reflected_index(x, image.width),
                      reflected_index(y, image.height));
}

Color sample_bilinear(const Image& image, double x, double y, EdgeMode mode) {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return edge_color(mode);
    }
    const double bounded_x = clamp_value(
        x, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const double bounded_y = clamp_value(
        y, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const long long x0 = static_cast<long long>(std::floor(bounded_x));
    const long long y0 = static_cast<long long>(std::floor(bounded_y));
    const double tx = bounded_x - static_cast<double>(x0);
    const double ty = bounded_y - static_cast<double>(y0);
    const std::array<Color, 4> samples = {
        sample_texel(image, x0, y0, mode),
        sample_texel(image, x0 + 1, y0, mode),
        sample_texel(image, x0, y0 + 1, mode),
        sample_texel(image, x0 + 1, y0 + 1, mode)};
    const std::array<double, 4> weights = {
        (1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
        (1.0 - tx) * ty, tx * ty};

    Color result;
    double rgb_weight = 0.0;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        // RGB is intentionally independent of coverage. This is straight alpha:
        // a fully transparent procedural pixel still retains its useful color
        // for alpha-aware compositors and later transforms.
        const long long sample_x = x0 + static_cast<long long>(index & 1U);
        const long long sample_y = y0 + static_cast<long long>(index >> 1U);
        const bool outside = sample_x < 0 || sample_x >= image.width
                             || sample_y < 0 || sample_y >= image.height;
        // Alpha edge samples represent missing coverage, not black paint. Do
        // not let those synthetic transparent texels darken straight RGB; the
        // alpha interpolation alone supplies the edge fade. Transparent texels
        // that are genuinely inside the image still contribute their RGB.
        if (mode != EdgeMode::Alpha || !outside) {
            result.r += samples[index].r * weights[index];
            result.g += samples[index].g * weights[index];
            result.b += samples[index].b * weights[index];
            rgb_weight += weights[index];
        }
        result.a += samples[index].a * weights[index];
    }
    if (mode == EdgeMode::Alpha && rgb_weight > 1.0e-12) {
        result.r /= rgb_weight;
        result.g /= rgb_weight;
        result.b /= rgb_weight;
    }
    result.a = clamp_value(result.a, 0.0, 1.0);
    return result;
}

Color sample_bilinear_wrapped_x(const Image& image, double x, double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || image.width <= 0 || image.height <= 0) {
        return {};
    }
    const double width = static_cast<double>(image.width);
    double wrapped_x = std::fmod(x, width);
    if (wrapped_x < 0.0) {
        wrapped_x += width;
    }
    const long long x0 = static_cast<long long>(std::floor(wrapped_x));
    const long long x1 = (x0 + 1LL) % image.width;
    const double bounded_y = clamp_value(
        y, static_cast<double>(std::numeric_limits<long long>::min() / 4),
        static_cast<double>(std::numeric_limits<long long>::max() / 4));
    const long long y0 = static_cast<long long>(std::floor(bounded_y));
    const double tx = wrapped_x - static_cast<double>(x0);
    const double ty = bounded_y - static_cast<double>(y0);
    const std::array<Color, 4> samples = {
        load_color(image, static_cast<int>(x0), reflected_index(y0, image.height)),
        load_color(image, static_cast<int>(x1), reflected_index(y0, image.height)),
        load_color(image, static_cast<int>(x0), reflected_index(y0 + 1, image.height)),
        load_color(image, static_cast<int>(x1), reflected_index(y0 + 1, image.height))};
    const std::array<double, 4> weights = {
        (1.0 - tx) * (1.0 - ty), tx * (1.0 - ty),
        (1.0 - tx) * ty, tx * ty};
    Color result;
    for (std::size_t index = 0; index < samples.size(); ++index) {
        result.r += samples[index].r * weights[index];
        result.g += samples[index].g * weights[index];
        result.b += samples[index].b * weights[index];
        result.a += samples[index].a * weights[index];
    }
    result.a = clamp_value(result.a, 0.0, 1.0);
    return result;
}

Color blend_straight_alpha(const Color& first, const Color& second, double amount) {
    amount = clamp_value(amount, 0.0, 1.0);
    const double first_weight = 1.0 - amount;
    return {first.r * first_weight + second.r * amount,
            first.g * first_weight + second.g * amount,
            first.b * first_weight + second.b * amount,
            first.a * first_weight + second.a * amount};
}

bool image_pixel_offset(const Image& image, int x, int y, std::size_t& offset) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return false;
    }
    std::size_t expected_pixels = 0;
    std::size_t expected_components = 0;
    if (!checked_multiply(static_cast<std::size_t>(image.width),
                          static_cast<std::size_t>(image.height), expected_pixels)
        || !checked_multiply(expected_pixels, 4U, expected_components)
        || image.pixels.size() != expected_components) {
        return false;
    }
    std::size_t row_offset = 0;
    std::size_t pixel_offset = 0;
    if (!checked_multiply(static_cast<std::size_t>(y),
                          static_cast<std::size_t>(image.width), row_offset)
        || row_offset > std::numeric_limits<std::size_t>::max()
                             - static_cast<std::size_t>(x)) {
        return false;
    }
    pixel_offset = row_offset + static_cast<std::size_t>(x);
    if (!checked_multiply(pixel_offset, 4U, offset)
        || offset > image.pixels.size()
                        || image.pixels.size() - offset < 4U) {
        return false;
    }
    return true;
}

} // namespace

float* Image::pixel(int x, int y) {
    std::size_t index = 0;
    if (!image_pixel_offset(*this, x, y, index)) {
        return nullptr;
    }
    return pixels.data() + index;
}

const float* Image::pixel(int x, int y) const {
    std::size_t index = 0;
    if (!image_pixel_offset(*this, x, y, index)) {
        return nullptr;
    }
    return pixels.data() + index;
}

const char* effect_type_name(EffectType value) {
    switch (value) {
        case EffectType::EndlessZoom: return "Endless zoom";
        case EffectType::Ripple: return "Ripple";
        case EffectType::Shake: return "Shake";
        case EffectType::FlagWave: return "Flag wave";
        case EffectType::Glow: return "Glow";
    }
    return "Unknown";
}

const char* edge_mode_name(EdgeMode value) {
    switch (value) {
        case EdgeMode::Alpha: return "Alpha";
        case EdgeMode::Black: return "Black";
        case EdgeMode::White: return "White";
        case EdgeMode::Reflect: return "Reflected pattern";
    }
    return "Unknown";
}

const char* dither_method_name(DitherMethod value) {
    switch (value) {
        case DitherMethod::BlueNoise: return "Blue noise";
        case DitherMethod::OrderedBayer: return "Ordered Bayer";
        case DitherMethod::FloydSteinberg: return "Floyd-Steinberg";
    }
    return "Unknown";
}

const char* surface_mapping_name(SurfaceMapping value) {
    switch (value) {
        case SurfaceMapping::Plane: return "Plane";
        case SurfaceMapping::Cylinder: return "Cylinder";
        case SurfaceMapping::Sphere: return "Sphere";
        case SurfaceMapping::Cube: return "Cube";
    }
    return "Unknown";
}

const char* waveform_name(Waveform value) {
    switch (value) {
        case Waveform::Sine: return "Sine";
        case Waveform::Triangle: return "Triangle";
        case Waveform::SmoothPulse: return "Smooth pulse";
        case Waveform::Bounce: return "Bounce";
    }
    return "Unknown";
}

const char* quantization_mode_name(QuantizationMode value) {
    switch (value) {
        case QuantizationMode::Rgb: return "RGB";
        case QuantizationMode::Luminance: return "Luminance";
        case QuantizationMode::Hue: return "Hue";
    }
    return "Unknown";
}

WaveConfig default_wave(std::size_t index) {
    WaveConfig wave;
    wave.name = "Wave " + std::to_string(index + 1U);
    if (index == 0U) {
        wave.x_percent = 50.0000;
        wave.y_percent = 50.0000;
        wave.amplitude = 0.55;
        wave.spatial_frequency = 3.7815;
        wave.phase_degrees = 25.7831;
    } else if (index == 1U) {
        wave.x_percent = 29.1667;
        wave.y_percent = 26.8519;
        wave.amplitude = 0.25;
        wave.spatial_frequency = 9.4538;
        wave.phase_degrees = 25.7831;
    } else if (index == 2U) {
        wave.x_percent = 70.8333;
        wave.y_percent = 73.1481;
        wave.amplitude = 0.20;
        wave.spatial_frequency = 12.0321;
        wave.phase_degrees = 25.7831;
    } else {
        const double angle = std::fmod(static_cast<double>(index) * 137.50776405003785,
                                      360.0);
        wave.x_percent = 50.0 + 32.0 * std::cos(radians(angle));
        wave.y_percent = 50.0 + 32.0 * std::sin(radians(angle));
        wave.amplitude = 0.20;
        wave.spatial_frequency = 4.0 + static_cast<double>(index % 9U);
        wave.phase_degrees = angle;
    }
    return wave;
}

SwingConfig default_swing(std::size_t index) {
    SwingConfig swing;
    swing.name = "Swing " + std::to_string(index + 1U);
    if (index == 0U) {
        swing.amount = 0.28;
        swing.cycles_per_loop = 16;
    } else {
        swing.amount = 0.15;
        swing.cycles_per_loop = 4 + static_cast<int>(index % 13U);
        swing.phase_degrees = std::fmod(static_cast<double>(index) * 137.50776405003785,
                                       360.0);
    }
    return swing;
}

EffectConfig default_effect(EffectType type) {
    EffectConfig effect;
    effect.type = type;
    effect.name = effect_type_name(type);
    effect.enabled = false;
    switch (type) {
        case EffectType::EndlessZoom:
            effect.intensity = 1.0;
            effect.magnitude = 1.0;
            effect.frequency = 1.0;
            break;
        case EffectType::Ripple:
            effect.intensity = 0.65;
            effect.magnitude = 0.03;
            effect.frequency = 6.0;
            break;
        case EffectType::Shake:
            effect.intensity = 0.5;
            effect.magnitude = 0.02;
            effect.frequency = 3.0;
            effect.secondary = 0.8;
            break;
        case EffectType::FlagWave:
            effect.intensity = 0.6;
            effect.magnitude = 0.03;
            effect.frequency = 3.0;
            effect.secondary = 0.35;
            break;
        case EffectType::Glow:
            effect.intensity = 0.8;
            effect.secondary = 0.35;
            effect.radius_pixels = 12.0;
            effect.threshold = 0.65;
            effect.soft_knee = 0.25;
            break;
    }
    return effect;
}

RenderConfig default_config() {
    RenderConfig config;
    config.waves.reserve(3);
    for (std::size_t index = 0; index < 3; ++index) {
        WaveConfig wave = default_wave(index);
        wave.id = static_cast<std::uint64_t>(index) + 1U;
        config.waves.push_back(std::move(wave));
    }

    config.swings.push_back(default_swing(0));
    config.swings[0].id = 4;

    config.effects.reserve(5);
    const std::array<EffectType, 5> types = {
        EffectType::EndlessZoom, EffectType::Ripple, EffectType::Shake,
        EffectType::FlagWave, EffectType::Glow};
    for (std::size_t index = 0; index < types.size(); ++index) {
        EffectConfig effect = default_effect(types[index]);
        effect.id = static_cast<std::uint64_t>(index) + 5U;
        config.effects.push_back(std::move(effect));
    }
    return config;
}

std::uint64_t allocate_id(const RenderConfig& config) {
    std::unordered_set<std::uint64_t> used;
    used.reserve(config.waves.size() + config.swings.size() + config.effects.size());
    std::uint64_t maximum = 0;
    const auto remember = [&](std::uint64_t id) {
        if (id != 0) {
            used.insert(id);
            maximum = std::max(maximum, id);
        }
    };
    for (const WaveConfig& wave : config.waves) remember(wave.id);
    for (const SwingConfig& swing : config.swings) remember(swing.id);
    for (const EffectConfig& effect : config.effects) remember(effect.id);

    if (maximum != std::numeric_limits<std::uint64_t>::max()) {
        return maximum + 1U;
    }
    for (std::uint64_t candidate = 1; candidate != 0; ++candidate) {
        if (used.find(candidate) == used.end()) {
            return candidate;
        }
    }
    return 0;
}

ValidationResult validate_impl(const RenderConfig& config, bool include_export) {
    if (config.width < 16 || config.width > kMaximumDimension
        || config.height < 16 || config.height > kMaximumDimension) {
        return invalid_result("Width and height must each be between 16 and 16384 pixels.");
    }
    if (config.block_size < 1
        || config.block_size > std::max(config.width, config.height)) {
        return invalid_result("Block size must be between 1 and the larger image dimension.");
    }
    if (config.total_frames < 2 || config.total_frames > kMaximumFrames) {
        return invalid_result("Frame count must be between 2 and 1000000.");
    }
    if (!finite_in_range(config.fps, 1.0, 240.0)) {
        return invalid_result("FPS must be finite and between 1 and 240.");
    }
    if (config.waves.size() > kMaximumWaves) {
        return invalid_result("The configuration contains too many waves.");
    }
    if (config.swings.size() > kMaximumSwings) {
        return invalid_result("The configuration contains too many swings.");
    }
    if (config.effects.size() > kMaximumEffects) {
        return invalid_result("The configuration contains too many effects.");
    }

    std::unordered_set<std::uint64_t> identifiers;
    identifiers.reserve(config.waves.size() + config.swings.size() + config.effects.size());
    const auto accept_id = [&identifiers](std::uint64_t id) {
        return id != 0 && identifiers.insert(id).second;
    };

    for (std::size_t index = 0; index < config.waves.size(); ++index) {
        const WaveConfig& wave = config.waves[index];
        if (!accept_id(wave.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(wave.name)) {
            return invalid_result("Wave " + std::to_string(index + 1U)
                                  + " has an invalid or overlong name.");
        }
        if (!finite_in_range(wave.x_percent, -100.0, 200.0)
            || !finite_in_range(wave.y_percent, -100.0, 200.0)
            || !finite_in_range(wave.amplitude, 0.0, 10.0)
            || !finite_in_range(wave.spatial_frequency, 0.0, 1000.0)
            || wave.cycles_per_loop < -1000 || wave.cycles_per_loop > 1000
            || !finite_in_range(wave.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(wave.direction, 0.0, 1.0)) {
            return invalid_result("Wave " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
    }

    for (std::size_t index = 0; index < config.swings.size(); ++index) {
        const SwingConfig& swing = config.swings[index];
        if (!accept_id(swing.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(swing.name) || !valid_enum(swing.waveform)
            || !finite_in_range(swing.amount, -2.0, 2.0)
            || swing.cycles_per_loop < 0 || swing.cycles_per_loop > 1000
            || !finite_in_range(swing.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(swing.shape, 0.0, 1.0)) {
            return invalid_result("Swing " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
    }

    bool has_enabled_effect = false;
    bool has_enabled_glow = false;
    bool has_transparent_edge_effect = false;
    // Base generation is bounded near unit range. Track a deliberately
    // conservative upper bound for sequential additive glow amplification so
    // every accepted setup remains representable by 32-bit float channels.
    double logarithmic_color_bound = std::log(8.0);
    for (std::size_t index = 0; index < config.effects.size(); ++index) {
        const EffectConfig& effect = config.effects[index];
        if (!accept_id(effect.id)) {
            return invalid_result("Every wave, swing, and effect must have a unique nonzero ID.");
        }
        if (!valid_name(effect.name) || !valid_enum(effect.type)
            || !valid_enum(effect.edge_mode)
            || effect.cycles_per_loop < -1000 || effect.cycles_per_loop > 1000
            || !finite_in_range(effect.phase_degrees, -36000.0, 36000.0)
            || !finite_in_range(effect.intensity, 0.0, 100.0)
            || !finite_in_range(effect.magnitude, 0.0, 10.0)
            || !finite_in_range(effect.frequency, 0.0, 1000.0)
            || !finite_in_range(effect.secondary, -100.0, 100.0)
            || !finite_in_range(effect.center_x, -10.0, 10.0)
            || !finite_in_range(effect.center_y, -10.0, 10.0)
            || !finite_in_range(effect.angle_degrees, -36000.0, 36000.0)
            || !finite_in_range(effect.radius_pixels, 0.0,
                                static_cast<double>(kMaximumDimension))
            || !finite_in_range(effect.threshold, 0.0, 64.0)
            || !finite_in_range(effect.soft_knee, 0.0, 1.0)) {
            return invalid_result("Effect " + std::to_string(index + 1U)
                                  + " has a value outside its allowed range.");
        }
        const bool active_effect = effect_has_render_work(effect);
        const bool active_glow = active_effect && effect.type == EffectType::Glow;
        has_enabled_effect = has_enabled_effect || active_effect;
        has_enabled_glow = has_enabled_glow || active_glow;
        has_transparent_edge_effect = has_transparent_edge_effect
                                      || (active_effect
                                          && effect.type != EffectType::Glow
                                          && effect.edge_mode == EdgeMode::Alpha);
        if (active_glow) {
            logarithmic_color_bound += std::log1p(effect.intensity);
        }
    }
    if (logarithmic_color_bound
        >= std::log(static_cast<double>(std::numeric_limits<float>::max()))) {
        return invalid_result(
            "The enabled glow stack can exceed the 32-bit float color range; "
            "reduce glow intensity or the number of enabled glow effects.");
    }

    if (!finite_in_range(config.phrase_warp, 0.0, 2.0)
        || !finite_in_range(config.ghost_mix, 0.0, 1.0)
        || !finite_in_range(config.ghost_lag_degrees, -360.0, 360.0)
        || !finite_in_range(config.displacement, 0.0, 1000.0)
        || !finite_in_range(config.wave_depth, 0.0, 10.0)
        || !finite_in_range(config.spiral_frequency, 0.0, 1000.0)
        || config.spiral_arms < -100 || config.spiral_arms > 100
        || !finite_in_range(config.wall_frequency, 0.0, 1000.0)
        || !finite_in_range(config.wall_mix, 0.0, 5.0)
        || config.hue_cycles < -100 || config.hue_cycles > 100
        || !finite_in_range(config.saturation, 0.0, 1.0)) {
        return invalid_result("One or more pattern, rhythm, or lighting values are out of range.");
    }

    if (!finite_in_range(config.alpha.minimum, 0.0, 1.0)
        || !finite_in_range(config.alpha.maximum, 0.0, 1.0)
        || config.alpha.minimum > config.alpha.maximum
        || !finite_in_range(config.alpha.spatial_frequency, 0.0, 1000.0)
        || config.alpha.cycles_per_loop < -1000
        || config.alpha.cycles_per_loop > 1000
        || !finite_in_range(config.alpha.phase_degrees, -36000.0, 36000.0)) {
        return invalid_result("Alpha modulation values are out of range.");
    }
    if (config.quantization.levels < 2 || config.quantization.levels > 65536
        || !finite_in_range(config.quantization.mix, 0.0, 1.0)
        || !valid_enum(config.quantization.mode)) {
        return invalid_result("Quantization values are out of range.");
    }
    if (!valid_enum(config.surface.mapping)
        || config.surface.rotations_per_loop < -1000
        || config.surface.rotations_per_loop > 1000
        || !finite_in_range(config.surface.phase_degrees, -36000.0, 36000.0)
        || !finite_in_range(config.surface.curvature, 0.0, 1.0)
        || !finite_in_range(config.surface.lighting, 0.0, 10.0)) {
        return invalid_result("Surface mapping values are out of range.");
    }
    if (include_export) {
        const bool has_transparent_surface =
            surface_has_render_work(config.surface)
            && config.surface.mapping != SurfaceMapping::Plane;
        if (!config.alpha.enabled
            && (has_transparent_edge_effect || has_transparent_surface)) {
            return invalid_result(
                "Alpha output must be enabled when an active effect uses transparent "
                "edge handling or an active 3D surface has a transparent exterior.");
        }
        if (config.output.bit_depth != 8 && config.output.bit_depth != 16
            && config.output.bit_depth != 32) {
            return invalid_result("Export bit depth must be 8, 16, or 32.");
        }
        if (!valid_enum(config.output.dither_method)
            || !valid_path_text(config.output.output_directory, kMaximumPathBytes, false)
            || !valid_path_text(config.output.filename_prefix, kMaximumPrefixBytes, true)
            || config.output.first_frame_number < 0
            || config.output.first_frame_number > 1000000000
            || config.output.filename_digits < 1 || config.output.filename_digits > 12) {
            return invalid_result("One or more output values are invalid.");
        }
    }

    std::size_t pixel_count = 0;
    std::size_t float_count = 0;
    std::size_t frame_bytes = 0;
    if (!checked_multiply(static_cast<std::size_t>(config.width),
                          static_cast<std::size_t>(config.height), pixel_count)
        || !checked_multiply(pixel_count, 4U, float_count)
        || !checked_multiply(float_count, sizeof(float), frame_bytes)) {
        return invalid_result("The requested image dimensions overflow addressable memory.");
    }

    // Rendering is transactional: a previously rendered destination remains
    // alive until the new frame succeeds and is swapped into it.
    std::size_t buffer_count = 2U;
    if (has_enabled_effect || surface_has_render_work(config.surface)) {
        ++buffer_count;
    }
    if (has_enabled_glow) {
        ++buffer_count;
    }
    std::size_t peak_bytes = 0;
    if (!checked_multiply(frame_bytes, buffer_count, peak_bytes)) {
        return invalid_result("The renderer's peak memory estimate overflowed.");
    }
    if (peak_bytes > kMaximumPeakBytes) {
        std::ostringstream message;
        message << "Estimated peak image memory is "
                << (peak_bytes / (1024U * 1024U))
                << " MiB, above the 1024 MiB safety budget.";
        return invalid_result(message.str(), peak_bytes);
    }

    ValidationResult result;
    result.ok = true;
    result.message = "Configuration is valid.";
    result.estimated_peak_bytes = peak_bytes;
    return result;
}

ValidationResult validate(const RenderConfig& config) {
    return validate_impl(config, true);
}

namespace {

double alpha_at(const RenderConfig& config, int x, int y, double loop_phase) {
    if (!config.alpha.enabled) {
        return 1.0;
    }
    const double width_scale = config.width > 1
                                   ? static_cast<double>(x) / (config.width - 1)
                                   : 0.0;
    const double height_scale = config.height > 1
                                    ? static_cast<double>(y) / (config.height - 1)
                                    : 0.0;
    const double spatial = (width_scale + height_scale) * 0.7071067811865476;
    const double phase = kTau * config.alpha.spatial_frequency * spatial
                         - static_cast<double>(config.alpha.cycles_per_loop) * loop_phase
                         + radians(config.alpha.phase_degrees);
    const double amount = 0.5 + 0.5 * std::sin(phase);
    return mix_value(config.alpha.minimum, config.alpha.maximum, amount);
}

void generate_base_image(const RenderConfig& config, double loop_phase,
                         double motion_phase, Image& image) {
    ensure_image(image, config.width, config.height);
    const double short_side = static_cast<double>(std::min(config.width, config.height));
    double center_x = 0.5 * static_cast<double>(config.width);
    double center_y = 0.5 * static_cast<double>(config.height);
    for (const WaveConfig& wave : config.waves) {
        if (wave.enabled) {
            center_x = wave.x_percent * 0.01 * static_cast<double>(config.width);
            center_y = wave.y_percent * 0.01 * static_cast<double>(config.height);
            break;
        }
    }

    const double ghost_phase = motion_phase - radians(config.ghost_lag_degrees);
    const double breath = 0.85 + 0.35 * std::sin(loop_phase);

    for (int block_y = 0; block_y < config.height; block_y += config.block_size) {
        for (int block_x = 0; block_x < config.width; block_x += config.block_size) {
            const double height_here = wave_height(config, block_x, block_y,
                                                   loop_phase, motion_phase);
            const double height_right = wave_height(config,
                                                     block_x + config.block_size,
                                                     block_y,
                                                     loop_phase, motion_phase);
            const double height_down = wave_height(config, block_x,
                                                    block_y + config.block_size,
                                                    loop_phase, motion_phase);
            const double slope_x = height_right - height_here;
            const double slope_y = height_down - height_here;
            const double displacement = config.displacement_enabled
                                            ? config.displacement * breath
                                            : 0.0;
            const double displaced_x = block_x + slope_x * displacement;
            const double displaced_y = block_y + slope_y * displacement;
            const double dx = displaced_x - center_x;
            const double dy = displaced_y - center_y;
            const double normalized_distance = std::hypot(dx, dy) / short_side;
            const double angle = std::atan2(dy, dx);
            const double wall_distance = std::min(
                std::min(static_cast<double>(block_x),
                         static_cast<double>(config.width - 1 - block_x)),
                std::min(static_cast<double>(block_y),
                         static_cast<double>(config.height - 1 - block_y)));
            const double normalized_wall_distance = wall_distance / short_side;

            const double main_spiral = config.spiral_enabled
                ? std::sin(kTau * config.spiral_frequency * normalized_distance
                           + angle * config.spiral_arms - motion_phase)
                : 0.0;
            const double ghost_spiral = config.spiral_enabled
                ? std::sin(kTau * config.spiral_frequency * normalized_distance
                           + angle * config.spiral_arms - ghost_phase)
                : 0.0;
            const double main_wall = config.wall_reflection_enabled
                ? std::sin(kTau * config.wall_frequency * normalized_wall_distance
                           + 2.0 * motion_phase)
                : 0.0;
            const double ghost_wall = config.wall_reflection_enabled
                ? std::sin(kTau * config.wall_frequency * normalized_wall_distance
                           + 2.0 * ghost_phase)
                : 0.0;
            const double main_signal = main_spiral + config.wall_mix * main_wall;
            const double ghost_signal = ghost_spiral + config.wall_mix * ghost_wall;
            const double combined_signal = (1.0 - config.ghost_mix) * main_signal
                                           + config.ghost_mix * ghost_signal;
            const double hue = (combined_signal + 1.45) * 260.0
                               + 360.0 * config.hue_cycles * (loop_phase / kTau);

            double lightness = 0.40;
            if (config.lighting_enabled) {
                const double reflection = (slope_x + slope_y) * -0.7071067811865476;
                const double normalized_light = reflection * config.wave_depth * breath;
                lightness += normalized_light < 0.0
                                 ? 0.36 * normalized_light
                                 : 0.28 * normalized_light;
            }
            lightness = clamp_value(lightness, 0.04, 0.68);
            const Color base = hsl_to_linear_rgb(hue, config.saturation, lightness);
            const int end_x = std::min(block_x + config.block_size, config.width);
            const int end_y = std::min(block_y + config.block_size, config.height);
            for (int y = block_y; y < end_y; ++y) {
                for (int x = block_x; x < end_x; ++x) {
                    Color output = base;
                    output.a = alpha_at(config, x, y, loop_phase);
                    store_color(image, x, y, output);
                }
            }
        }
    }
}

double effect_phase(const EffectConfig& effect, double loop_phase,
                    double motion_phase) {
    const double clock = effect.synchronized ? motion_phase : loop_phase;
    return static_cast<double>(effect.cycles_per_loop) * clock
           + radians(effect.phase_degrees);
}

void apply_coordinate_effect(const Image& source, Image& destination,
                             const EffectConfig& effect, double phase) {
    ensure_image(destination, source.width, source.height);
    const double short_side = static_cast<double>(std::min(source.width, source.height));
    const double center_x = effect.center_x * static_cast<double>(source.width - 1);
    const double center_y = effect.center_y * static_cast<double>(source.height - 1);
    const double intensity = std::max(0.0, effect.intensity);
    const double displacement = effect.magnitude * short_side * intensity;
    const double angle = radians(effect.angle_degrees);
    const double axis_x = std::cos(angle);
    const double axis_y = std::sin(angle);
    const double perpendicular_x = -axis_y;
    const double perpendicular_y = axis_x;

    const int shake_harmonic = std::max(1, std::min(1000,
        static_cast<int>(std::lround(effect.frequency))));
    const double shake_x = displacement
        * (0.72 * std::sin(shake_harmonic * phase)
           + 0.20 * std::sin((shake_harmonic + 2) * phase + 1.234)
           + 0.08 * std::sin((shake_harmonic + 4) * phase + 3.456));
    const double shake_y = displacement * effect.secondary
        * (0.70 * std::cos((shake_harmonic + 1) * phase + 0.731)
           + 0.22 * std::sin((shake_harmonic + 3) * phase + 2.718)
           + 0.08 * std::cos((shake_harmonic + 5) * phase + 4.123));
    const double rotated_shake_x = axis_x * shake_x - axis_y * shake_y;
    const double rotated_shake_y = axis_y * shake_x + axis_x * shake_y;

    const double zoom_fraction = wrap_unit(phase / kTau);
    const double zoom_blend = smoothstep(zoom_fraction);
    const double zoom_octaves = clamp_value(
        effect.magnitude * std::max(0.01, effect.frequency), 0.0, 4.0);
    const double zoom_ratio = std::pow(2.0, zoom_octaves);
    const double zoom_scale_a = std::pow(zoom_ratio, zoom_fraction);
    const double zoom_scale_b = zoom_ratio > 1.0e-12
                                    ? zoom_scale_a / zoom_ratio
                                    : zoom_scale_a;

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            double sample_x = static_cast<double>(x);
            double sample_y = static_cast<double>(y);
            Color sampled;

            switch (effect.type) {
                case EffectType::EndlessZoom: {
                    if (effect.magnitude <= 1.0e-12 || intensity <= 1.0e-12) {
                        sampled = load_color(source, x, y);
                        break;
                    }
                    const double relative_x = x - center_x;
                    const double relative_y = y - center_y;
                    const Color first = sample_bilinear(
                        source, center_x + relative_x / zoom_scale_a,
                        center_y + relative_y / zoom_scale_a, effect.edge_mode);
                    const Color second = sample_bilinear(
                        source, center_x + relative_x / zoom_scale_b,
                        center_y + relative_y / zoom_scale_b, effect.edge_mode);
                    const Color zoomed = blend_straight_alpha(first, second, zoom_blend);
                    sampled = blend_straight_alpha(load_color(source, x, y), zoomed,
                                                   clamp_value(intensity, 0.0, 1.0));
                    break;
                }
                case EffectType::Ripple: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double distance = std::hypot(dx, dy);
                    if (distance > 1.0e-12) {
                        const double wave = std::sin(kTau * effect.frequency
                                                     * (distance / short_side) - phase);
                        const double attenuation = effect.secondary > 1.0
                            ? 1.0 / (1.0 + (effect.secondary - 1.0)
                                           * distance / short_side)
                            : 1.0;
                        sample_x -= dx / distance * displacement * wave * attenuation;
                        sample_y -= dy / distance * displacement * wave * attenuation;
                    }
                    sampled = sample_bilinear(source, sample_x, sample_y, effect.edge_mode);
                    break;
                }
                case EffectType::Shake:
                    sampled = sample_bilinear(source, x - rotated_shake_x,
                                              y - rotated_shake_y, effect.edge_mode);
                    break;
                case EffectType::FlagWave: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double along = (dx * axis_x + dy * axis_y) / short_side;
                    const double flag = std::sin(kTau * effect.frequency * along - phase);
                    const double harmonic = std::sin(kTau * effect.frequency * 0.5 * along
                                                     - 2.0 * phase + 1.0472);
                    sample_x -= perpendicular_x * displacement
                                * (flag + effect.secondary * 0.35 * harmonic);
                    sample_y -= perpendicular_y * displacement
                                * (flag + effect.secondary * 0.35 * harmonic);
                    sampled = sample_bilinear(source, sample_x, sample_y, effect.edge_mode);
                    break;
                }
                case EffectType::Glow:
                    sampled = load_color(source, x, y);
                    break;
            }
            store_color(destination, x, y, sampled);
        }
    }
}

double bloom_weight(double luminance, double threshold, double soft_knee) {
    const double knee = std::max(1.0e-6, threshold * soft_knee);
    double soft = clamp_value(luminance - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1.0e-12);
    const double contribution = std::max(luminance - threshold, soft);
    return luminance > 1.0e-12 ? clamp_value(contribution / luminance, 0.0, 1.0) : 0.0;
}

void extract_bright(const Image& source, Image& bright,
                    double threshold, double soft_knee) {
    ensure_image(bright, source.width, source.height);
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const Color input = load_color(source, x, y);
            const double luminance = 0.2126 * input.r + 0.7152 * input.g + 0.0722 * input.b;
            const double weight = bloom_weight(luminance, threshold, soft_knee);
            store_color(bright, x, y,
                        {input.r, input.g, input.b, input.a * weight});
        }
    }
}

void blur_nine_tap(const Image& source, Image& destination,
                   double radius, bool horizontal) {
    ensure_image(destination, source.width, source.height);
    constexpr std::array<double, 9> weights = {
        0.02763055, 0.06628225, 0.12383154, 0.18017382, 0.20416369,
        0.18017382, 0.12383154, 0.06628225, 0.02763055};
    const double spacing = radius / 4.0;
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            Color accumulated;
            for (int tap = -4; tap <= 4; ++tap) {
                const long long offset = static_cast<long long>(
                    std::llround(static_cast<double>(tap) * spacing));
                const long long sample_x = horizontal
                                               ? static_cast<long long>(x) + offset
                                               : x;
                const long long sample_y = horizontal
                                               ? y
                                               : static_cast<long long>(y) + offset;
                const Color sample = sample_texel(source, sample_x, sample_y,
                                                  EdgeMode::Reflect);
                const double weight = weights[static_cast<std::size_t>(tap + 4)];
                accumulated.r += sample.r * sample.a * weight;
                accumulated.g += sample.g * sample.a * weight;
                accumulated.b += sample.b * sample.a * weight;
                accumulated.a += sample.a * weight;
            }
            if (accumulated.a > 1.0e-12) {
                accumulated.r /= accumulated.a;
                accumulated.g /= accumulated.a;
                accumulated.b /= accumulated.a;
            } else {
                accumulated.r = accumulated.g = accumulated.b = 0.0;
            }
            store_color(destination, x, y, accumulated);
        }
    }
}

void apply_glow(Image& image, Image& scratch, Image& auxiliary,
                const EffectConfig& effect, double phase) {
    const double pulse_depth = clamp_value(std::fabs(effect.secondary), 0.0, 1.0);
    const double pulse = 0.5 + 0.5 * std::sin(phase);
    const double animated_intensity = effect.intensity
                                      * mix_value(1.0, pulse, pulse_depth);
    if (animated_intensity <= 1.0e-12 || effect.radius_pixels <= 1.0e-12) {
        return;
    }
    extract_bright(image, scratch, effect.threshold, effect.soft_knee);
    blur_nine_tap(scratch, auxiliary, effect.radius_pixels, true);
    blur_nine_tap(auxiliary, scratch, effect.radius_pixels, false);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            Color original = load_color(image, x, y);
            const Color glow = load_color(scratch, x, y);
            // Build the additive halo in premultiplied space, then return to
            // the library's straight-alpha representation. This prevents a
            // partially transparent source from attenuating the halo twice.
            const double glow_coverage = clamp_value(
                animated_intensity * glow.a, 0.0, 1.0);
            const double output_alpha = original.a
                                        + glow_coverage * (1.0 - original.a);
            if (output_alpha > 1.0e-12) {
                original.r = (original.r * original.a
                              + animated_intensity * glow.r * glow.a)
                             / output_alpha;
                original.g = (original.g * original.a
                              + animated_intensity * glow.g * glow.a)
                             / output_alpha;
                original.b = (original.b * original.a
                              + animated_intensity * glow.b * glow.a)
                             / output_alpha;
            }
            original.a = output_alpha;
            store_color(image, x, y, original);
        }
    }
}

Vec3 add(Vec3 first, Vec3 second) {
    return {first.x + second.x, first.y + second.y, first.z + second.z};
}

Vec3 multiply(Vec3 value, double amount) {
    return {value.x * amount, value.y * amount, value.z * amount};
}

double dot(Vec3 first, Vec3 second) {
    return first.x * second.x + first.y * second.y + first.z * second.z;
}

Vec3 normalize(Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 1.0e-12 ? multiply(value, 1.0 / length) : Vec3{};
}

Vec3 rotate_x(Vec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {value.x, cosine * value.y - sine * value.z,
            sine * value.y + cosine * value.z};
}

Vec3 rotate_y(Vec3 value, double angle) {
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    return {cosine * value.x + sine * value.z, value.y,
            -sine * value.x + cosine * value.z};
}

Color shade_surface(Color color, Vec3 normal, double lighting) {
    const Vec3 light = normalize({-0.45, -0.55, 0.75});
    const double diffuse = std::max(0.0, dot(normalize(normal), light));
    const double lit = 0.28 + 0.72 * diffuse;
    const double multiplier = std::max(0.0, 1.0 + lighting * (lit - 1.0));
    color.r *= multiplier;
    color.g *= multiplier;
    color.b *= multiplier;
    return color;
}

bool intersect_cube(Vec3 origin, Vec3 direction, double& distance, Vec3& point,
                    Vec3& normal) {
    double near_distance = -std::numeric_limits<double>::infinity();
    double far_distance = std::numeric_limits<double>::infinity();
    const std::array<double, 3> origins = {origin.x, origin.y, origin.z};
    const std::array<double, 3> directions = {direction.x, direction.y, direction.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (std::fabs(directions[axis]) < 1.0e-12) {
            if (origins[axis] < -1.0 || origins[axis] > 1.0) {
                return false;
            }
            continue;
        }
        double first = (-1.0 - origins[axis]) / directions[axis];
        double second = (1.0 - origins[axis]) / directions[axis];
        if (first > second) {
            std::swap(first, second);
        }
        near_distance = std::max(near_distance, first);
        far_distance = std::min(far_distance, second);
        if (near_distance > far_distance) {
            return false;
        }
    }
    if (far_distance < 0.0) {
        return false;
    }
    distance = near_distance >= 0.0 ? near_distance : far_distance;
    point = add(origin, multiply(direction, distance));
    const double absolute_x = std::fabs(point.x);
    const double absolute_y = std::fabs(point.y);
    const double absolute_z = std::fabs(point.z);
    if (absolute_x >= absolute_y && absolute_x >= absolute_z) {
        normal = {point.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0};
    } else if (absolute_y >= absolute_x && absolute_y >= absolute_z) {
        normal = {0.0, point.y >= 0.0 ? 1.0 : -1.0, 0.0};
    } else {
        normal = {0.0, 0.0, point.z >= 0.0 ? 1.0 : -1.0};
    }
    return true;
}

std::pair<double, double> cube_uv(Vec3 point, Vec3 normal) {
    double u = 0.5;
    double v = 0.5;
    if (std::fabs(normal.x) > 0.5) {
        u = normal.x > 0.0 ? (1.0 - point.z) * 0.5 : (point.z + 1.0) * 0.5;
        v = (1.0 - point.y) * 0.5;
    } else if (std::fabs(normal.y) > 0.5) {
        u = (point.x + 1.0) * 0.5;
        v = normal.y > 0.0 ? (point.z + 1.0) * 0.5 : (1.0 - point.z) * 0.5;
    } else {
        u = normal.z > 0.0 ? (point.x + 1.0) * 0.5 : (1.0 - point.x) * 0.5;
        v = (1.0 - point.y) * 0.5;
    }
    return {clamp_value(u, 0.0, 1.0), clamp_value(v, 0.0, 1.0)};
}

void apply_surface_mapping(const Image& source, Image& destination,
                           const SurfaceConfig& surface, double loop_phase) {
    ensure_image(destination, source.width, source.height);
    const double phase = static_cast<double>(surface.rotations_per_loop) * loop_phase
                         + radians(surface.phase_degrees);
    const double curvature = clamp_value(surface.curvature, 0.0, 1.0);
    const double short_side = static_cast<double>(std::min(source.width, source.height));
    const double center_x = 0.5 * static_cast<double>(source.width - 1);
    const double center_y = 0.5 * static_cast<double>(source.height - 1);

    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const double screen_u = source.width > 1
                                      ? static_cast<double>(x) / (source.width - 1)
                                      : 0.5;
            const double screen_v = source.height > 1
                                      ? static_cast<double>(y) / (source.height - 1)
                                      : 0.5;
            Color output;
            bool visible = true;

            switch (surface.mapping) {
                case SurfaceMapping::Plane: {
                    const double dx = x - center_x;
                    const double dy = y - center_y;
                    const double cosine = std::cos(-phase);
                    const double sine = std::sin(-phase);
                    const double sample_x = center_x + cosine * dx - sine * dy;
                    const double sample_y = center_y + sine * dx + cosine * dy;
                    output = sample_bilinear(source, sample_x, sample_y,
                                             EdgeMode::Reflect);
                    break;
                }
                case SurfaceMapping::Cylinder: {
                    const double radius = 0.46 * short_side;
                    const double normalized_x = (x - center_x) / radius;
                    const double normalized_y = (y - center_y)
                                                / (0.46 * source.height);
                    if (std::fabs(normalized_x) > 1.0
                        || std::fabs(normalized_y) > 1.0) {
                        visible = false;
                        break;
                    }
                    const double longitude = std::asin(clamp_value(normalized_x, -1.0, 1.0));
                    const double wrapped_u = wrap_unit(
                        0.5 + longitude / kTau - phase / kTau);
                    const double surface_v = 0.5 + 0.5 * normalized_y;
                    Color wrapped = sample_bilinear_wrapped_x(
                        source, wrapped_u * source.width,
                        surface_v * (source.height - 1));
                    wrapped = shade_surface(
                        wrapped,
                        {normalized_x, 0.0,
                         std::sqrt(std::max(0.0,
                             1.0 - normalized_x * normalized_x))},
                        surface.lighting);
                    const Color planar = load_color(source, x, y);
                    output = blend_straight_alpha(planar, wrapped, curvature);
                    break;
                }
                case SurfaceMapping::Sphere: {
                    const double radius = 0.46 * short_side;
                    const double normalized_x = (x - center_x) / radius;
                    const double normalized_y = (center_y - y) / radius;
                    const double radius_squared = normalized_x * normalized_x
                                                  + normalized_y * normalized_y;
                    if (radius_squared > 1.0) {
                        visible = false;
                        break;
                    }
                    const double normalized_z = std::sqrt(std::max(0.0,
                        1.0 - radius_squared));
                    const Vec3 texture_normal = rotate_y(
                        {normalized_x, normalized_y, normalized_z}, -phase);
                    const double longitude = std::atan2(texture_normal.x,
                                                        texture_normal.z);
                    const double latitude = std::asin(
                        clamp_value(texture_normal.y, -1.0, 1.0));
                    const double wrapped_u = wrap_unit(0.5 + longitude / kTau);
                    const double sphere_v = 0.5 - latitude / kPi;
                    Color wrapped = sample_bilinear_wrapped_x(
                        source, wrapped_u * source.width,
                        sphere_v * (source.height - 1));
                    wrapped = shade_surface(
                        wrapped, {normalized_x, normalized_y, normalized_z},
                        surface.lighting);
                    const Color planar = load_color(source, x, y);
                    output = blend_straight_alpha(planar, wrapped, curvature);
                    break;
                }
                case SurfaceMapping::Cube: {
                    const double scale = 0.52 * short_side;
                    const double screen_x = (x - center_x) / scale;
                    const double screen_y = (center_y - y) / scale;
                    Vec3 origin = {0.0, 0.0, 3.4};
                    Vec3 direction = normalize({screen_x, screen_y, -2.5});
                    const double fixed_x_rotation = -0.35;
                    const double y_rotation = 0.55 + phase;
                    origin = rotate_x(rotate_y(origin, -y_rotation),
                                      -fixed_x_rotation);
                    direction = rotate_x(rotate_y(direction, -y_rotation),
                                         -fixed_x_rotation);
                    double distance = 0.0;
                    Vec3 point;
                    Vec3 normal;
                    if (!intersect_cube(origin, direction, distance, point, normal)) {
                        visible = false;
                        break;
                    }
                    const auto uv = cube_uv(point, normal);
                    const double mapped_u = mix_value(screen_u, uv.first, curvature);
                    const double mapped_v = mix_value(screen_v, uv.second, curvature);
                    output = sample_bilinear(source,
                                             mapped_u * (source.width - 1),
                                             mapped_v * (source.height - 1),
                                             EdgeMode::Reflect);
                    const Vec3 world_normal = rotate_y(
                        rotate_x(normal, fixed_x_rotation), y_rotation);
                    output = shade_surface(output, world_normal,
                                           surface.lighting * curvature);
                    break;
                }
            }

            if (!visible) {
                // Fade both color and coverage continuously from the planar
                // source to the primitive's transparent exterior. At curvature
                // one this retains the established fully transparent/black mask;
                // an infinitesimal curvature now remains infinitesimally close
                // to the planar image instead of abruptly cropping it.
                output = blend_straight_alpha(load_color(source, x, y), {}, curvature);
            }
            store_color(destination, x, y, output);
        }
    }
}

double quantize_value(double value, int levels) {
    const double maximum_index = static_cast<double>(levels - 1);
    return std::round(clamp_value(value, 0.0, 1.0) * maximum_index)
           / maximum_index;
}

std::array<double, 3> rgb_to_hsv(double red, double green, double blue) {
    const double maximum = std::max(red, std::max(green, blue));
    const double minimum = std::min(red, std::min(green, blue));
    const double delta = maximum - minimum;
    double hue = 0.0;
    if (delta > 1.0e-12) {
        if (maximum == red) {
            hue = std::fmod((green - blue) / delta, 6.0);
        } else if (maximum == green) {
            hue = (blue - red) / delta + 2.0;
        } else {
            hue = (red - green) / delta + 4.0;
        }
        hue /= 6.0;
        if (hue < 0.0) {
            hue += 1.0;
        }
    }
    const double saturation = maximum > 1.0e-12 ? delta / maximum : 0.0;
    return {hue, saturation, maximum};
}

std::array<double, 3> hsv_to_rgb(double hue, double saturation, double value) {
    hue = wrap_unit(hue);
    saturation = clamp_value(saturation, 0.0, 1.0);
    const double chroma = value * saturation;
    const double hue_sector = hue * 6.0;
    const double x = chroma * (1.0 - std::fabs(std::fmod(hue_sector, 2.0) - 1.0));
    const double match = value - chroma;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    if (hue_sector < 1.0) {
        red = chroma; green = x;
    } else if (hue_sector < 2.0) {
        red = x; green = chroma;
    } else if (hue_sector < 3.0) {
        green = chroma; blue = x;
    } else if (hue_sector < 4.0) {
        green = x; blue = chroma;
    } else if (hue_sector < 5.0) {
        red = x; blue = chroma;
    } else {
        red = chroma; blue = x;
    }
    return {red + match, green + match, blue + match};
}

void apply_quantization(Image& image, const QuantizationConfig& quantization) {
    if (!quantization.enabled || quantization.mix <= 0.0) {
        return;
    }
    const double amount = quantization.mix;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            Color color = load_color(image, x, y);
            Color quantized = color;
            switch (quantization.mode) {
                case QuantizationMode::Rgb:
                    quantized.r = quantize_value(color.r, quantization.levels);
                    quantized.g = quantize_value(color.g, quantization.levels);
                    quantized.b = quantize_value(color.b, quantization.levels);
                    break;
                case QuantizationMode::Luminance: {
                    const double luminance = 0.2126 * color.r
                                             + 0.7152 * color.g
                                             + 0.0722 * color.b;
                    const double quantized_luminance = quantize_value(
                        luminance, quantization.levels);
                    if (luminance > 1.0e-12) {
                        const double scale = quantized_luminance / luminance;
                        quantized.r *= scale;
                        quantized.g *= scale;
                        quantized.b *= scale;
                    } else {
                        quantized.r = quantized.g = quantized.b = 0.0;
                    }
                    break;
                }
                case QuantizationMode::Hue: {
                    const auto hsv = rgb_to_hsv(color.r, color.g, color.b);
                    const double quantized_hue = wrap_unit(
                        std::round(hsv[0] * quantization.levels)
                        / static_cast<double>(quantization.levels));
                    const auto rgb = hsv_to_rgb(quantized_hue, hsv[1], hsv[2]);
                    quantized.r = rgb[0];
                    quantized.g = rgb[1];
                    quantized.b = rgb[2];
                    break;
                }
            }
            color.r = mix_value(color.r, quantized.r, amount);
            color.g = mix_value(color.g, quantized.g, amount);
            color.b = mix_value(color.b, quantized.b, amount);
            store_color(image, x, y, color);
        }
    }
}

} // namespace

bool render_frame_at_phase(const RenderConfig& config, double normalized_phase,
                           Image& destination, std::string* error) {
    try {
        const ValidationResult validation = validate_impl(config, false);
        if (!validation.ok) {
            set_error(error, validation.message);
            return false;
        }
        if (!std::isfinite(normalized_phase)) {
            set_error(error, "Normalized render phase must be finite.");
            return false;
        }

        const double loop_phase = kTau * wrap_unit(normalized_phase);
        const double motion_phase = master_motion_phase(config, loop_phase);
        Image current;
        Image scratch;
        Image auxiliary;
        generate_base_image(config, loop_phase, motion_phase, current);

        for (const EffectConfig& effect : config.effects) {
            if (!effect_has_render_work(effect)) {
                continue;
            }
            const double phase = effect_phase(effect, loop_phase, motion_phase);
            if (effect.type == EffectType::Glow) {
                apply_glow(current, scratch, auxiliary, effect, phase);
            } else {
                apply_coordinate_effect(current, scratch, effect, phase);
                current.pixels.swap(scratch.pixels);
            }
        }

        // Curvature zero is the neutral setting for 3D primitive mappings. The
        // primitive's visibility mask and lighting must not crop or shade the
        // planar source in that state. Plane mapping remains active whenever its
        // configured phase or per-loop rotation can produce a 2D rotation.
        if (surface_has_render_work(config.surface)) {
            apply_surface_mapping(current, scratch, config.surface, loop_phase);
            current.pixels.swap(scratch.pixels);
        }
        apply_quantization(current, config.quantization);

        destination.width = current.width;
        destination.height = current.height;
        destination.pixels.swap(current.pixels);
        set_error(error, std::string{});
        return true;
    } catch (const std::bad_alloc&) {
        set_error(error, "The renderer could not allocate its validated image buffers.");
        return false;
    } catch (const std::exception& exception) {
        set_error(error, std::string("Rendering failed: ") + exception.what());
        return false;
    } catch (...) {
        set_error(error, "Rendering failed with an unknown exception.");
        return false;
    }
}

bool render_frame(const RenderConfig& config, int frame_index,
                  Image& destination, std::string* error) {
    if (config.total_frames <= 0) {
        set_error(error, "Frame count must be positive.");
        return false;
    }
    int wrapped_frame = frame_index % config.total_frames;
    if (wrapped_frame < 0) {
        wrapped_frame += config.total_frames;
    }
    return render_frame_at_phase(config,
                                 static_cast<double>(wrapped_frame)
                                     / static_cast<double>(config.total_frames),
                                 destination, error);
}

} // namespace pvt
