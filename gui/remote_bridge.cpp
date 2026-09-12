#include "remote_bridge.h"
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextEdit>
#include <QVBoxLayout>
#include <algorithm>

RemoteBridge::RemoteBridge(QObject* parent) : QObject(parent) {
    connect(&process_, &QProcess::readyReadStandardOutput, this, &RemoteBridge::receive);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto error = QString::fromUtf8(process_.readAllStandardError()).right(4000);
        if (!error.trimmed().isEmpty()) emit statusChanged(error);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this] {
        enabled_ = ready_ = false;
        emit statusChanged(tr("Remote worker could not run: %1").arg(process_.errorString()));
        emit configurationChanged();
    });
    connect(&process_, &QProcess::finished, this, [this] {
        enabled_ = ready_ = false;
        state_timer_.stop();
        emit configurationChanged();
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
    const auto runtime = QDir::homePath() + QStringLiteral("/.local/share/pvt-remotes/venv/")
#ifdef Q_OS_WIN
        + QStringLiteral("Scripts/python.exe");
#else
        + QStringLiteral("bin/python");
#endif
    const auto python = QSettings().value("remotes/python", QFile::exists(runtime) ? runtime : QStringLiteral("python3")).toString();
    process_.setProgram(python);
    process_.setArguments({"-I", "-m", "pvt_remote.host", "--directory",
        QSettings().value("remotes/directory", QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/remotes").toString()});
    process_.start();
}
void RemoteBridge::stop() {
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
        if (event == "ready" || event == "configured") {
            ready_ = true;
            config_ = object.value("config").toObject();
            profile_ = object.value("profile").toObject();
            enabled_ = object.value("enabled").toBool();
            emit configurationChanged();
            if (event == "ready" && requested_enabled_) send({{"op", "enable"}, {"enabled", true}});
            state_timer_.start();
            emit statusChanged(enabled_ ? tr("Networking & Remotes enabled") : tr("Networking & Remotes disabled"));
        } else if (event == "command") {
            emit commandRequested(object.value("token").toString(), object.value("remote").toString(), object.value("command").toObject());
        } else if (event == "status") {
            if (object.contains("enabled")) enabled_ = object.value("enabled").toBool();
            emit statusChanged(object.value("error").toString());
            emit configurationChanged();
        }
    }
}
void RemoteBridge::configure(const QJsonObject& config, bool enabled) {
    if (!ready_) return;
    // Deny commands immediately, including commands queued before revocation.
    enabled_ = false;
    requested_enabled_ = enabled;
    send({{"op", "configure"}, {"config", config}, {"enabled", enabled}});
    QSettings().setValue("remotes/enabled", enabled);
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
    image.scaled(QSize(1920,1080), Qt::KeepAspectRatio, Qt::FastTransformation).save(&buffer, "JPEG", 85);
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
    dialog.resize(700, 680);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* python = new QLineEdit(QSettings().value("remotes/python", process_.program().isEmpty() ? QStringLiteral("python3") : process_.program()).toString());
    auto* restart = new QPushButton(tr("Start / restart worker"));
    form->addRow(tr("Python with pvt-remote installed"), python);
    form->addRow(restart);
    auto* enable = new QCheckBox(tr("Enable Networking & Remotes"));
    enable->setObjectName(QStringLiteral("remoteNetworkingEnabled"));
    auto* lan = new QCheckBox(tr("Allow paired remotes on the local network (mDNS)"));
    auto* close = new QCheckBox(tr("Close window to system tray / menu bar"));
    close->setChecked(minimizeOnClose());
    auto* label = new QLineEdit;
    auto* port = new QSpinBox;
    port->setRange(1024,65535);
    auto* relay = new QLineEdit;
    relay->setPlaceholderText(tr("Optional wss:// signaling server"));
    auto* ice = new QTextEdit;
    ice->setMaximumHeight(75);
    ice->setPlaceholderText(tr("ICE servers as JSON; [] keeps LAN connections offline"));
    form->addRow(enable);
    form->addRow(lan);
    form->addRow(close);
    form->addRow(tr("Host name"), label);
    form->addRow(tr("Local signaling port"), port);
    form->addRow(tr("Remote signaling URL"), relay);
    form->addRow(tr("STUN / TURN servers"), ice);
    layout->addLayout(form);
    auto* list = new QListWidget;
    layout->addWidget(new QLabel(tr("Paired remotes — public identities only")));
    layout->addWidget(list);
    auto* active = new QComboBox;
    form->addRow(tr("Active Control Remote"), active);
    auto* row = new QHBoxLayout;
    auto* import = new QPushButton(tr("Import .pvtremote…"));
    auto* remove = new QPushButton(tr("Remove remote"));
    auto* export_host = new QPushButton(tr("Export .pvthost…"));
    row->addWidget(import); row->addWidget(remove); row->addWidget(export_host);
    layout->addLayout(row);
    auto* background = new QPushButton(tr("Toggle background mode"));
    layout->addWidget(background);
    auto* status = new QLabel(tr("Install the optional transport: python3 -m pip install /path/to/PVT/remote"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    buttons->setObjectName(QStringLiteral("remoteManagerButtons"));
    layout->addWidget(buttons);
    QJsonArray profiles;
    const auto reload = [&] {
        enable->setChecked(enabled_); lan->setChecked(config_.value("lan").toBool());
        label->setText(config_.value("label").toString("PVT host"));
        port->setValue(config_.value("port").toInt(49731));
        relay->setText(config_.value("signaling_url").toString());
        ice->setPlainText(QString::fromUtf8(QJsonDocument(config_.value("ice_servers").toArray()).toJson()));
        profiles = config_.value("remotes").toArray();
        list->clear(); active->clear(); active->addItem(tr("None"), "");
        for (const auto& item : profiles) {
            const auto p = item.toObject();
            list->addItem(p.value("label").toString() + " · " + p.value("role").toString() + " · " + p.value("id").toString());
            if (p.value("role") == "control") active->addItem(p.value("label").toString(), p.value("id").toString());
        }
        active->setCurrentIndex(std::max(0, active->findData(config_.value("active_control").toString())));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(true);
        export_host->setEnabled(ready_);
    };
    connect(this, &RemoteBridge::configurationChanged, &dialog, reload);
    connect(this, &RemoteBridge::statusChanged, status, &QLabel::setText);
    connect(restart, &QPushButton::clicked, &dialog, [&] {
        QSettings().setValue("remotes/python", python->text().trimmed());
        stop(); start();
    });
    connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("Import remote"), {}, tr("PVT remote (*.pvtremote)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) { status->setText(tr("Cannot read pairing file (16 KiB maximum).")); return; }
        const auto p = QJsonDocument::fromJson(file.readAll()).object();
        if (p.value("type") != "pvtremote" || p.value("version").toInt() != 1 || profiles.size() >= 64) { status->setText(tr("Invalid pairing file or remote limit reached.")); return; }
        for (const auto& existing : profiles) if (existing.toObject().value("id") == p.value("id")) { status->setText(tr("Remove the existing identity before replacing its keys.")); return; }
        profiles.append(p);
        list->addItem(p.value("label").toString() + " · " + p.value("role").toString());
        if (p.value("role") == "control") active->addItem(p.value("label").toString(), p.value("id").toString());
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const int index = list->currentRow();
        if (index < 0) return;
        const auto id = profiles[index].toObject().value("id").toString();
        profiles.removeAt(index); delete list->takeItem(index);
        const int choice = active->findData(id);
        if (choice >= 0) active->removeItem(choice);
    });
    connect(export_host, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getSaveFileName(&dialog, tr("Export host"), "PVT.pvthost", tr("PVT host (*.pvthost)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        file.write(QJsonDocument(profile_).toJson());
        if (!file.commit()) status->setText(file.errorString());
        else status->setText(tr("Host profile exported. Apply networking changes before exporting a new profile."));
    });
    connect(background, &QPushButton::clicked, &dialog, [&] {
        dialog.accept(); emit backgroundRequested(!background_);
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        QJsonParseError error;
        const auto servers = QJsonDocument::fromJson(ice->toPlainText().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !servers.isArray()) { status->setText(tr("ICE servers must be a JSON array.")); return; }
        QSettings().setValue("remotes/python", python->text().trimmed());
        QSettings().setValue("remotes/minimizeOnClose", close->isChecked());
        if (!ready_) { start(); return; }
        configure({{"remotes", profiles}, {"active_control", active->currentData().toString()},
                   {"label", label->text()}, {"lan", lan->isChecked()}, {"port", port->value()},
                   {"signaling_url", relay->text().trimmed()}, {"ice_servers", servers.array()}}, enable->isChecked());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    reload();
    if (!ready_) start();
    dialog.exec();
}
