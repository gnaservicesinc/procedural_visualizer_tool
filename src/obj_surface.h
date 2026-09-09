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

// OpenGL's mapped/layer/depth readback arrays, two RGBA32F textures, and
// two depth32F textures, excluding the caller's source/destination images.
constexpr std::size_t kOpenGlMeshBytesPerPixel = 19U * sizeof(float);

struct MeshGeometryMemory {
    std::size_t projected_vertices = 0U;
    std::size_t transformed_normals = 0U;
    std::size_t upload_staging = 0U;
    std::size_t gpu_buffers = 0U;
    std::size_t total = 0U;
};

// Per-frame CPU geometry plus a conservative cold OpenGL upload peak. The
// immutable ObjMesh allocation is shared and must be accounted separately.
PVT_API bool mesh_geometry_memory_requirements(
    std::size_t positions, std::size_t normals, std::size_t triangles,
    bool opengl_upload, MeshGeometryMemory& result) noexcept;

// loop_phase is expressed in radians, matching core.cpp's internal convention.
// The operation is transactional: destination is unchanged on failure.
bool apply_obj_surface_mapping(const Image& source,
                               Image& destination,
                               const std::string& utf8_obj_path,
                               const SurfaceConfig& surface,
                               double loop_phase,
                               std::string* error,
                               const std::atomic_bool* cancel = nullptr);

// Renders an already-built immutable mesh through the same projection,
// visibility, UV, lighting, and transparency pipeline used for Custom OBJ.
// This is the common path used by generated displacement planes.
bool apply_mesh_surface_mapping(const Image& source,
                                Image& destination,
                                const ObjMesh& mesh,
                                const SurfaceConfig& surface,
                                double loop_phase,
                                std::string* error,
                                const std::atomic_bool* cancel = nullptr);

} // namespace detail
} // namespace pvt

#endif
