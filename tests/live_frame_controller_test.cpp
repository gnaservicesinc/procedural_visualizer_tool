#include "live_frame_controller.h"
#include "display_color.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

pvt::ProjectConfig blank_project() {
    // Keep the installed application's blank canvas aligned with the supplied
    // Untitled.zip concept without changing the richer public API defaults
    // used by existing library clients and tests.
    auto project = pvt::default_project();
    project.name = "Untitled";
    project.canvas.width = 1920;
    project.canvas.height = 1080;
    project.canvas.block_size = 1;
    project.canvas.total_frames = 300;
    project.canvas.fps = 60.0;
    project.canvas.clock = {};
    project.canvas.motion_paths.clear();
    project.canvas.audio_reactive_defaults = {};
    project.output = {};
    project.output.write_alpha = true;

    auto& layer = project.layers.front();
    layer.name = "Layer 1";
    layer.enabled = true;
    layer.opacity = 1.0;
    layer.blend_mode = pvt::BlendMode::Normal;
    layer.alpha_mode = pvt::AlphaMode::AlphaOver;
    auto& render = layer.render;
    render.waves.clear();
    render.swings.clear();
    auto swing = pvt::default_swing(0U);
    swing.id = 4U;
    render.swings.push_back(std::move(swing));
    render.swings_enabled = false;
    render.effects.clear();
    auto zoom = pvt::default_effect(pvt::EffectType::EndlessZoom);
    zoom.id = 5U;
    zoom.enabled = false;
    render.effects.push_back(std::move(zoom));
    render.layer_clock = {};
    render.audio_reactive = {};
    render.audio_reactive_override_enabled = false;
    render.phrase_warp = 0.0;
    render.ghost_mix = 0.0;
    render.ghost_lag_degrees = 0.0;
    render.displacement_enabled = false;
    render.lighting_enabled = false;
    render.spiral_enabled = false;
    render.wall_reflection_enabled = false;
    render.hue_cycles = 1;
    render.saturation = 1.0;
    render.starting_image = {};
    render.palette = {};
    render.surface = {};
    render.transform = {};
    render.motion = {};
    render.quantization = {};
    render.alpha = {};
    render.alpha.minimum = 0.0;
    render.alpha.maximum = 1.0;
    render.alpha.spatial_frequency = 1.99;
    render.alpha.cycles_per_loop = 6;
    render.alpha.use_source_alpha = true;
    render.starting_colors = {};
    return project;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    LiveFrameController controller;
    std::vector<LiveFrameController::Result> results;
    QObject::connect(&controller, &LiveFrameController::frameFinished,
        [&](const auto& result) { results.push_back(result); });
    auto project = pvt::default_project();
    project.canvas.width = 64;
    project.canvas.height = 64;
    project.canvas.block_size = 0.0;
    project.canvas.block_size_modulation.lfo_enabled = true;
    project.canvas.block_size_modulation.minimum = 64.0;
    project.canvas.block_size_modulation.maximum = 64.0;
    pvt::FrameRenderOptions options;
    options.backend = pvt::RenderBackend::Cpu;
    if (argc > 1) {
        project = blank_project();
        if (std::string(argv[1]) == "gpu") options.backend = pvt::RenderBackend::Gpu;
        for (const double scale : {0.25, 0.5, 1.0}) {
            for (int frame = 0; frame < 600; ++frame) {
                results.clear();
                controller.request(project, 0.0, frame % 300, QSize(640, 360), scale,
                    1000.0 / 60.0, 100, options, 3U, 3U);
                QElapsedTimer wait;
                wait.start();
                while (results.empty() && wait.elapsed() < 10000) {
                    QCoreApplication::processEvents();
                    QThread::msleep(1);
                }
                if (results.empty()) return 2;
                const auto& r = results.back();
                if (r.cancelled || !r.error.isEmpty() || r.render_milliseconds > 25 || frame % 100 == 0)
                    std::cout << "scale=" << scale << " frame=" << frame << " ms=" << r.render_milliseconds
                              << " cancelled=" << r.cancelled << " watchdog=" << r.watchdog_expired
                              << " error=" << r.error.toStdString() << std::endl;
            }
        }
        return 0;
    }
    const auto request = [&] {
        controller.request(project, 0.25, {}, QSize(32, 32), 1.0,
                           1000.0, 5000, options, 1U, 1U);
    };
    request();
    // Complete the worker without processing its queued finished event.
    if (!QThreadPool::globalInstance()->waitForDone(5000)) return 1;
    request();
    QElapsedTimer timer;
    timer.start();
    while (results.size() < 2U && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    if (results.size() != 2U || results[0].sequence != 1U
        || results[1].sequence != 2U) {
        std::cerr << "Live playback lost an undelivered completed frame\n";
        return 1;
    }
    for (const auto& result : results) {
        if (!result.error.isEmpty() || result.image.size() != QSize(32, 32)) {
            std::cerr << "Scaled block-size LFO failed: "
                      << result.error.toStdString() << '\n';
            return 1;
        }
    }
    results.clear();
    request();
    if (!QThreadPool::globalInstance()->waitForDone(5000)) return 1;
    controller.stop();
    QCoreApplication::processEvents();
    if (!results.empty() || controller.isRendering()) {
        std::cerr << "Stopped Live controller delivered a stale completion\n";
        return 1;
    }

    // A reduced one-pixel project must render on the preview pixel grid.
    // Turning 1 into 0.25 invokes full-canvas supersampling for every effect;
    // checking only the returned image dimensions misses that regression.
    // Explicitly authored subpixel blocks must retain their existing resolve.
    for (const auto sizes : {std::pair{1.0, 1.0}, std::pair{3.0, 1.0},
                             std::pair{6.0, 1.5}, std::pair{0.5, 0.125},
                             std::pair{0.0, 0.0}}) {
        project = pvt::default_project();
        project.canvas.width = 128;
        project.canvas.height = 128;
        project.canvas.block_size = sizes.first;
        auto& layer = project.layers.front().render;
        layer.effects.clear();
        layer.waves.clear();
        layer.swings.clear();
        layer.displacement_enabled = false;
        layer.lighting_enabled = false;
        layer.post_process.antialias_enabled = true;
        layer.post_process.antialias_passes = 2;
        for (const bool synchronized : {false, true}) {
            auto expected_project = project;
            expected_project.canvas.width = 32;
            expected_project.canvas.height = 32;
            expected_project.canvas.block_size = sizes.second;
            auto& expected_layer = expected_project.layers.front().render;
            expected_layer.starting_colors.reference_width = sizes.first > 0 ? 128 : 0;
            expected_layer.starting_colors.reference_height = sizes.first > 0 ? 128 : 0;
            expected_layer.starting_colors.reference_block_size = sizes.first;
            expected_layer.displacement *= 0.25;
            pvt::Image expected;
            std::string error;
            const bool rendered = synchronized
                ? pvt::render_project_frame(expected_project, 19, options,
                                             expected, nullptr, &error)
                : pvt::render_project_frame_at_phase(expected_project, 0.25,
                        options, expected, nullptr, &error);
            if (!rendered) {
                std::cerr << error << '\n';
                return 1;
            }
            results.clear();
            controller.request(project, 0.25,
                synchronized ? std::optional<int>{19} : std::nullopt,
                QSize(32, 32), 1.0, 1000.0, 5000, options, 2U, 2U);
            timer.restart();
            while (results.empty() && timer.elapsed() < 5000) {
                QCoreApplication::processEvents();
                QThread::msleep(1);
            }
            if (results.size() != 1U || !results.front().error.isEmpty()
                || results.front().image.size() != QSize(32, 32)) {
                std::cerr << "Reduced Live frame failed\n";
                return 1;
            }
            unsigned char expected_row[32 * 4];
            for (int y = 0; y < 32; ++y) {
                pvt::display::convert_rgba_row(expected.pixel(0, y),
                                               expected_row, 32U);
                if (std::memcmp(expected_row,
                        results.front().image.constScanLine(y), sizeof(expected_row)) != 0) {
                    std::cerr << "Preview block scaling changed the pixel grid for "
                              << sizes.first << '\n';
                    return 1;
                }
            }
            controller.stop();
        }
    }
    return 0;
}
