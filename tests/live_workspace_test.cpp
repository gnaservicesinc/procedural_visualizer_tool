#include "live_workspace.h"
#include "project_bundle.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <iostream>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir settings_directory;
    if (!settings_directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PVT tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Live startup"));
    QSettings().setValue(QStringLiteral("live/resolutionScale"), 0.25);

    pvt::ProjectDocument document;
    auto project = pvt::default_project();
    project.canvas.width = 64;
    project.canvas.height = 64;
    project.canvas.block_size = 1.0;
    if (argc > 1) {
        std::string error;
        if (!pvt::load_project_document(argv[1], document, &error)) {
            std::cerr << error << '\n';
            return 1;
        }
        project = document.project;
    }
    auto& config = project.canvas.live;
    config = {};
    pvt::LiveEndpointConfig endpoint;
    endpoint.uuid = "c5ea3a32-f3a0-49c1-80ac-decc109f741c";
    endpoint.protocol = pvt::LiveEndpointProtocol::Audio;
    endpoint.direction = pvt::LiveEndpointDirection::Input;
    config.endpoints.push_back(endpoint);
    pvt::LiveClockInputConfig input;
    input.enabled = true;
    input.source = pvt::LiveClockInputSource::AudioStream;
    input.endpoint_uuid = endpoint.uuid;
    input.holdover_milliseconds = 0;
    config.clock_inputs.push_back(input);
    config.safety.frame_time_watchdog_enabled = false;
    config.safety.last_good_frame_timeout_milliseconds = 1000;
    // This identity cannot select hardware. The regression must also pass on
    // machines with no microphone permission or no audio devices at all.
    QSettings().setValue(QStringLiteral("live/bindings/")
                            + QString::fromStdString(endpoint.uuid)
                            + QStringLiteral("/audioDevice"),
                        QStringLiteral("pvt-nonexistent-test-input"));
    LiveWorkspace workspace([&] { return project; }, [&] { return project; },
        [] { return 0; }, [] {
            pvt::FrameRenderOptions options;
            options.backend = pvt::RenderBackend::Cpu;
            return options;
        }, [] { return 1U; }, [&] { return project.layers.front().uuid; }, {});
    workspace.setProjectLiveConfig(config);
    workspace.resize(800, 600);
    workspace.show();
    int delivered = 0;
    QImage first_image;
    bool animated = false;
    QObject::connect(&workspace, &LiveWorkspace::livePreviewFrame,
        [&](const QImage& image) {
            ++delivered;
            if (first_image.isNull()) first_image = image;
            else if (image != first_image) animated = true;
        });
    const auto wait_for = [&](const auto& ready, int timeout) {
        QElapsedTimer timer;
        timer.start();
        while (!ready() && timer.elapsed() < timeout) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return ready();
    };
    auto* pool = QThreadPool::globalInstance();
    const int previous_threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    pool->reserveThread();
    workspace.setLiveActive(true);
    // Delay the first frame past audio holdover, as a cold renderer can do.
    // Previously its completion was discarded and all later requests stopped.
    QTimer::singleShot(250, &workspace, [pool] { pool->releaseThread(); });
    const bool started = wait_for([&] { return delivered >= 4 && animated; }, 5000);
    workspace.setLiveActive(false);
    pool->setMaxThreadCount(previous_threads);
    if (!started) {
        std::cerr << "Live startup with unavailable audio delivered " << delivered
                  << " frames; animation=" << animated << '\n';
        return 1;
    }
    const int before_restart = delivered;
    workspace.setLiveActive(true);
    const bool restarted = wait_for([&] { return delivered >= before_restart + 4; }, 5000);
    bool waiting_for_audio = false;
    for (auto* widget : workspace.findChildren<QWidget*>()) {
        waiting_for_audio |= widget->toolTip() == LiveWorkspace::tr(
            "Waiting for live audio; animation is using the project clock. Check the selected audio input and microphone permission.");
    }
    const auto* monitor = workspace.findChild<QLabel*>(QStringLiteral("liveProgramLabel"));
    const bool visible_image = monitor != nullptr && !monitor->pixmap().isNull();
    workspace.setLiveActive(false);
    QCoreApplication::processEvents();
    if (!restarted || !waiting_for_audio || !visible_image || workspace.isRealtimeOutputActive()) {
        std::cerr << "Live restart, input status, or stop failed\n";
        return 1;
    }
    config.safety.dropout_behavior = pvt::LiveDropoutBehavior::Blackout;
    workspace.setProjectLiveConfig(config);
    workspace.setLiveActive(true);
    const bool blacked_out = wait_for([&] {
        return monitor->text() == LiveWorkspace::tr("SAFETY BLACKOUT\nLast-good watchdog");
    }, 2000);
    workspace.setLiveActive(false);
    if (!blacked_out) {
        std::cerr << "Explicit audio-dropout Blackout policy was bypassed\n";
        return 1;
    }
    std::cout << "Live unavailable-input startup/restart: " << delivered
              << " frames, animation advancing, explicit blackout retained, runtime stopped\n";
    return 0;
}
