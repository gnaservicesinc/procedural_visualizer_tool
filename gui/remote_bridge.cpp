#include "remote_bridge.h"
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>

RemoteBridge::RemoteBridge(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &RemoteBridge::receive);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto error = QString::fromUtf8(process_.readAllStandardError()).right(4000);
        if (!error.trimmed().isEmpty()) qWarning().noquote() << "PVT Remotes:" << error;
    });
    connect(&process_, &QProcess::errorOccurred, this, [this] {
        enabled_ = ready_ = false;
        emit statusChanged(tr("Remotes are temporarily unavailable. PVT will try again automatically."));
        if (requested_enabled_) restart_timer_.start();
        emit configurationChanged();
    });
    connect(&process_, &QProcess::finished, this, [this] {
        enabled_ = ready_ = false;
        state_timer_.stop();
        if (requested_enabled_) restart_timer_.start();
        emit configurationChanged();
    });
    restart_timer_.setInterval(3000);
    restart_timer_.setSingleShot(true);
    connect(&restart_timer_, &QTimer::timeout, this, [this] {
        if (ready_ && requested_enabled_) send({{"op", "enable"}, {"enabled", true}});
        else start();
    });
    frame_timer_.setInterval(33);
    connect(&frame_timer_, &QTimer::timeout, this, &RemoteBridge::flushFrame);
    frame_timer_.start();
    state_timer_.setInterval(500);
    connect(&state_timer_, &QTimer::timeout, this, [this] {
        if (enabled_ && state_provider_) send({{"op", "state"}, {"state", state_provider_()}});
    });
    requested_enabled_ = QSettings().value("remotes/enabled", false).toBool();
    if (requested_enabled_) QTimer::singleShot(0, this, &RemoteBridge::start);
}
RemoteBridge::~RemoteBridge() { stop(); }
void RemoteBridge::setStateProvider(std::function<QJsonObject()> provider) { state_provider_ = std::move(provider); }
bool RemoteBridge::minimizeOnClose() const { return QSettings().value("remotes/minimizeOnClose", false).toBool(); }
void RemoteBridge::start() {
    if (process_.state() != QProcess::NotRunning) return;
    input_.clear();
    ready_ = false;
    restart_timer_.stop();
    const auto directory = QSettings().value("remotes/directory",
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/remotes").toString();
    // Only developer/test runs opt into an interpreter. Product builds always
    // use the bundled executable, independent of PATH and user installations.
    const auto test_python = qEnvironmentVariableIsSet("PVT_REMOTE_TEST_BUNDLED")
        ? QString() : qEnvironmentVariable("PVT_REMOTE_TEST_PYTHON");
    const auto test_worker = qEnvironmentVariable("PVT_REMOTE_TEST_WORKER");
    if (!test_python.isEmpty()) {
        process_.setProgram(test_python);
        process_.setArguments({"-I", "-m", "pvt_remote.host", "--directory", directory});
    } else {
        const auto executable = QCoreApplication::applicationDirPath() + "/pvt-remote/pvt-remote"
#ifdef Q_OS_WIN
            + ".exe"
#endif
            ;
        process_.setProgram(test_worker.isEmpty() ? executable : test_worker);
        process_.setArguments({"--directory", directory});
    }
    process_.start();
}
void RemoteBridge::stop() {
    restart_timer_.stop();
    const bool wanted = requested_enabled_;
    requested_enabled_ = false;
    enabled_ = ready_ = false;
    state_timer_.stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.write("{\"op\":\"shutdown\"}\n");
        process_.closeWriteChannel();
        if (!process_.waitForFinished(1500)) {
            process_.terminate();
            if (!process_.waitForFinished(500)) { process_.kill(); process_.waitForFinished(500); }
        }
    }
    requested_enabled_ = wanted;
}
void RemoteBridge::send(const QJsonObject& message) {
    if (ready_ && process_.state() == QProcess::Running && process_.bytesToWrite() < 4 * 1024 * 1024)
        process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}
void RemoteBridge::receive() {
    input_ += process_.readAllStandardOutput();
    if (input_.size() > 4 * 1024 * 1024) { stop(); emit statusChanged(tr("Remote worker exceeded the message limit.")); return; }
    while (input_.contains('\n')) {
        const auto end = input_.indexOf('\n');
        const auto object = QJsonDocument::fromJson(input_.left(end)).object();
        input_.remove(0, end + 1);
        const auto event = object.value("event").toString();
        if (event == "ready" || event == "configured" || event == "rejected") {
            if (event != "ready" || !pending_config_) configuring_ = false;
            ready_ = true;
            config_ = object.value("config").toObject();
            config_loaded_ = true;
            profile_ = object.value("profile").toObject();
            enabled_ = object.value("enabled").toBool();
            if (event == "ready" && pending_config_) {
                const auto pending = *pending_config_;
                configure(pending, requested_enabled_);
            } else if (event == "ready" && requested_enabled_) {
                send({{"op", "enable"}, {"enabled", true}});
            }
            emit configurationChanged();
            state_timer_.start();
            if (event == "rejected") {
                requested_enabled_ = enabled_;
                QSettings().setValue("remotes/enabled", enabled_);
                emit statusChanged(tr("Pairing changes could not be saved. Check the pairing file and try again."));
            } else emit statusChanged(enabled_ ? tr("Networking & Remotes enabled") : tr("Networking & Remotes disabled"));
        } else if (event == "command") {
            emit commandRequested(object.value("token").toString(), object.value("remote").toString(), object.value("command").toObject());
        } else if (event == "status") {
            if (object.contains("enabled")) enabled_ = object.value("enabled").toBool();
            if (!enabled_ && requested_enabled_) restart_timer_.start();
            if (object.contains("error") && !object.value("error").toString().isEmpty()) {
                qWarning().noquote() << "PVT Remotes:" << object.value("error").toString();
                emit statusChanged(tr("Waiting for a connection. PVT will reconnect automatically."));
            }
            emit configurationChanged();
        }
    }
    // Keep normal packet capacity for reuse, but release an exceptional large
    // worker burst once only a small partial line (or nothing) remains.
    if (input_.capacity() > 64 * 1024 && input_.size() < 64 * 1024) input_.squeeze();
}
void RemoteBridge::configure(const QJsonObject& config, bool enabled) {
    // Deny commands immediately, including commands queued before revocation.
    enabled_ = false;
    requested_enabled_ = enabled;
    configuring_ = true;
    emit configurationChanged();
    QSettings().setValue("remotes/enabled", enabled);
    if (!ready_) {
        pending_config_ = config;
        start();
        return;
    }
    send({{"op", "configure"}, {"config", config}, {"enabled", enabled}});
    pending_config_.reset();
}
bool RemoteBridge::authorized(const QString& remote, const QString& action) const {
    if (!enabled_) return false;
    for (const auto& item : config_.value("remotes").toArray()) {
        const auto p = item.toObject();
        if (p.value("id").toString() != remote) continue;
        if (action == "state" || action == "background") return true;
        return p.value("role") == "control" && config_.value("active_control").toString() == remote;
    }
    return false;
}
void RemoteBridge::reply(const QString& token, const QJsonObject& result) { send({{"op", "reply"}, {"token", token}, {"result", result}}); }
void RemoteBridge::sendFrame(const QImage& image) {
    if (enabled_ && !image.isNull()) pending_frame_ = image;
}
void RemoteBridge::flushFrame() {
    if (!enabled_) { pending_frame_ = {}; return; }
    if (pending_frame_.isNull() || process_.bytesToWrite() > 512 * 1024) return;
    const auto image = std::move(pending_frame_);
    pending_frame_ = {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    const auto output = image.width() > 1920 || image.height() > 1080
        ? image.scaled(QSize(1920,1080), Qt::KeepAspectRatio, Qt::FastTransformation) : image;
    output.save(&buffer, "JPEG", 85);
    send({{"op", "video"}, {"jpeg", QString::fromLatin1(bytes.toBase64())}});
}
void RemoteBridge::sendAudio(const QByteArray& pcm) {
    if (enabled_ && pcm.size() == 3840 && process_.bytesToWrite() < 512 * 1024)
        send({{"op", "audio"}, {"pcm", QString::fromLatin1(pcm.toBase64())}});
}
QStringList RemoteBridge::controlNames() const {
    QStringList names{tr("None")};
    for (const auto& item : config_.value("remotes").toArray())
        if (item.toObject().value("role") == "control") names << item.toObject().value("label").toString();
    return names;
}
int RemoteBridge::activeControlSlot() const {
    int slot = 0;
    for (const auto& item : config_.value("remotes").toArray()) {
        const auto p = item.toObject();
        if (p.value("role") != "control") continue;
        ++slot;
        if (p.value("id") == config_.value("active_control")) return slot;
    }
    return 0;
}
void RemoteBridge::selectControl(int slot) {
    if (!ready_ || slot < 0 || slot >= controlNames().size()) return;
    QString id;
    int index = 0;
    for (const auto& item : config_.value("remotes").toArray()) {
        const auto p = item.toObject();
        if (p.value("role") == "control" && ++index == slot) id = p.value("id").toString();
    }
    if (id == config_.value("active_control").toString()) return;
    auto updated = config_;
    updated.insert("active_control", id);
    configure(updated, requested_enabled_);
}
void RemoteBridge::showManager(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("Networking & Remotes"));
    dialog.resize(620, 480);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* enable = new QCheckBox(tr("Enable Networking & Remotes"));
    enable->setObjectName(QStringLiteral("remoteNetworkingEnabled"));
    auto* close = new QCheckBox(tr("Close window to system tray / menu bar"));
    close->setChecked(minimizeOnClose());
    form->addRow(enable);
    form->addRow(close);
    auto* instructions = new QLabel(tr("Pair once: save the pairing file from Remote Display or Remote Control and open it here. Then save PVT’s pairing file and open it in the remote. Paired devices reconnect automatically while PVT is running."));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    layout->addLayout(form);
    auto* list = new QListWidget;
    layout->addWidget(new QLabel(tr("Paired devices")));
    layout->addWidget(list);
    auto* active = new QComboBox;
    form->addRow(tr("Active Control Remote"), active);
    auto* row = new QHBoxLayout;
    auto* import = new QPushButton(tr("Import .pvtremote…"));
    auto* remove = new QPushButton(tr("Remove remote"));
    remove->setObjectName(QStringLiteral("remoteRemove"));
    auto* export_host = new QPushButton(tr("Export .pvthost…"));
    row->addWidget(import); row->addWidget(remove); row->addWidget(export_host);
    layout->addLayout(row);
    auto* background = new QPushButton(tr("Toggle background mode"));
    layout->addWidget(background);
    auto* status = new QLabel(tr("Preparing pairing…"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    buttons->setObjectName(QStringLiteral("remoteManagerButtons"));
    layout->addWidget(buttons);
    QJsonArray profiles;
    QSet<QString> edited_fields;
    bool loading = false;
    const auto reload = [&] {
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        import->setEnabled(config_loaded_ && !configuring_);
        remove->setEnabled(config_loaded_ && !configuring_);
        active->setEnabled(config_loaded_ && !configuring_);
        // Worker status and startup must not replace an in-progress draft.
        loading = true;
        if (!edited_fields.contains("enabled")) enable->setChecked(enabled_);
        if (!edited_fields.contains("remotes")) profiles = config_.value("remotes").toArray();
        const auto active_id = edited_fields.contains("active_control")
            ? active->currentData().toString() : config_.value("active_control").toString();
        list->clear(); active->clear(); active->addItem(tr("None"), "");
        for (const auto& item : profiles) {
            const auto p = item.toObject();
            list->addItem(p.value("label").toString() + " · " + p.value("role").toString());
            if (p.value("role") == "control") active->addItem(p.value("label").toString(), p.value("id").toString());
        }
        active->setCurrentIndex(std::max(0, active->findData(active_id)));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(!configuring_);
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        loading = false;
    };
    connect(this, &RemoteBridge::configurationChanged, &dialog, reload);
    connect(this, &RemoteBridge::statusChanged, status, &QLabel::setText);
    connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("Import remote"), {}, tr("PVT remote (*.pvtremote)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) { status->setText(tr("Cannot read pairing file (16 KiB maximum).")); return; }
        const auto p = QJsonDocument::fromJson(file.readAll()).object();
        if (p.value("type") != "pvtremote" || p.value("version").toInt() != 1 || profiles.size() >= 64) { status->setText(tr("Invalid pairing file or remote limit reached.")); return; }
        for (const auto& existing : profiles) if (existing.toObject().value("id") == p.value("id")) { status->setText(tr("Remove the existing identity before replacing its keys.")); return; }
        profiles.append(p);
        edited_fields.remove("remotes");
        list->addItem(p.value("label").toString() + " · " + p.value("role").toString());
        if (p.value("role") == "control") {
            active->addItem(p.value("label").toString(), p.value("id").toString());
            if (active->currentData().toString().isEmpty()) active->setCurrentIndex(active->count() - 1);
        }
        enable->setChecked(true);
        edited_fields.remove("enabled");
        edited_fields.remove("active_control");
        configure({{"remotes", profiles}, {"active_control", active->currentData().toString()}}, true);
        export_host->setEnabled(false);
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentRow();
        if (index < 0) return;
        edited_fields.remove("remotes");
        const auto id = profiles[index].toObject().value("id").toString();
        profiles.removeAt(index); delete list->takeItem(index);
        const int choice = active->findData(id);
        if (choice >= 0) active->removeItem(choice);
        edited_fields.remove("active_control");
        configure({{"remotes", profiles}, {"active_control", active->currentData().toString()}}, enable->isChecked());
    });
    connect(export_host, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, tr("Export host"), "PVT.pvthost", tr("PVT host (*.pvthost)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        file.write(QJsonDocument(profile_).toJson());
        if (!file.commit()) status->setText(file.errorString());
        else status->setText(tr("Pairing file saved. Open it in Remote Display or Remote Control to finish setup."));
    });
    connect(background, &QPushButton::clicked, &dialog, [&] {
        dialog.accept(); emit backgroundRequested(!background_);
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        QSettings().setValue("remotes/minimizeOnClose", close->isChecked());
        QJsonObject changes{{"remotes", profiles}, {"active_control", active->currentData().toString()}};
        // On first startup we haven't read the saved configuration yet. Send
        // only authored fields so defaults cannot erase unseen paired remotes
        // or network settings; the worker already merges configuration patches.
        if (!config_loaded_) {
            for (const auto& key : changes.keys())
                if (!edited_fields.contains(key)) changes.remove(key);
        }
        configure(changes, enable->isChecked());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    reload();
    const auto edited = [&](const QString& key) { if (!loading) edited_fields.insert(key); };
    connect(enable, &QCheckBox::toggled, &dialog, [&] { edited("enabled"); });
    connect(active, &QComboBox::currentIndexChanged, &dialog, [&] { edited("active_control"); });
    if (!ready_) start();
    dialog.exec();
}
