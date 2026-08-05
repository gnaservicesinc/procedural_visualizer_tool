#include "../src/obj_mesh.h"

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

    ObjLoadLimits tiny_limits;
    tiny_limits.maximum_triangles = 0U;
    if (parse_obj_mesh("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n",
                       unchanged, &error, tiny_limits)) {
        return fail(5, "triangle limit was not enforced");
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
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    }
    clear_obj_mesh_cache();
    std::shared_ptr<const ObjMesh> first;
    std::shared_ptr<const ObjMesh> repeated;
    if (!load_obj_mesh_cached(temporary.string(), first, &error)
        || !load_obj_mesh_cached(temporary.string(), repeated, &error)
        || first != repeated) {
        return fail(6, "strong cache reuse failed: " + error);
    }

    std::vector<std::shared_ptr<const ObjMesh>> threaded(8U);
    std::vector<std::thread> threads;
    for (std::size_t index = 0U; index < threaded.size(); ++index) {
        threads.emplace_back([&, index] {
            std::string local_error;
            if (!load_obj_mesh_cached(temporary.string(), threaded[index],
                                      &local_error)) {
                std::cerr << local_error << '\n';
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::shared_ptr<const ObjMesh>& mesh : threaded) {
        if (mesh != first) {
            return fail(7, "thread-safe cache returned a different instance");
        }
    }

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
                  "f 1 2 3\nf 1 3 4\n";
    }
    std::shared_ptr<const ObjMesh> changed;
    if (!load_obj_mesh_cached(temporary.string(), changed, &error)
        || changed == first || changed->triangles.size() != 2U) {
        return fail(8, "cache change detection failed: " + error);
    }

    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    std::cout << "OBJ parser/cache isolated tests passed\n";
    return 0;
}
