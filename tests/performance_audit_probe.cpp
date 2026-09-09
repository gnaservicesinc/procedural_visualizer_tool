#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// Deliberately separate from CTest: timings are evidence, not pass/fail gates.
int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string(argv[1]) == "gpu";
    pvt::FrameRenderOptions options;
    options.backend = gpu ? pvt::RenderBackend::Gpu : pvt::RenderBackend::Cpu;
    for (const auto mapping : {pvt::SurfaceMapping::Sphere,
                               pvt::SurfaceMapping::Cylinder,
                               pvt::SurfaceMapping::Cube}) {
        for (const bool translucent : {false, true}) {
            pvt::RenderConfig config = pvt::default_config();
            config.width = 512;
            config.height = 512;
            config.block_size = 16;
            config.surface.enabled = true;
            config.surface.mapping = mapping;
            config.surface.rotation_y_turns_per_loop = 1;
            config.surface.lighting = 0.8;
            config.alpha.enabled = translucent;
            config.alpha.minimum = translucent ? 0.4 : 1.0;
            config.alpha.maximum = config.alpha.minimum;
            pvt::Image image;
            std::string error;
            if (!pvt::render_frame(config, 0, options, image, nullptr, &error)) {
                std::cerr << error << '\n';
                return 1;
            }
            std::vector<double> times;
            std::uint64_t hash = 14695981039346656037ULL;
            for (int frame = 0; frame < 12; ++frame) {
                const auto start = std::chrono::steady_clock::now();
                if (!pvt::render_frame(config, frame * 19, options, image,
                                       nullptr, &error)) {
                    std::cerr << error << '\n';
                    return 1;
                }
                times.push_back(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count());
                for (float component : image.pixels) {
                    std::uint32_t bits = 0U;
                    std::memcpy(&bits, &component, sizeof(bits));
                    hash = (hash ^ bits) * 1099511628211ULL;
                }
            }
            std::sort(times.begin(), times.end());
            std::cout << (gpu ? "gpu" : "cpu") << ' '
                      << static_cast<int>(mapping) << ' '
                      << (translucent ? "translucent" : "opaque")
                      << " median_ms=" << times[times.size() / 2U]
                      << " hash=" << std::hex << hash << std::dec << '\n';
        }
    }
}
