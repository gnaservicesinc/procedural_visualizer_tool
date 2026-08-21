#ifndef PVT_APPLICATION_SETTINGS_DIALOG_H
#define PVT_APPLICATION_SETTINGS_DIALOG_H

#include "performance_settings.h"
#include "procedural_visualizer_tool.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QSpinBox;
class QShowEvent;

class ApplicationSettingsDialog final : public QDialog {
public:
    enum class NewProjectDefaultsAction {
        Keep,
        SaveCurrentProject,
        RestoreBuiltIn
    };

    ApplicationSettingsDialog(int undoLimit,
                              const PerformanceSettings& performanceSettings,
                              int recentProjectLimit,
                              bool hasCustomNewProjectDefaults,
                              QWidget* parent = nullptr,
                              const pvt::RendererCapabilities*
                                  capabilitiesOverride = nullptr);

    int undoLimit() const;
    PerformanceSettings performanceSettings() const;
    int recentProjectLimit() const;
    NewProjectDefaultsAction newProjectDefaultsAction() const;

private:
    QSpinBox* undo_limit_ = nullptr;
    QComboBox* render_backend_ = nullptr;
    QSpinBox* preview_live_cpu_workers_ = nullptr;
    QSpinBox* export_frame_workers_ = nullptr;
    QSpinBox* export_cpu_workers_ = nullptr;
    QSpinBox* gpu_frames_in_flight_ = nullptr;
    QSpinBox* render_memory_budget_mib_ = nullptr;
    QSpinBox* recent_project_limit_ = nullptr;
    QLabel* defaults_status_ = nullptr;
    NewProjectDefaultsAction defaults_action_ =
        NewProjectDefaultsAction::Keep;

protected:
    void showEvent(QShowEvent* event) override;
};

#endif
