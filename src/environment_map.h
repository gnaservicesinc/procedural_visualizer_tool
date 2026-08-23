#ifndef PVT_ENVIRONMENT_MAP_H
#define PVT_ENVIRONMENT_MAP_H

#include "procedural_visualizer_tool.h"

#include <atomic>
#include <memory>
#include <string>

namespace pvt::detail {

struct EnvironmentMapRgb {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
};

// Immutable render-time state. Preparation performs source decoding and all
// control validation once; sampling is allocation-free and noexcept.
struct PreparedEnvironmentMap {
    std::shared_ptr<const Image> image;
    double rotation_turns = 0.0;
    double radiance_scale = 1.0;
    double mix = 0.0;

    explicit operator bool() const noexcept { return image != nullptr; }
};

bool prepare_environment_map(const EnvironmentMapConfig& config,
                             PreparedEnvironmentMap& prepared,
                             const std::atomic_bool* cancel,
                             std::string* error);

// Samples a fixed five-direction cosine-weighted approximation of diffuse
// irradiance from a Y-up equirectangular map. U wraps at the longitude seam and
// V clamps at the poles. Alpha is deliberately ignored.
EnvironmentMapRgb sample_environment_map_diffuse(
    const PreparedEnvironmentMap& environment,
    double normal_x, double normal_y, double normal_z) noexcept;

} // namespace pvt::detail

#endif
