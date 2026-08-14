#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (!(expression)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                             \
                      << ": check failed: " #expression "\n";                  \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

bool close_enough(double first, double second, double tolerance = 1.0e-6) {
    return std::fabs(first - second) <= tolerance;
}

pvt::Image solid(float value, float alpha = 1.0F) {
    pvt::Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = {value, value, value, alpha};
    return image;
}

void expect_opaque_blend(pvt::BlendMode mode, double expected) {
    pvt::Image backdrop = solid(0.25F);
    const pvt::Image source = solid(0.75F);
    std::string error;
    CHECK(pvt::composite_over(source, backdrop, mode, 1.0, &error));
    CHECK(error.empty());
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        CHECK(close_enough(backdrop.pixels[channel], expected));
    }
    CHECK(backdrop.pixels[3U] == 1.0F);
}

double maximum_difference(const pvt::Image& first, const pvt::Image& second) {
    if (first.width != second.width || first.height != second.height
        || first.pixels.size() != second.pixels.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double difference = 0.0;
    for (std::size_t index = 0U; index < first.pixels.size(); ++index) {
        difference = std::max(
            difference,
            std::fabs(static_cast<double>(first.pixels[index])
                      - static_cast<double>(second.pixels[index])));
    }
    return difference;
}

void make_small(pvt::ProjectConfig& project) {
    project.canvas.width = 16;
    project.canvas.height = 16;
    project.canvas.block_size = 2;
    project.canvas.total_frames = 8;
    project.canvas.fps = 24.0;
}

pvt::ClockConfig ready_music_clock(double duration_seconds) {
    pvt::ClockConfig clock;
    clock.mode = pvt::ClockMode::Music;
    clock.music.analyzer_version = "project-test-1";
    clock.music.source_sha256 = std::string(64U, 'b');
    clock.music.source_basename = "project-track.wav";
    clock.music.source_format = "wav-f32";
    clock.music.source_sample_rate = 1000U;
    clock.music.source_frame_count = static_cast<std::uint64_t>(
        std::llround(duration_seconds * 1000.0));
    clock.music.source_channel_count = 2U;
    clock.music.duration_seconds =
        static_cast<double>(clock.music.source_frame_count) / 1000.0;
    clock.music.detected_bpm = 120.0;
    clock.music.tempo_confidence = 0.9;
    clock.music.beat_times_seconds = {0.0,
        0.5 * clock.music.duration_seconds};
    return clock;
}

void test_uuid_factories_and_adapters() {
    const std::string first_uuid = pvt::generate_uuid();
    const std::string second_uuid = pvt::generate_uuid();
    CHECK(first_uuid.size() == 36U);
    CHECK(first_uuid[14U] == '4');
    CHECK(first_uuid[19U] == '8' || first_uuid[19U] == '9'
          || first_uuid[19U] == 'a' || first_uuid[19U] == 'b');
    CHECK(first_uuid != second_uuid);

    const pvt::ProjectConfig first = pvt::default_project();
    const pvt::ProjectConfig second = pvt::default_project();
    CHECK(first.uuid != second.uuid);
    CHECK(first.layers.size() == 1U);
    CHECK(first.layers[0U].uuid != second.layers[0U].uuid);
    CHECK(first.layers[0U].file_id == 0U);
    CHECK(pvt::validate(first).ok);

    pvt::LayerConfig layer = pvt::default_layer(2U);
    CHECK(layer.file_id == 2U);
    CHECK(layer.name == "Layer 3");
    pvt::ProjectConfig allocation = first;
    layer.file_id = 2U;
    allocation.layers.push_back(layer);
    CHECK(pvt::allocate_layer_file_id(allocation) == 3U);

    const pvt::RenderConfig materialized = pvt::apply_global_config(
        first.canvas, first.output, first.layers[0U].render);
    CHECK(materialized.width == first.canvas.width);
    CHECK(materialized.height == first.canvas.height);
    CHECK(materialized.waves.size() == first.layers[0U].render.waves.size());
    CHECK(materialized.clock.mode == first.canvas.clock.mode);
    CHECK(pvt::allocate_id(first.layers[0U].render)
          == pvt::allocate_id(materialized));
}

void test_project_audio_response_inheritance() {
    pvt::ProjectConfig project = pvt::default_project();
    make_small(project);
    CHECK(!project.canvas.audio_reactive_defaults.enabled);
    CHECK(!project.layers.front().render.audio_reactive_override_enabled);

    project.canvas.fps = 10.0;
    project.canvas.total_frames = 10;
    project.canvas.clock = ready_music_clock(1.0);
    project.canvas.clock.interpolation = pvt::ClockInterpolation::Hold;
    project.canvas.clock.music.feature_samples.assign(11U, {});
    project.canvas.clock.music.feature_samples[5U].energy = 1.0F;

    auto& render = project.layers.front().render;
    render.waves.clear();
    render.swings.clear();
    render.effects.clear();
    render.swings_enabled = false;
    render.displacement_enabled = false;
    render.lighting_enabled = false;
    render.spiral_enabled = false;
    render.wall_reflection_enabled = false;
    render.ghost_mix = 0.0;
    render.hue_cycles = 0;

    auto& defaults = project.canvas.audio_reactive_defaults;
    defaults.enabled = true;
    defaults.waves_enabled = false;
    defaults.effects_enabled = false;
    defaults.color_enabled = true;
    defaults.color_source = pvt::MusicFeature::Energy;
    defaults.color_amount_degrees = 180.0;

    std::string error;
    pvt::Image before_spike;
    pvt::Image at_spike;
    CHECK(pvt::render_project_frame(project, 4, before_spike, nullptr, &error));
    CHECK(pvt::render_project_frame(project, 5, at_spike, nullptr, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    // An explicit layer override is authoritative without mutating the shared
    // project profile. Turning the override back off restores inheritance.
    render.audio_reactive_override_enabled = true;
    render.audio_reactive = defaults;
    render.audio_reactive.enabled = false;
    CHECK(pvt::render_project_frame(project, 4, before_spike, nullptr, &error));
    CHECK(pvt::render_project_frame(project, 5, at_spike, nullptr, &error));
    CHECK(before_spike.pixels == at_spike.pixels);
    CHECK(project.canvas.audio_reactive_defaults.enabled);

    render.audio_reactive_override_enabled = false;
    CHECK(pvt::render_project_frame(project, 4, before_spike, nullptr, &error));
    CHECK(pvt::render_project_frame(project, 5, at_spike, nullptr, &error));
    CHECK(before_spike.pixels != at_spike.pixels);
}

void test_blend_modes() {
    expect_opaque_blend(pvt::BlendMode::Normal, 0.75);
    expect_opaque_blend(pvt::BlendMode::SoftLight, 0.375);
    expect_opaque_blend(pvt::BlendMode::GrainMerge, 0.5);
    expect_opaque_blend(pvt::BlendMode::Overlay, 0.375);
    expect_opaque_blend(pvt::BlendMode::ColorDodge, 1.0);
    expect_opaque_blend(pvt::BlendMode::LinearBurn, 0.0);
    expect_opaque_blend(pvt::BlendMode::ColorBurn, 0.0);
    expect_opaque_blend(pvt::BlendMode::Difference, 0.5);
    expect_opaque_blend(pvt::BlendMode::Subtract, 0.0);
    expect_opaque_blend(pvt::BlendMode::Multiply, 0.1875);
    expect_opaque_blend(pvt::BlendMode::Add, 1.0);

    // Destination-out modes preserve straight RGB while changing only the
    // already-composited lower coverage.
    pvt::Image erased = solid(0.25F, 0.8F);
    std::string erase_error;
    CHECK(pvt::composite_over(solid(0.75F, 0.5F), erased,
                              pvt::BlendMode::Erase, 1.0, &erase_error));
    CHECK(close_enough(erased.pixels[0U], 0.25));
    CHECK(close_enough(erased.pixels[3U], 0.4));

    erased = solid(0.25F, 1.0F);
    CHECK(pvt::composite_over(solid(0.25F, 1.0F), erased,
                              pvt::BlendMode::ColorEraseTones, 1.0,
                              &erase_error));
    CHECK(close_enough(erased.pixels[3U], 0.0));
    erased = solid(0.25F, 1.0F);
    CHECK(pvt::composite_over(solid(0.75F, 1.0F), erased,
                              pvt::BlendMode::ColorEraseBrightness, 1.0,
                              &erase_error));
    CHECK(close_enough(erased.pixels[3U], 0.0));

    std::string error;
    pvt::Image backdrop = solid(0.75F);
    CHECK(pvt::composite_over(solid(0.25F), backdrop,
                              pvt::BlendMode::Overlay, 1.0, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.625));

    backdrop = solid(0.75F);
    CHECK(pvt::composite_over(solid(0.25F), backdrop,
                              pvt::BlendMode::SoftLight, 1.0, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.65625));

    backdrop = solid(0.75F);
    CHECK(pvt::composite_over(solid(0.25F), backdrop,
                              pvt::BlendMode::Subtract, 1.0, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.5));

    backdrop = solid(0.0F);
    CHECK(pvt::composite_over(solid(1.0F), backdrop,
                              pvt::BlendMode::ColorDodge, 1.0, &error));
    CHECK(backdrop.pixels[0U] == 0.0F); // Defined 0/0 singularity.
    backdrop = solid(1.0F);
    CHECK(pvt::composite_over(solid(0.0F), backdrop,
                              pvt::BlendMode::ColorBurn, 1.0, &error));
    CHECK(backdrop.pixels[0U] == 1.0F); // Defined 0/0 singularity.

    backdrop = solid(0.5F);
    CHECK(pvt::composite_over(solid(2.0F), backdrop,
                              pvt::BlendMode::Normal, 1.0, &error));
    CHECK(backdrop.pixels[0U] == 2.0F); // Normal preserves HDR.
    backdrop = solid(0.5F);
    CHECK(pvt::composite_over(solid(2.0F), backdrop,
                              pvt::BlendMode::Add, 1.0, &error));
    CHECK(backdrop.pixels[0U] == 1.0F); // Unit-domain artistic blend.

    const std::vector<pvt::BlendMode> modes = {
        pvt::BlendMode::Normal, pvt::BlendMode::SoftLight,
        pvt::BlendMode::GrainMerge, pvt::BlendMode::Overlay,
        pvt::BlendMode::ColorDodge, pvt::BlendMode::LinearBurn,
        pvt::BlendMode::ColorBurn, pvt::BlendMode::Difference,
        pvt::BlendMode::Subtract, pvt::BlendMode::Multiply,
        pvt::BlendMode::Add, pvt::BlendMode::Erase,
        pvt::BlendMode::ColorEraseTones,
        pvt::BlendMode::ColorEraseBrightness};
    for (const pvt::BlendMode mode : modes) {
        CHECK(std::string(pvt::blend_mode_name(mode)) != "Unknown blend mode");
        pvt::Image finite = solid(-4.0F, 0.37F);
        CHECK(pvt::composite_over(solid(8.0F, 0.61F), finite,
                                  mode, 0.73, &error));
        for (const float component : finite.pixels) {
            CHECK(std::isfinite(component));
        }
        CHECK(finite.pixels[3U] >= 0.0F && finite.pixels[3U] <= 1.0F);
    }
}

void test_straight_alpha_and_transactionality() {
    std::string error;
    pvt::Image backdrop = solid(0.2F, 0.4F);
    const pvt::Image source = solid(0.8F, 0.5F);
    CHECK(pvt::composite_over(source, backdrop, pvt::BlendMode::Normal,
                              1.0, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.44 / 0.7));
    CHECK(close_enough(backdrop.pixels[3U], 0.7));

    backdrop = solid(0.2F, 0.4F);
    CHECK(pvt::composite_over(source, backdrop, pvt::BlendMode::Multiply,
                              1.0, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.312 / 0.7));
    CHECK(close_enough(backdrop.pixels[3U], 0.7));

    backdrop = solid(0.2F, 0.4F);
    CHECK(pvt::composite_over(source, backdrop, pvt::BlendMode::Normal,
                              0.5, &error));
    CHECK(close_enough(backdrop.pixels[0U], 0.26 / 0.55));
    CHECK(close_enough(backdrop.pixels[3U], 0.55));

    backdrop = solid(0.2F, 0.0F);
    CHECK(pvt::composite_over(source, backdrop, pvt::BlendMode::Difference,
                              0.5, &error));
    CHECK(backdrop.pixels[0U] == source.pixels[0U]);
    CHECK(close_enough(backdrop.pixels[3U], 0.25));

    backdrop = solid(0.2F, 0.4F);
    const pvt::Image before = backdrop;
    CHECK(pvt::composite_over(solid(9.0F, 0.0F), backdrop,
                              pvt::BlendMode::Add, 1.0, &error));
    CHECK(backdrop.pixels == before.pixels);
    CHECK(pvt::composite_over(source, backdrop, pvt::BlendMode::Add,
                              0.0, &error));
    CHECK(backdrop.pixels == before.pixels);

    pvt::Image invalid = source;
    invalid.pixels[0U] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!pvt::composite_over(invalid, backdrop, pvt::BlendMode::Normal,
                               1.0, &error));
    CHECK(backdrop.pixels == before.pixels);
    CHECK(!pvt::composite_over(source, backdrop,
                               static_cast<pvt::BlendMode>(255), 1.0, &error));
    CHECK(backdrop.pixels == before.pixels);
    CHECK(!pvt::composite_over(source, backdrop, pvt::BlendMode::Normal,
                               std::numeric_limits<double>::infinity(), &error));
    CHECK(backdrop.pixels == before.pixels);
}

void test_project_validation() {
    pvt::ProjectConfig project = pvt::default_project();
    make_small(project);
    CHECK(pvt::validate(project).ok);

    pvt::ProjectConfig invalid = project;
    invalid.name = "bad/name";
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.name = std::string("bad") + static_cast<char>(0xc0)
                   + static_cast<char>(0xaf);
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.uuid[0U] = 'A';
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.layers[0U].name = std::string("bad") + static_cast<char>(0xc0)
                              + static_cast<char>(0xaf);
    CHECK(!pvt::validate(invalid).ok);

    pvt::LayerConfig second = pvt::default_layer(1U);
    project.layers.push_back(second);
    // Multiple opaque layers may intentionally export RGB.
    CHECK(pvt::validate(project).ok);

    // A translucent upper layer cannot make an already-opaque lower stack
    // transparent. RGB export is therefore safe even though the upper render
    // data can produce alpha below one.
    project.layers[1U].opacity = 0.5;
    project.layers[1U].render.alpha.enabled = true;
    project.layers[1U].render.alpha.minimum = 0.0;
    CHECK(pvt::validate(project).ok);
    project.layers[1U].opacity = 1.0;
    project.layers[1U].render.alpha.enabled = false;
    project.layers[1U].render.alpha.minimum = 1.0;

    // Per-layer rendering is always RGBA in memory. A transparent-edge upper
    // layer must therefore render successfully even when the opaque base makes
    // the final project safe to export as RGB.
    project.layers[1U].render.effects.front().enabled = true;
    project.layers[1U].render.effects.front().edge_mode = pvt::EdgeMode::Alpha;
    project.layers[1U].render.effects.front().magnitude = 0.1;
    CHECK(pvt::validate(project).ok);
    pvt::Image rgb_composite;
    std::string render_error;
    CHECK(pvt::render_project_frame(project, 0, rgb_composite, nullptr,
                                    &render_error));
    project.layers[1U].render.effects.front().enabled = false;

    // Particle Field ignores its serialized edge mode and only adds coverage;
    // a hidden/randomized Alpha value must not make an opaque RGB layer fail.
    pvt::ProjectConfig particle_project = pvt::default_project();
    make_small(particle_project);
    auto particle = pvt::default_effect(pvt::EffectType::ParticleField);
    particle.id = pvt::allocate_id(particle_project.layers.front().render);
    particle.enabled = true;
    particle.edge_mode = pvt::EdgeMode::Alpha;
    particle_project.layers.front().render.effects.push_back(particle);
    particle_project.output.write_alpha = false;
    CHECK(pvt::validate(particle_project).ok);

    invalid = project;
    invalid.layers[1U].uuid = invalid.layers[0U].uuid;
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.layers[1U].file_id = invalid.layers[0U].file_id;
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.layers[1U].opacity = std::numeric_limits<double>::quiet_NaN();
    CHECK(!pvt::validate(invalid).ok);
    invalid = project;
    invalid.layers[1U].blend_mode = static_cast<pvt::BlendMode>(255);
    CHECK(!pvt::validate(invalid).ok);

    // Disabled transparent content is structurally validated but does not
    // dictate the final channel choice when another opaque layer contributes.
    project.layers[1U].enabled = false;
    project.layers[1U].render.alpha.enabled = true;
    project.layers[1U].render.alpha.minimum = 0.0;
    CHECK(pvt::validate(project).ok);

    invalid = project;
    invalid.layers[0U].enabled = false;
    CHECK(!pvt::validate(invalid).ok); // Empty enabled stack needs RGBA.
    invalid.output.write_alpha = true;
    CHECK(pvt::validate(invalid).ok);

    invalid = pvt::default_project();
    make_small(invalid);
    invalid.layers[0U].opacity = 0.5;
    CHECK(!pvt::validate(invalid).ok);
    invalid.output.write_alpha = true;
    CHECK(pvt::validate(invalid).ok);

    invalid = pvt::default_project();
    make_small(invalid);
    invalid.layers[0U].render.alpha.enabled = true;
    invalid.layers[0U].render.alpha.minimum = 0.0;
    CHECK(!pvt::validate(invalid).ok);
    invalid.output.write_alpha = true;
    CHECK(pvt::validate(invalid).ok);

    invalid = pvt::default_project();
    make_small(invalid);
    for (std::size_t index = 1U; index <= pvt::kMaximumLayers; ++index) {
        invalid.layers.push_back(pvt::default_layer(index));
    }
    invalid.output.write_alpha = true;
    CHECK(invalid.layers.size() == pvt::kMaximumLayers + 1U);
    CHECK(!pvt::validate(invalid).ok);

    // Each individual default layer fits, but project transactionality plus
    // the accumulator must remain inside the same 1 GiB process budget.
    invalid = pvt::default_project();
    invalid.canvas.width = 5000;
    invalid.canvas.height = 5000;
    invalid.canvas.block_size = 16;
    CHECK(!pvt::validate(invalid).ok);
}

void test_project_rendering_order_equivalence_and_seam() {
    std::string error;
    pvt::ProjectConfig project = pvt::default_project();
    make_small(project);

    const pvt::RenderConfig legacy = pvt::apply_global_config(
        project.canvas, project.output, project.layers[0U].render);
    pvt::Image legacy_image;
    pvt::Image project_image;
    CHECK(pvt::render_frame(legacy, 3, legacy_image, &error));
    CHECK(pvt::render_project_frame(project, 3, project_image, nullptr, &error));
    CHECK(project_image.pixels == legacy_image.pixels);

    pvt::LayerConfig top = pvt::default_layer(1U);
    top.render.hue_cycles = -3;
    top.render.saturation = 0.65;
    top.render.alpha.enabled = true;
    top.render.alpha.minimum = 0.5;
    top.render.alpha.maximum = 0.5;
    project.layers.push_back(top);
    project.output.write_alpha = true;
    CHECK(pvt::validate(project).ok);

    pvt::Image bottom_image;
    pvt::Image top_image;
    CHECK(pvt::render_frame_at_phase(
        pvt::apply_global_config(project.canvas, project.output,
                                 project.layers[0U].render),
        0.375, bottom_image, &error));
    CHECK(pvt::render_frame_at_phase(
        pvt::apply_global_config(project.canvas, project.output,
                                 project.layers[1U].render),
        0.375, top_image, &error));
    pvt::Image expected = bottom_image;
    CHECK(pvt::composite_over(top_image, expected, pvt::BlendMode::Normal,
                              1.0, &error));
    CHECK(pvt::render_project_frame_at_phase(project, 0.375, project_image,
                                              nullptr, &error));
    CHECK(project_image.pixels == expected.pixels);

    project.layers[1U].blend_mode = pvt::BlendMode::Multiply;
    expected = bottom_image;
    CHECK(pvt::composite_over(top_image, expected, pvt::BlendMode::Multiply,
                              1.0, &error));
    CHECK(pvt::render_project_frame_at_phase(project, 0.375, project_image,
                                              nullptr, &error));
    CHECK(project_image.pixels == expected.pixels);

    const pvt::Image ordered = project_image;
    std::swap(project.layers[0U], project.layers[1U]);
    CHECK(pvt::render_project_frame_at_phase(project, 0.375, project_image,
                                              nullptr, &error));
    CHECK(maximum_difference(ordered, project_image) > 1.0e-4);

    std::swap(project.layers[0U], project.layers[1U]);
    const std::vector<pvt::BlendMode> modes = {
        pvt::BlendMode::Normal, pvt::BlendMode::SoftLight,
        pvt::BlendMode::GrainMerge, pvt::BlendMode::Overlay,
        pvt::BlendMode::ColorDodge, pvt::BlendMode::LinearBurn,
        pvt::BlendMode::ColorBurn, pvt::BlendMode::Difference,
        pvt::BlendMode::Subtract, pvt::BlendMode::Multiply,
        pvt::BlendMode::Add, pvt::BlendMode::Erase,
        pvt::BlendMode::ColorEraseTones,
        pvt::BlendMode::ColorEraseBrightness};
    for (const pvt::BlendMode mode : modes) {
        project.layers[1U].blend_mode = mode;
        pvt::Image phase_zero;
        pvt::Image phase_one;
        pvt::Image before_seam;
        pvt::Image after_seam;
        CHECK(pvt::render_project_frame_at_phase(project, 0.0, phase_zero,
                                                  nullptr, &error));
        CHECK(pvt::render_project_frame_at_phase(project, 1.0, phase_one,
                                                  nullptr, &error));
        CHECK(phase_zero.pixels == phase_one.pixels);
        constexpr double epsilon = 1.0e-7;
        CHECK(pvt::render_project_frame_at_phase(project, 1.0 - epsilon,
                                                  before_seam, nullptr, &error));
        CHECK(pvt::render_project_frame_at_phase(project, epsilon,
                                                  after_seam, nullptr, &error));
        const double seam_difference =
            maximum_difference(before_seam, after_seam);
        if (!(seam_difference < 0.01)) {
            std::cerr << "near-seam difference for "
                      << pvt::blend_mode_name(mode) << ": "
                      << seam_difference << '\n';
        }
        CHECK(seam_difference < 0.01);
    }

    std::atomic_bool cancelled {true};
    const pvt::Image preserved = project_image;
    CHECK(!pvt::render_project_frame(project, 0, project_image, &cancelled,
                                     &error));
    CHECK(project_image.pixels == preserved.pixels);

    for (pvt::LayerConfig& layer : project.layers) {
        layer.enabled = false;
    }
    project.output.write_alpha = true;
    CHECK(pvt::render_project_frame(project, 0, project_image, nullptr, &error));
    CHECK(std::all_of(project_image.pixels.begin(), project_image.pixels.end(),
                      [](float value) { return value == 0.0F; }));
}

void test_project_cancellation_during_layer_render() {
    pvt::ProjectConfig project = pvt::default_project();
    project.canvas.width = 1024;
    project.canvas.height = 1024;
    project.canvas.block_size = 1;
    pvt::RenderData& render = project.layers[0U].render;
    render.waves.reserve(pvt::kMaximumWaves);
    for (std::size_t index = render.waves.size();
        index < pvt::kMaximumWaves; ++index) {
        pvt::WaveConfig wave = pvt::default_wave(index);
        wave.id = static_cast<std::uint64_t>(1000U + index);
        render.waves.push_back(std::move(wave));
    }
    CHECK(pvt::validate(project).ok);

    pvt::Image destination;
    destination.width = 1;
    destination.height = 1;
    destination.pixels = {0.9F, 0.8F, 0.7F, 0.6F};
    const pvt::Image preserved = destination;
    std::atomic_bool cancel {false};
    std::atomic_bool entered {false};
    std::atomic_bool finished {false};
    bool rendered = true;
    std::string error;

    std::thread worker([&] {
        entered.store(true, std::memory_order_release);
        rendered = pvt::render_project_frame_at_phase(
            project, 0.25, destination, &cancel, &error);
        finished.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!finished.load(std::memory_order_acquire));
    cancel.store(true, std::memory_order_relaxed);
    worker.join();

    CHECK(!rendered);
    CHECK(error.find("cancelled") != std::string::npos);
    CHECK(destination.width == preserved.width);
    CHECK(destination.height == preserved.height);
    CHECK(destination.pixels == preserved.pixels);
}

void test_project_sequence() {
    pvt::ProjectConfig project = pvt::default_project();
    make_small(project);
    project.canvas.total_frames = 2;
    const fs::path directory = fs::temp_directory_path()
                               / ("pvt-project-composite-" + pvt::generate_uuid());
    project.output.output_directory = directory.string();
    project.output.filename_prefix = "layered_";
    project.output.png_compression_level = 0;
    project.output.overwrite_existing = false;
    int progress_calls = 0;
    std::string error;
    CHECK(pvt::render_project_sequence(
        project,
        [&progress_calls](int completed, int total) {
            ++progress_calls;
            return completed <= total;
        },
        nullptr, &error));
    CHECK(progress_calls == 2);
    CHECK(fs::exists(directory / "layered_0000.png"));
    CHECK(fs::exists(directory / "layered_0001.png"));

    project.output.output_directory = (directory / "cancelled").string();
    std::atomic_bool cancelled {true};
    CHECK(!pvt::render_project_sequence(project, {}, &cancelled, &error));
    CHECK(!fs::exists(directory / "cancelled" / "layered_0000.png"));

    project.canvas.clock = ready_music_clock(0.11);
    project.canvas.total_frames = 2;
    project.canvas.fps = 24.0;
    project.output.output_directory = (directory / "music").string();
    project.output.filename_prefix = "music_";
    progress_calls = 0;
    cancelled.store(false, std::memory_order_relaxed);
    CHECK(pvt::effective_frame_count(project.canvas, &error) == 3);
    CHECK(pvt::render_project_sequence(
        project,
        [&progress_calls](int completed, int total) {
            CHECK(total == 3);
            CHECK(completed == ++progress_calls);
            return true;
        }, nullptr, &error));
    CHECK(progress_calls == 3);
    CHECK(project.canvas.total_frames == 2);
    CHECK(fs::exists(directory / "music" / "music_0000.png"));
    CHECK(fs::exists(directory / "music" / "music_0002.png"));
    CHECK(!fs::exists(directory / "music" / "music_0003.png"));

    const pvt::RenderConfig layer_render = pvt::apply_global_config(
        project.canvas, project.output, project.layers.front().render);
    pvt::Image project_frame;
    pvt::Image layer_frame;
    CHECK(pvt::render_project_frame(project, 2, project_frame, nullptr, &error));
    CHECK(pvt::render_frame(layer_render, 2, layer_frame, &error));
    CHECK(project_frame.pixels == layer_frame.pixels);

    std::error_code ignored;
    fs::remove_all(directory, ignored);
    CHECK(!ignored);
}

void test_active_layer_clock_mappings() {
    pvt::ProjectConfig project = pvt::default_project();
    make_small(project);
    project.canvas.fps = 10.0;
    project.canvas.clock = ready_music_clock(10.0);
    auto& local = project.layers.front().render.layer_clock;
    CHECK(local.clock.data_only); // Layer sources begin silent by design.
    local.enabled = true;
    local.clock = ready_music_clock(3.0);
    local.clock.data_only = true;
    local.clock.music.source_sha256 = std::string(64U, 'c');
    local.clock.music.source_basename = "layer-pulse.wav";
    CHECK(pvt::validate(project).ok);

    pvt::RenderConfig local_render = pvt::apply_global_config(
        project.canvas, project.output, project.layers.front().render);
    local_render.clock = local.clock;
    pvt::RenderConfig project_render = pvt::apply_global_config(
        project.canvas, project.output, project.layers.front().render);
    project_render.layer_clock.enabled = false;

    const auto expect_local_frame = [&](pvt::LayerClockScale scale,
                                        int master_frame,
                                        int expected_local_frame) {
        project.layers.front().render.layer_clock.scale = scale;
        pvt::Image actual;
        pvt::Image expected;
        std::string error;
        CHECK(pvt::render_project_frame(project, master_frame, actual,
                                        nullptr, &error));
        CHECK(pvt::render_frame(local_render, expected_local_frame, expected,
                                &error));
        CHECK(actual.pixels == expected.pixels);
    };
    expect_local_frame(pvt::LayerClockScale::SmartLoopFit, 20, 18);
    expect_local_frame(pvt::LayerClockScale::StraightFit, 20, 6);
    expect_local_frame(pvt::LayerClockScale::PlayOnce, 50, 29);
    expect_local_frame(pvt::LayerClockScale::OriginalSpeedLoop, 20, 20);

    project.layers.front().render.layer_clock.scale =
        pvt::LayerClockScale::PlayOnceThenProject;
    pvt::Image fallback;
    pvt::Image project_expected;
    std::string error;
    CHECK(pvt::render_project_frame(project, 50, fallback, nullptr, &error));
    CHECK(pvt::render_frame(project_render, 50, project_expected, &error));
    CHECK(fallback.pixels == project_expected.pixels);

    project.layers.front().render.layer_clock.clock = ready_music_clock(11.0);
    project.layers.front().render.layer_clock.scale =
        pvt::LayerClockScale::PlayOnce;
    CHECK(!pvt::validate(project).ok);
    project.layers.front().render.layer_clock.scale =
        pvt::LayerClockScale::StraightFit;
    CHECK(pvt::validate(project).ok);

    // A music duration need not contain a whole number of output frames.
    // Mapping from frame/fps must agree with preview/movie audio; normalizing
    // by the two independently ceiled frame counts would select local frame 10
    // here instead of the correct frame 9.
    project.canvas.clock = ready_music_clock(1.99);
    project.layers.front().render.layer_clock.clock = ready_music_clock(1.01);
    project.layers.front().render.layer_clock.clock.music.source_sha256 =
        std::string(64U, 'c');
    project.layers.front().render.layer_clock.clock.music.source_basename =
        "fractional-layer.wav";
    project.layers.front().render.layer_clock.scale =
        pvt::LayerClockScale::StraightFit;
    local_render = pvt::apply_global_config(
        project.canvas, project.output, project.layers.front().render);
    local_render.clock = project.layers.front().render.layer_clock.clock;
    pvt::Image fractional_actual;
    pvt::Image fractional_expected;
    pvt::Image incorrectly_count_scaled;
    CHECK(pvt::render_project_frame(project, 19, fractional_actual,
                                    nullptr, &error));
    CHECK(pvt::render_frame(local_render, 9, fractional_expected, &error));
    CHECK(pvt::render_frame(local_render, 10, incorrectly_count_scaled, &error));
    CHECK(fractional_actual.pixels == fractional_expected.pixels);
    CHECK(maximum_difference(fractional_actual, incorrectly_count_scaled)
          > 1.0e-5);
}

} // namespace

int main() {
    test_uuid_factories_and_adapters();
    test_project_audio_response_inheritance();
    test_blend_modes();
    test_straight_alpha_and_transactionality();
    test_project_validation();
    test_project_rendering_order_equivalence_and_seam();
    test_project_cancellation_during_layer_render();
    test_project_sequence();
    test_active_layer_clock_mappings();

    if (failures != 0) {
        std::cerr << failures << " project/composite assertion(s) failed.\n";
        return 1;
    }
    std::cout << "All project and compositing tests passed.\n";
    return 0;
}
