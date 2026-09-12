#include "main_window.h"
#include "remote_bridge.h"
#include "live_workspace.h"
#include "live_target_registry.h"
#include "preview_widget.h"
#include "../src/audio_playback.h"
#include <QApplication>
#include <QJsonArray>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUndoStack>
#include <cmath>

void MainWindow::initializeRemotes() {
    remote_bridge_ = new RemoteBridge(this);
    remote_bridge_->setStateProvider([this] {
        QJsonArray targets;
        for (const auto& target : buildLiveTargetRegistry(project_)) {
            targets.append(QJsonObject{{"path", target.path}, {"label", target.label},
                {"section", target.section}, {"kind", static_cast<int>(target.kind)},
                {"minimum", target.minimum}, {"maximum", target.maximum}, {"value", target.current_value}});
        }
        return QJsonObject{{"targets", targets}, {"revision", QString::number(document_revision_)},
            {"background", remote_bridge_->background()}, {"live", live_workspace_->isLiveActive()},
            {"playing", playback_timer_ && playback_timer_->isActive()},
            {"busy", project_io_active_ || export_active_}};
    });
    connect(remote_bridge_, &RemoteBridge::statusChanged, this, [this](const QString& text) {
        if (status_) status_->setText(text);
    });
    connect(remote_bridge_, &RemoteBridge::backgroundRequested, this, [this](bool value) { setRemoteBackground(value); });
    connect(remote_bridge_, &RemoteBridge::configurationChanged, this, [this] {
        live_workspace_->setRemoteControlTargets(remote_bridge_->controlNames(), remote_bridge_->activeControlSlot());
        audio_playback_->enable_remote_audio(remote_bridge_->enabled());
        live_workspace_->enableRemoteAudio(remote_bridge_->enabled());
        if (remote_bridge_->enabled()) { schedulePreview(); live_workspace_->requestRealtimeFrame(); }
    });
    connect(live_workspace_, &LiveWorkspace::remoteControlSelected, remote_bridge_, &RemoteBridge::selectControl);
    connect(live_workspace_, &LiveWorkspace::remotePresentationFrame, remote_bridge_, &RemoteBridge::sendFrame);
    connect(preview_, &PreviewWidget::imagePresented, this, [this](const QImage& image) {
        if (!live_workspace_->isRealtimeOutputActive()) remote_bridge_->sendFrame(image);
    });
    auto* audio_timer = new QTimer(this);
    audio_timer->setInterval(20);
    connect(audio_timer, &QTimer::timeout, this, [this] {
        if (!remote_bridge_->enabled()) return;
        QByteArray pcm(3840, '\0');
        auto* samples = reinterpret_cast<std::uint8_t*>(pcm.data());
        for (int i = 0; i < 5; ++i) {
            const bool available = live_workspace_->isLiveActive()
                ? live_workspace_->readRemoteAudio(samples) : audio_playback_->read_remote_audio(samples);
            if (!available) break;
            remote_bridge_->sendAudio(pcm);
        }
    });
    audio_timer->start();
    connect(remote_bridge_, &RemoteBridge::commandRequested, this,
        [this](const QString& token, const QString& remote, const QJsonObject& command) {
        const auto action = command.value("action").toString();
        QString error;
        if (!remote_bridge_->authorized(remote, action)) error = tr("Remote is no longer authorized.");
        else if (action == "background") {
            if (!command.value("value").isBool() || !setRemoteBackground(command.value("value").toBool()))
                error = tr("Background mode requires an available system tray or menu bar.");
        } else if (project_io_active_ || export_active_) error = tr("PVT is busy loading, saving or exporting.");
        else if (action == "set") {
            const auto path = command.value("path").toString();
            const auto value = command.value("value");
            const auto targets = buildLiveTargetRegistry(project_);
            auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& target) { return target.path == path; });
            if (command.value("revision").toString() != QString::number(document_revision_)) error = tr("Project changed; refresh before editing.");
            else if (found == targets.end() || !value.isDouble() || !std::isfinite(value.toDouble())) error = tr("Unknown target or invalid value.");
            else if (value.toDouble() < found->minimum || value.toDouble() > found->maximum
                     || (found->kind != LiveTargetKind::Real && std::floor(value.toDouble()) != value.toDouble())) error = tr("Value is outside the target range.");
            else {
                auto candidate = project_;
                if (!found->apply(candidate, value.toDouble())) error = tr("Target could not be changed.");
                else if (const auto validation = pvt::validate(candidate); !validation.ok) error = QString::fromStdString(validation.message);
                else {
                    auto before = captureProjectState();
                    const auto active = active_layer_uuid_;
                    project_ = std::move(candidate);
                    loadActiveConfiguration();
                    if (document_) document_->project = project_;
                    recordProjectStateChange(tr("Remote: %1").arg(found->label), std::move(before), active);
                    refreshAll();
                    live_workspace_->refreshProjectSnapshot();
                    schedulePreview();
                }
            }
        } else if (action == "live") {
            if (!command.value("value").isBool()) error = tr("Expected an on/off value.");
            else live_workspace_->setLiveActive(command.value("value").toBool());
        } else if (action == "playback") {
            if (!command.value("value").isBool()) error = tr("Expected an on/off value.");
            else if (command.value("value").toBool() != playback_timer_->isActive()) togglePlayback();
        } else if (action == "undo") undo_stack_->undo();
        else if (action == "redo") undo_stack_->redo();
        else error = tr("Unsupported remote command.");
        remote_bridge_->reply(token, {{"ok", error.isEmpty()}, {"error", error}, {"revision", QString::number(document_revision_)}});
    });
}

bool MainWindow::setRemoteBackground(bool background) {
    if (!remote_bridge_) return false;
    if (background == remote_bridge_->background()) return true;
    if (background && !QSystemTrayIcon::isSystemTrayAvailable()) return false;
    if (!remote_tray_) {
        remote_tray_ = new QSystemTrayIcon(windowIcon().isNull() ? style()->standardIcon(QStyle::SP_ComputerIcon) : windowIcon(), this);
        remote_tray_->setToolTip(tr("Procedural Visualizer Tool"));
        auto* menu = new QMenu(this);
        menu->addAction(tr("Show PVT"), this, [this] { setRemoteBackground(false); });
        menu->addAction(tr("Networking & Remotes…"), this, [this] { setRemoteBackground(false); remote_bridge_->showManager(this); });
        menu->addAction(tr("Quit PVT…"), this, [this] {
            setRemoteBackground(false);
            remote_quit_ = true;
            close();
            remote_quit_ = false;
        });
        remote_tray_->setContextMenu(menu);
        connect(remote_tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick) setRemoteBackground(false);
        });
    }
    remote_bridge_->setBackground(background);
    live_workspace_->setBackgroundOutput(background);
    if (background) {
        remote_tray_->show();
        remote_started_presentation_ = !live_workspace_->isRealtimeOutputActive();
        if (remote_started_presentation_) live_workspace_->setPresentationActive(true);
        if (live_popout_window_) live_popout_window_->hide();
        hide();
    } else {
        if (remote_started_presentation_) live_workspace_->setPresentationActive(false);
        remote_started_presentation_ = false;
        showNormal(); raise(); activateWindow();
        remote_tray_->hide();
    }
    return true;
}

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include "stage_output_window.h"

// Opt-in integration smoke; launched only with --remote-smoke-test and an
// explicit test Python. Uses isolated settings and ephemeral pairing identities.
bool MainWindow::runRemoteSmokeChecks(QString* error) {
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    const auto python = qEnvironmentVariable("PVT_REMOTE_TEST_PYTHON");
    if (python.isEmpty()) return fail(QStringLiteral("Set PVT_REMOTE_TEST_PYTHON to an interpreter with pvt-remote installed."));
    QTemporaryDir temporary;
    if (!temporary.isValid()) return fail(QStringLiteral("Cannot create temporary remote test directory."));
    QProcess fixture;
    fixture.start(python, {"-c", QStringLiteral(
        "import sys,json\n"
        "from pvt_remote.host import Host\n"
        "from pvt_remote.protocol import new_identity,save_private\n"
        "h=Host(sys.argv[1])\n"
        "r=new_identity('pvtremote','control')['public']\n"
        "d=new_identity('pvtremote','display')['public']\n"
        "h.config.update(remotes=[r,d],active_control=r['id'],port=49738)\n"
        "save_private(h.config_path,h.config)\n"
        "print(json.dumps([r,d]))\n"), temporary.path()});
    if (!fixture.waitForFinished(15000) || fixture.exitCode() != 0)
        return fail(QString::fromUtf8(fixture.readAllStandardError()));
    const auto profiles = QJsonDocument::fromJson(fixture.readAllStandardOutput()).array();
    if (profiles.size() != 2) return fail(QStringLiteral("Pairing fixture was not created."));
    QSettings().setValue("remotes/python", python);
    QSettings().setValue("remotes/directory", temporary.path());
    bool ready = false;
    QObject guard;
    connect(remote_bridge_, &RemoteBridge::configurationChanged, &guard, [&] { ready = true; });
    const auto spin = [](const std::function<bool()>& condition) {
        QElapsedTimer timeout; timeout.start();
        while (!condition() && timeout.elapsed() < 10000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(2);
        }
        return condition();
    };
    remote_bridge_->start();
    if (!spin([&] { return ready; })) return fail(QStringLiteral("Remote worker did not start."));
    if (remote_bridge_->enabled()) return fail(QStringLiteral("Networking started without opt-in."));
    // Apply while a new worker is still starting, with the existing pairing
    // draft retained from the previous process.
    remote_bridge_->stop();
    bool manager_applied = false;
    QTimer::singleShot(0, &guard, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* enable = dialog->findChild<QCheckBox*>(QStringLiteral("remoteNetworkingEnabled"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>(QStringLiteral("remoteManagerButtons"));
        auto* label = dialog->findChild<QLineEdit*>(QStringLiteral("remoteHostName"));
        if (!enable || !buttons || !label) { dialog->reject(); return; }
        enable->setChecked(true);
        label->setText(QStringLiteral("Applied during startup"));
        buttons->button(QDialogButtonBox::Apply)->click();
        manager_applied = true;
        dialog->reject();
    });
    remote_bridge_->showManager(this);
    if (!manager_applied || !spin([&] { return remote_bridge_->enabled(); }))
        return fail(QStringLiteral("Remote Manager could not enable networking."));
    QFile saved_config(temporary.path() + "/remotes.json");
    if (!saved_config.open(QIODevice::ReadOnly)
        || QJsonDocument::fromJson(saved_config.readAll()).object().value("label") != "Applied during startup")
        return fail(QStringLiteral("Remote Manager lost settings applied during startup."));
    const auto controller = profiles[0].toObject().value("id").toString();
    const auto display = profiles[1].toObject().value("id").toString();
    if (!remote_bridge_->authorized(controller, "set") || remote_bridge_->authorized(display, "set")
        || !remote_bridge_->authorized(display, "background")) return fail(QStringLiteral("Remote role authorization failed."));
    const double original = project_.canvas.fps;
    const double changed = original == 48.0 ? 30.0 : 48.0;
    const auto set = [&](const QString& remote, double value, const QString& revision) {
        emit remote_bridge_->commandRequested(QStringLiteral("test-token"), remote,
            {{"action", "set"}, {"path", "project.fps"}, {"value", value}, {"revision", revision}});
    };
    set(display, changed, QString::number(document_revision_));
    if (project_.canvas.fps != original) return fail(QStringLiteral("Display remote changed a parameter."));
    set(controller, changed, QString::number(document_revision_));
    if (project_.canvas.fps != changed) return fail(QStringLiteral("Controller edit did not reach the authored project."));
    undo_stack_->undo();
    if (project_.canvas.fps != original) return fail(QStringLiteral("Remote edit did not undo."));
    undo_stack_->redo();
    if (project_.canvas.fps != changed) return fail(QStringLiteral("Remote edit did not redo."));
    set(controller, original, QStringLiteral("stale-revision"));
    if (project_.canvas.fps != changed) return fail(QStringLiteral("Stale remote edit was accepted."));
    auto document = pvt::default_project_document();
    document.project = project_;
    std::string persistence_error;
    const auto path = (temporary.path() + "/remote-edit.zip").toStdString();
    if (!pvt::save_project_document(document, path, nullptr, &persistence_error)) return fail(QString::fromStdString(persistence_error));
    pvt::ProjectDocument loaded;
    if (!pvt::load_project_document(path, loaded, &persistence_error)
        || loaded.project.canvas.fps != changed) return fail(QStringLiteral("Remote edit did not survive save/load."));
    int delivered = 0;
    connect(live_workspace_, &LiveWorkspace::remotePresentationFrame, &guard, [&](const QImage& image) { if (!image.isNull()) ++delivered; });
    live_workspace_->setBackgroundOutput(true);
    live_workspace_->setPresentationActive(true);
    live_workspace_->requestRealtimeFrame();
    if (!spin([&] { return delivered > 0; })) return fail(QStringLiteral("Hidden output did not deliver a frame."));
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<StageOutputWindow*>(widget) && widget->isVisible())
            return fail(QStringLiteral("Background renderer exposed its stage window."));
    }
    live_workspace_->setBackgroundOutput(false);
    live_workspace_->setPresentationActive(false);
    remote_bridge_->selectControl(0);
    if (!spin([&] { return remote_bridge_->enabled() && remote_bridge_->activeControlSlot() == 0; }))
        return fail(QStringLiteral("Controller handoff did not persist."));
    if (remote_bridge_->authorized(controller, "set")) return fail(QStringLiteral("Previous controller retained permission."));
    remote_bridge_->selectControl(9999);
    if (remote_bridge_->activeControlSlot() != 0) return fail(QStringLiteral("MIDI selected an unknown profile."));
    remote_bridge_->stop();
    return true;
}
