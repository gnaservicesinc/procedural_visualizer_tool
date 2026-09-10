#include "procedural_visualizer_tool.h"

#include "frame_renderer_internal.h"

#include <atomic>
#include <cmath>
#include <exception>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace pvt {
namespace {

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

void assign_blackout(Image& image, int width, int height) {
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<std::size_t>(width)
                            * static_cast<std::size_t>(height) * 4U,
                        0.0F);
    for (std::size_t offset = 3U; offset < image.pixels.size(); offset += 4U) {
        image.pixels[offset] = 1.0F;
    }
}

bool downsample_area(const Image& source, int width, int height,
                     Image& destination, const std::atomic_bool* cancel,
                     std::string* error) {
    Image candidate;
    candidate.width = width;
    candidate.height = height;
    candidate.pixels.resize(static_cast<std::size_t>(width)
                            * static_cast<std::size_t>(height) * 4U);
    const double scale_x = static_cast<double>(source.width) / width;
    const double scale_y = static_cast<double>(source.height) / height;
    for (int y = 0; y < height; ++y) {
        if (cancelled(cancel)) {
            return fail(error,
                        "Rendering was cancelled during subpixel resolve.");
        }
        const double y0 = y * scale_y;
        const double y1 = (y + 1) * scale_y;
        for (int x = 0; x < width; ++x) {
            const double x0 = x * scale_x;
            const double x1 = (x + 1) * scale_x;
            double weight_sum = 0.0;
            double alpha_sum = 0.0;
            double premultiplied[3]{};
            for (int sy = static_cast<int>(std::floor(y0));
                 sy < static_cast<int>(std::ceil(y1)); ++sy) {
                const double wy = std::max(
                    0.0, std::min(y1, static_cast<double>(sy + 1))
                             - std::max(y0, static_cast<double>(sy)));
                if (sy < 0 || sy >= source.height || wy == 0.0) continue;
                for (int sx = static_cast<int>(std::floor(x0));
                     sx < static_cast<int>(std::ceil(x1)); ++sx) {
                    const double wx = std::max(
                        0.0, std::min(x1, static_cast<double>(sx + 1))
                                 - std::max(x0, static_cast<double>(sx)));
                    if (sx < 0 || sx >= source.width || wx == 0.0) continue;
                    const double weight = wx * wy;
                    const std::size_t offset =
                        (static_cast<std::size_t>(sy)
                             * static_cast<std::size_t>(source.width)
                         + static_cast<std::size_t>(sx)) * 4U;
                    const double alpha = source.pixels[offset + 3U];
                    weight_sum += weight;
                    alpha_sum += alpha * weight;
                    for (std::size_t channel = 0U; channel < 3U; ++channel) {
                        premultiplied[channel] +=
                            source.pixels[offset + channel] * alpha * weight;
                    }
                }
            }
            const std::size_t output =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                 + static_cast<std::size_t>(x)) * 4U;
            const double alpha = weight_sum > 0.0
                ? alpha_sum / weight_sum : 0.0;
            candidate.pixels[output + 3U] = static_cast<float>(alpha);
            if (alpha_sum > 1.0e-20) {
                for (std::size_t channel = 0U; channel < 3U; ++channel) {
                    candidate.pixels[output + channel] = static_cast<float>(
                        premultiplied[channel] / alpha_sum);
                }
            } else {
                candidate.pixels[output] = 0.0F;
                candidate.pixels[output + 1U] = 0.0F;
                candidate.pixels[output + 2U] = 0.0F;
            }
        }
    }
    destination = std::move(candidate);
    return true;
}

class OpenGLAccelerationScope final {
public:
    explicit OpenGLAccelerationScope(const detail::PreparedFrame* prepared)
        : previous_active_(
              detail::set_opengl_surface_acceleration_active(true)),
          previous_prepared_(detail::set_opengl_prepared_frame(prepared)) {}

    ~OpenGLAccelerationScope() {
        detail::set_opengl_prepared_frame(previous_prepared_);
        detail::set_opengl_surface_acceleration_active(previous_active_);
    }

    OpenGLAccelerationScope(const OpenGLAccelerationScope&) = delete;
    OpenGLAccelerationScope& operator=(const OpenGLAccelerationScope&) = delete;

private:
    bool previous_active_ = false;
    const detail::PreparedFrame* previous_prepared_ = nullptr;
};

bool validate_frame_options(const FrameRenderOptions& options,
                            std::string* error) {
    switch (options.backend) {
        case RenderBackend::Cpu:
        case RenderBackend::CpuAndGpu:
        case RenderBackend::Gpu:
            break;
        default:
            return fail(error, "The selected rendering backend is invalid.");
    }
    if (options.maximum_gpu_frames_in_flight
        > kMaximumGpuFramesInFlight) {
        return fail(error,
                    "GPU frames in flight cannot exceed "
                        + std::to_string(kMaximumGpuFramesInFlight) + ".");
    }
    if (options.maximum_cpu_workers > kMaximumSequenceWorkers) {
        return fail(error,
                    "CPU layer worker count cannot exceed "
                        + std::to_string(kMaximumSequenceWorkers) + ".");
    }
    return true;
}

template <typename Prepare, typename CpuRender>
bool render_with_backend(const RenderConfig& config,
                         const FrameRenderOptions& options,
                         Image& destination,
                         const std::atomic_bool* cancel,
                         std::string* error,
                         Prepare&& prepare,
                         CpuRender&& cpu_render) {
    if (!validate_frame_options(options, error)) {
        return false;
    }
    if (cancelled(cancel)) {
        return fail(error,
                    "Rendering was cancelled; destination was unchanged.");
    }
    if (config.block_size == 0.0) {
        assign_blackout(destination, config.width, config.height);
        if (error != nullptr) error->clear();
        return true;
    }
    if (options.backend == RenderBackend::Cpu) {
        return cpu_render();
    }

    std::string device_name;
    std::string metal_status;
    const bool metal_available = detail::metal_backend_available(
        &device_name, &metal_status);
    std::string unsupported_reason;
    if (metal_available) {
        const bool supported = detail::metal_backend_supports(
            config, &unsupported_reason);
        if (!supported) {
            return fail(error,
                        unsupported_reason.empty()
                            ? "This frame is not supported by the Metal backend."
                            : unsupported_reason);
        }
        detail::PreparedFrame prepared;
        std::string metal_error;
        if (prepare(prepared, &metal_error)
            && detail::render_prepared_frame_metal(
                config, prepared, options, destination, cancel, &metal_error)) {
            if (error != nullptr) error->clear();
            return true;
        }
        // Once Metal is available, CPU + GPU never hides an acceleration
        // failure behind an unexpectedly slow whole-frame retry.
        return fail(error,
                    metal_error.empty()
                        ? "Metal rendering failed."
                        : metal_error);
    }

    std::string opengl_device;
    std::string opengl_status;
    const bool opengl_available = detail::opengl_surface_backend_available(
        &opengl_device, &opengl_status);
    const bool opengl_supported = opengl_available
        && detail::opengl_backend_supports(config, &unsupported_reason);
    if (opengl_supported) {
        detail::PreparedFrame prepared;
        std::string prepare_error;
        if (!prepare(prepared, &prepare_error)) {
            return fail(error,
                        prepare_error.empty()
                            ? "OpenGL frame preparation failed."
                            : prepare_error);
        }
        // The reference renderer retains ordered CPU stages, while OpenGL
        // owns every admitted generated-source and surface stage. Runtime
        // failures are returned directly and never retried on CPU.
        OpenGLAccelerationScope scope(&prepared);
        return cpu_render();
    }
    if (options.backend == RenderBackend::Gpu) {
        if (opengl_available && !unsupported_reason.empty()) {
            return fail(error, unsupported_reason);
        }
        if (!opengl_status.empty()
            && detail::opengl_surface_backend_compiled()) {
            return fail(error, opengl_status);
        }
        return fail(error,
                    metal_status.empty()
                        ? "No supported GPU renderer is available on this host."
                        : metal_status);
    }
    // CPU + GPU is a cooperative accelerator mode, not a CPU compatibility
    // fallback. If no GPU lane can accept the frame, surface that explicitly;
    // silently rerendering the whole frame on the CPU destroys live latency.
    if (opengl_available && !unsupported_reason.empty()) {
        return fail(error, unsupported_reason);
    }
    if (!opengl_status.empty()
        && detail::opengl_surface_backend_compiled()) {
        return fail(error, opengl_status);
    }
    return fail(error,
                metal_status.empty()
                    ? "CPU + GPU requires a supported GPU renderer; CPU-only operation is unsupported."
                    : metal_status);
}

} // namespace

bool render_frame_at_phase(const RenderConfig& config,
                           double normalized_phase,
                           const FrameRenderOptions& options,
                           Image& destination,
                           const std::atomic_bool* cancel,
                           std::string* error) {
    try {
        const ValidationResult validation =
            detail::validate_frame_render_config(config);
        if (!validation.ok) return fail(error, validation.message);
        if (!std::isfinite(normalized_phase)) {
            return fail(error, "Normalized render phase must be finite.");
        }
        std::optional<RenderConfig> resolved_storage;
        const RenderConfig* resolved = &config;
        if (detail::has_enabled_parameter_lfo(config)
            || config.block_size_modulation.lfo_enabled) {
            resolved_storage.emplace(config);
            if (detail::has_enabled_parameter_lfo(*resolved_storage)) {
                *resolved_storage = detail::materialize_parameter_lfos(
                    *resolved_storage, normalized_phase);
            }
            if (resolved_storage->block_size_modulation.lfo_enabled) {
                *resolved_storage = detail::materialize_block_size_modulation(
                    *resolved_storage, normalized_phase);
            }
            resolved = &*resolved_storage;
            const ValidationResult resolved_validation =
                detail::validate_frame_render_config(*resolved);
            if (!resolved_validation.ok) {
                return fail(error, resolved_validation.message);
            }
        }
        if (resolved->block_size > 0.0 && resolved->block_size < 1.0) {
            RenderConfig supersampled = *resolved;
            const double scale = 1.0 / supersampled.block_size;
            const int output_width = supersampled.width;
            const int output_height = supersampled.height;
            supersampled.width = static_cast<int>(std::ceil(
                static_cast<double>(output_width) * scale));
            supersampled.height = static_cast<int>(std::ceil(
                static_cast<double>(output_height) * scale));
            supersampled.block_size = 1.0;
            supersampled.block_size_modulation = {};
            Image high_resolution;
            if (!render_frame_at_phase(
                    supersampled, normalized_phase, options,
                    high_resolution, cancel, error)) {
                return false;
            }
            return downsample_area(high_resolution, output_width,
                                   output_height, destination, cancel, error);
        }
        return render_with_backend(
            *resolved, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend_at_phase_validated_resolved(
                    *resolved, normalized_phase, prepared, prepare_error);
            },
            [&] {
                return detail::render_frame_at_phase_validated_resolved(
                    *resolved, normalized_phase, destination, cancel, error);
            });
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "The selected frame renderer ran out of memory; destination was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("The selected frame renderer failed: ")
                               + exception.what());
    } catch (...) {
        return fail(error,
                    "The selected frame renderer failed with an unknown error.");
    }
}

bool render_frame(const RenderConfig& config, int frame_index,
                  const FrameRenderOptions& options,
                  Image& destination,
                  const std::atomic_bool* cancel,
                  std::string* error) {
    try {
        const ValidationResult validation =
            detail::validate_frame_render_config(config);
        if (!validation.ok) return fail(error, validation.message);
        std::optional<RenderConfig> resolved_storage;
        const RenderConfig* resolved = &config;
        if (detail::has_enabled_parameter_lfo(config)
            || config.block_size_modulation.lfo_enabled) {
            resolved_storage.emplace(config);
            if (detail::has_enabled_parameter_lfo(*resolved_storage)) {
                *resolved_storage = detail::materialize_parameter_lfos_at_frame(
                    *resolved_storage, frame_index);
            }
            if (resolved_storage->block_size_modulation.lfo_enabled) {
                *resolved_storage =
                    detail::materialize_block_size_modulation_at_frame(
                        *resolved_storage, frame_index);
            }
            resolved = &*resolved_storage;
            const ValidationResult resolved_validation =
                detail::validate_frame_render_config(*resolved);
            if (!resolved_validation.ok) {
                return fail(error, resolved_validation.message);
            }
        }
        if (resolved->block_size > 0.0 && resolved->block_size < 1.0) {
            RenderConfig supersampled = *resolved;
            const double scale = 1.0 / supersampled.block_size;
            const int output_width = supersampled.width;
            const int output_height = supersampled.height;
            supersampled.width = static_cast<int>(std::ceil(
                static_cast<double>(output_width) * scale));
            supersampled.height = static_cast<int>(std::ceil(
                static_cast<double>(output_height) * scale));
            supersampled.block_size = 1.0;
            supersampled.block_size_modulation = {};
            Image high_resolution;
            if (!render_frame(supersampled, frame_index, options,
                              high_resolution, cancel, error)) {
                return false;
            }
            return downsample_area(high_resolution, output_width,
                                   output_height, destination, cancel, error);
        }
        return render_with_backend(
            *resolved, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend_validated_resolved(
                    *resolved, frame_index, prepared, prepare_error);
            },
            [&] {
                return detail::render_frame_validated_resolved(
                    *resolved, frame_index, destination, cancel, error);
            });
    } catch (const std::bad_alloc&) {
        return fail(error,
                    "The selected frame renderer ran out of memory; destination was unchanged.");
    } catch (const std::exception& exception) {
        return fail(error, std::string("The selected frame renderer failed: ")
                               + exception.what());
    } catch (...) {
        return fail(error,
                    "The selected frame renderer failed with an unknown error.");
    }
}

RendererCapabilities renderer_capabilities() {
    RendererCapabilities capabilities;
    capabilities.metal_compiled = detail::metal_backend_compiled();
    capabilities.metal_available = detail::metal_backend_available(
        &capabilities.metal_device_name, &capabilities.metal_status);
    capabilities.opengl_surface_compiled =
        detail::opengl_surface_backend_compiled();
    capabilities.opengl_surface_available =
        detail::opengl_surface_backend_available(
            &capabilities.opengl_surface_device_name,
            &capabilities.opengl_surface_status);
    return capabilities;
}

const char* render_backend_name(RenderBackend value) {
    switch (value) {
        case RenderBackend::Cpu: return "CPU";
        case RenderBackend::CpuAndGpu: return "CPU + GPU";
        case RenderBackend::Gpu: return "GPU";
    }
    return "Unknown backend";
}

} // namespace pvt
