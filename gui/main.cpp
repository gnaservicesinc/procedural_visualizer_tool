#include "main_window.h"

#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Procedural Visualizer Tool"));
    QApplication::setApplicationDisplayName(
        QStringLiteral("Procedural Visualizer Tool"));
    QApplication::setDesktopFileName(
        QStringLiteral("procedural-visualizer-tool"));
#ifdef PVT_PROGRAM_VERSION
    QApplication::setApplicationVersion(QStringLiteral(PVT_PROGRAM_VERSION));
#endif
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
        const qsizetype stage_index =
            arguments.indexOf(QStringLiteral("--stage"));
        if ((screenshot_index >= 0 && screenshot_index + 1 >= arguments.size())
            || (tab_index >= 0 && tab_index + 1 >= arguments.size())
            || (stage_index >= 0 && stage_index + 1 >= arguments.size())) {
            qCritical("--screenshot, --tab, and --stage each require a value");
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
        if (stage_index >= 0) {
            bool valid_stage = false;
            const int requested_stage =
                arguments.at(stage_index + 1).toInt(&valid_stage);
            auto* button = window.findChild<QPushButton*>(
                QStringLiteral("workflowStage%1").arg(requested_stage));
            if (!valid_stage || button == nullptr) {
                qCritical("--stage must identify an existing zero-based workspace category");
                return 1;
            }
            button->click();
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
            // On macOS, quit() routes through native application termination,
            // which closes the window and can display the unsaved-changes
            // dialog after the smoke test deliberately exercises edits. Exit
            // the test event loop directly so unattended CI cannot block on a
            // modal prompt after all checks have already passed.
            QTimer::singleShot(1000, &application,
                               [&application] { application.exit(0); });
        } else {
            QTimer::singleShot(500, &application,
                               [&application] { application.exit(0); });
        }
        return application.exec();
    }

    QString startup_project;
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        if (argument == QStringLiteral("--working-directory")) {
            ++index;
            continue;
        }
        if (!argument.startsWith(QLatin1Char('-'))) {
            startup_project = QFileInfo(argument).absoluteFilePath();
            break;
        }
    }

    window.show();
    if (!startup_project.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, startup_project] {
            window.openProject(startup_project);
        });
    }
    return application.exec();
}
