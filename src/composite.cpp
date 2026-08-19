#include "procedural_visualizer_tool.h"

#include "frame_renderer_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <locale>
#include <new>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pvt {
namespace {

constexpr std::size_t kMaximumProjectNameBytes = kMaximumUiItems;

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t& result) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

ValidationResult invalid_result(std::string message,
                                std::size_t estimated_peak_bytes = 0U) {
    ValidationResult result;
    result.message = std::move(message);
    result.estimated_peak_bytes = estimated_peak_bytes;
    return result;
}

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

bool valid_utf8_without_controls(const std::string& value, bool allow_tab) {
    for (std::size_t index = 0U; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::uint32_t code_point = 0U;
        std::size_t length = 0U;
        if (first <= 0x7fU) {
            code_point = first;
            length = 1U;
        } else if (first >= 0xc2U && first <= 0xdfU) {
            code_point = first & 0x1fU;
            length = 2U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code_point = first & 0x0fU;
            length = 3U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code_point = first & 0x07U;
            length = 4U;
        } else {
            return false;
        }
        if (index + length > value.size()) {
            return false;
        }
        for (std::size_t continuation_index = 1U;
             continuation_index < length; ++continuation_index) {
            const auto continuation =
                static_cast<unsigned char>(value[index + continuation_index]);
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3U && code_point < 0x800U)
            || (length == 4U && code_point < 0x10000U)
            || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)
            || (code_point < 0x20U
                && !(allow_tab && code_point == static_cast<std::uint32_t>('\t')))
            || (code_point >= 0x7fU && code_point <= 0x9fU)) {
            return false;
        }
        index += length;
    }
    return true;
}

bool valid_project_name(const std::string& value) {
    if (value.empty() || value.size() > kMaximumProjectNameBytes
        || !valid_utf8_without_controls(value, false)) {
        return false;
    }
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == '/' || character == '\\') {
            return false;
        }
    }
    return true;
}

bool valid_layer_name(const std::string& value) {
    if (value.size() > kMaximumProjectNameBytes
        || !valid_utf8_without_controls(value, true)) {
        return false;
    }
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0U || character == 0x7fU
            || (character < 0x20U && character != '\t')) {
            return false;
        }
    }
    return true;
}

bool valid_uuid(const std::string& value) {
    if (value.size() != 36U) {
        return false;
    }
    bool has_nonzero_digit = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const char character = value[index];
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f'))) {
            return false;
        }
        has_nonzero_digit = has_nonzero_digit || character != '0';
    }
    // Projects create RFC 4122 version-4 UUIDs. Keeping this strict prevents
    // superficially UUID-shaped random input from becoming persistent identity.
    const char variant = value[19U];
    return has_nonzero_digit && value[14U] == '4'
           && (variant == '8' || variant == '9' || variant == 'a'
               || variant == 'b');
}

template <typename Enum>
bool valid_live_enum(Enum) {
    return false;
}

template <>
bool valid_live_enum(LiveEndpointProtocol value) {
    switch (value) {
        case LiveEndpointProtocol::Audio:
        case LiveEndpointProtocol::Midi:
        case LiveEndpointProtocol::Osc:
        case LiveEndpointProtocol::FootController:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveEndpointDirection value) {
    switch (value) {
        case LiveEndpointDirection::Input:
        case LiveEndpointDirection::Output:
        case LiveEndpointDirection::Bidirectional:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveControlInput value) {
    switch (value) {
        case LiveControlInput::MidiControlChange:
        case LiveControlInput::MidiNote:
        case LiveControlInput::MidiProgramChange:
        case LiveControlInput::MidiPitchBend:
        case LiveControlInput::MidiChannelPressure:
        case LiveControlInput::OscValue:
        case LiveControlInput::Footswitch:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveMappingMode value) {
    switch (value) {
        case LiveMappingMode::Absolute:
        case LiveMappingMode::Relative:
        case LiveMappingMode::Toggle:
        case LiveMappingMode::Momentary:
        case LiveMappingMode::Trigger:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveMappingTarget value) {
    switch (value) {
        case LiveMappingTarget::Setting:
        case LiveMappingTarget::Action:
        case LiveMappingTarget::Scene:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveAction value) {
    switch (value) {
        case LiveAction::Freeze:
        case LiveAction::Blackout:
        case LiveAction::NextScene:
        case LiveAction::PreviousScene:
        case LiveAction::RestartScene:
        case LiveAction::TapTempo:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveClockTarget value) {
    switch (value) {
        case LiveClockTarget::Project:
        case LiveClockTarget::Layer:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveClockInputSource value) {
    switch (value) {
        case LiveClockInputSource::MidiClock:
        case LiveClockInputSource::AudioStream:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveSceneValueType value) {
    switch (value) {
        case LiveSceneValueType::Boolean:
        case LiveSceneValueType::Integer:
        case LiveSceneValueType::Real:
        case LiveSceneValueType::EnumToken:
        case LiveSceneValueType::String:
            return true;
    }
    return false;
}

template <>
bool valid_live_enum(LiveDropoutBehavior value) {
    switch (value) {
        case LiveDropoutBehavior::LastGoodFrame:
        case LiveDropoutBehavior::Blackout:
            return true;
    }
    return false;
}

bool live_direction_has_input(LiveEndpointDirection value) {
    return value == LiveEndpointDirection::Input
           || value == LiveEndpointDirection::Bidirectional;
}

bool live_direction_has_output(LiveEndpointDirection value) {
    return value == LiveEndpointDirection::Output
           || value == LiveEndpointDirection::Bidirectional;
}

bool valid_live_text(const std::string& value, bool allow_empty = false) {
    return (allow_empty || !value.empty())
           && value.size() <= kMaximumLiveTextBytes
           && valid_utf8_without_controls(value, false);
}

bool valid_osc_address(const std::string& value) {
    if (value.empty() || value.size() > kMaximumLiveTextBytes
        || value.front() != '/') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char raw) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        return byte >= 0x21U && byte <= 0x7eU;
    });
}

bool valid_scene_value(const LiveSceneValue& value) {
    if (!valid_live_enum(value.type)
        || value.value.size() > kMaximumLiveTextBytes
        || !valid_utf8_without_controls(value.value, false)) {
        return false;
    }
    switch (value.type) {
        case LiveSceneValueType::Boolean:
            return value.value == "0" || value.value == "1";
        case LiveSceneValueType::Integer: {
            if (value.value.empty()) return false;
            std::int64_t parsed = 0;
            const auto result = std::from_chars(
                value.value.data(), value.value.data() + value.value.size(),
                parsed, 10);
            return result.ec == std::errc{}
                   && result.ptr == value.value.data() + value.value.size();
        }
        case LiveSceneValueType::Real: {
            if (value.value.empty()) return false;
            std::istringstream stream(value.value);
            stream.imbue(std::locale::classic());
            stream >> std::noskipws;
            double parsed = 0.0;
            stream >> parsed;
            return stream && stream.peek() == std::char_traits<char>::eof()
                   && std::isfinite(parsed);
        }
        case LiveSceneValueType::EnumToken:
            return !value.value.empty()
                   && std::all_of(
                       value.value.begin(), value.value.end(), [](char raw) {
                           const unsigned char byte =
                               static_cast<unsigned char>(raw);
                           return (byte >= 'a' && byte <= 'z')
                                  || (byte >= '0' && byte <= '9')
                                  || byte == '_' || byte == '-'
                                  || byte == '.';
                       });
        case LiveSceneValueType::String:
            return true;
    }
    return false;
}

bool valid_blend_mode(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal:
        case BlendMode::SoftLight:
        case BlendMode::GrainMerge:
        case BlendMode::Overlay:
        case BlendMode::ColorDodge:
        case BlendMode::LinearBurn:
        case BlendMode::ColorBurn:
        case BlendMode::Difference:
        case BlendMode::Subtract:
        case BlendMode::Multiply:
        case BlendMode::Add:
        case BlendMode::Erase:
        case BlendMode::ColorEraseTones:
        case BlendMode::ColorEraseBrightness:
            return true;
    }
    return false;
}

bool valid_alpha_mode(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::AlphaOver:
        case AlphaMode::AlphaUnder:
            return true;
    }
    return false;
}

const LayerGroup* find_group(const ProjectConfig& project,
                             std::string_view uuid) {
    if (uuid.empty()) return nullptr;
    const auto found = std::find_if(
        project.groups.begin(), project.groups.end(),
        [uuid](const LayerGroup& group) { return group.uuid == uuid; });
    return found == project.groups.end() ? nullptr : &*found;
}

bool layer_effectively_enabled(const ProjectConfig& project,
                               const LayerConfig& layer) {
    if (!layer.enabled) return false;
    const LayerGroup* group = find_group(project, layer.group_uuid);
    return group == nullptr || group->enabled;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool effect_can_create_transparency(const EffectConfig& effect) {
    if (!effect.enabled
        || (effect.type == EffectType::Blur
                ? effect.radius_pixels <= 0.0
                  || effect.blur_maximum <= 0.0
                : effect.intensity <= 0.0)
        || effect.type == EffectType::Glow
        || effect.type == EffectType::BlockScale
        || effect.type == EffectType::ParticleField
        || effect.edge_mode != EdgeMode::Alpha) {
        return false;
    }
    if (effect.type == EffectType::Blur) return effect.radius_pixels > 0.0;
    return effect.magnitude > 0.0
           && (effect.type != EffectType::LensDistortion
               || effect.secondary != 0.0);
}

bool effect_can_create_particle_coverage(const EffectConfig& effect) {
    return effect.enabled && effect.type == EffectType::ParticleField
           && effect.intensity > 0.0 && effect.frequency >= 1.0
           && effect.radius_pixels > 0.0;
}

bool eraser_source_is_guaranteed_transparent(const RenderData& render) {
    if (!render.alpha.enabled || render.alpha.maximum != 0.0) return false;
    // Procedural alpha is applied before effects. Every effect and mapping
    // preserves an all-zero alpha field except Particle Field, which authors
    // new spark coverage independently of the source pixels.
    return std::none_of(render.effects.begin(), render.effects.end(),
                        effect_can_create_particle_coverage);
}

bool render_data_can_create_transparency(const RenderData& render) {
    if (render.alpha.use_source_alpha) {
        if (render.starting_image.enabled) {
            return true;
        }
        if (render.palette.enabled
            && std::any_of(
                render.palette.colors.begin(), render.palette.colors.end(),
                [](const PaletteColor& color) { return color.alpha < 1.0; })) {
            return true;
        }
    }
    // Generated alpha is authored by the adjacent include-alpha control and
    // is therefore independent of the palette/PNG source-alpha switch.
    if (!render.starting_image.enabled && !render.palette.enabled
        && render.starting_colors.include_alpha
        && render.starting_colors.alpha_minimum < 1.0) {
        return true;
    }
    if (render.alpha.enabled && render.alpha.minimum < 1.0) {
        return true;
    }
    if (render.surface.enabled
        && (render.surface.mapping != SurfaceMapping::Plane
            || render.surface.plane_displacement.enabled)
        && render.surface.curvature > 0.0) {
        return true;
    }
    const bool built_in_motion_path_has_work =
        render.motion.path != LayerMotionPath::None
        && (std::fabs(render.motion.travel_x) > 1.0e-12
            || std::fabs(render.motion.travel_y) > 1.0e-12);
    const bool motion_scale_has_work =
        render.motion.scale_pulse > 1.0e-12
        && (render.motion.cycles_y != 0
            || std::fmod(render.motion.phase_degrees, 180.0) != 0.0);
    if (render.motion.enabled
        && (built_in_motion_path_has_work
            || render.motion.custom_path.enabled
            || std::fabs(render.motion.center_x - 0.5) > 1.0e-12
            || std::fabs(render.motion.center_y - 0.5) > 1.0e-12
            || render.motion.rotations_per_loop != 0
            || std::fmod(render.motion.rotation_offset_degrees, 360.0) != 0.0
            || motion_scale_has_work)) {
        return true;
    }
    return std::any_of(render.effects.begin(), render.effects.end(),
                       effect_can_create_transparency);
}

bool validate_image(const Image& image, std::string_view label,
                    std::string* error) {
    if (image.width <= 0 || image.height <= 0) {
        return fail(error, std::string(label) + " dimensions must be positive.");
    }
    std::size_t pixels = 0U;
    std::size_t components = 0U;
    if (!checked_multiply(static_cast<std::size_t>(image.width),
                          static_cast<std::size_t>(image.height), pixels)
        || !checked_multiply(pixels, 4U, components)) {
        return fail(error, std::string(label)
                               + " dimensions overflow the pixel buffer size.");
    }
    if (image.pixels.size() != components) {
        return fail(error, std::string(label)
                               + " must contain exactly four components per pixel.");
    }
    for (std::size_t offset = 0U; offset < components; offset += 4U) {
        if (!std::isfinite(image.pixels[offset])
            || !std::isfinite(image.pixels[offset + 1U])
            || !std::isfinite(image.pixels[offset + 2U])) {
            return fail(error, std::string(label)
                                   + " contains a non-finite RGB component.");
        }
        const float alpha = image.pixels[offset + 3U];
        if (!std::isfinite(alpha) || alpha < 0.0F || alpha > 1.0F) {
            return fail(error, std::string(label)
                                   + " contains alpha outside finite [0, 1].");
        }
    }
    return true;
}

double clamp_unit(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double blend_channel(double backdrop, double source, BlendMode mode) {
    if (mode == BlendMode::Normal) {
        // Normal compositing preserves the renderer's linear HDR values.
        return source;
    }
    const double b = clamp_unit(backdrop);
    const double s = clamp_unit(source);
    switch (mode) {
        case BlendMode::Normal:
            return source;
        case BlendMode::SoftLight:
            if (s <= 0.5) {
                return b - (1.0 - 2.0 * s) * b * (1.0 - b);
            } else {
                const double curve = b <= 0.25
                                         ? ((16.0 * b - 12.0) * b + 4.0) * b
                                         : std::sqrt(b);
                return b + (2.0 * s - 1.0) * (curve - b);
            }
        case BlendMode::GrainMerge:
            return clamp_unit(b + s - 0.5);
        case BlendMode::Overlay:
            return b <= 0.5 ? 2.0 * b * s
                            : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
        case BlendMode::ColorDodge:
            if (b == 0.0) {
                return 0.0;
            }
            return s == 1.0 ? 1.0 : std::min(1.0, b / (1.0 - s));
        case BlendMode::LinearBurn:
            return std::max(0.0, b + s - 1.0);
        case BlendMode::ColorBurn:
            if (b == 1.0) {
                return 1.0;
            }
            return s == 0.0 ? 0.0
                            : 1.0 - std::min(1.0, (1.0 - b) / s);
        case BlendMode::Difference:
            return std::fabs(b - s);
        case BlendMode::Subtract:
            return std::max(0.0, b - s);
        case BlendMode::Multiply:
            return b * s;
        case BlendMode::Add:
            return std::min(1.0, b + s);
        case BlendMode::Erase:
        case BlendMode::ColorEraseTones:
        case BlendMode::ColorEraseBrightness:
            return backdrop;
    }
    return source;
}

float stored_channel(double value) {
    if (std::isnan(value)) {
        return 0.0F;
    }
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    return static_cast<float>(std::max(-maximum, std::min(maximum, value)));
}

bool composite_pixels(const Image& source, Image& destination,
                      BlendMode mode, double opacity,
                      const std::atomic_bool* cancel) {
    const std::size_t components = source.pixels.size();
    for (std::size_t offset = 0U; offset < components; offset += 4U) {
        if ((offset & 4095U) == 0U && cancelled(cancel)) {
            return false;
        }
        const double source_alpha =
            static_cast<double>(source.pixels[offset + 3U]) * opacity;
        if (source_alpha <= 0.0) {
            // Opacity zero and fully transparent source are exact no-ops,
            // including the backdrop's useful transparent RGB.
            continue;
        }
        const double backdrop_alpha =
            static_cast<double>(destination.pixels[offset + 3U]);
        if (mode == BlendMode::Erase
            || mode == BlendMode::ColorEraseTones
            || mode == BlendMode::ColorEraseBrightness) {
            if (backdrop_alpha <= 0.0) {
                continue;
            }
            double match = 1.0;
            if (mode == BlendMode::ColorEraseTones) {
                const double red = static_cast<double>(source.pixels[offset])
                                   - destination.pixels[offset];
                const double green = static_cast<double>(source.pixels[offset + 1U])
                                     - destination.pixels[offset + 1U];
                const double blue = static_cast<double>(source.pixels[offset + 2U])
                                    - destination.pixels[offset + 2U];
                const double distance = std::sqrt(
                    red * red + green * green + blue * blue);
                match = clamp_unit((0.30 - distance) / 0.22);
                match = match * match * (3.0 - 2.0 * match);
            } else if (mode == BlendMode::ColorEraseBrightness) {
                const double source_luma =
                    0.2126 * source.pixels[offset]
                    + 0.7152 * source.pixels[offset + 1U]
                    + 0.0722 * source.pixels[offset + 2U];
                const double backdrop_luma =
                    0.2126 * destination.pixels[offset]
                    + 0.7152 * destination.pixels[offset + 1U]
                    + 0.0722 * destination.pixels[offset + 2U];
                match = clamp_unit((source_luma - backdrop_luma) / 0.10);
                match = match * match * (3.0 - 2.0 * match);
            }
            const double erase = clamp_unit(source_alpha * match);
            destination.pixels[offset + 3U] = static_cast<float>(
                clamp_unit(backdrop_alpha * (1.0 - erase)));
            continue;
        }
        if (backdrop_alpha <= 0.0) {
            destination.pixels[offset] = source.pixels[offset];
            destination.pixels[offset + 1U] = source.pixels[offset + 1U];
            destination.pixels[offset + 2U] = source.pixels[offset + 2U];
            destination.pixels[offset + 3U] = static_cast<float>(source_alpha);
            continue;
        }

        const double output_alpha =
            source_alpha + backdrop_alpha * (1.0 - source_alpha);
        const double source_only = source_alpha * (1.0 - backdrop_alpha);
        const double overlap = source_alpha * backdrop_alpha;
        const double backdrop_only = (1.0 - source_alpha) * backdrop_alpha;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double source_value =
                static_cast<double>(source.pixels[offset + channel]);
            const double backdrop_value =
                static_cast<double>(destination.pixels[offset + channel]);
            const double blended = blend_channel(backdrop_value, source_value, mode);
            const double premultiplied = source_only * source_value
                                         + overlap * blended
                                         + backdrop_only * backdrop_value;
            destination.pixels[offset + channel] =
                stored_channel(premultiplied / output_alpha);
        }
        destination.pixels[offset + 3U] =
            static_cast<float>(clamp_unit(output_alpha));
    }
    return !cancelled(cancel);
}

bool composite_layer_pixels(Image& layer_image, Image& accumulator,
                            BlendMode mode, AlphaMode alpha_mode,
                            double opacity,
                            const std::atomic_bool* cancel) {
    // Erasers are explicitly destination-out operations on the accumulated
    // lower stack. Their meaning is independent of paint ordering, so retain
    // that contract even when the layer's ordinary alpha mode is Under.
    if (alpha_mode == AlphaMode::AlphaOver
        || mode == BlendMode::Erase
        || mode == BlendMode::ColorEraseTones
        || mode == BlendMode::ColorEraseBrightness) {
        return composite_pixels(layer_image, accumulator, mode, opacity,
                                cancel);
    }

    // Apply the selected layer's opacity before swapping the Porter-Duff
    // operands. The accumulated lower stack then paints over this layer using
    // the same artistic blend function, producing true destination-over for
    // Normal and deterministic under-order behavior for every other mode.
    for (std::size_t offset = 3U; offset < layer_image.pixels.size();
         offset += 4U) {
        if ((offset & 4095U) == 3U && cancelled(cancel)) return false;
        layer_image.pixels[offset] = static_cast<float>(
            static_cast<double>(layer_image.pixels[offset]) * opacity);
    }
    if (!composite_pixels(accumulator, layer_image, mode, 1.0, cancel)) {
        return false;
    }
    accumulator = std::move(layer_image);
    return true;
}

struct LayerTimelineSelection {
    int frame = 0;
};

double wrap_phase(double value) {
    value -= std::floor(value);
    return value < 0.0 ? value + 1.0 : value;
}

LayerTimelineSelection select_layer_timeline(
    const ProjectConfig& project, const LayerConfig& layer,
    double normalized_phase, const int* synchronized_frame) {
    (void)layer;
    std::string ignored;
    const int master_count = std::max(
        1, effective_frame_count(project.canvas, &ignored));
    int master_frame = synchronized_frame == nullptr
        ? static_cast<int>(std::floor(
              wrap_phase(normalized_phase) * static_cast<double>(master_count)))
        : *synchronized_frame;
    master_frame %= master_count;
    if (master_frame < 0) master_frame += master_count;
    // Active-layer clocks are evaluated from this authoritative project frame
    // inside the renderer. LayerClockScale maps only the local source time;
    // it never substitutes a different project duration or frame index.
    return {master_frame};
}

RenderConfig materialize_project_layer(const ProjectConfig& project,
                                       const ExportConfig& layer_output,
                                       std::size_t index) {
    return apply_global_config(
        project.canvas, layer_output, project.layers[index].render);
}

bool render_project_at_phase_validated(const ProjectConfig& project,
                                       double normalized_phase,
                                       const int* synchronized_frame,
                                       Image& destination,
                                       const std::atomic_bool* cancel,
                                       std::string* error) {
    std::size_t pixel_count = 0U;
    std::size_t component_count = 0U;
    if (!checked_multiply(static_cast<std::size_t>(project.canvas.width),
                          static_cast<std::size_t>(project.canvas.height), pixel_count)
        || !checked_multiply(pixel_count, 4U, component_count)) {
        return fail(error, "Project canvas dimensions overflow the pixel buffer size.");
    }

    Image accumulator;
    accumulator.width = project.canvas.width;
    accumulator.height = project.canvas.height;
    accumulator.pixels.assign(component_count, 0.0F);

    // Every layer is rendered into an in-memory RGBA image. Final channel
    // selection belongs to the completed project export, not to an isolated
    // layer: a transparent upper layer can still produce a guaranteed-opaque
    // RGB composite when an opaque layer covers the canvas beneath it.
    ExportConfig layer_output = project.output;
    layer_output.write_alpha = true;

    for (std::size_t index = 0U; index < project.layers.size(); ++index) {
        const LayerConfig& layer = project.layers[index];
        if (!layer_effectively_enabled(project, layer)
            || layer.opacity <= 0.0) {
            continue;
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled between layers.");
        }

        const LayerTimelineSelection timeline = select_layer_timeline(
            project, layer, normalized_phase, synchronized_frame);
        const RenderConfig render = materialize_project_layer(
            project, layer_output, index);
        Image layer_image;
        std::string layer_error;
        const bool layer_owns_clock = layer.render.layer_clock.enabled;
        const bool rendered = synchronized_frame == nullptr && !layer_owns_clock
            ? render_frame_at_phase_cancellable(render, normalized_phase,
                                                layer_image, cancel,
                                                &layer_error)
            : render_frame_cancellable(render, timeline.frame,
                                       layer_image, cancel, &layer_error);
        if (!rendered) {
            if (cancelled(cancel)) {
                return fail(error, "Project rendering was cancelled while rendering layer "
                                       + std::to_string(index + 1U) + ".");
            }
            return fail(error, "Could not render layer " + std::to_string(index + 1U)
                                   + " ('" + layer.name + "'): " + layer_error);
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled between layers.");
        }
        // Both images and the layer settings were validated before allocation;
        // per-pixel compositing cannot fail or allocate.
        if (!composite_layer_pixels(layer_image, accumulator,
                                    layer.blend_mode, layer.alpha_mode,
                                    layer.opacity, cancel)) {
            return fail(error, "Project rendering was cancelled while compositing layer "
                                   + std::to_string(index + 1U) + ".");
        }
    }

    destination.width = accumulator.width;
    destination.height = accumulator.height;
    destination.pixels.swap(accumulator.pixels);
    return true;
}

bool render_project_with_backend_validated(
    const ProjectConfig& project,
    double normalized_phase,
    const int* synchronized_frame,
    const FrameRenderOptions& options,
    Image& destination,
    const std::atomic_bool* cancel,
    std::string* error) {
    if (options.backend == RenderBackend::Cpu) {
        return render_project_at_phase_validated(
            project, normalized_phase, synchronized_frame,
            destination, cancel, error);
    }

    std::size_t pixel_count = 0U;
    std::size_t component_count = 0U;
    if (!checked_multiply(static_cast<std::size_t>(project.canvas.width),
                          static_cast<std::size_t>(project.canvas.height),
                          pixel_count)
        || !checked_multiply(pixel_count, 4U, component_count)) {
        return fail(error,
                    "Project canvas dimensions overflow the pixel buffer size.");
    }
    Image accumulator;
    accumulator.width = project.canvas.width;
    accumulator.height = project.canvas.height;
    accumulator.pixels.assign(component_count, 0.0F);

    ExportConfig layer_output = project.output;
    layer_output.write_alpha = true;
    std::vector<std::size_t> contributing;
    contributing.reserve(project.layers.size());
    for (std::size_t index = 0U; index < project.layers.size(); ++index) {
        if (layer_effectively_enabled(project, project.layers[index])
            && project.layers[index].opacity > 0.0) {
            contributing.push_back(index);
        }
    }

    const auto materialize = [&](std::size_t index) {
        return materialize_project_layer(project, layer_output, index);
    };
    const auto render_one = [&](std::size_t index,
                                const FrameRenderOptions& selected,
                                Image& image,
                                std::string& layer_error) {
        const LayerTimelineSelection timeline = select_layer_timeline(
            project, project.layers[index], normalized_phase,
            synchronized_frame);
        const RenderConfig render = materialize(index);
        const bool layer_owns_clock =
            project.layers[index].render.layer_clock.enabled;
        return synchronized_frame == nullptr && !layer_owns_clock
            ? render_frame_at_phase(render, normalized_phase, selected,
                                    image, cancel, &layer_error)
            : render_frame(render, timeline.frame, selected,
                           image, cancel, &layer_error);
    };
    const auto composite_one = [&](std::size_t index, Image& image) {
        if (cancelled(cancel)) {
            return fail(error,
                        "Project rendering was cancelled between layers.");
        }
        const LayerConfig& layer = project.layers[index];
        if (!composite_layer_pixels(image, accumulator,
                                    layer.blend_mode, layer.alpha_mode,
                                    layer.opacity, cancel)) {
            return fail(error,
                        "Project rendering was cancelled while compositing layer "
                            + std::to_string(index + 1U) + ".");
        }
        return true;
    };
    const auto contextual_failure = [&](std::size_t index,
                                         const std::string& layer_error) {
        if (cancelled(cancel)) {
            return fail(error,
                        "Project rendering was cancelled while rendering layer "
                            + std::to_string(index + 1U) + ".");
        }
        return fail(error,
                    "Could not render layer " + std::to_string(index + 1U)
                        + " ('" + project.layers[index].name + "'): "
                        + layer_error);
    };

    if (options.backend == RenderBackend::Gpu) {
        for (const std::size_t index : contributing) {
            Image image;
            std::string layer_error;
            if (!render_one(index, options, image, layer_error)) {
                return contextual_failure(index, layer_error);
            }
            if (!composite_one(index, image)) return false;
        }
        destination = std::move(accumulator);
        return true;
    }

    // Hybrid preview rendering uses one reference CPU lane beside one Metal
    // lane. Results are retained only for the current pair and composited in
    // paint order, so parallelism cannot reorder alpha/blend semantics or grow
    // memory with the layer count.
    std::string metal_device;
    std::string metal_status;
    const bool metal_available = detail::metal_backend_available(
        &metal_device, &metal_status);

    // The portable OpenGL backend accelerates the analytic surface stage
    // inside each reference render rather than occupying an independent
    // whole-layer lane. Preserve the requested CPU + GPU policy so every
    // eligible Windows/Linux layer reaches that stage.
    if (!metal_available) {
        for (const std::size_t index : contributing) {
            Image image;
            std::string layer_error;
            if (!render_one(index, options, image, layer_error)) {
                return contextual_failure(index, layer_error);
            }
            if (!composite_one(index, image)) return false;
        }
        destination = std::move(accumulator);
        return true;
    }
    FrameRenderOptions cpu_options = options;
    cpu_options.backend = RenderBackend::Cpu;
    FrameRenderOptions gpu_options = options;
    gpu_options.backend = RenderBackend::Gpu;

    for (std::size_t position = 0U; position < contributing.size();) {
        const std::size_t first_index = contributing[position];
        if (position + 1U >= contributing.size()) {
            Image image;
            std::string layer_error;
            FrameRenderOptions selected = options;
            if (!render_one(first_index, selected, image, layer_error)) {
                return contextual_failure(first_index, layer_error);
            }
            if (!composite_one(first_index, image)) return false;
            ++position;
            continue;
        }

        const std::size_t second_index = contributing[position + 1U];
        const RenderConfig first_render = materialize(first_index);
        const RenderConfig second_render = materialize(second_index);
        std::string ignored_reason;
        const bool first_gpu = metal_available
                               && detail::metal_backend_supports(
                                      first_render, &ignored_reason);
        const bool second_gpu = metal_available
                                && detail::metal_backend_supports(
                                       second_render, &ignored_reason);
        if (!first_gpu && !second_gpu) {
            for (const std::size_t index : {first_index, second_index}) {
                Image image;
                std::string layer_error;
                if (!render_one(index, cpu_options, image, layer_error)) {
                    return contextual_failure(index, layer_error);
                }
                if (!composite_one(index, image)) return false;
            }
            position += 2U;
            continue;
        }

        // Prefer the second layer for Metal when either is supported. This
        // lets the CPU start the bottom layer immediately while the GPU works
        // independently, then preserves bottom-to-top compositing below.
        const bool gpu_is_second = second_gpu;
        const std::size_t cpu_index = gpu_is_second ? first_index : second_index;
        const std::size_t gpu_index = gpu_is_second ? second_index : first_index;
        const ValidationResult cpu_validation = validate(materialize(cpu_index));
        const ValidationResult gpu_validation = validate(materialize(gpu_index));
        std::size_t concurrent_peak = 0U;
        const bool bounded_pair = cpu_validation.ok && gpu_validation.ok
            && checked_add(cpu_validation.estimated_peak_bytes,
                           gpu_validation.estimated_peak_bytes,
                           concurrent_peak);
        if (!bounded_pair) {
            // Memory safety outranks concurrency. Still use Metal for supported
            // work, but complete and release one layer before starting the next.
            for (const std::size_t index : {first_index, second_index}) {
                Image image;
                std::string layer_error;
                const bool supported = index == first_index ? first_gpu : second_gpu;
                const FrameRenderOptions& selected = supported
                                                         ? gpu_options
                                                         : cpu_options;
                if (!render_one(index, selected, image, layer_error)) {
                    return contextual_failure(index, layer_error);
                }
                if (!composite_one(index, image)) return false;
            }
            position += 2U;
            continue;
        }

        Image cpu_image;
        Image gpu_image;
        std::string cpu_error;
        std::string gpu_error;
        bool cpu_ok = false;
        std::thread cpu_worker([&] {
            cpu_ok = render_one(cpu_index, cpu_options, cpu_image, cpu_error);
        });
        const bool gpu_ok = render_one(gpu_index, gpu_options,
                                       gpu_image, gpu_error);
        cpu_worker.join();
        if (!cpu_ok) return contextual_failure(cpu_index, cpu_error);
        if (!gpu_ok) {
            return contextual_failure(gpu_index, gpu_error);
        }
        Image& first_image = first_index == cpu_index
                                 ? cpu_image : gpu_image;
        Image& second_image = second_index == cpu_index
                                  ? cpu_image : gpu_image;
        if (!composite_one(first_index, first_image)
            || !composite_one(second_index, second_image)) {
            return false;
        }
        position += 2U;
    }

    destination = std::move(accumulator);
    return true;
}

std::uint64_t mix_entropy(std::uint64_t& state) {
    state += UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t value = state;
    value = (value ^ (value >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

} // namespace

std::string generate_uuid() {
    static std::atomic<std::uint64_t> counter {0U};
    const auto system_ticks = static_cast<std::uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const auto steady_ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t state = system_ticks ^ (steady_ticks << 1U)
                          ^ counter.fetch_add(1U, std::memory_order_relaxed);
    try {
        std::random_device random;
        for (int index = 0; index < 4; ++index) {
            state ^= static_cast<std::uint64_t>(random())
                     << static_cast<unsigned int>(index * 16);
            (void)mix_entropy(state);
        }
    } catch (...) {
        // The independent clocks plus process-local counter remain a robust
        // uniqueness fallback on systems whose random_device is unavailable.
    }

    std::array<unsigned char, 16U> bytes{};
    const std::uint64_t first = mix_entropy(state);
    const std::uint64_t second = mix_entropy(state);
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[index] = static_cast<unsigned char>(first >> (index * 8U));
        bytes[index + 8U] = static_cast<unsigned char>(second >> (index * 8U));
    }
    bytes[6U] = static_cast<unsigned char>((bytes[6U] & 0x0fU) | 0x40U);
    bytes[8U] = static_cast<unsigned char>((bytes[8U] & 0x3fU) | 0x80U);

    constexpr char hexadecimal[] = "0123456789abcdef";
    std::string result;
    result.reserve(36U);
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (index == 4U || index == 6U || index == 8U || index == 10U) {
            result.push_back('-');
        }
        result.push_back(hexadecimal[bytes[index] >> 4U]);
        result.push_back(hexadecimal[bytes[index] & 0x0fU]);
    }
    return result;
}

ValidationResult validate(const LiveConfig& live) {
    try {
        if (live.endpoints.size() > kMaximumLiveEndpoints
            || live.mappings.size() > kMaximumLiveMappings
            || live.clock_inputs.size() > kMaximumLiveClockInputs
            || live.midi_clock_outputs.size() > kMaximumLiveClockOutputs
            || live.scenes.size() > kMaximumLiveScenes) {
            return invalid_result(
                "A Live collection exceeds its deterministic real-time work limit.");
        }

        std::unordered_map<std::string, const LiveEndpointConfig*> endpoints;
        endpoints.reserve(live.endpoints.size());
        for (std::size_t index = 0U; index < live.endpoints.size(); ++index) {
            const LiveEndpointConfig& endpoint = live.endpoints[index];
            if (!valid_uuid(endpoint.uuid)
                || !valid_live_text(endpoint.name)
                || !valid_live_enum(endpoint.protocol)
                || !valid_live_enum(endpoint.direction)) {
                return invalid_result(
                    "Live endpoint " + std::to_string(index + 1U)
                    + " has an invalid UUID, name, protocol, direction, or latency calibration.");
            }
            if (!endpoints.emplace(endpoint.uuid, &endpoint).second) {
                return invalid_result(
                    "Every Live endpoint must have a unique UUID.");
            }
        }

        std::unordered_map<std::string, const LiveSceneConfig*> scenes;
        scenes.reserve(live.scenes.size());
        std::size_t total_scene_values = 0U;
        for (std::size_t index = 0U; index < live.scenes.size(); ++index) {
            const LiveSceneConfig& scene = live.scenes[index];
            if (!valid_uuid(scene.uuid) || !valid_live_text(scene.name)
                || scene.transition_milliseconds < 0) {
                return invalid_result(
                    "Live scene " + std::to_string(index + 1U)
                    + " has an invalid UUID, name, or transition duration.");
            }
            if (!scenes.emplace(scene.uuid, &scene).second) {
                return invalid_result("Every Live scene must have a unique UUID.");
            }
            if (scene.values.size()
                    > kMaximumLiveSceneValues - total_scene_values) {
                return invalid_result(
                    "The total Live scene-value count exceeds its deterministic switching limit.");
            }
            total_scene_values += scene.values.size();
            std::unordered_set<std::string> targets;
            targets.reserve(scene.values.size());
            for (std::size_t value_index = 0U;
                 value_index < scene.values.size(); ++value_index) {
                const LiveSceneValue& value = scene.values[value_index];
                if (!valid_live_text(value.target_path)
                    || !valid_scene_value(value)) {
                    return invalid_result(
                        "Live scene " + std::to_string(index + 1U)
                        + " contains an invalid target or typed value at position "
                        + std::to_string(value_index + 1U) + ".");
                }
                if (!targets.insert(value.target_path).second) {
                    return invalid_result(
                        "A Live scene cannot assign the same setting target more than once.");
                }
            }
        }
        if (!live.startup_scene_uuid.empty()
            && (!valid_uuid(live.startup_scene_uuid)
                || scenes.find(live.startup_scene_uuid) == scenes.end())) {
            return invalid_result(
                "The Live startup scene must reference a saved scene UUID.");
        }

        for (std::size_t index = 0U; index < live.mappings.size(); ++index) {
            const LiveControlMapping& mapping = live.mappings[index];
            if (!valid_live_text(mapping.name)
                || !valid_live_enum(mapping.input)
                || !valid_live_enum(mapping.target)
                || !valid_live_enum(mapping.action)
                || !valid_live_enum(mapping.mode)
                || mapping.midi_channel < 0 || mapping.midi_channel > 16
                || mapping.control_number < 0
                || mapping.control_number > 127
                || !std::isfinite(mapping.input_minimum)
                || !std::isfinite(mapping.input_maximum)
                || mapping.input_minimum >= mapping.input_maximum
                || !std::isfinite(mapping.output_minimum)
                || !std::isfinite(mapping.output_maximum)
                || !std::isfinite(mapping.curve)
                || mapping.curve <= 0.0
                || !std::isfinite(mapping.dead_zone)
                || mapping.dead_zone < 0.0 || mapping.dead_zone >= 1.0
                || mapping.smoothing_milliseconds < 0) {
                return invalid_result(
                    "Live mapping " + std::to_string(index + 1U)
                    + " has an invalid source, target, transform, or smoothing value.");
            }

            const bool midi =
                mapping.input == LiveControlInput::MidiControlChange
                || mapping.input == LiveControlInput::MidiNote
                || mapping.input == LiveControlInput::MidiProgramChange
                || mapping.input == LiveControlInput::MidiPitchBend
                || mapping.input == LiveControlInput::MidiChannelPressure;
            const bool numbered_midi =
                mapping.input == LiveControlInput::MidiControlChange
                || mapping.input == LiveControlInput::MidiNote
                || mapping.input == LiveControlInput::MidiProgramChange;
            if ((midi && !mapping.osc_address.empty())
                || (!midi && mapping.input != LiveControlInput::OscValue
                    && mapping.midi_channel != 0)
                || (!numbered_midi
                    && mapping.input != LiveControlInput::Footswitch
                    && mapping.control_number != 0)
                || (mapping.input == LiveControlInput::OscValue
                    && (mapping.midi_channel != 0
                        || mapping.control_number != 0
                        || !valid_osc_address(mapping.osc_address)))
                || (mapping.input != LiveControlInput::OscValue
                    && !mapping.osc_address.empty())) {
                return invalid_result(
                    "A Live mapping contains source fields that do not match its input type.");
            }

            if (mapping.target == LiveMappingTarget::Setting) {
                if (!valid_live_text(mapping.target_path)
                    || !mapping.scene_uuid.empty()) {
                    return invalid_result(
                        "A Live setting mapping requires one portable target path.");
                }
            } else if (mapping.target == LiveMappingTarget::Action) {
                if (!mapping.target_path.empty() || !mapping.scene_uuid.empty()) {
                    return invalid_result(
                        "A Live action mapping cannot also contain a setting or scene target.");
                }
            } else {
                if (!mapping.target_path.empty()
                    || !valid_uuid(mapping.scene_uuid)
                    || (mapping.enabled
                        && scenes.find(mapping.scene_uuid) == scenes.end())) {
                    return invalid_result(
                        "An enabled Live scene mapping must reference a saved scene UUID.");
                }
            }

            if (!mapping.endpoint_uuid.empty()
                && !valid_uuid(mapping.endpoint_uuid)) {
                return invalid_result(
                    "A Live mapping endpoint reference is not a canonical UUID.");
            }
            if (mapping.enabled) {
                const auto found = endpoints.find(mapping.endpoint_uuid);
                if (found == endpoints.end()
                    || !live_direction_has_input(found->second->direction)
                    || (midi
                        && found->second->protocol
                               != LiveEndpointProtocol::Midi)
                    || (mapping.input == LiveControlInput::OscValue
                        && found->second->protocol
                               != LiveEndpointProtocol::Osc)
                    || (mapping.input == LiveControlInput::Footswitch
                        && found->second->protocol
                               != LiveEndpointProtocol::FootController)) {
                    return invalid_result(
                        "An enabled Live mapping requires a compatible logical input endpoint.");
                }
            }
        }

        std::unordered_set<std::string> active_clock_targets;
        for (std::size_t index = 0U; index < live.clock_inputs.size(); ++index) {
            const LiveClockInputConfig& clock = live.clock_inputs[index];
            if (!valid_live_enum(clock.target)
                || !valid_live_enum(clock.source)
                || clock.audio_channel < 0
                || clock.holdover_milliseconds < 0
                || (clock.target == LiveClockTarget::Project
                    && !clock.layer_uuid.empty())
                || (clock.target == LiveClockTarget::Layer
                    && !valid_uuid(clock.layer_uuid))
                || (clock.source == LiveClockInputSource::MidiClock
                    && clock.audio_channel != 0)
                || (!clock.endpoint_uuid.empty()
                    && !valid_uuid(clock.endpoint_uuid))) {
                return invalid_result(
                    "Live clock input " + std::to_string(index + 1U)
                    + " has an invalid target, source, channel, endpoint, or holdover.");
            }
            if (!clock.enabled) continue;
            const std::string target_key =
                clock.target == LiveClockTarget::Project
                    ? std::string("project")
                    : std::string("layer:") + clock.layer_uuid;
            if (!active_clock_targets.insert(target_key).second) {
                return invalid_result(
                    "Only one enabled Live input may drive each project or layer clock.");
            }
            const auto endpoint = endpoints.find(clock.endpoint_uuid);
            const LiveEndpointProtocol required =
                clock.source == LiveClockInputSource::MidiClock
                    ? LiveEndpointProtocol::Midi
                    : LiveEndpointProtocol::Audio;
            if (endpoint == endpoints.end()
                || endpoint->second->protocol != required
                || !live_direction_has_input(endpoint->second->direction)) {
                return invalid_result(
                    "An enabled Live clock input requires a compatible logical input endpoint.");
            }
        }

        std::unordered_set<std::string> active_clock_outputs;
        for (std::size_t index = 0U;
             index < live.midi_clock_outputs.size(); ++index) {
            const LiveMidiClockOutputConfig& output =
                live.midi_clock_outputs[index];
            if (!valid_live_enum(output.source)
                || (output.source == LiveClockTarget::Project
                    && !output.layer_uuid.empty())
                || (output.source == LiveClockTarget::Layer
                    && !valid_uuid(output.layer_uuid))
                || (!output.endpoint_uuid.empty()
                    && !valid_uuid(output.endpoint_uuid))) {
                return invalid_result(
                    "Live MIDI clock output " + std::to_string(index + 1U)
                    + " has an invalid clock source or endpoint reference.");
            }
            if (!output.enabled) continue;
            const auto endpoint = endpoints.find(output.endpoint_uuid);
            if (endpoint == endpoints.end()
                || endpoint->second->protocol != LiveEndpointProtocol::Midi
                || !live_direction_has_output(endpoint->second->direction)) {
                return invalid_result(
                    "An enabled Live MIDI clock output requires a logical MIDI output endpoint.");
            }
            // MIDI Clock is a system real-time stream with no channel or
            // source identifier. Interleaving project and layer ticks on one
            // port would be undecodable, so each logical endpoint carries at
            // most one enabled clock. Use separate output roles/ports when a
            // rig needs several clocks.
            if (!active_clock_outputs.insert(output.endpoint_uuid).second) {
                return invalid_result(
                    "A logical MIDI endpoint can carry only one enabled Live clock output.");
            }
        }

        if (!valid_live_enum(live.safety.dropout_behavior)
            || live.safety.watchdog_timeout_milliseconds < 1
            || live.safety.audio_dropout_grace_milliseconds < 0
            || live.safety.last_good_frame_timeout_milliseconds < 0) {
            return invalid_result(
                "Live watchdog or dropout-safety preferences are outside their supported range.");
        }

        ValidationResult result;
        result.ok = true;
        result.message = "Live configuration is valid.";
        return result;
    } catch (const std::bad_alloc&) {
        return invalid_result("Live configuration validation ran out of memory.");
    } catch (const std::exception& exception) {
        return invalid_result("Live configuration validation failed: "
                              + std::string(exception.what()));
    } catch (...) {
        return invalid_result(
            "Live configuration validation failed with an unknown error.");
    }
}

ValidationResult validate(const ProjectConfig& project) {
    try {
        if (!valid_uuid(project.uuid)) {
            return invalid_result(
                "Project UUID must be a canonical lower-case RFC 4122 version-4 UUID.");
        }
        if (!valid_project_name(project.name)) {
            return invalid_result(
                "Project name must be nonempty, valid UTF-8 without control characters, and fit the signed-int text API.");
        }
        if (project.layers.empty()) {
            return invalid_result("A project must contain at least one layer.");
        }
        if (project.layers.size() > kMaximumLayers) {
            return invalid_result("The layer count exceeds the signed-int UI/API limit.");
        }
        if (project.groups.size() > kMaximumLayerGroups) {
            return invalid_result("The layer-group count exceeds the signed-int UI/API limit.");
        }
        // Structural RenderData validation must not let a disabled transparent
        // layer dictate final export channels. Enable alpha only in the
        // temporary validation adapter, then enforce the enabled stack below.
        ExportConfig structural_output = project.output;
        structural_output.write_alpha = true;
        const RenderConfig defaults = default_config();
        const RenderConfig global_probe = apply_global_config(
            project.canvas, structural_output,
            static_cast<const RenderData&>(defaults));
        const ValidationResult global_validation = validate(global_probe);
        if (!global_validation.ok) {
            return invalid_result("Project output is invalid: "
                                  + global_validation.message,
                                  global_validation.estimated_peak_bytes);
        }

        std::unordered_set<std::string> uuids;
        std::unordered_set<std::uint64_t> file_ids;
        uuids.reserve(project.layers.size() + project.groups.size() + 1U);
        uuids.insert(project.uuid);
        std::unordered_map<std::string, std::size_t> group_members;
        group_members.reserve(project.groups.size());
        for (std::size_t index = 0U; index < project.groups.size(); ++index) {
            const LayerGroup& group = project.groups[index];
            if (!valid_uuid(group.uuid)) {
                return invalid_result(
                    "Layer group " + std::to_string(index + 1U)
                    + " UUID must be a canonical lower-case RFC 4122 version-4 UUID.");
            }
            if (!uuids.insert(group.uuid).second) {
                return invalid_result(
                    "Project, layer, and group UUIDs must all be unique.");
            }
            if (group.name.empty() || !valid_layer_name(group.name)) {
                return invalid_result("Layer group "
                                      + std::to_string(index + 1U)
                                      + " has an invalid name.");
            }
            group_members.emplace(group.uuid, 0U);
        }
        file_ids.reserve(project.layers.size());
        std::size_t worst_layer_peak = 0U;
        bool has_contributing_layer = false;
        bool enabled_stack_is_guaranteed_opaque = false;
        std::string master_count_error;
        const int master_frame_count = effective_frame_count(
            project.canvas, &master_count_error);
        if (master_frame_count < 1) {
            return invalid_result(master_count_error.empty()
                                      ? "The project clock has no renderable duration."
                                      : master_count_error);
        }
        std::unordered_set<std::string> closed_groups;
        std::string open_group;
        for (std::size_t index = 0U; index < project.layers.size(); ++index) {
            const LayerConfig& layer = project.layers[index];
            if (!valid_uuid(layer.uuid)) {
                return invalid_result(
                    "Layer " + std::to_string(index + 1U)
                    + " UUID must be a canonical lower-case RFC 4122 version-4 UUID.");
            }
            if (!uuids.insert(layer.uuid).second) {
                return invalid_result(
                    "Project, layer, and group UUIDs must all be unique.");
            }
            if (!file_ids.insert(layer.file_id).second) {
                return invalid_result(
                    "Every layer file ID must be unique within a project.");
            }
            if (!valid_layer_name(layer.name)) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " has an invalid name.");
            }
            if (!valid_blend_mode(layer.blend_mode)) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " has an invalid blend mode.");
            }
            if (!valid_alpha_mode(layer.alpha_mode)) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " has an invalid alpha mode.");
            }
            if (layer.group_uuid != open_group) {
                if (!open_group.empty()) closed_groups.insert(open_group);
                if (!layer.group_uuid.empty()
                    && closed_groups.find(layer.group_uuid)
                           != closed_groups.end()) {
                    return invalid_result(
                        "Every layer group must occupy one contiguous paint-order block.");
                }
                open_group = layer.group_uuid;
            }
            if (!layer.group_uuid.empty()) {
                const auto group = group_members.find(layer.group_uuid);
                if (group == group_members.end()) {
                    return invalid_result(
                        "Layer " + std::to_string(index + 1U)
                        + " references a missing layer group.");
                }
                ++group->second;
            }
            if (!std::isfinite(layer.opacity) || layer.opacity < 0.0
                || layer.opacity > 1.0) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " opacity must be finite and between 0 and 1.");
            }

            RenderConfig render =
                apply_global_config(project.canvas, structural_output, layer.render);
            // The global probe above already validated the same project-wide
            // Live block once. It never affects offline layer pixels, so avoid
            // multiplying its bounded routing/scene scan by the layer count.
            render.live = {};
            const ValidationResult layer_validation = validate(render);
            if (!layer_validation.ok) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " is invalid: " + layer_validation.message,
                                      layer_validation.estimated_peak_bytes);
            }
            if (layer_effectively_enabled(project, layer)
                && layer.opacity > 0.0) {
                const bool erases_lower_layers =
                    layer.blend_mode == BlendMode::Erase
                    || layer.blend_mode == BlendMode::ColorEraseTones
                    || layer.blend_mode == BlendMode::ColorEraseBrightness;
                if (erases_lower_layers) {
                    // A destination-out layer can punch coverage back out of
                    // an opaque lower stack. Procedural alpha fixed at zero is
                    // an exact no-op unless Particle Field synthesizes coverage.
                    // A later ordinary opaque layer may establish coverage again.
                    if (!eraser_source_is_guaranteed_transparent(render)) {
                        enabled_stack_is_guaranteed_opaque = false;
                    }
                } else {
                    has_contributing_layer = true;
                }
                // Once an enabled layer is guaranteed to cover every pixel at
                // alpha 1, ordinary source-over alpha remains 1 for every
                // layer above it, even when those upper layers are partially
                // transparent. This permits a safely opaque composite to be
                // exported as RGB without merely guessing from the presence
                // of alpha-capable render data elsewhere in the stack.
                if (!erases_lower_layers) {
                    enabled_stack_is_guaranteed_opaque =
                        enabled_stack_is_guaranteed_opaque
                        || (layer.opacity == 1.0
                            && !render_data_can_create_transparency(layer.render));
                }
                worst_layer_peak =
                    std::max(worst_layer_peak, layer_validation.estimated_peak_bytes);
            }
        }

        for (const auto& group : group_members) {
            if (group.second == 0U) {
                return invalid_result(
                    "Every layer group must contain at least one layer.");
            }
        }

        const auto live_layer_exists = [&project](const std::string& uuid) {
            return std::any_of(
                project.layers.begin(), project.layers.end(),
                [&uuid](const LayerConfig& layer) {
                    return layer.uuid == uuid;
                });
        };
        for (const LiveClockInputConfig& input :
             project.canvas.live.clock_inputs) {
            if (input.enabled && input.target == LiveClockTarget::Layer
                && !live_layer_exists(input.layer_uuid)) {
                return invalid_result(
                    "An enabled Live clock input references a layer that is not in this project.");
            }
        }
        for (const LiveMidiClockOutputConfig& output :
             project.canvas.live.midi_clock_outputs) {
            if (output.enabled && output.source == LiveClockTarget::Layer
                && !live_layer_exists(output.layer_uuid)) {
                return invalid_result(
                    "An enabled Live MIDI clock output references a layer that is not in this project.");
            }
        }

        if (!project.output.write_alpha
            && (!has_contributing_layer || !enabled_stack_is_guaranteed_opaque)) {
            return invalid_result(
                "Alpha output must be enabled when the enabled layer stack can be transparent.");
        }

        std::size_t pixel_count = 0U;
        std::size_t component_count = 0U;
        std::size_t frame_bytes = 0U;
        std::size_t project_buffers = 0U;
        std::size_t peak_bytes = 0U;
        if (!checked_multiply(static_cast<std::size_t>(project.canvas.width),
                              static_cast<std::size_t>(project.canvas.height), pixel_count)
            || !checked_multiply(pixel_count, 4U, component_count)
            || !checked_multiply(component_count, sizeof(float), frame_bytes)
            || !checked_multiply(frame_bytes, 2U, project_buffers)
            || !checked_add(worst_layer_peak, project_buffers, peak_bytes)) {
            return invalid_result("The project peak memory estimate overflowed.");
        }
        ValidationResult result;
        result.ok = true;
        result.message = "Project configuration is valid.";
        result.estimated_peak_bytes = peak_bytes;
        return result;
    } catch (const std::bad_alloc&) {
        return invalid_result("Project validation ran out of memory.");
    } catch (const std::exception& exception) {
        return invalid_result("Project validation failed: "
                              + std::string(exception.what()));
    } catch (...) {
        return invalid_result("Project validation failed with an unknown error.");
    }
}

bool composite_over(const Image& source, Image& destination,
                    BlendMode mode, double opacity, std::string* error) {
    clear_error(error);
    try {
        if (!valid_blend_mode(mode)) {
            return fail(error, "The selected layer blend mode is invalid.");
        }
        if (!std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0) {
            return fail(error, "Layer opacity must be finite and between 0 and 1.");
        }
        if (!validate_image(source, "Source image", error)
            || !validate_image(destination, "Backdrop image", error)) {
            return false;
        }
        if (source.width != destination.width
            || source.height != destination.height) {
            return fail(error, "Source and backdrop image dimensions must match.");
        }

        // Copy first so malformed input, allocation failure, and source/dest
        // aliasing all retain transactional behavior.
        Image candidate = destination;
        (void)composite_pixels(source, candidate, mode, opacity, nullptr);
        destination.pixels.swap(candidate.pixels);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "Image compositing ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Image compositing failed: "
                           + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Image compositing failed with an unknown error.");
    }
}

bool render_project_frame_at_phase(const ProjectConfig& project,
                                   double normalized_phase,
                                   Image& destination,
                                   const std::atomic_bool* cancel,
                                   std::string* error) {
    clear_error(error);
    try {
        const ValidationResult validation = validate(project);
        if (!validation.ok) {
            return fail(error, validation.message);
        }
        if (!std::isfinite(normalized_phase)) {
            return fail(error, "Normalized project render phase must be finite.");
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled.");
        }
        return render_project_at_phase_validated(project, normalized_phase,
                                                 nullptr,
                                                 destination, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Project rendering ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Project rendering failed: "
                           + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Project rendering failed with an unknown error.");
    }
}

bool render_project_frame(const ProjectConfig& project, int frame_index,
                          Image& destination, const std::atomic_bool* cancel,
                          std::string* error) {
    clear_error(error);
    try {
        const ValidationResult validation = validate(project);
        if (!validation.ok) {
            return fail(error, validation.message);
        }
        std::string frame_count_error;
        const int frame_count = effective_frame_count(project.canvas,
                                                      &frame_count_error);
        if (frame_count < 1) {
            return fail(error, frame_count_error.empty()
                                   ? "Project clock has no renderable frames."
                                   : frame_count_error);
        }
        int wrapped_frame = frame_index % frame_count;
        if (wrapped_frame < 0) {
            wrapped_frame += frame_count;
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled.");
        }
        return render_project_at_phase_validated(
            project,
            static_cast<double>(wrapped_frame)
                / static_cast<double>(frame_count),
            &wrapped_frame,
            destination, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Project rendering ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Project rendering failed: "
                           + std::string(exception.what()));
    } catch (...) {
        return fail(error, "Project rendering failed with an unknown error.");
    }
}

bool render_project_frame_at_phase(
    const ProjectConfig& project,
    double normalized_phase,
    const FrameRenderOptions& options,
    Image& destination,
    const std::atomic_bool* cancel,
    std::string* error) {
    clear_error(error);
    try {
        const ValidationResult validation = validate(project);
        if (!validation.ok) return fail(error, validation.message);
        if (!std::isfinite(normalized_phase)) {
            return fail(error,
                        "Normalized project render phase must be finite.");
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled.");
        }
        return render_project_with_backend_validated(
            project, normalized_phase, nullptr, options,
            destination, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Project rendering ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Project rendering failed: "
                               + std::string(exception.what()));
    } catch (...) {
        return fail(error,
                    "Project rendering failed with an unknown error.");
    }
}

bool render_project_frame(const ProjectConfig& project, int frame_index,
                          const FrameRenderOptions& options,
                          Image& destination,
                          const std::atomic_bool* cancel,
                          std::string* error) {
    clear_error(error);
    try {
        const ValidationResult validation = validate(project);
        if (!validation.ok) return fail(error, validation.message);
        std::string frame_count_error;
        const int frame_count = effective_frame_count(project.canvas,
                                                      &frame_count_error);
        if (frame_count < 1) {
            return fail(error, frame_count_error.empty()
                                   ? "Project clock has no renderable frames."
                                   : frame_count_error);
        }
        int wrapped_frame = frame_index % frame_count;
        if (wrapped_frame < 0) wrapped_frame += frame_count;
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled.");
        }
        return render_project_with_backend_validated(
            project,
            static_cast<double>(wrapped_frame)
                / static_cast<double>(frame_count),
            &wrapped_frame, options, destination, cancel, error);
    } catch (const std::bad_alloc&) {
        return fail(error, "Project rendering ran out of memory.");
    } catch (const std::exception& exception) {
        return fail(error, "Project rendering failed: "
                               + std::string(exception.what()));
    } catch (...) {
        return fail(error,
                    "Project rendering failed with an unknown error.");
    }
}

const char* blend_mode_name(BlendMode value) {
    switch (value) {
        case BlendMode::Normal: return "Normal";
        case BlendMode::SoftLight: return "Soft light";
        case BlendMode::GrainMerge: return "Grain merge";
        case BlendMode::Overlay: return "Overlay";
        case BlendMode::ColorDodge: return "Color dodge";
        case BlendMode::LinearBurn: return "Linear burn";
        case BlendMode::ColorBurn: return "Color burn";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Subtract: return "Subtract";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::Add: return "Add";
        case BlendMode::Erase: return "Erase";
        case BlendMode::ColorEraseTones: return "Color eraser (tones)";
        case BlendMode::ColorEraseBrightness: return "Color eraser (brightness)";
    }
    return "Unknown blend mode";
}

const char* alpha_mode_name(AlphaMode value) {
    switch (value) {
        case AlphaMode::AlphaOver: return "Alpha Over";
        case AlphaMode::AlphaUnder: return "Alpha Under";
    }
    return "Unknown alpha mode";
}

const char* live_endpoint_protocol_name(LiveEndpointProtocol value) {
    switch (value) {
        case LiveEndpointProtocol::Audio: return "Audio stream";
        case LiveEndpointProtocol::Midi: return "MIDI";
        case LiveEndpointProtocol::Osc: return "OSC";
        case LiveEndpointProtocol::FootController: return "Foot controller";
    }
    return "Unknown";
}

const char* live_endpoint_direction_name(LiveEndpointDirection value) {
    switch (value) {
        case LiveEndpointDirection::Input: return "Input";
        case LiveEndpointDirection::Output: return "Output";
        case LiveEndpointDirection::Bidirectional: return "Input and output";
    }
    return "Unknown";
}

const char* live_control_input_name(LiveControlInput value) {
    switch (value) {
        case LiveControlInput::MidiControlChange: return "MIDI CC";
        case LiveControlInput::MidiNote: return "MIDI note";
        case LiveControlInput::MidiProgramChange: return "MIDI program";
        case LiveControlInput::MidiPitchBend: return "MIDI pitch bend";
        case LiveControlInput::MidiChannelPressure:
            return "MIDI channel pressure";
        case LiveControlInput::OscValue: return "OSC value";
        case LiveControlInput::Footswitch: return "Footswitch";
    }
    return "Unknown";
}

const char* live_mapping_mode_name(LiveMappingMode value) {
    switch (value) {
        case LiveMappingMode::Absolute: return "Absolute";
        case LiveMappingMode::Relative: return "Relative";
        case LiveMappingMode::Toggle: return "Toggle";
        case LiveMappingMode::Momentary: return "Momentary";
        case LiveMappingMode::Trigger: return "Trigger";
    }
    return "Unknown";
}

const char* live_mapping_target_name(LiveMappingTarget value) {
    switch (value) {
        case LiveMappingTarget::Setting: return "Setting";
        case LiveMappingTarget::Action: return "Live action";
        case LiveMappingTarget::Scene: return "Scene";
    }
    return "Unknown";
}

const char* live_action_name(LiveAction value) {
    switch (value) {
        case LiveAction::Freeze: return "Freeze";
        case LiveAction::Blackout: return "Blackout";
        case LiveAction::NextScene: return "Next scene";
        case LiveAction::PreviousScene: return "Previous scene";
        case LiveAction::RestartScene: return "Restart scene";
        case LiveAction::TapTempo: return "Tap tempo";
    }
    return "Unknown";
}

const char* live_clock_target_name(LiveClockTarget value) {
    switch (value) {
        case LiveClockTarget::Project: return "Project clock";
        case LiveClockTarget::Layer: return "Layer clock";
    }
    return "Unknown";
}

const char* live_clock_input_source_name(LiveClockInputSource value) {
    switch (value) {
        case LiveClockInputSource::MidiClock: return "MIDI clock";
        case LiveClockInputSource::AudioStream: return "Audio stream";
    }
    return "Unknown";
}

const char* live_scene_value_type_name(LiveSceneValueType value) {
    switch (value) {
        case LiveSceneValueType::Boolean: return "Boolean";
        case LiveSceneValueType::Integer: return "Integer";
        case LiveSceneValueType::Real: return "Number";
        case LiveSceneValueType::EnumToken: return "Choice";
        case LiveSceneValueType::String: return "Text";
    }
    return "Unknown";
}

const char* live_dropout_behavior_name(LiveDropoutBehavior value) {
    switch (value) {
        case LiveDropoutBehavior::LastGoodFrame: return "Last good frame";
        case LiveDropoutBehavior::Blackout: return "Blackout";
    }
    return "Unknown";
}

} // namespace pvt
