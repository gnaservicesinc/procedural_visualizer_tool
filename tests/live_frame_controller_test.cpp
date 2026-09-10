#include "live_frame_controller.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>

#include <iostream>
#include <vector>

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
    return 0;
}
