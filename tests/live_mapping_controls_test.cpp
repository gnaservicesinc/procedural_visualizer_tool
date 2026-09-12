#include "live_workspace.h"
#include "live_midi.h"
#include "project_bundle.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QThread>

#include <cmath>
#include <iostream>
#include <stdexcept>

// Exercise incoming controls through the MIDI router and inspect them through
// the ordinary scene capture action, including edits made while Live is active.
bool test_live_mapping_controls() {
    try {
        auto project = pvt::default_project();
        project.canvas.width = project.canvas.height = 16;
        project.layers.push_back(pvt::default_layer(1U));
        project.layers.front().opacity = 0.6;
        std::uint64_t revision = 1;
        auto& live = project.canvas.live;
        live = {};
        live.safety.prevent_device_sleep = false;
        pvt::LiveEndpointConfig endpoint;
        endpoint.uuid = pvt::generate_uuid();
        endpoint.protocol = pvt::LiveEndpointProtocol::Midi;
        endpoint.direction = pvt::LiveEndpointDirection::Input;
        live.endpoints.push_back(endpoint);
        const auto opacity = "layer/" + project.layers.front().uuid + "/opacity";
        const auto add = [&](int cc, const std::string& path,
                             pvt::LiveMappingMode mode, double lo, double hi, int ms) {
            pvt::LiveControlMapping mapping;
            mapping.endpoint_uuid = endpoint.uuid;
            mapping.control_number = cc;
            mapping.target_path = path;
            mapping.mode = mode;
            mapping.output_minimum = lo;
            mapping.output_maximum = hi;
            mapping.smoothing_milliseconds = ms;
            live.mappings.push_back(mapping);
        };
        add(1, "project.clock.bpm", pvt::LiveMappingMode::Absolute, 24, 120, 200);
        add(2, opacity, pvt::LiveMappingMode::Relative, 0, 1, 0);
        add(3, opacity, pvt::LiveMappingMode::Absolute, 0, 1, 0);
        const auto enabled = "layer/" + project.layers.front().uuid + "/enabled";
        add(4, enabled, pvt::LiveMappingMode::Toggle, 0, 1, 0);
        pvt::LiveSceneConfig scene;
        scene.uuid = pvt::generate_uuid();
        scene.name = "Control snapshot";
        live.scenes.push_back(scene);
        pvt::LiveConfig captured;
        LiveWorkspace* workspace_ptr = nullptr;
        LiveWorkspace workspace([&] { return project; }, [&] { return project; },
            [] { return 0; }, [] {
                pvt::FrameRenderOptions options;
                options.backend = pvt::RenderBackend::Cpu;
                return options;
            }, [&] { return revision; }, [&] { return project.layers.front().uuid; },
            [&](const pvt::LiveConfig& config, const QString&) {
                captured = config;
                project.canvas.live = config;
                ++revision;
                // Mirror the main editor's synchronous accepted-config echo.
                workspace_ptr->setProjectLiveConfig(config);
                workspace_ptr->refreshProjectSnapshot();
            });
        workspace_ptr = &workspace;
        workspace.setProjectLiveConfig(live);
        const auto require = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        auto* midi = workspace.findChild<LiveMidiRouter*>();
        auto* scenes = workspace.findChild<QListWidget*>(QStringLiteral("liveSceneList"));
        auto* update = workspace.findChild<QPushButton*>(QStringLiteral("liveSceneUpdate"));
        require(midi && scenes && update, "Live mapping controls are unavailable.");
        workspace.setLiveActive(true);
        const auto send = [&](int cc, double value) {
            midi->controlMessage(LiveMidiRouter::MessageKind::ControlChange,
                                 1, cc, value, QStringLiteral("Test controller"));
        };
        const auto capture = [&](const std::string& path) {
            scenes->setCurrentRow(0);
            update->click();
            require(!captured.scenes.empty(), "Scene capture did not reach the editor.");
            for (const auto& value : captured.scenes.front().values) {
                if (value.target_path == path) return std::stod(value.value);
            }
            throw std::runtime_error("Captured scene omitted the requested target.");
        };
        send(2, 1.0);
        require(std::fabs(capture(opacity) - (0.6 - 0.1 / 127.0)) < 1e-12,
                "The first relative input jumped away from the authored setting.");
        send(4, 1.0);
        require(capture(enabled) == 0.0,
                "The first toggle ignored the authored enabled state.");
        send(4, 0.0);
        send(4, 1.0);
        require(capture(enabled) == 1.0, "The toggle did not restore the enabled state.");
        send(3, 0.5);
        send(2, 1.0); // MIDI 127: one decrement.
        require(std::fabs(capture(opacity) - (0.5 - 0.1 / 127.0)) < 1e-12,
                "Relative MIDI 127 did not decrement.");
        send(2, 1.0 / 127.0);
        send(2, 0.0);
        require(std::fabs(capture(opacity) - 0.5) < 1e-12,
                "Relative MIDI increments/neutral did not preserve symmetry.");
        const double authored_bpm = project.canvas.clock.meter.bpm;
        send(1, 0.0);
        QThread::msleep(30);
        const double first_smoothed = capture("project.clock.bpm");
        require(first_smoothed > 24.0 && first_smoothed < authored_bpm,
                "The first smoothed input jumped straight to its destination.");
        require(project.canvas.clock.meter.bpm == authored_bpm,
                "Live wrote a transient value into the authored project.");
        send(1, 1.0);
        QElapsedTimer elapsed;
        elapsed.start();
        double smoothed = 0;
        while (elapsed.elapsed() < 240) {
            send(1, 1.0); // Audio/OSC can repeat the same target every UI tick.
            smoothed = capture("project.clock.bpm");
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        smoothed = capture("project.clock.bpm");
        require(smoothed == 120.0, "Smoothing missed its configured completion time.");
        project.layers.front().opacity = 0.8;
        ++revision; // Ordinary edits do not explicitly rebuild the Live UI.
        require(capture(opacity) == 0.8,
                "An old Live override concealed a main-editor edit.");
        require(capture("project.clock.bpm") == 120.0,
                "Editing one setting released an unrelated Live override.");
        send(2, 1.0);
        require(std::fabs(capture(opacity) - (0.8 - 0.1 / 127.0)) < 1e-12,
                "Relative input did not resume from the main-editor edit.");

        send(3, 0.4);
        live.mappings[2].enabled = false;
        workspace.setProjectLiveConfig(live);
        require(capture(opacity) == 0.8, "A disabled mapping retained its output override.");
        live.mappings[2].enabled = true;
        workspace.setProjectLiveConfig(live);
        send(3, 0.4);
        live.mappings.erase(live.mappings.begin());
        workspace.setProjectLiveConfig(live);
        require(capture(opacity) == 0.4,
                "Deleting another mapping lost an unchanged connection's output.");
        live.mappings[1].target_path = "project.fps";
        workspace.setProjectLiveConfig(live);
        require(capture(opacity) == 0.8, "Rebinding a mapping left its old target overridden.");
        live.mappings[1].target_path = opacity;
        workspace.setProjectLiveConfig(live);

        // A manual edit must also take precedence over the rest of a timed take.
        live.scenes.front().transition_milliseconds = 10000;
        live.scenes.front().values = {{opacity, pvt::LiveSceneValueType::Real, "0.2"}};
        workspace.setProjectLiveConfig(live);
        auto* take = workspace.findChild<QPushButton*>(QStringLiteral("liveSceneTake"));
        require(take != nullptr, "Scene take is unavailable.");
        scenes->setCurrentRow(0);
        take->click();
        project.layers.front().opacity = 0.9;
        ++revision;
        require(capture(opacity) == 0.9, "A timed scene overwrote an authored edit.");
        QThread::msleep(25);
        require(capture(opacity) == 0.9, "A timed scene reclaimed an edited setting.");

        auto removed = project.layers.front();
        project.layers.erase(project.layers.begin());
        workspace.refreshProjectSnapshot();
        send(3, 0.1); // An unresolved mapping must not recreate the override.
        project.layers.insert(project.layers.begin(), removed);
        workspace.refreshProjectSnapshot();
        require(capture(opacity) == removed.opacity,
                "A deleted target's override reappeared after restoring its ID.");
        workspace.setLiveActive(false);
        workspace.setLiveActive(true);
        require(capture(opacity) == removed.opacity,
                "Reopening Live changed the authored setting.");
        workspace.setLiveActive(false);
        return true;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return false;
    }
}
