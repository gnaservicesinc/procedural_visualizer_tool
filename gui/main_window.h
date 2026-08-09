#ifndef PVT_MAIN_WINDOW_H
#define PVT_MAIN_WINDOW_H

#include "procedural_visualizer_tool.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QString>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QPlainTextEdit;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class QUndoStack;
class PreviewWidget;

namespace pvt { struct ProjectDocument; }

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool runSmokeChecks(QString* error = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct PreviewResult {
        QImage image;
        QString error;
        int frame = 0;
        std::uint64_t generation = 0;
        std::uint64_t document_revision = 0;
    };

    struct ExportResult {
        bool ok = false;
        bool cancelled = false;
        QString error;
    };

    enum class SavedProjectRenameAction {
        Cancel,
        KeepBundleFilename,
        SaveCopyAndOpen,
        SaveCopyAndStay
    };

    QWidget* createWavePage();
    QWidget* createSwingPage();
    QWidget* createEffectPage();
    QWidget* createLayerSettingsPage();
    QWidget* createOutputPage();
    QWidget* createVersionsPage();
    QWidget* createTimeline();
    void createLayerDock();
    void createToolbar();
    void connectEditors();

    void refreshAll();
    void refreshWaveList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void refreshSwingList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void refreshEffectList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void updateTimelineState();
    void loadSelectedWave();
    void loadSelectedSwing();
    void loadSelectedEffect();
    void loadGlobalEditors();
    void applyWaveEditor(const QObject* changedEditor);
    void applySwingEditor(const QObject* changedEditor);
    void applyEffectEditor(const QObject* changedEditor);
    void applyGlobalEditor(const QObject* changedEditor);
    void ensureAlphaForTransparency();
    bool outputEditorsValid(QString* error = nullptr) const;
    void updateOutputEditorValidity();
    void updateWaveListItem(std::size_t index);
    void updateSwingListItem(std::size_t index);
    void updateEffectListItem(std::size_t index);
    void updateEffectEditorVisibility();
    void refreshPaletteEditor();
    void applyPalettePreset(std::size_t index);
    void addPaletteColor();
    void editSelectedPaletteColor();
    void removeSelectedPaletteColor();

    pvt::LayerConfig* activeLayer();
    const pvt::LayerConfig* activeLayer() const;
    pvt::LayerConfig* findLayer(const std::string& uuid);
    const pvt::LayerConfig* findLayer(const std::string& uuid) const;
    void selectLayer(const std::string& uuid);
    void loadActiveConfiguration();
    void syncActiveRender();
    void syncProjectGlobals();
    void refreshLayerList();
    void loadLayerEditors();
    void addLayer();
    void duplicateLayer();
    void removeLayer();
    void moveActiveLayer(int direction);
    void updateWindowTitle();
    void updateCompatibilityWarning();
    void noteDocumentChange();
    bool hasUnsavedChanges() const;
    bool confirmDiscardChanges();
    void restoreUserSettings();
    void saveUserSettings();
    void editUndoLimit();
    bool documentReplacementAllowed(QString* error = nullptr);
    void refreshVersionsPage();
    void refreshVersionDiff();
    void makeSelectedVersionCurrent();
    void revertSelectedVersion();
    bool loadProjectPath(const QString& path, QString* error = nullptr);

    struct ActiveDocumentState {
        pvt::RenderData render;
        pvt::CanvasLoopConfig canvas;
        pvt::ExportConfig output;
    };
    ActiveDocumentState captureActiveState() const;
    void restoreActiveState(const std::string& layerUuid,
                            const ActiveDocumentState& state);
    void recordActiveStateChange(const QString& text,
                                 ActiveDocumentState before,
                                 const QString& mergeKey = {});
    void restoreProjectState(const pvt::ProjectConfig& state,
                             const std::string& activeLayerUuid);
    void recordProjectStateChange(const QString& text,
                                  pvt::ProjectConfig before,
                                  const std::string& beforeActiveLayerUuid);
    void recordUndo(const QString& text,
                    std::function<void()> undo,
                    std::function<void()> redo,
                    const QString& mergeKey = {},
                    std::size_t estimatedPayloadBytes = 0U);
    void clearUndoHistory(bool preserveDirtyState);

    std::optional<std::size_t> selectedWaveIndex() const;
    std::optional<std::size_t> selectedSwingIndex() const;
    std::optional<std::size_t> selectedEffectIndex() const;
    void moveSelectedWave(int direction);
    void moveSelectedSwing(int direction);
    void moveSelectedEffect(int direction);

    void schedulePreview();
    void startPreview();
    pvt::ProjectConfig previewProjectSnapshot() const;
    static PreviewResult generatePreview(pvt::ProjectConfig project, int frame,
                                         std::uint64_t generation,
                                         std::uint64_t documentRevision,
                                         int test_delay_ms,
                                         const std::shared_ptr<std::atomic_bool>& cancel);
    void randomizeExistingStackSettings();
    void randomizeStackComposition();
    QString resolvedOutputDirectory(const QString& path) const;
    QString usableDialogDirectory(const QString& preferred = {}) const;
    void rememberDialogLocation(const QString& selectedPath);
    bool startExport();
    void finishProjectNameEdit();
    void applyProjectNameChange(const std::string& before,
                                const std::string& after);
    SavedProjectRenameAction promptForSavedProjectRename(
        const std::string& before, const std::string& after);
    QString chooseIndependentCopyPath(const std::string& projectName);
    bool saveIndependentRenamedCopy(const std::string& projectName,
                                    const QString& path,
                                    bool openCopy,
                                    QString* error = nullptr);
    void saveSetup();
    void saveSetupAs();
    bool saveProjectPath(const QString& path);
    void loadSetup();
    bool loadSetupFile(const QString& path, QString* error = nullptr);

    pvt::ProjectConfig project_;
    std::unique_ptr<pvt::ProjectDocument> document_;
    // Materialized active-layer view retained to keep the existing editors and
    // draggable-wave overlay independent from project-global storage.
    pvt::RenderConfig config_;
    std::string active_layer_uuid_;
    std::optional<std::string> solo_layer_uuid_;
    bool populating_ = false;
    bool restoring_undo_ = false;
    bool baseline_dirty_ = false;
    bool preview_deferred_ = false;
    bool integer_dither_preference_ = true;
    bool export_active_ = false;
    bool close_after_export_ = false;
    bool playback_preview_advanced_ = false;
    int last_previewed_frame_ = -1;
    int preview_test_delay_ms_ = 0;
    QString independent_copy_test_path_;
    std::uint64_t preview_generation_ = 0;
    std::uint64_t document_revision_ = 1;
    struct ItemDragState {
        std::string layer_uuid;
        std::uint64_t item_id = 0;
        ActiveDocumentState before;
        bool moved = false;
    };
    std::optional<ItemDragState> wave_drag_state_;
    std::optional<ItemDragState> swing_drag_state_;
    std::optional<ItemDragState> effect_drag_state_;
    std::size_t undo_history_estimated_bytes_ = 0U;
    std::shared_ptr<std::atomic_bool> preview_cancel_;
    std::atomic_bool cancel_export_{false};
    QString startup_working_directory_;
    QString last_dialog_directory_;
    QString current_project_path_;
    QString imported_legacy_path_;
    QString compatibility_warning_;

    PreviewWidget* preview_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* frame_label_ = nullptr;
    QSlider* timeline_ = nullptr;
    QPushButton* play_button_ = nullptr;
    QTimer* preview_timer_ = nullptr;
    QTimer* playback_timer_ = nullptr;
    QFutureWatcher<PreviewResult>* preview_watcher_ = nullptr;
    QFutureWatcher<ExportResult>* export_watcher_ = nullptr;
    QUndoStack* undo_stack_ = nullptr;

    QDockWidget* layers_dock_ = nullptr;
    QListWidget* layer_list_ = nullptr;
    QLineEdit* project_name_ = nullptr;
    QLabel* compatibility_warning_label_ = nullptr;
    QLineEdit* layer_name_ = nullptr;
    QCheckBox* layer_enabled_ = nullptr;
    QCheckBox* layer_solo_ = nullptr;
    QComboBox* layer_blend_ = nullptr;
    QDoubleSpinBox* layer_opacity_ = nullptr;

    QListWidget* version_list_ = nullptr;
    QComboBox* version_before_ = nullptr;
    QComboBox* version_after_ = nullptr;
    QPlainTextEdit* version_diff_ = nullptr;
    QLabel* version_summary_ = nullptr;
    QPushButton* version_make_current_ = nullptr;
    QPushButton* version_revert_ = nullptr;

    QListWidget* wave_list_ = nullptr;
    QLineEdit* wave_name_ = nullptr;
    QCheckBox* wave_enabled_ = nullptr;
    QCheckBox* wave_sync_ = nullptr;
    QDoubleSpinBox* wave_x_ = nullptr;
    QDoubleSpinBox* wave_y_ = nullptr;
    QDoubleSpinBox* wave_amplitude_ = nullptr;
    QDoubleSpinBox* wave_frequency_ = nullptr;
    QSpinBox* wave_cycles_ = nullptr;
    QDoubleSpinBox* wave_phase_ = nullptr;
    QDoubleSpinBox* wave_direction_ = nullptr;

    QListWidget* swing_list_ = nullptr;
    QLineEdit* swing_name_ = nullptr;
    QCheckBox* swing_enabled_ = nullptr;
    QComboBox* swing_waveform_ = nullptr;
    QDoubleSpinBox* swing_amount_ = nullptr;
    QSpinBox* swing_cycles_ = nullptr;
    QDoubleSpinBox* swing_phase_ = nullptr;
    QDoubleSpinBox* swing_shape_ = nullptr;
    QDoubleSpinBox* swing_center_x_ = nullptr;
    QDoubleSpinBox* swing_center_y_ = nullptr;
    QDoubleSpinBox* swing_radius_ = nullptr;

    QListWidget* effect_list_ = nullptr;
    QComboBox* add_effect_type_ = nullptr;
    QLineEdit* effect_name_ = nullptr;
    QCheckBox* effect_enabled_ = nullptr;
    QCheckBox* effect_sync_ = nullptr;
    QComboBox* effect_type_ = nullptr;
    QComboBox* effect_space_ = nullptr;
    QSpinBox* effect_cycles_ = nullptr;
    QDoubleSpinBox* effect_phase_ = nullptr;
    QComboBox* effect_edge_ = nullptr;
    QDoubleSpinBox* effect_intensity_ = nullptr;
    QDoubleSpinBox* effect_magnitude_ = nullptr;
    QDoubleSpinBox* effect_frequency_ = nullptr;
    QDoubleSpinBox* effect_secondary_ = nullptr;
    QDoubleSpinBox* effect_center_x_ = nullptr;
    QDoubleSpinBox* effect_center_y_ = nullptr;
    QDoubleSpinBox* effect_angle_ = nullptr;
    QDoubleSpinBox* effect_radius_ = nullptr;
    QDoubleSpinBox* effect_threshold_ = nullptr;
    QDoubleSpinBox* effect_knee_ = nullptr;
    QDoubleSpinBox* effect_area_radius_ = nullptr;
    QFormLayout* effect_form_ = nullptr;

    QSpinBox* width_ = nullptr;
    QSpinBox* height_ = nullptr;
    QSpinBox* block_size_ = nullptr;
    QSpinBox* frames_ = nullptr;
    QDoubleSpinBox* fps_ = nullptr;
    QDoubleSpinBox* phrase_warp_ = nullptr;
    QDoubleSpinBox* ghost_mix_ = nullptr;
    QDoubleSpinBox* ghost_lag_ = nullptr;
    QCheckBox* displacement_enabled_ = nullptr;
    QDoubleSpinBox* displacement_ = nullptr;
    QCheckBox* lighting_enabled_ = nullptr;
    QDoubleSpinBox* wave_depth_ = nullptr;
    QCheckBox* spiral_enabled_ = nullptr;
    QDoubleSpinBox* spiral_frequency_ = nullptr;
    QSpinBox* spiral_arms_ = nullptr;
    QCheckBox* wall_enabled_ = nullptr;
    QDoubleSpinBox* wall_frequency_ = nullptr;
    QDoubleSpinBox* wall_mix_ = nullptr;
    QSpinBox* hue_cycles_ = nullptr;
    QDoubleSpinBox* saturation_ = nullptr;
    QCheckBox* surface_enabled_ = nullptr;
    QComboBox* surface_mapping_ = nullptr;
    QLineEdit* surface_obj_path_ = nullptr;
    QSpinBox* surface_rotations_ = nullptr;
    QDoubleSpinBox* surface_phase_ = nullptr;
    QDoubleSpinBox* surface_curvature_ = nullptr;
    QDoubleSpinBox* surface_lighting_ = nullptr;
    QCheckBox* transform_flip_horizontal_ = nullptr;
    QCheckBox* transform_flip_vertical_ = nullptr;
    QComboBox* transform_mirror_ = nullptr;
    QCheckBox* palette_enabled_ = nullptr;
    QLineEdit* palette_name_ = nullptr;
    QComboBox* palette_preset_ = nullptr;
    QListWidget* palette_colors_ = nullptr;
    QCheckBox* quantization_enabled_ = nullptr;
    QSpinBox* quantization_levels_ = nullptr;
    QDoubleSpinBox* quantization_mix_ = nullptr;
    QComboBox* quantization_mode_ = nullptr;
    QCheckBox* alpha_enabled_ = nullptr;
    QDoubleSpinBox* alpha_minimum_ = nullptr;
    QDoubleSpinBox* alpha_maximum_ = nullptr;
    QDoubleSpinBox* alpha_frequency_ = nullptr;
    QSpinBox* alpha_cycles_ = nullptr;
    QDoubleSpinBox* alpha_phase_ = nullptr;
    QComboBox* bit_depth_ = nullptr;
    QSpinBox* png_compression_ = nullptr;
    QCheckBox* dither_enabled_ = nullptr;
    QComboBox* dither_method_ = nullptr;
    QCheckBox* write_alpha_ = nullptr;
    QLineEdit* output_directory_ = nullptr;
    QLineEdit* prefix_ = nullptr;
    QSpinBox* first_frame_ = nullptr;
    QSpinBox* filename_digits_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
};

#endif
