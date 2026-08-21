#include "procedural_visualizer_tool.h"

#include "frame_renderer_internal.h"

#include <atomic>
#include <cmath>
#include <exception>
#include <new>
#include <string>

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
    // CPU + GPU remains useful on machines without a suitable context and for
    // frames that have no supported analytic surface stage.
    return cpu_render();
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
        const RenderConfig resolved = detail::materialize_parameter_lfos(
            config, normalized_phase);
        return render_with_backend(
            resolved, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend_at_phase(
                    resolved, normalized_phase, prepared, prepare_error);
            },
            [&] {
                return render_frame_at_phase_cancellable(
                    resolved, normalized_phase, destination, cancel, error);
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
        const RenderConfig resolved =
            detail::materialize_parameter_lfos_at_frame(config, frame_index);
        return render_with_backend(
            resolved, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend(
                    resolved, frame_index, prepared, prepare_error);
            },
            [&] {
                return render_frame_cancellable(
                    resolved, frame_index, destination, cancel, error);
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
