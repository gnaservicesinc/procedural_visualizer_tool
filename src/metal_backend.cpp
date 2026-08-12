#include "frame_renderer_internal.h"

#include "metal_kernels_source.h"

#define METALCPP_SYMBOL_VISIBILITY_HIDDEN
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace pvt::detail {
namespace {

struct alignas(16) UInt4 {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t z = 0U;
    std::uint32_t w = 0U;
};

struct alignas(16) Int4 {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
    std::int32_t w = 0;
};

struct alignas(16) Float4 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
};

struct alignas(16) GpuFrameConstants {
    UInt4 dimensions_counts;
    UInt4 counts_flags;
    UInt4 base_flags;
    Int4 signed_values;
    UInt4 transform_quant;
    UInt4 quant_values;
    Float4 phases;
    Float4 center_ghost;
    Float4 pattern0;
    Float4 pattern1;
    Float4 alpha_quant;
};

struct alignas(16) GpuWave {
    Float4 geometry;
    Float4 phase;
    Int4 behavior;
};

struct alignas(16) GpuSwing {
    Float4 value;
};

struct alignas(16) GpuEffect {
    UInt4 kind;
    Float4 primary;
    Float4 placement;
    Float4 glow_area;
};

struct alignas(16) GpuSurface {
    UInt4 kind;
    Float4 values;
};

static_assert(sizeof(UInt4) == 16U);
static_assert(sizeof(Int4) == 16U);
static_assert(sizeof(Float4) == 16U);
static_assert(sizeof(GpuFrameConstants) == 176U);
static_assert(sizeof(GpuWave) == 48U);
static_assert(sizeof(GpuSwing) == 16U);
static_assert(sizeof(GpuEffect) == 64U);
static_assert(sizeof(GpuSurface) == 32U);

bool cancelled(const std::atomic_bool* cancel) {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

template <typename T>
struct MetalReleaser {
    void operator()(T* pointer) const {
        if (pointer != nullptr) pointer->release();
    }
};

template <typename T>
using MetalPtr = std::unique_ptr<T, MetalReleaser<T>>;

template <typename T>
MetalPtr<T> take_metal_ownership(T* pointer) {
    return MetalPtr<T>(pointer);
}

std::string ns_text(const NS::String* text) {
    if (text == nullptr || text->utf8String() == nullptr) {
        return {};
    }
    return text->utf8String();
}

std::string ns_error_text(const NS::Error* error) {
    return error == nullptr ? std::string{} : ns_text(error->localizedDescription());
}

enum class Pipeline : std::size_t {
    Base = 0,
    Coordinate,
    Surface,
    BlockScale,
    GlowExtract,
    GlowHorizontal,
    GlowVertical,
    GlowCombine,
    Transform,
    Quantize,
    Count
};

constexpr const char* kPipelineNames[] = {
    "base_render",
    "coordinate_effect",
    "surface_mapping",
    "block_scale",
    "glow_extract",
    "glow_blur_horizontal",
    "glow_blur_vertical",
    "glow_combine",
    "transform_image",
    "quantize_image"};
static_assert(sizeof(kPipelineNames) / sizeof(*kPipelineNames)
              == static_cast<std::size_t>(Pipeline::Count));

class MetalContext final {
public:
    MetalContext() {
        MetalPtr<NS::AutoreleasePool> pool = take_metal_ownership(
            NS::AutoreleasePool::alloc()->init());
        device_ = take_metal_ownership(MTL::CreateSystemDefaultDevice());
        if (!device_) {
            status_ = "Metal could not find a system default GPU device.";
            return;
        }
        device_name_ = ns_text(device_->name());
        queue_ = take_metal_ownership(device_->newCommandQueue());
        if (!queue_) {
            status_ = "Metal could not create a command queue on "
                      + device_name_ + ".";
            return;
        }

        MetalPtr<MTL::CompileOptions> compile_options = take_metal_ownership(
            MTL::CompileOptions::alloc()->init());
        if (!compile_options) {
            status_ = "Metal could not allocate shader compile options.";
            return;
        }
        // Fast relaxed math noticeably changes alpha edges and seam-adjacent
        // trigonometry. Safe IEEE-oriented compilation keeps the GPU close to
        // the double-precision CPU reference while still using float storage.
        compile_options->setFastMathEnabled(false);
        NS::Error* raw_error = nullptr;
        NS::String* source = NS::String::string(
            kMetalKernelSource, NS::UTF8StringEncoding);
        library_ = take_metal_ownership(
            device_->newLibrary(source, compile_options.get(), &raw_error));
        if (!library_) {
            status_ = "Metal shader compilation failed";
            const std::string detail = ns_error_text(raw_error);
            if (!detail.empty()) status_ += ": " + detail;
            return;
        }

        pipelines_.resize(static_cast<std::size_t>(Pipeline::Count));
        for (std::size_t index = 0U; index < pipelines_.size(); ++index) {
            NS::String* function_name = NS::String::string(
                kPipelineNames[index], NS::UTF8StringEncoding);
            MetalPtr<MTL::Function> function = take_metal_ownership(
                library_->newFunction(function_name));
            if (!function) {
                status_ = "Metal shader function is missing: "
                          + std::string(kPipelineNames[index]) + ".";
                return;
            }
            raw_error = nullptr;
            pipelines_[index] = take_metal_ownership(
                device_->newComputePipelineState(function.get(), &raw_error));
            if (!pipelines_[index]) {
                status_ = "Metal could not create the "
                          + std::string(kPipelineNames[index]) + " pipeline";
                const std::string detail = ns_error_text(raw_error);
                if (!detail.empty()) status_ += ": " + detail;
                return;
            }
        }
        ready_ = true;
        status_ = "Metal is ready on " + device_name_ + ".";
    }

    bool ready() const { return ready_; }
    const std::string& status() const { return status_; }
    const std::string& device_name() const { return device_name_; }
    MTL::Device* device() const { return device_.get(); }
    MTL::CommandQueue* queue() const { return queue_.get(); }
    MTL::ComputePipelineState* pipeline(Pipeline value) const {
        return pipelines_[static_cast<std::size_t>(value)].get();
    }

private:
    bool ready_ = false;
    std::string status_;
    std::string device_name_;
    MetalPtr<MTL::Device> device_;
    MetalPtr<MTL::CommandQueue> queue_;
    MetalPtr<MTL::Library> library_;
    std::vector<MetalPtr<MTL::ComputePipelineState>> pipelines_;
};

MetalContext& metal_context() {
    static MetalContext context;
    return context;
}

class AdmissionLimiter final {
public:
    bool acquire(std::size_t requested_limit,
                 std::size_t requested_bytes,
                 std::size_t memory_budget,
                 const std::atomic_bool* cancel) {
        const std::size_t limit = requested_limit == 0U
                                      ? 2U
                                      : requested_limit;
        std::unique_lock<std::mutex> lock(mutex_);
        while (active_ >= limit
               || (memory_budget != 0U
                   && (active_bytes_ > memory_budget
                       || requested_bytes
                              > memory_budget - active_bytes_))) {
            if (cancelled(cancel)) return false;
            wake_.wait_for(lock, std::chrono::milliseconds(5));
        }
        ++active_;
        active_bytes_ += requested_bytes;
        return true;
    }

    void release(std::size_t requested_bytes) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ > 0U) {
                --active_;
                active_bytes_ -= std::min(active_bytes_, requested_bytes);
            }
        }
        wake_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::size_t active_ = 0U;
    std::size_t active_bytes_ = 0U;
};

AdmissionLimiter& admission_limiter() {
    static AdmissionLimiter limiter;
    return limiter;
}

class AdmissionPermit final {
public:
    AdmissionPermit(std::size_t limit, std::size_t requested_bytes,
                    std::size_t memory_budget,
                    const std::atomic_bool* cancel)
        : requested_bytes_(requested_bytes),
          acquired_(admission_limiter().acquire(
              limit, requested_bytes, memory_budget, cancel)) {}
    ~AdmissionPermit() {
        if (acquired_) admission_limiter().release(requested_bytes_);
    }
    bool acquired() const { return acquired_; }

private:
    std::size_t requested_bytes_ = 0U;
    bool acquired_ = false;
};

template <typename T>
MetalPtr<MTL::Buffer> make_input_buffer(MTL::Device* device,
                                       const std::vector<T>& values) {
    const T zero{};
    const void* data = values.empty()
                           ? static_cast<const void*>(&zero)
                           : static_cast<const void*>(values.data());
    const std::size_t count = std::max<std::size_t>(1U, values.size());
    return take_metal_ownership(device->newBuffer(
        data, count * sizeof(T), MTL::ResourceStorageModeShared));
}

MetalPtr<MTL::Buffer> make_frame_buffer(MTL::Device* device,
                                       std::size_t bytes) {
    return take_metal_ownership(device->newBuffer(
        bytes, MTL::ResourceStorageModeShared));
}

void encode_grid(MTL::CommandBuffer* command_buffer,
                 MTL::ComputePipelineState* pipeline,
                 MTL::Size grid,
                 const std::function<void(MTL::ComputeCommandEncoder*)>& bind) {
    MTL::ComputeCommandEncoder* encoder =
        command_buffer->computeCommandEncoder();
    encoder->setComputePipelineState(pipeline);
    bind(encoder);
    const NS::UInteger execution_width =
        std::max<NS::UInteger>(1U, pipeline->threadExecutionWidth());
    const NS::UInteger maximum_threads =
        std::max<NS::UInteger>(execution_width,
                               pipeline->maxTotalThreadsPerThreadgroup());
    const NS::UInteger thread_x = std::min<NS::UInteger>(16U,
                                                        execution_width);
    const NS::UInteger thread_y = std::max<NS::UInteger>(
        1U, std::min<NS::UInteger>(16U, maximum_threads / thread_x));
    encoder->dispatchThreads(grid, MTL::Size(thread_x, thread_y, 1U));
    encoder->endEncoding();
}

GpuFrameConstants make_constants(const RenderConfig& config,
                                 const PreparedFrame& prepared) {
    GpuFrameConstants result;
    result.dimensions_counts = {
        static_cast<std::uint32_t>(config.width),
        static_cast<std::uint32_t>(config.height),
        static_cast<std::uint32_t>(config.block_size),
        static_cast<std::uint32_t>(prepared.waves.size())};
    result.counts_flags = {
        static_cast<std::uint32_t>(prepared.spatial_swings.size()),
        static_cast<std::uint32_t>(prepared.starting_palette.size()),
        config.alpha.enabled ? 1U : 0U,
        config.displacement_enabled ? 1U : 0U};
    result.base_flags = {
        config.lighting_enabled ? 1U : 0U,
        config.spiral_enabled ? 1U : 0U,
        config.wall_reflection_enabled ? 1U : 0U,
        0U};
    result.signed_values = {
        config.alpha.cycles_per_loop,
        config.spiral_arms,
        config.hue_cycles,
        static_cast<std::int32_t>(config.transform.mirror)};
    result.transform_quant = {
        config.transform.flip_horizontal ? 1U : 0U,
        config.transform.flip_vertical ? 1U : 0U,
        config.quantization.enabled ? 1U : 0U,
        static_cast<std::uint32_t>(config.quantization.mode)};
    result.quant_values = {
        static_cast<std::uint32_t>(config.quantization.levels), 0U, 0U, 0U};
    result.phases = {
        static_cast<float>(prepared.loop_phase),
        static_cast<float>(prepared.global_motion_phase),
        static_cast<float>(0.85 + 0.35 * std::sin(prepared.loop_phase)),
        static_cast<float>(std::min(config.width, config.height))};
    double center_x = 0.5 * static_cast<double>(config.width);
    double center_y = 0.5 * static_cast<double>(config.height);
    if (!prepared.waves.empty()) {
        center_x = prepared.waves.front().source_x;
        center_y = prepared.waves.front().source_y;
    }
    constexpr double kPi = 3.141592653589793238462643383279502884;
    result.center_ghost = {
        static_cast<float>(center_x), static_cast<float>(center_y),
        static_cast<float>(config.ghost_lag_degrees * kPi / 180.0),
        static_cast<float>(config.ghost_mix)};
    result.pattern0 = {
        static_cast<float>(config.displacement),
        static_cast<float>(config.wave_depth),
        static_cast<float>(config.spiral_frequency),
        static_cast<float>(config.wall_frequency)};
    result.pattern1 = {
        static_cast<float>(config.wall_mix),
        static_cast<float>(config.saturation),
        static_cast<float>(prepared.audio_hue_shift_degrees),
        static_cast<float>(config.alpha.spatial_frequency)};
    result.alpha_quant = {
        static_cast<float>(config.alpha.minimum),
        static_cast<float>(config.alpha.maximum),
        static_cast<float>(config.alpha.phase_degrees * kPi / 180.0),
        static_cast<float>(config.quantization.mix)};
    return result;
}

std::vector<GpuWave> make_waves(const PreparedFrame& prepared) {
    std::vector<GpuWave> result;
    result.reserve(prepared.waves.size());
    for (const PreparedWave& wave : prepared.waves) {
        GpuWave gpu;
        gpu.geometry = {static_cast<float>(wave.source_x),
                        static_cast<float>(wave.source_y),
                        static_cast<float>(wave.amplitude),
                        static_cast<float>(wave.spatial_frequency)};
        gpu.phase = {static_cast<float>(wave.phase_radians),
                     static_cast<float>(wave.direction), 0.0F, 0.0F};
        gpu.behavior = {wave.cycles_per_loop,
                        wave.synchronized ? 1 : 0, 0, 0};
        result.push_back(gpu);
    }
    return result;
}

std::vector<GpuSwing> make_swings(const PreparedFrame& prepared) {
    std::vector<GpuSwing> result;
    result.reserve(prepared.spatial_swings.size());
    for (const PreparedSpatialSwing& swing : prepared.spatial_swings) {
        result.push_back({{static_cast<float>(swing.center_x),
                           static_cast<float>(swing.center_y),
                           static_cast<float>(swing.radius),
                           static_cast<float>(swing.contribution)}});
    }
    return result;
}

std::vector<Float4> make_palette(const PreparedFrame& prepared) {
    std::vector<Float4> result;
    result.reserve(prepared.starting_palette.size());
    for (const auto& color : prepared.starting_palette) {
        result.push_back({static_cast<float>(color[0]),
                          static_cast<float>(color[1]),
                          static_cast<float>(color[2]),
                          static_cast<float>(color[3])});
    }
    return result;
}

GpuEffect make_effect(const PreparedEffect& effect) {
    GpuEffect result;
    result.kind = {static_cast<std::uint32_t>(effect.type),
                   static_cast<std::uint32_t>(effect.space),
                   static_cast<std::uint32_t>(effect.edge_mode), 0U};
    result.primary = {static_cast<float>(effect.phase),
                      static_cast<float>(effect.intensity),
                      static_cast<float>(effect.magnitude),
                      static_cast<float>(effect.frequency)};
    result.placement = {static_cast<float>(effect.secondary),
                        static_cast<float>(effect.center_x),
                        static_cast<float>(effect.center_y),
                        static_cast<float>(effect.angle_radians)};
    result.glow_area = {static_cast<float>(effect.radius_pixels),
                        static_cast<float>(effect.threshold),
                        static_cast<float>(effect.soft_knee),
                        static_cast<float>(effect.area_radius)};
    return result;
}

GpuSurface make_surface(const RenderConfig& config,
                        const PreparedFrame& prepared) {
    constexpr double kPi = 3.141592653589793238462643383279502884;
    GpuSurface result;
    result.kind = {static_cast<std::uint32_t>(config.surface.mapping),
                   0U, 0U, 0U};
    result.values = {
        static_cast<float>(
            static_cast<double>(config.surface.rotations_per_loop)
                * prepared.loop_phase
            + config.surface.phase_degrees * kPi / 180.0),
        static_cast<float>(config.surface.curvature),
        static_cast<float>(config.surface.lighting), 0.0F};
    return result;
}

bool surface_has_work(const SurfaceConfig& surface) {
    if (!surface.enabled) return false;
    if (surface.mapping != SurfaceMapping::Plane) {
        return surface.curvature > 0.0;
    }
    return surface.rotations_per_loop != 0
           || std::fmod(surface.phase_degrees, 360.0) != 0.0;
}

bool transform_has_work(const LayerTransformConfig& transform) {
    return transform.flip_horizontal || transform.flip_vertical
           || transform.mirror != MirrorMode::None;
}

} // namespace

bool metal_backend_compiled() {
    return true;
}

bool metal_backend_available(std::string* device_name,
                             std::string* status) {
    MetalContext& context = metal_context();
    if (device_name != nullptr) *device_name = context.device_name();
    if (status != nullptr) *status = context.status();
    return context.ready();
}

bool metal_backend_supports(const RenderConfig& config,
                            std::string* reason) {
    if (surface_has_work(config.surface)
        && config.surface.mapping == SurfaceMapping::CustomObj) {
        if (reason != nullptr) {
            *reason = "The strict GPU backend does not rasterize custom OBJ surfaces; use CPU + GPU for automatic CPU fallback.";
        }
        return false;
    }
    if (config.motion.enabled
        && (config.motion.path != LayerMotionPath::None
            || config.motion.rotations_per_loop != 0
            || config.motion.scale_pulse > 0.0
            || std::fabs(config.motion.center_x - 0.5) > 1.0e-12
            || std::fabs(config.motion.center_y - 0.5) > 1.0e-12)) {
        if (reason != nullptr) {
            *reason = "Animated layer motion currently uses the reference CPU path; use CPU + GPU for automatic per-layer fallback.";
        }
        return false;
    }
    const auto particle = std::find_if(
        config.effects.begin(), config.effects.end(),
        [](const EffectConfig& effect) {
            return effect.enabled && effect.type == EffectType::ParticleField
                   && effect.intensity > 0.0;
        });
    if (particle != config.effects.end()) {
        if (reason != nullptr) {
            *reason = "Particle fields currently use the reference CPU path; use CPU + GPU for automatic per-layer fallback.";
        }
        return false;
    }
    if (reason != nullptr) reason->clear();
    return true;
}

bool render_prepared_frame_metal(const RenderConfig& config,
                                 const PreparedFrame& prepared,
                                 const FrameRenderOptions& options,
                                 Image& destination,
                                 const std::atomic_bool* cancel,
                                 std::string* error) {
    if (cancelled(cancel)) {
        return fail(error,
                    "Metal rendering was cancelled; destination was unchanged.");
    }
    MetalContext& context = metal_context();
    if (!context.ready()) return fail(error, context.status());

    const std::size_t pixel_count = static_cast<std::size_t>(config.width)
                                    * static_cast<std::size_t>(config.height);
    if (pixel_count > std::numeric_limits<std::size_t>::max()
                          / sizeof(Float4)) {
        return fail(error, "Metal frame buffer size overflowed.");
    }
    const std::size_t frame_bytes = pixel_count * sizeof(Float4);
    if (frame_bytes > std::numeric_limits<std::size_t>::max() / 3U) {
        return fail(error, "Metal working buffer size overflowed.");
    }
    const std::size_t working_bytes = frame_bytes * 3U;
    // Reserve no more than three quarters of the device's advisory working set
    // across all callers. Input arrays are tiny and the remaining quarter also
    // leaves the driver and the rest of the application breathing room.
    const std::uint64_t recommended =
        context.device()->recommendedMaxWorkingSetSize();
    const std::uint64_t recommended_budget =
        recommended == 0U ? 0U : recommended - recommended / 4U;
    const std::size_t memory_budget = recommended_budget
                                             > std::numeric_limits<std::size_t>::max()
                                         ? std::numeric_limits<std::size_t>::max()
                                         : static_cast<std::size_t>(recommended_budget);
    if (memory_budget != 0U && working_bytes > memory_budget) {
        return fail(error,
                    "This Metal frame's three working buffers would exceed the bounded GPU working-set budget.");
    }
    AdmissionPermit permit(options.maximum_gpu_frames_in_flight,
                           working_bytes, memory_budget, cancel);
    if (!permit.acquired()) {
        return fail(error,
                    "Metal rendering was cancelled while waiting for a bounded GPU slot; destination was unchanged.");
    }
    MetalPtr<NS::AutoreleasePool> pool = take_metal_ownership(
        NS::AutoreleasePool::alloc()->init());

    const GpuFrameConstants constants = make_constants(config, prepared);
    const std::vector<GpuWave> waves = make_waves(prepared);
    const std::vector<GpuSwing> swings = make_swings(prepared);
    const std::vector<Float4> palette = make_palette(prepared);
    auto wave_buffer = make_input_buffer(context.device(), waves);
    auto swing_buffer = make_input_buffer(context.device(), swings);
    auto palette_buffer = make_input_buffer(context.device(), palette);
    auto first = make_frame_buffer(context.device(), frame_bytes);
    auto second = make_frame_buffer(context.device(), frame_bytes);
    auto auxiliary = make_frame_buffer(context.device(), frame_bytes);
    if (!wave_buffer || !swing_buffer || !palette_buffer
        || !first || !second || !auxiliary) {
        return fail(error,
                    "Metal could not allocate its bounded shared frame buffers.");
    }

    MTL::CommandBuffer* command_buffer = context.queue()->commandBuffer();
    if (command_buffer == nullptr) {
        return fail(error, "Metal could not create a command buffer.");
    }
    MTL::Buffer* current = first.get();
    MTL::Buffer* scratch = second.get();
    MTL::Buffer* aux = auxiliary.get();
    const auto pixel_grid = MTL::Size(
        static_cast<NS::UInteger>(config.width),
        static_cast<NS::UInteger>(config.height), 1U);
    const auto block_grid = MTL::Size(
        static_cast<NS::UInteger>((config.width + config.block_size - 1)
                                  / config.block_size),
        static_cast<NS::UInteger>((config.height + config.block_size - 1)
                                  / config.block_size), 1U);

    encode_grid(command_buffer, context.pipeline(Pipeline::Base), block_grid,
                [&](MTL::ComputeCommandEncoder* encoder) {
                    encoder->setBytes(&constants, sizeof(constants), 0U);
                    encoder->setBuffer(wave_buffer.get(), 0U, 1U);
                    encoder->setBuffer(swing_buffer.get(), 0U, 2U);
                    encoder->setBuffer(palette_buffer.get(), 0U, 3U);
                    encoder->setBuffer(current, 0U, 4U);
                });

    const auto encode_effect_stage = [&](EffectSpace stage) {
        for (const PreparedEffect& prepared_effect : prepared.effects) {
            if (prepared_effect.space != stage) continue;
            const GpuEffect effect = make_effect(prepared_effect);
            if (prepared_effect.type == EffectType::Glow) {
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::GlowExtract), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(current, 0U, 2U);
                                encoder->setBuffer(scratch, 0U, 3U);
                            });
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::GlowHorizontal), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(scratch, 0U, 2U);
                                encoder->setBuffer(aux, 0U, 3U);
                            });
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::GlowVertical), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(aux, 0U, 2U);
                                encoder->setBuffer(scratch, 0U, 3U);
                            });
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::GlowCombine), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(current, 0U, 2U);
                                encoder->setBuffer(scratch, 0U, 3U);
                            });
            } else if (prepared_effect.type == EffectType::BlockScale) {
                // Dispatching the maximum possible block grid is safe: the
                // kernel derives the animated block size and rejects excess IDs.
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::BlockScale), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(current, 0U, 2U);
                                encoder->setBuffer(scratch, 0U, 3U);
                            });
                std::swap(current, scratch);
            } else {
                encode_grid(command_buffer,
                            context.pipeline(Pipeline::Coordinate), pixel_grid,
                            [&](MTL::ComputeCommandEncoder* encoder) {
                                encoder->setBytes(&constants, sizeof(constants), 0U);
                                encoder->setBytes(&effect, sizeof(effect), 1U);
                                encoder->setBuffer(current, 0U, 2U);
                                encoder->setBuffer(scratch, 0U, 3U);
                            });
                std::swap(current, scratch);
            }
        }
    };

    encode_effect_stage(EffectSpace::Texture);
    if (surface_has_work(config.surface)) {
        const GpuSurface surface = make_surface(config, prepared);
        encode_grid(command_buffer, context.pipeline(Pipeline::Surface),
                    pixel_grid,
                    [&](MTL::ComputeCommandEncoder* encoder) {
                        encoder->setBytes(&constants, sizeof(constants), 0U);
                        encoder->setBytes(&surface, sizeof(surface), 1U);
                        encoder->setBuffer(current, 0U, 2U);
                        encoder->setBuffer(scratch, 0U, 3U);
                    });
        std::swap(current, scratch);
    }
    if (transform_has_work(config.transform)) {
        encode_grid(command_buffer, context.pipeline(Pipeline::Transform),
                    pixel_grid,
                    [&](MTL::ComputeCommandEncoder* encoder) {
                        encoder->setBytes(&constants, sizeof(constants), 0U);
                        encoder->setBuffer(current, 0U, 1U);
                        encoder->setBuffer(scratch, 0U, 2U);
                    });
        std::swap(current, scratch);
    }
    encode_effect_stage(EffectSpace::Surface);
    if (config.quantization.enabled && config.quantization.mix > 0.0) {
        encode_grid(command_buffer, context.pipeline(Pipeline::Quantize),
                    pixel_grid,
                    [&](MTL::ComputeCommandEncoder* encoder) {
                        encoder->setBytes(&constants, sizeof(constants), 0U);
                        encoder->setBuffer(current, 0U, 1U);
                    });
    }

    if (cancelled(cancel)) {
        return fail(error,
                    "Metal rendering was cancelled before submission; destination was unchanged.");
    }
    command_buffer->commit();
    command_buffer->waitUntilCompleted();
    if (command_buffer->status() == MTL::CommandBufferStatusError) {
        std::string message = "Metal command execution failed";
        const std::string detail = ns_error_text(command_buffer->error());
        if (!detail.empty()) message += ": " + detail;
        return fail(error, message);
    }
    if (cancelled(cancel)) {
        return fail(error,
                    "Metal rendering was cancelled; destination was unchanged.");
    }

    Image candidate;
    candidate.width = config.width;
    candidate.height = config.height;
    candidate.pixels.resize(pixel_count * 4U);
    std::memcpy(candidate.pixels.data(), current->contents(), frame_bytes);
    destination = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
}

} // namespace pvt::detail
