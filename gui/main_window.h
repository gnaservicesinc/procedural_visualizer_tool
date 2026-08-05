#ifndef PVT_MAIN_WINDOW_H
#define PVT_MAIN_WINDOW_H

#include "procedural_visualizer_tool.h"

#include <QFutureWatcher>
#include <QImage>
#include <QMainWindow>
#include <QString>

#include <atomic>
#include <cstddef>
#include <optional>

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QTabWidget;
class QTimer;
class PreviewWidget;

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
    };

    struct ExportResult {
        bool ok = false;
        bool cancelled = false;
        QString error;
    };

    QWidget* createWavePage();
    QWidget* createSwingPage();
    QWidget* createEffectPage();
    QWidget* createSettingsPage();
    QWidget* createTimeline();
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

    std::optional<std::size_t> selectedWaveIndex() const;
    std::optional<std::size_t> selectedSwingIndex() const;
    std::optional<std::size_t> selectedEffectIndex() const;
    void moveSelectedWave(int direction);
    void moveSelectedSwing(int direction);
    void moveSelectedEffect(int direction);

    void schedulePreview();
    void startPreview();
    static PreviewResult generatePreview(pvt::RenderConfig config, int frame,
                                         std::uint64_t generation,
                                         int test_delay_ms);
    void randomizeExistingStackSettings();
    void randomizeStackComposition();
    QString resolvedOutputDirectory(const QString& path) const;
    QString usableDialogDirectory(const QString& preferred = {}) const;
    void rememberDialogLocation(const QString& selectedPath);
    bool startExport();
    void saveSetup();
    void loadSetup();
    bool loadSetupFile(const QString& path, QString* error = nullptr);

    pvt::RenderConfig config_;
    bool populating_ = false;
    bool preview_deferred_ = false;
    bool integer_dither_preference_ = true;
    bool export_active_ = false;
    bool close_after_export_ = false;
    bool playback_preview_advanced_ = false;
    int last_previewed_frame_ = -1;
    int preview_test_delay_ms_ = 0;
    std::uint64_t preview_generation_ = 0;
    std::atomic_bool cancel_export_{false};
    QString startup_working_directory_;
    QString last_dialog_directory_;

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

    QListWidget* effect_list_ = nullptr;
    QComboBox* add_effect_type_ = nullptr;
    QLineEdit* effect_name_ = nullptr;
    QCheckBox* effect_enabled_ = nullptr;
    QCheckBox* effect_sync_ = nullptr;
    QComboBox* effect_type_ = nullptr;
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
    QLineEdit* output_directory_ = nullptr;
    QLineEdit* prefix_ = nullptr;
    QSpinBox* first_frame_ = nullptr;
    QSpinBox* filename_digits_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
};

#endif
