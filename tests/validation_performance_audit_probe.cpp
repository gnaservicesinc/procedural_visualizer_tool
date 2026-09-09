#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <vector>

// Timing runs are optional and outside CTest. Static builds also run the
// allocation-only regression. Count requested C++ allocation bytes on this
// thread, not RSS, driver heaps, or allocations in layer workers.
namespace {
namespace fs = std::filesystem;
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

pvt::ProjectConfig path_project(std::size_t layer_count) {
    auto project = pvt::default_project();
    project.canvas.width = 64;
    project.canvas.height = 64;
    for (std::size_t index = 1U; index < layer_count; ++index) {
        project.layers.push_back(pvt::default_layer(index));
    }
    // Unbound authored paths must still be checked, but additional layers
    // should not rebuild the node-identity sets for this shared collection.
    for (std::uint64_t index = 1U; index <= 64U; ++index) {
        auto path = pvt::default_ellipse_path(index, 1U, "Audit path");
        path.nodes.resize(64U, path.nodes.front());
        for (std::size_t node = 0U; node < path.nodes.size(); ++node) {
            path.nodes[node].id = node + 1U;
        }
        project.canvas.motion_paths.push_back(std::move(path));
    }
    return project;
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
        if (argc > 1 && std::string(argv[1]) == "--sequence") {
            // Keep the large saved analyses valid and reachable to validation,
            // but use the authored frame count so this remains a short export
            // throughput probe rather than a 10-minute music render.
            project.canvas.clock.mode = pvt::ClockMode::Default;
            project.canvas.total_frames = 24;
            project.output.write_alpha = true;
            project.output.png_compression_level = 0;
            project.output.overwrite_existing = true;
            project.output.filename_prefix = "audit_";
            const fs::path directory = fs::temp_directory_path()
                                       / "pvt-validation-sequence-audit";
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            project.output.output_directory = directory.string();
            for (auto& layer : project.layers) {
                layer.render.layer_clock.enabled = false;
            }
            pvt::SequenceRenderOptions sequence_options;
            sequence_options.worker_count = 4U;
            sequence_options.frame.backend = pvt::RenderBackend::Cpu;
            const auto start = std::chrono::steady_clock::now();
            std::string error;
            if (!pvt::render_project_sequence(
                    project, sequence_options, {}, nullptr, &error)) {
                throw std::runtime_error(error);
            }
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            std::uint64_t hash = UINT64_C(14695981039346656037);
            for (int frame = 0; frame < project.canvas.total_frames; ++frame) {
                std::string number = std::to_string(frame);
                number.insert(0U, 4U - number.size(), '0');
                std::ifstream input(
                    directory / ("audit_" + number + ".png"),
                    std::ios::binary);
                for (std::istreambuf_iterator<char> byte(input), end;
                     byte != end; ++byte) {
                    hash = (hash ^ static_cast<unsigned char>(*byte))
                           * UINT64_C(1099511628211);
                }
            }
            std::cout << "project_sequence_24 elapsed_ms=" << elapsed_ms
                      << " hash=" << std::hex << hash << std::dec << '\n';
            fs::remove_all(directory, ignored);
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "--check-allocations") {
            // Each validation must allocate less than one feature table.
            // This catches recursive clock copies, per-LFO layer copies, and
            // project adapters without depending on allocator bookkeeping.
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
            project.output.write_alpha = true;
            for (bool enabled_lfos : {false, true}) {
                for (bool enabled_clocks : {false, true}) {
                    // Saved analysis must remain borrowed even when its
                    // owning layer or its rendering contribution is disabled.
                    for (int contribution = 0; contribution < 3; ++contribution) {
                        for (auto& layer : project.layers) {
                            layer.enabled = contribution != 0;
                            layer.opacity = contribution == 1 ? 0.0 : 1.0;
                            layer.render.layer_clock.enabled = enabled_clocks;
                            for (auto& lfo : layer.render.parameter_lfos) {
                                lfo.enabled = enabled_lfos;
                            }
                        }
                        allocation_bytes = 0U;
                        count_allocations = true;
                        const auto validation = pvt::validate(project);
                        count_allocations = false;
                        check(validation);
                        std::cout << "project_validation_requested_bytes="
                                  << allocation_bytes << '\n';
                        if (allocation_bytes >= limit) {
                            throw std::runtime_error(
                                "Project validation duplicated a large music table.");
                        }
                    }
                }
            }
            std::size_t one_layer_bytes = 0U;
            for (const std::size_t layers : {1U, 4U}) {
                const auto paths = path_project(layers);
                allocation_bytes = 0U;
                count_allocations = true;
                const auto validation = pvt::validate(paths);
                count_allocations = false;
                check(validation);
                std::cout << "path_validation_layers=" << layers
                          << " requested_bytes=" << allocation_bytes << '\n';
                if (layers == 1U) {
                    one_layer_bytes = allocation_bytes;
                } else if (allocation_bytes >= 2U * one_layer_bytes) {
                    throw std::runtime_error(
                        "Additional layers repeated shared path-validation allocations.");
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
        std::cout << "animated_hash=" << std::hex << hash << std::dec << '\n';
        const auto paths_one = path_project(1U);
        const auto paths_four = path_project(4U);
        measure("validate_paths_1", [&] { check(pvt::validate(paths_one)); }, true);
        measure("validate_paths_4", [&] { check(pvt::validate(paths_four)); }, true);
    } catch (const std::exception& exception) {
        count_allocations = false;
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
