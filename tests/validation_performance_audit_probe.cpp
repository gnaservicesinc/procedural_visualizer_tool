#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

// Timing runs are optional and outside CTest. Static builds also run the
// allocation-only regression. Count requested C++ allocation bytes on this
// thread, not RSS, driver heaps, or allocations in layer workers.
namespace {
thread_local bool count_allocations = false;
thread_local std::size_t allocation_bytes = 0U;
}

void* operator new(std::size_t size) {
    if (void* result = std::malloc(size == 0U ? 1U : size)) {
        if (count_allocations) allocation_bytes += size;
        return result;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
pvt::ClockConfig music_clock() {
    pvt::ClockConfig clock;
    clock.mode = pvt::ClockMode::Music;
    auto& music = clock.music;
    music.analyzer_version = "validation-audit";
    music.source_sha256.assign(64U, 'a');
    music.source_basename = "audit.wav";
    music.source_format = "wav-f32";
    music.source_sample_rate = 1000U;
    music.source_frame_count = 600000U;
    music.source_channel_count = 2U;
    music.duration_seconds = 600.0;
    music.detected_bpm = 120.0;
    music.tempo_confidence = 0.9;
    for (int beat = 0; beat < 1200; ++beat) {
        music.beat_times_seconds.push_back(0.5 * beat);
    }
    music.tempo_points = {{0.0, 120.0, 0.9}};
    music.feature_samples.resize(60000U);
    for (std::size_t index = 0U; index < music.feature_samples.size(); ++index) {
        music.feature_samples[index].energy =
            static_cast<float>(index % 101U) / 100.0F;
    }
    for (int index = 0; index < 3; ++index) {
        pvt::AudioFrequencyStreamConfig range;
        range.uuid = "range-" + std::to_string(index);
        range.low_hz = 20.0 + 100.0 * index;
        range.high_hz = range.low_hz + 100.0;
        clock.audio_processing.frequency_streams.push_back(range);
        pvt::MusicFrequencyStreamAnalysis stream;
        stream.uuid = range.uuid;
        stream.low_hz = range.low_hz;
        stream.high_hz = range.high_hz;
        stream.beat_times_seconds = music.beat_times_seconds;
        stream.tempo_points = music.tempo_points;
        stream.feature_samples = music.feature_samples;
        music.frequency_streams.push_back(std::move(stream));
    }
    music.input_processing = clock.audio_processing;
    return clock;
}

template<class Operation>
void measure(const char* name, Operation operation, bool allocations) {
    operation();
    std::vector<double> times;
    std::size_t bytes = 0U;
    for (int iteration = 0; iteration < 9; ++iteration) {
        allocation_bytes = 0U;
        count_allocations = allocations;
        const auto start = std::chrono::steady_clock::now();
        operation();
        const auto end = std::chrono::steady_clock::now();
        count_allocations = false;
        bytes = allocation_bytes;
        times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::sort(times.begin(), times.end());
    std::cout << name << " median_ms=" << times[times.size() / 2U];
    if (allocations) std::cout << " requested_bytes=" << bytes;
    std::cout << '\n';
}
}

int main(int argc, char** argv) {
    try {
        auto project = pvt::default_project();
        project.canvas.width = 64;
        project.canvas.height = 64;
        project.canvas.clock = music_clock();
        project.canvas.audio_reactive_defaults.enabled = true;
        for (std::size_t index = 1U; index < 4U; ++index) {
            project.layers.push_back(pvt::default_layer(index));
        }
        for (auto& layer : project.layers) {
            layer.render.layer_clock.clock = project.canvas.clock;
            layer.render.layer_clock.enabled = true;
            for (const char* path : {"ghost_mix", "displacement", "wave_depth",
                                    "spiral_frequency", "wall_mix", "saturation",
                                    "surface.rotation_y", "motion.scale"}) {
                pvt::ParameterLfo lfo;
                lfo.id = layer.render.parameter_lfos.size() + 100U;
                lfo.target_path = path;
                lfo.minimum = 0.2;
                lfo.maximum = 0.8;
                layer.render.parameter_lfos.push_back(lfo);
            }
        }
        auto config = pvt::apply_global_config(
            project.canvas, project.output, project.layers.front().render);
        const auto check = [](const pvt::ValidationResult& result) {
            if (!result.ok) throw std::runtime_error(result.message);
        };
        if (argc > 1 && std::string(argv[1]) == "--check-allocations") {
            // The entire validation must allocate less than one feature
            // table. This catches both recursive clock copies and per-LFO
            // layer copies without depending on allocator bookkeeping sizes.
            const auto limit = config.clock.music.feature_samples.size()
                * sizeof(pvt::MusicFeatureSample);
            for (bool enabled : {false, true}) {
                for (auto& lfo : config.parameter_lfos) lfo.enabled = enabled;
                allocation_bytes = 0U;
                count_allocations = true;
                const auto validation = pvt::validate(config);
                count_allocations = false;
                check(validation);
                std::cout << "validation_requested_bytes=" << allocation_bytes << '\n';
                if (allocation_bytes >= limit) {
                    throw std::runtime_error("Validation duplicated a large music table.");
                }
            }
            return 0;
        }
        measure("validate_single", [&] { check(pvt::validate(config)); }, true);
        measure("validate_project_4", [&] { check(pvt::validate(project)); }, true);
        pvt::FrameRenderOptions options;
        options.backend = pvt::RenderBackend::Cpu;
        options.maximum_cpu_workers = 2U;
        pvt::Image image;
        std::string error;
        int frame = 0;
        measure("animated_project_4", [&] {
            if (!pvt::render_project_frame(project, frame++ * 719, options,
                                           image, nullptr, &error)) {
                throw std::runtime_error(error);
            }
        }, false);
        // Hash ten distinct synchronized frames independently of timing.
        std::uint64_t hash = UINT64_C(14695981039346656037);
        for (frame = 0; frame < 10; ++frame) {
            if (!pvt::render_project_frame(project, frame * 719, options,
                                           image, nullptr, &error)) {
                throw std::runtime_error(error);
            }
            for (float component : image.pixels) {
                std::uint32_t bits = 0U;
                std::memcpy(&bits, &component, sizeof(bits));
                hash = (hash ^ bits) * UINT64_C(1099511628211);
            }
        }
        std::cout << "animated_hash=" << std::hex << hash << '\n';
    } catch (const std::exception& exception) {
        count_allocations = false;
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
