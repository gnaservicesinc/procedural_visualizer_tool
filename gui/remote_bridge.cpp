#include "remote_bridge.h"
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
#include <QSpinBox>
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
        const auto executable = QCoreApplication::applicationDirPath()
#ifdef Q_OS_MACOS
            + "/../Resources/pvt-remote/pvt-remote"
#else
            + "/pvt-remote/pvt-remote"
#endif
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
            } else emit statusChanged(enabled_ ? tr("Networking & Remotes enabled") : tr("Networking & Remotes disabled"));
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
    dialog.setWindowTitle(tr("Networking & Remotes"));
    dialog.resize(820, 720);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(createManager(&dialog));
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
    auto* enable = new QCheckBox(tr("Enable Networking & Remotes"));
    enable->setObjectName(QStringLiteral("remoteNetworkingEnabled"));
    auto* close = new QCheckBox(tr("Close window to system tray / menu bar"));
    close->setChecked(minimizeOnClose());
    form->addRow(enable);
    form->addRow(close);
    auto* instructions = new QLabel(tr("Pair once: save the pairing file from Remote Display or Remote Control and open it here. Then save PVT’s pairing file and open it in the remote’s Hosts & settings. Save changes there if prompted. Paired devices reconnect automatically while PVT is running."));
    instructions->setWordWrap(true);
    layout->addWidget(instructions);
    layout->addLayout(form);
    auto* list = new QListWidget;
    list->setMinimumHeight(90); list->setMaximumHeight(150);
    layout->addWidget(new QLabel(tr("Paired devices")));
    layout->addWidget(list);
    auto* active = new QComboBox;
    form->addRow(tr("Active Control Remote"), active);
    auto* scope = new QComboBox;
    scope->setObjectName("remoteAddressScope");
    scope->addItem(tr("My subnets and this computer"), "subnet");
    scope->addItem(tr("Private networks (IPv4 and IPv6)"), "private");
    scope->addItem(tr("Any IP address"), "any");
    scope->addItem(tr("Custom IP addresses / ranges"), "custom");
    form->addRow(tr("Allow connections from"), scope);
    auto* networks = new QLineEdit;
    networks->setObjectName("remoteCustomNetworks");
    networks->setPlaceholderText("192.168.1.0/24, 10.0.0.10-10.0.0.40, fd12:3456::/48");
    networks->setToolTip(tr("Separate IPs, CIDR subnets, or start-end ranges with spaces or commas. Private networks includes RFC 1918, IPv6 unique-local (fc00::/7), link-local (fe80::/10), and loopback. Relay connections require Any IP."));
    form->addRow(tr("Custom ranges"), networks);
    auto* port_min = new QSpinBox;
    auto* port_max = new QSpinBox;
    port_min->setRange(1, 65535); port_max->setRange(1, 65535);
    port_min->setValue(1); port_max->setValue(65535);
    auto* ports = new QHBoxLayout;
    ports->addWidget(port_min); ports->addWidget(new QLabel(tr("through"))); ports->addWidget(port_max);
    form->addRow(tr("Remote endpoint ports"), ports);
    port_min->setToolTip(tr("Applies to remote signaling and media source ports. Browsers normally choose these automatically; keep the full range unless your network requires a restriction."));
    auto* tracker = new QPushButton(tr("Pop out live connection tracker…"));
    layout->addWidget(tracker);
    connect(tracker, &QPushButton::clicked, page, [=] { showTracker(page->window()); });
    auto* resume = new QPushButton(tr("Resume selected device"));
    layout->addWidget(resume);
    connect(resume, &QPushButton::clicked, page, [=] {
        if (auto* item = list->currentItem()) setPaused(item->data(Qt::UserRole).toString(), false);
    });
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
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply);
    buttons->setObjectName(QStringLiteral("remoteManagerButtons"));
    layout->addWidget(buttons);
    struct Draft { QJsonArray profiles; QSet<QString> edited_fields; bool loading = false; };
    auto draft = std::make_shared<Draft>();
    const auto reload = [=] {
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        import->setEnabled(config_loaded_ && !configuring_);
        remove->setEnabled(config_loaded_ && !configuring_);
        active->setEnabled(config_loaded_ && !configuring_);
        // Worker status and startup must not replace an in-progress draft.
        draft->loading = true;
        if (!draft->edited_fields.contains("enabled")) enable->setChecked(enabled_);
        if (!draft->edited_fields.contains("remotes")) draft->profiles = config_.value("remotes").toArray();
        const auto active_id = draft->edited_fields.contains("active_control")
            ? active->currentData().toString() : config_.value("active_control").toString();
        if (!draft->edited_fields.contains("address_scope")) scope->setCurrentIndex(std::max(0, scope->findData(config_.value("address_scope").toString("subnet"))));
        if (!draft->edited_fields.contains("custom_networks")) networks->setText(config_.value("custom_networks").toString());
        if (!draft->edited_fields.contains("remote_port_min")) port_min->setValue(config_.value("remote_port_min").toInt(1));
        if (!draft->edited_fields.contains("remote_port_max")) port_max->setValue(config_.value("remote_port_max").toInt(65535));
        networks->setEnabled(scope->currentData() == "custom");
        const auto selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear(); active->clear(); active->addItem(tr("None"), "");
        for (const auto& item : draft->profiles) {
            const auto p = item.toObject();
            list->addItem(p.value("label").toString() + " · " + p.value("role").toString() + " · " + p.value("id").toString().left(8)
                + (config_.value("paused_remotes").toArray().contains(p.value("id")) ? tr(" · Paused") : QString()));
            list->item(list->count() - 1)->setData(Qt::UserRole, p.value("id").toString());
            if (p.value("role") == "control") active->addItem(p.value("label").toString(), p.value("id").toString());
        }
        for (int i = 0; i < list->count(); ++i) if (list->item(i)->data(Qt::UserRole) == selected) list->setCurrentRow(i);
        active->setCurrentIndex(std::max(0, active->findData(active_id)));
        buttons->button(QDialogButtonBox::Apply)->setEnabled(!configuring_);
        export_host->setEnabled(ready_ && enabled_ && !configuring_);
        draft->loading = false;
    };
    connect(this, &RemoteBridge::configurationChanged, page, reload);
    connect(this, &RemoteBridge::statusChanged, status, &QLabel::setText);
    connect(import, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getOpenFileName(page, tr("Import remote"), {}, tr("PVT remote (*.pvtremote)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) { status->setText(tr("Cannot read pairing file (16 KiB maximum).")); return; }
        const auto p = QJsonDocument::fromJson(file.readAll()).object();
        if (p.value("type") != "pvtremote" || p.value("version").toInt() != 1 || draft->profiles.size() >= 64) { status->setText(tr("Invalid pairing file or remote limit reached.")); return; }
        for (const auto& existing : draft->profiles) if (existing.toObject().value("id") == p.value("id")) { status->setText(tr("Remove the existing identity before replacing its keys.")); return; }
        draft->profiles.append(p);
        draft->edited_fields.remove("remotes");
        list->addItem(p.value("label").toString() + " · " + p.value("role").toString() + " · " + p.value("id").toString().left(8)
                + (config_.value("paused_remotes").toArray().contains(p.value("id")) ? tr(" · Paused") : QString()));
            list->item(list->count() - 1)->setData(Qt::UserRole, p.value("id").toString());
        if (p.value("role") == "control") {
            active->addItem(p.value("label").toString(), p.value("id").toString());
            if (active->currentData().toString().isEmpty()) active->setCurrentIndex(active->count() - 1);
        }
        enable->setChecked(true);
        draft->edited_fields.remove("enabled");
        draft->edited_fields.remove("active_control");
        configure({{"remotes", draft->profiles}, {"active_control", active->currentData().toString()}}, true);
        export_host->setEnabled(false);
    });
    connect(remove, &QPushButton::clicked, page, [=] {
        const int index = list->currentRow();
        if (index < 0) return;
        draft->edited_fields.remove("remotes");
        const auto id = draft->profiles[index].toObject().value("id").toString();
        draft->profiles.removeAt(index); delete list->takeItem(index);
        const int choice = active->findData(id);
        if (choice >= 0) active->removeItem(choice);
        draft->edited_fields.remove("active_control");
        configure({{"remotes", draft->profiles}, {"active_control", active->currentData().toString()}}, enable->isChecked());
    });
    connect(export_host, &QPushButton::clicked, page, [=] {
        const auto path = QFileDialog::getSaveFileName(page, tr("Export host"), "PVT.pvthost", tr("PVT host (*.pvthost)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { status->setText(file.errorString()); return; }
        file.write(QJsonDocument(profile_).toJson());
        if (!file.commit()) status->setText(file.errorString());
        else status->setText(tr("Pairing file saved. Open it in Remote Display or Remote Control to finish setup."));
    });
    connect(background, &QPushButton::clicked, page, [=] {
        emit backgroundRequested(!background_);
    });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, page, [=] {
        QSettings().setValue("remotes/minimizeOnClose", close->isChecked());
        QJsonObject changes{{"remotes", draft->profiles}, {"active_control", active->currentData().toString()},
            {"address_scope", scope->currentData().toString()}, {"custom_networks", networks->text()},
            {"remote_port_min", port_min->value()}, {"remote_port_max", port_max->value()}};
        // On first startup we haven't read the saved configuration yet. Send
        // only authored fields so defaults cannot erase unseen paired remotes
        // or network settings; the worker already merges configuration patches.
        if (!config_loaded_) {
            for (const auto& key : changes.keys())
                if (!draft->edited_fields.contains(key)) changes.remove(key);
        }
        configure(changes, enable->isChecked());
    });

    if (auto* dialog = qobject_cast<QDialog*>(parent))
        connect(dialog, &QDialog::accepted, page, [buttons] { buttons->button(QDialogButtonBox::Apply)->click(); });
    reload();
    const auto edited = [=](const QString& key) { if (!draft->loading) draft->edited_fields.insert(key); };
    connect(enable, &QCheckBox::toggled, page, [=] { edited("enabled"); });
    connect(active, &QComboBox::currentIndexChanged, page, [=] { edited("active_control"); });
    connect(scope, &QComboBox::currentIndexChanged, page, [=] { edited("address_scope"); networks->setEnabled(scope->currentData() == "custom"); });
    connect(networks, &QLineEdit::textChanged, page, [=] { edited("custom_networks"); });
    connect(port_min, &QSpinBox::valueChanged, page, [=] { edited("remote_port_min"); });
    connect(port_max, &QSpinBox::valueChanged, page, [=] { edited("remote_port_max"); });
    if (!ready_) start();
    return page;
}

QString RemoteBridge::connectionSummary() const {
    QStringList names;
    for (const auto& value : connections_) {
        const auto row = value.toObject();
        names << tr("%1 %2 [%3]").arg(row.value("role") == "control" ? tr("Control") : tr("View"),
            row.value("label").toString(), row.value("id").toString().left(8));
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
    dialog->setWindowTitle(tr("Live remote connections"));
    dialog->resize(1100, 420);
    auto* layout = new QVBoxLayout(dialog);
    auto* hint = new QLabel(tr("Authenticated devices. Browser details are self-reported; the identity distinguishes pairings. Media endpoints are allowed ICE candidates, not a claim that every candidate is in use. — means no measurement is available. Pause disconnects a device and prevents automatic reconnection until resumed in Remotes."));
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* table = new QTableWidget(0, 10);
    table->setHorizontalHeaderLabels({tr("Device / identity"), tr("Role / access"), tr("Browser / extension / OS"), tr("Signaling endpoint"), tr("Media candidates"), tr("Status"), tr("Connected"), tr("RTT ms"), tr("Lost packets"), tr("Traffic kbit/s / sent / received")});
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
            QStringList values{row.value("label").toString() + " / " + row.value("id").toString(),
                row.value("role").toString() + (row.value("active_control").toBool() ? tr(" · Can edit") : tr(" · View / status")),
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
