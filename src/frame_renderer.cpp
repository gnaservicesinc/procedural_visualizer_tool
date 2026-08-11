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
                    "GPU frames in flight must be between 0 and 8.");
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
    const bool available = detail::metal_backend_available(
        &device_name, &metal_status);
    std::string unsupported_reason;
    const bool supported = available
                           && detail::metal_backend_supports(
                                  config, &unsupported_reason);
    if (!available || !supported) {
        if (options.backend == RenderBackend::Gpu) {
            if (!available) {
                return fail(error,
                            metal_status.empty()
                                ? "Metal rendering is unavailable on this host."
                                : metal_status);
            }
            return fail(error,
                        unsupported_reason.empty()
                            ? "This frame is not supported by the Metal backend."
                            : unsupported_reason);
        }
        return cpu_render();
    }

    detail::PreparedFrame prepared;
    std::string metal_error;
    if (prepare(prepared, &metal_error)
        && detail::render_prepared_frame_metal(
            config, prepared, options, destination, cancel, &metal_error)) {
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    if (options.backend == RenderBackend::Gpu || cancelled(cancel)) {
        return fail(error,
                    metal_error.empty()
                        ? "Metal rendering failed."
                        : metal_error);
    }

    // CpuAndGpu is the resilient application mode: a pipeline/compiler/device
    // failure affects only acceleration, never the user's ability to render.
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
