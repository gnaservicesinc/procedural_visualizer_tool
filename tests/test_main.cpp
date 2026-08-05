#include "procedural_visualizer_tool.h"
#include "../src/path_utf8.h"

#include <png.h>

#include <algorithm>
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

void test_defaults_and_dynamic_collections() {
    auto config = pvt::default_config();
    make_small(config);
    CHECK(pvt::validate(config).ok);
    CHECK(config.waves.size() == 3);
    CHECK(config.effects.size() >= 5);
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
        auto effect = pvt::default_effect(static_cast<pvt::EffectType>(i % 5U));
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
         raw_type <= static_cast<int>(pvt::EffectType::Glow); ++raw_type) {
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

void test_direction_alpha_and_surfaces() {
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

void test_validation_limits() {
    auto config = pvt::default_config();
    CHECK(pvt::validate(config).ok);
    config.waves.resize(pvt::kMaximumWaves + 1U);
    CHECK(!pvt::validate(config).ok);
    config = pvt::default_config();
    config.output.bit_depth = 12;
    CHECK(!pvt::validate(config).ok);
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
    original.effects.push_back(pvt::default_effect(pvt::EffectType::Shake));
    original.effects.back().id = pvt::allocate_id(original);
    original.effects.back().enabled = true;
    original.alpha.enabled = true;
    original.quantization.enabled = true;
    original.quantization.mode = pvt::QuantizationMode::Hue;
    original.surface.enabled = true;
    original.surface.mapping = pvt::SurfaceMapping::Cylinder;
    original.output.bit_depth = 16;
    original.output.dither_method = pvt::DitherMethod::FloydSteinberg;
    original.output.output_directory = "output folder/%safe";

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
    CHECK(pvt::save_setup(loaded, second.string(), &error));
    CHECK(read_bytes(first) == read_bytes(second));

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

    original.output.bit_depth = 32;
    original.output.dither_enabled = true; // Saving must normalize this to off.
    const fs::path float_setup = directory / "float.pvt";
    const fs::path float_round_trip = directory / "float-roundtrip.pvt";
    CHECK(pvt::save_setup(original, float_setup.string(), &error));
    CHECK(pvt::load_setup(float_setup.string(), loaded, &error));
    CHECK(!loaded.output.dither_enabled);
    CHECK(pvt::save_setup(loaded, float_round_trip.string(), &error));
    CHECK(read_bytes(float_setup) == read_bytes(float_round_trip));

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
    int progress_calls = 0;
    CHECK(pvt::render_sequence(
        config,
        [&progress_calls](int completed, int total) {
            CHECK(completed >= 1 && completed <= total);
            ++progress_calls;
            return true;
        },
        nullptr, &error));
    CHECK(progress_calls == 3);
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

    config.output.output_directory = (directory / "callback-throw").string();
    CHECK(!pvt::render_sequence(
        config,
        [](int, int) -> bool { throw std::runtime_error("test callback failure"); },
        nullptr, &error));
    CHECK(error.find("callback failed") != std::string::npos);

    config.output.output_directory = (directory / "atomic-cancel").string();
    std::atomic_bool cancelled {true};
    CHECK(!pvt::render_sequence(config, {}, &cancelled, &error));
    CHECK(!fs::exists(directory / "atomic-cancel" / "loop_0000.png"));
}

} // namespace

int main() {
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
    test_image_access_and_transactional_render();
    test_determinism_and_seam_continuity();
    test_direction_alpha_and_surfaces();
    test_partial_alpha_glow_composition();
    test_validation_limits();
    test_setup_round_trip_and_transaction(test_directory);
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
