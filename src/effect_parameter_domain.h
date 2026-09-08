#ifndef PVT_EFFECT_PARAMETER_DOMAIN_H
#define PVT_EFFECT_PARAMETER_DOMAIN_H

#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pvt::detail {

// Type-specific effect fields are edited through several independent paths:
// the ordinary editor, Live mappings, scenes, and render-time Numeric LFOs.
// Keep their structural domains in one place so a transient value cannot turn
// an otherwise valid project into a frame-time validation failure.
struct EffectParameterDomain {
    double intensity_maximum = maximum_render_parameter_magnitude();
    double magnitude_minimum = 0.0;
    double frequency_minimum = 0.0;
    double frequency_maximum = maximum_render_parameter_magnitude();
    double secondary_minimum = -maximum_render_parameter_magnitude();
    double secondary_maximum = maximum_render_parameter_magnitude();
    bool frequency_is_integer = false;
    bool secondary_is_integer = false;
};

inline EffectParameterDomain effect_parameter_domain(
    EffectType type, double magnitude) {
    constexpr double minimum_positive = 0.000001;
    const double render_maximum = maximum_render_parameter_magnitude();
    // Effect values also pass the shared finite-render-parameter gate, so a
    // whole-number field cannot actually use values above that bound even
    // when they still fit in int.
    const double integer_maximum = std::floor(std::min(
        render_maximum,
        static_cast<double>((std::numeric_limits<int>::max)())));
    EffectParameterDomain domain;
    switch (type) {
        case EffectType::BlockScale:
            domain.intensity_maximum = 1.0;
            domain.magnitude_minimum = minimum_positive;
            domain.frequency_minimum = std::max(
                minimum_positive, std::clamp(
                    magnitude, minimum_positive, render_maximum));
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = integer_maximum;
            domain.secondary_is_integer = true;
            break;
        case EffectType::ParticleField:
            domain.frequency_minimum = 1.0;
            domain.frequency_maximum = integer_maximum;
            domain.frequency_is_integer = true;
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::Glitch:
            domain.intensity_maximum = 1.0;
            domain.frequency_minimum = 1.0;
            domain.frequency_maximum = integer_maximum;
            domain.frequency_is_integer = true;
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::Starburst:
            domain.intensity_maximum = 1.0;
            domain.frequency_minimum = 1.0;
            domain.frequency_is_integer = true;
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::LensDistortion:
        case EffectType::Twirl:
            domain.intensity_maximum = 1.0;
            domain.frequency_minimum = 0.25;
            domain.secondary_minimum = -1.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::EdgeDetect:
            domain.intensity_maximum = 1.0;
            domain.frequency_minimum = 1.0;
            domain.frequency_maximum = integer_maximum;
            domain.frequency_is_integer = true;
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::Kaleidoscope:
            domain.intensity_maximum = 1.0;
            domain.magnitude_minimum = minimum_positive;
            domain.frequency_minimum = 1.0;
            domain.frequency_maximum = 256.0;
            domain.frequency_is_integer = true;
            domain.secondary_minimum = -1.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::Water:
            domain.intensity_maximum = 1.0;
            domain.secondary_minimum = 0.0;
            domain.secondary_maximum = 1.0;
            break;
        case EffectType::EndlessZoom:
        case EffectType::Ripple:
        case EffectType::Shake:
        case EffectType::FlagWave:
        case EffectType::Glow:
        case EffectType::Blur:
            break;
    }
    return domain;
}

inline double rounded_clamped(double value, double minimum,
                              double maximum, bool integer) {
    value = std::clamp(value, minimum, maximum);
    return integer ? std::round(value) : value;
}

inline void set_effect_intensity(EffectConfig& effect, double value) {
    const EffectParameterDomain domain = effect_parameter_domain(
        effect.type, effect.magnitude);
    effect.intensity = std::clamp(value, 0.0, domain.intensity_maximum);
}

inline void set_effect_magnitude(EffectConfig& effect, double value) {
    EffectParameterDomain domain = effect_parameter_domain(
        effect.type, effect.magnitude);
    effect.magnitude = std::clamp(
        value, domain.magnitude_minimum,
        maximum_render_parameter_magnitude());
    if (effect.type == EffectType::BlockScale) {
        effect.frequency = std::max(effect.frequency, effect.magnitude);
    }
}

inline void set_effect_frequency(EffectConfig& effect, double value) {
    const EffectParameterDomain domain = effect_parameter_domain(
        effect.type, effect.magnitude);
    effect.frequency = rounded_clamped(
        value, domain.frequency_minimum, domain.frequency_maximum,
        domain.frequency_is_integer);
}

inline void set_effect_secondary(EffectConfig& effect, double value) {
    const EffectParameterDomain domain = effect_parameter_domain(
        effect.type, effect.magnitude);
    effect.secondary = rounded_clamped(
        value, domain.secondary_minimum, domain.secondary_maximum,
        domain.secondary_is_integer);
}

inline void set_effect_radius(EffectConfig& effect, double value) {
    constexpr double minimum_positive = 0.000001;
    effect.radius_pixels = std::clamp(
        value,
        effect.type == EffectType::ParticleField ? minimum_positive : 0.0,
        maximum_render_parameter_magnitude());
}

inline void set_effect_threshold(EffectConfig& effect, double value) {
    effect.threshold = std::clamp(
        value, 0.0,
        effect.type == EffectType::ParticleField
            ? 1.0 : maximum_render_parameter_magnitude());
}

inline void repair_gaussian_blur_samples(EffectConfig& effect) {
    if (effect.type == EffectType::Blur
        && effect.blur_type == BlurType::Gaussian
        && effect.blur_samples % 2 == 0
        && effect.blur_samples < (std::numeric_limits<int>::max)()) {
        ++effect.blur_samples;
    }
}

inline void normalize_effect_parameter_domain(EffectConfig& effect) {
    set_effect_intensity(effect, effect.intensity);
    set_effect_magnitude(effect, effect.magnitude);
    set_effect_frequency(effect, effect.frequency);
    set_effect_secondary(effect, effect.secondary);
    set_effect_radius(effect, effect.radius_pixels);
    set_effect_threshold(effect, effect.threshold);
    repair_gaussian_blur_samples(effect);
}

} // namespace pvt::detail

#endif
