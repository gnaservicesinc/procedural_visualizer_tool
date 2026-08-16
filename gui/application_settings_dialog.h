#ifndef PVT_APPLICATION_SETTINGS_DIALOG_H
#define PVT_APPLICATION_SETTINGS_DIALOG_H

#include "procedural_visualizer_tool.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QSpinBox;

class ApplicationSettingsDialog final : public QDialog {
public:
    enum class NewProjectDefaultsAction {
        Keep,
        SaveCurrentProject,
        RestoreBuiltIn
    };

    ApplicationSettingsDialog(int undoLimit, pvt::RenderBackend renderBackend,
                              int recentProjectLimit,
                              bool hasCustomNewProjectDefaults,
                              QWidget* parent = nullptr);

    int undoLimit() const;
    pvt::RenderBackend renderBackend() const;
    int recentProjectLimit() const;
    NewProjectDefaultsAction newProjectDefaultsAction() const;

private:
    QSpinBox* undo_limit_ = nullptr;
    QComboBox* render_backend_ = nullptr;
    QSpinBox* recent_project_limit_ = nullptr;
    QLabel* defaults_status_ = nullptr;
    NewProjectDefaultsAction defaults_action_ =
        NewProjectDefaultsAction::Keep;
};

#endif
