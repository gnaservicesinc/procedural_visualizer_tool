#ifndef PVT_APPLICATION_SETTINGS_DIALOG_H
#define PVT_APPLICATION_SETTINGS_DIALOG_H

#include "performance_settings.h"
#include "procedural_visualizer_tool.h"

#include <QDialog>

class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QShowEvent;
class QResizeEvent;

class ApplicationSettingsDialog final : public QDialog {
    Q_OBJECT
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
    QString language() const;
    PerformanceSettings performanceSettings() const;
    int recentProjectLimit() const;
    NewProjectDefaultsAction newProjectDefaultsAction() const;

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void scheduleResponsiveLabelLayout();

    QSpinBox* undo_limit_ = nullptr;
    QComboBox* language_ = nullptr;
    QComboBox* render_backend_ = nullptr;
    QSpinBox* preview_live_cpu_workers_ = nullptr;
    QSpinBox* export_frame_workers_ = nullptr;
    QSpinBox* export_cpu_workers_ = nullptr;
    QSpinBox* gpu_frames_in_flight_ = nullptr;
    QComboBox* render_memory_budget_mode_ = nullptr;
    QDoubleSpinBox* render_memory_budget_value_ = nullptr;
    QLabel* render_memory_budget_status_ = nullptr;
    QSpinBox* maximum_decoded_image_mib_ = nullptr;
    QSpinBox* maximum_obj_file_mib_ = nullptr;
    QSpinBox* maximum_obj_mesh_mib_ = nullptr;
    QSpinBox* maximum_project_bundle_mib_ = nullptr;
    QSpinBox* source_image_cache_mib_ = nullptr;
    QSpinBox* source_image_cache_entries_ = nullptr;
    QSpinBox* obj_mesh_cache_mib_ = nullptr;
    QSpinBox* obj_mesh_cache_entries_ = nullptr;
    QSpinBox* displacement_mesh_cache_mib_ = nullptr;
    QSpinBox* displacement_mesh_cache_entries_ = nullptr;
    QLabel* resource_limits_status_ = nullptr;
    QCheckBox* pause_editor_preview_during_export_ = nullptr;
    QSpinBox* recent_project_limit_ = nullptr;
    QLabel* defaults_status_ = nullptr;
    NewProjectDefaultsAction defaults_action_ =
        NewProjectDefaultsAction::Keep;
    bool responsive_label_layout_pending_ = false;
};

#endif
