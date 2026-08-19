#ifndef PVT_OBJ_SURFACE_H
#define PVT_OBJ_SURFACE_H

#include "obj_mesh.h"
#include "procedural_visualizer_tool.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace pvt {
namespace detail {

// Extra per-pixel working storage owned by the OBJ mapper, excluding the
// caller-provided source/destination images and immutable cached mesh.
constexpr std::size_t kObjSurfaceOpaqueBytesPerPixel = sizeof(float);
constexpr std::size_t kObjSurfaceLayeredBytesPerPixel =
    2U * sizeof(float) + 4U * sizeof(float);

// loop_phase is expressed in radians, matching core.cpp's internal convention.
// The operation is transactional: destination is unchanged on failure.
bool apply_obj_surface_mapping(const Image& source,
                               Image& destination,
                               const std::string& utf8_obj_path,
                               int rotations_per_loop,
                               double phase_degrees,
                               double curvature,
                               double lighting,
                               double loop_phase,
                               std::string* error,
                               const std::atomic_bool* cancel = nullptr);

// Renders an already-built immutable mesh through the same projection,
// visibility, UV, lighting, and transparency pipeline used for Custom OBJ.
// This is the common path used by generated displacement planes.
bool apply_mesh_surface_mapping(const Image& source,
                                Image& destination,
                                const ObjMesh& mesh,
                                int rotations_per_loop,
                                double phase_degrees,
                                double curvature,
                                double lighting,
                                double loop_phase,
                                std::string* error,
                                const std::atomic_bool* cancel = nullptr);

} // namespace detail
} // namespace pvt

#endif
