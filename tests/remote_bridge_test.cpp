#include "remote_bridge.h"
#include "remote_firewall_support.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>

// Exercise the actual QProcess/JSON and modal-dialog paths without requiring
// Python, networking, private identities, or a user's settings.
class UrlReceiver : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void receive(const QUrl& url) { urls.append(url); }
};

static int worker() {
    QThread::msleep(200);
    const auto emitJson = [](const QJsonObject& value) {
        std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData()
                  << std::endl;
    };
    QJsonObject profile{
        {"id", "00000000-0000-4000-8000-000000000001"},
        {"endpoints", QJsonArray{"ws://127.0.0.1:54321",
                                  "ws://192.168.50.4:54321"}}};
    QJsonObject config{
        {"label", "Old disk name"},
        {"port", 54321},
        {"remotes", QJsonArray{QJsonObject{{"id", "saved-remote"},
                                            {"label", "Saved control"},
                                            {"role", "control"}}}}};
    emitJson({{"event", "ready"}, {"profile", profile},
              {"config", config}, {"enabled", false}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto message =
            QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto op = message.value("op").toString();
        if (op == "shutdown") break;
        if (op == "enable") {
            emitJson({{"event", "configured"}, {"profile", profile},
                      {"config", config}, {"enabled", message.value("enabled")}});
        }
        if (op == "configure") {
            const auto changes = message.value("config").toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it)
                config.insert(it.key(), it.value());
            emitJson({{"event", "configured"}, {"profile", profile},
                      {"config", config}, {"enabled", message.value("enabled")}});
        }
        if (op == "video") {
            const auto image = QImage::fromData(QByteArray::fromBase64(
                message.value("jpeg").toString().toLatin1()));
            if (image.size() == QSize(1, 1)) return 23;
            auto frames = config;
            frames.insert("remotes", QJsonArray{QJsonObject{
                {"role", "control"},
                {"label", QStringLiteral("frame %1x%2")
                              .arg(image.width()).arg(image.height())}}});
            emitJson({{"event", "configured"}, {"profile", profile},
                      {"config", frames}, {"enabled", true}});
        }
    }
    return 0;
}

static bool spin(const std::function<bool()>& condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}

int main(int argc, char** argv) {
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (argc > 1 && QByteArray(argv[1]) == "--directory") return worker();
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    QCoreApplication::setOrganizationName("PVT-test");
    QCoreApplication::setApplicationName("remote-bridge");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       temporary.path());
    qputenv("PVT_REMOTE_TEST_WORKER",
            QCoreApplication::applicationFilePath().toUtf8());
    RemoteBridge bridge;
    QString last_status;
    QObject::connect(&bridge, &RemoteBridge::statusChanged, &app,
                     [&](const QString& status) { last_status = status; });

    const QJsonObject support_config{
        {"port", 54321}, {"address_scope", "custom"},
        {"custom_networks", "10.20.0.0/16, 192.168.5.10-192.168.5.20"}};
    const QJsonObject support_profile{
        {"id", "00000000-0000-4000-8000-000000000001"},
        {"endpoints", QJsonArray{"ws://127.0.0.1:54321",
                                  "ws://192.168.50.4:54321"}}};
    const QList<quint16> expected_ports{49152, 53245, 54321, 57343, 61441};
    if (pvt::remote_support::connectionPorts(support_config, support_profile)
        != expected_ports) {
        std::cerr << "Firewall handoff did not use the stable PVT connection ports\n";
        return 1;
    }
    const auto instructions = pvt::remote_support::networkAdminInstructions(
        support_config, support_profile);
    const auto windows = pvt::remote_support::localFirewallSetup(
        pvt::remote_support::FirewallPlatform::Windows, support_config,
        support_profile, QStringLiteral("C:/Program Files/PVT/pvt-remote.exe"));
    const auto mac = pvt::remote_support::localFirewallSetup(
        pvt::remote_support::FirewallPlatform::MacOS, support_config,
        support_profile, QStringLiteral("/Applications/PVT.app/pvt-remote"));
    const auto ubuntu = pvt::remote_support::localFirewallSetup(
        pvt::remote_support::FirewallPlatform::Ubuntu, support_config,
        support_profile, QStringLiteral("/usr/lib/pvt/pvt-remote"));
    if (!instructions.contains("TCP") || !instructions.contains("UDP")
        || !instructions.contains("192.168.50.4")
        || !windows.contents.contains("netsh advfirewall")
        || !windows.contents.contains("10.20.0.0/16")
        || !mac.contents.contains("socketfilterfw")
        || !ubuntu.contents.contains("ufw allow proto tcp")
        || windows.contents.contains("remote_port")) {
        std::cerr << "Firewall setup or administrator handoff is incomplete\n";
        return 1;
    }

    UrlReceiver receiver;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &receiver,
                                    "receive");
    bool store_links = false;
    bool cancellation_exercised = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* enabled = dialog->findChild<QCheckBox*>("remoteNetworkingEnabled");
        auto* buttons = dialog->findChild<QDialogButtonBox*>("remoteManagerButtons");
        auto* scope = dialog->findChild<QComboBox*>("remoteAddressScope");
        auto* ranges = dialog->findChild<QLineEdit*>("remoteCustomNetworks");
        auto* firewall = dialog->findChild<QFrame*>("remoteFirewallHelp");
        if (!enabled || !buttons || !scope || !ranges || !firewall
            || buttons->button(QDialogButtonBox::Apply)
            || !dialog->findChildren<QSpinBox*>().isEmpty()) {
            dialog->reject();
            return;
        }
        auto* control_store =
            dialog->findChild<QPushButton*>("remoteControlStore");
        auto* display_store =
            dialog->findChild<QPushButton*>("remoteDisplayStore");
        if (control_store && display_store) {
            control_store->click();
            display_store->click();
            store_links = receiver.urls == QList<QUrl>{
                QUrl("https://chromewebstore.google.com/detail/pvt-remote-control/paachfdeekmbojpfifnaadedhogpgcde"),
                QUrl("https://chromewebstore.google.com/detail/pvt-remote-display/ebehogflkicknbgeimbmhfeaagjfgfda")};
        }
        if (!spin([&] { return enabled->isEnabled(); })
            || ranges->isVisible() || firewall->isVisible()) {
            dialog->reject();
            return;
        }
        scope->setCurrentIndex(scope->findData("custom"));
        if (!ranges->isVisible() || !firewall->isVisible()) {
            dialog->reject();
            return;
        }
        scope->setCurrentIndex(scope->findData("private"));
        if (!spin([&] { return scope->isEnabled(); })) {
            dialog->reject();
            return;
        }
        enabled->setChecked(true);
        if (!spin([&] { return bridge.enabled(); })) {
            dialog->reject();
            return;
        }
        auto* close = dialog->findChild<QCheckBox*>("remoteMinimizeOnClose");
        if (close) close->setChecked(true);
        cancellation_exercised = true;
        dialog->reject();
    });
    bridge.showManager(nullptr);
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    if (!store_links) {
        std::cerr << "Store actions opened an incorrect destination\n";
        return 1;
    }
    if (!cancellation_exercised || !spin([&] { return !bridge.enabled(); })
        || QSettings().value("remotes/minimizeOnClose", false).toBool()) {
        std::cerr << "Cancel did not restore live Remote settings\n";
        return 1;
    }

    bool accepted = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* enabled = dialog->findChild<QCheckBox*>("remoteNetworkingEnabled");
        if (!enabled || !spin([&] { return enabled->isEnabled(); })) {
            dialog->reject();
            return;
        }
        enabled->setChecked(true);
        accepted = spin([&] { return bridge.enabled(); });
        dialog->accept();
    });
    bridge.showManager(nullptr);
    if (!accepted || !bridge.enabled()) {
        std::cerr << "Remote changes were not applied as they were entered\n";
        return 1;
    }

    const auto write_pairing = [&](const QString& name, const QString& id,
                                   const QString& label, const QString& role) {
        QFile file(temporary.path() + QLatin1Char('/') + name);
        if (!file.open(QIODevice::WriteOnly)) return QString();
        file.write(QJsonDocument(QJsonObject{
            {"version", 1}, {"type", "pvtremote"}, {"id", id},
            {"label", label}, {"role", role}}).toJson());
        file.close();
        return file.fileName();
    };
    const QString control_file = write_pairing(
        "second.pvtremote", "second-remote", "Studio control", "control");
    const QString display_file = write_pairing(
        "third.pvtremote", "third-remote", "Stage display", "display");
    if (control_file.isEmpty() || display_file.isEmpty()) return 1;
    const QString pairing_directory = temporary.path();
    bool multi_imported = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* import = dialog->findChild<QPushButton*>("remoteImport");
        auto* list = dialog->findChild<QListWidget*>();
        if (!import || !list || !spin([&] { return import->isEnabled(); })) {
            dialog->reject();
            return;
        }
        auto* chooser = new QTimer(&app);
        chooser->setInterval(5);
        QObject::connect(chooser, &QTimer::timeout, &app, [=] {
            auto* files = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!files || files == dialog) return;
            auto* names =
                files->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
            if (!names) return;
            files->setDirectory(pairing_directory);
            names->setText(
                QStringLiteral("\"second.pvtremote\" \"third.pvtremote\""));
            chooser->stop();
            QMetaObject::invokeMethod(files, "accept", Qt::QueuedConnection);
            chooser->deleteLater();
        });
        chooser->start();
        import->click();
        multi_imported = spin([&] {
            return list->count() == 3
                && bridge.controlNames().contains("Studio control");
        });
        if (multi_imported) dialog->accept(); else dialog->reject();
    });
    bridge.showManager(nullptr);
    if (!multi_imported) {
        std::cerr << "Selecting multiple Remote pairing files did not import all of them\n";
        return 1;
    }

    bool retained = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        retained = dialog->findChild<QLineEdit*>("remoteCustomNetworks")
            && bridge.controlNames().contains("Saved control")
            && bridge.controlNames().contains("Studio control");
        dialog->reject();
    });
    bridge.showManager(nullptr);
    if (!retained) {
        std::cerr << "Worker did not retain the accepted configuration\n";
        return 1;
    }

    const std::pair<QSize, QSize> frame_sizes[] = {
        {{640, 360}, {640, 360}}, {{1920, 1080}, {1920, 1080}},
        {{3840, 2160}, {1920, 1080}}, {{720, 1440}, {540, 1080}}};
    for (const auto& [size, output] : frame_sizes) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(Qt::blue);
        last_status.clear();
        bridge.sendFrame(image);
        const auto expected = QStringLiteral("frame %1x%2")
                                  .arg(output.width()).arg(output.height());
        if (!spin([&] { return bridge.controlNames().contains(expected); })) {
            std::cerr << "Unexpected encoded frame dimensions: "
                      << last_status.toStdString() << '\n';
            return 1;
        }
    }
    bool lost = false;
    QObject::connect(&bridge, &RemoteBridge::configurationChanged, &app,
                     [&] { if (!bridge.enabled()) lost = true; });
    QImage crash(1, 1, QImage::Format_RGB32);
    crash.fill(Qt::black);
    bridge.sendFrame(crash);
    if (!spin([&] { return lost && bridge.enabled(); })) {
        std::cerr << "Worker did not recover automatically\n";
        return 1;
    }
    bridge.stop();
    std::cout << "Remote auto-apply, cancel rollback, multi-import, firewall "
                 "handoff and encoded video dimensions passed\n";
    return 0;
}

#include "remote_bridge_test.moc"
