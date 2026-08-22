#include "procedural_visualizer_tool.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__     \
                      << ": " #condition "\n";                                \
            ++failures;                                                        \
        }                                                                       \
    } while (false)

struct Difference {
    double maximum_rgb = 0.0;
    double mean_rgb = 0.0;
    double maximum_alpha = 0.0;
    double mean_alpha = 0.0;
};

Difference difference(const pvt::Image& first, const pvt::Image& second) {
    Difference result;
    if (first.width != second.width || first.height != second.height
        || first.pixels.size() != second.pixels.size()
        || first.pixels.empty()) {
        result.maximum_rgb = result.maximum_alpha = 1.0e30;
        result.mean_rgb = result.mean_alpha = 1.0e30;
        return result;
    }
    std::size_t rgb_count = 0U;
    std::size_t alpha_count = 0U;
    for (std::size_t offset = 0U; offset < first.pixels.size(); offset += 4U) {
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            const double value = std::fabs(
                static_cast<double>(first.pixels[offset + channel])
                - static_cast<double>(second.pixels[offset + channel]));
            result.maximum_rgb = std::max(result.maximum_rgb, value);
            result.mean_rgb += value;
            ++rgb_count;
        }
        const double alpha = std::fabs(
            static_cast<double>(first.pixels[offset + 3U])
            - static_cast<double>(second.pixels[offset + 3U]));
        result.maximum_alpha = std::max(result.maximum_alpha, alpha);
        result.mean_alpha += alpha;
        ++alpha_count;
    }
    result.mean_rgb /= static_cast<double>(rgb_count);
    result.mean_alpha /= static_cast<double>(alpha_count);
    return result;
}

void check_close(const pvt::Image& cpu, const pvt::Image& gpu,
                 double maximum_rgb, double mean_rgb,
                 double maximum_alpha, double mean_alpha,
                 const char* label) {
    const Difference value = difference(cpu, gpu);
    if (value.maximum_rgb > maximum_rgb || value.mean_rgb > mean_rgb
        || value.maximum_alpha > maximum_alpha
        || value.mean_alpha > mean_alpha) {
        std::cerr << label << " parity: max RGB " << value.maximum_rgb
                  << ", mean RGB " << value.mean_rgb
                  << ", max alpha " << value.maximum_alpha
                  << ", mean alpha " << value.mean_alpha << '\n';
        ++failures;
    }
}

pvt::RenderConfig parity_config() {
    pvt::RenderConfig config = pvt::default_config();
    config.width = 96;
    config.height = 64;
    config.block_size = 4;
    config.total_frames = 17;
    config.output.write_alpha = true;
    config.alpha.enabled = true;
    config.alpha.minimum = 0.15;
    config.alpha.maximum = 0.92;
    config.alpha.spatial_frequency = 2.75;
    config.alpha.cycles_per_loop = 3;
    config.alpha.phase_degrees = 17.0;
    config.palette = pvt::default_palette(2U);
    config.palette.enabled = true;
    config.palette.columns = 4U;
    config.palette.colors.front().name = "Linear HDR parity";
    config.palette.colors.front().encoding =
        pvt::PaletteColorEncoding::Linear;
    config.palette.colors.front().red = 1.35;
    config.swings.push_back(pvt::default_swing(1U));
    config.swings.back().id = pvt::allocate_id(config);
    config.swings.back().radius = 0.42;
    config.swings.back().center_x = 0.31;
    config.swings.back().center_y = 0.67;
    config.transform.flip_horizontal = true;
    config.quantization.enabled = true;
    config.quantization.mode = pvt::QuantizationMode::Luminance;
    config.quantization.levels = 13;
    config.quantization.mix = 0.45;
    return config;
}

void test_backend_contract() {
    const pvt::RendererCapabilities capabilities =
        pvt::renderer_capabilities();
    CHECK(!capabilities.metal_status.empty());
    if (capabilities.metal_available) {
        CHECK(capabilities.metal_compiled);
        CHECK(!capabilities.metal_device_name.empty());
    }

    pvt::RenderConfig config = parity_config();
    pvt::FrameRenderOptions cpu_options;
    cpu_options.backend = pvt::RenderBackend::Cpu;
    pvt::FrameRenderOptions hybrid_options;
    hybrid_options.backend = pvt::RenderBackend::CpuAndGpu;
    pvt::FrameRenderOptions gpu_options;
    gpu_options.backend = pvt::RenderBackend::Gpu;
    gpu_options.maximum_gpu_frames_in_flight = 1U;
    std::string error;
    pvt::Image cpu;
    CHECK(pvt::render_frame(config, 6, cpu_options, cpu, nullptr, &error));
    if (!capabilities.metal_available) {
        pvt::Image hybrid;
        CHECK(pvt::render_frame(config, 6, hybrid_options, hybrid, nullptr,
                                &error));
        CHECK(cpu.pixels == hybrid.pixels);
        pvt::Image unchanged = cpu;
        CHECK(!pvt::render_frame(config, 6, gpu_options, unchanged, nullptr,
                                 &error));
        CHECK(unchanged.pixels == cpu.pixels);
        return;
    }

    pvt::Image gpu;
    CHECK(pvt::render_frame(config, 6, gpu_options, gpu, nullptr, &error));
    check_close(cpu, gpu, 0.12, 0.012, 0.002, 0.0002,
                "base/palette/alpha/transform/quantization");

    pvt::RenderConfig blur = parity_config();
    blur.width = 79;
    blur.height = 53;
    blur.quantization.enabled = false;
    blur.transform = {};
    blur.effects.clear();
    auto blur_effect = pvt::default_effect(pvt::EffectType::Blur);
    blur_effect.id = 1001U;
    blur_effect.enabled = true;
    blur_effect.synchronized = true;
    blur_effect.cycles_per_loop = 2;
    blur_effect.phase_degrees = 13.0;
    blur_effect.blur_minimum = 0.19;
    blur_effect.blur_maximum = 0.81;
    blur_effect.blur_pulses_per_cycle = 3;
    blur_effect.blur_passes = 2;
    blur_effect.blur_samples = 7;
    blur_effect.radius_pixels = 5.5;
    blur_effect.angle_degrees = 31.0;
    blur_effect.center_x = 0.41;
    blur_effect.center_y = 0.63;
    blur_effect.area_radius = 0.72;
    blur_effect.edge_mode = pvt::EdgeMode::Alpha;
    blur.effects.push_back(blur_effect);
    for (const pvt::BlurType type : {
             pvt::BlurType::Gaussian, pvt::BlurType::Box,
             pvt::BlurType::Directional, pvt::BlurType::Radial,
             pvt::BlurType::Zoom}) {
        blur.effects.front().blur_type = type;
        CHECK(pvt::render_frame_at_phase(blur, 0.37, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(blur, 0.37, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("configurable ")
                                  + pvt::blur_type_name(type) + " blur";
        check_close(cpu, gpu, 0.004, 0.00012, 0.004, 0.00012,
                    label.c_str());
    }
    blur.effects.front().synchronized = false;
    blur.effects.front().blur_type = pvt::BlurType::Gaussian;
    CHECK(pvt::render_frame_at_phase(blur, 0.37, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(blur, 0.37, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.004, 0.00012, 0.004, 0.00012,
                "independent-clock modulated blur");

    // Generated colors use an output-scaled float32 lattice on both backends.
    // They must no longer make strict Metal fail, including authored ranges
    // and explicit generated alpha with palette/PNG source alpha disabled.
    pvt::RenderConfig generated = parity_config();
    generated.width = 97;
    generated.height = 61;
    generated.block_size = 1;
    generated.waves.clear();
    generated.swings.clear();
    generated.effects.clear();
    generated.palette.enabled = false;
    generated.displacement_enabled = false;
    generated.lighting_enabled = false;
    generated.spiral_enabled = false;
    generated.wall_reflection_enabled = false;
    generated.transform = {};
    generated.quantization.enabled = false;
    generated.alpha.enabled = false;
    generated.alpha.use_source_alpha = false;
    generated.starting_colors.include_alpha = true;
    generated.starting_colors.red_minimum = 0.13;
    generated.starting_colors.red_maximum = 0.91;
    generated.starting_colors.green_minimum = 0.07;
    generated.starting_colors.green_maximum = 0.83;
    generated.starting_colors.blue_minimum = 0.19;
    generated.starting_colors.blue_maximum = 0.97;
    generated.starting_colors.alpha_minimum = 0.21;
    generated.starting_colors.alpha_maximum = 0.88;
    for (const pvt::StartingColorMode mode : {
             pvt::StartingColorMode::ContinuousHue,
             pvt::StartingColorMode::HorizontalRainbow,
             pvt::StartingColorMode::VerticalRainbow,
             pvt::StartingColorMode::DiagonalRainbow,
             pvt::StartingColorMode::SpiralRainbow,
             pvt::StartingColorMode::SquareSpiralRainbow,
             pvt::StartingColorMode::Random}) {
        generated.starting_colors.mode = mode;
        CHECK(pvt::render_frame_at_phase(generated, 0.31, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(generated, 0.31, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("generated source ")
                                  + pvt::starting_color_mode_name(mode);
        check_close(cpu, gpu, 0.00002, 0.000001,
                    0.00002, 0.000001, label.c_str());
    }

    // Starting PNGs are decoded once on the host, then fitted and processed
    // by Metal. Every fit mode must retain the reference renderer's linear
    // RGBA sampling, including contain transparency and tiled edges.
    pvt::RenderConfig source_image = parity_config();
    source_image.width = 73;
    source_image.height = 51;
    source_image.block_size = 1;
    source_image.starting_image.enabled = true;
    source_image.starting_image.path =
        PVT_TEST_SOURCE_DIR "/icon/icon.png";
    source_image.waves.clear();
    source_image.swings.clear();
    source_image.effects.clear();
    source_image.displacement_enabled = false;
    source_image.lighting_enabled = false;
    source_image.spiral_enabled = false;
    source_image.wall_reflection_enabled = false;
    source_image.transform = {};
    source_image.quantization.enabled = false;
    source_image.palette.enabled = false;
    for (const pvt::StartingImageFit fit : {
             pvt::StartingImageFit::Stretch,
             pvt::StartingImageFit::Contain,
             pvt::StartingImageFit::Cover,
             pvt::StartingImageFit::Tile}) {
        source_image.starting_image.fit = fit;
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("starting image ")
                                  + pvt::starting_image_fit_name(fit);
        check_close(cpu, gpu, 0.003, 0.00008, 0.003, 0.00008,
                    label.c_str());
    }

    // Image placement, source-alpha handling, and starting palettes are
    // composable without rejecting strict Metal. Parallel dithers execute in
    // Metal; Floyd-Steinberg's ordered dependency is prepared on CPU while all
    // remaining full-frame stages stay GPU accelerated.
    source_image.palette = pvt::default_palette(1U);
    source_image.palette.enabled = true;
    source_image.alpha.use_source_alpha = true;
    for (const pvt::DitherMethod method : {
             pvt::DitherMethod::BlueNoise,
             pvt::DitherMethod::OrderedBayer,
             pvt::DitherMethod::FloydSteinberg}) {
        source_image.starting_image.palette_dither_enabled = true;
        source_image.starting_image.palette_dither_method = method;
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("starting image palette ")
                                  + pvt::dither_method_name(method);
        check_close(cpu, gpu, 0.003, 0.00008, 0.003, 0.00008,
                    label.c_str());
    }
    source_image.alpha.use_source_alpha = false;
    source_image.starting_image.palette_dither_enabled = false;
    CHECK(pvt::render_frame_at_phase(source_image, 0.31, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(source_image, 0.31, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.003, 0.00008, 0.003, 0.00008,
                "starting image ignored source alpha");
    source_image.palette.enabled = false;
    source_image.starting_image.palette_dither_enabled = false;

    // Built-in placement, rotation, and scale are a downstream image stage.
    // Exercise them with a starting source so GPU rendering covers the
    // same combination used by artist projects instead of only procedural art.
    source_image.starting_image.fit = pvt::StartingImageFit::Cover;
    source_image.motion.enabled = true;
    source_image.motion.center_x = 0.37;
    source_image.motion.center_y = 0.61;
    source_image.motion.travel_x = 0.22;
    source_image.motion.travel_y = 0.17;
    source_image.motion.cycles_x = 3;
    source_image.motion.cycles_y = 5;
    source_image.motion.phase_degrees = 19.0;
    source_image.motion.rotations_per_loop = 2;
    source_image.motion.rotation_offset_degrees = 11.0;
    source_image.motion.scale_pulse = 0.35;
    for (const pvt::LayerMotionPath path : {
             pvt::LayerMotionPath::None,
             pvt::LayerMotionPath::Orbit,
             pvt::LayerMotionPath::FigureEight,
             pvt::LayerMotionPath::Bounce,
             pvt::LayerMotionPath::Lissajous}) {
        source_image.motion.path = path;
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(source_image, 0.31, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("starting image and motion ")
                                  + pvt::layer_motion_path_name(path);
        check_close(cpu, gpu, 0.035, 0.0015, 0.02, 0.0008,
                    label.c_str());
    }

    pvt::Image source_sentinel = cpu;
    source_image.starting_image.path =
        PVT_TEST_SOURCE_DIR "/icon/missing-starting-image.png";
    CHECK(!pvt::render_frame_at_phase(source_image, 0.31, gpu_options,
                                      source_sentinel, nullptr, &error));
    CHECK(source_sentinel.pixels == cpu.pixels);

    // A held shared clock must not leak into free wave timing on either
    // backend. This exercises the independent phase carried in Metal's frame
    // constants rather than only testing direct-phase renders.
    pvt::RenderConfig free_clock = parity_config();
    free_clock.clock.mode = pvt::ClockMode::Frame;
    free_clock.clock.frame_interval = 3;
    free_clock.clock.interpolation = pvt::ClockInterpolation::Hold;
    free_clock.quantization.enabled = false;
    free_clock.transform = {};
    free_clock.effects.clear();
    for (auto& wave : free_clock.waves) wave.synchronized = false;
    CHECK(pvt::render_frame(free_clock, 1, cpu_options, cpu, nullptr, &error));
    CHECK(pvt::render_frame(free_clock, 1, gpu_options, gpu, nullptr, &error));
    check_close(cpu, gpu, 0.12, 0.012, 0.002, 0.0002,
                "independent wave clock under shared hold");

    // Reusable-path tangent following is resolved on the host but must remain
    // the propagation axis after the prepared frame crosses into Metal.
    pvt::RenderConfig path_wave = parity_config();
    path_wave.quantization.enabled = false;
    path_wave.transform = {};
    path_wave.effects.clear();
    path_wave.swings.clear();
    path_wave.motion_paths.push_back(
        pvt::default_ellipse_path(500U, 600U, "Metal tangent path"));
    path_wave.waves.resize(1U);
    path_wave.waves.front().synchronized = false;
    path_wave.waves.front().direction = 0.5;
    path_wave.waves.front().path.enabled = true;
    path_wave.waves.front().path.path_id = 500U;
    path_wave.waves.front().path.phase_degrees = 70.0;
    path_wave.waves.front().path.follow_tangent = true;
    CHECK(pvt::render_frame_at_phase(path_wave, 0.31, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(path_wave, 0.31, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.12, 0.012, 0.002, 0.0002,
                "reusable wave path tangent");
    path_wave.waves.front().path.reverse = true;
    CHECK(pvt::render_frame_at_phase(path_wave, 0.31, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(path_wave, 0.31, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.12, 0.012, 0.002, 0.0002,
                "reversed reusable wave path tangent");

    pvt::RenderConfig shaped_source = parity_config();
    shaped_source.palette = {};
    shaped_source.effects.clear();
    shaped_source.quantization.enabled = false;
    shaped_source.transform = {};
    shaped_source.starting_colors.kaleidoscope.enabled = true;
    shaped_source.starting_colors.kaleidoscope.mirrored_segments = 9;
    shaped_source.starting_colors.kaleidoscope.rotation_degrees = 23.0;
    shaped_source.starting_colors.kaleidoscope.mix = 0.72;
    shaped_source.starting_colors.domain_warp.enabled = true;
    shaped_source.starting_colors.domain_warp.strength = 0.24;
    shaped_source.starting_colors.domain_warp.scale = 2.6;
    shaped_source.starting_colors.domain_warp.octaves = 4;
    shaped_source.starting_colors.domain_warp.cycles_per_loop = -3;
    shaped_source.starting_colors.domain_warp.seed =
        UINT64_C(0x123456789abcdef0);
    CHECK(pvt::render_frame_at_phase(shaped_source, 0.37, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(shaped_source, 0.37, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.20, 0.018, 0.004, 0.0004,
                "generated kaleidoscope/domain warp");

    const std::vector<pvt::EffectType> effect_types = {
        pvt::EffectType::EndlessZoom,
        pvt::EffectType::Ripple,
        pvt::EffectType::Shake,
        pvt::EffectType::FlagWave,
        pvt::EffectType::Glow,
        pvt::EffectType::BlockScale,
        pvt::EffectType::ParticleField,
        pvt::EffectType::Glitch,
        pvt::EffectType::Starburst,
        pvt::EffectType::LensDistortion,
        pvt::EffectType::EdgeDetect,
        pvt::EffectType::Twirl};
    for (const pvt::EffectType type : effect_types) {
        pvt::RenderConfig single_effect = parity_config();
        single_effect.effects.clear();
        single_effect.quantization.enabled = false;
        single_effect.transform = {};
        pvt::EffectConfig effect = pvt::default_effect(type);
        effect.id = pvt::allocate_id(single_effect);
        effect.enabled = true;
        effect.intensity = type == pvt::EffectType::EndlessZoom
                               ? 2.0
                               : (type == pvt::EffectType::Glow ? 0.8 : 0.65);
        effect.area_radius = 0.58;
        if (type == pvt::EffectType::Glow) {
            effect.radius_pixels = 5.0;
            effect.threshold = 0.2;
            effect.soft_knee = 0.4;
        } else if (type == pvt::EffectType::BlockScale) {
            effect.magnitude = 0.7;
            effect.frequency = 2.2;
            effect.secondary = 5.0;
        } else if (type == pvt::EffectType::ParticleField) {
            effect.particle_shape = pvt::ParticleShape::Star;
            effect.intensity = 1.1;
            effect.magnitude = 0.18;
            effect.frequency = 24.0;
            effect.secondary = 0.4;
            effect.radius_pixels = 3.0;
            effect.threshold = 0.55;
            effect.soft_knee = 0.5;
        } else {
            effect.edge_mode = pvt::EdgeMode::Alpha;
        }
        // Exercise both effect stages across the set, not only texture-space
        // kernels. Their compute code is shared but buffer ordering is not.
        if (type == pvt::EffectType::FlagWave
            || type == pvt::EffectType::BlockScale) {
            effect.space = pvt::EffectSpace::Surface;
        }
        single_effect.effects.push_back(effect);
        CHECK(pvt::render_frame_at_phase(single_effect, 0.37, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(single_effect, 0.37, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("single effect ")
                                  + pvt::effect_type_name(type);
        check_close(cpu, gpu, 0.20, 0.018, 0.035, 0.002,
                    label.c_str());
    }

    // Particle silhouettes share the tiled particle kernel but take distinct
    // distance-field branches. Exercise every branch on real Metal rather than
    // treating one non-default shape as representative of the whole enum.
    for (const pvt::ParticleShape shape : {
             pvt::ParticleShape::Spark, pvt::ParticleShape::SoftOrb,
             pvt::ParticleShape::Ring, pvt::ParticleShape::Diamond,
             pvt::ParticleShape::Star}) {
        pvt::RenderConfig particles = parity_config();
        particles.effects.clear();
        particles.quantization.enabled = false;
        particles.transform = {};
        pvt::EffectConfig effect = pvt::default_effect(
            pvt::EffectType::ParticleField);
        effect.id = pvt::allocate_id(particles);
        effect.enabled = true;
        effect.particle_shape = shape;
        effect.particle_profile = pvt::ParticleRenderProfile::Defined;
        effect.intensity = 1.1;
        effect.magnitude = 0.18;
        effect.frequency = 24.0;
        effect.secondary = 0.4;
        effect.radius_pixels = 8.0;
        effect.threshold = 0.55;
        effect.soft_knee = 0.5;
        effect.particle_size_variation = 0.37;
        effect.particle_definition = 0.83;
        effect.particle_twinkle = 0.61;
        effect.particle_seed = UINT64_C(0xfedcba9876543210);
        effect.particle_orientation = pvt::ParticleOrientation::FollowMotion;
        effect.particle_rotation_degrees = -23.0;
        effect.area_radius = 0.58;
        particles.effects.push_back(effect);
        CHECK(pvt::render_frame_at_phase(particles, 0.37, cpu_options,
                                         cpu, nullptr, &error));
        CHECK(pvt::render_frame_at_phase(particles, 0.37, gpu_options,
                                         gpu, nullptr, &error));
        const std::string label = std::string("particle shape ")
                                  + pvt::particle_shape_name(shape);
        check_close(cpu, gpu, 0.20, 0.018, 0.035, 0.002,
                    label.c_str());
    }

    pvt::RenderConfig quantized = parity_config();
    quantized.effects.clear();
    quantized.transform.flip_horizontal = false;
    quantized.transform.flip_vertical = true;
    quantized.transform.mirror = pvt::MirrorMode::FourWay;
    const std::vector<pvt::QuantizationMode> quantization_modes = {
        pvt::QuantizationMode::Rgb,
        pvt::QuantizationMode::Luminance,
        pvt::QuantizationMode::Hue};
    for (const pvt::QuantizationMode mode : quantization_modes) {
        quantized.quantization.mode = mode;
        CHECK(pvt::render_frame(quantized, 8, cpu_options,
                                cpu, nullptr, &error));
        CHECK(pvt::render_frame(quantized, 8, gpu_options,
                                gpu, nullptr, &error));
        const std::string label = std::string("quantization ")
                                  + pvt::quantization_mode_name(mode);
        check_close(cpu, gpu, 0.12, 0.012, 0.002, 0.0002,
                    label.c_str());
    }

    pvt::RenderConfig post_processed = parity_config();
    post_processed.quantization.enabled = false;
    post_processed.post_process.invert_rgb_enabled = true;
    post_processed.post_process.invert_rgb_mix = 0.73;
    post_processed.post_process.invert_red_enabled = true;
    post_processed.post_process.invert_red_mix = 1.0;
    post_processed.post_process.invert_green_enabled = true;
    post_processed.post_process.invert_green_mix = 0.29;
    post_processed.post_process.invert_blue_enabled = true;
    post_processed.post_process.invert_blue_mix = 0.57;
    post_processed.post_process.invert_alpha_enabled = true;
    post_processed.post_process.invert_alpha_mix = 0.41;
    post_processed.post_process.antialias_enabled = true;
    post_processed.post_process.antialias_strength = 0.68;
    post_processed.post_process.antialias_threshold = 0.0;
    post_processed.post_process.antialias_passes = 2;
    CHECK(pvt::render_frame(post_processed, 8, cpu_options,
                            cpu, nullptr, &error));
    CHECK(pvt::render_frame(post_processed, 8, gpu_options,
                            gpu, nullptr, &error));
    check_close(cpu, gpu, 0.12, 0.012, 0.003, 0.0003,
                "post-process global/channel double inversion/straight-alpha antialias");

    // A one-color linear palette makes the source channels exact and visibly
    // out of range while spatial alpha still gives the later odd antialias
    // pass real work. Verify the map in isolation first so CPU/GPU agreement
    // cannot hide an overwrite-ordered implementation or missing alpha clamp.
    pvt::RenderConfig channel_routing = parity_config();
    channel_routing.width = 83;
    channel_routing.height = 57;
    channel_routing.block_size = 1;
    channel_routing.waves.clear();
    channel_routing.swings.clear();
    channel_routing.effects.clear();
    channel_routing.displacement_enabled = false;
    channel_routing.lighting_enabled = false;
    channel_routing.spiral_enabled = false;
    channel_routing.wall_reflection_enabled = false;
    channel_routing.transform = {};
    channel_routing.palette.enabled = true;
    channel_routing.palette.colors = {{
        2.5, -0.5, 0.25, 0.6, "HDR channel route",
        pvt::PaletteColorEncoding::Linear}};
    channel_routing.alpha.use_source_alpha = true;
    channel_routing.quantization.enabled = false;
    channel_routing.post_process = {};
    pvt::Image unmapped;
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, cpu_options,
                                     unmapped, nullptr, &error));

    channel_routing.post_process.channel_map.enabled = true;
    channel_routing.post_process.channel_map.red_source =
        pvt::ChannelSource::Green;
    channel_routing.post_process.channel_map.green_source =
        pvt::ChannelSource::Blue;
    channel_routing.post_process.channel_map.blue_source =
        pvt::ChannelSource::Alpha;
    channel_routing.post_process.channel_map.alpha_source =
        pvt::ChannelSource::Red;
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, gpu_options,
                                     gpu, nullptr, &error));
    bool simultaneous_map = cpu.pixels.size() == unmapped.pixels.size();
    bool high_alpha_clamped = simultaneous_map;
    if (simultaneous_map) {
        for (std::size_t offset = 0U;
             offset + 3U < cpu.pixels.size(); offset += 4U) {
            simultaneous_map = simultaneous_map
                && std::fabs(
                       cpu.pixels[offset] - unmapped.pixels[offset + 1U])
                       < 1.0e-6F
                && std::fabs(cpu.pixels[offset + 1U]
                             - unmapped.pixels[offset + 2U]) < 1.0e-6F
                && std::fabs(cpu.pixels[offset + 2U]
                             - unmapped.pixels[offset + 3U]) < 1.0e-6F;
            high_alpha_clamped = high_alpha_clamped
                                 && cpu.pixels[offset + 3U] == 1.0F;
        }
    }
    CHECK(simultaneous_map);
    CHECK(high_alpha_clamped);
    check_close(cpu, gpu, 0.0001, 0.00001, 0.0001, 0.00001,
                "simultaneous HDR RGBA channel routing/high alpha clamp");

    channel_routing.post_process.channel_map.alpha_source =
        pvt::ChannelSource::Green;
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, gpu_options,
                                     gpu, nullptr, &error));
    bool low_alpha_clamped = !cpu.pixels.empty();
    for (std::size_t offset = 3U; offset < cpu.pixels.size(); offset += 4U) {
        low_alpha_clamped = low_alpha_clamped
                            && cpu.pixels[offset] == 0.0F;
    }
    CHECK(low_alpha_clamped);
    check_close(cpu, gpu, 0.0001, 0.00001, 0.0001, 0.00001,
                "HDR channel routing/low alpha clamp");

    // Put quantization first and each inversion after an odd number of
    // antialias passes. This exercises authored dispatch order and verifies
    // that the final current/scratch buffer is carried into later stages.
    channel_routing.post_process.channel_map.alpha_source =
        pvt::ChannelSource::Red;
    channel_routing.quantization.enabled = true;
    channel_routing.quantization.mode = pvt::QuantizationMode::Rgb;
    channel_routing.quantization.levels = 9;
    channel_routing.quantization.mix = 1.0;
    channel_routing.post_process.antialias_enabled = true;
    channel_routing.post_process.antialias_strength = 0.71;
    channel_routing.post_process.antialias_threshold = 0.0;
    channel_routing.post_process.antialias_passes = 3;
    channel_routing.post_process.invert_rgb_enabled = true;
    channel_routing.post_process.invert_rgb_mix = 0.33;
    channel_routing.post_process.invert_red_enabled = true;
    channel_routing.post_process.invert_red_mix = 0.61;
    channel_routing.post_process.invert_green_enabled = true;
    channel_routing.post_process.invert_green_mix = 0.27;
    channel_routing.post_process.invert_blue_enabled = true;
    channel_routing.post_process.invert_blue_mix = 0.49;
    channel_routing.post_process.invert_alpha_enabled = true;
    channel_routing.post_process.invert_alpha_mix = 0.2;
    channel_routing.post_process.order = {
        pvt::PostProcessStage::Quantization,
        pvt::PostProcessStage::ChannelMap,
        pvt::PostProcessStage::Antialias,
        pvt::PostProcessStage::InvertAlpha,
        pvt::PostProcessStage::InvertBlue,
        pvt::PostProcessStage::InvertGreen,
        pvt::PostProcessStage::InvertRed,
        pvt::PostProcessStage::InvertRgb};
    CHECK(pvt::validate(channel_routing).ok);
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, cpu_options,
                                     cpu, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(channel_routing, 0.37, gpu_options,
                                     gpu, nullptr, &error));
    check_close(cpu, gpu, 0.15, 0.015, 0.006, 0.0006,
                "authored post order/HDR channel map/odd antialias passes");

    // Coordinate stages and glow exercise every shared-buffer direction and
    // retain straight-alpha coverage through the Metal blur pipeline.
    config = parity_config();
    config.quantization.enabled = false;
    config.transform.flip_horizontal = false;
    config.effects[1].enabled = true; // Ripple
    config.effects[1].edge_mode = pvt::EdgeMode::Alpha;
    config.effects[2].enabled = true; // Shake
    config.effects[2].area_radius = 0.55;
    config.effects[4].enabled = true; // Glow
    config.effects[4].intensity = 0.8;
    config.effects[4].radius_pixels = 5.0;
    config.effects[4].threshold = 0.2;
    config.effects[4].soft_knee = 0.4;
    CHECK(pvt::render_frame(config, 5, cpu_options, cpu, nullptr, &error));
    CHECK(pvt::render_frame(config, 5, gpu_options, gpu, nullptr, &error));
    check_close(cpu, gpu, 0.18, 0.02, 0.025, 0.0015,
                "coordinate/glow/straight-alpha");

    // Exact endpoints wrap by definition; near-endpoint samples ensure both
    // implementations approach the same seam instead of only rendering phase
    // zero twice.
    config.effects[4].enabled = false;
    pvt::Image cpu_before;
    pvt::Image cpu_after;
    pvt::Image gpu_before;
    pvt::Image gpu_after;
    CHECK(pvt::render_frame_at_phase(config, 1.0e-6, cpu_options,
                                     cpu_after, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0 - 1.0e-6, cpu_options,
                                     cpu_before, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0e-6, gpu_options,
                                     gpu_after, nullptr, &error));
    CHECK(pvt::render_frame_at_phase(config, 1.0 - 1.0e-6, gpu_options,
                                     gpu_before, nullptr, &error));
    check_close(cpu_before, gpu_before, 0.18, 0.02, 0.025, 0.0015,
                "near seam before");
    check_close(cpu_after, gpu_after, 0.18, 0.02, 0.025, 0.0015,
                "near seam after");
    const Difference cpu_seam = difference(cpu_before, cpu_after);
    const Difference gpu_seam = difference(gpu_before, gpu_after);
    CHECK(std::fabs(cpu_seam.mean_rgb - gpu_seam.mean_rgb) < 0.002);
    CHECK(std::fabs(cpu_seam.mean_alpha - gpu_seam.mean_alpha) < 0.0005);

    // All built-in analytic surfaces remain close to the CPU reference,
    // including their straight-alpha front/rear coverage. Custom OBJ geometry
    // rasterizes only its ordered mesh stage on CPU and resumes the same Metal
    // pipeline afterward instead of falling back for the whole layer.
    config.surface.enabled = true;
    config.surface.curvature = 0.78;
    config.surface.lighting = 0.65;
    config.surface.rotation_x_degrees = -13.0;
    config.surface.rotation_y_degrees = 23.0;
    config.surface.rotation_y_turns_per_loop = 2;
    config.surface.rotation_z_degrees = 7.0;
    config.surface.rotation_order = pvt::SurfaceRotationOrder::YZX;
    config.output.write_alpha = true;
    const std::vector<std::pair<pvt::SurfaceMapping, const char*>> surfaces = {
        {pvt::SurfaceMapping::Plane, "plane surface"},
        {pvt::SurfaceMapping::Cylinder, "cylinder surface"},
        {pvt::SurfaceMapping::Sphere, "sphere surface"},
        {pvt::SurfaceMapping::Cube, "cube surface"}};
    for (const auto& surface : surfaces) {
        config.surface.mapping = surface.first;
        CHECK(pvt::render_frame(config, 4, cpu_options, cpu, nullptr, &error));
        CHECK(pvt::render_frame(config, 4, gpu_options, gpu, nullptr, &error));
        check_close(cpu, gpu, 0.24, 0.018, 0.06, 0.0025,
                    surface.second);
    }

    config.surface.mapping = pvt::SurfaceMapping::CustomObj;
    config.surface.obj_path =
        PVT_TEST_SOURCE_DIR "/tests/assets/obj/closed_cube.obj";
    pvt::Image reference;
    CHECK(pvt::render_frame(config, 4, cpu_options, reference, nullptr,
                            &error));
    CHECK(pvt::render_frame(config, 4, gpu_options, gpu, nullptr,
                            &error));
    check_close(reference, gpu, 0.24, 0.018, 0.06, 0.0025,
                "custom OBJ split CPU/GPU surface");
    pvt::Image hybrid;
    CHECK(pvt::render_frame(config, 4, hybrid_options, hybrid, nullptr,
                            &error));
    check_close(reference, hybrid, 0.24, 0.018, 0.06, 0.0025,
                "custom OBJ hybrid surface");

    config.surface.mapping = pvt::SurfaceMapping::Plane;
    config.surface.plane_displacement.enabled = true;
    config.surface.plane_displacement.path =
        PVT_TEST_SOURCE_DIR "/icon/icon.png";
    config.surface.plane_displacement.minimum = -0.31;
    config.surface.plane_displacement.maximum = 0.44;
    config.surface.plane_displacement.midpoint = 0.46;
    config.surface.plane_displacement.pixels_per_node = 5;
    CHECK(pvt::render_frame(config, 4, cpu_options, reference, nullptr,
                            &error));
    CHECK(pvt::render_frame(config, 4, gpu_options, gpu, nullptr,
                            &error));
    check_close(reference, gpu, 0.24, 0.018, 0.06, 0.0025,
                "displacement Plane split CPU/GPU surface");
    CHECK(pvt::render_frame(config, 4, hybrid_options, hybrid, nullptr,
                            &error));
    check_close(reference, hybrid, 0.24, 0.018, 0.06, 0.0025,
                "displacement Plane hybrid surface");

    std::atomic_bool cancel {true};
    pvt::Image sentinel = cpu;
    config.surface.enabled = false;
    CHECK(!pvt::render_frame(config, 3, gpu_options, sentinel, &cancel,
                             &error));
    CHECK(sentinel.pixels == cpu.pixels);

    gpu_options.maximum_gpu_frames_in_flight =
        pvt::kMaximumGpuFramesInFlight + 1U;
    CHECK(!pvt::render_frame(config, 3, gpu_options, sentinel, nullptr,
                             &error));
}

void test_hybrid_project_parity() {
    const auto capabilities = pvt::renderer_capabilities();
    if (!capabilities.metal_available) return;
    pvt::ProjectConfig project = pvt::default_project();
    project.canvas.width = 96;
    project.canvas.height = 64;
    project.canvas.block_size = 4;
    project.output.write_alpha = true;
    project.layers.front().render = parity_config();
    pvt::LayerConfig upper = pvt::default_layer(1U);
    upper.file_id = pvt::allocate_layer_file_id(project);
    upper.opacity = 0.55;
    upper.blend_mode = pvt::BlendMode::Overlay;
    upper.render.alpha.enabled = true;
    upper.render.alpha.minimum = 0.2;
    upper.render.alpha.maximum = 0.8;
    upper.render.effects[1].enabled = true;
    project.layers.push_back(std::move(upper));

    pvt::FrameRenderOptions cpu_options;
    cpu_options.backend = pvt::RenderBackend::Cpu;
    pvt::FrameRenderOptions hybrid_options;
    hybrid_options.backend = pvt::RenderBackend::CpuAndGpu;
    hybrid_options.maximum_gpu_frames_in_flight = 1U;
    pvt::Image cpu;
    pvt::Image hybrid;
    std::string error;
    for (const pvt::AlphaMode alpha_mode : {pvt::AlphaMode::AlphaOver,
                                            pvt::AlphaMode::AlphaUnder}) {
        project.layers.back().alpha_mode = alpha_mode;
        CHECK(pvt::render_project_frame(project, 7, cpu_options, cpu, nullptr,
                                        &error));
        CHECK(pvt::render_project_frame(project, 7, hybrid_options, hybrid,
                                        nullptr, &error));
        check_close(cpu, hybrid, 0.16, 0.014, 0.006, 0.0005,
                    alpha_mode == pvt::AlphaMode::AlphaOver
                        ? "hybrid alpha-over layered project"
                        : "hybrid alpha-under layered project");
    }
}

} // namespace

int main() {
    test_backend_contract();
    test_hybrid_project_parity();
    if (failures != 0) {
        std::cerr << failures << " Metal backend test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Metal backend availability, dispatch, cancellation, alpha, "
                 "image, layer, and seam parity tests passed.\n";
    return EXIT_SUCCESS;
}
