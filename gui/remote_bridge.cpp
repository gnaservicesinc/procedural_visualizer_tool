#include "remote_bridge.h"
#include "remote_firewall_support.h"
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QUrl>
#include <QVersionNumber>
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
#include <QLocale>
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
                emit statusChanged(object.value("error").toString(tr("Remote settings could not be saved. Check the Remote file (.pvtremote) and address ranges.")));
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
    if (!enabled_) return false;
    for (const auto& item : config_.value("remotes").toArray()) {
        const auto p = item.toObject();
        if (p.value("id").toString() != remote) continue;
        if (action == "state" || action == "background") return true;
        return p.value("role") == "control";
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
QStringList RemoteBridge::profileNames() const {
    QStringList names;
    for (const auto& item : config_.value("remotes").toArray())
        names << item.toObject().value("label").toString();
    return names;
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
    layout->addWidget(new QLabel(tr("Remote connection details")));
    layout->addWidget(createConnections(page));
    auto* setup = new QPushButton(tr("Manage remote files and network settings"));
    setup->setObjectName("remoteSetupToggle");
    setup->setCheckable(true);
    setup->setChecked(config_.value("remotes").toArray().isEmpty());
    layout->addWidget(setup);
    auto* setup_page = new QWidget(page);
    layout->addWidget(setup_page);
    setup_page->setVisible(setup->isChecked());
    connect(setup, &QPushButton::toggled, setup_page, &QWidget::setVisible);
    layout = new QVBoxLayout(setup_page);
    auto* store_intro = new QLabel(tr("Update both browser extensions to 0.2.3 or later before use. Version 0.2.2 removed Disconnect and is unsupported. Do not use it until updated. Firefox downloads are available from the release page while store review is pending."));
    store_intro->setWordWrap(true);
    layout->addWidget(store_intro);
    auto* store_row = new QHBoxLayout;
    auto* get_control = new QPushButton(tr("Update Remote Control for Chrome"));
    get_control->setObjectName(QStringLiteral("remoteControlStore"));
    auto* get_display = new QPushButton(tr("Update Remote Display for Chrome"));
    get_display->setObjectName(QStringLiteral("remoteDisplayStore"));
    store_row->addWidget(get_control);
    store_row->addWidget(get_display);
    layout->addLayout(store_row);
    const auto open_store = [=](const QString& url) {
        if (!QDesktopServices::openUrl(QUrl(url)))
            store_intro->setText(tr("Could not open the browser. Open this address in your browser: %1").arg(url));
    };
    connect(get_control, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://chromewebstore.google.com/detail/pvt-remote-control/paachfdeekmbojpfifnaadedhogpgcde"));
    });
    connect(get_display, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://chromewebstore.google.com/detail/pvt-remote-display/ebehogflkicknbgeimbmhfeaagjfgfda"));
    });
    auto* firefox_row = new QHBoxLayout;
    auto* firefox_control = new QPushButton(tr("Update Remote Control for Firefox"));
    firefox_control->setObjectName("remoteControlFirefox");
    auto* firefox_display = new QPushButton(tr("Update Remote Display for Firefox"));
    firefox_display->setObjectName("remoteDisplayFirefox");
    firefox_row->addWidget(firefox_control);
    firefox_row->addWidget(firefox_display);
    layout->addLayout(firefox_row);
    connect(firefox_control, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://github.com/gnaservicesinc/PVT-RC/releases/latest"));
    });
    connect(firefox_display, &QPushButton::clicked, page, [=] {
        open_store(QStringLiteral("https://github.com/gnaservicesinc/PVT-RD/releases/latest"));
    });
    auto* form = new QFormLayout;
    auto* enable = new QCheckBox(tr("Enable Remotes"));
    enable->setObjectName(QStringLiteral("remoteNetworkingEnabled"));
    auto* close = new QCheckBox(tr("Close window to system tray / menu bar"));
    close->setObjectName(QStringLiteral("remoteMinimizeOnClose"));
    close->setChecked(minimizeOnClose());
    form->addRow(enable);
    form->addRow(close);
    auto* instructions = new QLabel(tr("Export the Remote file (.pvtremote) from each browser extension and import those files here. Export the PVT host file (.pvthost) here and import it in each remote. Paired remotes reconnect automatically while PVT is running."));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    layout->addLayout(form);
    auto* list = new QListWidget;
    list->setMinimumHeight(90); list->setMaximumHeight(150);
    layout->addWidget(new QLabel(tr("Paired devices")));
    layout->addWidget(list);
    auto* scope = new QComboBox;
    scope->setObjectName("remoteAddressScope");
    scope->addItem(tr("This computer and devices on the same IP subnets"), "subnet");
    scope->addItem(tr("Any private network"), "private");
    scope->addItem(tr("Anywhere"), "any");
    scope->addItem(tr("Only the addresses or ranges below"), "custom");
    form->addRow(tr("Accept remote connections from"), scope);
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

    auto* row = new QHBoxLayout;
    auto* import = new QPushButton(tr("Import Remote files (.pvtremote)…"));
    import->setObjectName(QStringLiteral("remoteImport"));
    auto* remove = new QPushButton(tr("Remove selected remote"));
    remove->setObjectName(QStringLiteral("remoteRemove"));
    auto* export_host = new QPushButton(tr("Export PVT host file (.pvthost)…"));
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

    const auto refresh_profiles = [=](const QString& selected) {
        list->clear();
        for (const auto& item : draft->profiles) {
            const auto profile = item.toObject();
            const QString role = profile.value("role") == "control"
                ? tr("Control") : tr("Display");
            const auto client = profile.value("client").toObject();
            QStringList identity;
            for (const char* key : {"browser", "platform"}) {
                const auto value = client.value(key).toString().trimmed();
                if (!value.isEmpty()) identity << value;
            }
            if (!profile.value("last_endpoint").toString().isEmpty())
                identity << profile.value("last_endpoint").toString();
            identity << tr("ID: %1").arg(profile.value("id").toString());
            const QString name = role + tr(" — ") + identity.join(tr(" · "));
            list->addItem(name);
            list->item(list->count() - 1)->setData(
                Qt::UserRole, profile.value("id").toString());
        }
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->data(Qt::UserRole) == selected) list->setCurrentRow(i);
        }
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
        enable->setEnabled(config_loaded_ && !configuring_);
        scope->setEnabled(config_loaded_ && !configuring_);
        save_firewall->setEnabled(ready_ && !configuring_);
        save_admin->setEnabled(ready_ && !configuring_);
        // Worker status and startup must not replace an edit awaiting validation.
        draft->loading = true;
        if (!draft->pending_fields.contains("enabled")) enable->setChecked(requested_enabled_);
        if (!draft->pending_fields.contains("remotes")) draft->profiles = config_.value("remotes").toArray();
        if (!draft->pending_fields.contains("address_scope")) {
            scope->setCurrentIndex(std::max(0, scope->findData(
                config_.value("address_scope").toString("subnet"))));
        }
        if (!draft->pending_fields.contains("custom_networks"))
            networks->setText(config_.value("custom_networks").toString());
        const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        refresh_profiles(selected);
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
                   {"address_scope", scope->currentData().toString()},
                   {"custom_networks", networks->text()}},
                  enable->isChecked());
    };

    connect(import, &QPushButton::clicked, page, [=] {
        const auto paths = QFileDialog::getOpenFileNames(
            page, tr("Import Remote files (.pvtremote)"), {},
            tr("Remote files (*.pvtremote)"));
        if (paths.isEmpty()) return;
        QSet<QString> identities;
        for (const auto& existing : draft->profiles)
            identities.insert(existing.toObject().value("id").toString());
        int added = 0;
        int skipped = 0;
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
            ++added;
        }
        if (added == 0) {
            status->setText(skipped == 1
                ? tr("That Remote file could not be added, or it is already paired.")
                : tr("None of the selected Remote files could be added, or they are already paired."));
            return;
        }
        draft->loading = true;
        enable->setChecked(true);
        refresh_profiles(QString());
        draft->loading = false;
        apply_current({"remotes", "enabled"});
        status->setText(skipped == 0
            ? tr("Added %n Remote(s).", nullptr, added)
            : tr("Added %1 Remote(s); skipped %2 file(s) that were invalid, already paired, or beyond the 64-remote limit.")
                  .arg(added).arg(skipped));
    });
    connect(remove, &QPushButton::clicked, page, [=] {
        const int index = list->currentRow();
        if (index < 0) return;
        const auto id = draft->profiles[index].toObject().value("id").toString();
        draft->profiles.removeAt(index);
        draft->loading = true;
        refresh_profiles(QString());
        draft->loading = false;
        apply_current({"remotes"});
    });
    connect(export_host, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getSaveFileName(
            page, tr("Export PVT host file (.pvthost)"), QStringLiteral("PVT.pvthost"),
            tr("PVT host file (*.pvthost)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        file.write(QJsonDocument(profile_).toJson());
        if (!file.commit()) status->setText(file.errorString());
        else status->setText(tr("PVT host file saved. Open it in each Remote Display or Remote Control to finish setup."));
    });
    connect(background, &QPushButton::clicked, page, [=] {
        draft->changed = true;
        emit backgroundRequested(!background_);
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
                                  row.value("client").toObject().value("browser").toString(row.value("endpoint").toString()));
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
    layout->addWidget(createConnections(dialog));
    dialog->show();
}

QWidget* RemoteBridge::createConnections(QWidget* parent) {
    auto* page = new QWidget(parent);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* warning = new QLabel(tr("Extension update required: PVT-RC and PVT-RD 0.2.2 are unsupported because Disconnect is missing. Do not use these extensions until updated to 0.2.3 or later."));
    warning->setObjectName("remoteExtensionWarning");
    warning->setWordWrap(true);
    layout->addWidget(warning);
    auto* hint = new QLabel(tr("Live connections reconnect automatically. Remove a Remote file from the saved devices to revoke its access. A dash means no measurement is available."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* table = new QTableWidget(0, 5);
    table->setObjectName("remoteConnections");
    table->setMinimumHeight(210);
    table->setHorizontalHeaderLabels({tr("Remote"), tr("Connection addresses"), tr("Status"), tr("Response / loss"), tr("Data use")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->setWordWrap(true);
    layout->addWidget(table);
    const auto refresh = [this, table] {
        const auto selected = table->currentRow() >= 0 ? table->item(table->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
        table->setRowCount(static_cast<int>(connections_.size()));
        for (int i = 0; i < connections_.size(); ++i) {
            const auto row = connections_[i].toObject();
            const auto client = row.value("client").toObject();
            const auto metric = [&](const char* key, int decimals = 0) { return row.contains(key) ? QLocale().toString(row.value(key).toDouble(), 'f', decimals) : QStringLiteral("—"); };
            const auto bytes = [&](const char* key) { return row.contains(key) ? QLocale().formattedDataSize(row.value(key).toInteger(), 1, QLocale::DataSizeTraditionalFormat) : QStringLiteral("—"); };
            const auto browser = client.value("browser").toString().trimmed();
            const auto platform = client.value("platform").toString().trimmed();
            QStringList device{row.value("role") == "control" ? tr("Control PVT") : tr("View PVT")};
            device << (browser.isEmpty() ? tr("Browser/system not reported — update the extension") : browser);
            if (!platform.isEmpty()) device << platform;
            device << tr("ID: %1").arg(row.value("id").toString().left(13));
            const auto version = client.value("version").toString();
            const auto parsed_version = QVersionNumber::fromString(version);
            const bool needs_update = parsed_version.isNull() || parsed_version < QVersionNumber(0, 2, 3);
            device << tr("Extension %1").arg(version.isEmpty() ? QStringLiteral("—") : version);
            QString status = row.value("status").toString();
            if (needs_update)
                status = tr("Unsupported extension — do not use until updated to 0.2.3 or later") + '\n' + status;
            QStringList addresses{row.value("endpoint").toString()};
            if (!row.value("media_endpoints").toString().isEmpty())
                addresses << tr("Media: %1").arg(row.value("media_endpoints").toString());
            QStringList values{device.join('\n'), addresses.join('\n'),
                status + '\n' + tr("%1 s").arg(row.value("seconds").toInt()),
                (row.contains("rtt_ms") ? tr("%1 ms").arg(metric("rtt_ms", 1)) : QStringLiteral("—")) + '\n' + tr("Packets lost: %1").arg(metric("packets_lost")),
                tr("%1\nSent %2\nReceived %3").arg(row.contains("kbps") ? (row.value("kbps").toDouble() >= 1000 ? tr("%1 Mb/s").arg(QLocale().toString(row.value("kbps").toDouble() / 1000, 'f', 1)) : tr("%1 kb/s").arg(metric("kbps", 1))) : QStringLiteral("—"), bytes("bytes_sent"), bytes("bytes_received"))};
            const auto key = row.value("session").toString(row.value("id").toString());
            for (int j = 0; j < values.size(); ++j) {
                auto* item = new QTableWidgetItem(values[j]);
                item->setToolTip(j == 0 ? values[j] + '\n' + row.value("id").toString() + '\n' + tr("Extension %1").arg(client.value("version").toString("—")) : values[j]);
                item->setData(Qt::UserRole, key); table->setItem(i, j, item);
            }
            if (key == selected) table->selectRow(i);
        }
        table->resizeRowsToContents();
    };
    connect(this, &RemoteBridge::connectionsChanged, page, refresh);
    refresh();
    return page;
}
