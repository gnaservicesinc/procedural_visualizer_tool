#include "remote_bridge.h"
#include "remote_firewall_support.h"
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>
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
#include <QLineEdit>
#include <QFrame>
#include <QTableWidget>
#include <QHeaderView>
#include <memory>
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
        connections_ = {}; emit connectionsChanged();
        emit statusChanged(tr("Remotes are temporarily unavailable. PVT will try again automatically."));
        if (requested_enabled_) restart_timer_.start();
        emit configurationChanged();
    });
    connect(&process_, &QProcess::finished, this, [this] {
        enabled_ = ready_ = false;
        connections_ = {}; emit connectionsChanged();
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
RemoteBridge::~RemoteBridge() { blockSignals(true); stop(); }
void RemoteBridge::setStateProvider(std::function<QJsonObject()> provider) { state_provider_ = std::move(provider); }
bool RemoteBridge::minimizeOnClose() const { return QSettings().value("remotes/minimizeOnClose", false).toBool(); }
QString RemoteBridge::workerExecutablePath() const {
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
#ifdef Q_OS_MACOS
        + "/../Resources/pvt-remote/pvt-remote"
#else
        + "/pvt-remote/pvt-remote"
#endif
#ifdef Q_OS_WIN
        + ".exe"
#endif
        );
}
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
        const auto executable = workerExecutablePath();
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
    connections_ = {}; emit connectionsChanged();
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
                emit statusChanged(object.value("error").toString(tr("Remote settings could not be saved. Check the pairing file and address ranges.")));
            } else emit statusChanged(enabled_ ? tr("Remotes enabled") : tr("Remotes disabled"));
        } else if (event == "connections") {
            connections_ = object.value("connections").toArray();
            emit connectionsChanged();
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
    if (!enabled_ || config_.value("paused_remotes").toArray().contains(remote)) return false;
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
    dialog.setWindowTitle(tr("Remotes"));
    dialog.resize(820, 720);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(createManager(&dialog));
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("remoteManagerButtons"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QWidget* RemoteBridge::createManager(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setAlignment(Qt::AlignTop);
    auto* store_intro = new QLabel(tr("Get the browser extensions from the Chrome Web Store, then pair them below. Chrome handles installation and approved updates."));
    store_intro->setWordWrap(true);
    layout->addWidget(store_intro);
    auto* store_row = new QHBoxLayout;
    auto* get_control = new QPushButton(tr("Get Remote Control for Chrome"));
    get_control->setObjectName(QStringLiteral("remoteControlStore"));
    auto* get_display = new QPushButton(tr("Get Remote Display for Chrome"));
    get_display->setObjectName(QStringLiteral("remoteDisplayStore"));
    store_row->addWidget(get_control);
    store_row->addWidget(get_display);
    layout->addLayout(store_row);
    const auto open_store = [=](const QString& url) {
        if (!QDesktopServices::openUrl(QUrl(url)))
            store_intro->setText(tr("Could not open the browser. Open this address in Chrome: %1").arg(url));
    };
    connect(get_control, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://chromewebstore.google.com/detail/pvt-remote-control/paachfdeekmbojpfifnaadedhogpgcde"));
    });
    connect(get_display, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://chromewebstore.google.com/detail/pvt-remote-display/ebehogflkicknbgeimbmhfeaagjfgfda"));
    });
    auto* form = new QFormLayout;
    auto* enable = new QCheckBox(tr("Enable Remotes"));
    enable->setObjectName(QStringLiteral("remoteNetworkingEnabled"));
    auto* close = new QCheckBox(tr("Close window to system tray / menu bar"));
    close->setObjectName(QStringLiteral("remoteMinimizeOnClose"));
    close->setChecked(minimizeOnClose());
    form->addRow(enable);
    form->addRow(close);
    auto* instructions = new QLabel(tr("Pair once: save the pairing file from each Remote Display or Remote Control, then add all of those files here. Save PVT’s pairing file and open it in each remote. Paired remotes reconnect automatically while PVT is running."));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    layout->addLayout(form);
    auto* list = new QListWidget;
    list->setMinimumHeight(90); list->setMaximumHeight(150);
    layout->addWidget(new QLabel(tr("Paired devices")));
    layout->addWidget(list);
    auto* active = new QComboBox;
    form->addRow(tr("Remote allowed to control PVT"), active);
    auto* scope = new QComboBox;
    scope->setObjectName("remoteAddressScope");
    scope->addItem(tr("This computer and nearby networks"), "subnet");
    scope->addItem(tr("Any private network"), "private");
    scope->addItem(tr("Anywhere"), "any");
    scope->addItem(tr("Only the addresses or ranges below"), "custom");
    form->addRow(tr("Allow Connections With Remotes On"), scope);
    auto* networks = new QLineEdit;
    networks->setObjectName("remoteCustomNetworks");
    networks->setPlaceholderText(tr("For example: 192.168.1.20, 10.0.0.10-10.0.0.40"));
    networks->setToolTip(tr("Enter individual addresses or a start-to-end range, separated by spaces or commas. A network administrator can also enter a whole network address."));
    form->addRow(tr("Addresses or ranges"), networks);

    auto* firewall_help = new QFrame;
    firewall_help->setObjectName(QStringLiteral("remoteFirewallHelp"));
    firewall_help->setFrameShape(QFrame::StyledPanel);
    auto* firewall_layout = new QVBoxLayout(firewall_help);
    auto* firewall_text = new QLabel(tr("These remotes may be on another network. A firewall or router between the computers may need permission before they can connect. PVT can save the exact instructions for the person who manages the network, or a setup file for this computer that asks for administrator approval when it is run."));
    firewall_text->setWordWrap(true);
    firewall_layout->addWidget(firewall_text);
    auto* firewall_buttons = new QHBoxLayout;
    auto* save_firewall = new QPushButton(tr("Save firewall setup…"));
    save_firewall->setObjectName(QStringLiteral("remoteSaveFirewallSetup"));
    auto* save_admin = new QPushButton(tr("Save network-admin instructions…"));
    save_admin->setObjectName(QStringLiteral("remoteSaveNetworkInstructions"));
    firewall_buttons->addWidget(save_firewall);
    firewall_buttons->addWidget(save_admin);
    firewall_layout->addLayout(firewall_buttons);
    layout->addWidget(firewall_help);

    auto* tracker = new QPushButton(tr("Connection details for support…"));
    layout->addWidget(tracker);
    connect(tracker, &QPushButton::clicked, page, [=] { showTracker(page->window()); });
    auto* resume = new QPushButton(tr("Reconnect selected remote"));
    layout->addWidget(resume);
    auto* row = new QHBoxLayout;
    auto* import = new QPushButton(tr("Add Remote pairing files…"));
    import->setObjectName(QStringLiteral("remoteImport"));
    auto* remove = new QPushButton(tr("Remove selected remote"));
    remove->setObjectName(QStringLiteral("remoteRemove"));
    auto* export_host = new QPushButton(tr("Save PVT pairing file…"));
    export_host->setObjectName(QStringLiteral("remoteExportHost"));
    row->addWidget(import); row->addWidget(remove); row->addWidget(export_host);
    layout->addLayout(row);
    auto* background = new QPushButton(tr("Run Remotes in the background"));
    layout->addWidget(background);
    auto* status = new QLabel(tr("Getting Remotes ready…"));
    status->setWordWrap(true);
    layout->addWidget(status);
    struct Draft {
        QJsonArray profiles;
        QSet<QString> pending_fields;
        QJsonObject original_config;
        bool original_enabled = false;
        bool original_close = false;
        bool original_background = false;
        bool original_captured = false;
        bool awaiting = false;
        bool changed = false;
        bool loading = false;
    };
    auto draft = std::make_shared<Draft>();
    draft->original_enabled = requested_enabled_;
    draft->original_close = minimizeOnClose();
    draft->original_background = background_;

    const auto refresh_profiles = [=](const QString& selected,
                                      const QString& active_id) {
        list->clear();
        active->clear();
        active->addItem(tr("None"), "");
        for (const auto& item : draft->profiles) {
            const auto profile = item.toObject();
            const QString role = profile.value("role") == "control"
                ? tr("Control") : tr("Display");
            QString name = profile.value("label").toString() + tr(" — ") + role;
            if (config_.value("paused_remotes").toArray().contains(profile.value("id")))
                name += tr(" — Paused");
            list->addItem(name);
            list->item(list->count() - 1)->setData(
                Qt::UserRole, profile.value("id").toString());
            if (profile.value("role") == "control") {
                active->addItem(profile.value("label").toString(),
                                profile.value("id").toString());
            }
        }
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole) == selected) list->setCurrentRow(i);
        }
        active->setCurrentIndex(std::max(0, active->findData(active_id)));
    };

    const auto update_visibility = [=] {
        const bool custom = scope->currentData() == "custom";
        form->setRowVisible(networks, custom);
        firewall_help->setVisible(scope->currentData() != "subnet");
        networks->setEnabled(config_loaded_ && !configuring_ && custom);
    };

    const auto reload = [=] {
        if (config_loaded_ && !draft->original_captured) {
            draft->original_config = config_;
            draft->original_captured = true;
        }
        if (draft->awaiting && !configuring_) {
            draft->awaiting = false;
            draft->pending_fields.clear();
        }
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        import->setEnabled(config_loaded_ && !configuring_);
        remove->setEnabled(config_loaded_ && !configuring_);
        active->setEnabled(config_loaded_ && !configuring_);
        enable->setEnabled(config_loaded_ && !configuring_);
        scope->setEnabled(config_loaded_ && !configuring_);
        resume->setEnabled(config_loaded_ && !configuring_);
        save_firewall->setEnabled(ready_ && !configuring_);
        save_admin->setEnabled(ready_ && !configuring_);
        // Worker status and startup must not replace an edit awaiting validation.
        draft->loading = true;
        if (!draft->pending_fields.contains("enabled")) enable->setChecked(requested_enabled_);
        if (!draft->pending_fields.contains("remotes")) draft->profiles = config_.value("remotes").toArray();
        const auto active_id = draft->pending_fields.contains("active_control")
            ? active->currentData().toString() : config_.value("active_control").toString();
        if (!draft->pending_fields.contains("address_scope")) {
            scope->setCurrentIndex(std::max(0, scope->findData(
                config_.value("address_scope").toString("subnet"))));
        }
        if (!draft->pending_fields.contains("custom_networks"))
            networks->setText(config_.value("custom_networks").toString());
        const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        refresh_profiles(selected, active_id);
        update_visibility();
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        draft->loading = false;
    };
    connect(this, &RemoteBridge::configurationChanged, page, reload);
    connect(this, &RemoteBridge::statusChanged, status, &QLabel::setText);

    const auto apply_current = [=](const QSet<QString>& fields) {
        if (draft->loading || !config_loaded_ || configuring_) return;
        draft->pending_fields.unite(fields);
        draft->changed = true;
        if (scope->currentData() == "custom" && networks->text().trimmed().isEmpty()) {
            save_firewall->setEnabled(false);
            save_admin->setEnabled(false);
            status->setText(tr("Enter at least one address or range."));
            return;
        }
        draft->awaiting = true;
        configure({{"remotes", draft->profiles},
                   {"active_control", active->currentData().toString()},
                   {"address_scope", scope->currentData().toString()},
                   {"custom_networks", networks->text()}},
                  enable->isChecked());
    };

    connect(import, &QPushButton::clicked, page, [=] {
        const auto paths = QFileDialog::getOpenFileNames(
            page, tr("Add Remote pairing files"), {},
            tr("PVT Remote pairing files (*.pvtremote)"));
        if (paths.isEmpty()) return;
        QSet<QString> identities;
        for (const auto& existing : draft->profiles)
            identities.insert(existing.toObject().value("id").toString());
        int added = 0;
        int skipped = 0;
        QString active_id = active->currentData().toString();
        for (const auto& path : paths) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) {
                ++skipped;
                continue;
            }
            const auto profile = QJsonDocument::fromJson(file.readAll()).object();
            const QString id = profile.value("id").toString();
            const QString role = profile.value("role").toString();
            if (profile.value("type") != "pvtremote"
                || profile.value("version").toInt() != 1
                || id.isEmpty() || (role != "control" && role != "display")
                || identities.contains(id) || draft->profiles.size() >= 64) {
                ++skipped;
                continue;
            }
            identities.insert(id);
            draft->profiles.append(profile);
            if (active_id.isEmpty() && role == "control") active_id = id;
            ++added;
        }
        if (added == 0) {
            status->setText(skipped == 1
                ? tr("That pairing file could not be added, or it is already paired.")
                : tr("None of the selected pairing files could be added, or they are already paired."));
            return;
        }
        draft->loading = true;
        enable->setChecked(true);
        refresh_profiles(QString(), active_id);
        draft->loading = false;
        apply_current({"remotes", "active_control", "enabled"});
        status->setText(skipped == 0
            ? tr("Added %n Remote(s).", nullptr, added)
            : tr("Added %1 Remote(s); skipped %2 file(s) that were invalid, already paired, or beyond the 64-remote limit.")
                  .arg(added).arg(skipped));
    });
    connect(remove, &QPushButton::clicked, page, [=] {
        const int index = list->currentRow();
        if (index < 0) return;
        const auto id = draft->profiles[index].toObject().value("id").toString();
        QString active_id = active->currentData().toString();
        if (active_id == id) active_id.clear();
        draft->profiles.removeAt(index);
        draft->loading = true;
        refresh_profiles(QString(), active_id);
        draft->loading = false;
        apply_current({"remotes", "active_control"});
    });
    connect(export_host, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getSaveFileName(
            page, tr("Save PVT pairing file"), QStringLiteral("PVT.pvthost"),
            tr("PVT pairing file (*.pvthost)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        file.write(QJsonDocument(profile_).toJson());
        if (!file.commit()) status->setText(file.errorString());
        else status->setText(tr("PVT pairing file saved. Open it in each Remote Display or Remote Control to finish setup."));
    });
    connect(background, &QPushButton::clicked, page, [=] {
        draft->changed = true;
        emit backgroundRequested(!background_);
    });
    connect(resume, &QPushButton::clicked, page, [=] {
        if (auto* item = list->currentItem()) {
            draft->changed = true;
            setPaused(item->data(Qt::UserRole).toString(), false);
        }
    });

    connect(save_admin, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getSaveFileName(
            page, tr("Save network-admin instructions"),
            QStringLiteral("PVT Remotes network instructions.txt"),
            tr("Text file (*.txt)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            status->setText(file.errorString());
            return;
        }
        file.write(pvt::remote_support::networkAdminInstructions(config_, profile_).toUtf8());
        status->setText(file.commit()
            ? tr("Network instructions saved.") : file.errorString());
    });

    connect(save_firewall, &QPushButton::clicked, page, [=] {
#ifdef Q_OS_WIN
        constexpr auto platform = pvt::remote_support::FirewallPlatform::Windows;
#elif defined(Q_OS_MACOS)
        constexpr auto platform = pvt::remote_support::FirewallPlatform::MacOS;
#else
        constexpr auto platform = pvt::remote_support::FirewallPlatform::Ubuntu;
#endif
        const auto setup = pvt::remote_support::localFirewallSetup(
            platform, config_, profile_, workerExecutablePath());
        const auto path = QFileDialog::getSaveFileName(
            page, tr("Save firewall setup"), setup.suggested_name,
            setup.dialog_filter);
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            status->setText(file.errorString());
            return;
        }
        file.write(setup.contents.toUtf8());
        if (!file.commit()) {
            status->setText(file.errorString());
            return;
        }
        if (setup.executable) {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                | QFileDevice::ReadOther | QFileDevice::ExeOther);
        }
        status->setText(tr("Firewall setup saved. Run it on the PVT computer; it will ask for administrator approval."));
    });

    connect(enable, &QCheckBox::toggled, page, [=] {
        if (!draft->loading) apply_current({"enabled"});
    });
    connect(active, &QComboBox::currentIndexChanged, page, [=] {
        if (!draft->loading) apply_current({"active_control"});
    });
    connect(scope, &QComboBox::currentIndexChanged, page, [=] {
        update_visibility();
        if (!draft->loading) apply_current({"address_scope"});
    });
    connect(networks, &QLineEdit::textEdited, page, [=] {
        if (!draft->loading) {
            draft->pending_fields.insert("custom_networks");
            draft->changed = true;
            save_firewall->setEnabled(false);
            save_admin->setEnabled(false);
        }
    });
    connect(networks, &QLineEdit::editingFinished, page, [=] {
        if (!draft->loading) apply_current({"custom_networks", "address_scope"});
    });
    connect(close, &QCheckBox::toggled, page, [=] {
        if (draft->loading) return;
        draft->changed = true;
        QSettings().setValue("remotes/minimizeOnClose", close->isChecked());
    });

    if (auto* dialog = qobject_cast<QDialog*>(parent)) {
        connect(dialog, &QDialog::rejected, page, [=] {
            if (!draft->changed) return;
            QSettings().setValue("remotes/minimizeOnClose", draft->original_close);
            if (draft->original_captured)
                configure(draft->original_config, draft->original_enabled);
            if (background_ != draft->original_background)
                emit backgroundRequested(draft->original_background);
        });
    }
    reload();
    if (!ready_) start();
    return page;
}

QString RemoteBridge::connectionSummary() const {
    QStringList names;
    for (const auto& value : connections_) {
        const auto row = value.toObject();
        names << tr("%1: %2").arg(row.value("role") == "control" ? tr("Control") : tr("Display"),
                                  row.value("label").toString());
    }
    return names.isEmpty() ? QString() : tr(" ● Remotes: %1").arg(names.join(", "));
}

void RemoteBridge::showTracker(QWidget* parent) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    if (auto* settings = qobject_cast<QDialog*>(parent)) {
        auto* owner = qobject_cast<QWidget*>(this->parent());
        connect(settings, &QDialog::finished, dialog, [dialog, owner] {
            dialog->setParent(owner, Qt::Window); dialog->show();
        });
    }
    dialog->setWindowTitle(tr("Remote connection details"));
    dialog->resize(1100, 420);
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(tr("Use these details when troubleshooting with your network administrator or PVT support. A dash means no measurement is available. Pausing a remote disconnects it until you reconnect it from Remotes."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* table = new QTableWidget(0, 10);
    table->setHorizontalHeaderLabels({tr("Remote"), tr("What it can do"), tr("Browser and system"), tr("First connection"), tr("Live stream path"), tr("Status"), tr("Time connected"), tr("Response time"), tr("Interruptions"), tr("Data use")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(table);
    auto* pause = new QPushButton(tr("Disconnect and pause selected device")); layout->addWidget(pause);
    const auto refresh = [this, table] {
        const auto selected = table->currentRow() >= 0 ? table->item(table->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
        table->setRowCount(static_cast<int>(connections_.size()));
        for (int i = 0; i < connections_.size(); ++i) {
            const auto row = connections_[i].toObject();
            const auto client = row.value("client").toObject();
            const auto metric = [&](const char* key) { return row.contains(key) ? QString::number(row.value(key).toDouble()) : QStringLiteral("—"); };
            QStringList values{row.value("label").toString(),
                row.value("active_control").toBool() ? tr("Control PVT") : tr("View PVT"),
                client.value("browser").toString() + " / " + client.value("version").toString() + " / " + client.value("platform").toString(),
                row.value("endpoint").toString(), row.value("media_endpoints").toString(), row.value("status").toString(),
                tr("%1 s").arg(row.value("seconds").toInt()), metric("rtt_ms"), metric("packets_lost"),
                metric("kbps") + " / " + metric("bytes_sent") + " B / " + metric("bytes_received") + " B"};
            for (int j = 0; j < values.size(); ++j) {
                auto* item = new QTableWidgetItem(values[j]); item->setToolTip(values[j]);
                item->setData(Qt::UserRole, row.value("id").toString()); table->setItem(i, j, item);
            }
            if (row.value("id").toString() == selected) table->selectRow(i);
        }
    };
    connect(this, &RemoteBridge::connectionsChanged, dialog, refresh);
    connect(pause, &QPushButton::clicked, dialog, [this, table] {
        if (table->currentRow() >= 0) setPaused(table->item(table->currentRow(), 0)->data(Qt::UserRole).toString(), true);
    });
    refresh(); dialog->show();
}

void RemoteBridge::setPaused(const QString& remote, bool paused) {
    auto list = config_.value("paused_remotes").toArray();
    if (paused && !list.contains(remote)) list.append(remote);
    if (!paused) for (int i = static_cast<int>(list.size()) - 1; i >= 0; --i) if (list[i] == remote) list.removeAt(i);
    config_.insert("paused_remotes", list);
    // Revoke queued desktop commands before waiting for the worker to close sockets.
    emit configurationChanged();
    send({{"op", "pause"}, {"remote", remote}, {"paused", paused}});
}
