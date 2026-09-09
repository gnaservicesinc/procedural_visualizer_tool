#include "../src/obj_surface.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

// Optional measurement target, not a timing-sensitive CTest. Accepts the
// repository root as its only argument. Hashes include every output float bit.
int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    using namespace pvt::detail;
    using Clock = std::chrono::steady_clock;
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path temporary = fs::temp_directory_path()
        / ("pvt-obj-probe-" + std::to_string(Clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{temporary};
    try {
        fs::create_directories(temporary);
        std::string error;
        const auto require = [&](bool ok) { if (!ok) throw std::runtime_error(error); };
        std::vector<fs::path> paths;
        for (int file = 0; file < 3; ++file) {
            paths.push_back(temporary / (std::to_string(file) + ".obj"));
            std::ofstream output(paths.back());
            constexpr int side = 72;
            for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x) {
                output << "v " << x << ' ' << y << " 0\n";
            }
            for (int y = 0; y + 1 < side; ++y) for (int x = 0; x + 1 < side; ++x) {
                const int i = y * side + x + 1;
                output << "f " << i << ' ' << i+1 << ' ' << i+side+1 << '\n';
                output << "f " << i << ' ' << i+side+1 << ' ' << i+side << '\n';
            }
        }
        clear_obj_mesh_cache();
        std::shared_ptr<const ObjMesh> cached;
        const auto start = Clock::now();
        for (int frame = 0; frame < 20; ++frame) for (const auto& path : paths) {
            require(load_obj_mesh_cached(path.string(), cached, &error));
        }
        std::cout << "alternating_3_obj_ms="
                  << std::chrono::duration<double, std::milli>(Clock::now() - start).count()
                  << " parses=" << obj_mesh_cache_parse_count_for_testing() << '\n';

        pvt::Image source;
        source.width = 129;
        source.height = 127;
        source.pixels.resize(129U * 127U * 4U);
        for (std::size_t i = 0; i < source.pixels.size(); i += 4U) {
            source.pixels[i] = static_cast<float>((i/4U)%129U)/128.0F;
            source.pixels[i+1U] = static_cast<float>((i/4U)/129U)/126.0F;
            source.pixels[i+2U] = 0.25F;
        }
        ObjMesh cube;
        require(load_obj_mesh((root / "tests/assets/obj/closed_cube.obj").string(), cube, &error));
        ObjMesh offscreen;
        offscreen.positions = {{-1,-1,0}, {1,-1,0}, {0,1,0}};
        ObjTriangle triangle;
        triangle.corners[0].position = 0U;
        triangle.corners[1].position = 1U;
        triangle.corners[2].position = 2U;
        offscreen.triangles.assign(100000U, triangle);
        for (int scenario = 0; scenario < 9; ++scenario) {
            pvt::SurfaceConfig surface;
            surface.enabled = true;
            surface.mapping = pvt::SurfaceMapping::CustomObj;
            surface.sizing = pvt::SurfaceSizing::ShortSide;
            surface.projection = scenario % 2 == 0
                ? pvt::SurfaceProjection::Orthographic : pvt::SurfaceProjection::Perspective;
            surface.camera_distance = 3.4;
            surface.focal_length = 2.5;
            surface.rotation_x_degrees = -20.0;
            surface.rotation_y_turns_per_loop = 1;
            surface.lighting = 0.8;
            surface.curvature = scenario >= 4 ? 0.63 : 1.0;
            surface.scale_x = scenario >= 6 ? -1.0 : 1.0;
            if (scenario == 8) surface.position_x_percent = 10000.0;
            for (std::size_t i = 3U; i < source.pixels.size(); i += 4U) {
                source.pixels[i] = scenario % 4 < 2 ? 1.0F : 0.5F;
            }
            std::vector<double> times;
            std::uint64_t hash = 14695981039346656037ULL;
            for (int frame = 0; frame < 13; ++frame) {
                pvt::Image output;
                const auto before = Clock::now();
                require(apply_mesh_surface_mapping(source, output,
                    scenario == 8 ? offscreen : cube, surface,
                    static_cast<double>(frame) * 0.37, &error));
                if (frame == 0) continue;
                times.push_back(std::chrono::duration<double, std::milli>(Clock::now()-before).count());
                for (float component : output.pixels) {
                    std::uint32_t bits = 0;
                    std::memcpy(&bits, &component, sizeof(bits));
                    hash = (hash ^ bits) * 1099511628211ULL;
                }
            }
            std::sort(times.begin(), times.end());
            std::cout << "mesh_case=" << scenario << " median_ms=" << times[times.size()/2U]
                      << " hash=" << std::hex << hash << std::dec << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
