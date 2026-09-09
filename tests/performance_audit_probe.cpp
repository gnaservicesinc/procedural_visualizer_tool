#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::uint64_t hash_image(const pvt::Image& image, std::uint64_t hash) {
    for (const float component : image.pixels) {
        std::uint32_t bits = 0U;
        std::memcpy(&bits, &component, sizeof(bits));
        hash = (hash ^ bits) * UINT64_C(1099511628211);
    }
    return hash;
}

int gpu_concurrency_probe(const std::string& selected_slots) {
    if (!pvt::renderer_capabilities().metal_available) {
        std::cerr << "Metal is unavailable.\n";
        return 1;
    }
    pvt::RenderConfig config = pvt::default_config();
    config.width = 1280;
    config.height = 720;
    config.block_size = 8;
    config.total_frames = 257;
    config.palette = pvt::default_palette(2U);
    config.palette.enabled = true;
    config.palette.columns = 4U;
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Sphere;
    config.surface.rotation_y_turns_per_loop = 1;
    config.surface.lighting = 0.75;
    config.post_process.antialias_enabled = true;
    config.post_process.antialias_passes = 3;
    config.post_process.antialias_strength = 0.73;
    config.post_process.antialias_threshold = 0.01;

    pvt::FrameRenderOptions warmup_options;
    warmup_options.backend = pvt::RenderBackend::Gpu;
    warmup_options.maximum_gpu_frames_in_flight = 1U;
    pvt::Image warmup;
    std::string warmup_error;
    if (!pvt::render_frame(config, 0, warmup_options, warmup, nullptr,
                           &warmup_error)) {
        std::cerr << warmup_error << '\n';
        return 1;
    }

    constexpr int frame_count = 48;
    const std::size_t thread_count = std::min<std::size_t>(
        12U, std::max<std::size_t>(2U, std::thread::hardware_concurrency()));
    const std::vector<std::size_t> slot_counts = selected_slots.empty()
        ? std::vector<std::size_t>{1U, 2U, 4U, 6U, 8U, 12U, 0U}
        : std::vector<std::size_t>{selected_slots == "auto"
                                       ? 0U
                                       : static_cast<std::size_t>(
                                             std::stoull(selected_slots))};
    std::uint64_t expected_hash = 0U;
    for (const std::size_t slots : slot_counts) {
        pvt::FrameRenderOptions options;
        options.backend = pvt::RenderBackend::Gpu;
        options.maximum_gpu_frames_in_flight = slots;
        std::vector<pvt::Image> images(static_cast<std::size_t>(frame_count));
        std::vector<std::string> errors(thread_count);
        std::atomic<int> next_frame {0};
        std::atomic_bool failed {false};
        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        const std::clock_t cpu_start = std::clock();
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t worker = 0U; worker < thread_count; ++worker) {
            workers.emplace_back([&, worker] {
                for (;;) {
                    const int frame = next_frame.fetch_add(
                        1, std::memory_order_relaxed);
                    if (frame >= frame_count
                        || failed.load(std::memory_order_relaxed)) {
                        return;
                    }
                    if (!pvt::render_frame(
                            config, frame, options,
                            images[static_cast<std::size_t>(frame)], nullptr,
                            &errors[worker])) {
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }
        for (std::thread& worker : workers) worker.join();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const double cpu_ms = 1000.0
            * static_cast<double>(std::clock() - cpu_start)
            / static_cast<double>(CLOCKS_PER_SEC);
        if (failed.load(std::memory_order_relaxed)) {
            const auto message = std::find_if(
                errors.begin(), errors.end(),
                [](const std::string& value) { return !value.empty(); });
            std::cerr << (message == errors.end()
                              ? "Concurrent Metal rendering failed."
                              : *message)
                      << '\n';
            return 1;
        }

        std::uint64_t hash = UINT64_C(14695981039346656037);
        for (const pvt::Image& image : images) {
            hash = hash_image(image, hash);
        }
        if (expected_hash == 0U) expected_hash = hash;
        if (hash != expected_hash) {
            std::cerr << "Concurrent Metal output changed with the admission limit.\n";
            return 1;
        }
        std::cout << (slots == 0U ? "slots=auto" : "slots=" + std::to_string(slots))
                  << " elapsed_ms=" << elapsed_ms
                  << " fps=" << static_cast<double>(frame_count) * 1000.0
                                    / elapsed_ms
                  << " average_cpu_cores=" << cpu_ms / elapsed_ms
                  << " hash=" << std::hex << hash << std::dec << '\n';
    }
    return 0;
}

int gpu_project_concurrency_probe() {
    if (!pvt::renderer_capabilities().metal_available) {
        std::cerr << "Metal is unavailable.\n";
        return 1;
    }
    pvt::ProjectConfig project = pvt::default_project();
    project.canvas.width = 1280;
    project.canvas.height = 720;
    project.canvas.block_size = 8;
    project.canvas.total_frames = 257;
    project.output.write_alpha = true;
    project.layers.front().render.surface.enabled = true;
    project.layers.front().render.surface.mapping =
        pvt::SurfaceMapping::Sphere;
    for (std::size_t index = 1U; index < 8U; ++index) {
        pvt::LayerConfig layer = pvt::default_layer(index);
        layer.file_id = pvt::allocate_layer_file_id(project);
        layer.opacity = 0.35;
        layer.blend_mode = index % 2U == 0U
            ? pvt::BlendMode::Overlay : pvt::BlendMode::Normal;
        layer.render.surface.enabled = true;
        layer.render.surface.mapping = index % 3U == 0U
            ? pvt::SurfaceMapping::Cylinder : pvt::SurfaceMapping::Sphere;
        layer.render.surface.rotation_y_turns_per_loop =
            static_cast<int>(index);
        layer.render.palette = pvt::default_palette(index % 6U);
        layer.render.palette.enabled = true;
        layer.render.palette.columns = 4U;
        project.layers.push_back(std::move(layer));
    }

    constexpr int frame_count = 8;
    std::uint64_t expected_hash = 0U;
    for (const std::size_t slots : {1U, 2U, 3U, 4U, 6U, 8U, 12U, 0U}) {
        pvt::FrameRenderOptions options;
        options.backend = pvt::RenderBackend::Gpu;
        options.maximum_gpu_frames_in_flight = slots;
        std::vector<pvt::Image> images(static_cast<std::size_t>(frame_count));
        const std::clock_t cpu_start = std::clock();
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        for (int frame = 0; frame < frame_count; ++frame) {
            if (!pvt::render_project_frame(
                    project, frame, options,
                    images[static_cast<std::size_t>(frame)], nullptr,
                    &error)) {
                std::cerr << error << '\n';
                return 1;
            }
        }
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        const double cpu_ms = 1000.0
            * static_cast<double>(std::clock() - cpu_start)
            / static_cast<double>(CLOCKS_PER_SEC);
        std::uint64_t hash = UINT64_C(14695981039346656037);
        for (const pvt::Image& image : images) {
            hash = hash_image(image, hash);
        }
        if (expected_hash == 0U) expected_hash = hash;
        if (hash != expected_hash) {
            std::cerr << "Project output changed with the Metal layer-worker limit.\n";
            return 1;
        }
        std::cout << (slots == 0U ? "project_slots=auto"
                                  : "project_slots=" + std::to_string(slots))
                  << " elapsed_ms=" << elapsed_ms
                  << " fps=" << static_cast<double>(frame_count) * 1000.0
                                    / elapsed_ms
                  << " average_cpu_cores=" << cpu_ms / elapsed_ms
                  << " hash=" << std::hex << hash << std::dec << '\n';
    }
    return 0;
}

} // namespace

// Deliberately separate from CTest: timings are evidence, not pass/fail gates.
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "gpu-concurrency") {
        return gpu_concurrency_probe(argc > 2 ? argv[2] : std::string{});
    }
    if (argc > 1 && std::string(argv[1]) == "gpu-project-concurrency") {
        return gpu_project_concurrency_probe();
    }
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
                hash = hash_image(image, hash);
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
