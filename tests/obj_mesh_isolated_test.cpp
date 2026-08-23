#include "../src/obj_mesh.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

int fail(int code, const std::string& message) {
    std::cerr << message << '\n';
    return code;
}

} // namespace

int main(int argc, char** argv) {
    using namespace pvt::detail;
    const fs::path source = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path assets = source / "tests" / "assets" / "obj";
    std::string error;

    ObjMesh polygon;
    if (!load_obj_mesh((assets / "polygon_negative.obj").string(), polygon, &error)
        || polygon.positions.size() != 5U || polygon.texcoords.size() != 5U
        || polygon.normals.size() != 1U || polygon.triangles.size() != 3U
        || std::fabs(polygon.normalization_scale - (2.0 / 3.0)) > 1.0e-12) {
        return fail(1, "polygon/negative-index parse failed: " + error);
    }

    ObjMesh variants;
    if (!load_obj_mesh((assets / "face_variants.obj").string(), variants, &error)
        || variants.triangles.size() != 4U
        || variants.triangles[0].corners[0].texcoord != ObjCorner::missing
        || variants.triangles[0].corners[0].normal != ObjCorner::missing
        || variants.triangles[1].corners[0].texcoord == ObjCorner::missing
        || variants.triangles[1].corners[0].normal != ObjCorner::missing
        || variants.triangles[2].corners[0].texcoord != ObjCorner::missing
        || variants.triangles[2].corners[0].normal == ObjCorner::missing
        || variants.triangles[3].corners[0].texcoord == ObjCorner::missing
        || variants.triangles[3].corners[0].normal == ObjCorner::missing) {
        return fail(2, "face-variant parse failed: " + error);
    }

    ObjMesh unchanged = polygon;
    if (load_obj_mesh((assets / "malformed_zero_index.obj").string(), unchanged, &error)
        || unchanged.triangles.size() != polygon.triangles.size()) {
        return fail(3, "transactional invalid-index rejection failed");
    }

    const std::string bom =
        "\xEF\xBB\xBFv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    ObjMesh bom_mesh;
    if (!parse_obj_mesh(bom, bom_mesh, &error) || bom_mesh.triangles.size() != 1U) {
        return fail(4, "UTF-8 BOM parse failed: " + error);
    }
    if (polygon.connected_component_count != 1U
        || polygon.triangle_components.size() != polygon.triangles.size()
        || std::any_of(polygon.triangle_components.begin(),
                       polygon.triangle_components.end(),
                       [](std::size_t component) { return component != 0U; })) {
        return fail(9, "connected polygon topology was not labelled stably");
    }
    const std::string disconnected =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "v 3 0 0\nv 4 0 0\nv 3 1 0\n"
        "f 1 2 3\nf 4 5 6\n";
    ObjMesh disconnected_mesh;
    if (!parse_obj_mesh(disconnected, disconnected_mesh, &error)
        || disconnected_mesh.connected_component_count != 2U
        || disconnected_mesh.triangle_components
               != std::vector<std::size_t>{0U, 1U}) {
        return fail(10, "disconnected triangle components were not deterministic: "
                            + error);
    }
    const std::size_t metadata_bytes =
        disconnected_mesh.triangle_components.capacity()
        * sizeof(std::size_t);
    if (disconnected_mesh.estimated_bytes()
        < sizeof(ObjMesh) + metadata_bytes) {
        return fail(11, "component metadata was omitted from the mesh estimate");
    }

    ObjLoadLimits tiny_limits;
    tiny_limits.maximum_triangles = 0U;
    if (parse_obj_mesh("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                       unchanged, &error, tiny_limits)) {
        return fail(5, "triangle limit was not enforced");
    }
    const ObjLoadLimits system_limits;
    if (system_limits.maximum_file_bytes
            != (std::numeric_limits<std::size_t>::max)()
        || system_limits.maximum_line_bytes
               != (std::numeric_limits<std::size_t>::max)()
        || system_limits.maximum_positions != ObjCorner::missing
        || system_limits.maximum_polygon_corners
               != (std::numeric_limits<std::uint32_t>::max)()
        || system_limits.maximum_mesh_bytes
               != (std::numeric_limits<std::size_t>::max)()) {
        return fail(5, "default OBJ limits are not derived from representation bounds");
    }
    const std::string concave =
        "v 0 0 0\nv 2 0 0\nv 1 1 0\nv 2 2 0\nv 0 2 0\n"
        "f 1 2 3 4 5\n";
    ObjMesh concave_mesh;
    if (!parse_obj_mesh(concave, concave_mesh, &error)
        || concave_mesh.triangles.size() != 3U) {
        return fail(5, "concave polygon ear clipping failed: " + error);
    }

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path temporary = fs::temp_directory_path()
                               / ("pvt-obj-cache-" + std::to_string(nonce) + ".obj");
    const fs::path unrelated = fs::temp_directory_path()
                               / ("pvt-obj-cache-unrelated-"
                                  + std::to_string(nonce) + ".obj");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    clear_obj_mesh_cache();
    std::vector<std::shared_ptr<const ObjMesh>> threaded(8U);
    std::vector<int> threaded_ok(threaded.size(), 0);
    std::vector<std::string> threaded_errors(threaded.size());
    std::atomic<std::size_t> ready{0U};
    std::atomic_bool release{false};
    std::vector<std::thread> threads;
    for (std::size_t index = 0U; index < threaded.size(); ++index) {
        threads.emplace_back([&, index] {
            ready.fetch_add(1U, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            threaded_ok[index] = load_obj_mesh_cached(
                temporary.string(), threaded[index],
                &threaded_errors[index]) ? 1 : 0;
        });
    }
    while (ready.load(std::memory_order_acquire) < threaded.size()) {
        std::this_thread::yield();
    }
    release.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t index = 0U; index < threaded.size(); ++index) {
        if (threaded_ok[index] == 0) {
            return fail(6, "cold shared cache load failed: "
                               + threaded_errors[index]);
        }
        if (threaded[index] != threaded.front()) {
            return fail(7, "cold cache returned a different mesh instance");
        }
    }
    if (obj_mesh_cache_parse_count_for_testing() != 1U) {
        return fail(7, "cold same-key requests did not share one OBJ parse");
    }

    std::shared_ptr<const ObjMesh> first = threaded.front();
    std::shared_ptr<const ObjMesh> repeated;
    if (!load_obj_mesh_cached(temporary.string(), repeated, &error)
        || first != repeated
        || obj_mesh_cache_parse_count_for_testing() != 1U) {
        return fail(6, "strong cache reuse failed: " + error);
    }

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                  "f 1 2 3\nf 1 3 4\n";
    }
    std::shared_ptr<const ObjMesh> changed;
    if (!load_obj_mesh_cached(temporary.string(), changed, &error)
        || changed == first || changed->triangles.size() != 2U
        || obj_mesh_cache_parse_count_for_testing() != 2U) {
        return fail(8, "cache change detection failed: " + error);
    }

    // Clearing is an epoch boundary. A pre-clear caller still receives the
    // mesh it requested, but a post-clear caller must neither join that parse
    // nor let its older result repopulate the one-entry cache.
    clear_obj_mesh_cache();
    set_obj_mesh_cache_parse_paused_for_testing(true);
    std::shared_ptr<const ObjMesh> pre_clear;
    std::shared_ptr<const ObjMesh> post_clear;
    std::string pre_clear_error;
    std::string post_clear_error;
    bool pre_clear_ok = false;
    bool post_clear_ok = false;
    std::thread pre_clear_thread([&] {
        pre_clear_ok = load_obj_mesh_cached(
            temporary.string(), pre_clear, &pre_clear_error);
    });
    while (obj_mesh_cache_parse_count_for_testing() != 1U) {
        std::this_thread::yield();
    }
    clear_obj_mesh_cache();
    std::thread post_clear_thread([&] {
        post_clear_ok = load_obj_mesh_cached(
            temporary.string(), post_clear, &post_clear_error);
    });
    while (obj_mesh_cache_parse_count_for_testing() != 1U) {
        std::this_thread::yield();
    }
    set_obj_mesh_cache_parse_paused_for_testing(false);
    pre_clear_thread.join();
    post_clear_thread.join();
    std::shared_ptr<const ObjMesh> after_clear_cached;
    if (!pre_clear_ok || !post_clear_ok
        || !load_obj_mesh_cached(
            temporary.string(), after_clear_cached, &error)
        || pre_clear == post_clear || after_clear_cached != post_clear
        || obj_mesh_cache_parse_count_for_testing() != 1U) {
        return fail(12, "cache clear generation fence failed: "
                            + pre_clear_error + post_clear_error + error);
    }

    // Final filesystem verification must not hold the global cache mutex. An
    // older, already-verified parse is paused immediately before publication;
    // an unrelated newer request must complete while it is paused and remain
    // the one strong cache entry after the older caller resumes.
    {
        std::ofstream output(unrelated, std::ios::binary | std::ios::trunc);
        output << "v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n";
    }
    clear_obj_mesh_cache();
    arm_obj_mesh_cache_publication_pause_for_testing();
    std::shared_ptr<const ObjMesh> older_publication;
    std::shared_ptr<const ObjMesh> newer_publication;
    std::string older_publication_error;
    std::string newer_publication_error;
    bool older_publication_ok = false;
    bool newer_publication_ok = false;
    std::atomic_bool newer_publication_done{false};
    std::thread older_publication_thread([&] {
        older_publication_ok = load_obj_mesh_cached(
            temporary.string(), older_publication,
            &older_publication_error);
    });
    wait_obj_mesh_cache_publication_paused_for_testing();
    std::thread newer_publication_thread([&] {
        newer_publication_ok = load_obj_mesh_cached(
            unrelated.string(), newer_publication,
            &newer_publication_error);
        newer_publication_done.store(true, std::memory_order_release);
    });
    const auto publication_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!newer_publication_done.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < publication_deadline) {
        std::this_thread::yield();
    }
    const bool newer_completed_before_resume =
        newer_publication_done.load(std::memory_order_acquire);
    resume_obj_mesh_cache_publication_for_testing();
    older_publication_thread.join();
    newer_publication_thread.join();
    std::shared_ptr<const ObjMesh> publication_cached;
    if (!newer_completed_before_resume
        || !older_publication_ok || !newer_publication_ok
        || !load_obj_mesh_cached(
            unrelated.string(), publication_cached, &error)
        || publication_cached != newer_publication
        || older_publication == newer_publication
        || obj_mesh_cache_parse_count_for_testing() != 2U) {
        return fail(13, "cache publication ordering failed: "
                            + older_publication_error
                            + newer_publication_error + error);
    }

    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    fs::remove(unrelated, cleanup_error);
    std::cout << "OBJ parser/cache isolated tests passed\n";
    return 0;
}
