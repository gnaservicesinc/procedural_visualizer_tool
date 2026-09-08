#ifndef PVT_POST_PROCESS_ALPHA_H
#define PVT_POST_PROCESS_ALPHA_H

#include "procedural_visualizer_tool.h"
#include "effect_parameter_domain.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace pvt::detail {

// A conservative whole-frame fact used by RGB-export admission. Unknown means
// the alpha field may contain any mix of zero, one, and fractional coverage.
enum class AlphaCertainty {
    Zero,
    One,
    Unknown,
};

struct PostProcessMixRange {
    double minimum = 0.0;
    double maximum = 0.0;
};

inline PostProcessMixRange parameter_lfo_value_range(
    const RenderData& render, std::string_view target_path,
    double authored_value, double field_minimum, double field_maximum) {
    const auto bounded = [field_minimum, field_maximum](double value) {
        return std::isfinite(value)
            ? std::clamp(value, field_minimum, field_maximum)
            : field_maximum;
    };

    PostProcessMixRange result{
        bounded(authored_value), bounded(authored_value)};
    bool lfo_controls_target = false;
    for (const ParameterLfo& lfo : render.parameter_lfos) {
        const bool motion_scale_alias =
            (target_path == "motion.scale"
             || target_path == "motion.scale_pulse")
            && (lfo.target_path == "motion.scale"
                || lfo.target_path == "motion.scale_pulse");
        if (lfo.target_path != target_path && !motion_scale_alias) continue;
        const std::string setting_prefix =
            "lfo/" + std::to_string(lfo.id) + "/";
        bool enabled_controlled = false;
        bool range_controlled = false;
        bool rest_controlled = false;
        if (lfo.id != 0U) {
            for (const ParameterLfo& controller : render.parameter_lfos) {
                if (controller.target_path == setting_prefix + "enabled") {
                    enabled_controlled = true;
                } else if (controller.target_path
                               == setting_prefix + "minimum"
                           || controller.target_path
                               == setting_prefix + "maximum") {
                    range_controlled = true;
                } else if (controller.target_path
                               == setting_prefix + "delay_fraction"
                           || controller.target_path
                               == setting_prefix + "skip_cycles") {
                    rest_controlled = true;
                }
            }
        }
        if (!lfo.enabled && !enabled_controlled) continue;
        PostProcessMixRange candidate = range_controlled
            ? PostProcessMixRange{field_minimum, field_maximum}
            : PostProcessMixRange{
                  bounded(lfo.minimum), bounded(lfo.maximum)};
        if (lfo.delay_fraction > 0.0 || lfo.skip_cycles > 0
            || enabled_controlled || rest_controlled) {
            candidate.minimum = std::min(
                candidate.minimum, bounded(authored_value));
            candidate.maximum = std::max(
                candidate.maximum, bounded(authored_value));
        }
        if (!lfo_controls_target) {
            result = candidate;
            lfo_controls_target = true;
        } else {
            result.minimum = std::min(result.minimum, candidate.minimum);
            result.maximum = std::max(result.maximum, candidate.maximum);
        }
    }
    if (result.minimum > result.maximum) {
        std::swap(result.minimum, result.maximum);
    }
    return result;
}

inline PostProcessMixRange post_process_mix_range(
    const RenderData& render, std::string_view target_path,
    double authored_value) {
    return parameter_lfo_value_range(
        render, target_path, authored_value, 0.0, 1.0);
}

inline AlphaCertainty post_process_alpha_certainty(
    const RenderData& render, AlphaCertainty input) {
    AlphaCertainty alpha = input;
    if (render.post_process.effects_authoritative) {
        for (const PostProcessEffectConfig& effect :
             render.post_process.effects) {
            if (!effect.enabled) continue;
            const std::string target =
                "post_effect/" + std::to_string(effect.id) + "/mix";
            if (effect.stage == PostProcessStage::InvertAlpha) {
                const PostProcessMixRange mix = post_process_mix_range(
                    render, target, effect.mix);
                if (mix.maximum == 0.0) continue;
                if (mix.minimum == 1.0 && mix.maximum == 1.0) {
                    if (alpha == AlphaCertainty::Zero) {
                        alpha = AlphaCertainty::One;
                    } else if (alpha == AlphaCertainty::One) {
                        alpha = AlphaCertainty::Zero;
                    }
                } else {
                    alpha = AlphaCertainty::Unknown;
                }
            } else if (effect.stage == PostProcessStage::ChannelMap) {
                const PostProcessMixRange mix = post_process_mix_range(
                    render, target, effect.mix);
                if (mix.maximum == 0.0
                    || effect.alpha_source == ChannelSource::Alpha) {
                    continue;
                }
                AlphaCertainty routed = AlphaCertainty::Unknown;
                if (effect.alpha_source == ChannelSource::Zero) {
                    routed = AlphaCertainty::Zero;
                } else if (effect.alpha_source == ChannelSource::One) {
                    routed = AlphaCertainty::One;
                }
                if (mix.minimum == 1.0 && mix.maximum == 1.0) {
                    alpha = routed;
                } else if (alpha != routed
                           || alpha == AlphaCertainty::Unknown) {
                    alpha = AlphaCertainty::Unknown;
                }
            }
        }
        return alpha;
    }
    for (const PostProcessStage stage : render.post_process.order) {
        switch (stage) {
            case PostProcessStage::InvertAlpha: {
                if (!render.post_process.invert_alpha_enabled) break;
                const PostProcessMixRange mix = post_process_mix_range(
                    render, "post.invert_alpha_mix",
                    render.post_process.invert_alpha_mix);
                if (mix.maximum == 0.0) break;
                if (mix.minimum == 1.0 && mix.maximum == 1.0) {
                    if (alpha == AlphaCertainty::Zero) {
                        alpha = AlphaCertainty::One;
                    } else if (alpha == AlphaCertainty::One) {
                        alpha = AlphaCertainty::Zero;
                    }
                } else {
                    alpha = AlphaCertainty::Unknown;
                }
                break;
            }
            case PostProcessStage::ChannelMap: {
                if (!render.post_process.channel_map.enabled) break;
                const PostProcessMixRange mix = post_process_mix_range(
                    render, "post.channel_map_mix",
                    render.post_process.channel_map.mix);
                if (mix.maximum == 0.0) break;
                const ChannelSource source =
                    render.post_process.channel_map.alpha_source;
                if (source == ChannelSource::Alpha) break;

                AlphaCertainty routed = AlphaCertainty::Unknown;
                if (source == ChannelSource::Zero) {
                    routed = AlphaCertainty::Zero;
                } else if (source == ChannelSource::One) {
                    routed = AlphaCertainty::One;
                }
                if (mix.minimum == 1.0 && mix.maximum == 1.0) {
                    alpha = routed;
                } else if (alpha != routed
                           || alpha == AlphaCertainty::Unknown) {
                    alpha = AlphaCertainty::Unknown;
                }
                break;
            }
            case PostProcessStage::InvertRgb:
            case PostProcessStage::InvertRed:
            case PostProcessStage::InvertGreen:
            case PostProcessStage::InvertBlue:
            case PostProcessStage::Antialias:
            case PostProcessStage::Quantization:
                break;
        }
    }
    return alpha;
}

inline bool range_can_be_positive(const PostProcessMixRange& range) {
    return range.maximum > 1.0e-12;
}

inline bool range_can_differ_from(const PostProcessMixRange& range,
                                  double value) {
    return std::fabs(range.minimum - value) > 1.0e-12
           || std::fabs(range.maximum - value) > 1.0e-12;
}

inline bool range_can_be_nonperiodic(const PostProcessMixRange& range,
                                     double period) {
    return range.minimum != range.maximum
           || std::fmod(range.minimum, period) != 0.0;
}

inline bool integer_range_can_be_nonzero(const PostProcessMixRange& range) {
    const auto rounded = [](double value) {
        return std::llround(std::clamp(
            value,
            static_cast<double>((std::numeric_limits<int>::min)()),
            static_cast<double>((std::numeric_limits<int>::max)())));
    };
    return rounded(range.minimum) != 0 || rounded(range.maximum) != 0;
}

inline std::string effect_lfo_target(const EffectConfig& effect,
                                     std::string_view property) {
    return "effect/" + std::to_string(effect.id) + "/"
           + std::string(property);
}

inline bool effect_can_create_transparency(const RenderData& render,
                                           const EffectConfig& effect) {
    if (!effect.enabled
        || effective_effect_edge_mode(effect) != EdgeMode::Alpha) {
        return false;
    }
    if (effect.type == EffectType::Glow
        || effect.type == EffectType::BlockScale
        || effect.type == EffectType::ParticleField
        || effect.type == EffectType::EdgeDetect) {
        return false;
    }
    const PostProcessMixRange radius = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "radius"), effect.radius_pixels,
        0.0, maximum_render_parameter_magnitude());
    const PostProcessMixRange blur_maximum = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "blur_maximum"),
        effect.blur_maximum, 0.0, 1.0);
    if (effect.type == EffectType::Blur) {
        return range_can_be_positive(radius)
               && range_can_be_positive(blur_maximum);
    }
    const PostProcessMixRange intensity = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "intensity"), effect.intensity,
        0.0, maximum_render_parameter_magnitude());
    const PostProcessMixRange magnitude = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "magnitude"), effect.magnitude,
        effect_parameter_domain(effect.type, effect.magnitude).magnitude_minimum,
        maximum_render_parameter_magnitude());
    if (!range_can_be_positive(intensity)
        || !range_can_be_positive(magnitude)) {
        return false;
    }
    if (effect.type == EffectType::LensDistortion
        || effect.type == EffectType::Twirl) {
        const PostProcessMixRange secondary = parameter_lfo_value_range(
            render, effect_lfo_target(effect, "secondary"),
            effect.secondary, -1.0, 1.0);
        return secondary.minimum < 0.0 || secondary.maximum > 0.0;
    }
    return true;
}

inline bool effect_can_create_particle_coverage(
    const RenderData& render, const EffectConfig& effect) {
    if (!effect.enabled || effect.type != EffectType::ParticleField) {
        return false;
    }
    const PostProcessMixRange intensity = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "intensity"), effect.intensity,
        0.0, maximum_render_parameter_magnitude());
    const PostProcessMixRange frequency = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "frequency"), effect.frequency,
        1.0,
        static_cast<double>((std::numeric_limits<int>::max)()));
    const PostProcessMixRange radius = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "radius"), effect.radius_pixels,
        0.000001, maximum_render_parameter_magnitude());
    return range_can_be_positive(intensity) && frequency.maximum >= 1.0
           && range_can_be_positive(radius);
}

inline bool effect_can_create_coverage_from_transparent_input(
    const RenderData& render, const EffectConfig& effect) {
    if (!effect.enabled) return false;
    if (effect.type == EffectType::ParticleField) {
        return effect_can_create_particle_coverage(render, effect);
    }
    // Black and White are opaque exterior samples. Most resampling effects
    // can therefore turn an all-zero-alpha eraser source into coverage at the
    // canvas edge. Alpha/Reflect preserve zero, while these three effects do
    // not sample an independently opaque exterior.
    if ((effective_effect_edge_mode(effect) != EdgeMode::Black
         && effective_effect_edge_mode(effect) != EdgeMode::White)
        || effect.type == EffectType::Glow
        || effect.type == EffectType::BlockScale
        || effect.type == EffectType::EdgeDetect) {
        return false;
    }
    const PostProcessMixRange radius = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "radius"), effect.radius_pixels,
        0.0, maximum_render_parameter_magnitude());
    const PostProcessMixRange blur_maximum = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "blur_maximum"),
        effect.blur_maximum, 0.0, 1.0);
    if (effect.type == EffectType::Blur) {
        return range_can_be_positive(radius)
               && range_can_be_positive(blur_maximum);
    }
    const PostProcessMixRange intensity = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "intensity"), effect.intensity,
        0.0, maximum_render_parameter_magnitude());
    const PostProcessMixRange magnitude = parameter_lfo_value_range(
        render, effect_lfo_target(effect, "magnitude"), effect.magnitude,
        effect_parameter_domain(effect.type, effect.magnitude).magnitude_minimum,
        maximum_render_parameter_magnitude());
    if (!range_can_be_positive(intensity)
        || !range_can_be_positive(magnitude)) {
        return false;
    }
    if (effect.type == EffectType::LensDistortion
        || effect.type == EffectType::Twirl) {
        const PostProcessMixRange secondary = parameter_lfo_value_range(
            render, effect_lfo_target(effect, "secondary"),
            effect.secondary, -1.0, 1.0);
        return secondary.minimum < 0.0 || secondary.maximum > 0.0;
    }
    return true;
}

inline bool surface_can_create_transparency(const RenderData& render) {
    const SurfaceConfig& surface = render.surface;
    if (!surface.enabled || surface.outside != SurfaceOutside::Transparent) {
        return false;
    }
    const PostProcessMixRange curvature = parameter_lfo_value_range(
        render, "surface.curvature", surface.curvature, 0.0, 1.0);
    if (surface.mapping != SurfaceMapping::Plane) {
        return range_can_be_positive(curvature);
    }
    if (surface.plane_displacement.enabled
        && range_can_be_positive(curvature)) {
        return true;
    }
    const double maximum = maximum_render_parameter_magnitude();
    const auto range = [&](std::string_view path, double authored,
                           double minimum, double maximum_value) {
        return parameter_lfo_value_range(
            render, path, authored, minimum, maximum_value);
    };
    return surface.projection != SurfaceProjection::Orthographic
           || surface.sizing != SurfaceSizing::Contain
           || integer_range_can_be_nonzero(range(
               "surface.rotation_x_turns",
               static_cast<double>(surface.rotation_x_turns_per_loop),
               static_cast<double>((std::numeric_limits<int>::min)()),
               static_cast<double>((std::numeric_limits<int>::max)())))
           || integer_range_can_be_nonzero(range(
               "surface.rotation_y_turns",
               static_cast<double>(surface.rotation_y_turns_per_loop),
               static_cast<double>((std::numeric_limits<int>::min)()),
               static_cast<double>((std::numeric_limits<int>::max)())))
           || integer_range_can_be_nonzero(range(
               "surface.rotation_z_turns",
               static_cast<double>(surface.rotation_z_turns_per_loop),
               static_cast<double>((std::numeric_limits<int>::min)()),
               static_cast<double>((std::numeric_limits<int>::max)())))
           || range_can_be_nonperiodic(range(
               "surface.rotation_x", surface.rotation_x_degrees,
               -maximum, maximum), 360.0)
           || range_can_be_nonperiodic(range(
               "surface.rotation_y", surface.rotation_y_degrees,
               -maximum, maximum), 360.0)
           || range_can_be_nonperiodic(range(
               "surface.rotation_z", surface.rotation_z_degrees,
               -maximum, maximum), 360.0)
           || range_can_differ_from(range(
               "surface.size", surface.size_percent, 0.000001, maximum),
               100.0)
           || range_can_differ_from(range(
               "surface.scale_x", surface.scale_x, 0.000001, maximum), 1.0)
           || range_can_differ_from(range(
               "surface.scale_y", surface.scale_y, 0.000001, maximum), 1.0)
           || range_can_differ_from(range(
               "surface.scale_z", surface.scale_z, 0.000001, maximum), 1.0)
           || range_can_differ_from(range(
               "surface.position_x", surface.position_x_percent,
               -maximum, maximum), 0.0)
           || range_can_differ_from(range(
               "surface.position_y", surface.position_y_percent,
               -maximum, maximum), 0.0)
           || range_can_differ_from(range(
               "surface.position_z", surface.position_z,
               -maximum, maximum), 0.0);
}

inline bool motion_can_create_transparency(const RenderData& render) {
    const LayerMotionConfig& motion = render.motion;
    if (!motion.enabled) return false;
    if (motion.custom_path.enabled) return true;
    const double maximum = maximum_render_parameter_magnitude();
    const auto range = [&](std::string_view path, double authored,
                           double minimum, double maximum_value) {
        return parameter_lfo_value_range(
            render, path, authored, minimum, maximum_value);
    };
    const PostProcessMixRange travel_x = range(
        "motion.travel_x", motion.travel_x, 0.0, maximum);
    const PostProcessMixRange travel_y = range(
        "motion.travel_y", motion.travel_y, 0.0, maximum);
    const bool built_in_path_has_work =
        motion.path != LayerMotionPath::None
        && (range_can_be_positive(travel_x)
            || range_can_be_positive(travel_y));
    const PostProcessMixRange scale = range(
        "motion.scale", motion.scale_pulse, 0.0, maximum);
    const PostProcessMixRange cycles_y = range(
        "motion.cycles_y", static_cast<double>(motion.cycles_y),
        static_cast<double>((std::numeric_limits<int>::min)()),
        static_cast<double>((std::numeric_limits<int>::max)()));
    const PostProcessMixRange phase = range(
        "motion.phase", motion.phase_degrees, -maximum, maximum);
    return built_in_path_has_work
           || range_can_differ_from(range(
               "motion.center_x", motion.center_x, -maximum, maximum), 0.5)
           || range_can_differ_from(range(
               "motion.center_y", motion.center_y, -maximum, maximum), 0.5)
           || integer_range_can_be_nonzero(range(
               "motion.rotations",
               static_cast<double>(motion.rotations_per_loop),
               static_cast<double>((std::numeric_limits<int>::min)()),
               static_cast<double>((std::numeric_limits<int>::max)())))
           || range_can_be_nonperiodic(range(
               "motion.rotation_offset", motion.rotation_offset_degrees,
               -maximum, maximum), 360.0)
           || (range_can_be_positive(scale)
               && (integer_range_can_be_nonzero(cycles_y)
                   || range_can_be_nonperiodic(phase, 180.0)));
}

inline double reachable_alpha_minimum(const RenderData& render) {
    const PostProcessMixRange minimum = parameter_lfo_value_range(
        render, "alpha.minimum", render.alpha.minimum, 0.0, 1.0);
    const PostProcessMixRange maximum = parameter_lfo_value_range(
        render, "alpha.maximum", render.alpha.maximum, 0.0, 1.0);
    return std::min(minimum.minimum, maximum.minimum);
}

inline double reachable_alpha_maximum(const RenderData& render) {
    const PostProcessMixRange minimum = parameter_lfo_value_range(
        render, "alpha.minimum", render.alpha.minimum, 0.0, 1.0);
    const PostProcessMixRange maximum = parameter_lfo_value_range(
        render, "alpha.maximum", render.alpha.maximum, 0.0, 1.0);
    return std::max(minimum.maximum, maximum.maximum);
}

inline bool render_data_can_create_transparency(const RenderData& render) {
    if (render.alpha.use_source_alpha) {
        if (render.starting_image.enabled) return true;
        if (render.palette.enabled
            && std::any_of(
                render.palette.colors.begin(), render.palette.colors.end(),
                [](const PaletteColor& color) { return color.alpha < 1.0; })) {
            return true;
        }
    }
    if (!render.starting_image.enabled && !render.palette.enabled
        && render.starting_colors.include_alpha) {
        const PostProcessMixRange minimum = parameter_lfo_value_range(
            render, "starting.alpha_minimum",
            render.starting_colors.alpha_minimum, 0.0, 1.0);
        const PostProcessMixRange maximum = parameter_lfo_value_range(
            render, "starting.alpha_maximum",
            render.starting_colors.alpha_maximum, 0.0, 1.0);
        if (std::min(minimum.minimum, maximum.minimum) < 1.0) return true;
    }
    if (render.alpha.enabled && reachable_alpha_minimum(render) < 1.0) {
        return true;
    }
    if (surface_can_create_transparency(render)
        || motion_can_create_transparency(render)
        || std::any_of(
            render.effects.begin(), render.effects.end(),
            [&render](const EffectConfig& effect) {
                return effect_can_create_transparency(render, effect);
            })) {
        return true;
    }
    return post_process_alpha_certainty(render, AlphaCertainty::One)
           != AlphaCertainty::One;
}

inline bool eraser_source_is_guaranteed_transparent(
    const RenderData& render) {
    if (!render.alpha.enabled || reachable_alpha_maximum(render) != 0.0) {
        return false;
    }
    const bool effect_can_create_coverage = std::any_of(
        render.effects.begin(), render.effects.end(),
        [&render](const EffectConfig& effect) {
            return effect_can_create_coverage_from_transparent_input(
                render, effect);
        });
    return post_process_alpha_certainty(
               render, effect_can_create_coverage
                           ? AlphaCertainty::Unknown
                           : AlphaCertainty::Zero)
           == AlphaCertainty::Zero;
}

} // namespace pvt::detail

#endif
