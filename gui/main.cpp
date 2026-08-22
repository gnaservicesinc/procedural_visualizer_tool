#include "main_window.h"

#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QFileInfo>
#include <QPalette>
#include <QPushButton>
#include <QStyleFactory>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>

namespace {

void apply_studio_theme(QApplication& application) {
    // A controlled dark palette reads consistently under venue lighting and
    // across macOS/Windows/Linux while retaining native menu/window behavior.
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(QStringLiteral("#202529")));
    palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#e7eaeb")));
    palette.setColor(QPalette::Base, QColor(QStringLiteral("#111619")));
    palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#192024")));
    palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#101416")));
    palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#f2f4f4")));
    palette.setColor(QPalette::Text, QColor(QStringLiteral("#e7eaeb")));
    palette.setColor(QPalette::Button, QColor(QStringLiteral("#30373b")));
    palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#edf0f1")));
    palette.setColor(QPalette::BrightText, QColor(QStringLiteral("#ff756d")));
    palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#397f80")));
    palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#ffffff")));
    palette.setColor(QPalette::Link, QColor(QStringLiteral("#69d6cb")));
    palette.setColor(QPalette::Mid, QColor(QStringLiteral("#485157")));
    palette.setColor(QPalette::Dark, QColor(QStringLiteral("#0d1113")));
    palette.setColor(QPalette::Shadow, QColor(QStringLiteral("#07090a")));
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     QColor(QStringLiteral("#788186")));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     QColor(QStringLiteral("#788186")));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor(QStringLiteral("#788186")));
    application.setPalette(palette);
    application.setStyleSheet(QStringLiteral(R"PVT_QSS(
        QMainWindow, QDialog { background: #202529; }
        QToolBar {
            background: #242a2e; border: 0; border-bottom: 1px solid #0d1113;
            spacing: 2px; padding: 3px 5px;
        }
        QToolBar QToolButton { padding: 5px 8px; border-radius: 4px; }
        QToolBar QToolButton:hover { background: #384247; }
        QMenuBar { background: #242a2e; border-bottom: 1px solid #111517; }
        QMenuBar::item { padding: 5px 9px; background: transparent; }
        QMenuBar::item:selected { background: #374146; border-radius: 3px; }
        QMenu { background: #242a2e; border: 1px solid #4a555a; padding: 4px; }
        QMenu::item { padding: 5px 28px 5px 22px; border-radius: 3px; }
        QMenu::item:selected { background: #397f80; }
        QGroupBox {
            background: #252b2f; border: 1px solid #424b50; border-radius: 6px;
            margin-top: 13px; padding: 10px 7px 7px 7px; font-weight: 600;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 10px; padding: 0 5px;
            color: #d8dddf;
        }
        QGroupBox:disabled { border-color: #343b3f; color: #788186; }
        QLabel#activeWorkflowContext {
            background: #182024; border: 1px solid #3a7777; border-radius: 4px;
            color: #8ee3da; padding: 6px 8px; font-weight: 600;
        }
        QLabel#workflowPrerequisite {
            background: #191e21; border: 1px solid #374045; border-radius: 4px;
            color: #cbd1d3; padding: 5px 7px;
        }
        QGroupBox#driversStrip {
            background: #1b2124; border: 1px solid #394449; border-radius: 6px;
        }
        QPushButton {
            min-height: 23px; padding: 3px 9px; border-radius: 4px;
            border: 1px solid #535e63;
            background: qlineargradient(x1:0,y1:0,x2:0,y2:1,
                                        stop:0 #3a4246, stop:1 #292f33);
        }
        QPushButton:hover { border-color: #6bcfc6; background: #394347; }
        QPushButton:pressed { background: #1c2225; padding-top: 4px; }
        QPushButton:checked { background: #397f80; border-color: #70d9cf; }
        QPushButton:disabled { background: #262c2f; border-color: #363e42; }
        QWidget#workflowStageNavigator QPushButton {
            font-weight: 600; border-radius: 5px; padding: 5px 8px;
        }
        QWidget#workflowStageNavigator QPushButton:checked {
            background: #3b8182; color: white; border: 1px solid #77d9d0;
        }
        QLineEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            min-height: 22px; padding: 2px 5px; border: 1px solid #49545a;
            border-radius: 3px; background: #14191c; selection-background-color: #397f80;
        }
        QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus,
        QDoubleSpinBox:focus, QComboBox:focus { border-color: #69d6cb; }
        QComboBox::drop-down { border: 0; width: 20px; }
        QComboBox QAbstractItemView { background: #1c2225; border: 1px solid #536066; }
        QListView, QListWidget, QTableView, QTableWidget {
            background: #111619; alternate-background-color: #192024;
            border: 1px solid #414b50; border-radius: 3px;
            selection-background-color: #397f80;
        }
        QHeaderView::section {
            background: #2b3337; border: 0; border-right: 1px solid #424c51;
            border-bottom: 1px solid #424c51; padding: 5px; font-weight: 600;
        }
        QCheckBox { spacing: 7px; }
        QCheckBox::indicator { width: 15px; height: 15px; }
        QCheckBox::indicator:unchecked {
            background: #111619; border: 1px solid #59656a; border-radius: 3px;
        }
        QCheckBox::indicator:checked {
            background: #4ab3aa; border: 1px solid #8ae5dc; border-radius: 3px;
        }
        QSlider::groove:horizontal {
            height: 5px; background: #111619; border: 1px solid #394247; border-radius: 3px;
        }
        QSlider::sub-page:horizontal { background: #4cb8ae; border-radius: 3px; }
        QSlider::handle:horizontal {
            width: 14px; margin: -6px 0; border-radius: 7px;
            border: 1px solid #111517; background: #aab1b4;
        }
        QScrollBar:vertical { width: 12px; margin: 0; background: #171c1f; }
        QScrollBar::handle:vertical { min-height: 28px; margin: 2px; border-radius: 4px; background: #4b565b; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QProgressBar {
            border: 1px solid #465157; border-radius: 3px; text-align: center;
            background: #111619;
        }
        QProgressBar::chunk { background: #4cb8ae; }
        QStatusBar { background: #1a2023; border-top: 1px solid #0e1214; }
        QToolTip { color: #f2f4f4; background: #101416; border: 1px solid #66747a; padding: 4px; }
    )PVT_QSS"));
}

} // namespace

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
    apply_studio_theme(application);

    MainWindow window;
    const QStringList arguments = application.arguments();
    if (arguments.contains(QStringLiteral("--smoke-test"))) {
        QString smoke_error;
        if (!window.runSmokeChecks(&smoke_error)) {
            // Qt routes GUI-application diagnostics to the Windows debugger,
            // which leaves native CI logs blank. Write the failure through
            // the inherited standard-error handle as well so CTest and
            // packaged smoke checks retain the actionable assertion.
            const QByteArray utf8_error = smoke_error.toUtf8();
            std::fputs(utf8_error.constData(), stderr);
            std::fputc('\n', stderr);
            std::fflush(stderr);
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

    const bool startup_live = arguments.contains(QStringLiteral("--live"));
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
    if (!startup_project.isEmpty() || startup_live) {
        QTimer::singleShot(0, &window,
                          [&window, startup_project, startup_live] {
            if (!startup_project.isEmpty()) window.openProject(startup_project);
            if (startup_live) window.openLiveMode();
        });
    }
    return application.exec();
}
