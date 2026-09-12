#include "remote_bridge.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>

// Exercise the actual QProcess/JSON and modal-dialog paths without requiring
// Python, networking, private identities, or a user's settings.
static int worker() {
    QThread::msleep(200);
    const auto emitJson = [](const QJsonObject& value) {
        std::cout << QJsonDocument(value).toJson(QJsonDocument::Compact).constData() << std::endl;
    };
    QJsonObject config{{"label", "Old disk name"}, {"port", 54321},
        {"remotes", QJsonArray{QJsonObject{{"id", "saved-remote"}, {"label", "Saved control"}, {"role", "control"}}}}};
    emitJson({{"event", "ready"}, {"config", config}, {"enabled", false}});
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto message = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
        const auto op = message.value("op").toString();
        if (op == "shutdown") break;
        if (op == "configure") {
            const auto changes = message.value("config").toObject();
            for (auto it = changes.begin(); it != changes.end(); ++it) config.insert(it.key(), it.value());
            emitJson({{"event", "configured"}, {"config", config}, {"enabled", message.value("enabled")}});
        }
        if (op == "video") {
            const auto image = QImage::fromData(QByteArray::fromBase64(message.value("jpeg").toString().toLatin1()));
            emitJson({{"event", "status"}, {"error", QStringLiteral("frame %1x%2").arg(image.width()).arg(image.height())}});
        }
    }
    return 0;
}

static bool spin(const std::function<bool()>& condition) {
    QElapsedTimer elapsed; elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    return condition();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc > 1 && QByteArray(argv[1]) == "-I") return worker();
    QTemporaryDir temporary;
    if (!temporary.isValid()) return 1;
    QCoreApplication::setOrganizationName("PVT-test");
    QCoreApplication::setApplicationName("remote-bridge");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temporary.path());
    QSettings().setValue("remotes/python", QCoreApplication::applicationFilePath());
    RemoteBridge bridge;
    QString last_status;
    QObject::connect(&bridge, &RemoteBridge::statusChanged, &app, [&](const QString& status) { last_status = status; });
    bool applied = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* label = dialog->findChild<QLineEdit*>("remoteHostName");
        auto* enabled = dialog->findChild<QCheckBox*>("remoteNetworkingEnabled");
        auto* buttons = dialog->findChild<QDialogButtonBox*>("remoteManagerButtons");
        if (!label || !enabled || !buttons) { dialog->reject(); return; }
        label->setText("Applied before ready");
        enabled->setChecked(true);
        buttons->button(QDialogButtonBox::Apply)->click();
        // Keep editing while startup and configure acknowledgments arrive.
        label->setText("Unapplied draft");
        const bool enabled_ok = spin([&] { return bridge.enabled(); });
        applied = enabled_ok && label->text() == "Unapplied draft";
        dialog->reject();
    });
    bridge.showManager(nullptr);
    if (!applied) { std::cerr << "Startup Apply or draft preservation failed\n"; return 1; }
    bool retained = false;
    QTimer::singleShot(0, &app, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* label = dialog->findChild<QLineEdit*>("remoteHostName");
        const auto* port = dialog->findChild<QSpinBox*>();
        retained = label && label->text() == "Applied before ready" && port && port->value() == 54321
            && bridge.controlNames().contains("Saved control");
        dialog->reject();
    });
    bridge.showManager(nullptr);
    if (!retained) { std::cerr << "Worker did not receive pending configuration\n"; return 1; }
    const std::pair<QSize, QSize> frame_sizes[] = {
        {{640,360}, {640,360}}, {{1920,1080}, {1920,1080}},
        {{3840,2160}, {1920,1080}}, {{720,1440}, {540,1080}}
    };
    for (const auto& [size, output] : frame_sizes) {
        QImage image(size, QImage::Format_RGB32); image.fill(Qt::blue);
        last_status.clear();
        bridge.sendFrame(image);
        const auto expected = QStringLiteral("frame %1x%2").arg(output.width()).arg(output.height());
        if (!spin([&] { return last_status == expected; })) {
            std::cerr << "Unexpected encoded frame dimensions: " << last_status.toStdString() << '\n'; return 1;
        }
    }
    bridge.stop();
    std::cout << "Remote startup configuration, draft retention and encoded video dimensions passed\n";
    return 0;
}
