#ifndef PVT_MAIN_WINDOW_H
#define PVT_MAIN_WINDOW_H

#include "procedural_visualizer_tool.h"
#include "../src/project_bundle.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QString>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QAction;
class QCloseEvent;
class QComboBox;
class QDockWidget;
class QDoubleSpinBox;
class QEvent;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QPlainTextEdit;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class QUndoStack;
class QWidget;
class PreviewWidget;

namespace pvt::audio {
class AudioPlayback;
}

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
    bool runSmokeChecks(QString* error = nullptr);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

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
        QString success_message;
    };

    enum class ProjectIoOperation {
        Load,
        Save
    };

    struct ProjectIoResult {
        bool ok = false;
        ProjectIoOperation operation = ProjectIoOperation::Load;
        QString path;
        QString error;
        std::shared_ptr<pvt::ProjectDocument> document;
        pvt::BundleSaveReport save_report;
    };

    struct VersionDiffResult {
        bool ok = false;
        QString error;
        std::vector<pvt::BundleDiffEntry> differences;
        std::uint64_t before = 0U;
        std::uint64_t after = 0U;
        std::uint64_t document_revision = 0U;
    };

    enum class MusicAnalysisAction {
        Choose,
        Relink,
        Reanalyze
    };

    struct MusicAnalysisResult {
        bool ok = false;
        bool cancelled = false;
        bool verified_only = false;
        QString error;
        QString source_path;
        pvt::MusicAnalysis analysis;
        pvt::ProjectAttachment attached;
        std::shared_ptr<pvt::ProjectDocument> staged_document;
        MusicAnalysisAction action = MusicAnalysisAction::Choose;
        bool layer_clock = false;
        std::string layer_uuid;
        std::uint64_t generation = 0;
        std::uint64_t document_revision = 0;
    };

    enum class SavedProjectRenameAction {
        Cancel,
        KeepBundleFilename,
        SaveCopyAndOpen,
        SaveCopyAndStay
    };

    QWidget* createWavePage();
    QWidget* createSynchronizationPage();
    QWidget* createSwingBlock();
    QWidget* createEffectPage();
    QWidget* createLayerSettingsPage();
    QWidget* createOutputPage();
    QWidget* createVersionsPage();
    QWidget* createTimeline();
    void createLayerDock();
    void restoreLayersDock(bool makeVisible = true);
    void createToolbar();
    void connectEditors();

    void refreshAll();
    void refreshWaveList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void refreshSwingList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void refreshEffectList(std::optional<std::uint64_t> selectedId = std::nullopt);
    void updateTimelineState();
    void updateSynchronizationState();
    void updateMusicSummary();
    void updateExportAvailability();
    void finishExportUiState();
    void updateTimelineReadout();
    void togglePlayback();
    void stopPlayback();
    void startProjectAudioPlayback();
    void loadSelectedWave();
    void loadSelectedSwing();
    void loadSelectedEffect();
    void loadGlobalEditors();
    void applyWaveEditor(const QObject* changedEditor);
    void applySwingEditor(const QObject* changedEditor);
    void applyEffectEditor(const QObject* changedEditor);
    void applyGlobalEditor(const QObject* changedEditor);
    void applyClockEditor(const QObject* changedEditor);
    void applyAudioReactiveEditor(const QObject* changedEditor);
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
    pvt::LayerGroup* findGroup(const std::string& uuid);
    const pvt::LayerGroup* findGroup(const std::string& uuid) const;
    const pvt::LayerGroup* groupForLayer(const pvt::LayerConfig& layer) const;
    void selectLayer(const std::string& uuid);
    void selectGroup(const std::string& uuid);
    void loadActiveConfiguration();
    void syncActiveRender();
    void syncProjectGlobals();
    void refreshLayerList();
    void loadLayerEditors();
    void addLayer();
    void duplicateLayer();
    void removeLayer();
    void moveActiveLayer(int direction);
    void addGroup();
    void removeSelectedGroup();
    void moveSelectedGroup(int direction);
    void setActiveLayerGroup(const std::string& groupUuid);
    bool setSurfaceObjSource(const QString& sourcePath);
    bool setStartingImageSource(const QString& sourcePath);
    void updateWindowTitle();
    void updateCompatibilityWarning();
    void noteDocumentChange();
    bool hasUnsavedChanges() const;
    bool confirmDiscardChanges(std::function<void()> after_save = {});
    void restoreUserSettings();
    void saveUserSettings();
    void showApplicationSettings();
    void showAboutDialog();
    void showMotionPathEditor();
    bool hasCustomNewProjectDefaults() const;
    std::unique_ptr<pvt::ProjectDocument> makeNewProjectDocument(
        QString* warning = nullptr) const;
    bool saveCurrentProjectAsDefaults(QString* error = nullptr);
    bool restoreBuiltInProjectDefaults(QString* error = nullptr);
    void replaceWithNewProject();
    bool documentReplacementAllowed(QString* error = nullptr);
    void refreshVersionsPage();
    void refreshVersionDiff();
    void startVersionDiff();
    void makeSelectedVersionCurrent();
    void revertSelectedVersion();
    bool loadProjectPath(const QString& path, QString* error = nullptr);
    bool adoptLoadedProject(pvt::ProjectDocument loaded,
                            QString* error = nullptr);
    void startProjectLoad(const QString& path);
    void startProjectSave(const QString& path);
    void setProjectIoActive(bool active, const QString& message = {});
    void finishProjectSave(pvt::ProjectDocument saved,
                           const pvt::BundleSaveReport& report,
                           const QString& path);

    struct ActiveDocumentState {
        pvt::RenderData render;
        pvt::CanvasLoopConfig canvas;
        pvt::ExportConfig output;
        std::vector<pvt::ProjectAttachment> attachments;
        std::shared_ptr<pvt::ProjectAttachmentCache> attachment_cache;
    };
    struct ProjectDocumentState {
        pvt::ProjectConfig project;
        std::vector<pvt::ProjectAttachment> attachments;
        std::shared_ptr<pvt::ProjectAttachmentCache> attachment_cache;
    };
    ActiveDocumentState captureActiveState() const;
    ProjectDocumentState captureProjectState() const;
    void restoreActiveState(const std::string& layerUuid,
                            const ActiveDocumentState& state);
    void recordActiveStateChange(const QString& text,
                                 ActiveDocumentState before,
                                 const QString& mergeKey = {});
    void restoreProjectState(const ProjectDocumentState& state,
                             const std::string& activeLayerUuid);
    void recordProjectStateChange(const QString& text,
                                  ProjectDocumentState before,
                                  const std::string& beforeActiveLayerUuid);
    void updateMusicTransactionGuards();
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
                                         pvt::FrameRenderOptions renderOptions,
                                         const std::shared_ptr<std::atomic_bool>& cancel);
    pvt::FrameRenderOptions frameRenderOptions() const;
    void randomizeExistingStackSettings();
    void randomizeStackComposition();
    QString resolvedOutputDirectory(const QString& path) const;
    QString usableDialogDirectory(const QString& preferred = {}) const;
    void rememberDialogLocation(const QString& selectedPath);
    bool startExport();
    bool startCurrentFrameExport(const QString& path);
    bool startVideoExport();
    bool startMusicAnalysis(const QString& sourcePath,
                            MusicAnalysisAction action,
                            bool layerClock = false);
    void finishMusicAnalysis(const MusicAnalysisResult& result);
    void cancelMusicAnalysis(const QString& message = {});
    void chooseMusicSource();
    void relinkMusicSource();
    void reanalyzeMusicSource();
    void clearMusicSource();
    void chooseLayerMusicSource();
    void relinkLayerMusicSource();
    void reanalyzeLayerMusicSource();
    void clearLayerMusicSource();
    QString currentMusicSourcePath(bool layerClock = false) const;
    int effectiveFrameCount(QString* error = nullptr) const;
    bool musicRenderReady() const;
    void navigateToBeat(int direction);
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
    std::optional<std::string> selected_group_uuid_;
    std::optional<std::string> solo_layer_uuid_;
    std::optional<std::string> solo_group_uuid_;
    bool populating_ = false;
    bool restoring_undo_ = false;
    bool baseline_dirty_ = false;
    bool preview_deferred_ = false;
    bool integer_dither_preference_ = true;
    bool export_active_ = false;
    bool close_after_export_ = false;
    bool playback_preview_advanced_ = false;
    pvt::RenderBackend render_backend_ = pvt::RenderBackend::CpuAndGpu;
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
    std::shared_ptr<std::atomic_bool> music_analysis_cancel_;
    std::atomic_bool cancel_export_{false};
    std::uint64_t music_analysis_generation_ = 0;
    bool music_analysis_active_ = false;
    bool music_analysis_layer_clock_ = false;
    bool project_io_active_ = false;
    bool close_after_project_io_ = false;
    std::function<void()> project_io_success_continuation_;
    QString startup_working_directory_;
    QString last_dialog_directory_;
    QString current_project_path_;
    QString imported_legacy_path_;
    QString compatibility_warning_;
    QString custom_defaults_load_warning_;

    std::unique_ptr<pvt::audio::AudioPlayback> audio_playback_;

    PreviewWidget* preview_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QWidget* wave_page_ = nullptr;
    QWidget* synchronization_page_ = nullptr;
    QWidget* effect_page_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* export_progress_ = nullptr;
    QProgressBar* project_io_progress_ = nullptr;
    QLabel* frame_label_ = nullptr;
    QSlider* timeline_ = nullptr;
    QSlider* audio_volume_ = nullptr;
    QPushButton* play_button_ = nullptr;
    QPushButton* previous_beat_ = nullptr;
    QPushButton* next_beat_ = nullptr;
    QTimer* preview_timer_ = nullptr;
    QTimer* playback_timer_ = nullptr;
    QFutureWatcher<PreviewResult>* preview_watcher_ = nullptr;
    QFutureWatcher<ExportResult>* export_watcher_ = nullptr;
    QFutureWatcher<MusicAnalysisResult>* music_analysis_watcher_ = nullptr;
    QFutureWatcher<ProjectIoResult>* project_io_watcher_ = nullptr;
    QFutureWatcher<VersionDiffResult>* version_diff_watcher_ = nullptr;
    QUndoStack* undo_stack_ = nullptr;
    QAction* export_action_ = nullptr;
    QAction* current_frame_export_action_ = nullptr;
    QAction* video_export_action_ = nullptr;
    QAction* cancel_export_action_ = nullptr;
    QAction* new_action_ = nullptr;
    QAction* open_action_ = nullptr;
    QAction* open_folder_action_ = nullptr;
    QAction* save_action_ = nullptr;
    QAction* save_as_action_ = nullptr;
    QAction* randomize_values_action_ = nullptr;
    QAction* randomize_mix_action_ = nullptr;
    QAction* undo_action_ = nullptr;
    QAction* redo_action_ = nullptr;
    QAction* settings_action_ = nullptr;
    QAction* about_action_ = nullptr;
    QAction* restore_layers_dock_action_ = nullptr;

    QDockWidget* layers_dock_ = nullptr;
    QListWidget* layer_list_ = nullptr;
    QLineEdit* project_name_ = nullptr;
    QLabel* compatibility_warning_label_ = nullptr;
    QLineEdit* layer_name_ = nullptr;
    QCheckBox* layer_enabled_ = nullptr;
    QCheckBox* layer_solo_ = nullptr;
    QComboBox* layer_blend_ = nullptr;
    QComboBox* layer_alpha_mode_ = nullptr;
    QComboBox* layer_group_ = nullptr;
    QDoubleSpinBox* layer_opacity_ = nullptr;
    QGroupBox* selected_group_box_ = nullptr;
    QLineEdit* group_name_ = nullptr;
    QCheckBox* group_enabled_ = nullptr;
    QCheckBox* group_solo_ = nullptr;
    QCheckBox* group_locked_ = nullptr;

    QListWidget* version_list_ = nullptr;
    QComboBox* version_before_ = nullptr;
    QComboBox* version_after_ = nullptr;
    QPushButton* version_compare_ = nullptr;
    QPlainTextEdit* version_diff_ = nullptr;
    QLabel* version_summary_ = nullptr;
    QPushButton* version_make_current_ = nullptr;
    QPushButton* version_revert_ = nullptr;

    QListWidget* wave_list_ = nullptr;
    QLineEdit* wave_name_ = nullptr;
    QCheckBox* wave_enabled_ = nullptr;
    QCheckBox* wave_sync_ = nullptr;
    QComboBox* wave_audio_response_ = nullptr;
    QFormLayout* wave_form_ = nullptr;
    QDoubleSpinBox* wave_x_ = nullptr;
    QDoubleSpinBox* wave_y_ = nullptr;
    QDoubleSpinBox* wave_amplitude_ = nullptr;
    QDoubleSpinBox* wave_frequency_ = nullptr;
    QSpinBox* wave_cycles_ = nullptr;
    QDoubleSpinBox* wave_phase_ = nullptr;
    QDoubleSpinBox* wave_direction_ = nullptr;

    QGroupBox* clock_group_ = nullptr;
    QFormLayout* clock_form_ = nullptr;
    QComboBox* clock_mode_ = nullptr;
    QComboBox* clock_interpolation_ = nullptr;
    QComboBox* clock_fit_ = nullptr;
    QSpinBox* clock_frame_interval_ = nullptr;
    QDoubleSpinBox* clock_time_interval_ms_ = nullptr;
    QLineEdit* meter_expression_ = nullptr;
    QLabel* meter_summary_ = nullptr;
    QDoubleSpinBox* meter_bpm_ = nullptr;
    QSpinBox* meter_tempo_note_ = nullptr;
    QCheckBox* clock_reverse_ = nullptr;
    QDoubleSpinBox* clock_phase_offset_ = nullptr;
    QComboBox* music_tempo_mode_ = nullptr;
    QDoubleSpinBox* music_beat_offset_ms_ = nullptr;
    QCheckBox* music_data_only_ = nullptr;
    QLineEdit* music_source_ = nullptr;
    QLabel* music_summary_ = nullptr;
    QLabel* music_error_ = nullptr;
    QProgressBar* music_progress_ = nullptr;
    QPushButton* music_choose_ = nullptr;
    QPushButton* music_relink_ = nullptr;
    QPushButton* music_reanalyze_ = nullptr;
    QPushButton* music_clear_ = nullptr;
    QPushButton* music_cancel_ = nullptr;

    QGroupBox* layer_clock_group_ = nullptr;
    QFormLayout* layer_clock_form_ = nullptr;
    QComboBox* layer_clock_scale_ = nullptr;
    QComboBox* layer_clock_mode_ = nullptr;
    QComboBox* layer_clock_interpolation_ = nullptr;
    QComboBox* layer_clock_fit_ = nullptr;
    QSpinBox* layer_clock_frame_interval_ = nullptr;
    QDoubleSpinBox* layer_clock_time_interval_ms_ = nullptr;
    QLineEdit* layer_meter_expression_ = nullptr;
    QLabel* layer_meter_summary_ = nullptr;
    QDoubleSpinBox* layer_meter_bpm_ = nullptr;
    QSpinBox* layer_meter_tempo_note_ = nullptr;
    QCheckBox* layer_clock_reverse_ = nullptr;
    QDoubleSpinBox* layer_clock_phase_offset_ = nullptr;
    QComboBox* layer_music_tempo_mode_ = nullptr;
    QDoubleSpinBox* layer_music_beat_offset_ms_ = nullptr;
    QCheckBox* layer_music_data_only_ = nullptr;
    QLineEdit* layer_music_source_ = nullptr;
    QLabel* layer_music_summary_ = nullptr;
    QLabel* layer_music_error_ = nullptr;
    QProgressBar* layer_music_progress_ = nullptr;
    QPushButton* layer_music_choose_ = nullptr;
    QPushButton* layer_music_relink_ = nullptr;
    QPushButton* layer_music_reanalyze_ = nullptr;
    QPushButton* layer_music_clear_ = nullptr;
    QPushButton* layer_music_cancel_ = nullptr;

    QGroupBox* swings_group_ = nullptr;
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

    QGroupBox* project_audio_response_group_ = nullptr;
    QCheckBox* project_audio_sync_only_ = nullptr;
    QCheckBox* project_audio_waves_enabled_ = nullptr;
    QComboBox* project_audio_wave_source_ = nullptr;
    QDoubleSpinBox* project_audio_wave_amount_ = nullptr;
    QCheckBox* project_audio_effects_enabled_ = nullptr;
    QComboBox* project_audio_effect_source_ = nullptr;
    QDoubleSpinBox* project_audio_effect_amount_ = nullptr;
    QCheckBox* project_audio_color_enabled_ = nullptr;
    QComboBox* project_audio_color_source_ = nullptr;
    QDoubleSpinBox* project_audio_color_amount_ = nullptr;

    QGroupBox* audio_response_group_ = nullptr;
    QCheckBox* audio_response_enabled_ = nullptr;
    QLabel* audio_response_effective_ = nullptr;
    QPushButton* audio_copy_project_ = nullptr;
    QCheckBox* audio_sync_only_ = nullptr;
    QCheckBox* audio_waves_enabled_ = nullptr;
    QComboBox* audio_wave_source_ = nullptr;
    QDoubleSpinBox* audio_wave_amount_ = nullptr;
    QCheckBox* audio_effects_enabled_ = nullptr;
    QComboBox* audio_effect_source_ = nullptr;
    QDoubleSpinBox* audio_effect_amount_ = nullptr;
    QCheckBox* audio_color_enabled_ = nullptr;
    QComboBox* audio_color_source_ = nullptr;
    QDoubleSpinBox* audio_color_amount_ = nullptr;

    QListWidget* effect_list_ = nullptr;
    QComboBox* add_effect_type_ = nullptr;
    QLineEdit* effect_name_ = nullptr;
    QCheckBox* effect_enabled_ = nullptr;
    QCheckBox* effect_sync_ = nullptr;
    QComboBox* effect_audio_response_ = nullptr;
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
    QLabel* effective_frames_ = nullptr;
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
    QPushButton* surface_obj_browse_ = nullptr;
    QGroupBox* starting_image_group_ = nullptr;
    QCheckBox* starting_image_enabled_ = nullptr;
    QLineEdit* starting_image_path_ = nullptr;
    QComboBox* starting_image_fit_ = nullptr;
    QPushButton* starting_image_browse_ = nullptr;
    QPushButton* starting_image_clear_ = nullptr;
    QCheckBox* transform_flip_horizontal_ = nullptr;
    QCheckBox* transform_flip_vertical_ = nullptr;
    QComboBox* transform_mirror_ = nullptr;
    QGroupBox* motion_group_ = nullptr;
    QComboBox* motion_path_ = nullptr;
    QDoubleSpinBox* motion_center_x_ = nullptr;
    QDoubleSpinBox* motion_center_y_ = nullptr;
    QDoubleSpinBox* motion_travel_x_ = nullptr;
    QDoubleSpinBox* motion_travel_y_ = nullptr;
    QSpinBox* motion_cycles_x_ = nullptr;
    QSpinBox* motion_cycles_y_ = nullptr;
    QDoubleSpinBox* motion_phase_ = nullptr;
    QSpinBox* motion_rotations_ = nullptr;
    QDoubleSpinBox* motion_scale_pulse_ = nullptr;
    QPushButton* motion_paths_edit_ = nullptr;
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
