#include "procedural_visualizer_tool.h"
#include "../src/config_codec.h"
#include "../src/displacement_surface.h"
#include "../src/frame_renderer_internal.h"
#include "../src/path_utf8.h"
#include "../src/source_image.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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

bool write_test_png16(const fs::path& path, png_uint_32 width,
                      png_uint_32 height,
                      const std::vector<png_uint_16>& pixels) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_LINEAR_RGB_ALPHA;
    return pixels.size() == PNG_IMAGE_SIZE(image) / sizeof(png_uint_16)
           && png_image_write_to_file(&image, path.string().c_str(), 0,
                                      pixels.data(), 0, nullptr) != 0;
}

bool write_test_png16_data(const fs::path& path, png_uint_32 width,
                           png_uint_32 height,
                           const std::vector<png_uint_16>& pixels,
                           bool interlaced = false) {
    if (pixels.size() != static_cast<std::size_t>(width)
                             * static_cast<std::size_t>(height) * 4U) {
        return false;
    }
    std::vector<unsigned char> raw(pixels.size() * 2U);
    for (std::size_t index = 0U; index < pixels.size(); ++index) {
        raw[index * 2U] = static_cast<unsigned char>(pixels[index] >> 8U);
        raw[index * 2U + 1U] =
            static_cast<unsigned char>(pixels[index] & 0xffU);
    }
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 8U;
    std::vector<png_bytep> rows(static_cast<std::size_t>(height));
    for (png_uint_32 row = 0U; row < height; ++row) {
        rows[static_cast<std::size_t>(row)] =
            raw.data() + static_cast<std::size_t>(row) * row_bytes;
    }
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) return false;
    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png == nullptr) {
        std::fclose(file);
        return false;
    }
    png_infop info = png_create_info_struct(png);
    if (info == nullptr) {
        png_destroy_write_struct(&png, nullptr);
        std::fclose(file);
        return false;
    }
    if (setjmp(png_jmpbuf(png)) != 0) {
        png_destroy_write_struct(&png, &info);
        std::fclose(file);
        return false;
    }
    png_init_io(png, file);
    png_set_IHDR(png, info, width, height, 16, PNG_COLOR_TYPE_RGBA,
                 interlaced ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE,
                 PNG_FILTER_TYPE_BASE);
    png_write_info(png, info);
    png_write_image(png, rows.data());
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return std::fclose(file) == 0;
}

void append_test_exr_u32(std::vector<unsigned char>& bytes,
                         std::uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<unsigned char>((value >> 24U) & 0xffU));
}

void append_test_exr_u64(std::vector<unsigned char>& bytes,
                         std::uint64_t value) {
    for (unsigned int shift = 0U; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void append_test_exr_string(std::vector<unsigned char>& bytes,
                            const char* value) {
    while (*value != '\0') {
        bytes.push_back(static_cast<unsigned char>(*value++));
    }
    bytes.push_back(0U);
}

void append_test_exr_attribute(std::vector<unsigned char>& header,
                               const char* name, const char* type,
                               const std::vector<unsigned char>& value) {
    append_test_exr_string(header, name);
    append_test_exr_string(header, type);
    append_test_exr_u32(header, static_cast<std::uint32_t>(value.size()));
    header.insert(header.end(), value.begin(), value.end());
}

bool write_test_half_y_exr(const fs::path& path, std::uint32_t width,
                           std::uint32_t height,
                           const std::vector<std::uint16_t>& samples) {
    if (width == 0U || height == 0U
        || samples.size()
               != static_cast<std::size_t>(width)
                      * static_cast<std::size_t>(height)) {
        return false;
    }
    std::vector<unsigned char> header;
    append_test_exr_u32(header, 20000630U);
    append_test_exr_u32(header, 2U);
    std::vector<unsigned char> value;
    append_test_exr_string(value, "Y");
    append_test_exr_u32(value, 1U);
    value.insert(value.end(), 4U, 0U);
    append_test_exr_u32(value, 1U);
    append_test_exr_u32(value, 1U);
    value.push_back(0U);
    append_test_exr_attribute(header, "channels", "chlist", value);
    value.assign(1U, 0U);
    append_test_exr_attribute(header, "compression", "compression", value);
    value.clear();
    append_test_exr_u32(value, 0U);
    append_test_exr_u32(value, 0U);
    append_test_exr_u32(value, width - 1U);
    append_test_exr_u32(value, height - 1U);
    append_test_exr_attribute(header, "dataWindow", "box2i", value);
    append_test_exr_attribute(header, "displayWindow", "box2i", value);
    value.assign(1U, 0U);
    append_test_exr_attribute(header, "lineOrder", "lineOrder", value);
    header.push_back(0U);

    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 2U;
    const std::uint64_t first_chunk = static_cast<std::uint64_t>(header.size())
                                      + static_cast<std::uint64_t>(height) * 8U;
    const std::uint64_t chunk_bytes = 8U + row_bytes;
    std::vector<unsigned char> encoded = header;
    for (std::uint32_t y = 0U; y < height; ++y) {
        append_test_exr_u64(
            encoded, first_chunk + static_cast<std::uint64_t>(y) * chunk_bytes);
    }
    for (std::uint32_t y = 0U; y < height; ++y) {
        append_test_exr_u32(encoded, y);
        append_test_exr_u32(encoded, static_cast<std::uint32_t>(row_bytes));
        for (std::uint32_t x = 0U; x < width; ++x) {
            const std::uint16_t half = samples[
                static_cast<std::size_t>(y) * width + x];
            encoded.push_back(static_cast<unsigned char>(half & 0xffU));
            encoded.push_back(static_cast<unsigned char>(half >> 8U));
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    return output.good();
}

void test_plane_displacement_mesh(const fs::path& directory) {
    std::size_t columns = 0U;
    std::size_t rows = 0U;
    std::size_t mesh_bytes = 0U;
    std::string requirements_error;
    CHECK(pvt::detail::displacement_mesh_requirements(
        96, 64, 4, columns, rows, mesh_bytes, &requirements_error));
    CHECK(columns == 25U);
    CHECK(rows == 17U);
    CHECK(mesh_bytes > 0U);
    CHECK(pvt::detail::displacement_mesh_requirements(
        96, 64, 1, columns, rows, mesh_bytes, &requirements_error));
    CHECK(columns == 96U);
    CHECK(rows == 64U);

    const fs::path height_map = directory / "plane-height.png";
    std::vector<unsigned char> pixels(9U * 9U * 4U, 255U);
    for (std::size_t y = 0U; y < 9U; ++y) {
        for (std::size_t x = 0U; x < 9U; ++x) {
            const double dx = static_cast<double>(x) - 4.0;
            const double dy = static_cast<double>(y) - 4.0;
            const double distance = std::sqrt(dx * dx + dy * dy);
            const unsigned char value = static_cast<unsigned char>(
                std::lround(std::clamp(1.0 - distance / 5.7, 0.0, 1.0)
                            * 255.0));
            const std::size_t offset = (y * 9U + x) * 4U;
            pixels[offset] = value;
            pixels[offset + 1U] = value;
            pixels[offset + 2U] = value;
        }
    }
    CHECK(write_test_png(height_map, 9U, 9U, pixels));

    pvt::RenderConfig config = pvt::default_config();
    make_small(config);
    config.width = 96;
    config.height = 64;
    config.block_size = 1;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.output.write_alpha = true;
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::Plane;
    config.surface.curvature = 1.0;
    config.surface.lighting = 0.65;
    config.surface.plane_displacement.enabled = true;
    config.surface.plane_displacement.path = height_map.string();
    config.surface.plane_displacement.minimum = -0.25;
    config.surface.plane_displacement.maximum = 0.45;
    config.surface.plane_displacement.midpoint = 0.5;
    config.surface.plane_displacement.pixels_per_node = 4;

    const auto validation = pvt::validate(config);
    CHECK(validation.ok);
    pvt::Image first;
    std::string error;
    CHECK(pvt::render_frame(config, 0, first, &error));
    CHECK(first.width == config.width);
    CHECK(first.height == config.height);
    bool transparent_exterior = false;
    for (std::size_t offset = 3U; offset < first.pixels.size(); offset += 4U) {
        transparent_exterior = transparent_exterior
                               || first.pixels[offset] < 0.01F;
    }
    CHECK(transparent_exterior);

    pvt::RenderConfig flat = config;
    flat.surface.plane_displacement.enabled = false;
    flat.surface.rotations_per_loop = 0;
    flat.surface.phase_degrees = 0.0;
    pvt::Image flat_image;
    CHECK(pvt::render_frame(flat, 0, flat_image, &error));
    CHECK(mean_absolute_difference(first, flat_image) > 0.01);

    // A larger pixels-per-node ratio must reduce retained mesh memory, while
    // a larger render target builds a correspondingly larger output mesh.
    pvt::RenderConfig coarse = config;
    coarse.surface.plane_displacement.pixels_per_node = 8;
    const auto coarse_validation = pvt::validate(coarse);
    CHECK(coarse_validation.ok);
    CHECK(coarse_validation.estimated_peak_bytes
          < validation.estimated_peak_bytes);
    pvt::RenderConfig full = config;
    full.width = 192;
    full.height = 128;
    const auto full_validation = pvt::validate(full);
    CHECK(full_validation.ok);
    CHECK(full_validation.estimated_peak_bytes
          > validation.estimated_peak_bytes);

    // Replacing the PNG at the same path, with the same dimensions, must
    // invalidate the decoded-image identity and rebuild the generated plane.
    const auto previous_modified = fs::last_write_time(height_map);
    std::vector<unsigned char> replacement(9U * 9U * 4U, 255U);
    for (std::size_t offset = 0U; offset < replacement.size(); offset += 4U) {
        const unsigned char value = static_cast<unsigned char>(
            255U - static_cast<unsigned int>(pixels[offset % pixels.size()]));
        replacement[offset] = value;
        replacement[offset + 1U] = value;
        replacement[offset + 2U] = value;
    }
    CHECK(write_test_png(height_map, 9U, 9U, replacement));
    std::error_code timestamp_error;
    fs::last_write_time(
        height_map, previous_modified + std::chrono::seconds(1),
        timestamp_error);
    CHECK(!timestamp_error);
    pvt::Image changed;
    CHECK(pvt::render_frame(config, 0, changed, &error));
    CHECK(mean_absolute_difference(first, changed) > 0.001);

    pvt::RenderConfig invalid = config;
    invalid.surface.plane_displacement.path.clear();
    CHECK(!pvt::validate(invalid).ok);
    invalid = config;
    invalid.surface.plane_displacement.minimum =
        invalid.surface.plane_displacement.maximum + 0.1;
    CHECK(!pvt::validate(invalid).ok);
}

void test_post_process_effects(const fs::path& directory) {
    const fs::path source = directory / "post-process-edge.png";
    std::vector<unsigned char> source_pixels(16U * 16U * 4U);
    for (std::size_t y = 0U; y < 16U; ++y) {
        for (std::size_t x = 0U; x < 16U; ++x) {
            const std::size_t offset = (y * 16U + x) * 4U;
            const bool opaque = x < 8U;
            source_pixels[offset] = opaque ? 255U : 0U;
            source_pixels[offset + 1U] = 0U;
            source_pixels[offset + 2U] = opaque ? 0U : 255U;
            source_pixels[offset + 3U] = opaque ? 255U : 0U;
        }
    }
    CHECK(write_test_png(source, 16U, 16U, source_pixels));

    pvt::RenderConfig config = pvt::default_config();
    config.width = 16;
    config.height = 16;
    config.block_size = 1;
    config.output.write_alpha = true;
    config.starting_image.enabled = true;
    config.starting_image.path = source.string();
    config.starting_image.fit = pvt::StartingImageFit::Stretch;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.quantization.enabled = false;

    std::string error;
    pvt::Image baseline;
    CHECK(pvt::render_frame_at_phase(config, 0.0, baseline, &error));

    config.post_process.invert_rgb_enabled = true;
    config.post_process.invert_alpha_enabled = true;
    pvt::Image inverted;
    CHECK(pvt::render_frame_at_phase(config, 0.0, inverted, &error));
    CHECK(inverted.pixels.size() == baseline.pixels.size());
    for (std::size_t offset = 0U;
         offset + 3U < baseline.pixels.size(); offset += 4U) {
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
            CHECK(std::fabs(inverted.pixels[offset + channel]
                            - (1.0F - baseline.pixels[offset + channel]))
                  < 1.0e-6F);
        }
    }

    config.post_process = {};
    config.post_process.invert_rgb_enabled = true;
    config.post_process.invert_rgb_mix = 0.25;
    config.post_process.invert_alpha_enabled = true;
    config.post_process.invert_alpha_mix = 0.25;
    pvt::Image partially_inverted;
    CHECK(pvt::render_frame_at_phase(
        config, 0.0, partially_inverted, &error));
    if (const float* pixel = partially_inverted.pixel(0, 0)) {
        CHECK(std::fabs(pixel[0] - 0.75F) < 1.0e-6F);
        CHECK(std::fabs(pixel[1] - 0.25F) < 1.0e-6F);
        CHECK(std::fabs(pixel[2] - 0.25F) < 1.0e-6F);
        CHECK(std::fabs(pixel[3] - 0.75F) < 1.0e-6F);
    }

    config.post_process = {};
    config.post_process.antialias_enabled = true;
    config.post_process.antialias_strength = 1.0;
    config.post_process.antialias_threshold = 0.0;
    config.post_process.antialias_passes = 1;
    pvt::Image antialiased;
    CHECK(pvt::render_frame_at_phase(config, 0.0, antialiased, &error));
    // The transparent blue texel next to opaque red receives red coverage,
    // not a blue fringe: filtering occurs in premultiplied space and the
    // public image remains straight alpha.
    if (const float* edge = antialiased.pixel(8, 8)) {
        CHECK(std::fabs(edge[3] - 0.125F) < 1.0e-6F);
        CHECK(edge[0] > 0.999F);
        CHECK(edge[1] < 1.0e-6F);
        CHECK(edge[2] < 1.0e-6F);
    }
    if (const float* untouched = antialiased.pixel(9, 8)) {
        CHECK(untouched[3] == 0.0F);
        if (const float* original = baseline.pixel(9, 8)) {
            CHECK(std::equal(untouched, untouched + 4, original));
        }
    }

    pvt::RenderConfig invalid = config;
    invalid.post_process.antialias_passes = 5;
    CHECK(pvt::validate(invalid).ok);
    invalid.post_process.antialias_passes = 0;
    CHECK(!pvt::validate(invalid).ok);
    invalid = config;
    invalid.post_process.antialias_threshold =
        std::numeric_limits<double>::quiet_NaN();
    CHECK(!pvt::validate(invalid).ok);
    invalid = config;
    invalid.post_process.invert_rgb_mix = -0.01;
    CHECK(!pvt::validate(invalid).ok);
}

void test_live_control_model_and_setup_codec() {
    const std::string midi_uuid =
        "71111111-1111-4111-8111-111111111111";
    const std::string audio_uuid =
        "72222222-2222-4222-8222-222222222222";
    const std::string osc_uuid =
        "73333333-3333-4333-8333-333333333333";
    const std::string scene_uuid =
        "74444444-4444-4444-8444-444444444444";
    const std::string layer_midi_output_uuid =
        "76666666-6666-4666-8666-666666666666";

    pvt::ProjectConfig project = pvt::default_project();
    CHECK(!project.layers.empty());
    if (project.layers.empty()) return;
    pvt::LiveConfig& live = project.canvas.live;
    live.enabled = true;
    live.endpoints = {
        {midi_uuid, "Keys and clock", pvt::LiveEndpointProtocol::Midi,
         pvt::LiveEndpointDirection::Bidirectional, 2300, -400},
        {audio_uuid, "Front-of-house mix", pvt::LiveEndpointProtocol::Audio,
         pvt::LiveEndpointDirection::Input, 18750, 0},
        {osc_uuid, "Stage OSC", pvt::LiveEndpointProtocol::Osc,
         pvt::LiveEndpointDirection::Input, 0, 0},
        {layer_midi_output_uuid, "Layer MIDI out",
         pvt::LiveEndpointProtocol::Midi,
         pvt::LiveEndpointDirection::Output, 0, 900},
    };

    pvt::LiveSceneConfig scene;
    scene.uuid = scene_uuid;
    scene.name = "Drop";
    scene.transition_milliseconds = 90;
    scene.values = {
        {"layers/lead/opacity", pvt::LiveSceneValueType::Real, "1.25"},
        {"project/live/output/fullscreen",
         pvt::LiveSceneValueType::Boolean, "1"},
        {"layers/lead/blend", pvt::LiveSceneValueType::EnumToken,
         "color_dodge"},
    };
    live.scenes.push_back(scene);
    live.startup_scene_uuid = scene_uuid;

    pvt::LiveControlMapping midi;
    midi.name = "Expression to glow";
    midi.endpoint_uuid = midi_uuid;
    midi.midi_channel = 1;
    midi.control_number = 11;
    midi.target_path = "layers/lead/effects/glow/intensity";
    midi.output_minimum = -4.0;
    midi.output_maximum = 12.0;
    midi.curve = 1.75;
    midi.smoothing_milliseconds = 24;
    pvt::LiveControlMapping osc;
    osc.name = "Freeze button";
    osc.endpoint_uuid = osc_uuid;
    osc.input = pvt::LiveControlInput::OscValue;
    osc.osc_address = "/visuals/freeze";
    osc.target = pvt::LiveMappingTarget::Action;
    osc.target_path.clear();
    osc.action = pvt::LiveAction::Freeze;
    osc.mode = pvt::LiveMappingMode::Toggle;
    live.mappings = {midi, osc};

    live.clock_inputs = {
        {true, pvt::LiveClockTarget::Project, {},
         pvt::LiveClockInputSource::MidiClock, midi_uuid, 0, true, 600, {}},
        {true, pvt::LiveClockTarget::Layer, project.layers.front().uuid,
         pvt::LiveClockInputSource::AudioStream, audio_uuid, 1, false, 250, {}},
    };
    live.midi_clock_outputs = {
        {true, pvt::LiveClockTarget::Project, {}, midi_uuid, true, true},
        {true, pvt::LiveClockTarget::Layer, project.layers.front().uuid,
         layer_midi_output_uuid, false, false},
    };
    live.safety.watchdog_timeout_milliseconds = 75;
    live.safety.audio_dropout_grace_milliseconds = 325;
    live.safety.last_good_frame_timeout_milliseconds = 3000;
    live.safety.prevent_device_sleep = true;
    live.audio_processing.high_pass_enabled = true;
    live.audio_processing.high_pass_hz = 45.0;
    live.audio_processing.equalizer_enabled = true;
    live.audio_processing.equalizer_bands[5U].gain_db = -2.5;
    live.audio_processing.frequency_streams = {
        {"live-bass", "Live bass", 45.0, 240.0}};
    live.clock_inputs[1U].frequency_stream_uuid = "live-bass";

    CHECK(pvt::validate(live).ok);
    CHECK(pvt::validate(project).ok);

    pvt::LiveConfig expanded_live = live;
    expanded_live.endpoints.front().input_latency_microseconds = 20000000;
    expanded_live.mappings.front().output_minimum = -2.0e12;
    expanded_live.mappings.front().output_maximum = 2.0e12;
    expanded_live.mappings.front().curve = 1000.0;
    expanded_live.mappings.front().smoothing_milliseconds = 60001;
    expanded_live.scenes.front().transition_milliseconds = 60001;
    expanded_live.safety.watchdog_timeout_milliseconds = 60001;
    expanded_live.safety.audio_dropout_grace_milliseconds = 60001;
    expanded_live.safety.last_good_frame_timeout_milliseconds = 60001;
    CHECK(pvt::validate(expanded_live).ok);
    CHECK(std::string(pvt::live_endpoint_protocol_name(
                          pvt::LiveEndpointProtocol::Midi)) == "MIDI");
    CHECK(std::string(pvt::live_action_name(pvt::LiveAction::Blackout))
          == "Blackout");
    CHECK(std::string(pvt::live_clock_input_source_name(
                          pvt::LiveClockInputSource::AudioStream))
          == "Audio stream");

    pvt::RenderConfig setup = pvt::default_config();
    setup.live = live;
    std::string serialized;
    std::string error;
    CHECK(pvt::detail::serialize_setup_config(setup, serialized, &error));
    CHECK(serialized.rfind("PVT_SETUP\t15\n", 0U) == 0U);
    CHECK(serialized.find("live.endpoints.0.name\tKeys%20and%20clock\n")
          != std::string::npos);
    CHECK(serialized.find("live.clock_inputs.1.source\taudio_stream\n")
          != std::string::npos);
    CHECK(serialized.find("device_id") == std::string::npos);
    CHECK(serialized.find("device_uid") == std::string::npos);
    CHECK(serialized.find("stream_data") == std::string::npos);

    pvt::RenderConfig loaded;
    CHECK(pvt::detail::deserialize_setup_config(serialized, loaded, &error));
    CHECK(loaded.live.enabled);
    CHECK(loaded.live.endpoints.size() == 4U);
    CHECK(loaded.live.endpoints[1].input_latency_microseconds == 18750);
    CHECK(loaded.live.mappings.size() == 2U);
    CHECK(loaded.live.safety.prevent_device_sleep);
    CHECK(loaded.live.audio_processing.high_pass_enabled);
    CHECK(loaded.live.audio_processing.high_pass_hz == 45.0);
    CHECK(loaded.live.audio_processing.equalizer_bands[5U].gain_db == -2.5);
    CHECK(loaded.live.audio_processing.frequency_streams.size() == 1U);
    CHECK(loaded.live.clock_inputs[1U].frequency_stream_uuid == "live-bass");
    CHECK(loaded.live.mappings[0].target_path
          == "layers/lead/effects/glow/intensity");
    CHECK(loaded.live.clock_inputs.size() == 2U);
    CHECK(loaded.live.midi_clock_outputs.size() == 2U);
    CHECK(loaded.live.scenes.front().values.size() == 3U);
    CHECK(loaded.live.safety.dropout_behavior
          == pvt::LiveDropoutBehavior::LastGoodFrame);

    pvt::LiveConfig invalid = live;
    invalid.clock_inputs[0].endpoint_uuid = audio_uuid;
    CHECK(!pvt::validate(invalid).ok);
    invalid = live;
    invalid.clock_inputs.push_back(invalid.clock_inputs.front());
    CHECK(!pvt::validate(invalid).ok);
    invalid = live;
    const std::string second_audio_uuid =
        "78888888-8888-4888-8888-888888888888";
    invalid.endpoints.push_back(
        {second_audio_uuid, "Second audio input",
         pvt::LiveEndpointProtocol::Audio,
         pvt::LiveEndpointDirection::Input, 0, 0});
    invalid.clock_inputs[0].source =
        pvt::LiveClockInputSource::AudioStream;
    invalid.clock_inputs[0].endpoint_uuid = second_audio_uuid;
    invalid.clock_inputs[0].follow_midi_transport = false;
    CHECK(!pvt::validate(invalid).ok);
    invalid.clock_inputs[0].endpoint_uuid = audio_uuid;
    CHECK(pvt::validate(invalid).ok);
    invalid = live;
    invalid.midi_clock_outputs[1].endpoint_uuid = midi_uuid;
    CHECK(!pvt::validate(invalid).ok);
    invalid = live;
    invalid.scenes.front().values.front().value = "nan";
    CHECK(!pvt::validate(invalid).ok);
    invalid = live;
    invalid.mappings.front().osc_address = "/wrong/source";
    CHECK(!pvt::validate(invalid).ok);

    pvt::ProjectConfig missing_layer = project;
    missing_layer.canvas.live.clock_inputs[1].layer_uuid =
        "75555555-5555-4555-8555-555555555555";
    CHECK(pvt::validate(missing_layer.canvas.live).ok);
    CHECK(!pvt::validate(missing_layer).ok);
    missing_layer.canvas.live.clock_inputs[1].enabled = false;
    CHECK(pvt::validate(missing_layer).ok);
}

void test_starting_images_and_reusable_paths(const fs::path& directory) {
    const fs::path source = directory / "starting-image.png";
    CHECK(write_test_png(
        source, 2U, 2U,
        {255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U,
         0U, 0U, 255U, 255U, 255U, 255U, 255U, 0U}));

    // Loading is never routed through an 8-bit intermediate. Values that
    // cannot be represented by 8-bit expansion must survive a 16-bit PNG
    // decode into the float32 working image.
    const fs::path source_16 = directory / "starting-image-16.png";
    const std::vector<png_uint_16> source_16_values = {
        258U, 1025U, 32769U, 65534U,
        511U, 4097U, 49153U, 60001U};
    CHECK(write_test_png16(source_16, 2U, 1U, source_16_values));
    std::shared_ptr<const pvt::Image> decoded_16;
    std::string precision_error;
    CHECK(pvt::detail::load_starting_image_source(
        source_16.string(), decoded_16, nullptr, &precision_error));
    CHECK(decoded_16 != nullptr && decoded_16->pixels.size() == 8U);
    if (decoded_16 != nullptr && decoded_16->pixels.size() == 8U) {
        for (std::size_t index = 0U; index < source_16_values.size(); ++index) {
            CHECK(std::llround(decoded_16->pixels[index] * 65535.0F)
                  == source_16_values[index]);
        }
    }
    const fs::path data_16 = directory / "height-data-16.png";
    CHECK(write_test_png16_data(data_16, 2U, 1U, source_16_values));
    std::shared_ptr<const pvt::Image> decoded_data_16;
    CHECK(pvt::detail::load_data_image_source(
        data_16.string(), decoded_data_16, nullptr, &precision_error));
    CHECK(decoded_data_16 != nullptr && decoded_data_16->pixels.size() == 8U);
    if (decoded_data_16 != nullptr && decoded_data_16->pixels.size() == 8U) {
        for (std::size_t index = 0U; index < source_16_values.size(); ++index) {
            CHECK(std::llround(decoded_data_16->pixels[index] * 65535.0F)
                  == source_16_values[index]);
        }
    }
    const fs::path data_16_interlaced =
        directory / "height-data-16-interlaced.png";
    CHECK(write_test_png16_data(data_16_interlaced, 2U, 1U,
                                source_16_values, true));
    std::shared_ptr<const pvt::Image> decoded_data_16_interlaced;
    CHECK(pvt::detail::load_data_image_source(
        data_16_interlaced.string(), decoded_data_16_interlaced, nullptr,
        &precision_error));
    CHECK(decoded_data_16_interlaced != nullptr
          && decoded_data_16_interlaced->pixels.size() == 8U);
    if (decoded_data_16_interlaced != nullptr
        && decoded_data_16_interlaced->pixels.size() == 8U) {
        for (std::size_t index = 0U; index < source_16_values.size(); ++index) {
            CHECK(std::llround(
                      decoded_data_16_interlaced->pixels[index] * 65535.0F)
                  == source_16_values[index]);
        }
    }

    // Common grayscale height maps use one HALF channel rather than RGB.
    // Import must preserve all representable half values and replicate Y to
    // RGB without requiring an 8-bit or display-color intermediate.
    const fs::path half_exr = directory / "single-channel-half-height.exr";
    const std::vector<std::uint16_t> half_values = {
        0x0000U, 0x3400U, 0x3800U, 0x3c00U}; // 0, .25, .5, 1
    CHECK(write_test_half_y_exr(half_exr, 2U, 2U, half_values));
    std::shared_ptr<const pvt::Image> decoded_half;
    CHECK(pvt::detail::load_data_image_source(
        half_exr.string(), decoded_half, nullptr, &precision_error));
    CHECK(decoded_half != nullptr && decoded_half->width == 2
          && decoded_half->height == 2 && decoded_half->pixels.size() == 16U);
    if (decoded_half != nullptr && decoded_half->pixels.size() == 16U) {
        const std::array<float, 4U> expected{{0.0F, 0.25F, 0.5F, 1.0F}};
        for (std::size_t pixel = 0U; pixel < expected.size(); ++pixel) {
            CHECK(decoded_half->pixels[pixel * 4U] == expected[pixel]);
            CHECK(decoded_half->pixels[pixel * 4U + 1U] == expected[pixel]);
            CHECK(decoded_half->pixels[pixel * 4U + 2U] == expected[pixel]);
            CHECK(decoded_half->pixels[pixel * 4U + 3U] == 1.0F);
        }
    }

    // The application's own full-float EXR output must round-trip as an input
    // with no additional quantization, including HDR and negative RGB data.
    const fs::path float_exr = directory / "starting-image-float.exr";
    pvt::Image float_source;
    float_source.width = 16;
    float_source.height = 16;
    float_source.pixels.assign(16U * 16U * 4U, 0.0F);
    for (std::size_t pixel = 0U; pixel < 16U * 16U; ++pixel) {
        float_source.pixels[pixel * 4U + 3U] = 1.0F;
    }
    const std::array<float, 16U> precision_samples{{
        -0.125F, 0.1234567F, 2.25F, 1.0F,
        0.33333334F, 0.75F, 1.5F, 0.5F,
        4.0F, -1.25F, 0.0625F, 0.25F,
        0.875F, 1.125F, -0.5F, 0.75F}};
    std::copy(precision_samples.begin(), precision_samples.end(),
              float_source.pixels.begin());
    pvt::RenderConfig float_output = pvt::default_config();
    float_output.width = float_source.width;
    float_output.height = float_source.height;
    float_output.output.bit_depth = 32;
    float_output.output.write_alpha = true;
    float_output.output.overwrite_existing = true;
    const bool wrote_float_source = pvt::write_image(
        float_exr.string(), float_source, float_output, 0U, &precision_error);
    if (!wrote_float_source) {
        std::cerr << "high-precision EXR fixture: " << precision_error << '\n';
    }
    CHECK(wrote_float_source);
    std::shared_ptr<const pvt::Image> decoded_float;
    const bool loaded_float_source = pvt::detail::load_starting_image_source(
        float_exr.string(), decoded_float, nullptr, &precision_error);
    if (!loaded_float_source) {
        std::cerr << "high-precision EXR import: " << precision_error << '\n';
    }
    CHECK(loaded_float_source);
    CHECK(decoded_float != nullptr
          && decoded_float->pixels == float_source.pixels);

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
    pvt::RenderConfig image_with_generated_limits = image_config;
    image_with_generated_limits.starting_colors.red_minimum = 0.35;
    image_with_generated_limits.starting_colors.red_maximum = 0.35;
    image_with_generated_limits.starting_colors.green_minimum = 0.45;
    image_with_generated_limits.starting_colors.green_maximum = 0.45;
    image_with_generated_limits.starting_colors.blue_minimum = 0.55;
    image_with_generated_limits.starting_colors.blue_maximum = 0.55;
    pvt::Image image_not_remapped;
    CHECK(pvt::render_frame_at_phase(
        image_with_generated_limits, 0.0, image_not_remapped, &error));
    CHECK(image_not_remapped.pixels == stretched.pixels);
    image_with_generated_limits.starting_colors.kaleidoscope.enabled = true;
    image_with_generated_limits.starting_colors.kaleidoscope.mirrored_segments = 9;
    image_with_generated_limits.starting_colors.domain_warp.enabled = true;
    image_with_generated_limits.starting_colors.domain_warp.strength = 0.3;
    image_with_generated_limits.starting_colors.domain_warp.seed = 778899U;
    CHECK(pvt::render_frame_at_phase(
        image_with_generated_limits, 0.37, image_not_remapped, &error));
    CHECK(image_not_remapped.pixels == stretched.pixels);
    image_config.alpha.use_source_alpha = false;
    pvt::Image ignored_source_alpha;
    CHECK(pvt::render_frame_at_phase(
        image_config, 0.0, ignored_source_alpha, &error));
    if (const float* pixel = ignored_source_alpha.pixel(15, 15)) {
        CHECK(pixel[3] == 1.0F);
    }
    image_config.alpha.use_source_alpha = true;

    // A starting image and a starting palette are composable: image pixels
    // choose spatial placement, then source quantization supplies authored
    // RGBA values before effects. Source alpha can be ignored without erasing
    // the palette values.
    image_config.palette.enabled = true;
    image_config.palette.name = "RGBA source palette";
    image_config.palette.colors = {
        {1.0, 0.0, 0.0, 0.25, {}, pvt::PaletteColorEncoding::Srgb},
        {0.0, 0.0, 1.0, 0.75, {}, pvt::PaletteColorEncoding::Srgb}};
    image_config.starting_image.palette_dither_enabled = false;
    pvt::Image image_paletted;
    CHECK(pvt::render_frame_at_phase(
        image_config, 0.0, image_paletted, &error));
    if (const float* pixel = image_paletted.pixel(0, 0)) {
        const bool red = pixel[0] > 0.99F && pixel[1] < 0.01F
                         && pixel[2] < 0.01F
                         && std::fabs(pixel[3] - 0.25F) < 0.0001F;
        const bool blue = pixel[0] < 0.01F && pixel[1] < 0.01F
                          && pixel[2] > 0.99F
                          && std::fabs(pixel[3] - 0.75F) < 0.0001F;
        CHECK(red || blue);
    }
    image_config.alpha.use_source_alpha = false;
    CHECK(pvt::render_frame_at_phase(
        image_config, 0.0, ignored_source_alpha, &error));
    if (const float* pixel = ignored_source_alpha.pixel(0, 0)) {
        CHECK(pixel[3] == 1.0F);
    }
    image_config.alpha.use_source_alpha = true;
    image_config.starting_image.palette_dither_enabled = true;
    for (const auto method : {pvt::DitherMethod::BlueNoise,
                              pvt::DitherMethod::OrderedBayer,
                              pvt::DitherMethod::FloydSteinberg}) {
        image_config.starting_image.palette_dither_method = method;
        CHECK(pvt::render_frame_at_phase(
            image_config, 0.0, image_paletted, &error));
    }
    image_config.palette = {};
    image_config.starting_image.palette_dither_enabled = false;
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

    pvt::RenderConfig generated = image_config;
    generated.starting_image = {};
    generated.starting_colors.mode =
        pvt::StartingColorMode::HorizontalRainbow;
    generated.starting_colors.red_steps = 2;
    generated.starting_colors.green_steps = 3;
    generated.starting_colors.blue_steps = 4;
    generated.starting_colors.alpha_steps = 2;
    generated.starting_colors.alpha_minimum = 0.25;
    generated.starting_colors.alpha_maximum = 0.25;
    generated.starting_colors.include_alpha = false;
    pvt::Image generated_rgb;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, generated_rgb, &error));
    CHECK(generated_rgb.pixel(0, 0) != nullptr);
    if (const float* pixel = generated_rgb.pixel(0, 0)) CHECK(pixel[3] == 1.0F);
    generated.starting_colors.include_alpha = true;
    pvt::Image generated_rgba;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, generated_rgba, &error));
    if (const float* pixel = generated_rgba.pixel(0, 0)) {
        CHECK(std::fabs(pixel[3] - 0.25F) < 0.0001F);
    }
    generated.alpha.use_source_alpha = false;
    CHECK(pvt::render_frame_at_phase(
        generated, 0.0, ignored_source_alpha, &error));
    if (const float* pixel = ignored_source_alpha.pixel(0, 0)) {
        // Generated alpha is controlled by its adjacent include-alpha setting;
        // the palette/PNG source-alpha switch must not silently defeat it.
        CHECK(std::fabs(pixel[3] - 0.25F) < 0.0001F);
    }
    generated.alpha.use_source_alpha = true;
    generated.starting_colors.include_alpha = false;
    generated.starting_colors.mode = pvt::StartingColorMode::VerticalRainbow;
    pvt::Image vertical;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, vertical, &error));
    CHECK(mean_absolute_difference(generated_rgb, vertical) > 0.0001);
    generated.starting_colors.mode = pvt::StartingColorMode::DiagonalRainbow;
    pvt::Image diagonal;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, diagonal, &error));
    CHECK(mean_absolute_difference(vertical, diagonal) > 0.0001);
    generated.starting_colors.mode = pvt::StartingColorMode::SpiralRainbow;
    pvt::Image spiral;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, spiral, &error));
    CHECK(mean_absolute_difference(diagonal, spiral) > 0.0001);
    generated.starting_colors.mode =
        pvt::StartingColorMode::SquareSpiralRainbow;
    pvt::Image square_spiral;
    CHECK(pvt::render_frame_at_phase(
        generated, 0.0, square_spiral, &error));
    CHECK(mean_absolute_difference(spiral, square_spiral) > 0.0001);
    generated.starting_colors.mode = pvt::StartingColorMode::Random;
    pvt::Image random;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, random, &error));
    CHECK(mean_absolute_difference(square_spiral, random) > 0.0001);

    CHECK(std::string(pvt::starting_color_mode_name(
              pvt::StartingColorMode::ContinuousHue)) == "Continuous hue");
    CHECK(std::string(pvt::starting_color_mode_name(
              pvt::StartingColorMode::Random)) == "Random");
    CHECK(std::string(pvt::starting_color_mode_name(
              pvt::StartingColorMode::SpiralRainbow)) == "Spiral rainbow");
    CHECK(std::string(pvt::starting_color_mode_name(
              pvt::StartingColorMode::SquareSpiralRainbow))
          == "Square spiral rainbow");

    // Every traversal assigns a distinct position in one complete rainbow to
    // each full-resolution block. The 4x4 fixture must therefore contain all
    // sixteen unique generated RGBA colors in every ordered/Random mode.
    pvt::RenderConfig exhaustive = generated;
    exhaustive.width = 16;
    exhaustive.height = 16;
    exhaustive.block_size = 4;
    exhaustive.starting_colors.include_alpha = true;
    exhaustive.starting_colors.red_steps = 2;
    exhaustive.starting_colors.green_steps = 2;
    exhaustive.starting_colors.blue_steps = 2;
    exhaustive.starting_colors.alpha_steps = 2;
    exhaustive.starting_colors.red_minimum = 0.0;
    exhaustive.starting_colors.red_maximum = 1.0;
    exhaustive.starting_colors.green_minimum = 0.0;
    exhaustive.starting_colors.green_maximum = 1.0;
    exhaustive.starting_colors.blue_minimum = 0.0;
    exhaustive.starting_colors.blue_maximum = 1.0;
    exhaustive.starting_colors.alpha_minimum = 0.25;
    exhaustive.starting_colors.alpha_maximum = 0.75;
    exhaustive.hue_cycles = 0;
    for (const auto mode : {pvt::StartingColorMode::HorizontalRainbow,
                            pvt::StartingColorMode::VerticalRainbow,
                            pvt::StartingColorMode::DiagonalRainbow,
                            pvt::StartingColorMode::SpiralRainbow,
                            pvt::StartingColorMode::SquareSpiralRainbow,
                            pvt::StartingColorMode::Random}) {
        exhaustive.starting_colors.mode = mode;
        pvt::Image combinations;
        CHECK(pvt::render_frame_at_phase(
            exhaustive, 0.0, combinations, &error));
        std::set<std::array<float, 4U>> observed;
        std::array<bool, 3U> dominant_channels{};
        for (int y = 0; y < exhaustive.height; y += exhaustive.block_size) {
            for (int x = 0; x < exhaustive.width; x += exhaustive.block_size) {
                if (const float* pixel = combinations.pixel(x, y)) {
                    observed.insert({pixel[0], pixel[1], pixel[2], pixel[3]});
                    const std::size_t dominant = static_cast<std::size_t>(
                        std::max_element(pixel, pixel + 3) - pixel);
                    dominant_channels[dominant] = true;
                }
            }
        }
        CHECK(observed.size() == 16U);
        CHECK(dominant_channels[0] && dominant_channels[1]
              && dominant_channels[2]);
    }

    // Non-square grids exercise the diagonal and square-spiral rank formulas.
    // Every spatial pattern must remain one-to-one there too.
    pvt::RenderConfig rectangular = exhaustive;
    rectangular.width = 20;
    rectangular.height = 16;
    rectangular.block_size = 4;
    rectangular.starting_colors.include_alpha = false;
    for (const auto mode : {pvt::StartingColorMode::HorizontalRainbow,
                            pvt::StartingColorMode::VerticalRainbow,
                            pvt::StartingColorMode::DiagonalRainbow,
                            pvt::StartingColorMode::SpiralRainbow,
                            pvt::StartingColorMode::SquareSpiralRainbow,
                            pvt::StartingColorMode::Random}) {
        rectangular.starting_colors.mode = mode;
        pvt::Image pattern;
        CHECK(pvt::render_frame_at_phase(
            rectangular, 0.0, pattern, &error));
        std::set<std::array<float, 3U>> observed;
        for (int y = 0; y < rectangular.height;
             y += rectangular.block_size) {
            for (int x = 0; x < rectangular.width;
                 x += rectangular.block_size) {
                if (const float* pixel = pattern.pixel(x, y)) {
                    observed.insert({pixel[0], pixel[1], pixel[2]});
                }
            }
        }
        CHECK(observed.size() == 20U);
    }

    // Resolution, not an authored 8-bit step count, chooses the radix. Every
    // full-resolution block receives a distinct float32 RGB tuple, and the
    // generated source remains fixed across time when procedural controls are
    // disabled.
    pvt::RenderConfig scaled = exhaustive;
    scaled.width = 192;
    scaled.height = 108;
    scaled.block_size = 1;
    scaled.starting_colors.mode = pvt::StartingColorMode::VerticalRainbow;
    scaled.starting_colors.include_alpha = false;
    scaled.hue_cycles = 73;
    scaled.spiral_enabled = false;
    scaled.wall_reflection_enabled = false;
    scaled.lighting_enabled = false;
    scaled.effects.clear();
    scaled.waves.clear();
    scaled.swings.clear();
    pvt::Image scaled_start;
    pvt::Image scaled_later;
    CHECK(pvt::render_frame_at_phase(scaled, 0.0, scaled_start, &error));
    CHECK(pvt::render_frame_at_phase(scaled, 0.713, scaled_later, &error));
    CHECK(scaled_start.pixels == scaled_later.pixels);
    pvt::RenderConfig legacy_values_changed = scaled;
    legacy_values_changed.starting_colors.red_steps = 1;
    legacy_values_changed.starting_colors.green_steps = 7;
    legacy_values_changed.starting_colors.blue_steps = 31;
    legacy_values_changed.starting_colors.alpha_steps = 65536;
    pvt::Image ignored_legacy_values;
    CHECK(pvt::render_frame_at_phase(
        legacy_values_changed, 0.0, ignored_legacy_values, &error));
    CHECK(scaled_start.pixels == ignored_legacy_values.pixels);
    std::set<std::array<float, 3U>> scaled_colors;
    for (std::size_t offset = 0U; offset < scaled_start.pixels.size();
         offset += 4U) {
        scaled_colors.insert({scaled_start.pixels[offset],
                              scaled_start.pixels[offset + 1U],
                              scaled_start.pixels[offset + 2U]});
    }
    CHECK(scaled_colors.size()
          == static_cast<std::size_t>(scaled.width * scaled.height));

    pvt::RenderConfig radial_spiral = scaled;
    radial_spiral.starting_colors.mode =
        pvt::StartingColorMode::SpiralRainbow;
    pvt::Image radial_spiral_start;
    pvt::Image radial_spiral_later;
    CHECK(pvt::render_frame_at_phase(
        radial_spiral, 0.0, radial_spiral_start, &error));
    CHECK(pvt::render_frame_at_phase(
        radial_spiral, 0.713, radial_spiral_later, &error));
    CHECK(radial_spiral_start.pixels == radial_spiral_later.pixels);
    std::set<std::array<float, 3U>> radial_spiral_colors;
    for (std::size_t offset = 0U;
         offset < radial_spiral_start.pixels.size(); offset += 4U) {
        radial_spiral_colors.insert({radial_spiral_start.pixels[offset],
                                     radial_spiral_start.pixels[offset + 1U],
                                     radial_spiral_start.pixels[offset + 2U]});
    }
    CHECK(radial_spiral_colors.size()
          == static_cast<std::size_t>(radial_spiral.width
                                      * radial_spiral.height));

    // The ordered traversal is hue-major rather than a blue-fast RGB scan.
    // Samples from its six equally sized non-gray sectors must progress around
    // red, yellow, green, cyan, blue, and magenta while preserving every tuple.
    std::uint64_t generated_levels = 1U;
    const std::uint64_t scaled_count = static_cast<std::uint64_t>(
        scaled.width) * static_cast<std::uint64_t>(scaled.height);
    while (generated_levels * generated_levels * generated_levels
           < scaled_count) {
        ++generated_levels;
    }
    const std::uint64_t hue_sector_size =
        (generated_levels * generated_levels * generated_levels
         - generated_levels) / 6U;
    const std::array<std::array<int, 3U>, 6U> channel_order{{
        {{0, 1, 2}}, {{1, 0, 2}}, {{1, 2, 0}},
        {{2, 1, 0}}, {{2, 0, 1}}, {{0, 2, 1}},
    }};
    for (std::size_t sector = 0U; sector < channel_order.size(); ++sector) {
        const std::uint64_t sample_index =
            static_cast<std::uint64_t>(sector) * hue_sector_size
            + hue_sector_size / 2U;
        CHECK(sample_index < scaled_count);
        if (sample_index >= scaled_count) continue;
        const std::uint64_t scaled_width =
            static_cast<std::uint64_t>(scaled.width);
        const float* sample = scaled_start.pixel(
            static_cast<int>(sample_index % scaled_width),
            static_cast<int>(sample_index / scaled_width));
        CHECK(sample != nullptr);
        if (sample == nullptr) continue;
        const auto& order = channel_order[sector];
        CHECK(sample[order[0]] >= sample[order[1]]);
        CHECK(sample[order[1]] >= sample[order[2]]);
        CHECK(sample[order[0]] > sample[order[2]]);
    }
    std::array<float, 3U> ordered_minimum{1.0F, 1.0F, 1.0F};
    std::array<float, 3U> ordered_maximum{};
    for (std::size_t offset = 0U; offset < scaled_start.pixels.size();
         offset += 4U) {
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            ordered_minimum[channel] = std::min(
                ordered_minimum[channel],
                scaled_start.pixels[offset + channel]);
            ordered_maximum[channel] = std::max(
                ordered_maximum[channel],
                scaled_start.pixels[offset + channel]);
        }
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        CHECK(ordered_minimum[channel] < 0.01F);
        CHECK(ordered_maximum[channel] > 0.8F);
    }
    std::array<std::size_t, 3U> dominant_counts{};
    for (std::size_t offset = 0U; offset < scaled_start.pixels.size();
         offset += 4U) {
        const auto begin = scaled_start.pixels.begin()
                           + static_cast<std::ptrdiff_t>(offset);
        const std::size_t dominant = static_cast<std::size_t>(
            std::max_element(begin, begin + 3) - begin);
        ++dominant_counts[dominant];
    }
    for (const std::size_t count : dominant_counts) {
        CHECK(count > scaled_colors.size() / 5U);
    }

    // The former unconditional dispersion is now the explicit Random mode.
    // It remains a one-to-one traversal and keeps its useful static-like spread
    // across both axes without changing the ordered modes.
    pvt::RenderConfig distributed = scaled;
    distributed.starting_colors.mode = pvt::StartingColorMode::Random;
    pvt::Image distributed_image;
    CHECK(pvt::render_frame_at_phase(
        distributed, 0.0, distributed_image, &error));
    std::array<std::set<float>, 3U> row_channel_values;
    std::array<std::set<float>, 3U> column_channel_values;
    std::set<std::array<float, 3U>> random_colors;
    for (std::size_t offset = 0U; offset < distributed_image.pixels.size();
         offset += 4U) {
        random_colors.insert({distributed_image.pixels[offset],
                              distributed_image.pixels[offset + 1U],
                              distributed_image.pixels[offset + 2U]});
    }
    CHECK(random_colors.size()
          == static_cast<std::size_t>(distributed.width * distributed.height));
    for (int x = 0; x < distributed.width; ++x) {
        if (const float* pixel = distributed_image.pixel(x, 0)) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                row_channel_values[channel].insert(pixel[channel]);
            }
        }
    }
    for (int y = 0; y < distributed.height; ++y) {
        if (const float* pixel = distributed_image.pixel(0, y)) {
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                column_channel_values[channel].insert(pixel[channel]);
            }
        }
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        CHECK(row_channel_values[channel].size() >= 24U);
        CHECK(column_channel_values[channel].size() >= 24U);
    }

    pvt::RenderConfig blocked = scaled;
    blocked.width = 18;
    blocked.height = 18;
    blocked.block_size = 3;
    pvt::Image blocked_image;
    const bool blocked_ok = pvt::render_frame_at_phase(
        blocked, 0.0, blocked_image, &error);
    if (!blocked_ok) std::cerr << "blocked generated render: " << error << '\n';
    CHECK(blocked_ok);
    std::set<std::array<float, 3U>> block_colors;
    for (int block_y = 0; block_y < blocked.height;
         block_y += blocked.block_size) {
        for (int block_x = 0; block_x < blocked.width;
             block_x += blocked.block_size) {
            const float* first = blocked_image.pixel(block_x, block_y);
            CHECK(first != nullptr);
            if (first == nullptr) continue;
            block_colors.insert({first[0], first[1], first[2]});
            for (int y = block_y;
                 y < std::min(block_y + blocked.block_size, blocked.height);
                 ++y) {
                for (int x = block_x;
                     x < std::min(block_x + blocked.block_size, blocked.width);
                     ++x) {
                    const float* pixel = blocked_image.pixel(x, y);
                    CHECK(pixel != nullptr
                          && std::equal(first, first + 4, pixel));
                }
            }
        }
    }
    CHECK(block_colors.size()
          == static_cast<std::size_t>(
              (blocked.width / blocked.block_size)
              * (blocked.height / blocked.block_size)));

    pvt::RenderConfig ranged = scaled;
    ranged.width = 16;
    ranged.height = 16;
    ranged.starting_colors.mode = pvt::StartingColorMode::HorizontalRainbow;
    ranged.starting_colors.red_minimum = 0.25;
    ranged.starting_colors.red_maximum = 0.25;
    ranged.starting_colors.green_minimum = 0.2;
    ranged.starting_colors.green_maximum = 0.4;
    ranged.starting_colors.blue_minimum = 0.6;
    ranged.starting_colors.blue_maximum = 0.8;
    pvt::Image ranged_image;
    const bool ranged_ok = pvt::render_frame_at_phase(
        ranged, 0.0, ranged_image, &error);
    if (!ranged_ok) std::cerr << "ranged generated render: " << error << '\n';
    CHECK(ranged_ok);
    const auto linear_test = [](double value) {
        return value <= 0.04045 ? value / 12.92
                                : std::pow((value + 0.055) / 1.055, 2.4);
    };
    float minimum_green = 1.0F;
    float maximum_green = 0.0F;
    float minimum_blue = 1.0F;
    float maximum_blue = 0.0F;
    const float expected_red = static_cast<float>(linear_test(0.25));
    for (std::size_t offset = 0U; offset < ranged_image.pixels.size();
         offset += 4U) {
        CHECK(std::fabs(ranged_image.pixels[offset] - expected_red) < 1.0e-6F);
        minimum_green = std::min(minimum_green,
                                 ranged_image.pixels[offset + 1U]);
        maximum_green = std::max(maximum_green,
                                 ranged_image.pixels[offset + 1U]);
        minimum_blue = std::min(minimum_blue,
                                ranged_image.pixels[offset + 2U]);
        maximum_blue = std::max(maximum_blue,
                                ranged_image.pixels[offset + 2U]);
    }
    CHECK(minimum_green >= linear_test(0.2) - 1.0e-6);
    CHECK(maximum_green <= linear_test(0.4) + 1.0e-6);
    CHECK(maximum_green > linear_test(0.395));
    CHECK(minimum_blue >= linear_test(0.6) - 1.0e-6);
    CHECK(maximum_blue <= linear_test(0.8) + 1.0e-6);
    CHECK(maximum_blue > linear_test(0.795));

    ranged.starting_colors.mode = pvt::StartingColorMode::SpiralRainbow;
    pvt::Image ranged_spiral;
    CHECK(pvt::render_frame_at_phase(
        ranged, 0.0, ranged_spiral, &error));
    for (std::size_t offset = 0U; offset < ranged_spiral.pixels.size();
         offset += 4U) {
        CHECK(std::fabs(ranged_spiral.pixels[offset] - expected_red) < 1.0e-6F);
        CHECK(ranged_spiral.pixels[offset + 1U]
              >= linear_test(0.2) - 1.0e-6);
        CHECK(ranged_spiral.pixels[offset + 1U]
              <= linear_test(0.4) + 1.0e-6);
        CHECK(ranged_spiral.pixels[offset + 2U]
              >= linear_test(0.6) - 1.0e-6);
        CHECK(ranged_spiral.pixels[offset + 2U]
              <= linear_test(0.8) + 1.0e-6);
    }

    // Min/Max owns every choice in the Generated starting colors box,
    // including Continuous hue. Collapsed RGB ranges make the expected source
    // unambiguous while authored image/palette sources remain separate.
    pvt::RenderConfig continuous_ranged = ranged;
    continuous_ranged.starting_colors.mode =
        pvt::StartingColorMode::ContinuousHue;
    continuous_ranged.starting_colors.red_minimum = 0.15;
    continuous_ranged.starting_colors.red_maximum = 0.15;
    continuous_ranged.starting_colors.green_minimum = 0.35;
    continuous_ranged.starting_colors.green_maximum = 0.35;
    continuous_ranged.starting_colors.blue_minimum = 0.75;
    continuous_ranged.starting_colors.blue_maximum = 0.75;
    pvt::Image continuous_ranged_image;
    CHECK(pvt::render_frame_at_phase(
        continuous_ranged, 0.0, continuous_ranged_image, &error));
    for (std::size_t offset = 0U;
         offset < continuous_ranged_image.pixels.size(); offset += 4U) {
        CHECK(std::fabs(continuous_ranged_image.pixels[offset]
                        - linear_test(0.15)) < 1.0e-6);
        CHECK(std::fabs(continuous_ranged_image.pixels[offset + 1U]
                        - linear_test(0.35)) < 1.0e-6);
        CHECK(std::fabs(continuous_ranged_image.pixels[offset + 2U]
                        - linear_test(0.75)) < 1.0e-6);
    }

    // Very large authored canvases compute rainbow progress from the authored
    // dimensions without allocating that full image for a reduced preview.
    pvt::RenderConfig large_reference = scaled;
    large_reference.width = 64;
    large_reference.height = 64;
    large_reference.starting_colors.mode =
        pvt::StartingColorMode::VerticalRainbow;
    large_reference.starting_colors.reference_width = 24000;
    large_reference.starting_colors.reference_height = 24000;
    large_reference.starting_colors.reference_block_size = 1;
    pvt::Image large_preview;
    CHECK(pvt::render_frame_at_phase(
        large_reference, 0.0, large_preview, &error));
    std::set<std::array<float, 3U>> large_preview_colors;
    for (std::size_t offset = 0U; offset < large_preview.pixels.size();
         offset += 4U) {
        large_preview_colors.insert({large_preview.pixels[offset],
                                     large_preview.pixels[offset + 1U],
                                     large_preview.pixels[offset + 2U]});
    }
    CHECK(large_preview_colors.size()
          == static_cast<std::size_t>(large_reference.width
                                      * large_reference.height));
    CHECK(large_preview.pixel(0, 0) != nullptr);
    CHECK(large_preview.pixel(large_reference.width - 1,
                              large_reference.height - 1) != nullptr);
    if (const float* first = large_preview.pixel(0, 0)) {
        if (const float* last = large_preview.pixel(
                large_reference.width - 1,
                large_reference.height - 1)) {
            CHECK(!std::equal(first, first + 3, last));
        }
    }
    large_reference.starting_colors.mode =
        pvt::StartingColorMode::SpiralRainbow;
    pvt::Image large_spiral_preview;
    CHECK(pvt::render_frame_at_phase(
        large_reference, 0.0, large_spiral_preview, &error));
    CHECK(large_spiral_preview.pixel(0, 0) != nullptr);
    CHECK(large_spiral_preview.pixel(large_reference.width - 1,
                                     large_reference.height - 1) != nullptr);

    // A reduced preview samples the same full-resolution lattice coordinates
    // as export. This prevents a preview resize from rearranging source colors.
    pvt::RenderConfig preview = scaled;
    preview.width = scaled.width / 2;
    preview.height = scaled.height / 2;
    preview.starting_colors.reference_width = scaled.width;
    preview.starting_colors.reference_height = scaled.height;
    preview.starting_colors.reference_block_size = scaled.block_size;
    pvt::Image preview_image;
    CHECK(pvt::render_frame_at_phase(preview, 0.0, preview_image, &error));
    for (int y = 0; y < preview.height; ++y) {
        for (int x = 0; x < preview.width; ++x) {
            const float* preview_pixel = preview_image.pixel(x, y);
            const float* export_pixel = scaled_start.pixel(x * 2, y * 2);
            CHECK(preview_pixel != nullptr && export_pixel != nullptr);
            if (preview_pixel != nullptr && export_pixel != nullptr) {
                CHECK(std::equal(preview_pixel, preview_pixel + 4,
                                 export_pixel));
            }
        }
    }
    preview.starting_colors.mode = pvt::StartingColorMode::SpiralRainbow;
    pvt::Image spiral_preview;
    CHECK(pvt::render_frame_at_phase(
        preview, 0.0, spiral_preview, &error));
    for (int y = 0; y < preview.height; ++y) {
        for (int x = 0; x < preview.width; ++x) {
            const float* preview_pixel = spiral_preview.pixel(x, y);
            const float* export_pixel = radial_spiral_start.pixel(x * 2, y * 2);
            CHECK(preview_pixel != nullptr && export_pixel != nullptr);
            if (preview_pixel != nullptr && export_pixel != nullptr) {
                CHECK(std::equal(preview_pixel, preview_pixel + 4,
                                 export_pixel));
            }
        }
    }

    // Generated source ordering must not bypass the procedural color signal.
    exhaustive.width = 32;
    exhaustive.height = 16;
    exhaustive.starting_colors.include_alpha = false;
    exhaustive.starting_colors.mode = pvt::StartingColorMode::VerticalRainbow;
    exhaustive.spiral_enabled = false;
    exhaustive.wall_reflection_enabled = false;
    pvt::Image procedural_off;
    CHECK(pvt::render_frame_at_phase(
        exhaustive, 0.25, procedural_off, &error));
    exhaustive.spiral_enabled = true;
    exhaustive.spiral_frequency = 2.25;
    exhaustive.spiral_arms = 3;
    pvt::Image procedural_spiral;
    CHECK(pvt::render_frame_at_phase(
        exhaustive, 0.25, procedural_spiral, &error));
    CHECK(mean_absolute_difference(procedural_off, procedural_spiral) > 0.0001);
    exhaustive.spiral_enabled = false;
    exhaustive.wall_reflection_enabled = true;
    exhaustive.wall_frequency = 3.5;
    exhaustive.wall_mix = 1.0;
    pvt::Image procedural_wall;
    CHECK(pvt::render_frame_at_phase(
        exhaustive, 0.25, procedural_wall, &error));
    CHECK(mean_absolute_difference(procedural_off, procedural_wall) > 0.0001);

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

    // Reverse changes traversal direction, not the separately authored
    // starting phase. Follow-tangent orientation must face actual travel and
    // survive preparation for strict GPU rendering.
    path_config.waves.front().synchronized = false;
    path_config.waves.front().path.phase_degrees = 90.0;
    pvt::detail::PreparedFrame forward_path;
    CHECK(pvt::detail::prepare_frame_for_backend_at_phase(
        path_config, 0.0, forward_path, &error));
    path_config.waves.front().path.reverse = true;
    pvt::detail::PreparedFrame reverse_path;
    CHECK(pvt::detail::prepare_frame_for_backend_at_phase(
        path_config, 0.0, reverse_path, &error));
    CHECK(forward_path.waves.size() == reverse_path.waves.size());
    if (!forward_path.waves.empty() && !reverse_path.waves.empty()) {
        const auto& forward = forward_path.waves.front();
        const auto& reverse = reverse_path.waves.front();
        CHECK(std::fabs(forward.source_x - reverse.source_x) < 1.0e-9);
        CHECK(std::fabs(forward.source_y - reverse.source_y) < 1.0e-9);
        CHECK(forward.follow_tangent && reverse.follow_tangent);
        CHECK(std::fabs(std::cos(forward.tangent_radians)
                        + std::cos(reverse.tangent_radians)) < 1.0e-9);
        CHECK(std::fabs(std::sin(forward.tangent_radians)
                        + std::sin(reverse.tangent_radians)) < 1.0e-9);
    }

    // A static authored rotation is render work even without a built-in path.
    pvt::RenderConfig static_rotation = pvt::default_config();
    make_small(static_rotation);
    static_rotation.output.write_alpha = true;
    static_rotation.motion.enabled = false;
    pvt::Image unrotated;
    pvt::Image rotated;
    CHECK(pvt::render_frame_at_phase(
        static_rotation, 0.0, unrotated, &error));
    static_rotation.motion.enabled = true;
    static_rotation.motion.path = pvt::LayerMotionPath::None;
    static_rotation.motion.rotation_offset_degrees = 31.0;
    CHECK(pvt::render_frame_at_phase(
        static_rotation, 0.0, rotated, &error));
    CHECK(mean_absolute_difference(unrotated, rotated) > 0.00001);

    // A selected built-in path with no travel is an exact identity. Likewise,
    // a zero-cycle scale pulse at a zero-crossing never changes scale.
    pvt::RenderConfig identity_motion = pvt::default_config();
    make_small(identity_motion);
    identity_motion.motion.enabled = true;
    identity_motion.motion.path = pvt::LayerMotionPath::Orbit;
    identity_motion.motion.travel_x = 0.0;
    identity_motion.motion.travel_y = 0.0;
    CHECK(pvt::validate(identity_motion).ok);
    pvt::Image identity_path;
    CHECK(pvt::render_frame_at_phase(
        identity_motion, 0.37, identity_path, &error));
    identity_motion.motion.path = pvt::LayerMotionPath::None;
    identity_motion.motion.scale_pulse = 0.5;
    identity_motion.motion.cycles_y = 0;
    identity_motion.motion.phase_degrees = 180.0;
    CHECK(pvt::validate(identity_motion).ok);
    pvt::Image identity_scale;
    CHECK(pvt::render_frame_at_phase(
        identity_motion, 0.37, identity_scale, &error));
    identity_motion.motion.enabled = false;
    pvt::Image identity_disabled;
    CHECK(pvt::render_frame_at_phase(
        identity_motion, 0.37, identity_disabled, &error));
    CHECK(identity_path.pixels == identity_disabled.pixels);
    CHECK(identity_scale.pixels == identity_disabled.pixels);

    // Starting phase is independent of how many cycles are authored. Cycle
    // count changes subsequent travel, not the layer's phase-zero placement.
    pvt::RenderConfig phased_motion = pvt::default_config();
    make_small(phased_motion);
    phased_motion.output.write_alpha = true;
    phased_motion.motion.enabled = true;
    phased_motion.motion.path = pvt::LayerMotionPath::Orbit;
    phased_motion.motion.phase_degrees = 90.0;
    phased_motion.motion.cycles_x = 1;
    pvt::Image one_cycle_start;
    pvt::Image one_cycle_later;
    pvt::Image two_cycle_start;
    pvt::Image two_cycle_later;
    CHECK(pvt::render_frame_at_phase(
        phased_motion, 0.0, one_cycle_start, &error));
    CHECK(pvt::render_frame_at_phase(
        phased_motion, 0.125, one_cycle_later, &error));
    phased_motion.motion.cycles_x = 2;
    CHECK(pvt::render_frame_at_phase(
        phased_motion, 0.0, two_cycle_start, &error));
    CHECK(pvt::render_frame_at_phase(
        phased_motion, 0.125, two_cycle_later, &error));
    CHECK(one_cycle_start.pixels == two_cycle_start.pixels);
    CHECK(mean_absolute_difference(one_cycle_later, two_cycle_later)
          > 0.00001);

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
    constexpr std::size_t cancellation_wave_count = 512U;
    pvt::RenderConfig config = pvt::default_config();
    config.width = 1024;
    config.height = 1024;
    config.block_size = 1;
    config.waves.reserve(cancellation_wave_count);
    for (std::size_t index = config.waves.size();
        index < cancellation_wave_count; ++index) {
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
    CHECK(static_cast<std::uint8_t>(
              pvt::StartingColorMode::SquareSpiralRainbow) == 4U);
    CHECK(static_cast<std::uint8_t>(
              pvt::StartingColorMode::Subtractive) == 4U);
    CHECK(static_cast<std::uint8_t>(pvt::StartingColorMode::Random) == 5U);
    CHECK(static_cast<std::uint8_t>(
              pvt::StartingColorMode::SpiralRainbow) == 6U);
    auto config = pvt::default_config();
    make_small(config);
    CHECK(pvt::validate(config).ok);
    CHECK(config.waves.size() == 3);
    CHECK(config.effects.size() >= 7);
    CHECK(config.output.png_compression_level == 5);
    CHECK(config.audio_reactive_override_enabled);
    CHECK(!config.audio_reactive_defaults.enabled);
    CHECK(config.waves.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(config.effects.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(std::string(pvt::audio_response_mode_name(
              pvt::AudioResponseMode::Beat)) == "Beat");
    CHECK(std::string(pvt::audio_response_mode_name(
              pvt::AudioResponseMode::Enabled))
          == "Profile source (force this item on)");
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

    // Free waves keep their independent seamless clock even while the shared
    // synchronized clock is held. Re-enabling synchronization makes the same
    // frames identical again.
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
    CHECK(held_zero.pixels != held_one.pixels);
    CHECK(held_zero.pixels != held_two.pixels);
    CHECK(held_zero.pixels != next_pulse.pixels);
    for (pvt::WaveConfig& wave : config.waves) wave.synchronized = true;
    CHECK(pvt::render_frame(config, 0, held_zero, &error));
    CHECK(pvt::render_frame(config, 1, held_one, &error));
    CHECK(held_zero.pixels == held_one.pixels);

    // Endless Zoom follows the same contract: synchronized instances consume
    // the held clock, while free instances advance on their own loop clock.
    pvt::RenderConfig zoom_clock = config;
    zoom_clock.effects.clear();
    auto zoom = pvt::default_effect(pvt::EffectType::EndlessZoom);
    zoom.id = pvt::allocate_id(zoom_clock);
    zoom.enabled = true;
    zoom.synchronized = true;
    zoom_clock.effects.push_back(zoom);
    CHECK(pvt::render_frame(zoom_clock, 0, held_zero, &error));
    CHECK(pvt::render_frame(zoom_clock, 1, held_one, &error));
    CHECK(held_zero.pixels == held_one.pixels);
    zoom_clock.effects.front().synchronized = false;
    CHECK(pvt::render_frame(zoom_clock, 0, held_zero, &error));
    CHECK(pvt::render_frame(zoom_clock, 1, held_one, &error));
    CHECK(held_zero.pixels != held_one.pixels);

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
    CHECK(pvt::describe_meter("2147483647/4", meter_description, &error));
    CHECK(meter_description.find("2147483647 pulses")
          != std::string::npos);
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

    // Very large valid pulse counts stay compact, and signed INT64 beat
    // offsets must not lose the frame delta in a huge absolute pulse index.
    config.clock.interpolation = pvt::ClockInterpolation::Linear;
    config.clock.meter.expression = "2147483647/4";
    config.clock.meter.bpm = 1000.0;
    config.clock.meter.tempo_note_denominator = 1;
    pvt::Image extreme_start;
    pvt::Image extreme_next;
    for (const std::int64_t offset : {
             (std::numeric_limits<std::int64_t>::min)(),
             (std::numeric_limits<std::int64_t>::max)()}) {
        config.clock.beat_offset_microseconds = offset;
        CHECK(pvt::validate(config).ok);
        CHECK(pvt::render_frame(config, 0, extreme_start, &error));
        CHECK(pvt::render_frame(config, 1, extreme_next, &error));
        CHECK(error.empty());
        CHECK(mean_absolute_difference(extreme_start, extreme_next)
              > 0.00001);
    }

    // At the signed limit, offsets one microsecond apart must remain distinct
    // even when the meter cycle itself is only three microseconds. The split
    // integer reduction preserves that low-order authored timing information.
    pvt::RenderConfig tiny_cycle = pvt::default_config();
    make_small(tiny_cycle);
    tiny_cycle.total_frames = 2;
    tiny_cycle.fps = 239.0;
    tiny_cycle.clock.mode = pvt::ClockMode::Meter;
    tiny_cycle.clock.interpolation = pvt::ClockInterpolation::Hold;
    tiny_cycle.clock.meter.expression = "1/20000";
    tiny_cycle.clock.meter.bpm = 1000.0;
    tiny_cycle.clock.meter.tempo_note_denominator = 1;
    tiny_cycle.clock.beat_offset_microseconds =
        (std::numeric_limits<std::int64_t>::max)() - 1;
    pvt::detail::PreparedFrame tiny_even;
    pvt::detail::PreparedFrame tiny_odd;
    pvt::Image tiny_even_image;
    pvt::Image tiny_odd_image;
    CHECK(pvt::detail::prepare_frame_for_backend(
        tiny_cycle, 1, tiny_even, &error));
    CHECK(pvt::render_frame(tiny_cycle, 1, tiny_even_image, &error));
    tiny_cycle.clock.beat_offset_microseconds =
        (std::numeric_limits<std::int64_t>::max)();
    CHECK(pvt::detail::prepare_frame_for_backend(
        tiny_cycle, 1, tiny_odd, &error));
    CHECK(pvt::render_frame(tiny_cycle, 1, tiny_odd_image, &error));
    CHECK(std::fabs(tiny_even.loop_phase - tiny_odd.loop_phase) > 0.0001);
    CHECK(mean_absolute_difference(tiny_even_image, tiny_odd_image)
          > 0.0000001);

    // This valid one-pulse clock lands infinitesimally below a cycle boundary:
    // floating subtraction produces a negative local remainder. Correcting
    // that remainder must also decrement the whole-cycle ordinal, preserving
    // the exact linear-clock identity frame/total_frames.
    pvt::RenderConfig boundary_clock = pvt::default_config();
    make_small(boundary_clock);
    constexpr int kBoundaryFrame = 1690074;
    boundary_clock.total_frames = kBoundaryFrame + 2;
    boundary_clock.fps = 1.631247438212073;
    boundary_clock.clock.mode = pvt::ClockMode::Meter;
    boundary_clock.clock.interpolation = pvt::ClockInterpolation::Linear;
    boundary_clock.clock.meter.expression = "1/1";
    boundary_clock.clock.meter.bpm = 315.5763844695659;
    boundary_clock.clock.meter.tempo_note_denominator = 1;
    pvt::detail::PreparedFrame boundary_prepared;
    CHECK(pvt::detail::prepare_frame_for_backend(
        boundary_clock, kBoundaryFrame, boundary_prepared, &error));
    const double expected_boundary_phase =
        2.0 * 3.141592653589793238462643383279502884
        * static_cast<double>(kBoundaryFrame)
        / static_cast<double>(boundary_clock.total_frames);
    CHECK(std::fabs(boundary_prepared.loop_phase - expected_boundary_phase)
          < 1.0e-9);

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
    music.palette.colors = {
        {1.0, 0.0, 0.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
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

    // Synchronized items can override both the effective category default and
    // its selected feature. Legacy format-8 force/ignore modes retain their
    // exact behavior.
    music.audio_reactive.waves_enabled = false;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels == at_spike.pixels);
    music.waves.front().audio_response = pvt::AudioResponseMode::Enabled;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    music.waves.front().audio_response = pvt::AudioResponseMode::Energy;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);

    pvt::detail::PreparedFrame prepared_before;
    pvt::detail::PreparedFrame prepared_at;
    // Per-item audio routing is independent of clock synchronization. The
    // synchronized-only profile switch remains an explicit opt-in filter.
    music.waves.front().synchronized = false;
    music.audio_reactive.synchronized_only = false;
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 4, prepared_before, &error));
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 5, prepared_at, &error));
    CHECK(prepared_before.waves.front().amplitude
          != prepared_at.waves.front().amplitude);
    music.audio_reactive.synchronized_only = true;
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 4, prepared_before, &error));
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 5, prepared_at, &error));
    CHECK(prepared_before.waves.front().amplitude
          == prepared_at.waves.front().amplitude);
    music.audio_reactive.synchronized_only = false;
    music.waves.front().synchronized = true;
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 4, prepared_before, &error));
    CHECK(pvt::detail::prepare_frame_for_backend(
        music, 5, prepared_at, &error));
    CHECK(!prepared_before.waves.empty());
    CHECK(prepared_before.waves.size() == prepared_at.waves.size());
    if (!prepared_before.waves.empty()
        && prepared_before.waves.size() == prepared_at.waves.size()) {
        CHECK(prepared_before.waves.front().amplitude
              != prepared_at.waves.front().amplitude);
    }

    music.audio_reactive.waves_enabled = true;
    music.audio_reactive.wave_source = pvt::MusicFeature::Beat;
    for (auto& wave : music.waves) {
        wave.audio_response = pvt::AudioResponseMode::Default;
    }
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels == at_spike.pixels);
    music.waves.front().audio_response = pvt::AudioResponseMode::Energy;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);
    for (auto& wave : music.waves) {
        wave.audio_response = pvt::AudioResponseMode::Disabled;
    }
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels == at_spike.pixels);
    for (auto& wave : music.waves) {
        wave.audio_response = pvt::AudioResponseMode::Default;
    }

    music.audio_reactive.waves_enabled = false;
    music.audio_reactive.effects_enabled = true;
    music.audio_reactive.effect_source = pvt::MusicFeature::Beat;
    music.audio_reactive.effect_amount = 1.0;
    const auto ripple = std::find_if(
        music.effects.begin(), music.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::Ripple;
        });
    CHECK(ripple != music.effects.end());
    if (ripple != music.effects.end()) ripple->enabled = true;
    CHECK(pvt::render_frame(music, 4, before_spike, &error));
    CHECK(pvt::render_frame(music, 5, at_spike, &error));
    CHECK(before_spike.pixels == at_spike.pixels);
    if (ripple != music.effects.end()) {
        music.audio_reactive.effects_enabled = false;
        music.audio_reactive.effect_source = pvt::MusicFeature::Energy;
        ripple->audio_response = pvt::AudioResponseMode::Enabled;
        CHECK(pvt::render_frame(music, 4, before_spike, &error));
        CHECK(pvt::render_frame(music, 5, at_spike, &error));
        CHECK(before_spike.pixels != at_spike.pixels);
        music.audio_reactive.effect_source = pvt::MusicFeature::Beat;
        ripple->audio_response = pvt::AudioResponseMode::Energy;
        CHECK(pvt::render_frame(music, 4, before_spike, &error));
        CHECK(pvt::render_frame(music, 5, at_spike, &error));
        CHECK(before_spike.pixels != at_spike.pixels);
        CHECK(pvt::detail::prepare_frame_for_backend(
            music, 4, prepared_before, &error));
        CHECK(pvt::detail::prepare_frame_for_backend(
            music, 5, prepared_at, &error));
        const auto prepared_ripple_before = std::find_if(
            prepared_before.effects.begin(), prepared_before.effects.end(),
            [](const auto& effect) {
                return effect.type == pvt::EffectType::Ripple;
            });
        const auto prepared_ripple_at = std::find_if(
            prepared_at.effects.begin(), prepared_at.effects.end(),
            [](const auto& effect) {
                return effect.type == pvt::EffectType::Ripple;
            });
        CHECK(prepared_ripple_before != prepared_before.effects.end());
        CHECK(prepared_ripple_at != prepared_at.effects.end());
        if (prepared_ripple_before != prepared_before.effects.end()
            && prepared_ripple_at != prepared_at.effects.end()) {
            CHECK(prepared_ripple_before->intensity
                  != prepared_ripple_at->intensity);
        }
        music.audio_reactive.effects_enabled = true;
        ripple->audio_response = pvt::AudioResponseMode::Disabled;
        CHECK(pvt::render_frame(music, 4, before_spike, &error));
        CHECK(pvt::render_frame(music, 5, at_spike, &error));
        CHECK(before_spike.pixels == at_spike.pixels);
        ripple->audio_response = pvt::AudioResponseMode::Default;
    }

    // Endless Zoom's default intensity is already a full-strength blend, but
    // positive audio response must still produce a visible change instead of
    // disappearing into that clamp.
    pvt::RenderConfig music_zoom = pvt::default_config();
    make_small(music_zoom);
    music_zoom.fps = 10.0;
    music_zoom.clock = ready_music_clock();
    music_zoom.clock.interpolation = pvt::ClockInterpolation::Hold;
    music_zoom.clock.music.beat_times_seconds = {0.0, 1.0};
    music_zoom.clock.music.feature_samples.assign(11U, {});
    music_zoom.clock.music.feature_samples[5U].energy = 1.0F;
    music_zoom.audio_reactive.enabled = true;
    music_zoom.audio_reactive.effects_enabled = false;
    music_zoom.audio_reactive.effect_source = pvt::MusicFeature::Energy;
    music_zoom.audio_reactive.effect_amount = 1.0;
    music_zoom.effects.clear();
    zoom = pvt::default_effect(pvt::EffectType::EndlessZoom);
    zoom.id = pvt::allocate_id(music_zoom);
    zoom.enabled = true;
    zoom.synchronized = true;
    zoom.audio_response = pvt::AudioResponseMode::Energy;
    music_zoom.effects.push_back(zoom);
    CHECK(pvt::render_frame(music_zoom, 4, before_spike, &error));
    CHECK(pvt::render_frame(music_zoom, 5, at_spike, &error));
    CHECK(before_spike.pixels != at_spike.pixels);
    CHECK(pvt::detail::prepare_frame_for_backend(
        music_zoom, 4, prepared_before, &error));
    CHECK(pvt::detail::prepare_frame_for_backend(
        music_zoom, 5, prepared_at, &error));
    CHECK(prepared_before.effects.size() == 1U);
    CHECK(prepared_at.effects.size() == 1U);
    if (prepared_before.effects.size() == 1U
        && prepared_at.effects.size() == 1U) {
        CHECK(prepared_before.effects.front().intensity
              != prepared_at.effects.front().intensity);
    }

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

void test_layer_clock_mixing_and_generated_shaping() {
    constexpr double kTau = 6.283185307179586476925286766559;
    const auto normalized_phase = [&](const pvt::detail::PreparedFrame& frame) {
        return frame.loop_phase / kTau;
    };
    const auto close = [](double first, double second) {
        return std::fabs(first - second) < 1.0e-12;
    };

    pvt::RenderConfig clock = pvt::default_config();
    CHECK(!clock.layer_clock.mix_enabled);
    CHECK(clock.layer_clock.mix == pvt::LayerClockMixMode::Replace);
    make_small(clock);
    clock.total_frames = 16;
    clock.clock = {};
    clock.clock.phase_offset_degrees = 36.0;
    clock.layer_clock.enabled = true;
    clock.layer_clock.clock = {};
    clock.layer_clock.clock.phase_offset_degrees = 72.0;
    clock.layer_clock.mix = pvt::LayerClockMixMode::Add;
    clock.layer_clock.mix_enabled = false;

    std::string error;
    pvt::detail::PreparedFrame prepared;
    CHECK(pvt::detail::prepare_frame_for_backend(clock, 4, prepared, &error));
    // Mixing is opt-in. A disabled non-Replace record must preserve the
    // historical active-layer behavior and replace the project clock.
    CHECK(close(normalized_phase(prepared), 0.45));

    clock.layer_clock.mix_enabled = true;
    const std::array<std::pair<pvt::LayerClockMixMode, double>, 4U> expected{{
        {pvt::LayerClockMixMode::Replace, 0.45},
        {pvt::LayerClockMixMode::Add, 0.80},
        {pvt::LayerClockMixMode::Difference, 0.90},
        {pvt::LayerClockMixMode::SoftXor, 0.485},
    }};
    for (const auto& [mode, phase] : expected) {
        clock.layer_clock.mix = mode;
        CHECK(pvt::detail::prepare_frame_for_backend(
            clock, 4, prepared, &error));
        CHECK(close(normalized_phase(prepared), phase));
    }
    clock.layer_clock.mix = pvt::LayerClockMixMode::BitwiseXor;
    CHECK(pvt::detail::prepare_frame_for_backend(clock, 4, prepared, &error));
    constexpr std::uint32_t kFixedScale = UINT32_C(1) << 24U;
    const std::uint32_t project_fixed = static_cast<std::uint32_t>(
        std::floor(0.35 * static_cast<double>(kFixedScale)));
    const std::uint32_t layer_fixed = static_cast<std::uint32_t>(
        std::floor(0.45 * static_cast<double>(kFixedScale)));
    const double xor_phase = static_cast<double>(project_fixed ^ layer_fixed)
                             / static_cast<double>(kFixedScale);
    CHECK(close(normalized_phase(prepared), xor_phase));
    CHECK(std::string(pvt::layer_clock_mix_mode_name(
              pvt::LayerClockMixMode::BitwiseXor)) == "Bitwise XOR");

    // The project timeline remains authoritative, while a layer Music clock
    // supplies the effective dense audio envelope after clock mixing.
    pvt::RenderConfig music = pvt::default_config();
    make_small(music);
    music.total_frames = 10;
    music.fps = 10.0;
    music.clock = {};
    music.layer_clock.enabled = true;
    music.layer_clock.scale = pvt::LayerClockScale::OriginalSpeedLoop;
    music.layer_clock.clock = ready_music_clock(1.0, 10U);
    music.layer_clock.clock.music.feature_samples.assign(11U, {});
    music.layer_clock.clock.music.feature_samples[5U].energy = 0.8F;
    music.audio_reactive.enabled = true;
    music.audio_reactive.color_enabled = true;
    music.audio_reactive.color_source = pvt::MusicFeature::Energy;
    music.audio_reactive.color_amount_degrees = 100.0;
    CHECK(pvt::effective_frame_count(music, &error) == 10);
    CHECK(pvt::detail::prepare_frame_for_backend(music, 5, prepared, &error));
    CHECK(std::fabs(prepared.audio_hue_shift_degrees - 80.0) < 1.0e-5);

    // An active layer Music clock historically supplied the independent phase
    // too. Keep free-running effects and path bindings on the mapped layer
    // timeline even though mixing is disabled by default.
    music.total_frames = 20;
    CHECK(pvt::detail::prepare_frame_for_backend(music, 15, prepared, &error));
    CHECK(close(prepared.independent_loop_phase / kTau, 0.5));
    music.layer_clock.scale = pvt::LayerClockScale::PlayOnceThenProject;
    CHECK(pvt::detail::prepare_frame_for_backend(music, 15, prepared, &error));
    CHECK(close(prepared.independent_loop_phase / kTau, 0.75));
    // Once the one-shot layer clock hands off, the project clock is the
    // complete visual timeline. Mixing must not combine the project phase
    // with the copied project phase (Add would double it and Difference/XOR
    // would collapse it).
    music.layer_clock.mix_enabled = true;
    for (const pvt::LayerClockMixMode mode : {
             pvt::LayerClockMixMode::Replace,
             pvt::LayerClockMixMode::Add,
             pvt::LayerClockMixMode::Difference,
             pvt::LayerClockMixMode::SoftXor,
             pvt::LayerClockMixMode::BitwiseXor}) {
        music.layer_clock.mix = mode;
        CHECK(pvt::detail::prepare_frame_for_backend(
            music, 15, prepared, &error));
        CHECK(close(normalized_phase(prepared), 0.75));
        CHECK(close(prepared.independent_loop_phase / kTau, 0.75));
    }

    pvt::RenderConfig generated = pvt::default_config();
    make_small(generated);
    generated.waves.clear();
    generated.swings.clear();
    generated.effects.clear();
    generated.displacement_enabled = false;
    generated.lighting_enabled = false;
    generated.starting_colors.kaleidoscope.mirrored_segments = 7;
    generated.starting_colors.kaleidoscope.rotation_degrees = 19.0;
    generated.starting_colors.kaleidoscope.mix = 0.82;
    generated.starting_colors.domain_warp.strength = 0.28;
    generated.starting_colors.domain_warp.scale = 2.7;
    generated.starting_colors.domain_warp.octaves = 4;
    generated.starting_colors.domain_warp.cycles_per_loop = 3;
    generated.starting_colors.domain_warp.seed = UINT64_C(0x123456789abcdef0);

    pvt::Image baseline;
    pvt::Image neutral;
    pvt::Image shaped;
    pvt::Image repeated;
    pvt::Image seam;
    CHECK(pvt::render_frame_at_phase(generated, 0.37, baseline, &error));
    CHECK(pvt::render_frame_at_phase(generated, 0.37, neutral, &error));
    CHECK(baseline.pixels == neutral.pixels);

    generated.starting_colors.kaleidoscope.enabled = true;
    CHECK(pvt::render_frame_at_phase(generated, 0.37, shaped, &error));
    CHECK(mean_absolute_difference(baseline, shaped) > 0.0001);
    CHECK(pvt::render_frame_at_phase(generated, 0.37, repeated, &error));
    CHECK(shaped.pixels == repeated.pixels);

    generated.starting_colors.kaleidoscope.enabled = false;
    generated.starting_colors.domain_warp.enabled = true;
    CHECK(pvt::render_frame_at_phase(generated, 0.37, shaped, &error));
    CHECK(mean_absolute_difference(baseline, shaped) > 0.0001);
    CHECK(pvt::render_frame_at_phase(generated, 0.37, repeated, &error));
    CHECK(shaped.pixels == repeated.pixels);
    const pvt::Image first_seed = shaped;
    CHECK(pvt::render_frame_at_phase(generated, 0.0, shaped, &error));
    CHECK(pvt::render_frame_at_phase(generated, 1.0, seam, &error));
    CHECK(shaped.pixels == seam.pixels);
    generated.starting_colors.domain_warp.seed ^= UINT64_C(0x55aa55aa);
    CHECK(pvt::render_frame_at_phase(generated, 0.37, repeated, &error));
    CHECK(mean_absolute_difference(repeated, first_seed) > 0.0001);
}

void test_new_procedural_effects() {
    pvt::RenderConfig base = pvt::default_config();
    make_small(base);
    base.effects.clear();
    base.output.write_alpha = true;
    base.alpha.enabled = true;
    base.alpha.minimum = 0.15;
    base.alpha.maximum = 0.85;

    std::string error;
    pvt::Image unchanged;
    CHECK(pvt::render_frame_at_phase(base, 0.37, unchanged, &error));
    for (const pvt::EffectType type : {
             pvt::EffectType::Glitch,
             pvt::EffectType::Starburst,
             pvt::EffectType::LensDistortion,
             pvt::EffectType::EdgeDetect,
             pvt::EffectType::Twirl}) {
        pvt::RenderConfig config = base;
        pvt::EffectConfig effect = pvt::default_effect(type);
        effect.id = UINT64_C(0x1234567800000000)
                    + static_cast<std::uint64_t>(type);
        effect.enabled = true;
        effect.edge_mode = pvt::EdgeMode::Alpha;
        config.effects.push_back(effect);
        CHECK(pvt::validate(config).ok);
        CHECK(std::string(pvt::effect_type_name(type)) != "Unknown");

        pvt::Image first;
        pvt::Image second;
        pvt::Image loop_end;
        CHECK(pvt::render_frame_at_phase(config, 0.37, first, &error));
        CHECK(pvt::render_frame_at_phase(config, 0.37, second, &error));
        CHECK(first.pixels == second.pixels);
        CHECK(mean_absolute_difference(unchanged, first) > 0.000001);
        CHECK(pvt::render_frame_at_phase(config, 0.0, first, &error));
        CHECK(pvt::render_frame_at_phase(config, 1.0, loop_end, &error));
        CHECK(first.pixels == loop_end.pixels);
        for (std::size_t offset = 0U; offset < first.pixels.size();
             offset += 4U) {
            CHECK(std::isfinite(first.pixels[offset]));
            CHECK(std::isfinite(first.pixels[offset + 1U]));
            CHECK(std::isfinite(first.pixels[offset + 2U]));
            CHECK(std::isfinite(first.pixels[offset + 3U]));
            CHECK(first.pixels[offset + 3U] >= 0.0F);
            CHECK(first.pixels[offset + 3U] <= 1.0F);
        }
    }
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
    const std::array<pvt::EffectType, 13U> effect_types{{
        pvt::EffectType::EndlessZoom,
        pvt::EffectType::Ripple,
        pvt::EffectType::Shake,
        pvt::EffectType::FlagWave,
        pvt::EffectType::Glow,
        pvt::EffectType::BlockScale,
        pvt::EffectType::ParticleField,
        pvt::EffectType::Blur,
        pvt::EffectType::Glitch,
        pvt::EffectType::Starburst,
        pvt::EffectType::LensDistortion,
        pvt::EffectType::EdgeDetect,
        pvt::EffectType::Twirl,
    }};
    for (const pvt::EffectType type : effect_types) {
        for (const bool synchronized : {false, true}) {
            auto one_effect = pvt::default_config();
            make_small(one_effect);
            one_effect.effects.clear();
            auto effect = pvt::default_effect(type);
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
            if (mapping == pvt::SurfaceMapping::Cylinder) {
                int minimum_x = config.width;
                int maximum_x = -1;
                int minimum_y = config.height;
                int maximum_y = -1;
                for (int y = 0; y < config.height; ++y) {
                    for (int x = 0; x < config.width; ++x) {
                        const float* pixel = radial.pixel(x, y);
                        if (pixel != nullptr && pixel[3] > 0.5F) {
                            minimum_x = std::min(minimum_x, x);
                            maximum_x = std::max(maximum_x, x);
                            minimum_y = std::min(minimum_y, y);
                            maximum_y = std::max(maximum_y, y);
                        }
                    }
                }
                CHECK(maximum_x > minimum_x);
                CHECK(maximum_y > minimum_y);
                int transparent_bounds_corners = 0;
                for (const auto corner : {
                         std::pair{minimum_x, minimum_y},
                         std::pair{maximum_x, minimum_y},
                         std::pair{minimum_x, maximum_y},
                         std::pair{maximum_x, maximum_y}}) {
                    const float* pixel = radial.pixel(corner.first,
                                                      corner.second);
                    transparent_bounds_corners +=
                        pixel != nullptr && pixel[3] < 0.01F ? 1 : 0;
                }
                // The old implementation was exactly a rectangular side
                // mask. A closed tilted cylinder has rounded cap/side bounds.
                CHECK(transparent_bounds_corners >= 1);
            }
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

void test_configurable_blur_effects() {
    pvt::RenderConfig config = pvt::default_config();
    make_small(config);
    config.width = 47;
    config.height = 31;
    config.block_size = 1;
    config.waves.clear();
    config.swings.clear();
    config.effects.clear();
    config.hue_cycles = 0;
    config.displacement_enabled = false;
    config.lighting_enabled = false;
    config.spiral_enabled = false;
    config.wall_reflection_enabled = false;
    config.quantization.enabled = false;
    auto blur = pvt::default_effect(pvt::EffectType::Blur);
    blur.id = pvt::allocate_id(config);
    blur.enabled = true;
    blur.synchronized = false;
    blur.cycles_per_loop = 1;
    blur.radius_pixels = 5.0;
    blur.blur_passes = 2;
    blur.blur_samples = 7;
    blur.blur_minimum = 0.0;
    blur.blur_maximum = 1.0;
    blur.blur_pulses_per_cycle = 1;
    blur.edge_mode = pvt::EdgeMode::Alpha;
    blur.center_x = 0.43;
    blur.center_y = 0.57;
    blur.angle_degrees = 27.0;
    config.effects.push_back(blur);

    std::string error;
    pvt::Image unblurred;
    pvt::Image modulated;
    CHECK(pvt::render_frame_at_phase(config, 0.0, unblurred, &error));
    CHECK(pvt::render_frame_at_phase(config, 0.5, modulated, &error));
    CHECK(mean_absolute_difference(unblurred, modulated) > 0.0001);
    bool made_transparent_edge = false;
    for (std::size_t offset = 3U; offset < modulated.pixels.size(); offset += 4U) {
        made_transparent_edge = made_transparent_edge
                                || modulated.pixels[offset] < 0.999F;
    }
    CHECK(made_transparent_edge);

    // The removed legacy field must not influence blur. Cycles per loop is the
    // sole modulation count and changing it must visibly change the result.
    pvt::Image one_cycle;
    pvt::Image legacy_pulse_value;
    pvt::Image two_cycles;
    config.effects.front().blur_pulses_per_cycle = 1;
    config.effects.front().cycles_per_loop = 1;
    CHECK(pvt::render_frame_at_phase(config, 0.25, one_cycle, &error));
    config.effects.front().blur_pulses_per_cycle = 97;
    CHECK(pvt::render_frame_at_phase(
        config, 0.25, legacy_pulse_value, &error));
    CHECK(one_cycle.pixels == legacy_pulse_value.pixels);
    config.effects.front().cycles_per_loop = 2;
    CHECK(pvt::render_frame_at_phase(config, 0.25, two_cycles, &error));
    CHECK(mean_absolute_difference(one_cycle, two_cycles) > 0.0001);
    config.effects.front().cycles_per_loop = 1;

    for (const auto type : {pvt::BlurType::Gaussian, pvt::BlurType::Box,
                            pvt::BlurType::Directional, pvt::BlurType::Radial,
                            pvt::BlurType::Zoom}) {
        config.effects.front().blur_type = type;
        CHECK(pvt::render_frame_at_phase(config, 0.37, modulated, &error));
    }

    // Equal bounds are the explicit constant-blur control. Clock routing
    // remains meaningful for other effect parameters, but cannot change this
    // constant mix.
    config.effects.front().blur_type = pvt::BlurType::Gaussian;
    config.effects.front().blur_minimum = 0.6;
    config.effects.front().blur_maximum = 0.6;
    pvt::Image constant_start;
    pvt::Image constant_middle;
    CHECK(pvt::render_frame_at_phase(config, 0.0, constant_start, &error));
    CHECK(pvt::render_frame_at_phase(config, 0.43, constant_middle, &error));
    CHECK(constant_start.pixels == constant_middle.pixels);
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
    config.palette.colors = {
        {0.0, 0.0, 0.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
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
    particles.particle_profile = pvt::ParticleRenderProfile::LegacyGlow;
    particles.particle_size_variation = 0.0;
    particles.particle_twinkle = 1.0;
    particles.particle_orientation = pvt::ParticleOrientation::Fixed;
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

void test_defined_particle_controls_and_silhouettes() {
    const pvt::EffectConfig defaults = pvt::default_effect(
        pvt::EffectType::ParticleField);
    CHECK(defaults.particle_profile == pvt::ParticleRenderProfile::Defined);
    CHECK(defaults.radius_pixels >= 8.0);
    CHECK(defaults.particle_size_variation > 0.0);
    CHECK(defaults.particle_orientation
          == pvt::ParticleOrientation::FollowMotion);

    auto config = pvt::default_config();
    make_small(config);
    config.width = 96;
    config.height = 96;
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
    config.palette.colors = {
        {0.0, 0.0, 0.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
    config.alpha.enabled = true;
    config.alpha.minimum = 0.0;
    config.alpha.maximum = 0.0;
    config.output.write_alpha = true;

    auto particles = defaults;
    particles.id = pvt::allocate_id(config);
    particles.enabled = true;
    particles.intensity = 0.7;
    particles.magnitude = 0.0;
    particles.frequency = 8.0;
    particles.secondary = 0.0;
    particles.radius_pixels = 10.0;
    particles.particle_size_variation = 0.0;
    particles.particle_definition = 0.9;
    particles.particle_twinkle = 0.0;
    particles.particle_seed = 1234567U;
    particles.particle_orientation = pvt::ParticleOrientation::FollowMotion;
    config.effects.push_back(particles);

    std::vector<pvt::Image> shapes;
    std::string error;
    for (const auto shape : {pvt::ParticleShape::Spark,
                             pvt::ParticleShape::SoftOrb,
                             pvt::ParticleShape::Ring,
                             pvt::ParticleShape::Diamond,
                             pvt::ParticleShape::Star}) {
        config.effects.front().particle_shape = shape;
        pvt::Image image;
        CHECK(pvt::render_frame_at_phase(config, 0.37, image, &error));
        shapes.push_back(std::move(image));
    }
    CHECK(mean_absolute_difference(shapes[0], shapes[3]) > 0.001);
    CHECK(mean_absolute_difference(shapes[0], shapes[4]) > 0.001);
    CHECK(mean_absolute_difference(shapes[3], shapes[4]) > 0.001);
    CHECK(mean_absolute_difference(shapes[1], shapes[2]) > 0.001);

    // A stationary motion-following field has no meaningful behind direction.
    // Trail amount must not manufacture a fixed-angle streak or stack repeated
    // copies at the same point.
    config.effects.front().particle_shape = pvt::ParticleShape::Star;
    config.effects.front().secondary = 0.0;
    pvt::Image no_trail;
    pvt::Image stationary_trail;
    CHECK(pvt::render_frame_at_phase(config, 0.37, no_trail, &error));
    config.effects.front().secondary = 1.0;
    CHECK(pvt::render_frame_at_phase(config, 0.37, stationary_trail, &error));
    CHECK(mean_absolute_difference(no_trail, stationary_trail) < 1.0e-12);

    config.effects.front().particle_seed = 7654321U;
    pvt::Image reseeded;
    CHECK(pvt::render_frame_at_phase(config, 0.37, reseeded, &error));
    CHECK(mean_absolute_difference(stationary_trail, reseeded) > 0.0001);

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
    config.palette.colors = {
        {0.0, 0.0, 0.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb},
        {1.0, 1.0, 1.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
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
    config.palette.colors = {
        {0.0, 0.0, 0.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb},
        {1.0, 1.0, 1.0, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
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

    // Interchange metadata is operational, not decorative: the same numeric
    // component is decoded from sRGB but passed through when authored linear.
    auto srgb_palette = config;
    srgb_palette.palette.colors = {
        {0.5, 0.5, 0.5, 1.0, {}, pvt::PaletteColorEncoding::Srgb}};
    pvt::Image srgb_palette_image;
    CHECK(pvt::render_frame_at_phase(
        srgb_palette, 0.371, srgb_palette_image, &error));
    auto linear_palette = srgb_palette;
    linear_palette.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    pvt::Image linear_palette_image;
    CHECK(pvt::render_frame_at_phase(
        linear_palette, 0.371, linear_palette_image, &error));
    CHECK(mean_absolute_difference(srgb_palette_image,
                                   linear_palette_image) > 0.1);
    CHECK(std::fabs(linear_palette_image.pixels.front() - 0.5F) < 1.0e-6F);

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
    CHECK(pvt::kMaximumWaves
          == static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    for (std::size_t index = config.waves.size(); index < 257U; ++index) {
        pvt::WaveConfig wave = pvt::default_wave(index);
        wave.id = static_cast<std::uint64_t>(1000U + index);
        config.waves.push_back(std::move(wave));
    }
    CHECK(pvt::validate(config).ok); // The former 256-wave policy cap is gone.
    while (config.palette.colors.size() < 257U) {
        config.palette.colors.push_back(config.palette.colors.back());
    }
    CHECK(pvt::validate(config).ok); // The former 256-color policy cap is gone.
    config = pvt::default_config();
    config.palette.enabled = true;
    config.palette.columns = 4U;
    config.palette.colors.front().name = "Linear HDR";
    config.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    config.palette.colors.front().red = -0.25;
    config.palette.colors.front().green = 2.5;
    CHECK(pvt::validate(config).ok);
    config.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Srgb;
    CHECK(!pvt::validate(config).ok);
    config.palette.colors.front().encoding =
        static_cast<pvt::PaletteColorEncoding>(255);
    CHECK(!pvt::validate(config).ok);
    config.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    config.palette.colors.front().red =
        std::numeric_limits<double>::infinity();
    CHECK(!pvt::validate(config).ok);
    config.palette.colors.front().red =
        static_cast<double>((std::numeric_limits<float>::max)());
    config.palette.enabled = false;
    CHECK(pvt::validate(config).ok);
    config = pvt::default_config();
    config.clock.time_interval_microseconds =
        (std::numeric_limits<std::int64_t>::max)();
    config.clock.beat_offset_microseconds =
        (std::numeric_limits<std::int64_t>::min)();
    config.output.filename_digits = 200;
    CHECK(pvt::validate(config).ok);
    config = pvt::default_config();
    config.output.bit_depth = 12;
    CHECK(!pvt::validate(config).ok);
    config = pvt::default_config();
    config.output.png_compression_level = -1;
    CHECK(!pvt::validate(config).ok);
    config.output.png_compression_level = 10;
    CHECK(!pvt::validate(config).ok);

    // RGB export must reject every active source of transparency, while
    // explicitly ignoring authored source alpha makes image/palette alpha
    // opaque without disabling explicitly generated alpha.
    config = pvt::default_config();
    config.palette.enabled = true;
    config.palette.colors.front().alpha = 0.5;
    CHECK(!pvt::validate(config).ok);
    config.alpha.use_source_alpha = false;
    CHECK(pvt::validate(config).ok);

    config = pvt::default_config();
    config.starting_colors.include_alpha = true;
    config.starting_colors.alpha_minimum = 0.25;
    config.starting_colors.alpha_maximum = 0.75;
    CHECK(!pvt::validate(config).ok);
    config.alpha.use_source_alpha = false;
    CHECK(!pvt::validate(config).ok);
    config.output.write_alpha = true;
    CHECK(pvt::validate(config).ok);

    config = pvt::default_config();
    config.starting_image.enabled = true;
    config.starting_image.path = "source.png";
    CHECK(!pvt::validate(config).ok);
    config.alpha.use_source_alpha = false;
    CHECK(pvt::validate(config).ok);

    // Reusable placement and a static rotation can both expose the canvas and
    // both require the motion work buffer even when path=None.
    config = pvt::default_config();
    config.motion_paths.push_back(
        pvt::default_ellipse_path(500U, 600U, "Validation path"));
    config.motion.enabled = true;
    config.motion.custom_path.enabled = true;
    config.motion.custom_path.path_id = 500U;
    CHECK(!pvt::validate(config).ok);
    config.output.write_alpha = true;
    const auto custom_motion_result = pvt::validate(config);
    CHECK(custom_motion_result.ok);

    config = pvt::default_config();
    config.motion.enabled = true;
    config.motion.rotation_offset_degrees = 45.0;
    CHECK(!pvt::validate(config).ok);
    config.output.write_alpha = true;
    const auto static_rotation_result = pvt::validate(config);
    CHECK(static_rotation_result.ok);

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
    particles.frequency = 1001.0;
    particles.secondary = 1.0;
    particles.radius_pixels = 16384.0;
    CHECK(!pvt::validate(config).ok);
    particles.radius_pixels = 2.0;
    CHECK(pvt::validate(config).ok);
    particles.particle_size_variation = 1.01;
    CHECK(!pvt::validate(config).ok);
    particles.particle_size_variation = 0.25;
    particles.particle_definition = -0.01;
    CHECK(!pvt::validate(config).ok);
    particles.particle_definition = 0.8;
    particles.particle_twinkle = 1.01;
    CHECK(!pvt::validate(config).ok);
    particles.particle_twinkle = 0.5;
    particles.particle_orientation =
        static_cast<pvt::ParticleOrientation>(255);
    CHECK(!pvt::validate(config).ok);

    config = pvt::default_config();
    config.starting_colors.kaleidoscope.mirrored_segments = 1;
    CHECK(!pvt::validate(config).ok);
    config.starting_colors.kaleidoscope.mirrored_segments = 6;
    config.starting_colors.domain_warp.octaves = 9;
    CHECK(pvt::validate(config).ok);
    config.fps = 0.5;
    config.phrase_warp = -5.0;
    config.ghost_lag_degrees = 72000.0;
    config.displacement = 1001.0;
    config.wave_depth = 11.0;
    config.spiral_frequency = 1001.0;
    config.spiral_arms = 101;
    config.wall_frequency = 1001.0;
    config.wall_mix = -6.0;
    config.hue_cycles = 101;
    config.starting_colors.kaleidoscope.mirrored_segments = 257;
    config.starting_colors.kaleidoscope.rotation_degrees = 36001.0;
    config.starting_colors.domain_warp.strength = 2.1;
    config.starting_colors.domain_warp.scale = 65.0;
    config.motion.center_x = 11.0;
    config.motion.travel_x = 11.0;
    config.motion.scale_pulse = 1.0;
    config.alpha.spatial_frequency = 1001.0;
    config.quantization.levels = 65537;
    config.post_process.antialias_passes = 5;
    config.surface.lighting = 11.0;
    config.surface.plane_displacement.minimum = 3.0;
    config.surface.plane_displacement.maximum = 5.0;
    CHECK(pvt::validate(config).ok);
    config.starting_colors.domain_warp.octaves = 3;
    config.layer_clock.mix = static_cast<pvt::LayerClockMixMode>(255U);
    CHECK(!pvt::validate(config).ok);

    for (const pvt::EffectType type : {
             pvt::EffectType::Glitch, pvt::EffectType::Starburst,
             pvt::EffectType::EdgeDetect}) {
        config = pvt::default_config();
        config.effects.clear();
        auto procedural = pvt::default_effect(type);
        procedural.id = 9001U;
        procedural.enabled = true;
        config.effects.push_back(procedural);
        CHECK(pvt::validate(config).ok);
        config.effects.front().frequency += 0.5;
        CHECK(!pvt::validate(config).ok);
    }
    config = pvt::default_config();
    config.effects.clear();
    auto lens = pvt::default_effect(pvt::EffectType::LensDistortion);
    lens.id = 9002U;
    lens.enabled = true;
    config.effects.push_back(lens);
    CHECK(pvt::validate(config).ok);
    config.effects.front().secondary = 0.0;
    CHECK(pvt::validate(config).ok); // Neutral direction is a valid no-op.
    config = pvt::default_config();
    make_small(config);
    config.effects.clear();
    const auto no_lens_result = pvt::validate(config);
    lens.secondary = 0.0;
    config.effects.push_back(lens);
    const auto neutral_lens_result = pvt::validate(config);
    CHECK(neutral_lens_result.ok);
    CHECK(neutral_lens_result.estimated_peak_bytes
          == no_lens_result.estimated_peak_bytes);
    config.effects.front().secondary = 1.0;
    const auto active_lens_result = pvt::validate(config);
    CHECK(active_lens_result.ok);
    CHECK(active_lens_result.estimated_peak_bytes
          > neutral_lens_result.estimated_peak_bytes);

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
    const auto large_frame_validation = pvt::validate(config);
    CHECK(large_frame_validation.ok);
    CHECK(large_frame_validation.estimated_peak_bytes > std::size_t{1} << 30U);

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
    const auto rotated_plane_result = pvt::validate(config);
    CHECK(rotated_plane_result.ok);
    CHECK(rotated_plane_result.estimated_peak_bytes
          > neutral_plane_result.estimated_peak_bytes);
    config.surface.enabled = false;
    config.surface.phase_degrees = 0.0;
    config.effects[1].enabled = true;
    config.effects[1].intensity = 0.0;
    const auto neutral_effect_result = pvt::validate(config);
    CHECK(neutral_effect_result.ok);
    CHECK(neutral_effect_result.estimated_peak_bytes
          == two_buffer_result.estimated_peak_bytes);
    config.effects[1].intensity = 1.0;
    const auto active_effect_result = pvt::validate(config);
    CHECK(active_effect_result.ok);
    CHECK(active_effect_result.estimated_peak_bytes
          > neutral_effect_result.estimated_peak_bytes);
    config.effects[1].intensity = 0.0;
    config.motion.enabled = true;
    config.motion.path = pvt::LayerMotionPath::Orbit;
    config.motion.travel_x = 0.0;
    config.motion.travel_y = 0.0;
    const auto identity_motion_result = pvt::validate(config);
    CHECK(identity_motion_result.ok);
    CHECK(identity_motion_result.estimated_peak_bytes
          == neutral_effect_result.estimated_peak_bytes);
    config.motion.travel_x = 0.15;
    config.motion.travel_y = 0.15;
    const auto active_motion_result = pvt::validate(config);
    CHECK(active_motion_result.ok);
    CHECK(active_motion_result.estimated_peak_bytes
          > neutral_effect_result.estimated_peak_bytes);

    config = pvt::default_config();
    make_small(config);
    const auto no_obj_memory_result = pvt::validate(config);
    CHECK(no_obj_memory_result.ok);
    config.surface.enabled = true;
    config.surface.mapping = pvt::SurfaceMapping::CustomObj;
    config.surface.obj_path = "mesh.obj";
    config.alpha.enabled = true;
    const auto obj_memory_result = pvt::validate(config);
    CHECK(obj_memory_result.ok);
    CHECK(obj_memory_result.estimated_peak_bytes
          > no_obj_memory_result.estimated_peak_bytes);

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
    const auto original_particles = std::find_if(
        original.effects.begin(), original.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(original_particles != original.effects.end());
    if (original_particles != original.effects.end()) {
        original_particles->particle_shape = pvt::ParticleShape::Diamond;
        original_particles->particle_profile =
            pvt::ParticleRenderProfile::Defined;
        original_particles->particle_size_variation = 0.44;
        original_particles->particle_definition = 0.87;
        original_particles->particle_twinkle = 0.23;
        original_particles->particle_seed = UINT64_C(0xfedcba9876543210);
        original_particles->particle_orientation =
            pvt::ParticleOrientation::Random;
        original_particles->particle_rotation_degrees = -37.5;
    }
    original.alpha.enabled = true;
    original.alpha.use_source_alpha = false;
    original.starting_colors.mode = pvt::StartingColorMode::Random;
    original.starting_colors.include_alpha = true;
    original.starting_colors.red_steps = 11;
    original.starting_colors.green_steps = 12;
    original.starting_colors.blue_steps = 13;
    original.starting_colors.alpha_steps = 14;
    original.starting_colors.red_minimum = 0.1;
    original.starting_colors.red_maximum = 0.9;
    original.starting_colors.alpha_minimum = 0.2;
    original.starting_colors.alpha_maximum = 0.8;
    original.starting_colors.kaleidoscope.enabled = true;
    original.starting_colors.kaleidoscope.mirrored_segments = 11;
    original.starting_colors.kaleidoscope.rotation_degrees = -27.5;
    original.starting_colors.kaleidoscope.mix = 0.63;
    original.starting_colors.domain_warp.enabled = true;
    original.starting_colors.domain_warp.strength = 0.37;
    original.starting_colors.domain_warp.scale = 3.25;
    original.starting_colors.domain_warp.octaves = 5;
    original.starting_colors.domain_warp.cycles_per_loop = -4;
    original.starting_colors.domain_warp.seed = UINT64_C(0xfedcba9876543210);
    original.starting_image.palette_dither_enabled = true;
    original.starting_image.palette_dither_method =
        pvt::DitherMethod::OrderedBayer;
    original.quantization.enabled = true;
    original.quantization.mode = pvt::QuantizationMode::Hue;
    original.post_process.invert_rgb_enabled = true;
    original.post_process.invert_rgb_mix = 0.61;
    original.post_process.invert_alpha_enabled = true;
    original.post_process.invert_alpha_mix = 0.37;
    original.post_process.antialias_enabled = true;
    original.post_process.antialias_strength = 0.82;
    original.post_process.antialias_threshold = 0.14;
    original.post_process.antialias_passes = 3;
    original.surface.enabled = true;
    original.surface.mapping = pvt::SurfaceMapping::Cylinder;
    original.surface.obj_path = "mesh folder/test.obj";
    original.surface.plane_displacement.enabled = false;
    original.surface.plane_displacement.minimum = -0.42;
    original.surface.plane_displacement.maximum = 0.73;
    original.surface.plane_displacement.midpoint = 0.37;
    original.surface.plane_displacement.pixels_per_node = 7;
    original.surface.plane_displacement.path =
        "height maps/test height.png";
    original.surface.plane_displacement.sha256 = std::string(64U, 'b');
    original.surface.plane_displacement.basename = "test height.png";
    original.palette = pvt::default_palette(2U);
    original.palette.enabled = false;
    original.palette.columns = 7U;
    original.palette.colors.front().name = "HDR ember";
    original.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    original.palette.colors.front().red = 0.275;
    original.palette.colors.back().name = "Display violet";
    original.palette.colors.front().alpha = 0.37;
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
    original.audio_reactive_override_enabled = false;
    original.audio_reactive_defaults.enabled = true;
    original.audio_reactive_defaults.synchronized_only = true;
    original.audio_reactive_defaults.wave_source = pvt::MusicFeature::Treble;
    original.audio_reactive_defaults.wave_amount = 0.29;
    original.audio_reactive_defaults.effect_source = pvt::MusicFeature::Beat;
    original.audio_reactive_defaults.effect_amount = 0.31;
    original.audio_reactive_defaults.color_enabled = false;
    original.audio_reactive_defaults.color_source = pvt::MusicFeature::ChromaHue;
    original.audio_reactive_defaults.color_amount_degrees = -33.0;
    original.audio_reactive.enabled = true;
    original.audio_reactive.synchronized_only = false;
    original.audio_reactive.wave_source = pvt::MusicFeature::Bass;
    original.audio_reactive.wave_amount = 0.61;
    original.audio_reactive.effect_source = pvt::MusicFeature::Onset;
    original.audio_reactive.effect_amount = 0.72;
    original.audio_reactive.color_enabled = true;
    original.audio_reactive.color_source = pvt::MusicFeature::Midrange;
    original.audio_reactive.color_amount_degrees = 42.0;
    original.waves.front().audio_response = pvt::AudioResponseMode::Beat;
    original.effects.front().audio_response = pvt::AudioResponseMode::Energy;
    original.layer_clock.enabled = true;
    original.layer_clock.scale = pvt::LayerClockScale::OriginalSpeedLoop;
    original.layer_clock.mix = pvt::LayerClockMixMode::SoftXor;
    original.layer_clock.mix_enabled = true;
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
    // Preserve the full mode the hosting filesystem actually accepts. Some
    // sandboxed/network filesystems report chmod success while clearing
    // set-ID bits, so capture the installed mode instead of assuming 04750 is
    // representable there.
    CHECK(::chmod(first.string().c_str(), 04750) == 0);
    struct stat expected_setup_status {};
    CHECK(::stat(first.string().c_str(), &expected_setup_status) == 0);
    const mode_t expected_setup_mode = expected_setup_status.st_mode & 07777;
    CHECK((expected_setup_mode & 0777) == 0750);
    CHECK(pvt::save_setup(original, first.string(), &error));
    struct stat setup_status {};
    CHECK(::stat(first.string().c_str(), &setup_status) == 0);
    CHECK((setup_status.st_mode & 07777) == expected_setup_mode);

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
    CHECK(loaded.surface.plane_displacement.minimum == -0.42);
    CHECK(loaded.surface.plane_displacement.maximum == 0.73);
    CHECK(loaded.surface.plane_displacement.midpoint == 0.37);
    CHECK(loaded.surface.plane_displacement.pixels_per_node == 7);
    CHECK(loaded.surface.plane_displacement.path
          == original.surface.plane_displacement.path);
    CHECK(loaded.surface.plane_displacement.sha256
          == original.surface.plane_displacement.sha256);
    CHECK(loaded.surface.plane_displacement.basename
          == original.surface.plane_displacement.basename);
    CHECK(loaded.swings.back().radius == original.swings.back().radius);
    CHECK(loaded.effects.back().space == pvt::EffectSpace::Surface);
    CHECK(loaded.effects.back().area_radius == original.effects.back().area_radius);
    const auto loaded_particles = std::find_if(
        loaded.effects.begin(), loaded.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(loaded_particles != loaded.effects.end());
    if (loaded_particles != loaded.effects.end()) {
        CHECK(loaded_particles->particle_shape == pvt::ParticleShape::Diamond);
        CHECK(loaded_particles->particle_profile
              == pvt::ParticleRenderProfile::Defined);
        CHECK(loaded_particles->particle_size_variation == 0.44);
        CHECK(loaded_particles->particle_definition == 0.87);
        CHECK(loaded_particles->particle_twinkle == 0.23);
        CHECK(loaded_particles->particle_seed
              == UINT64_C(0xfedcba9876543210));
        CHECK(loaded_particles->particle_orientation
              == pvt::ParticleOrientation::Random);
        CHECK(loaded_particles->particle_rotation_degrees == -37.5);
    }
    CHECK(!loaded.alpha.use_source_alpha);
    CHECK(loaded.starting_colors.mode == pvt::StartingColorMode::Random);
    CHECK(loaded.starting_colors.include_alpha);
    CHECK(loaded.starting_colors.red_steps == 11);
    CHECK(loaded.starting_colors.green_steps == 12);
    CHECK(loaded.starting_colors.blue_steps == 13);
    CHECK(loaded.starting_colors.alpha_steps == 14);
    CHECK(loaded.starting_colors.red_minimum == 0.1);
    CHECK(loaded.starting_colors.red_maximum == 0.9);
    CHECK(loaded.starting_colors.alpha_minimum == 0.2);
    CHECK(loaded.starting_colors.alpha_maximum == 0.8);
    CHECK(loaded.starting_colors.kaleidoscope.enabled);
    CHECK(loaded.starting_colors.kaleidoscope.mirrored_segments == 11);
    CHECK(loaded.starting_colors.kaleidoscope.rotation_degrees == -27.5);
    CHECK(loaded.starting_colors.kaleidoscope.mix == 0.63);
    CHECK(loaded.starting_colors.domain_warp.enabled);
    CHECK(loaded.starting_colors.domain_warp.strength == 0.37);
    CHECK(loaded.starting_colors.domain_warp.scale == 3.25);
    CHECK(loaded.starting_colors.domain_warp.octaves == 5);
    CHECK(loaded.starting_colors.domain_warp.cycles_per_loop == -4);
    CHECK(loaded.starting_colors.domain_warp.seed
          == UINT64_C(0xfedcba9876543210));
    CHECK(loaded.post_process.invert_rgb_enabled);
    CHECK(loaded.post_process.invert_rgb_mix == 0.61);
    CHECK(loaded.post_process.invert_alpha_enabled);
    CHECK(loaded.post_process.invert_alpha_mix == 0.37);
    CHECK(loaded.post_process.antialias_enabled);
    CHECK(loaded.post_process.antialias_strength == 0.82);
    CHECK(loaded.post_process.antialias_threshold == 0.14);
    CHECK(loaded.post_process.antialias_passes == 3);

    // New radial spirals serialize under their own token. The former
    // `subtractive` token migrates to the explicitly named square spiral so
    // projects authored before 1.2.6 retain their rectangular-ring artwork.
    auto spiral_setup = original;
    spiral_setup.starting_colors.mode = pvt::StartingColorMode::SpiralRainbow;
    const fs::path spiral_path = directory / "spiral.pvt";
    CHECK(pvt::save_setup(spiral_setup, spiral_path.string(), &error));
    const auto spiral_bytes = read_bytes(spiral_path);
    const std::string spiral_text(spiral_bytes.begin(), spiral_bytes.end());
    CHECK(spiral_text.find("starting_colors.mode\tspiral\n")
          != std::string::npos);
    auto loaded_spiral = pvt::default_config();
    CHECK(pvt::load_setup(spiral_path.string(), loaded_spiral, &error));
    CHECK(loaded_spiral.starting_colors.mode
          == pvt::StartingColorMode::SpiralRainbow);

    auto square_setup = original;
    square_setup.starting_colors.mode =
        pvt::StartingColorMode::SquareSpiralRainbow;
    const fs::path square_path = directory / "square-spiral.pvt";
    CHECK(pvt::save_setup(square_setup, square_path.string(), &error));
    const auto square_bytes = read_bytes(square_path);
    std::string legacy_square(square_bytes.begin(), square_bytes.end());
    const std::string square_token =
        "starting_colors.mode\tsquare_spiral\n";
    const std::size_t square_position = legacy_square.find(square_token);
    CHECK(square_position != std::string::npos);
    if (square_position != std::string::npos) {
        legacy_square.replace(square_position, square_token.size(),
                              "starting_colors.mode\tsubtractive\n");
    }
    const fs::path legacy_square_path = directory / "legacy-square.pvt";
    {
        std::ofstream output(legacy_square_path, std::ios::binary);
        output.write(legacy_square.data(),
                     static_cast<std::streamsize>(legacy_square.size()));
        CHECK(output.good());
    }
    auto loaded_square = pvt::default_config();
    CHECK(pvt::load_setup(
        legacy_square_path.string(), loaded_square, &error));
    CHECK(loaded_square.starting_colors.mode
          == pvt::StartingColorMode::SquareSpiralRainbow);
    CHECK(loaded.starting_image.palette_dither_enabled);
    CHECK(loaded.starting_image.palette_dither_method
          == pvt::DitherMethod::OrderedBayer);
    CHECK(!loaded.palette.enabled);
    CHECK(loaded.palette.name == original.palette.name);
    CHECK(loaded.palette.columns == original.palette.columns);
    CHECK(loaded.palette.colors.size() == original.palette.colors.size());
    if (loaded.palette.colors.size() == original.palette.colors.size()) {
        for (std::size_t index = 0U; index < loaded.palette.colors.size(); ++index) {
            CHECK(loaded.palette.colors[index].red
                  == original.palette.colors[index].red);
            CHECK(loaded.palette.colors[index].green
                  == original.palette.colors[index].green);
            CHECK(loaded.palette.colors[index].blue
                  == original.palette.colors[index].blue);
            CHECK(loaded.palette.colors[index].alpha
                  == original.palette.colors[index].alpha);
            CHECK(loaded.palette.colors[index].name
                  == original.palette.colors[index].name);
            CHECK(loaded.palette.colors[index].encoding
                  == original.palette.colors[index].encoding);
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
    CHECK(!loaded.audio_reactive_override_enabled);
    CHECK(loaded.audio_reactive_defaults.enabled);
    CHECK(loaded.audio_reactive_defaults.wave_source
          == pvt::MusicFeature::Treble);
    CHECK(loaded.audio_reactive_defaults.effect_amount == 0.31);
    CHECK(!loaded.audio_reactive_defaults.color_enabled);
    CHECK(loaded.audio_reactive.enabled);
    CHECK(loaded.audio_reactive.wave_source == pvt::MusicFeature::Bass);
    CHECK(loaded.audio_reactive.color_amount_degrees == 42.0);
    CHECK(loaded.waves.front().audio_response
          == pvt::AudioResponseMode::Beat);
    CHECK(loaded.effects.front().audio_response
          == pvt::AudioResponseMode::Energy);
    CHECK(loaded.layer_clock.enabled);
    CHECK(loaded.layer_clock.scale == pvt::LayerClockScale::OriginalSpeedLoop);
    CHECK(loaded.layer_clock.mix == pvt::LayerClockMixMode::SoftXor);
    CHECK(loaded.layer_clock.mix_enabled);
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

    pvt::RenderConfig procedural_effect_setup = pvt::default_config();
    procedural_effect_setup.effects.clear();
    for (const pvt::EffectType type : {
             pvt::EffectType::Glitch,
             pvt::EffectType::Starburst,
             pvt::EffectType::LensDistortion,
             pvt::EffectType::EdgeDetect,
             pvt::EffectType::Twirl}) {
        auto effect = pvt::default_effect(type);
        effect.id = pvt::allocate_id(procedural_effect_setup);
        effect.enabled = true;
        procedural_effect_setup.effects.push_back(effect);
    }
    std::string procedural_effect_text;
    CHECK(pvt::detail::serialize_setup_config(
        procedural_effect_setup, procedural_effect_text, &error));
    pvt::RenderConfig loaded_procedural_effects;
    CHECK(pvt::detail::deserialize_setup_config(
        procedural_effect_text, loaded_procedural_effects, &error));
    CHECK(loaded_procedural_effects.effects.size() == 5U);
    if (loaded_procedural_effects.effects.size() == 5U) {
        CHECK(loaded_procedural_effects.effects[0].type
              == pvt::EffectType::Glitch);
        CHECK(loaded_procedural_effects.effects[1].type
              == pvt::EffectType::Starburst);
        CHECK(loaded_procedural_effects.effects[2].type
              == pvt::EffectType::LensDistortion);
        CHECK(loaded_procedural_effects.effects[3].type
              == pvt::EffectType::EdgeDetect);
        CHECK(loaded_procedural_effects.effects[4].type
              == pvt::EffectType::Twirl);
    }

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

    const auto current_version_bytes = read_bytes(first);
    CHECK(std::string(current_version_bytes.begin(),
                      current_version_bytes.end())
              .rfind("PVT_SETUP\t15\n", 0U) == 0U);
    std::string version_fourteen(current_version_bytes.begin(),
                                 current_version_bytes.end());
    version_fourteen.replace(0U, std::string("PVT_SETUP\t15").size(),
                             "PVT_SETUP\t14");
    for (const char* field : {"particle_profile", "particle_size_variation",
                              "particle_definition", "particle_twinkle",
                              "particle_seed", "particle_orientation",
                              "particle_rotation_degrees"}) {
        erase_records_with_fragment(version_fourteen, field);
    }
    pvt::RenderConfig loaded_version_fourteen;
    CHECK(pvt::detail::deserialize_setup_config(
        version_fourteen, loaded_version_fourteen, &error));
    const auto legacy_particles = std::find_if(
        loaded_version_fourteen.effects.begin(),
        loaded_version_fourteen.effects.end(), [](const auto& effect) {
            return effect.type == pvt::EffectType::ParticleField;
        });
    CHECK(legacy_particles != loaded_version_fourteen.effects.end());
    if (legacy_particles != loaded_version_fourteen.effects.end()) {
        CHECK(legacy_particles->particle_profile
              == pvt::ParticleRenderProfile::LegacyGlow);
        CHECK(legacy_particles->particle_size_variation == 0.0);
        CHECK(legacy_particles->particle_twinkle == 1.0);
        CHECK(legacy_particles->particle_seed == 0U);
        CHECK(legacy_particles->particle_orientation
              == pvt::ParticleOrientation::Fixed);
    }

    // Workloads accepted by setup <=14 remain recoverable even when the
    // dimension-aware renderer admission added in setup 15 considers them
    // unsafe. The authored controls survive, only the effect is disabled, and
    // the original enabled value remains in a non-applying compatibility
    // record so reducing the workload cannot silently re-enable the effect.
    if (original_particles != original.effects.end()) {
        const auto replace_legacy_record = [](std::string& setup,
                                              const std::string& key,
                                              const std::string& value) {
            const std::string prefix = key + "\t";
            const std::size_t position = setup.find(prefix);
            CHECK(position != std::string::npos);
            if (position == std::string::npos) return;
            const std::size_t newline = setup.find('\n', position);
            CHECK(newline != std::string::npos);
            if (newline != std::string::npos) {
                setup.replace(position + prefix.size(),
                              newline - position - prefix.size(), value);
            }
        };
        const std::size_t particle_index = static_cast<std::size_t>(
            std::distance(original.effects.begin(), original_particles));
        const std::string prefix = "effects."
                                   + std::to_string(particle_index) + ".";
        std::string unsafe_legacy_particles = version_fourteen;
        replace_legacy_record(unsafe_legacy_particles, prefix + "enabled", "1");
        replace_legacy_record(unsafe_legacy_particles, prefix + "intensity", "1");
        replace_legacy_record(unsafe_legacy_particles, prefix + "frequency", "1001");
        replace_legacy_record(unsafe_legacy_particles, prefix + "secondary", "1");
        replace_legacy_record(unsafe_legacy_particles, prefix + "radius_pixels",
                              "16384");

        pvt::RenderConfig recovered_unsafe_particles;
        CHECK(pvt::detail::deserialize_setup_config(
            unsafe_legacy_particles, recovered_unsafe_particles, &error));
        const auto recovered_particles = std::find_if(
            recovered_unsafe_particles.effects.begin(),
            recovered_unsafe_particles.effects.end(), [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(recovered_particles != recovered_unsafe_particles.effects.end());
        if (recovered_particles != recovered_unsafe_particles.effects.end()) {
            CHECK(!recovered_particles->enabled);
            CHECK(recovered_particles->frequency == 1001.0);
            CHECK(recovered_particles->secondary == 1.0);
            CHECK(recovered_particles->radius_pixels == 16384.0);
        }
        const std::string recovery_key =
            prefix + "recovery_unsafe_particle_enabled";
        const auto preserved_enabled = std::find_if(
            recovered_unsafe_particles.source_compatibility.records.begin(),
            recovered_unsafe_particles.source_compatibility.records.end(),
            [&recovery_key](const pvt::PreservedConfigRecord& record) {
                return record.key == recovery_key
                       && record.value == "1" && !record.rejected;
            });
        CHECK(preserved_enabled
              != recovered_unsafe_particles.source_compatibility.records.end());
        CHECK(!recovered_unsafe_particles.source_compatibility.repair_notes.empty());

        // A malformed v15-only workload field must not make the generic
        // recovery path throw away the entire effects collection. Disable
        // the unsafe particle conservatively, preserve its authored controls,
        // and let field recovery retain the malformed value for inspection.
        std::string malformed_current_particles(
            current_version_bytes.begin(), current_version_bytes.end());
        replace_legacy_record(malformed_current_particles,
                              prefix + "enabled", "1");
        replace_legacy_record(malformed_current_particles,
                              prefix + "intensity", "1");
        replace_legacy_record(malformed_current_particles,
                              prefix + "frequency", "1001");
        replace_legacy_record(malformed_current_particles,
                              prefix + "secondary", "1");
        replace_legacy_record(malformed_current_particles,
                              prefix + "radius_pixels", "16384");
        replace_legacy_record(malformed_current_particles,
                              prefix + "particle_size_variation", "broken");
        pvt::RenderConfig recovered_malformed_particles;
        CHECK(pvt::detail::deserialize_setup_config(
            malformed_current_particles, recovered_malformed_particles,
            &error));
        CHECK(recovered_malformed_particles.effects.size()
              == original.effects.size());
        const auto malformed_particles = std::find_if(
            recovered_malformed_particles.effects.begin(),
            recovered_malformed_particles.effects.end(),
            [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(malformed_particles
              != recovered_malformed_particles.effects.end());
        if (malformed_particles
            != recovered_malformed_particles.effects.end()) {
            CHECK(!malformed_particles->enabled);
            CHECK(malformed_particles->frequency == 1001.0);
            CHECK(malformed_particles->radius_pixels == 16384.0);
        }
        CHECK(std::any_of(
            recovered_malformed_particles.source_compatibility.records.begin(),
            recovered_malformed_particles.source_compatibility.records.end(),
            [&recovery_key](const pvt::PreservedConfigRecord& record) {
                return record.key == recovery_key
                       && record.value == "1" && !record.rejected;
            }));
        CHECK(std::any_of(
            recovered_malformed_particles.source_compatibility.records.begin(),
            recovered_malformed_particles.source_compatibility.records.end(),
            [&prefix](const pvt::PreservedConfigRecord& record) {
                return record.key == prefix + "particle_size_variation"
                       && record.value == "broken" && record.rejected;
            }));

        // An unrelated semantic repair must not make the grouped fallback
        // retry the unsafe authored enabled flag and throw away every effect.
        std::string unsafe_with_bad_canvas = unsafe_legacy_particles;
        replace_legacy_record(unsafe_with_bad_canvas, "canvas.block_size", "0");
        pvt::RenderConfig recovered_unsafe_with_bad_canvas;
        CHECK(pvt::detail::deserialize_setup_config(
            unsafe_with_bad_canvas, recovered_unsafe_with_bad_canvas, &error));
        const auto grouped_particles = std::find_if(
            recovered_unsafe_with_bad_canvas.effects.begin(),
            recovered_unsafe_with_bad_canvas.effects.end(),
            [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(grouped_particles
              != recovered_unsafe_with_bad_canvas.effects.end());
        if (grouped_particles
            != recovered_unsafe_with_bad_canvas.effects.end()) {
            CHECK(!grouped_particles->enabled);
            CHECK(grouped_particles->frequency == 1001.0);
            CHECK(grouped_particles->radius_pixels == 16384.0);
        }

        const fs::path recovered_path =
            directory / "legacy-unsafe-particle-recovery.pvt";
        CHECK(pvt::save_setup(
            recovered_unsafe_particles, recovered_path.string(), &error));
        pvt::RenderConfig recovered_round_trip;
        CHECK(pvt::load_setup(
            recovered_path.string(), recovered_round_trip, &error));
        const auto round_trip_particles = std::find_if(
            recovered_round_trip.effects.begin(),
            recovered_round_trip.effects.end(), [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(round_trip_particles != recovered_round_trip.effects.end());
        if (round_trip_particles != recovered_round_trip.effects.end()) {
            CHECK(!round_trip_particles->enabled);
            CHECK(round_trip_particles->frequency == 1001.0);
            CHECK(round_trip_particles->secondary == 1.0);
            CHECK(round_trip_particles->radius_pixels == 16384.0);
        }
        CHECK(std::any_of(
            recovered_round_trip.source_compatibility.records.begin(),
            recovered_round_trip.source_compatibility.records.end(),
            [&recovery_key](const pvt::PreservedConfigRecord& record) {
                return record.key == recovery_key
                       && record.value == "1" && !record.rejected;
            }));

        // Making the authored workload safe while retaining enabled=false is
        // authoritative across another save/load; recovery metadata never
        // auto-applies the historical true value.
        if (round_trip_particles != recovered_round_trip.effects.end()) {
            round_trip_particles->frequency = 8.0;
            round_trip_particles->secondary = 0.0;
            round_trip_particles->radius_pixels = 2.0;
        }
        const fs::path safe_disabled_path =
            directory / "legacy-safe-disabled-particle-recovery.pvt";
        CHECK(pvt::save_setup(
            recovered_round_trip, safe_disabled_path.string(), &error));
        pvt::RenderConfig safe_disabled_round_trip;
        CHECK(pvt::load_setup(
            safe_disabled_path.string(), safe_disabled_round_trip, &error));
        const auto safe_disabled_particles = std::find_if(
            safe_disabled_round_trip.effects.begin(),
            safe_disabled_round_trip.effects.end(), [](const auto& effect) {
                return effect.type == pvt::EffectType::ParticleField;
            });
        CHECK(safe_disabled_particles != safe_disabled_round_trip.effects.end());
        if (safe_disabled_particles != safe_disabled_round_trip.effects.end()) {
            CHECK(!safe_disabled_particles->enabled);
            CHECK(safe_disabled_particles->frequency == 8.0);
            CHECK(safe_disabled_particles->secondary == 0.0);
            CHECK(safe_disabled_particles->radius_pixels == 2.0);
        }

        // Recovery preflight must never iterate or allocate from an authored
        // count that cannot possibly match the available records. Both the
        // signed-int API limit and the first value above it recover promptly.
        for (const std::size_t hostile_count : {
                 pvt::kMaximumEffects, pvt::kMaximumEffects + 1U}) {
            std::string hostile_collection = version_fourteen;
            replace_legacy_record(
                hostile_collection, "effects.count",
                std::to_string(hostile_count));
            pvt::RenderConfig recovered_collection;
            CHECK(pvt::detail::deserialize_setup_config(
                hostile_collection, recovered_collection, &error));
            CHECK(recovered_collection.effects.size()
                  == pvt::default_config().effects.size());
            CHECK(std::any_of(
                recovered_collection.source_compatibility.records.begin(),
                recovered_collection.source_compatibility.records.end(),
                [hostile_count](const pvt::PreservedConfigRecord& record) {
                    return record.key == "effects.count"
                           && record.value == std::to_string(hostile_count)
                           && record.rejected;
                }));
        }
    }

    std::string version_thirteen(version_fourteen.begin(),
                                 version_fourteen.end());
    version_thirteen.replace(0U, std::string("PVT_SETUP\t14").size(),
                             "PVT_SETUP\t13");
    erase_records_with_fragment(version_thirteen, "audio_input.");
    erase_records_with_fragment(version_thirteen, "input_processing.");
    erase_records_with_fragment(version_thirteen, "frequency_streams.");
    erase_records_with_fragment(version_thirteen, "frequency_stream_uuid");
    erase_records_with_fragment(version_thirteen, "particle_shape");
    erase_record(version_thirteen, "live.safety.prevent_device_sleep");
    pvt::RenderConfig loaded_version_thirteen;
    CHECK(pvt::detail::deserialize_setup_config(
        version_thirteen, loaded_version_thirteen, &error));

    std::string version_twelve(version_thirteen.begin(),
                               version_thirteen.end());
    version_twelve.replace(0U, std::string("PVT_SETUP\t13").size(),
                           "PVT_SETUP\t12");
    erase_records_with_prefix(version_twelve,
                              "surface.plane_displacement.");
    pvt::RenderConfig loaded_version_twelve;
    CHECK(pvt::detail::deserialize_setup_config(
        version_twelve, loaded_version_twelve, &error));
    CHECK(!loaded_version_twelve.surface.plane_displacement.enabled);
    CHECK(loaded_version_twelve.surface.plane_displacement.path.empty());
    CHECK(loaded_version_twelve.surface.plane_displacement.minimum == -0.2);
    CHECK(loaded_version_twelve.surface.plane_displacement.maximum == 0.2);
    CHECK(loaded_version_twelve.surface.plane_displacement.midpoint == 0.5);
    CHECK(loaded_version_twelve.surface.plane_displacement.pixels_per_node == 4);

    std::string version_eleven(version_twelve.begin(),
                               version_twelve.end());
    version_eleven.replace(0U, std::string("PVT_SETUP\t12").size(),
                           "PVT_SETUP\t11");
    erase_records_with_prefix(version_eleven, "post_process.");
    erase_records_with_prefix(version_eleven, "live.");
    pvt::RenderConfig loaded_version_eleven;
    CHECK(pvt::detail::deserialize_setup_config(
        version_eleven, loaded_version_eleven, &error));
    CHECK(!loaded_version_eleven.post_process.invert_rgb_enabled);
    CHECK(!loaded_version_eleven.post_process.invert_alpha_enabled);
    CHECK(!loaded_version_eleven.post_process.antialias_enabled);
    CHECK(loaded_version_eleven.post_process.antialias_passes == 1);
    CHECK(!loaded_version_eleven.live.enabled);
    CHECK(loaded_version_eleven.live.endpoints.empty());
    CHECK(loaded_version_eleven.live.mappings.empty());

    std::string version_ten(version_eleven.begin(), version_eleven.end());
    version_ten.replace(0U, std::string("PVT_SETUP\t11").size(),
                        "PVT_SETUP\t10");
    erase_record(version_ten, "layer_clock.mix");
    erase_record(version_ten, "layer_clock.mix_enabled");
    erase_records_with_prefix(version_ten,
                              "starting_colors.kaleidoscope.");
    erase_records_with_prefix(version_ten,
                              "starting_colors.domain_warp.");
    erase_record(version_ten, "palette.columns");
    for (std::size_t index = 0U; index < original.palette.colors.size(); ++index) {
        erase_record(version_ten, "palette.colors." + std::to_string(index)
                                      + ".name");
        erase_record(version_ten, "palette.colors." + std::to_string(index)
                                      + ".encoding");
    }
    pvt::RenderConfig loaded_version_ten;
    CHECK(pvt::detail::deserialize_setup_config(
        version_ten, loaded_version_ten, &error));
    CHECK(loaded_version_ten.layer_clock.mix
          == pvt::LayerClockMixMode::Replace);
    CHECK(!loaded_version_ten.layer_clock.mix_enabled);
    CHECK(!loaded_version_ten.starting_colors.kaleidoscope.enabled);
    CHECK(!loaded_version_ten.starting_colors.domain_warp.enabled);
    CHECK(loaded_version_ten.palette.columns == 0U);
    for (const auto& color : loaded_version_ten.palette.colors) {
        CHECK(color.name.empty());
        CHECK(color.encoding == pvt::PaletteColorEncoding::Srgb);
    }
    const std::vector<char> version_ten_bytes(version_ten.begin(),
                                               version_ten.end());
    std::string version_nine(version_ten_bytes.begin(),
                             version_ten_bytes.end());
    version_nine.replace(0U, std::string("PVT_SETUP\t10").size(),
                         "PVT_SETUP\t9");
    erase_records_with_prefix(version_nine, "starting_colors.");
    erase_record(version_nine, "alpha.use_source_alpha");
    erase_record(version_nine, "source_image.palette_dither_enabled");
    erase_record(version_nine, "source_image.palette_dither_method");
    erase_records_with_fragment(version_nine, ".blur_");
    for (std::size_t index = 0U; index < original.palette.colors.size(); ++index) {
        erase_record(version_nine, "palette.colors." + std::to_string(index)
                                      + ".alpha");
    }
    CHECK(version_nine.rfind("PVT_SETUP\t9\n", 0U) == 0U);
    const std::vector<char> version_nine_bytes(version_nine.begin(),
                                                version_nine.end());

    // Format 8's three-state values keep their original meanings when loaded
    // into the richer format-9 source selector.
    std::string version_eight(version_nine_bytes.begin(),
                              version_nine_bytes.end());
    version_eight.replace(0U, std::string("PVT_SETUP\t9").size(),
                          "PVT_SETUP\t8");
    const auto replace_record_value = [](std::string& setup,
                                         const std::string& key,
                                         const std::string& value) {
        const std::string prefix = key + "\t";
        const std::size_t position = setup.find(prefix);
        CHECK(position != std::string::npos);
        if (position == std::string::npos) return;
        const std::size_t newline = setup.find('\n', position);
        CHECK(newline != std::string::npos);
        if (newline != std::string::npos) {
            setup.replace(position + prefix.size(),
                          newline - position - prefix.size(), value);
        }
    };
    replace_record_value(version_eight, "waves.0.audio_response", "enabled");
    replace_record_value(version_eight, "effects.0.audio_response", "disabled");
    pvt::RenderConfig loaded_version_eight;
    CHECK(pvt::detail::deserialize_setup_config(
        version_eight, loaded_version_eight, &error));
    CHECK(loaded_version_eight.waves.front().audio_response
          == pvt::AudioResponseMode::Enabled);
    CHECK(loaded_version_eight.effects.front().audio_response
          == pvt::AudioResponseMode::Disabled);

    // Nullable/missing routing fields are neutral authored defaults, not
    // parse errors or accidental opt-ins.
    std::string nullable_routing(version_nine_bytes.begin(),
                                 version_nine_bytes.end());
    const auto replace_record_with_null = [](std::string& setup,
                                             const std::string& key) {
        const std::string prefix = key + "\t";
        const std::size_t position = setup.find(prefix);
        CHECK(position != std::string::npos);
        if (position == std::string::npos) return;
        const std::size_t newline = setup.find('\n', position);
        CHECK(newline != std::string::npos);
        if (newline != std::string::npos) {
            setup.replace(position + prefix.size(),
                          newline - position - prefix.size(), "null");
        }
    };
    replace_record_with_null(nullable_routing,
                             "audio_response_defaults.enabled");
    replace_record_with_null(nullable_routing,
                             "audio_reactive.override_enabled");
    replace_record_with_null(nullable_routing, "waves.0.audio_response");
    replace_record_with_null(nullable_routing, "effects.0.audio_response");
    pvt::RenderConfig loaded_nullable;
    CHECK(pvt::detail::deserialize_setup_config(
        nullable_routing, loaded_nullable, &error));
    CHECK(!loaded_nullable.audio_reactive_defaults.enabled);
    CHECK(!loaded_nullable.audio_reactive_override_enabled);
    CHECK(loaded_nullable.waves.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(loaded_nullable.effects.front().audio_response
          == pvt::AudioResponseMode::Default);

    std::string missing_routing(version_nine_bytes.begin(),
                                version_nine_bytes.end());
    erase_records_with_prefix(missing_routing,
                              "audio_response_defaults.");
    erase_record(missing_routing, "audio_reactive.override_enabled");
    erase_records_with_fragment(missing_routing, ".audio_response");
    pvt::RenderConfig loaded_missing;
    CHECK(pvt::detail::deserialize_setup_config(
        missing_routing, loaded_missing, &error));
    CHECK(!loaded_missing.audio_reactive_defaults.enabled);
    CHECK(!loaded_missing.audio_reactive_override_enabled);
    CHECK(loaded_missing.waves.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(loaded_missing.effects.front().audio_response
          == pvt::AudioResponseMode::Default);

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
    dependent.output.write_alpha = true;
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

    std::string version_seven = missing_routing;
    CHECK(version_seven.rfind("PVT_SETUP\t9\n", 0U) == 0U);
    version_seven.replace(0U, std::string("PVT_SETUP\t9").size(),
                          "PVT_SETUP\t7");
    pvt::RenderConfig loaded_version_seven;
    CHECK(pvt::detail::deserialize_setup_config(
        version_seven, loaded_version_seven, &error));
    CHECK(loaded_version_seven.audio_reactive_override_enabled);
    CHECK(!loaded_version_seven.audio_reactive_defaults.enabled);
    CHECK(loaded_version_seven.waves.front().audio_response
          == pvt::AudioResponseMode::Default);
    CHECK(loaded_version_seven.effects.front().audio_response
          == pvt::AudioResponseMode::Default);

    std::string version_six = version_seven;
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

    std::string oversized_analysis(version_nine_bytes.begin(),
                                   version_nine_bytes.end());
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
    constexpr std::size_t dense_sample_count = 16384U;
    pvt::RenderConfig original = pvt::default_config();
    make_small(original);
    const pvt::MusicFeatureSample sample{
        0.125F, 0.25F, 0.375F, 0.5F, 0.625F, 0.75F,
    };
    original.clock.music.feature_samples.assign(
        dense_sample_count, sample);

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
          == dense_sample_count);
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
    constexpr std::size_t cancellation_wave_count = 512U;
    heavy.waves.reserve(cancellation_wave_count);
    for (std::size_t index = heavy.waves.size();
         index < cancellation_wave_count; ++index) {
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
    test_live_control_model_and_setup_codec();
    test_synchronized_clocks_and_music();
    test_layer_clock_mixing_and_generated_shaping();
    test_image_access_and_transactional_render();
    test_cancellable_single_layer_render();
    test_post_process_effects(test_directory);
    test_plane_displacement_mesh(test_directory);
    test_starting_images_and_reusable_paths(test_directory);
    test_determinism_and_seam_continuity();
    test_new_procedural_effects();
    test_direction_alpha_and_surfaces(source_root);
    test_configurable_blur_effects();
    test_partial_alpha_glow_composition();
    test_particle_straight_alpha_emission();
    test_defined_particle_controls_and_silhouettes();
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
