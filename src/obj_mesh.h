#ifndef PVT_OBJ_MESH_H
#define PVT_OBJ_MESH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pvt {
namespace detail {

// Deliberately small, renderer-facing representation of the Wavefront OBJ
// subset used by the procedural surface mapper. Meshes are immutable after
// loading, so callers may safely share them between preview/export threads.
struct ObjVec2 {
    double x = 0.0;
    double y = 0.0;
};

struct ObjVec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct ObjCorner {
    static constexpr std::uint32_t missing =
        std::numeric_limits<std::uint32_t>::max();

    std::uint32_t position = missing;
    std::uint32_t texcoord = missing;
    std::uint32_t normal = missing;
};

struct ObjTriangle {
    std::array<ObjCorner, 3U> corners{};
};

struct ObjMesh {
    std::vector<ObjVec3> positions;
    std::vector<ObjVec2> texcoords;
    std::vector<ObjVec3> normals;
    std::vector<ObjTriangle> triangles;

    // Bounds include referenced positions only. The renderer can transform
    // (position - normalization_center) * normalization_scale to fit the
    // longest object axis into [-1, 1] without distorting its aspect ratio.
    ObjVec3 bounds_min{};
    ObjVec3 bounds_max{};
    ObjVec3 normalization_center{};
    double normalization_scale = 1.0;

    std::size_t estimated_bytes() const noexcept;
};

struct ObjLoadLimits {
    std::size_t maximum_file_bytes = 64U * 1024U * 1024U;
    std::size_t maximum_line_bytes = 1024U * 1024U;
    std::size_t maximum_positions = 1'000'000U;
    std::size_t maximum_texcoords = 1'000'000U;
    std::size_t maximum_normals = 1'000'000U;
    std::size_t maximum_triangles = 1'000'000U;
    std::size_t maximum_polygon_corners = 4096U;
    std::size_t maximum_mesh_bytes = 256U * 1024U * 1024U;
};

// Parses v, vt, vn and f records. Faces may use v, v/vt, v//vn or v/vt/vn
// corners, including negative relative indices. Simple convex or concave
// polygons are validated and triangulated with winding-preserving projected
// ear clipping; self-intersecting faces are rejected explicitly. Common
// metadata records are ignored; no MTL or sibling file is opened.
//
// Both functions are transactional: destination is unchanged on failure.
bool parse_obj_mesh(std::string_view contents,
                    ObjMesh& destination,
                    std::string* error,
                    const ObjLoadLimits& limits = ObjLoadLimits{});

bool load_obj_mesh(const std::string& utf8_path,
                   ObjMesh& destination,
                   std::string* error,
                   const ObjLoadLimits& limits = ObjLoadLimits{});

// Keeps one immutable mesh alive across frames. The cache key includes the
// absolute normalized path, file size, last-write time, and load limits. A
// file that changes during parsing is retried once and then rejected.
bool load_obj_mesh_cached(const std::string& utf8_path,
                          std::shared_ptr<const ObjMesh>& destination,
                          std::string* error,
                          const ObjLoadLimits& limits = ObjLoadLimits{});

void clear_obj_mesh_cache() noexcept;

} // namespace detail
} // namespace pvt

#endif
