#include "procedural_visualizer_tool.h"

#include "frame_renderer_internal.h"

#include <atomic>
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

class OpenGLSurfaceScope final {
public:
    explicit OpenGLSurfaceScope(bool active)
        : previous_(detail::set_opengl_surface_acceleration_active(active)) {}

    ~OpenGLSurfaceScope() {
        detail::set_opengl_surface_acceleration_active(previous_);
    }

    OpenGLSurfaceScope(const OpenGLSurfaceScope&) = delete;
    OpenGLSurfaceScope& operator=(const OpenGLSurfaceScope&) = delete;

private:
    bool previous_ = false;
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
                    "GPU frames in flight must fit the signed-int API limit.");
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
        && detail::opengl_surface_backend_supports(config, &unsupported_reason);
    if (opengl_supported) {
        // The reference renderer owns ordered stages and invokes OpenGL only
        // at the analytic 3D surface boundary. A runtime OpenGL failure is
        // returned directly; CPU + GPU never repeats that surface on CPU.
        OpenGLSurfaceScope scope(true);
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
        return render_with_backend(
            config, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend_at_phase(
                    config, normalized_phase, prepared, prepare_error);
            },
            [&] {
                return render_frame_at_phase_cancellable(
                    config, normalized_phase, destination, cancel, error);
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
        return render_with_backend(
            config, options, destination, cancel, error,
            [&](detail::PreparedFrame& prepared, std::string* prepare_error) {
                return detail::prepare_frame_for_backend(
                    config, frame_index, prepared, prepare_error);
            },
            [&] {
                return render_frame_cancellable(
                    config, frame_index, destination, cancel, error);
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
