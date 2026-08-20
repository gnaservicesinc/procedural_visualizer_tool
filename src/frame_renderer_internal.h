#ifndef PVT_FRAME_RENDERER_INTERNAL_H
#define PVT_FRAME_RENDERER_INTERNAL_H

#include "procedural_visualizer_tool.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

namespace pvt::detail {

// Particle work is bounded in units of conservatively covered stamp pixels.
// Keep this calculation shared by validation, persistence recovery, and GPU
// preflight so an accepted setup cannot acquire a different trail count in a
// later stage.  This header is private; none of these helpers extend the public
// installed API.
inline bool particle_stamp_budget_for_canvas(int width, int height,
                                             std::size_t& budget) {
    budget = 0U;
    if (width <= 0 || height <= 0) return false;
    const std::size_t unsigned_width = static_cast<std::size_t>(width);
    const std::size_t unsigned_height = static_cast<std::size_t>(height);
    if (unsigned_width != 0U
        && unsigned_height
               > (std::numeric_limits<std::size_t>::max)() / unsigned_width) {
        return false;
    }
    const std::size_t pixels = unsigned_width * unsigned_height;
    constexpr std::size_t kMinimumParticleStampBudget = 20000000U;
    const std::size_t scaled =
        pixels > (std::numeric_limits<std::size_t>::max)() / 8U
            ? (std::numeric_limits<std::size_t>::max)()
            : pixels * 8U;
    budget = std::max(kMinimumParticleStampBudget, scaled);
    return true;
}

template <typename ParticleEffect>
inline std::size_t effective_particle_trail_steps(
    int width, int height, const ParticleEffect& effect) {
    const std::size_t authored = 1U + static_cast<std::size_t>(std::llround(
        std::clamp(effect.secondary, 0.0, 1.0) * 12.0));
    const double short_side = static_cast<double>(std::min(width, height));
    const double travel = effect.magnitude * short_side;
    return effect.particle_profile == ParticleRenderProfile::Defined
               && effect.particle_orientation != ParticleOrientation::Fixed
               && std::abs(travel) <= 1.0e-12
           ? 1U
           : authored;
}

inline bool particle_effect_stamp_workload(
    int width, int height, const EffectConfig& effect, std::size_t& work) {
    work = 0U;
    if (width <= 0 || height <= 0
        || !std::isfinite(effect.frequency) || effect.frequency < 1.0
        || effect.frequency
               > static_cast<double>((std::numeric_limits<int>::max)())
        || std::floor(effect.frequency) != effect.frequency
        || !std::isfinite(effect.secondary) || effect.secondary < 0.0
        || effect.secondary > 1.0
        || !std::isfinite(effect.radius_pixels)
        || effect.radius_pixels <= 0.0
        || !std::isfinite(effect.particle_size_variation)
        || effect.particle_size_variation < 0.0
        || effect.particle_size_variation > 1.0
        || !std::isfinite(effect.magnitude)) {
        return false;
    }
    const std::size_t particle_count = static_cast<std::size_t>(
        std::llround(effect.frequency));
    const std::size_t trail_steps = effective_particle_trail_steps(
        width, height, effect);
    const long double maximum_radius = std::max<long double>(
        0.5L, static_cast<long double>(effect.radius_pixels)
                  * (1.0L
                     + static_cast<long double>(
                         effect.particle_size_variation)));
    const long double bound =
        effect.particle_profile == ParticleRenderProfile::Defined ? 3.5L
                                                                   : 2.5L;
    const long double side_long =
        std::ceil(2.0L * bound * maximum_radius) + 3.0L;
    if (!std::isfinite(side_long) || side_long < 0.0L
        || side_long
               > static_cast<long double>(
                   (std::numeric_limits<std::size_t>::max)())) {
        return false;
    }
    const std::size_t side = static_cast<std::size_t>(side_long);
    const std::size_t stamp_width = std::min(
        static_cast<std::size_t>(width), side);
    const std::size_t stamp_height = std::min(
        static_cast<std::size_t>(height), side);
    if (stamp_width != 0U
        && stamp_height
               > (std::numeric_limits<std::size_t>::max)() / stamp_width) {
        return false;
    }
    const std::size_t stamp_pixels = stamp_width * stamp_height;
    if (particle_count != 0U
        && trail_steps
               > (std::numeric_limits<std::size_t>::max)() / particle_count) {
        return false;
    }
    const std::size_t stamps = particle_count * trail_steps;
    if (stamps != 0U
        && stamp_pixels
               > (std::numeric_limits<std::size_t>::max)() / stamps) {
        return false;
    }
    work = stamps * stamp_pixels;
    return true;
}

struct ParticleStampWorkloadEstimate {
    std::size_t work = 0U;
    std::size_t budget = 0U;
    std::size_t offending_effect = (std::numeric_limits<std::size_t>::max)();
    bool exceeds_budget = false;
};

inline bool estimate_particle_stamp_workload(
    const RenderConfig& config, ParticleStampWorkloadEstimate& estimate) {
    estimate = {};
    if (!particle_stamp_budget_for_canvas(config.width, config.height,
                                          estimate.budget)) {
        return false;
    }
    for (std::size_t index = 0U; index < config.effects.size(); ++index) {
        const EffectConfig& effect = config.effects[index];
        if (!effect.enabled || effect.type != EffectType::ParticleField
            || effect.intensity <= 0.0 || effect.frequency < 1.0
            || effect.radius_pixels <= 0.0) {
            continue;
        }
        std::size_t effect_work = 0U;
        if (!particle_effect_stamp_workload(config.width, config.height,
                                            effect, effect_work)) {
            estimate.offending_effect = index;
            return false;
        }
        if (effect_work > estimate.budget - estimate.work) {
            estimate.offending_effect = index;
            estimate.exceeds_budget = true;
            return false;
        }
        estimate.work += effect_work;
    }
    return true;
}

// Layer codecs do not know their eventual project canvas. This validates all
// structural and security bounds while deliberately deferring only the
// canvas-dependent particle admission check to assembled-project validation.
PVT_API ValidationResult validate_render_config_structure(
    const RenderConfig& config);

struct PreparedSpatialSwing {
    double center_x = 0.5;
    double center_y = 0.5;
    double radius = 0.0;
    double contribution = 0.0;
};

struct PreparedWave {
    double source_x = 0.0;
    double source_y = 0.0;
    double amplitude = 0.0;
    double spatial_frequency = 0.0;
    double phase_radians = 0.0;
    double direction = 0.5;
    double tangent_radians = 0.0;
    int cycles_per_loop = 0;
    bool synchronized = true;
    bool follow_tangent = false;
};

struct PreparedEffect {
    std::uint64_t id = 0U;
    EffectType type = EffectType::Ripple;
    EffectSpace space = EffectSpace::Texture;
    EdgeMode edge_mode = EdgeMode::Reflect;
    double phase = 0.0;
    double intensity = 0.0;
    double magnitude = 0.0;
    double frequency = 0.0;
    double secondary = 0.0;
    double center_x = 0.5;
    double center_y = 0.5;
    double angle_radians = 0.0;
    double radius_pixels = 0.0;
    double threshold = 0.0;
    double soft_knee = 0.0;
    double area_radius = 0.0;
    BlurType blur_type = BlurType::Gaussian;
    int blur_passes = 1;
    int blur_samples = 9;
    ParticleShape particle_shape = ParticleShape::Spark;
    ParticleRenderProfile particle_profile = ParticleRenderProfile::LegacyGlow;
    double particle_size_variation = 0.0;
    double particle_definition = 0.7;
    double particle_twinkle = 1.0;
    std::uint64_t particle_seed = 0U;
    ParticleOrientation particle_orientation = ParticleOrientation::Fixed;
    double particle_rotation_radians = 0.0;
};

struct PreparedFrame {
    double loop_phase = 0.0;
    double independent_loop_phase = 0.0;
    double global_motion_phase = 0.0;
    double audio_hue_shift_degrees = 0.0;
    std::vector<PreparedSpatialSwing> spatial_swings;
    std::vector<PreparedWave> waves;
    std::vector<PreparedEffect> effects;
    std::vector<std::array<double, 4U>> starting_palette;
    LayerMotionConfig motion;
};

PVT_API bool prepare_frame_for_backend_at_phase(const RenderConfig& config,
                                                double normalized_phase,
                                                PreparedFrame& prepared,
                                                std::string* error);
PVT_API bool prepare_frame_for_backend(const RenderConfig& config,
                                       int frame_index,
                                       PreparedFrame& prepared,
                                       std::string* error);

bool metal_backend_compiled();
bool metal_backend_available(std::string* device_name,
                             std::string* status);
bool prepare_starting_image_for_backend(const RenderConfig& config,
                                        double loop_phase,
                                        Image& destination,
                                        const std::atomic_bool* cancel,
                                        std::string* error);
bool metal_backend_supports(const RenderConfig& config, std::string* reason);
bool render_prepared_frame_metal(const RenderConfig& config,
                                 const PreparedFrame& prepared,
                                 const FrameRenderOptions& options,
                                 Image& destination,
                                 const std::atomic_bool* cancel,
                                 std::string* error);

bool opengl_surface_backend_compiled();
bool opengl_surface_backend_available(std::string* device_name,
                                      std::string* status);
bool opengl_backend_supports(const RenderConfig& config,
                             std::string* reason);
bool opengl_generated_base_supported(const RenderConfig& config);
bool opengl_surface_backend_supports(const SurfaceConfig& surface);
bool opengl_surface_acceleration_active();
bool set_opengl_surface_acceleration_active(bool active);
const PreparedFrame* opengl_prepared_frame();
const PreparedFrame* set_opengl_prepared_frame(
    const PreparedFrame* prepared);
bool render_generated_base_opengl(const RenderConfig& config,
                                  const PreparedFrame& prepared,
                                  Image& destination,
                                  const std::atomic_bool* cancel,
                                  std::string* error);
bool apply_surface_mapping_opengl(const Image& source, Image& destination,
                                  const SurfaceConfig& surface,
                                  double loop_phase,
                                  const std::atomic_bool* cancel,
                                  std::string* error);

} // namespace pvt::detail

#endif
