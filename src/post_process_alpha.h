#ifndef PVT_POST_PROCESS_ALPHA_H
#define PVT_POST_PROCESS_ALPHA_H

#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <cmath>
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

inline PostProcessMixRange post_process_mix_range(
    const RenderData& render, std::string_view target_path,
    double authored_value) {
    const auto normalized = [](double value) {
        return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 1.0;
    };

    PostProcessMixRange result{
        normalized(authored_value), normalized(authored_value)};
    bool lfo_controls_target = false;
    for (const ParameterLfo& lfo : render.parameter_lfos) {
        if (!lfo.enabled || lfo.target_path != target_path) continue;
        const PostProcessMixRange candidate{
            normalized(lfo.minimum), normalized(lfo.maximum)};
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

} // namespace pvt::detail

#endif
