#ifndef PVT_FRAME_RENDERER_INTERNAL_H
#define PVT_FRAME_RENDERER_INTERNAL_H

#include "procedural_visualizer_tool.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace pvt::detail {

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

} // namespace pvt::detail

#endif
