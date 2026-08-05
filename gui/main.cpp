#include "main_window.h"

#include <QApplication>
#include <QDebug>
#include <QTabWidget>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Procedural Visualizer Tool"));
    QApplication::setOrganizationName(QStringLiteral("GNA Services"));

    MainWindow window;
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--smoke-test"))) {
        QString smoke_error;
        if (!window.runSmokeChecks(&smoke_error)) {
            qCritical().noquote() << smoke_error;
            return 1;
        }
        const qsizetype screenshot_index =
            arguments.indexOf(QStringLiteral("--screenshot"));
        const qsizetype tab_index = arguments.indexOf(QStringLiteral("--tab"));
        if ((screenshot_index >= 0 && screenshot_index + 1 >= arguments.size())
            || (tab_index >= 0 && tab_index + 1 >= arguments.size())) {
            qCritical("--screenshot and --tab each require a value");
            return 1;
        }
        if (tab_index >= 0) {
            bool valid_tab = false;
            const int requested_tab = arguments.at(tab_index + 1).toInt(&valid_tab);
            if (auto* tabs = window.findChild<QTabWidget*>()) {
                if (!valid_tab || requested_tab < 0 || requested_tab >= tabs->count()) {
                    qCritical("--tab must identify an existing zero-based tab index");
                    return 1;
                }
                tabs->setCurrentIndex(requested_tab);
            }
        }
        window.show();
        if (screenshot_index >= 0) {
            const QString path = arguments.at(screenshot_index + 1);
            QTimer::singleShot(800, &window, [&application, &window, path] {
                if (!window.grab().save(path)) {
                    qCritical().noquote() << "Could not save smoke-test screenshot to" << path;
                    application.exit(1);
                }
            });
            QTimer::singleShot(1000, &application, &QCoreApplication::quit);
        } else {
            QTimer::singleShot(500, &application, &QCoreApplication::quit);
        }
        return application.exec();
    }

    window.show();
    return application.exec();
}
