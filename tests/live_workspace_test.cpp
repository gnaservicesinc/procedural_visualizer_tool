#include "live_workspace.h"
#include "live_frame_controller.h"
#include "studio_widgets.h"
#include "stage_output_window.h"
#include "project_bundle.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QSettings>
#include <QTableWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <iostream>

bool test_live_mapping_controls();

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
    QApplication app(argc, argv);
    QTemporaryDir settings_directory;
    if (!settings_directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PVT tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Live startup"));
    QSettings().setValue(QStringLiteral("live/resolutionScale"), 0.25);

    if (argc > 1 && std::string(argv[1]) == "--blank-probe") {
        QSettings().setValue(QStringLiteral("live/resolutionScale"), 0.0);
        auto project = blank_project();
        pvt::ParameterLfo lfo;
        lfo.id = 12;
        lfo.target_path = "block_size";
        lfo.minimum = 1;
        lfo.maximum = 2;
        project.layers.front().render.parameter_lfos.push_back(lfo);
        QSettings().setValue(QStringLiteral("live/resolutionScale"), 0.25);
        LiveWorkspace workspace([&] { return project; }, [&] { return project; },
            [] { return 0; }, [] {
                pvt::FrameRenderOptions options;
                options.backend = pvt::RenderBackend::CpuAndGpu;
                return options;
            }, [] { return 1U; }, [&] { return project.layers.front().uuid; }, {});
        workspace.setProjectLiveConfig(project.canvas.live);
        workspace.resize(1600, 960);
        workspace.show();
        QElapsedTimer elapsed;
        elapsed.start();
        double previous = 0;
        int delivered = 0;
        auto* renderer = workspace.findChild<LiveFrameController*>();
        QObject::connect(renderer, &LiveFrameController::frameFinished,
            [&](const LiveFrameController::Result& r) {
                const double now = elapsed.nsecsElapsed() / 1e6;
                if (r.cancelled || !r.error.isEmpty() || now - previous > 50 || delivered % 60 == 0)
                    std::cout << "t=" << now << " ms=" << r.render_milliseconds
                              << " gap=" << now-previous << " size=" << r.image.width() << "x" << r.image.height()
                              << " cancelled=" << r.cancelled << " watchdog=" << r.watchdog_expired
                              << " error=" << r.error.toStdString() << std::endl;
                previous=now; ++delivered;
            });
        workspace.setLiveActive(true);
        while (elapsed.elapsed() < 16000) {
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        workspace.setLiveActive(false);
        std::cout << "Delivered: " << delivered << std::endl;
        return 0;
    }
    // Clock files join the normal input list and machine routing matrix without
    // starting devices or rendering just because their controls are visible.
    {
        auto source_project = pvt::default_project();
        LiveWorkspace routes([&] { return source_project; }, [&] { return source_project; },
            [] { return 0; }, [] { return pvt::FrameRenderOptions{}; }, [] { return 1U; },
            [&] { return source_project.layers.front().uuid; },
            [&](const pvt::LiveConfig& live, const QString&) { source_project.canvas.live = live; });
        routes.setClockAudioProvider([] {
            return QVector<LiveWorkspace::ClockAudioSource>{
                {QStringLiteral("clock:test"), QStringLiteral("Test song"), QStringLiteral("/test/song.wav")}};
        });
        routes.refreshAudioInputs();
        auto* matrix = routes.findChild<QTableWidget*>(QStringLiteral("liveAudioRoutingMatrix"));
        int file_row = -1;
        for (int row = 0; matrix && row < matrix->rowCount(); ++row)
            if (matrix->item(row, 0)->text().startsWith(QStringLiteral("Test song"))) file_row = row;
        bool file_choice = false;
        for (const auto& input : routes.availableAudioInputs())
            file_choice |= input.id == QStringLiteral("clock:test");
        bool file_role = false;
        QString file_role_uuid;
        for (const auto& role : source_project.canvas.live.endpoints) {
            if (routes.audioInputBinding(role.uuid) == QStringLiteral("clock:test")) {
                file_role = true;
                file_role_uuid = QString::fromStdString(role.uuid);
            }
        }
        if (file_row < 0 || !file_choice || !file_role || routes.isRealtimeOutputActive()) return 1;
        matrix->item(file_row, 2)->setCheckState(Qt::Checked);
        routes.refreshAudioInputs();
        if (matrix->item(file_row, 2)->checkState() != Qt::Checked
            || !QSettings().value(QStringLiteral("live/audioMatrix/clock:test/analysis")).toBool()) return 1;
        matrix->item(file_row, 2)->setCheckState(Qt::Unchecked);
        QPushButton* manager = nullptr;
        for (auto* button : routes.findChildren<QPushButton*>())
            if (button->text() == LiveWorkspace::tr("Manage Roles…")) manager = button;
        if (!manager) return 1;
        QTimer::singleShot(0, &routes, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* selection = dialog->findChild<QComboBox*>(QStringLiteral("liveRoleSelection"));
            selection->setCurrentIndex(selection->findData(file_role_uuid));
            dialog->findChild<QLineEdit*>()->setText(QStringLiteral("Renamed song"));
            dialog->accept();
        });
        manager->click();
        const auto renamed = source_project.canvas.live;
        const auto named = std::find_if(renamed.endpoints.begin(), renamed.endpoints.end(),
            [&](const auto& role) { return QString::fromStdString(role.uuid) == file_role_uuid; });
        if (named == renamed.endpoints.end() || named->name != "Renamed song") return 1;
        QTimer::singleShot(0, &routes, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* selection = dialog->findChild<QComboBox*>(QStringLiteral("liveRoleSelection"));
            selection->setCurrentIndex(selection->findData(file_role_uuid));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            for (auto* button : buttons->buttons())
                if (buttons->buttonRole(button) == QDialogButtonBox::DestructiveRole) button->click();
        });
        manager->click();
        routes.refreshAudioInputs();
        if (std::any_of(source_project.canvas.live.endpoints.begin(), source_project.canvas.live.endpoints.end(),
            [&](const auto& role) { return QString::fromStdString(role.uuid) == file_role_uuid; })) return 1;
        // The authored undo/load path restores the same role identity and binding.
        routes.setProjectLiveConfig(renamed);
        if (routes.audioInputBinding(named->uuid) != QStringLiteral("clock:test")) return 1;
    }
    if (!test_live_mapping_controls()) return 1;
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
    QElapsedTimer project_clock;
    project_clock.start();
    LiveWorkspace workspace([&] { return project; }, [&] { return project; },
        [&] { return static_cast<int>(project_clock.elapsed() * project.canvas.fps / 1000.0)
            % std::max(1, project.canvas.total_frames); }, [] {
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
    StageOutputWindow* stage = nullptr;
    for (auto* widget : QApplication::topLevelWidgets())
        if (auto* candidate = qobject_cast<StageOutputWindow*>(widget)) stage = candidate;
    const bool visible_image = stage && stage->isVisible() && stage->hasGoodFrame();
    if (workspace.findChild<QLabel*>(QStringLiteral("liveProgramLabel"))) return 1;
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
        return stage && stage->isBlackout();
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
