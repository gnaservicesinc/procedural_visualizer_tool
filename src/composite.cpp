#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace pvt {
namespace {

constexpr std::size_t kMaximumProjectPeakBytes = std::size_t{1} << 30;
constexpr std::size_t kMaximumProjectNameBytes = 256U;

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
            return true;
    }
    return false;
}

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool effect_can_create_transparency(const EffectConfig& effect) {
    if (!effect.enabled || effect.intensity <= 0.0
        || effect.type == EffectType::Glow
        || effect.type == EffectType::BlockScale
        || effect.edge_mode != EdgeMode::Alpha) {
        return false;
    }
    return effect.magnitude > 0.0;
}

bool render_data_can_create_transparency(const RenderData& render) {
    if (render.alpha.enabled && render.alpha.minimum < 1.0) {
        return true;
    }
    if (render.surface.enabled
        && render.surface.mapping != SurfaceMapping::Plane
        && render.surface.curvature > 0.0) {
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
        if (!layer.enabled || layer.opacity <= 0.0) {
            continue;
        }
        if (cancelled(cancel)) {
            return fail(error, "Project rendering was cancelled between layers.");
        }

        const RenderConfig render =
            apply_global_config(project.canvas, layer_output, layer.render);
        Image layer_image;
        std::string layer_error;
        const bool rendered = synchronized_frame == nullptr
            ? render_frame_at_phase_cancellable(render, normalized_phase,
                                                layer_image, cancel,
                                                &layer_error)
            : render_frame_cancellable(render, *synchronized_frame,
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
        if (!composite_pixels(layer_image, accumulator, layer.blend_mode,
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

ValidationResult validate(const ProjectConfig& project) {
    try {
        if (!valid_uuid(project.uuid)) {
            return invalid_result(
                "Project UUID must be a canonical lower-case RFC 4122 version-4 UUID.");
        }
        if (!valid_project_name(project.name)) {
            return invalid_result(
                "Project name must contain 1 to 256 bytes without control characters.");
        }
        if (project.layers.empty()) {
            return invalid_result("A project must contain at least one layer.");
        }
        if (project.layers.size() > kMaximumLayers) {
            return invalid_result("The project contains more than 64 layers.");
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
        uuids.reserve(project.layers.size());
        file_ids.reserve(project.layers.size());
        std::size_t worst_layer_peak = 0U;
        bool has_contributing_layer = false;
        bool enabled_stack_is_guaranteed_opaque = false;
        for (std::size_t index = 0U; index < project.layers.size(); ++index) {
            const LayerConfig& layer = project.layers[index];
            if (!valid_uuid(layer.uuid)) {
                return invalid_result(
                    "Layer " + std::to_string(index + 1U)
                    + " UUID must be a canonical lower-case RFC 4122 version-4 UUID.");
            }
            if (!uuids.insert(layer.uuid).second) {
                return invalid_result("Every layer UUID must be unique within a project.");
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
            if (!std::isfinite(layer.opacity) || layer.opacity < 0.0
                || layer.opacity > 1.0) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " opacity must be finite and between 0 and 1.");
            }

            const RenderConfig render =
                apply_global_config(project.canvas, structural_output, layer.render);
            const ValidationResult layer_validation = validate(render);
            if (!layer_validation.ok) {
                return invalid_result("Layer " + std::to_string(index + 1U)
                                      + " is invalid: " + layer_validation.message,
                                      layer_validation.estimated_peak_bytes);
            }
            if (layer.enabled && layer.opacity > 0.0) {
                has_contributing_layer = true;
                // Once an enabled layer is guaranteed to cover every pixel at
                // alpha 1, ordinary source-over alpha remains 1 for every
                // layer above it, even when those upper layers are partially
                // transparent. This permits a safely opaque composite to be
                // exported as RGB without merely guessing from the presence
                // of alpha-capable render data elsewhere in the stack.
                enabled_stack_is_guaranteed_opaque =
                    enabled_stack_is_guaranteed_opaque
                    || (layer.opacity == 1.0
                        && !render_data_can_create_transparency(layer.render));
                worst_layer_peak =
                    std::max(worst_layer_peak, layer_validation.estimated_peak_bytes);
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
        if (peak_bytes > kMaximumProjectPeakBytes) {
            return invalid_result(
                "Estimated peak project rendering memory exceeds the 1024 MiB safety budget.",
                peak_bytes);
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
    }
    return "Unknown blend mode";
}

} // namespace pvt
