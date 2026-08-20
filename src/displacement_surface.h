#ifndef PVT_DISPLACEMENT_SURFACE_H
#define PVT_DISPLACEMENT_SURFACE_H

#include "obj_mesh.h"
#include "procedural_visualizer_tool.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

namespace pvt::detail {

// Computes the generated grid dimensions and retained mesh allocation without
// opening the height map. Returns false if the requested grid cannot be
// represented by ObjCorner's uint32 indices or size_t storage.
PVT_API bool displacement_mesh_requirements(int render_width,
                                            int render_height,
                                            int pixels_per_node,
                                            std::size_t& columns,
                                            std::size_t& rows,
                                            std::size_t& estimated_bytes,
                                            std::string* error = nullptr);

// The cache key includes the decoded PNG/OpenEXR identity, render dimensions, ratio,
// and displacement range. The source-image cache replaces its shared image
// when the file size or modification time changes, which automatically
// invalidates the generated mesh as well.
PVT_API bool load_displacement_plane_mesh(
    const PlaneDisplacementConfig& displacement,
    int render_width,
    int render_height,
    std::shared_ptr<const ObjMesh>& destination,
    const std::atomic_bool* cancel = nullptr,
    std::string* error = nullptr);

bool apply_displacement_plane_mapping(
    const Image& source,
    Image& destination,
    const SurfaceConfig& surface,
    double loop_phase,
    std::string* error,
    const std::atomic_bool* cancel = nullptr);

void clear_displacement_mesh_cache() noexcept;

} // namespace pvt::detail

#endif
