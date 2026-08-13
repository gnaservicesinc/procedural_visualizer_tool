#include "procedural_visualizer_tool.h"
#include "../src/config_codec.h"
#include "../src/path_utf8.h"

#include <png.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <process.h>
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: "         \
                      << #condition << '\n';                                        \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

std::vector<unsigned char> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool has_temporary_output(const fs::path& directory) {
    if (!fs::exists(directory)) {
        return false;
    }
    return std::any_of(fs::directory_iterator(directory), fs::directory_iterator(),
                       [](const fs::directory_entry& entry) {
                           return entry.path().filename().string().find(".tmp.")
                                  != std::string::npos;
                       });
}

std::vector<unsigned char> decode_png_rgba8(const fs::path& path,
                                             png_uint_32* width,
                                             png_uint_32* height) {
    png_image image {};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0) {
        CHECK(false);
        return {};
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0) {
        CHECK(false);
        png_image_free(&image);
        return {};
    }
    *width = image.width;
    *height = image.height;
    png_image_free(&image);
    return pixels;
}

void make_small(pvt::RenderConfig& config) {
    config.width = 96;
    config.height = 64;
    config.block_size = 4;
    config.total_frames = 12;
    config.fps = 24.0;
}

pvt::ClockConfig ready_music_clock(double duration_seconds = 1.0,
                                   std::uint32_t sample_rate = 1000U) {
    pvt::ClockConfig clock;
    clock.mode = pvt::ClockMode::Music;
    clock.music.analyzer_version = "test-analyzer-1";
    clock.music.source_sha256 = std::string(64U, 'a');
    clock.music.source_basename = "test-track.wav";
    clock.music.source_format = "wav-f32";
    clock.music.source_sample_rate = sample_rate;
    clock.music.source_frame_count = static_cast<std::uint64_t>(
        std::llround(duration_seconds * static_cast<double>(sample_rate)));
    clock.music.source_channel_count = 2U;
    clock.music.duration_seconds =
        static_cast<double>(clock.music.source_frame_count)
        / static_cast<double>(sample_rate);
    clock.music.detected_bpm = 120.0;
    clock.music.tempo_confidence = 0.9;
    clock.music.beat_times_seconds = {
        0.0, 0.5 * clock.music.duration_seconds};
    return clock;
}

double mean_absolute_difference(const pvt::Image& a, const pvt::Image& b) {
    CHECK(a.pixels.size() == b.pixels.size());
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) {
        return 1.0;
    }
    double total = 0.0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        total += std::abs(static_cast<double>(a.pixels[i])
                          - static_cast<double>(b.pixels[i]));
    }
    return total / static_cast<double>(a.pixels.size());
}

bool write_test_png(const fs::path& path, png_uint_32 width,
                    png_uint_32 height,
                    const std::vector<unsigned char>& pixels) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    return pixels.size() == PNG_IMAGE_SIZE(image)
           && png_image_write_to_file(&image, path.string().c_str(), 0,
                                      pixels.data(), 0, nullptr) != 0;
}

void test_starting_images_and_reusable_paths(const fs::path& directory) {
    const fs::path source = directory / "starting-image.png";
    CHECK(write_test_png(
        source, 2U, 2U,
        {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U,
         0U, 0U, 255U, 255U, 255U, 255U, 255U, 0U}));

    pvt::RenderConfig image_config = pvt::default_config();
    image_config.width = 16;
    image_config.height = 16;
    image_config.block_size = 1;
    image_config.output.write_alpha = true;
    image_config.starting_image.enabled = true;
    image_config.starting_image.path = source.string();
    image_config.starting_image.fit = pvt::StartingImageFit::Stretch;
    image_config.waves.clear();
    image_config.swings.clear();
    image_config.effects.clear();
    image_config.displacement_enabled = false;
    image_config.lighting_enabled = false;
    image_config.spiral_enabled = false;
    image_config.wall_reflection_enabled = false;
    pvt::Image stretched;
    std::string error;
    const bool stretched_ok =
        pvt::render_frame_at_phase(image_config, 0.0, stretched, &error);
    if (!stretched_ok) std::cerr << "starting-image render: " << error << '\n';
    CHECK(stretched_ok);
    CHECK(stretched.width == 16 && stretched.height == 16);
    CHECK(stretched.pixel(0, 0) != nullptr);
    if (const float* pixel = stretched.pixel(0, 0)) {
        CHECK(pixel[0] > 0.5F && pixel[1] < 0.2F && pixel[2] < 0.2F);
    }
    if (const float* pixel = stretched.pixel(15, 15)) {
        CHECK(pixel[3] < 0.5F);
    }
    image_config.starting_image.fit = pvt::StartingImageFit::Tile;
    pvt::Image tiled;
    const bool tiled_ok =
        pvt::render_frame_at_phase(image_config, 0.0, tiled, &error);
    if (!tiled_ok) std::cerr << "tiled starting-image render: " << error << '\n';
    CHECK(tiled_ok);
    if (const float* first = tiled.pixel(0, 0);
        first != nullptr && tiled.pixel(2, 0) != nullptr) {
        const float* repeated = tiled.pixel(2, 0);
        CHECK(std::equal(first, first + 4, repeated));
    }

    pvt::RenderConfig path_config = pvt::default_config();
    make_small(path_config);
    path_config.motion_paths.push_back(
        pvt::default_ellipse_path(500U, 600U, "Test ellipse"));
    path_config.waves.front().path.enabled = true;
    path_config.waves.front().path.path_id = 500U;
    path_config.waves.front().path.cycles_per_loop = 1;
    path_config.waves.front().path.follow_tangent = true;
    path_config.motion.enabled = true;
    path_config.motion.custom_path.enabled = true;
    path_config.motion.custom_path.path_id = 500U;
    path_config.motion.custom_path.cycles_per_loop = 1;
    path_config.motion.custom_path.follow_tangent = true;
    path_config.output.write_alpha = true;
    CHECK(pvt::validate(path_config).ok);
    pvt::Image start;
    pvt::Image middle;
    pvt::Image seam;
    CHECK(pvt::render_frame_at_phase(path_config, 0.0, start, &error));
    CHECK(pvt::render_frame_at_phase(path_config, 0.25, middle, &error));
    CHECK(pvt::render_frame_at_phase(path_config, 1.0, seam, &error));
    CHECK(start.pixels == seam.pixels);
    CHECK(mean_absolute_difference(start, middle) > 0.00001);

    std::string serialized;
    CHECK(pvt::detail::serialize_setup_config(path_config, serialized, &error));
    pvt::RenderConfig loaded;
    CHECK(pvt::detail::deserialize_setup_config(serialized, loaded, &error));
    CHECK(loaded.motion_paths.size() == 1U);
    CHECK(loaded.motion_paths.front().nodes.size() == 4U);
    CHECK(loaded.waves.front().path.enabled);
    CHECK(loaded.motion.custom_path.follow_tangent);
}

void test_image_access_and_transactional_render() {
    pvt::Image image;
    image.width = 2;
    image.height = 2;
    image.pixels.resize(16U);
    CHECK(image.pixel(0, 0) == image.pixels.data());
    CHECK(image.pixel(1, 1) == image.pixels.data() + 12U);
    CHECK(image.pixel(-1, 0) == nullptr);
    CHECK(image.pixel(2, 0) == nullptr);

    const pvt::Image& const_image = image;
    CHECK(const_image.pixel(1, 1) == image.pixels.data() + 12U);
    image.pixels.resize(15U);
    CHECK(image.pixel(1, 1) == nullptr);
    image.pixels.resize(17U);
    CHECK(image.pixel(0, 0) == nullptr);

    // Malformed public metadata must not wrap an index calculation into a
    // seemingly valid pointer, including on 32-bit size_t implementations.
    image.width = std::numeric_limits<int>::max();
    image.height = std::numeric_limits<int>::max();
    image.pixels.resize(4U);
    CHECK(image.pixel(image.width - 1, image.height - 1) == nullptr);

    auto config = pvt::default_config();
    make_small(config);
    pvt::Image destination;
    destination.width = 1;
    destination.height = 1;
    destination.pixels = {1.0F, 2.0F, 3.0F, 0.5F};
    const pvt::Image unchanged = destination;
    std::string error = "stale";
    config.width = 0;
    CHECK(!pvt::render_frame(config, 0, destination, &error));
    CHECK(destination.width == unchanged.width);
    CHECK(destination.height == unchanged.height);
    CHECK(destination.pixels == unchanged.pixels);
    CHECK(!error.empty());

    make_small(config);
    pvt::Image negative;
    pvt::Image final;
    CHECK(pvt::render_frame(config, -1, negative, &error));
    CHECK(error.empty());
    CHECK(pvt::render_frame(config, config.total_frames - 1, final, &error));
    CHECK(negative.pixels == final.pixels);
}

void test_cancellable_single_layer_render() {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 1024;
    config.height = 1024;
    config.block_size = 1;
    config.waves.reserve(pvt::kMaximumWaves);
    for (std::size_t index = config.waves.size();
        index < pvt::kMaximumWaves; ++index) {
        pvt::WaveConfig wave = pvt::default_wave(index);
        wave.id = static_cast<std::uint64_t>(1000U + index);
        config.waves.push_back(std::move(wave));
    }
    CHECK(pvt::validate(config).ok);

    pvt::Image destination;
    destination.width = 1;
    destination.height = 1;
    destination.pixels = {0.25F, 0.5F, 0.75F, 1.0F};
    const pvt::Image preserved = destination;
    std::atomic_bool cancel {false};
    std::atomic_bool entered {false};
    std::atomic_bool finished {false};
    bool rendered = true;
    std::string error;

    std::thread worker([&] {
        entered.store(true, std::memory_order_release);
        rendered = pvt::render_frame_at_phase_cancellable(
            config, 0.25, destination, &cancel, &error);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // This valid maximum-wave render cannot finish in this interval. Flip the
    // token only after the render thread has begun, exercising an in-flight
    // row/chunk checkpoint instead of only the entry guard.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!finished.load(std::memory_order_acquire));
    cancel.store(true, std::memory_order_relaxed);
    worker.join();

    CHECK(!rendered);
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(destination.width == preserved.width);
    CHECK(destination.height == preserved.height);
    CHECK(destination.pixels == preserved.pixels);

    make_small(config);
    config.waves.resize(3U);
    cancel.store(false, std::memory_order_relaxed);
    pvt::Image cancellable_result;
    pvt::Image legacy_result;
    CHECK(pvt::render_frame_cancellable(config, -1, cancellable_result,
                                        &cancel, &error));
    CHECK(error.empty());
    CHECK(pvt::render_frame(config, -1, legacy_result, &error));
    CHECK(cancellable_result.pixels == legacy_result.pixels);
}

void test_defaults_and_dynamic_collections() {
    auto config = pvt::default_config();
    make_small(config);
    CHECK(pvt::validate(config).ok);
    CHECK(config.waves.size() == 3);
    CHECK(config.effects.size() >= 7);
    CHECK(config.output.png_compression_level == 5);
    CHECK(pvt::default_wave().id == 0U);
    CHECK(pvt::default_swing().id == 0U);
    CHECK(pvt::default_effect(pvt::EffectType::Ripple).id == 0U);

    pvt::Image image;
    std::string error;
    CHECK(pvt::render_frame(config, 0, image, &error));
    CHECK(image.width == config.width);
    CHECK(image.height == config.height);
    CHECK(image.pixels.size() == static_cast<std::size_t>(config.width * config.height * 4));

    const pvt::Image base = image;
    for (auto& effect : config.effects) {
        if (effect.type != pvt::EffectType::Glow) {
            effect.enabled = true;
            effect.intensity = 0.0;
        }
    }
    CHECK(pvt::render_frame(config, 0, image, &error));
    CHECK(image.pixels == base.pixels);
    for (auto& effect : config.effects) {
        effect.enabled = false;
    }

    config.waves.clear();
    CHECK(pvt::validate(config).ok);
    CHECK(pvt::render_frame(config, 0, image, &error));

    for (std::size_t i = 0; i < 10; ++i) {
        auto wave = pvt::default_wave(i);
        wave.id = 1000 + i;
        wave.synchronized = (i % 2U) == 0U;
        wave.direction = static_cast<double>(i) / 9.0;
        config.waves.push_back(std::move(wave));
    }
    config.effects.clear();
    for (std::size_t i = 0; i < 10; ++i) {
        auto effect = pvt::default_effect(static_cast<pvt::EffectType>(i % 7U));
        effect.id = 2000 + i;
        effect.enabled = true;
        effect.synchronized = (i % 2U) != 0U;
        effect.edge_mode = pvt::EdgeMode::Reflect;
        effect.radius_pixels = 2.0;
        effect.magnitude = 0.005;
        config.effects.push_back(std::move(effect));
    }
    CHECK(pvt::validate(config).ok);
    CHECK(pvt::render_frame(config, 3, image, &error));
}

void test_synchronized_clocks_and_music() {
    pvt::RenderConfig config = pvt::default_config();
    make_small(config);
    std::string error;
    CHECK(config.audio_reactive.color_source == pvt::MusicFeature::Energy);

    // Default mode is the historical frame/phase mapping exactly, including
    // the existing swing stack and every later render stage.
    pvt::Image default_frame;
    pvt::Image direct_phase;
    CHECK(pvt::render_frame(config, 5, default_frame, &error));
    CHECK(pvt::render_frame_at_phase(
        config, 5.0 / static_cast<double>(config.total_frames),
        direct_phase, &error));
    CHECK(default_frame.pixels == direct_phase.pixels);

    // Free waves bypass swing, but still consume the held base clock.
    for (pvt::WaveConfig& wave : config.waves) wave.synchronized = false;
    config.clock.mode = pvt::ClockMode::Frame;
    config.clock.frame_interval = 3;
    config.clock.interpolation = pvt::ClockInterpolation::Hold;
    pvt::Image held_zero;
    pvt::Image held_one;
    pvt::Image held_two;
    pvt::Image next_pulse;
    CHECK(pvt::render_frame(config, 0, held_zero, &error));
    CHECK(pvt::render_frame(config, 1, held_one, &error));
    CHECK(pvt::render_frame(config, 2, held_two, &error));
    CHECK(pvt::render_frame(config, 3, next_pulse, &error));
    CHECK(held_zero.pixels == held_one.pixels);
    CHECK(held_zero.pixels == held_two.pixels);
    CHECK(held_zero.pixels != next_pulse.pixels);

    pvt::RenderConfig fit_clock = config;
    fit_clock.total_frames = 10;
    fit_clock.clock.frame_interval = 3;
    fit_clock.clock.fit = pvt::ClockFit::FitSequence;
    CHECK(pvt::render_frame(fit_clock, 0, held_zero, &error));
    CHECK(pvt::render_frame(fit_clock, 3, held_one, &error));
    CHECK(held_zero.pixels == held_one.pixels);
    fit_clock.clock.fit = pvt::ClockFit::Exact;
    CHECK(pvt::render_frame(fit_clock, 3, next_pulse, &error));
    CHECK(held_zero.pixels != next_pulse.pixels);

    config.clock.interpolation = pvt::ClockInterpolation::Linear;
    pvt::Image frame_linear;
    CHECK(pvt::render_frame(config, 1, frame_linear, &error));
    CHECK(pvt::render_frame_at_phase(
        config, 1.0 / static_cast<double>(config.total_frames),
        direct_phase, &error));
    CHECK(frame_linear.pixels == direct_phase.pixels);
    config.clock.interpolation = pvt::ClockInterpolation::Smoothstep;
    pvt::Image frame_smooth;
    CHECK(pvt::render_frame(config, 1, frame_smooth, &error));
    CHECK(frame_smooth.pixels != frame_linear.pixels);

    // Elapsed-time pulses are FPS-independent: both frames below represent
    // the same 125 ms instant in equal-duration sequences.
    config.clock.mode = pvt::ClockMode::Time;
    config.clock.interpolation = pvt::ClockInterpolation::Hold;
    config.clock.time_interval_microseconds = 125000;
    pvt::Image time_24;
    CHECK(pvt::render_frame(config, 3, time_24, &error));
    pvt::RenderConfig high_fps = config;
    high_fps.fps = 48.0;
    high_fps.total_frames = 24;
    pvt::Image time_48;
    CHECK(pvt::render_frame(high_fps, 6, time_48, &error));
    CHECK(time_24.pixels == time_48.pixels);
    CHECK(pvt::render_frame(config, 4, held_one, &error));
    CHECK(time_24.pixels == held_one.pixels);

    std::string meter_description;
    CHECK(pvt::describe_meter("7/8", meter_description, &error));
    CHECK(meter_description.find("7 pulses") != std::string::npos);
    CHECK(pvt::describe_meter("3+2+3/8 | 5/4", meter_description, &error));
    CHECK(pvt::describe_meter("4/3 | 6/7", meter_description, &error));
    CHECK(!pvt::describe_meter("3++2/8", meter_description, &error));
    CHECK(!pvt::describe_meter("7/0", meter_description, &error));
    CHECK(!pvt::describe_meter("7/8 |", meter_description, &error));

    config.clock.mode = pvt::ClockMode::Meter;
    config.clock.interpolation = pvt::ClockInterpolation::Hold;
    config.clock.meter.expression = "7/8";
    config.clock.meter.bpm = 120.0;
    config.clock.meter.tempo_note_denominator = 4;
    CHECK(pvt::render_frame(config, 0, held_zero, &error));
    CHECK(pvt::render_frame(config, 5, held_one, &error));
    CHECK(pvt::render_frame(config, 6, next_pulse, &error));
    CHECK(held_zero.pixels == held_one.pixels);
    CHECK(held_zero.pixels != next_pulse.pixels);

    pvt::RenderConfig music = pvt::default_config();
    make_small(music);
    music.fps = 10.0;
    music.clock = ready_music_clock();
    music.clock.interpolation = pvt::ClockInterpolation::Linear;
    music.clock.music.beat_times_seconds = {0.1, 0.3, 0.9};
    CHECK(pvt::validate(music).ok);
    CHECK(pvt::effective_frame_count(music, &error) == 10);
    CHECK(music.total_frames == 12);

    // Beat ordinal, not wall-clock fraction, drives music phase. These two
    // instants are midpoints of differently sized beat intervals.
    pvt::Image music_frame;
    pvt::Image expected_music_phase;
    CHECK(pvt::render_frame(music, 2, music_frame, &error));
    CHECK(pvt::render_frame_at_phase(music, 0.375,
                                     expected_music_phase, &error));
    CHECK(music_frame.pixels == expected_music_phase.pixels);
    CHECK(pvt::render_frame(music, 6, music_frame, &error));
    CHECK(pvt::render_frame_at_phase(music, 0.625,
                                     expected_music_phase, &error));
    CHECK(music_frame.pixels == expected_music_phase.pixels);

    music.clock.interpolation = pvt::ClockInterpolation::Hold;
    CHECK(pvt::render_frame(music, 2, music_frame, &error));
    CHECK(pvt::render_frame_at_phase(music, 0.25,
                                     expected_music_phase, &error));
    CHECK(music_frame.pixels == expected_music_phase.pixels);

    // The dense opt-in audio envelope remains accurate between beat anchors,
    // even while Hold freezes the base motion clock.
    music.clock.music.beat_times_seconds = {0.0, 1.0};
    music.clock.music.feature_samples.assign(11U, {});
    music.clock.music.feature_samples[5U].energy = 1.0F;
    music.audio_reactive.enabled = true;
    music.audio_reactive.waves_enabled = false;
    music.audio_reactive.effects_enabled = false;
    music.audio_reactive.color_enabled = true;
    music.audio_reactive.color_source = pvt::MusicFeature::Energy;
    music.audio_reactive.color_amount_degrees = 180.0;
    pvt::Image before_spike;
    pvt::Image at_spike;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    // Hue response is applied to the selected source color, so even a fixed
    // one-color starting palette must visibly follow the music envelope.
    music.palette.enabled = true;
    music.palette.name = "Audio hue regression";
    music.palette.colors = {{1.0, 0.0, 0.0}};
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);
    music.palette = {};

    music.audio_reactive.color_enabled = false;
    music.audio_reactive.waves_enabled = true;
    music.audio_reactive.wave_source = pvt::MusicFeature::Energy;
    music.audio_reactive.wave_amount = 1.0;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    music.audio_reactive.waves_enabled = false;
    music.audio_reactive.effects_enabled = true;
    music.audio_reactive.effect_source = pvt::MusicFeature::Energy;
    music.audio_reactive.effect_amount = 1.0;
    const auto ripple = std::find_if(
        music.effects.begin(), music.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::Ripple;
        });
    CHECK(ripple != music.effects.end());
    if (ripple != music.effects.end()) ripple->enabled = true;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    // The master toggle is authoritative in Music mode. Legacy policy values
    // remain loadable but no longer suppress authored swings behind the GUI's
    // checkbox.
    music.audio_reactive.enabled = false;
    music.clock.interpolation = pvt::ClockInterpolation::Linear;
    music.clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
    pvt::Image kept_swing;
    pvt::Image legacy_policy_swing;
    CHECK(music.swings.front().enabled);
    CHECK(pvt::render_frame(music, 3, kept_swing, &error));
    music.clock.music_swing_policy = pvt::MusicSwingPolicy::SuppressAll;
    CHECK(pvt::render_frame(music, 3, legacy_policy_swing, &error));
    CHECK(kept_swing.pixels == legacy_policy_swing.pixels);
    CHECK(music.swings.front().enabled);
    music.swings.front().radius = 0.5;
    music.clock.music_swing_policy = pvt::MusicSwingPolicy::SuppressGlobal;
    CHECK(pvt::render_frame(music, 3, kept_swing, &error));
    music.clock.music_swing_policy = pvt::MusicSwingPolicy::SuppressAll;
    CHECK(pvt::render_frame(music, 3, legacy_policy_swing, &error));
    CHECK(kept_swing.pixels == legacy_policy_swing.pixels);
    music.swings_enabled = false;
    CHECK(pvt::render_frame(music, 3, direct_phase, &error));
    CHECK(direct_phase.pixels != legacy_policy_swing.pixels);
    CHECK(music.swings.front().enabled);

    pvt::RenderConfig duration = music;
    duration.clock = ready_music_clock(1.01);
    duration.fps = 24.0;
    CHECK(pvt::effective_frame_count(duration, &error) == 25);
    CHECK(duration.total_frames == 12);

    pvt::RenderConfig invalid = pvt::default_config();
    make_small(invalid);
    invalid.clock.mode = pvt::ClockMode::Frame;
    invalid.clock.frame_interval = 0;
    CHECK(!pvt::validate(invalid).ok);
    invalid = pvt::default_config();
    make_small(invalid);
    invalid.clock.mode = pvt::ClockMode::Time;
    invalid.clock.time_interval_microseconds = 0;
    CHECK(!pvt::validate(invalid).ok);
    invalid = pvt::default_config();
    make_small(invalid);
    invalid.clock.mode = pvt::ClockMode::Meter;
    invalid.clock.meter.expression = "3+0/8";
    CHECK(!pvt::validate(invalid).ok);
    invalid = pvt::default_config();
    make_small(invalid);
    invalid.clock.mode = pvt::ClockMode::Music;
    CHECK(!pvt::validate(invalid).ok);

    CHECK(std::string(pvt::clock_mode_name(pvt::ClockMode::Music)) == "Music");
    CHECK(std::string(pvt::clock_interpolation_name(
              pvt::ClockInterpolation::Smoothstep)) == "Smoothstep");
    CHECK(std::string(pvt::music_feature_name(pvt::MusicFeature::Bass))
          == "Bass");
}

void test_determinism_and_seam_continuity() {
    auto config = pvt::default_config();
    make_small(config);
    for (auto& effect : config.effects) {
        effect.enabled = true;
        effect.edge_mode = pvt::EdgeMode::Reflect;
        effect.radius_pixels = 3.0;
        effect.magnitude = 0.01;
    }
    config.alpha.enabled = true;

    pvt::Image a;
    pvt::Image b;
    pvt::Image before;
    pvt::Image after;
    std::string error;
    CHECK(pvt::render_frame_at_phase(config, 0.3125, a, &error));
    CHECK(pvt::render_frame_at_phase(config, 0.3125, b, &error));
    CHECK(a.pixels == b.pixels);

    constexpr double epsilon = 1.0e-6;
    CHECK(pvt::render_frame_at_phase(config, 1.0 - epsilon, before, &error));
    CHECK(pvt::render_frame_at_phase(config, epsilon, after, &error));
    CHECK(mean_absolute_difference(before, after) < 0.01);
    CHECK(pvt::render_frame_at_phase(config, 0.0, before, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0, after, &error));
    CHECK(before.pixels == after.pixels);

    // Every effect type closes its loop with either synchronization mode.
    for (int raw_type = static_cast<int>(pvt::EffectType::EndlessZoom);
         raw_type <= static_cast<int>(pvt::EffectType::ParticleField); ++raw_type) {
        for (const bool synchronized : {false, true}) {
            auto one_effect = pvt::default_config();
            make_small(one_effect);
            one_effect.effects.clear();
            auto effect = pvt::default_effect(static_cast<pvt::EffectType>(raw_type));
            effect.id = pvt::allocate_id(one_effect);
            effect.enabled = true;
            effect.synchronized = synchronized;
            effect.cycles_per_loop = 3;
            effect.edge_mode = pvt::EdgeMode::Reflect;
            effect.radius_pixels = 2.0;
            effect.magnitude = 0.01;
            one_effect.effects.push_back(effect);
            CHECK(pvt::render_frame_at_phase(one_effect, 0.0, before, &error));
            CHECK(pvt::render_frame_at_phase(one_effect, 1.0, after, &error));
            CHECK(before.pixels == after.pixels);
            CHECK(pvt::render_frame_at_phase(one_effect, 1.0 - epsilon, before, &error));
            CHECK(pvt::render_frame_at_phase(one_effect, epsilon, after, &error));
            CHECK(mean_absolute_difference(before, after) < 0.01);
        }
    }
}

void test_direction_alpha_and_surfaces(const fs::path& source_root) {
    auto config = pvt::default_config();
    make_small(config);
    config.waves.resize(1);
    config.effects.clear();
    config.alpha.enabled = true;
    config.alpha.minimum = 0.0;
    config.alpha.maximum = 1.0;
    config.alpha.spatial_frequency = 3.0;

    pvt::Image horizontal;
    pvt::Image radial;
    pvt::Image vertical;
    std::string error;
    config.waves[0].direction = 0.0;
    CHECK(pvt::render_frame(config, 2, horizontal, &error));
    config.waves[0].direction = 0.5;
    CHECK(pvt::render_frame(config, 2, radial, &error));
    config.waves[0].direction = 1.0;
    CHECK(pvt::render_frame(config, 2, vertical, &error));
    CHECK(mean_absolute_difference(horizontal, radial) > 0.001);
    CHECK(mean_absolute_difference(radial, vertical) > 0.001);

    float minimum_alpha = 1.0F;
    float maximum_alpha = 0.0F;
    for (std::size_t i = 3; i < radial.pixels.size(); i += 4) {
        minimum_alpha = std::min(minimum_alpha, radial.pixels[i]);
        maximum_alpha = std::max(maximum_alpha, radial.pixels[i]);
    }
    CHECK(minimum_alpha < 0.15F);
    CHECK(maximum_alpha > 0.85F);

    // Straight alpha keeps procedural RGB meaningful even at zero coverage.
    config.alpha.minimum = 0.0;
    config.alpha.maximum = 0.0;
    auto shake = pvt::default_effect(pvt::EffectType::Shake);
    shake.id = pvt::allocate_id(config);
    shake.enabled = true;
    shake.edge_mode = pvt::EdgeMode::Reflect;
    config.effects.push_back(shake);
    CHECK(pvt::render_frame(config, 2, radial, &error));
    bool found_transparent_color = false;
    for (std::size_t i = 0; i < radial.pixels.size(); i += 4) {
        CHECK(radial.pixels[i + 3U] == 0.0F);
        found_transparent_color = found_transparent_color
                                  || radial.pixels[i] > 0.001F
                                  || radial.pixels[i + 1U] > 0.001F
                                  || radial.pixels[i + 2U] > 0.001F;
    }
    CHECK(found_transparent_color);
    config.effects.clear();

    config.alpha.enabled = false;
    config.surface.enabled = false;
    pvt::Image unmapped;
    CHECK(pvt::render_frame(config, 1, unmapped, &error));

    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Plane;
    config.surface.curvature = 0.0; // Plane intentionally ignores curvature.
    config.surface.rotations_per_loop = 0;
    config.surface.phase_degrees = 720.0;
    CHECK(pvt::render_frame(config, 1, radial, &error));
    CHECK(radial.pixels == unmapped.pixels);

    config.surface.rotations_per_loop = 1;
    config.surface.phase_degrees = 0.0;
    CHECK(pvt::render_frame(config, 1, radial, &error));
    CHECK(mean_absolute_difference(radial, unmapped) > 0.001);

    // Zero curvature is the neutral value for primitive wrapping. It must not
    // crop, shade, or otherwise change the planar source image.
    config.surface.curvature = 0.0;
    config.surface.lighting = 2.0;
    config.surface.rotations_per_loop = 3;
    config.surface.phase_degrees = 37.0;
    for (const auto mapping : {pvt::SurfaceMapping::Cylinder,
                               pvt::SurfaceMapping::Sphere,
                               pvt::SurfaceMapping::Cube}) {
        config.surface.mapping = mapping;
        CHECK(pvt::render_frame(config, 1, radial, &error));
        CHECK(radial.pixels == unmapped.pixels);
    }

    // Curvature is a continuous planar-to-primitive interpolation. A tiny
    // positive value must remain close to the exact zero-curvature image,
    // including outside the primitive mask and in cube lighting.
    config.surface.curvature = 1.0e-6;
    for (const auto mapping : {pvt::SurfaceMapping::Cylinder,
                               pvt::SurfaceMapping::Sphere,
                               pvt::SurfaceMapping::Cube}) {
        config.surface.mapping = mapping;
        CHECK(pvt::render_frame(config, 1, radial, &error));
        CHECK(mean_absolute_difference(radial, unmapped) < 1.0e-4);
    }

    config.surface.curvature = 1.0;
    config.surface.lighting = 0.35;
    for (const auto mapping : {pvt::SurfaceMapping::Plane,
                               pvt::SurfaceMapping::Cylinder,
                               pvt::SurfaceMapping::Sphere,
                               pvt::SurfaceMapping::Cube}) {
        config.surface.mapping = mapping;
        CHECK(pvt::render_frame(config, 1, radial, &error));
        CHECK(radial.pixels.size()
              == static_cast<std::size_t>(config.width * config.height * 4));
        if (mapping != pvt::SurfaceMapping::Plane) {
            bool has_transparent_exterior = false;
            for (std::size_t offset = 3U; offset < radial.pixels.size(); offset += 4U) {
                has_transparent_exterior = has_transparent_exterior
                                           || radial.pixels[offset] == 0.0F;
            }
            CHECK(has_transparent_exterior);
        }
    }

    // A transparent primitive is a shell, not a nearest-hit cardboard mask.
    // At its center a ray crosses the front and rear surfaces, so two 0.5-alpha
    // samples must composite to 0.75 coverage for every closed primitive.
    config.alpha.enabled = true;
    config.alpha.minimum = 0.5;
    config.alpha.maximum = 0.5;
    config.surface.lighting = 0.0;
    for (const auto mapping : {pvt::SurfaceMapping::Cylinder,
                               pvt::SurfaceMapping::Sphere,
                               pvt::SurfaceMapping::Cube}) {
        config.surface.mapping = mapping;
        config.alpha.enabled = false;
        pvt::Image opaque_surface;
        CHECK(pvt::render_frame(config, 1, opaque_surface, &error));
        config.alpha.enabled = true;
        CHECK(pvt::render_frame(config, 1, radial, &error));
        const float* center = radial.pixel(config.width / 2, config.height / 2);
        CHECK(center != nullptr);
        if (center != nullptr) {
            CHECK(std::abs(center[3] - 0.75F) < 1.0e-5F);
        }
        // Alpha alone could be faked by compositing the nearest sample twice.
        // Requiring an RGB difference from the opaque nearest-hit image proves
        // that a distinct UV/color from the exit surface was sampled.
        bool sampled_distinct_rear_color = false;
        for (std::size_t offset = 0U; offset < radial.pixels.size(); offset += 4U) {
            if (radial.pixels[offset + 3U] < 0.7F) {
                continue;
            }
            sampled_distinct_rear_color = sampled_distinct_rear_color
                || std::abs(radial.pixels[offset]
                            - opaque_surface.pixels[offset]) > 1.0e-4F
                || std::abs(radial.pixels[offset + 1U]
                            - opaque_surface.pixels[offset + 1U]) > 1.0e-4F
                || std::abs(radial.pixels[offset + 2U]
                            - opaque_surface.pixels[offset + 2U]) > 1.0e-4F;
        }
        CHECK(sampled_distinct_rear_color);
    }

    // Exercise the public RenderConfig dispatch, not only the isolated OBJ
    // rasterizer. Custom meshes participate in the same transparent-shell
    // behavior and central validation as built-in surfaces.
    config.surface.mapping = pvt::SurfaceMapping::CustomObj;
    config.surface.obj_path =
        (source_root / "tests" / "assets" / "obj" / "closed_cube.obj").string();
    CHECK(pvt::validate(config).ok);
    CHECK(pvt::render_frame(config, 1, radial, &error));
    const float* obj_center = radial.pixel(config.width / 2, config.height / 2);
    CHECK(obj_center != nullptr);
    if (obj_center != nullptr) {
        CHECK(obj_center[3] > 0.72F && obj_center[3] < 0.78F);
    }
    config.surface.obj_path.clear();
    CHECK(!pvt::validate(config).ok);
    config.surface.obj_path =
        (source_root / "tests" / "assets" / "obj" / "closed_cube.obj").string();

    // Lighting scales neutral channels uniformly. This protects primitive
    // shading from introducing a channel-specific tint.
    config.saturation = 0.0;
    config.surface.mapping = pvt::SurfaceMapping::Sphere;
    CHECK(pvt::render_frame(config, 1, radial, &error));
    for (std::size_t offset = 0; offset < radial.pixels.size(); offset += 4U) {
        CHECK(std::abs(radial.pixels[offset] - radial.pixels[offset + 1U]) < 1.0e-6F);
        CHECK(std::abs(radial.pixels[offset + 1U] - radial.pixels[offset + 2U])
              < 1.0e-6F);
    }
}

void test_partial_alpha_glow_composition() {
    auto config = pvt::default_config();
    make_small(config);
    config.width = 24;
    config.height = 24;
    config.block_size = 1;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.hue_cycles = 0;
    config.alpha.enabled = true;
    config.alpha.minimum = 0.5;
    config.alpha.maximum = 0.5;

    pvt::Image base;
    pvt::Image glowing;
    std::string error;
    CHECK(pvt::render_frame_at_phase(config, 0.0, base, &error));

    auto glow = pvt::default_effect(pvt::EffectType::Glow);
    glow.id = pvt::allocate_id(config);
    glow.enabled = true;
    glow.intensity = 1.0;
    glow.secondary = 0.0;
    glow.radius_pixels = 1.0;
    glow.threshold = 0.0;
    glow.soft_knee = 0.0;
    config.effects.push_back(glow);
    CHECK(pvt::render_frame_at_phase(config, 0.0, glowing, &error));

    const float* base_pixel = base.pixel(12, 12);
    const float* glow_pixel = glowing.pixel(12, 12);
    CHECK(base_pixel != nullptr);
    CHECK(glow_pixel != nullptr);
    if (base_pixel != nullptr && glow_pixel != nullptr) {
        CHECK(std::abs(glow_pixel[3] - 0.75F) < 1.0e-5F);
        int checked_color_channels = 0;
        for (int channel = 0; channel < 3; ++channel) {
            if (base_pixel[channel] > 1.0e-6F) {
                CHECK(std::abs(glow_pixel[channel] / base_pixel[channel]
                               - (4.0F / 3.0F)) < 1.0e-4F);
                ++checked_color_channels;
            }
        }
        CHECK(checked_color_channels >= 2);
    }
}

void test_particle_straight_alpha_emission() {
    auto config = pvt::default_config();
    make_small(config);
    config.width = 32;
    config.height = 32;
    config.block_size = 1;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.hue_cycles = 0;
    config.palette.enabled = true;
    config.palette.colors = {{0.0, 0.0, 0.0}};
    config.alpha.enabled = true;
    config.alpha.minimum = 0.0;
    config.alpha.maximum = 0.0;
    config.output.write_alpha = true;

    auto particles = pvt::default_effect(pvt::EffectType::ParticleField);
    particles.id = pvt::allocate_id(config);
    particles.enabled = true;
    particles.intensity = 1.0;
    particles.magnitude = 0.0;
    particles.frequency = 1.0;
    particles.secondary = 0.0;
    particles.radius_pixels = 4.0;
    particles.threshold = 0.5;
    config.effects.push_back(particles);

    pvt::Image image;
    std::string error;
    CHECK(pvt::render_frame_at_phase(config, 0.25, image, &error));
    bool found_soft_particle_edge = false;
    for (std::size_t offset = 0U; offset < image.pixels.size(); offset += 4U) {
        const float alpha = image.pixels[offset + 3U];
        if (alpha > 0.02F && alpha < 0.20F) {
            found_soft_particle_edge = true;
            // Straight RGB keeps the full spark color. Multiplying RGB by the
            // same coverage here would make later compositing apply alpha twice.
            CHECK(image.pixels[offset] >= 1.19F);
            CHECK(std::isfinite(image.pixels[offset]));
            CHECK(std::isfinite(image.pixels[offset + 1U]));
            CHECK(std::isfinite(image.pixels[offset + 2U]));
        }
    }
    CHECK(found_soft_particle_edge);
}

void test_block_scale_and_default_glow_visibility() {
    auto glow_config = pvt::default_config();
    make_small(glow_config);
    for (auto& effect : glow_config.effects) {
        effect.enabled = false;
    }
    pvt::Image base;
    pvt::Image glowing;
    std::string error;
    CHECK(pvt::render_frame_at_phase(glow_config, 0.25, base, &error));
    const auto glow = std::find_if(
        glow_config.effects.begin(), glow_config.effects.end(),
        [](const pvt::EffectConfig& effect) {
            return effect.type == pvt::EffectType::Glow;
        });
    CHECK(glow != glow_config.effects.end());
    if (glow != glow_config.effects.end()) {
        glow->enabled = true;
    }
    CHECK(pvt::render_frame_at_phase(glow_config, 0.25, glowing, &error));
    const double default_glow_difference = mean_absolute_difference(base, glowing);
    CHECK(default_glow_difference > 0.02);
    CHECK(default_glow_difference < 0.15);

    // Make a static but spatially varied source so any changes across sampled
    // phases come from BlockScale alone rather than from the underlying waves.
    auto config = pvt::default_config();
    make_small(config);
    config.block_size = 1;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.hue_cycles = 0;
    config.alpha.enabled = true;
    config.alpha.minimum = 0.0;
    config.alpha.maximum = 1.0;
    config.alpha.spatial_frequency = 3.0;
    config.alpha.cycles_per_loop = 0;

    CHECK(pvt::render_frame_at_phase(config, 0.0, base, &error));
    pvt::Image static_check;
    CHECK(pvt::render_frame_at_phase(config, 0.37, static_check, &error));
    CHECK(base.pixels == static_check.pixels);

    auto block_scale = pvt::default_effect(pvt::EffectType::BlockScale);
    block_scale.id = pvt::allocate_id(config);
    block_scale.enabled = true;
    block_scale.synchronized = false;
    block_scale.intensity = 0.0;
    block_scale.magnitude = 1.0;
    block_scale.frequency = 8.0;
    config.effects.push_back(block_scale);
    pvt::Image effected;
    CHECK(pvt::render_frame_at_phase(config, 0.5, effected, &error));
    CHECK(base.pixels == effected.pixels); // Zero mix is exactly neutral.

    config.effects[0].intensity = 1.0;
    config.effects[0].frequency = 1.0;
    CHECK(pvt::render_frame_at_phase(config, 0.5, effected, &error));
    CHECK(base.pixels == effected.pixels); // A one-pixel group is exactly neutral.

    config.effects[0].frequency = 8.0;
    CHECK(pvt::render_frame_at_phase(config, 0.5, effected, &error));
    CHECK(mean_absolute_difference(base, effected) > 0.005);
    pvt::Image seam;
    CHECK(pvt::render_frame_at_phase(config, 0.0, effected, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0, seam, &error));
    CHECK(effected.pixels == seam.pixels);

    config.effects[0].secondary = 2.0;
    std::set<std::vector<float>> quantized_frames;
    for (int sample = 0; sample <= 16; ++sample) {
        CHECK(pvt::render_frame_at_phase(
            config, static_cast<double>(sample) / 16.0, effected, &error));
        quantized_frames.insert(effected.pixels);
    }
    CHECK(quantized_frames.size() == 3U);

    config.effects[0].secondary = 0.0;
    std::set<std::vector<float>> smooth_frames;
    for (int sample = 0; sample <= 16; ++sample) {
        CHECK(pvt::render_frame_at_phase(
            config, static_cast<double>(sample) / 16.0, effected, &error));
        smooth_frames.insert(effected.pixels);
    }
    CHECK(smooth_frames.size() > quantized_frames.size());

    // The effect consumes the image at its exact stack position: warping
    // grouped pixels is observably different from grouping warped pixels.
    config.effects[0].magnitude = 8.0;
    config.effects[0].frequency = 8.0;
    auto ripple = pvt::default_effect(pvt::EffectType::Ripple);
    ripple.id = config.effects[0].id + 1U;
    ripple.enabled = true;
    ripple.synchronized = false;
    ripple.intensity = 1.0;
    ripple.magnitude = 0.08;
    ripple.edge_mode = pvt::EdgeMode::Reflect;
    config.effects.push_back(ripple);
    pvt::Image block_then_ripple;
    pvt::Image ripple_then_block;
    CHECK(pvt::render_frame_at_phase(config, 0.25, block_then_ripple, &error));
    std::swap(config.effects[0], config.effects[1]);
    CHECK(pvt::render_frame_at_phase(config, 0.25, ripple_then_block, &error));
    CHECK(mean_absolute_difference(block_then_ripple, ripple_then_block) > 0.001);
}

void test_palettes_transforms_and_spatial_stages() {
    auto config = pvt::default_config();
    make_small(config);
    config.effects.clear();
    config.palette.enabled = false;
    config.transform = {};
    std::string error;
    pvt::Image baseline;
    CHECK(pvt::render_frame_at_phase(config, 0.371, baseline, &error));

    // A populated but disabled palette is a complete render bypass. This
    // locks down the GUI/CLI toggle contract independently of persistence.
    config.palette.name = "Ignored while disabled";
    config.palette.colors = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    pvt::Image disabled_palette;
    CHECK(pvt::render_frame_at_phase(config, 0.371, disabled_palette, &error));
    CHECK(disabled_palette.pixels == baseline.pixels);

    config.transform.flip_horizontal = true;
    pvt::Image flipped;
    CHECK(pvt::render_frame_at_phase(config, 0.371, flipped, &error));
    for (int y = 0; y < baseline.height; ++y) {
        for (int x = 0; x < baseline.width; ++x) {
            const float* expected = baseline.pixel(baseline.width - 1 - x, y);
            const float* actual = flipped.pixel(x, y);
            CHECK(expected != nullptr && actual != nullptr);
            if (expected != nullptr && actual != nullptr) {
                CHECK(std::equal(expected, expected + 4, actual));
            }
        }
    }

    config.transform = {};
    config.transform.mirror = pvt::MirrorMode::LeftToRight;
    pvt::Image mirrored;
    CHECK(pvt::render_frame_at_phase(config, 0.371, mirrored, &error));
    for (int y = 0; y < baseline.height; ++y) {
        for (int x = 0; x < baseline.width; ++x) {
            const int source_x = x < (baseline.width + 1) / 2
                                     ? x : baseline.width - 1 - x;
            const float* expected = baseline.pixel(source_x, y);
            const float* actual = mirrored.pixel(x, y);
            CHECK(expected != nullptr && actual != nullptr);
            if (expected != nullptr && actual != nullptr) {
                CHECK(std::equal(expected, expected + 4, actual));
            }
        }
    }

    config.transform = {};
    config.palette.enabled = true;
    config.palette.name = "Binary";
    config.palette.colors = {{0.0, 0.0, 0.0}, {1.0, 1.0, 1.0}};
    config.lighting_enabled = false;
    pvt::Image paletted;
    CHECK(pvt::render_frame_at_phase(config, 0.371, paletted, &error));
    for (std::size_t offset = 0U; offset < paletted.pixels.size(); offset += 4U) {
        const bool black = paletted.pixels[offset] == 0.0F
                           && paletted.pixels[offset + 1U] == 0.0F
                           && paletted.pixels[offset + 2U] == 0.0F;
        const bool white = paletted.pixels[offset] == 1.0F
                           && paletted.pixels[offset + 1U] == 1.0F
                           && paletted.pixels[offset + 2U] == 1.0F;
        CHECK(black || white);
    }
    pvt::Image palette_seam_start;
    pvt::Image palette_seam;
    CHECK(pvt::render_frame_at_phase(config, 0.0, palette_seam_start, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0, palette_seam, &error));
    CHECK(palette_seam_start.pixels == palette_seam.pixels);
    CHECK(!pvt::default_palette(0U).colors.empty());
    CHECK(pvt::default_palette(0U).name != pvt::default_palette(1U).name);

    // Slope lighting follows source-color selection, so a starting palette
    // does not make that independent layer feature inert.
    config.lighting_enabled = true;
    pvt::Image slope_lit_palette;
    CHECK(pvt::render_frame_at_phase(
        config, 0.371, slope_lit_palette, &error));
    CHECK(mean_absolute_difference(paletted, slope_lit_palette) > 0.0001);
    config.lighting_enabled = false;

    // Palette colors seed the procedural source; they are not a final color
    // lock. Glow is therefore allowed to create brighter, off-palette values.
    auto glow = pvt::default_effect(pvt::EffectType::Glow);
    glow.id = pvt::allocate_id(config);
    glow.enabled = true;
    glow.synchronized = false;
    glow.intensity = 1.0;
    glow.secondary = 0.0;
    glow.radius_pixels = 4.0;
    glow.threshold = 0.0;
    glow.soft_knee = 0.0;
    config.effects.push_back(glow);
    pvt::Image glowed;
    CHECK(pvt::render_frame_at_phase(config, 0.371, glowed, &error));
    bool glow_created_off_palette_color = false;
    for (std::size_t offset = 0U; offset < glowed.pixels.size(); offset += 4U) {
        const auto is_binary = [](float value) {
            return value == 0.0F || value == 1.0F;
        };
        glow_created_off_palette_color = glow_created_off_palette_color
                                         || !is_binary(glowed.pixels[offset])
                                         || !is_binary(glowed.pixels[offset + 1U])
                                         || !is_binary(glowed.pixels[offset + 2U]);
    }
    CHECK(glow_created_off_palette_color);

    // Explicit quantization remains the final color-reduction stage.
    config.quantization.enabled = true;
    config.quantization.levels = 2;
    config.quantization.mix = 1.0;
    config.quantization.mode = pvt::QuantizationMode::Rgb;
    pvt::Image explicitly_quantized;
    CHECK(pvt::render_frame_at_phase(
        config, 0.371, explicitly_quantized, &error));
    for (std::size_t offset = 0U; offset < explicitly_quantized.pixels.size();
         offset += 4U) {
        const auto is_binary = [](float value) {
            return value == 0.0F || value == 1.0F;
        };
        CHECK(is_binary(explicitly_quantized.pixels[offset]));
        CHECK(is_binary(explicitly_quantized.pixels[offset + 1U]));
        CHECK(is_binary(explicitly_quantized.pixels[offset + 2U]));
    }
    config.palette.colors.clear();
    CHECK(!pvt::validate(config).ok);

    // Localized shake leaves pixels outside its feathered circle untouched,
    // while the center is visibly transformed and still closes at the seam.
    config = pvt::default_config();
    make_small(config);
    config.effects.clear();
    config.swings.clear();
    CHECK(pvt::render_frame_at_phase(config, 0.371, baseline, &error));
    auto shake = pvt::default_effect(pvt::EffectType::Shake);
    shake.id = pvt::allocate_id(config);
    shake.enabled = true;
    shake.synchronized = false;
    shake.intensity = 1.0;
    shake.magnitude = 0.12;
    shake.area_radius = 0.22;
    shake.center_x = 0.5;
    shake.center_y = 0.5;
    shake.edge_mode = pvt::EdgeMode::Reflect;
    config.effects.push_back(shake);
    pvt::Image localized;
    CHECK(pvt::render_frame_at_phase(config, 0.371, localized, &error));
    CHECK(std::equal(baseline.pixels.begin(), baseline.pixels.begin() + 4,
                     localized.pixels.begin()));
    CHECK(mean_absolute_difference(baseline, localized) > 0.0001);
    CHECK(pvt::render_frame_at_phase(config, 0.0, localized, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0, palette_seam, &error));
    CHECK(localized.pixels == palette_seam.pixels);

    // The same effect produces a distinct result on either side of surface
    // mapping: mapped-object space moves the complete primitive silhouette.
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Sphere;
    config.surface.curvature = 1.0;
    config.effects.front().area_radius = 0.0;
    config.effects.front().space = pvt::EffectSpace::Texture;
    pvt::Image texture_stage;
    pvt::Image surface_stage;
    CHECK(pvt::render_frame_at_phase(config, 0.371, texture_stage, &error));
    config.effects.front().space = pvt::EffectSpace::Surface;
    CHECK(pvt::render_frame_at_phase(config, 0.371, surface_stage, &error));
    CHECK(mean_absolute_difference(texture_stage, surface_stage) > 0.0001);
    bool silhouette_moved = false;
    for (std::size_t offset = 3U; offset < texture_stage.pixels.size();
         offset += 4U) {
        silhouette_moved = silhouette_moved
                           || texture_stage.pixels[offset]
                                  != surface_stage.pixels[offset];
    }
    CHECK(silhouette_moved);

    // Layer transforms define the mapped-object effect canvas. A localized
    // post-surface effect therefore stays under its visible handle instead of
    // being mirrored to the opposite side after the effect is evaluated.
    config.transform.flip_horizontal = true;
    config.effects.front().center_x = 0.2;
    config.effects.front().center_y = 0.5;
    config.effects.front().area_radius = 0.18;
    config.effects.front().enabled = false;
    pvt::Image transformed_without_effect;
    pvt::Image transformed_with_effect;
    CHECK(pvt::render_frame_at_phase(
        config, 0.371, transformed_without_effect, &error));
    config.effects.front().enabled = true;
    CHECK(pvt::render_frame_at_phase(
        config, 0.371, transformed_with_effect, &error));
    double left_difference = 0.0;
    double right_difference = 0.0;
    for (int y = 0; y < transformed_with_effect.height; ++y) {
        for (int x = 0; x < transformed_with_effect.width; ++x) {
            const float* before = transformed_without_effect.pixel(x, y);
            const float* after = transformed_with_effect.pixel(x, y);
            CHECK(before != nullptr && after != nullptr);
            if (before == nullptr || after == nullptr) continue;
            double& difference = x < transformed_with_effect.width / 2
                                     ? left_difference : right_difference;
            for (int channel = 0; channel < 4; ++channel) {
                difference += std::fabs(
                    static_cast<double>(before[channel] - after[channel]));
            }
        }
    }
    CHECK(left_difference > 0.0001);
    CHECK(left_difference > right_difference * 10.0 + 1.0e-6);

    // Spatial swings now modulate the clock at render coordinates rather than
    // pretending a center/radius can be represented by one global scalar.
    config = pvt::default_config();
    make_small(config);
    config.effects.clear();
    config.swings.clear();
    CHECK(pvt::render_frame_at_phase(config, 0.371, baseline, &error));
    auto swing = pvt::default_swing(0U);
    swing.id = pvt::allocate_id(config);
    swing.amount = 1.0;
    swing.center_x = 0.5;
    swing.center_y = 0.5;
    swing.radius = 0.22;
    config.swings.push_back(swing);
    CHECK(pvt::render_frame_at_phase(config, 0.371, localized, &error));
    CHECK(std::equal(baseline.pixels.begin(), baseline.pixels.begin() + 4,
                     localized.pixels.begin()));
    CHECK(mean_absolute_difference(baseline, localized) > 0.0001);

    // A localized Swing is a source/UV clock. Texture effects sample it at
    // their source center, while mapped-object effects deliberately use only
    // the global synchronized clock because screen points cannot be mapped
    // uniquely back through arbitrary surfaces and mirrors.
    config = pvt::default_config();
    make_small(config);
    for (auto& wave : config.waves) wave.synchronized = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.swings.clear();
    swing = pvt::default_swing(0U);
    swing.id = pvt::allocate_id(config);
    swing.amount = 1.0;
    swing.center_x = 0.25;
    swing.center_y = 0.5;
    swing.radius = 0.3;
    config.swings.push_back(swing);
    config.effects.clear();
    shake = pvt::default_effect(pvt::EffectType::Shake);
    shake.id = pvt::allocate_id(config);
    shake.enabled = true;
    shake.synchronized = true;
    shake.intensity = 1.0;
    shake.magnitude = 0.12;
    shake.center_x = swing.center_x;
    shake.center_y = swing.center_y;
    shake.edge_mode = pvt::EdgeMode::Reflect;
    shake.space = pvt::EffectSpace::Surface;
    config.effects.push_back(shake);
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Sphere;
    config.surface.curvature = 1.0;
    pvt::Image swing_enabled_effect;
    pvt::Image swing_disabled_effect;
    CHECK(pvt::render_frame_at_phase(
        config, 0.123, swing_enabled_effect, &error));
    config.swings.front().enabled = false;
    CHECK(pvt::render_frame_at_phase(
        config, 0.123, swing_disabled_effect, &error));
    CHECK(swing_enabled_effect.pixels == swing_disabled_effect.pixels);

    config.effects.front().space = pvt::EffectSpace::Texture;
    config.swings.front().enabled = true;
    CHECK(pvt::render_frame_at_phase(
        config, 0.123, swing_enabled_effect, &error));
    config.swings.front().enabled = false;
    CHECK(pvt::render_frame_at_phase(
        config, 0.123, swing_disabled_effect, &error));
    CHECK(mean_absolute_difference(swing_enabled_effect,
                                   swing_disabled_effect) > 0.0001);
}

void test_validation_limits() {
    auto config = pvt::default_config();
    CHECK(pvt::validate(config).ok);
    config.waves.resize(pvt::kMaximumWaves + 1U);
    CHECK(!pvt::validate(config).ok);
    config = pvt::default_config();
    config.output.bit_depth = 12;
    CHECK(!pvt::validate(config).ok);
    config = pvt::default_config();
    config.output.png_compression_level = -1;
    CHECK(!pvt::validate(config).ok);
    config.output.png_compression_level = 10;
    CHECK(!pvt::validate(config).ok);

    config = pvt::default_config();
    auto& block_scale = *std::find_if(
        config.effects.begin(), config.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::BlockScale;
        });
    CHECK(block_scale.type == pvt::EffectType::BlockScale);
    block_scale.enabled = true;
    block_scale.secondary = 3.0;
    CHECK(pvt::validate(config).ok);
    block_scale.intensity = 1.01;
    CHECK(!pvt::validate(config).ok);
    block_scale.intensity = 1.0;
    block_scale.magnitude = 0.0;
    CHECK(!pvt::validate(config).ok);
    block_scale.magnitude = 1.0;
    block_scale.frequency = 0.5;
    CHECK(!pvt::validate(config).ok);
    block_scale.frequency = 3.0;
    block_scale.secondary = 2.5;
    CHECK(!pvt::validate(config).ok);
    block_scale.secondary = -1.0;
    CHECK(!pvt::validate(config).ok);

    config = pvt::default_config();
    auto& particles = *std::find_if(
        config.effects.begin(), config.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    particles.enabled = true;
    CHECK(pvt::validate(config).ok);
    particles.frequency = 1000.0;
    particles.secondary = 1.0;
    particles.radius_pixels = 16384.0;
    CHECK(!pvt::validate(config).ok); // Bounded stamp-work guard.
    particles.radius_pixels = 2.0;
    CHECK(pvt::validate(config).ok);

    config = pvt::default_config();
    make_small(config);
    config.output.output_directory.clear();
    config.output.filename_prefix.clear();
    pvt::Image in_memory_only;
    std::string render_error;
    CHECK(pvt::render_frame(config, 0, in_memory_only, &render_error));
    config = pvt::default_config();
    config.width = 16384;
    config.height = 16384;
    config.block_size = 1;
    CHECK(!pvt::validate(config).ok); // Float pipeline memory-budget guard.

    config = pvt::default_config();
    config.width = 5000;
    config.height = 5000;
    config.block_size = 16;
    config.output.write_alpha = true;
    const auto two_buffer_result = pvt::validate(config);
    CHECK(two_buffer_result.ok);
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Plane;
    config.surface.rotations_per_loop = 0;
    config.surface.phase_degrees = -360.0;
    const auto neutral_plane_result = pvt::validate(config);
    CHECK(neutral_plane_result.ok);
    CHECK(neutral_plane_result.estimated_peak_bytes
          == two_buffer_result.estimated_peak_bytes);
    config.surface.phase_degrees = 45.0;
    CHECK(!pvt::validate(config).ok); // A rotated plane requires a third buffer.
    config.surface.enabled = false;
    config.surface.phase_degrees = 0.0;
    config.effects[1].enabled = true;
    config.effects[1].intensity = 0.0;
    const auto neutral_effect_result = pvt::validate(config);
    CHECK(neutral_effect_result.ok);
    CHECK(neutral_effect_result.estimated_peak_bytes
          == two_buffer_result.estimated_peak_bytes);
    config.effects[1].intensity = 1.0;
    CHECK(!pvt::validate(config).ok); // An active effect requires a third buffer.
    config.effects[1].intensity = 0.0;
    config.motion.enabled = true;
    config.motion.path = pvt::LayerMotionPath::Orbit;
    CHECK(!pvt::validate(config).ok); // Active motion uses the same third scratch buffer.

    config = pvt::default_config();
    make_small(config);
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::CustomObj;
    config.surface.obj_path = "mesh.obj";
    config.alpha.enabled = true;
    const auto obj_memory_result = pvt::validate(config);
    CHECK(obj_memory_result.ok);
    CHECK(obj_memory_result.estimated_peak_bytes > 350U * 1024U * 1024U);

    config = pvt::default_config();
    config.waves[0].direction = std::numeric_limits<double>::quiet_NaN();
    CHECK(!pvt::validate(config).ok);
    config = pvt::default_config();
    config.waves[0].name = std::string("bad") + static_cast<char>(0x7f);
    CHECK(!pvt::validate(config).ok);

    const std::string forbidden_prefix_characters = "<>:\"/\\|?*";
    for (const char character : forbidden_prefix_characters) {
        config = pvt::default_config();
        config.output.filename_prefix = "frame" + std::string(1U, character);
        CHECK(!pvt::validate(config).ok);
    }

    config = pvt::default_config();
    config.width = 16;
    config.height = 16;
    config.block_size = 1;
    config.effects.clear();
    for (std::size_t index = 0; index < 32U; ++index) {
        auto glow = pvt::default_effect(pvt::EffectType::Glow);
        glow.id = pvt::allocate_id(config);
        glow.enabled = true;
        glow.intensity = 100.0;
        glow.radius_pixels = 0.0;
        glow.threshold = 0.0;
        config.effects.push_back(std::move(glow));
    }
    CHECK(pvt::validate(config).ok); // Zero-radius glow is an exact no-op.
    for (auto& effect : config.effects) {
        effect.radius_pixels = 1.0;
    }
    CHECK(!pvt::validate(config).ok);
}

void test_setup_round_trip_and_transaction(const fs::path& directory) {
    auto original = pvt::default_config();
    make_small(original);
    original.waves.resize(1);
    original.waves[0].name = "Wave % one / alpha";
    original.waves[0].synchronized = false;
    original.waves[0].direction = 0.123;
    original.swings.push_back(pvt::default_swing(4));
    original.swings.back().id = pvt::allocate_id(original);
    original.swings.back().center_x = 0.27;
    original.swings.back().center_y = 0.73;
    original.swings.back().radius = 0.31;
    original.effects.push_back(pvt::default_effect(pvt::EffectType::Shake));
    original.effects.back().id = pvt::allocate_id(original);
    original.effects.back().enabled = true;
    original.effects.back().space = pvt::EffectSpace::Surface;
    original.effects.back().center_x = 0.42;
    original.effects.back().center_y = 0.61;
    original.effects.back().area_radius = 0.24;
    original.alpha.enabled = true;
    original.quantization.enabled = true;
    original.quantization.mode = pvt::QuantizationMode::Hue;
    original.surface.enabled = true;
    original.surface.mapping = pvt::SurfaceMapping::Cylinder;
    original.surface.obj_path = "mesh folder/test.obj";
    original.palette = pvt::default_palette(2U);
    original.palette.enabled = false;
    original.transform.flip_horizontal = true;
    original.transform.mirror = pvt::MirrorMode::BottomToTop;
    original.output.bit_depth = 16;
    original.output.write_alpha = true;
    original.output.png_compression_level = 3;
    original.output.dither_method = pvt::DitherMethod::FloydSteinberg;
    original.output.output_directory = "output folder/%safe";
    original.clock.mode = pvt::ClockMode::Music;
    original.clock.interpolation = pvt::ClockInterpolation::Smoothstep;
    original.clock.fit = pvt::ClockFit::FitSequence;
    original.clock.frame_interval = 7;
    original.clock.time_interval_microseconds = 375000;
    original.clock.meter.expression = "3+2+3/8 | 5/4";
    original.clock.meter.bpm = 137.5;
    original.clock.meter.tempo_note_denominator = 8;
    original.clock.music_tempo = pvt::MusicTempoMode::Double;
    original.clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
    original.clock.data_only = true;
    original.clock.beat_offset_microseconds = -25000;
    original.clock.phase_offset_degrees = 11.25;
    original.clock.reverse = true;
    original.clock.music.analyzer_version = "pvt-test/1";
    original.clock.music.source_sha256 = std::string(64U, 'a');
    original.clock.music.source_basename = "track % alpha.wav";
    original.clock.music.source_format = "wav-f32";
    original.clock.music.source_frame_count = 96000U;
    original.clock.music.source_sample_rate = 48000U;
    original.clock.music.source_channel_count = 2U;
    original.clock.music.duration_seconds = 2.0;
    original.clock.music.detected_bpm = 137.5;
    original.clock.music.tempo_confidence = 0.91;
    original.clock.music.beat_times_seconds = {0.125, 0.5625, 1.0, 1.4375};
    original.clock.music.tempo_points = {
        {0.0, 137.5, 0.91}, {1.0, 141.0, 0.84},
    };
    original.clock.music.feature_samples = {
        {0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F},
        {0.7F, 0.6F, 0.5F, 0.4F, 0.3F, 0.2F},
    };
    original.swings_enabled = false;
    original.audio_reactive.enabled = true;
    original.audio_reactive.synchronized_only = false;
    original.audio_reactive.wave_source = pvt::MusicFeature::Bass;
    original.audio_reactive.wave_amount = 0.61;
    original.audio_reactive.effect_source = pvt::MusicFeature::Onset;
    original.audio_reactive.effect_amount = 0.72;
    original.audio_reactive.color_enabled = true;
    original.audio_reactive.color_source = pvt::MusicFeature::Midrange;
    original.audio_reactive.color_amount_degrees = 42.0;
    original.layer_clock.enabled = true;
    original.layer_clock.scale = pvt::LayerClockScale::OriginalSpeedLoop;
    original.layer_clock.clock.mode = pvt::ClockMode::Time;
    original.layer_clock.clock.time_interval_microseconds = 187500;
    original.layer_clock.clock.reverse = true;
    CHECK(original.layer_clock.clock.data_only);
    original.motion.enabled = true;
    original.motion.path = pvt::LayerMotionPath::Lissajous;
    original.motion.center_x = 0.41;
    original.motion.center_y = 0.63;
    original.motion.travel_x = 0.22;
    original.motion.travel_y = 0.17;
    original.motion.cycles_x = 3;
    original.motion.cycles_y = 2;
    original.motion.phase_degrees = 27.0;
    original.motion.rotations_per_loop = -2;
    original.motion.scale_pulse = 0.14;

    const fs::path first = directory / "first.pvt";
    const fs::path second = directory / "second.pvt";
    std::string error;
    CHECK(pvt::save_setup(original, first.string(), &error));
#if defined(_WIN32)
    DWORD setup_attributes = GetFileAttributesW(first.c_str());
    CHECK(setup_attributes != INVALID_FILE_ATTRIBUTES);
    CHECK(SetFileAttributesW(first.c_str(),
                             setup_attributes | FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK(pvt::save_setup(original, pvt::detail::path_to_utf8(first), &error));
    setup_attributes = GetFileAttributesW(first.c_str());
    CHECK(setup_attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((setup_attributes & FILE_ATTRIBUTE_HIDDEN) != 0U);
#else
    // Preserve the full explicit mode, including bits that a later content
    // write could otherwise clear on POSIX.
    CHECK(::chmod(first.string().c_str(), 04750) == 0);
    CHECK(pvt::save_setup(original, first.string(), &error));
    struct stat setup_status {};
    CHECK(::stat(first.string().c_str(), &setup_status) == 0);
    CHECK((setup_status.st_mode & 07777) == 04750);

    const fs::path setup_symlink_target = directory / "setup-symlink-target";
    const fs::path setup_symlink = directory / "setup-symlink.pvt";
    {
        std::ofstream output(setup_symlink_target, std::ios::binary);
        output << "target-must-not-change";
    }
    const auto setup_symlink_target_bytes = read_bytes(setup_symlink_target);
    std::error_code setup_symlink_error;
    fs::create_symlink(setup_symlink_target.filename(), setup_symlink,
                       setup_symlink_error);
    CHECK(!setup_symlink_error);
    CHECK(pvt::save_setup(original, setup_symlink.string(), &error));
    CHECK(fs::is_regular_file(fs::symlink_status(setup_symlink)));
    CHECK(read_bytes(setup_symlink_target) == setup_symlink_target_bytes);
#endif
    auto loaded = pvt::default_config();
    loaded.width = 777;
    CHECK(pvt::load_setup(first.string(), loaded, &error));
    CHECK(loaded.output.png_compression_level == 3);
    CHECK(loaded.output.write_alpha);
    CHECK(loaded.surface.obj_path == original.surface.obj_path);
    CHECK(loaded.swings.back().radius == original.swings.back().radius);
    CHECK(loaded.effects.back().space == pvt::EffectSpace::Surface);
    CHECK(loaded.effects.back().area_radius == original.effects.back().area_radius);
    CHECK(!loaded.palette.enabled);
    CHECK(loaded.palette.name == original.palette.name);
    CHECK(loaded.palette.colors.size() == original.palette.colors.size());
    if (loaded.palette.colors.size() == original.palette.colors.size()) {
        for (std::size_t index = 0U; index < loaded.palette.colors.size(); ++index) {
            CHECK(loaded.palette.colors[index].red
                  == original.palette.colors[index].red);
            CHECK(loaded.palette.colors[index].green
                  == original.palette.colors[index].green);
            CHECK(loaded.palette.colors[index].blue
                  == original.palette.colors[index].blue);
        }
    }
    CHECK(loaded.transform.flip_horizontal);
    CHECK(loaded.transform.mirror == pvt::MirrorMode::BottomToTop);
    CHECK(loaded.clock.mode == pvt::ClockMode::Music);
    CHECK(loaded.clock.interpolation == pvt::ClockInterpolation::Smoothstep);
    CHECK(loaded.clock.fit == pvt::ClockFit::FitSequence);
    CHECK(loaded.clock.meter.expression == "3+2+3/8 | 5/4");
    CHECK(loaded.clock.music_tempo == pvt::MusicTempoMode::Double);
    CHECK(loaded.clock.music_swing_policy == pvt::MusicSwingPolicy::KeepAll);
    CHECK(loaded.clock.data_only);
    CHECK(loaded.clock.music.source_sha256 == std::string(64U, 'a'));
    CHECK(loaded.clock.music.source_basename == "track % alpha.wav");
    CHECK(loaded.clock.music.beat_times_seconds
          == original.clock.music.beat_times_seconds);
    CHECK(loaded.clock.music.tempo_points.size() == 2U);
    CHECK(loaded.clock.music.feature_samples.size() == 2U);
    CHECK(!loaded.swings_enabled);
    CHECK(loaded.audio_reactive.enabled);
    CHECK(loaded.audio_reactive.wave_source == pvt::MusicFeature::Bass);
    CHECK(loaded.audio_reactive.color_amount_degrees == 42.0);
    CHECK(loaded.layer_clock.enabled);
    CHECK(loaded.layer_clock.scale == pvt::LayerClockScale::OriginalSpeedLoop);
    CHECK(loaded.layer_clock.clock.mode == pvt::ClockMode::Time);
    CHECK(loaded.layer_clock.clock.time_interval_microseconds == 187500);
    CHECK(loaded.layer_clock.clock.data_only);
    CHECK(loaded.motion.enabled);
    CHECK(loaded.motion.path == pvt::LayerMotionPath::Lissajous);
    CHECK(loaded.motion.cycles_x == 3 && loaded.motion.cycles_y == 2);
    CHECK(loaded.motion.rotations_per_loop == -2);
    CHECK(loaded.motion.scale_pulse == 0.14);
    CHECK(pvt::save_setup(loaded, second.string(), &error));
    CHECK(read_bytes(first) == read_bytes(second));

    // Each compatibility fixture removes the records introduced by the newer
    // version; merely changing a header would create an impossible old file.
    const auto erase_record = [](std::string& setup, const std::string& key) {
        const std::string prefix = key + "\t";
        const std::size_t position = setup.find(prefix);
        CHECK(position != std::string::npos);
        if (position == std::string::npos) return;
        const std::size_t newline = setup.find('\n', position);
        CHECK(newline != std::string::npos);
        if (newline != std::string::npos) {
            setup.erase(position, newline + 1U - position);
        }
    };
    const auto erase_records_with_prefix = [](std::string& setup,
                                              const std::string& prefix) {
        std::size_t position = 0U;
        while ((position = setup.find(prefix, position)) != std::string::npos) {
            if (position != 0U && setup[position - 1U] != '\n') {
                position += prefix.size();
                continue;
            }
            const std::size_t newline = setup.find('\n', position);
            CHECK(newline != std::string::npos);
            if (newline == std::string::npos) return;
            setup.erase(position, newline + 1U - position);
        }
    };
    const auto erase_records_with_fragment = [](std::string& setup,
                                                const std::string& fragment) {
        std::size_t line_start = 0U;
        while (line_start < setup.size()) {
            const std::size_t newline = setup.find('\n', line_start);
            if (newline == std::string::npos) break;
            const std::size_t tab = setup.find('\t', line_start);
            if (tab != std::string::npos && tab < newline
                && setup.substr(line_start, tab - line_start).find(fragment)
                       != std::string::npos) {
                setup.erase(line_start, newline + 1U - line_start);
                continue;
            }
            line_start = newline + 1U;
        }
    };

    const auto version_seven_bytes = read_bytes(first);

    // Semantic recovery must not depend on alphabetical group ordering. A
    // custom motion binding is invalid until its reusable path is admitted,
    // while an unrelated bad canvas field forces the grouped recovery path.
    pvt::RenderConfig dependent = pvt::default_config();
    make_small(dependent);
    dependent.motion_paths.push_back(
        pvt::default_ellipse_path(700U, 800U, "Recovery ellipse"));
    dependent.motion.enabled = true;
    dependent.motion.custom_path.enabled = true;
    dependent.motion.custom_path.path_id = 700U;
    dependent.motion.custom_path.follow_tangent = true;
    std::string dependent_setup;
    CHECK(pvt::detail::serialize_setup_config(
        dependent, dependent_setup, &error));
    const std::string valid_width =
        "canvas.width\t" + std::to_string(dependent.width) + "\n";
    const std::size_t valid_width_at = dependent_setup.find(valid_width);
    CHECK(valid_width_at != std::string::npos);
    if (valid_width_at != std::string::npos) {
        dependent_setup.replace(valid_width_at, valid_width.size(),
                                "canvas.width\t0\n");
    }
    pvt::RenderConfig dependent_recovered;
    CHECK(pvt::detail::deserialize_setup_config(
        dependent_setup, dependent_recovered, &error));
    CHECK(dependent_recovered.width > 0);
    CHECK(dependent_recovered.motion_paths.size() == 1U);
    CHECK(dependent_recovered.motion.custom_path.enabled);
    CHECK(dependent_recovered.motion.custom_path.path_id == 700U);
    CHECK(std::any_of(
        dependent_recovered.output_compatibility.records.begin(),
        dependent_recovered.output_compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "canvas.width" && record.value == "0"
                   && record.rejected;
        }));

    std::string version_six(version_seven_bytes.begin(), version_seven_bytes.end());
    CHECK(version_six.rfind("PVT_SETUP\t7\n", 0U) == 0U);
    version_six.replace(0U, std::string("PVT_SETUP\t7").size(),
                        "PVT_SETUP\t6");
    erase_records_with_prefix(version_six, "source_image.");
    erase_records_with_prefix(version_six, "paths.");
    erase_record(version_six, "motion.rotation_offset_degrees");
    erase_records_with_prefix(version_six, "motion.custom_path.");
    erase_records_with_fragment(version_six, ".path.");

    std::string version_five = version_six;
    CHECK(version_five.rfind("PVT_SETUP\t6\n", 0U) == 0U);
    version_five.replace(0U, std::string("PVT_SETUP\t6").size(),
                         "PVT_SETUP\t5");
    erase_record(version_five, "timing.clock.data_only");
    erase_records_with_prefix(version_five, "layer_clock.");
    erase_records_with_prefix(version_five, "motion.");

    const fs::path version_five_setup = directory / "version-five.pvt";
    {
        std::ofstream output(version_five_setup, std::ios::binary);
        output.write(version_five.data(),
                     static_cast<std::streamsize>(version_five.size()));
    }
    auto loaded_version_five = original;
    CHECK(pvt::load_setup(version_five_setup.string(), loaded_version_five,
                          &error));
    CHECK(!loaded_version_five.clock.data_only);
    CHECK(!loaded_version_five.layer_clock.enabled);
    CHECK(!loaded_version_five.motion.enabled);

    // Version 4 predates synchronization and audio response. Remove every v5
    // record so new fields receive neutral defaults rather than leaking the
    // destination's previous state.
    std::string version_four = version_five;
    version_four.replace(0U, std::string("PVT_SETUP\t5").size(),
                         "PVT_SETUP\t4");
    erase_records_with_prefix(version_four, "timing.clock.");
    erase_records_with_prefix(version_four, "timing.music.");
    erase_record(version_four, "rhythm.swings_enabled");
    erase_records_with_prefix(version_four, "audio_reactive.");
    erase_record(version_four, "surface.obj_sha256");
    erase_record(version_four, "surface.obj_basename");

    const fs::path version_four_setup = directory / "version-four.pvt";
    {
        std::ofstream output(version_four_setup, std::ios::binary);
        output.write(version_four.data(),
                     static_cast<std::streamsize>(version_four.size()));
    }
    auto loaded_version_four = original;
    CHECK(pvt::load_setup(version_four_setup.string(), loaded_version_four, &error));
    CHECK(loaded_version_four.clock.mode == pvt::ClockMode::Default);
    CHECK(loaded_version_four.clock.music.source_sha256.empty());
    CHECK(loaded_version_four.clock.music.beat_times_seconds.empty());
    CHECK(loaded_version_four.swings_enabled);
    CHECK(!loaded_version_four.audio_reactive.enabled);

    // Version 2 predates the independent export-alpha flag. Legacy files map
    // it from the render alpha setting instead of silently losing that state.
    std::string version_three = version_four;
    CHECK(version_three.rfind("PVT_SETUP\t4\n", 0U) == 0U);
    version_three.replace(0U, std::string("PVT_SETUP\t4").size(), "PVT_SETUP\t3");
    for (std::size_t index = 0U; index < original.swings.size(); ++index) {
        for (const char* field : {"center_x", "center_y", "radius"}) {
            erase_record(version_three, "swings." + std::to_string(index)
                                           + "." + field);
        }
    }
    for (std::size_t index = 0U; index < original.effects.size(); ++index) {
        erase_record(version_three, "effects." + std::to_string(index) + ".space");
        erase_record(version_three,
                     "effects." + std::to_string(index) + ".area_radius");
    }
    erase_record(version_three, "palette.enabled");
    erase_record(version_three, "palette.name");
    erase_record(version_three, "palette.colors.count");
    for (std::size_t index = 0U; index < original.palette.colors.size(); ++index) {
        for (const char* field : {"red", "green", "blue"}) {
            erase_record(version_three, "palette.colors." + std::to_string(index)
                                           + "." + field);
        }
    }
    erase_record(version_three, "transform.flip_horizontal");
    erase_record(version_three, "transform.flip_vertical");
    erase_record(version_three, "transform.mirror");

    std::string version_two = version_three;
    CHECK(version_two.rfind("PVT_SETUP\t3\n", 0U) == 0U);
    version_two.replace(0U, std::string("PVT_SETUP\t3").size(), "PVT_SETUP\t2");
    const std::string write_alpha_record = "output.write_alpha\t1\n";
    const std::size_t write_alpha_position = version_two.find(write_alpha_record);
    CHECK(write_alpha_position != std::string::npos);
    if (write_alpha_position != std::string::npos) {
        version_two.erase(write_alpha_position, write_alpha_record.size());
    }
    const fs::path version_two_setup = directory / "version-two.pvt";
    {
        std::ofstream output(version_two_setup, std::ios::binary);
        output.write(version_two.data(), static_cast<std::streamsize>(version_two.size()));
    }
    auto loaded_version_two = pvt::default_config();
    CHECK(pvt::load_setup(version_two_setup.string(), loaded_version_two, &error));
    CHECK(loaded_version_two.output.write_alpha);
    CHECK(!loaded_version_two.palette.enabled);
    CHECK(loaded_version_two.palette.colors.empty());
    CHECK(loaded_version_two.effects.back().space == pvt::EffectSpace::Texture);
    CHECK(loaded_version_two.effects.back().area_radius == 0.0);
    CHECK(loaded_version_two.swings.back().radius == 0.0);
    CHECK(loaded_version_two.transform.mirror == pvt::MirrorMode::None);

    // Version 1 also predates PNG compression control and custom OBJ paths. It
    // remains loadable and receives the current defaults for both fields.
    std::string version_one = version_two;
    version_one.replace(0U, std::string("PVT_SETUP\t2").size(), "PVT_SETUP\t1");
    const std::string compression_record = "output.png_compression_level\t3\n";
    const std::size_t compression_position = version_one.find(compression_record);
    CHECK(compression_position != std::string::npos);
    if (compression_position != std::string::npos) {
        version_one.erase(compression_position, compression_record.size());
    }
    const std::string obj_record = "surface.obj_path\tmesh%20folder%2Ftest.obj\n";
    const std::size_t obj_position = version_one.find(obj_record);
    CHECK(obj_position != std::string::npos);
    if (obj_position != std::string::npos) {
        version_one.erase(obj_position, obj_record.size());
    }
    const fs::path version_one_setup = directory / "version-one.pvt";
    {
        std::ofstream output(version_one_setup, std::ios::binary);
        output.write(version_one.data(), static_cast<std::streamsize>(version_one.size()));
    }
    auto loaded_version_one = pvt::default_config();
    loaded_version_one.output.png_compression_level = 9;
    CHECK(pvt::load_setup(version_one_setup.string(), loaded_version_one, &error));
    CHECK(loaded_version_one.output.png_compression_level == 5);
    CHECK(loaded_version_one.output.write_alpha);
    CHECK(loaded_version_one.surface.obj_path.empty());

    const fs::path unicode_setup =
        directory / pvt::detail::path_from_utf8("setup-\xCE\xB3.pvt");
    CHECK(pvt::save_setup(loaded, pvt::detail::path_to_utf8(unicode_setup), &error));
    auto unicode_loaded = pvt::default_config();
    CHECK(pvt::load_setup(pvt::detail::path_to_utf8(unicode_setup),
                          unicode_loaded, &error));
    const fs::path unicode_round_trip =
        directory / pvt::detail::path_from_utf8("setup-\xCE\xB3-copy.pvt");
    CHECK(pvt::save_setup(unicode_loaded,
                          pvt::detail::path_to_utf8(unicode_round_trip), &error));
    CHECK(read_bytes(unicode_setup) == read_bytes(unicode_round_trip));

    const auto valid_setup_bytes = read_bytes(first);
    original.width = 0;
    CHECK(!pvt::save_setup(original, first.string(), &error));
    CHECK(read_bytes(first) == valid_setup_bytes);
    original.width = loaded.width;

    pvt::RenderConfig malformed_preservation = original;
    malformed_preservation.source_compatibility.records.push_back(
        {"Not-A-Valid-Key", "value", false});
    const fs::path malformed_preservation_path =
        directory / "malformed-preservation.pvt";
    CHECK(!pvt::save_setup(malformed_preservation,
                           malformed_preservation_path.string(), &error));
    CHECK(error.find("no preserved data was discarded") != std::string::npos);
    CHECK(!fs::exists(malformed_preservation_path));

    original.output.bit_depth = 32;
    original.output.dither_enabled = true; // Saving must normalize this to off.
    const fs::path float_setup = directory / "float.pvt";
    const fs::path float_round_trip = directory / "float-roundtrip.pvt";
    CHECK(pvt::save_setup(original, float_setup.string(), &error));
    CHECK(pvt::load_setup(float_setup.string(), loaded, &error));
    CHECK(!loaded.output.dither_enabled);
    CHECK(pvt::save_setup(loaded, float_round_trip.string(), &error));
    CHECK(read_bytes(float_setup) == read_bytes(float_round_trip));

    std::string oversized_analysis(version_seven_bytes.begin(),
                                   version_seven_bytes.end());
    const std::string feature_count = "timing.music.feature_samples.count\t2\n";
    const std::size_t feature_count_at = oversized_analysis.find(feature_count);
    CHECK(feature_count_at != std::string::npos);
    if (feature_count_at != std::string::npos) {
        oversized_analysis.replace(
            feature_count_at, feature_count.size(),
            "timing.music.feature_samples.count\t"
                + std::to_string(pvt::kMaximumMusicFeatureSamples + 1U) + "\n");
    }
    const fs::path oversized_setup = directory / "oversized-analysis.pvt";
    {
        std::ofstream output(oversized_setup, std::ios::binary);
        output.write(oversized_analysis.data(),
                     static_cast<std::streamsize>(oversized_analysis.size()));
    }
    loaded.width = 555;
    loaded.clock.mode = pvt::ClockMode::Frame;
    CHECK(pvt::load_setup(oversized_setup.string(), loaded, &error));
    CHECK(loaded.width == original.width);
    CHECK(loaded.clock.mode == original.clock.mode);
    CHECK(loaded.clock.music.feature_samples.size()
          <= pvt::kMaximumMusicFeatureSamples);
    CHECK(std::any_of(
        loaded.clock.music.compatibility.records.begin(),
        loaded.clock.music.compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "timing.music.feature_samples.count"
                   && record.rejected;
        }));
    const fs::path recovered_setup = directory / "recovered-analysis.pvt";
    CHECK(pvt::save_setup(loaded, recovered_setup.string(), &error));
    const auto recovered_bytes = read_bytes(recovered_setup);
    const std::string recovered_text(recovered_bytes.begin(),
                                     recovered_bytes.end());
    CHECK(recovered_text.find("compatibility.rejected.count\t")
          != std::string::npos);
    pvt::RenderConfig recovered_round_trip;
    CHECK(pvt::load_setup(recovered_setup.string(), recovered_round_trip,
                          &error));
    CHECK(std::any_of(
        recovered_round_trip.clock.music.compatibility.records.begin(),
        recovered_round_trip.clock.music.compatibility.records.end(),
        [](const pvt::PreservedConfigRecord& record) {
            return record.key == "timing.music.feature_samples.count"
                   && record.rejected;
        }));

    const auto unchanged = read_bytes(second);
    const int width_before = loaded.width;
    const fs::path malformed = directory / "malformed.pvt";
    {
        std::ofstream output(malformed, std::ios::binary);
        output << "PVT_SETUP\t1\nwidth\t96\nwidth\t97\n";
    }
    CHECK(!pvt::load_setup(malformed.string(), loaded, &error));
    CHECK(loaded.width == width_before);
    CHECK(read_bytes(second) == unchanged);
}

void test_maximum_music_analysis_setup(const fs::path& directory) {
    pvt::RenderConfig original = pvt::default_config();
    make_small(original);
    const pvt::MusicFeatureSample sample{
        0.125F, 0.25F, 0.375F, 0.5F, 0.625F, 0.75F,
    };
    original.clock.music.feature_samples.assign(
        pvt::kMaximumMusicFeatureSamples, sample);

    const fs::path first = directory / "maximum-analysis.pvt";
    const fs::path second = directory / "maximum-analysis-roundtrip.pvt";
    std::string error;
    CHECK(pvt::save_setup(original, first.string(), &error));
    const std::vector<unsigned char> first_bytes = read_bytes(first);
    CHECK(first_bytes.size() > 2U * 1024U * 1024U);
    CHECK(first_bytes.size() <= pvt::kMaximumSetupBytes);

    pvt::RenderConfig loaded;
    CHECK(pvt::load_setup(first.string(), loaded, &error));
    CHECK(loaded.clock.music.feature_samples.size()
          == pvt::kMaximumMusicFeatureSamples);
    if (!loaded.clock.music.feature_samples.empty()) {
        CHECK(loaded.clock.music.feature_samples.front().energy == sample.energy);
        CHECK(loaded.clock.music.feature_samples.back().beat == sample.beat);
    }
    CHECK(pvt::save_setup(loaded, second.string(), &error));
    CHECK(read_bytes(second) == first_bytes);
}

void check_png_header(const fs::path& path, int bit_depth, int color_type) {
    const auto bytes = read_bytes(path);
    static const unsigned char signature[] = {0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU,
                                               0x1aU, 0x0aU};
    CHECK(bytes.size() > 26U);
    if (bytes.size() > 26U) {
        CHECK(std::equal(std::begin(signature), std::end(signature), bytes.begin()));
        CHECK(bytes[24] == static_cast<unsigned char>(bit_depth));
        CHECK(bytes[25] == static_cast<unsigned char>(color_type));
    }
}

std::vector<std::pair<std::string, std::uint32_t>> exr_channels(const fs::path& path) {
    const auto bytes = read_bytes(path);
    std::vector<std::pair<std::string, std::uint32_t>> channels;
    CHECK(bytes.size() > 64U);
    if (bytes.size() < 64U) {
        return channels;
    }
    CHECK(bytes[0] == 0x76U && bytes[1] == 0x2fU && bytes[2] == 0x31U
          && bytes[3] == 0x01U);

    const std::string marker("channels\0chlist\0", 16);
    const auto found = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
    if (found == bytes.end()) {
        CHECK(false);
        return channels;
    }
    std::size_t position = static_cast<std::size_t>(found - bytes.begin()) + marker.size();
    if (position + 4U > bytes.size()) {
        CHECK(false);
        return channels;
    }
    const auto read_u32 = [&bytes](std::size_t at) {
        return static_cast<std::uint32_t>(bytes[at])
               | (static_cast<std::uint32_t>(bytes[at + 1U]) << 8U)
               | (static_cast<std::uint32_t>(bytes[at + 2U]) << 16U)
               | (static_cast<std::uint32_t>(bytes[at + 3U]) << 24U);
    };
    const std::size_t size = read_u32(position);
    position += 4U;
    const std::size_t end = position + size;
    if (end > bytes.size()) {
        CHECK(false);
        return channels;
    }
    while (position < end && bytes[position] != 0U) {
        std::string name;
        while (position < end && bytes[position] != 0U) {
            name.push_back(static_cast<char>(bytes[position++]));
        }
        ++position;
        if (position + 16U > end) {
            CHECK(false);
            return {};
        }
        const std::uint32_t type = read_u32(position);
        position += 16U;
        channels.emplace_back(std::move(name), type);
    }
    return channels;
}

void test_image_formats_and_dither(const fs::path& directory) {
    auto config = pvt::default_config();
    make_small(config);
    config.width = 33;
    config.height = 19;
    config.block_size = 1;
    config.alpha.enabled = true;
    config.alpha.minimum = 0.05;
    config.alpha.maximum = 0.95;
    config.alpha.spatial_frequency = 2.75;
    pvt::Image image;
    std::string error;
    CHECK(pvt::render_frame(config, 2, image, &error));

    const fs::path protected_destination = directory / "protected-image.bin";
    {
        std::ofstream output(protected_destination, std::ios::binary);
        output << "unchanged";
    }
    const auto protected_bytes = read_bytes(protected_destination);
    pvt::Image invalid_image = image;
    invalid_image.pixels[0] = std::numeric_limits<float>::quiet_NaN();
    config.output.overwrite_existing = true;
    CHECK(!pvt::write_image(protected_destination.string(), invalid_image,
                            config, 123U, &error));
    CHECK(read_bytes(protected_destination) == protected_bytes);
    config.output.overwrite_existing = false;

#if !defined(_WIN32)
    const fs::path permission_path = directory / "preserve-mode.png";
    CHECK(pvt::write_image(permission_path.string(), image, config, 123U, &error));
    CHECK(::chmod(permission_path.string().c_str(), 0660) == 0);
    const mode_t previous_umask = ::umask(0022);
    config.output.overwrite_existing = true;
    const bool overwrote_permission_file = pvt::write_image(
        permission_path.string(), image, config, 456U, &error);
    (void)::umask(previous_umask);
    CHECK(overwrote_permission_file);
    struct stat permission_status {};
    CHECK(::lstat(permission_path.string().c_str(), &permission_status) == 0);
    CHECK((permission_status.st_mode & 0777) == 0660);

    const fs::path symlink_target = directory / "image-symlink-target";
    const fs::path symlink_output = directory / "image-symlink.png";
    {
        std::ofstream output(symlink_target, std::ios::binary);
        output << "target-must-not-change";
    }
    const auto symlink_target_bytes = read_bytes(symlink_target);
    std::error_code symlink_error;
    fs::create_symlink(symlink_target.filename(), symlink_output, symlink_error);
    CHECK(!symlink_error);
    config.output.overwrite_existing = true;
    CHECK(pvt::write_image(symlink_output.string(), image, config, 789U, &error));
    CHECK(fs::is_regular_file(fs::symlink_status(symlink_output)));
    CHECK(read_bytes(symlink_target) == symlink_target_bytes);
    config.output.overwrite_existing = false;
#else
    const fs::path metadata_path = directory / "preserve-metadata.png";
    CHECK(pvt::write_image(metadata_path.string(), image, config, 123U, &error));
    DWORD image_attributes = GetFileAttributesW(metadata_path.c_str());
    CHECK(image_attributes != INVALID_FILE_ATTRIBUTES);
    CHECK(SetFileAttributesW(metadata_path.c_str(),
                             image_attributes | FILE_ATTRIBUTE_HIDDEN) != 0);
    config.output.overwrite_existing = true;
    CHECK(pvt::write_image(pvt::detail::path_to_utf8(metadata_path), image,
                           config, 456U, &error));
    image_attributes = GetFileAttributesW(metadata_path.c_str());
    CHECK(image_attributes != INVALID_FILE_ATTRIBUTES);
    CHECK((image_attributes & FILE_ATTRIBUTE_HIDDEN) != 0U);
    config.output.overwrite_existing = false;
#endif

    const fs::path truncated_path = directory / "nul-truncated-output";
    std::string nul_path = truncated_path.string();
    nul_path.push_back('\0');
    nul_path += ".png";
    CHECK(!pvt::write_image(nul_path, image, config, 123U, &error));
    CHECK(!fs::exists(truncated_path));

    for (const int depth : {8, 16}) {
        config.output.bit_depth = depth;
        config.output.dither_enabled = true;
        config.alpha.enabled = false;
        const fs::path rgb = directory / ("rgb" + std::to_string(depth) + ".png");
        CHECK(pvt::write_image(rgb.string(), image, config, 123U, &error));
        check_png_header(rgb, depth, 2);

        config.alpha.enabled = true;
        const fs::path rgba = directory / ("rgba" + std::to_string(depth) + ".png");
        CHECK(pvt::write_image(rgba.string(), image, config, 123U, &error));
        check_png_header(rgba, depth, 6);
        if (depth == 8) {
            png_uint_32 decoded_width = 0;
            png_uint_32 decoded_height = 0;
            const auto decoded = decode_png_rgba8(rgba, &decoded_width, &decoded_height);
            CHECK(decoded_width == static_cast<png_uint_32>(config.width));
            CHECK(decoded_height == static_cast<png_uint_32>(config.height));
            unsigned char minimum_alpha = 255U;
            unsigned char maximum_alpha = 0U;
            for (std::size_t offset = 3U; offset < decoded.size(); offset += 4U) {
                minimum_alpha = std::min(minimum_alpha, decoded[offset]);
                maximum_alpha = std::max(maximum_alpha, decoded[offset]);
            }
            CHECK(minimum_alpha < 64U);
            CHECK(maximum_alpha > 192U);
        }
    }

    config.output.bit_depth = 8;
    config.output.dither_method = pvt::DitherMethod::BlueNoise;
    config.alpha.enabled = true;
    const fs::path noise_a = directory / "noise-a.png";
    const fs::path noise_b = directory / "noise-b.png";
    CHECK(pvt::write_image(noise_a.string(), image, config, 999U, &error));
    CHECK(pvt::write_image(noise_b.string(), image, config, 999U, &error));
    CHECK(read_bytes(noise_a) == read_bytes(noise_b));
    const fs::path noise_other_seed = directory / "noise-other-seed.png";
    CHECK(pvt::write_image(noise_other_seed.string(), image, config, 1000U, &error));
    CHECK(read_bytes(noise_a) != read_bytes(noise_other_seed));
    config.output.dither_enabled = false;
    const fs::path undithered = directory / "undithered.png";
    CHECK(pvt::write_image(undithered.string(), image, config, 999U, &error));
    CHECK(read_bytes(noise_a) != read_bytes(undithered));

    const fs::path compression_off = directory / "compression-off.png";
    const fs::path compression_max = directory / "compression-max.png";
    config.output.png_compression_level = 0;
    CHECK(pvt::write_image(compression_off.string(), image, config, 999U, &error));
    config.output.png_compression_level = 9;
    CHECK(pvt::write_image(compression_max.string(), image, config, 999U, &error));
    png_uint_32 off_width = 0;
    png_uint_32 off_height = 0;
    png_uint_32 max_width = 0;
    png_uint_32 max_height = 0;
    const auto decoded_off = decode_png_rgba8(compression_off, &off_width, &off_height);
    const auto decoded_max = decode_png_rgba8(compression_max, &max_width, &max_height);
    CHECK(off_width == max_width && off_height == max_height);
    CHECK(decoded_off == decoded_max);
    CHECK(read_bytes(compression_max).size() < read_bytes(compression_off).size());

    config.output.bit_depth = 32;
    config.output.dither_enabled = true; // Must still be ignored for EXR.
    config.alpha.enabled = false;
    const fs::path rgb_exr = directory / "rgb.exr";
    CHECK(pvt::write_image(rgb_exr.string(), image, config, 1U, &error));
    auto channels = exr_channels(rgb_exr);
    CHECK(channels.size() == 3U);
    CHECK(std::all_of(channels.begin(), channels.end(),
                      [](const auto& channel) { return channel.second == 2U; }));

    config.alpha.enabled = true;
    const fs::path rgba_exr = directory / "rgba.exr";
    CHECK(pvt::write_image(rgba_exr.string(), image, config, 999U, &error));
    channels = exr_channels(rgba_exr);
    CHECK(channels.size() == 4U);
    CHECK(std::all_of(channels.begin(), channels.end(),
                      [](const auto& channel) { return channel.second == 2U; }));

    config.output.dither_enabled = false;
    const fs::path rgba_exr_no_dither = directory / "rgba-no-dither.exr";
    CHECK(pvt::write_image(rgba_exr_no_dither.string(), image, config, 42U, &error));
    CHECK(read_bytes(rgba_exr) == read_bytes(rgba_exr_no_dither));

    // Transparent edge handling requires an alpha-bearing output. Rendering in
    // memory remains available so callers can repair export settings without
    // losing their preview, but write_image must enforce full export validity.
    auto transparent_edge = pvt::default_config();
    make_small(transparent_edge);
    transparent_edge.effects.clear();
    auto shake = pvt::default_effect(pvt::EffectType::Shake);
    shake.id = pvt::allocate_id(transparent_edge);
    shake.enabled = true;
    shake.edge_mode = pvt::EdgeMode::Alpha;
    transparent_edge.effects.push_back(shake);
    transparent_edge.alpha.enabled = false;
    pvt::Image transparent_image;
    CHECK(pvt::render_frame(transparent_edge, 2, transparent_image, &error));
    const fs::path transparent_png = directory / "transparent-edge.png";
    CHECK(!pvt::write_image(transparent_png.string(), transparent_image,
                            transparent_edge, 12U, &error));
    CHECK(error.find("Alpha output") != std::string::npos);
    CHECK(!fs::exists(transparent_png));
    transparent_edge.alpha.enabled = true;
    CHECK(pvt::render_frame(transparent_edge, 2, transparent_image, &error));
    CHECK(pvt::write_image(transparent_png.string(), transparent_image,
                           transparent_edge, 12U, &error));
    check_png_header(transparent_png, 8, 6);

    auto transparent_surface = pvt::default_config();
    make_small(transparent_surface);
    transparent_surface.surface.enabled = true;
    transparent_surface.surface.mapping = pvt::SurfaceMapping::Sphere;
    transparent_surface.surface.curvature = 1.0;
    transparent_surface.alpha.enabled = false;
    transparent_surface.output.bit_depth = 32;
    CHECK(pvt::render_frame(transparent_surface, 2, transparent_image, &error));
    const fs::path transparent_exr = directory / "transparent-surface.exr";
    CHECK(!pvt::write_image(transparent_exr.string(), transparent_image,
                            transparent_surface, 13U, &error));
    CHECK(error.find("Alpha output") != std::string::npos);
    CHECK(!fs::exists(transparent_exr));
    transparent_surface.alpha.enabled = true;
    CHECK(pvt::render_frame(transparent_surface, 2, transparent_image, &error));
    CHECK(pvt::write_image(transparent_exr.string(), transparent_image,
                           transparent_surface, 13U, &error));
    channels = exr_channels(transparent_exr);
    CHECK(channels.size() == 4U);
    CHECK(std::all_of(channels.begin(), channels.end(),
                      [](const auto& channel) { return channel.second == 2U; }));
}

void test_sequence_preflight(const fs::path& directory) {
    auto config = pvt::default_config();
    make_small(config);
    config.width = 32;
    config.height = 24;
    config.block_size = 4;
    config.total_frames = 3;
    const std::string unicode_directory_name = "sequence-\xCE\xB1";
    const std::string unicode_prefix = "loop-\xCE\xB2_";
    const fs::path sequence_directory =
        directory / pvt::detail::path_from_utf8(unicode_directory_name);
    config.output.output_directory = pvt::detail::path_to_utf8(sequence_directory);
    config.output.filename_prefix = unicode_prefix;
    config.output.bit_depth = 8;
    config.output.overwrite_existing = false;
    std::string error;
    std::vector<int> progress_values;
    CHECK(pvt::render_sequence(
        config,
        [&progress_values](int completed, int total) {
            CHECK(completed >= 1 && completed <= total);
            progress_values.push_back(completed);
            return true;
        },
        nullptr, &error));
    CHECK(progress_values == std::vector<int>({1, 2, 3}));
    CHECK(fs::exists(sequence_directory
                     / pvt::detail::path_from_utf8(unicode_prefix + "0000.png")));
    CHECK(!pvt::render_sequence(config, {}, nullptr, &error));

    const fs::path late_collision_directory = directory / "late-collision";
    fs::create_directories(late_collision_directory);
    const fs::path late_collision = late_collision_directory / "loop_0002.png";
    {
        std::ofstream output(late_collision, std::ios::binary);
        output << "preexisting";
    }
    const auto collision_bytes = read_bytes(late_collision);
    config.output.filename_prefix = "loop_";
    config.output.output_directory = late_collision_directory.string();
    CHECK(!pvt::render_sequence(config, {}, nullptr, &error));
    CHECK(!fs::exists(late_collision_directory / "loop_0000.png"));
    CHECK(!fs::exists(late_collision_directory / "loop_0001.png"));
    CHECK(read_bytes(late_collision) == collision_bytes);

    // Parallel scheduling must be byte-deterministic and retain ascending,
    // single-threaded callback semantics. A one-worker run is the reference.
    config.total_frames = 6;
    config.output.png_compression_level = 0;
    config.output.filename_prefix = "deterministic_";
    const fs::path sequential_directory = directory / "workers-one";
    const fs::path parallel_directory = directory / "workers-four";
    pvt::SequenceRenderOptions sequence_options;
    sequence_options.worker_count = 1U;
    config.output.output_directory = sequential_directory.string();
    CHECK(pvt::render_sequence(config, sequence_options, {}, nullptr, &error));
    sequence_options.worker_count = 4U;
    config.output.output_directory = parallel_directory.string();
    progress_values.clear();
    CHECK(pvt::render_sequence(
        config, sequence_options,
        [&progress_values](int completed, int) {
            progress_values.push_back(completed);
            return true;
        },
        nullptr, &error));
    CHECK(progress_values == std::vector<int>({1, 2, 3, 4, 5, 6}));
    for (int frame = 0; frame < config.total_frames; ++frame) {
        std::string number = std::to_string(frame);
        number.insert(0U, 4U - number.size(), '0');
        const fs::path filename = "deterministic_" + number + ".png";
        CHECK(read_bytes(sequential_directory / filename)
              == read_bytes(parallel_directory / filename));
    }

    sequence_options.worker_count = pvt::kMaximumSequenceWorkers + 1U;
    config.output.output_directory = (directory / "too-many-workers").string();
    CHECK(!pvt::render_sequence(config, sequence_options, {}, nullptr, &error));
    CHECK(error.find("worker count") != std::string::npos);
    CHECK(!fs::exists(directory / "too-many-workers" / "deterministic_0000.png"));

    config.total_frames = 3;
    config.output.filename_prefix = "loop_";
    config.output.output_directory = (directory / "callback-cancel").string();
    int callback_calls = 0;
    CHECK(!pvt::render_sequence(
        config,
        [&callback_calls](int, int) {
            ++callback_calls;
            return false;
        },
        nullptr, &error));
    CHECK(callback_calls == 1);
    CHECK(fs::exists(directory / "callback-cancel" / "loop_0000.png"));
    CHECK(!fs::exists(directory / "callback-cancel" / "loop_0001.png"));
    CHECK(!has_temporary_output(directory / "callback-cancel"));

    config.output.output_directory = (directory / "callback-throw").string();
    CHECK(!pvt::render_sequence(
        config,
        [](int, int) -> bool { throw std::runtime_error("test callback failure"); },
        nullptr, &error));
    CHECK(error.find("callback failed") != std::string::npos);
    CHECK(!has_temporary_output(directory / "callback-throw"));

    config.output.output_directory = (directory / "atomic-cancel").string();
    std::atomic_bool cancelled {true};
    CHECK(!pvt::render_sequence(config, {}, &cancelled, &error));
    CHECK(!fs::exists(directory / "atomic-cancel" / "loop_0000.png"));

    // A sequence must pass its cancellation token into the active frame, not
    // wait until a potentially expensive frame has completed. Use the same
    // valid maximum-wave workload as the direct in-flight cancellation test.
    auto heavy = pvt::default_config();
    heavy.width = 1024;
    heavy.height = 1024;
    heavy.block_size = 1;
    heavy.total_frames = 2;
    heavy.output.output_directory = (directory / "in-frame-cancel").string();
    heavy.output.filename_prefix = "heavy_";
    heavy.waves.reserve(pvt::kMaximumWaves);
    for (std::size_t index = heavy.waves.size();
         index < pvt::kMaximumWaves; ++index) {
        auto wave = pvt::default_wave(index);
        wave.id = static_cast<std::uint64_t>(2000U + index);
        heavy.waves.push_back(std::move(wave));
    }
    CHECK(pvt::validate(heavy).ok);
    cancelled.store(false, std::memory_order_relaxed);
    std::atomic_bool sequence_entered {false};
    std::atomic_bool sequence_finished {false};
    bool sequence_result = true;
    std::thread sequence_worker([&] {
        sequence_entered.store(true, std::memory_order_release);
        sequence_result = pvt::render_sequence(heavy, {}, &cancelled, &error);
        sequence_finished.store(true, std::memory_order_release);
    });
    while (!sequence_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!sequence_finished.load(std::memory_order_acquire));
    cancelled.store(true, std::memory_order_relaxed);
    sequence_worker.join();
    CHECK(!sequence_result);
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(!fs::exists(directory / "in-frame-cancel" / "heavy_0000.png"));
    CHECK(!has_temporary_output(directory / "in-frame-cancel"));

    // Music duration replaces only the effective export count. The manually
    // authored count remains intact for switching back to another clock.
    pvt::RenderConfig music_sequence = pvt::default_config();
    make_small(music_sequence);
    music_sequence.width = 16;
    music_sequence.height = 16;
    music_sequence.total_frames = 2;
    music_sequence.fps = 24.0;
    music_sequence.clock = ready_music_clock(0.11);
    music_sequence.output.output_directory =
        (directory / "music-effective-count").string();
    music_sequence.output.filename_prefix = "music_";
    music_sequence.output.png_compression_level = 0;
    progress_values.clear();
    CHECK(pvt::effective_frame_count(music_sequence, &error) == 3);
    CHECK(pvt::render_sequence(
        music_sequence,
        [&progress_values](int completed, int total) {
            CHECK(total == 3);
            progress_values.push_back(completed);
            return true;
        }, nullptr, &error));
    CHECK(progress_values == std::vector<int>({1, 2, 3}));
    CHECK(music_sequence.total_frames == 2);
    CHECK(fs::exists(directory / "music-effective-count" / "music_0002.png"));
    CHECK(!fs::exists(directory / "music-effective-count" / "music_0003.png"));

    // A literal dot remains relative to the caller's working directory, and
    // sibling temporary output must stay there rather than drifting to root.
    const fs::path previous_working_directory = fs::current_path();
    const fs::path relative_output_directory = directory / "relative-dot";
    fs::create_directories(relative_output_directory);
    fs::current_path(relative_output_directory);
    config.output.output_directory = ".";
    config.output.filename_prefix = "relative_";
    config.total_frames = 2;
    config.output.overwrite_existing = false;
    CHECK(pvt::render_sequence(config, {}, nullptr, &error));
    fs::current_path(previous_working_directory);
    CHECK(fs::exists(relative_output_directory / "relative_0000.png"));
    CHECK(fs::exists(relative_output_directory / "relative_0001.png"));
}

} // namespace

int main(int argc, char** argv) {
    const fs::path source_root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const long process_id =
#if defined(_WIN32)
        static_cast<long>(::_getpid());
#else
        static_cast<long>(::getpid());
#endif
    const fs::path test_directory = fs::temp_directory_path()
                                    / ("pvt-tests-" + std::to_string(process_id));
    std::error_code ignored;
    fs::remove_all(test_directory, ignored);
    fs::create_directories(test_directory, ignored);
    CHECK(!ignored);

    test_defaults_and_dynamic_collections();
    test_synchronized_clocks_and_music();
    test_image_access_and_transactional_render();
    test_cancellable_single_layer_render();
    test_starting_images_and_reusable_paths(test_directory);
    test_determinism_and_seam_continuity();
    test_direction_alpha_and_surfaces(source_root);
    test_partial_alpha_glow_composition();
    test_particle_straight_alpha_emission();
    test_block_scale_and_default_glow_visibility();
    test_palettes_transforms_and_spatial_stages();
    test_validation_limits();
    test_setup_round_trip_and_transaction(test_directory);
    test_maximum_music_analysis_setup(test_directory);
    test_image_formats_and_dither(test_directory);
    test_sequence_preflight(test_directory);

    fs::remove_all(test_directory, ignored);
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All procedural visualizer tests passed.\n";
    return 0;
}
