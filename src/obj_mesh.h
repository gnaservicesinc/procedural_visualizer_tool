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
    // Stable, source-order labels for triangles connected through at least one
    // referenced position. Generated and imported meshes populate this once
    // before becoming immutable so animation planning never rebuilds topology
    // for every frame.
    std::vector<std::size_t> triangle_components;
    std::size_t connected_component_count = 0U;

    // Bounds include referenced positions only. The renderer can transform
    // (position - normalization_center) * normalization_scale to fit the
    // longest object axis into [-1, 1] without distorting its aspect ratio.
    ObjVec3 bounds_min{};
    ObjVec3 bounds_max{};
    ObjVec3 normalization_center{};
    double normalization_scale = 1.0;

    std::size_t estimated_bytes() const noexcept;
};

// Rebuilds triangle_components transactionally from the current triangle
// topology. Components are labelled in first-triangle order, independent of
// hash-table iteration or thread scheduling.
bool rebuild_obj_mesh_components(ObjMesh& mesh, std::string* error = nullptr);

struct ObjLoadLimits {
    // ObjCorner uses UINT32_MAX as its missing-index sentinel, so an OBJ may
    // contain every representable non-sentinel index. Other storage is bounded
    // only by size_t/vector allocation. Tests may still provide smaller limits
    // to exercise transactional rejection paths.
    std::size_t maximum_file_bytes = (std::numeric_limits<std::size_t>::max)();
    std::size_t maximum_line_bytes = (std::numeric_limits<std::size_t>::max)();
    std::size_t maximum_positions = ObjCorner::missing;
    std::size_t maximum_texcoords = ObjCorner::missing;
    std::size_t maximum_normals = ObjCorner::missing;
    std::size_t maximum_triangles = (std::numeric_limits<std::size_t>::max)();
    std::size_t maximum_polygon_corners =
        (std::numeric_limits<std::uint32_t>::max)();
    std::size_t maximum_mesh_bytes = (std::numeric_limits<std::size_t>::max)();
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
// file that changes during parsing is retried once and then rejected. Cold
// concurrent requests for the same key share one in-flight parse.
bool load_obj_mesh_cached(const std::string& utf8_path,
                          std::shared_ptr<const ObjMesh>& destination,
                          std::string* error,
                          const ObjLoadLimits& limits = ObjLoadLimits{});

void clear_obj_mesh_cache() noexcept;

#if defined(PVT_OBJ_MESH_TEST_HOOKS)
std::uint64_t obj_mesh_cache_parse_count_for_testing() noexcept;
void set_obj_mesh_cache_parse_paused_for_testing(bool paused) noexcept;
void arm_obj_mesh_cache_publication_pause_for_testing() noexcept;
void wait_obj_mesh_cache_publication_paused_for_testing();
void resume_obj_mesh_cache_publication_for_testing() noexcept;
#endif

} // namespace detail
} // namespace pvt

#endif
