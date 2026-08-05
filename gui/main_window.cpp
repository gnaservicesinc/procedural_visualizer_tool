#include "main_window.h"

#include "preview_widget.h"

#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFuture>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QValidator>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <exception>
#include <string_view>
#include <utility>

namespace {

constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4095;
constexpr std::size_t kMaximumPrefixBytes = 127;

enum class TextRule {
    Name,
    OutputDirectory,
    FilenamePrefix
};

bool valid_text(const QString& value, TextRule rule) {
    const QByteArray utf8 = value.toUtf8();
    const std::size_t size = static_cast<std::size_t>(utf8.size());
    const bool is_name = rule == TextRule::Name;
    const std::size_t maximum = is_name
                                    ? kMaximumNameBytes
                                    : (rule == TextRule::OutputDirectory
                                           ? kMaximumPathBytes
                                           : kMaximumPrefixBytes);
    if ((!is_name && utf8.isEmpty()) || size > maximum) {
        return false;
    }
    for (const char raw_character : utf8) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0U || character == 0x7fU
            || (character < 0x20U && (!is_name || character != '\t'))) {
            return false;
        }
        if (rule == TextRule::FilenamePrefix) {
            switch (character) {
                case '<':
                case '>':
                case ':':
                case '"':
                case '/':
                case '\\':
                case '|':
                case '?':
                case '*':
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

class Utf8TextValidator final : public QValidator {
public:
    Utf8TextValidator(TextRule rule, QObject* parent)
        : QValidator(parent), rule_(rule) {}

    State validate(QString& input, int&) const override {
        if (valid_text(input, rule_)) {
            return Acceptable;
        }
        if (input.isEmpty() && rule_ != TextRule::Name) {
            return Intermediate;
        }
        return Invalid;
    }

private:
    TextRule rule_;
};

void append_copy_suffix(std::string& name) {
    constexpr std::string_view suffix = " copy";
    if (name.size() + suffix.size() <= kMaximumNameBytes) {
        name.append(suffix);
    }
}

QDoubleSpinBox* real_editor(double minimum, double maximum, int decimals = 4,
                            double step = 0.01) {
    auto* editor = new QDoubleSpinBox;
    editor->setRange(minimum, maximum);
    editor->setDecimals(decimals);
    editor->setSingleStep(step);
    editor->setKeyboardTracking(false);
    return editor;
}

QSpinBox* integer_editor(int minimum, int maximum) {
    auto* editor = new QSpinBox;
    editor->setRange(minimum, maximum);
    editor->setKeyboardTracking(false);
    return editor;
}

template <typename Enum>
void add_enum_item(QComboBox* combo, const QString& label, Enum value) {
    combo->addItem(label, static_cast<int>(value));
}

template <typename Enum>
void select_enum(QComboBox* combo, Enum value) {
    const int index = combo->findData(static_cast<int>(value));
    combo->setCurrentIndex(std::max(0, index));
}

float linear_to_srgb(float value) {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    value = std::clamp(value, 0.0F, 1.0F);
    if (value <= 0.0031308F) {
        return value * 12.92F;
    }
    return 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

QString wave_label(const pvt::WaveConfig& wave, std::size_t index) {
    return QStringLiteral("%1. %2  [%3, %4]")
        .arg(index + 1)
        .arg(QString::fromStdString(wave.name))
        .arg(wave.enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(wave.synchronized ? QStringLiteral("sync") : QStringLiteral("free"));
}

QString swing_label(const pvt::SwingConfig& swing, std::size_t index) {
    return QStringLiteral("%1. %2  [%3, %4]")
        .arg(index + 1)
        .arg(QString::fromStdString(swing.name))
        .arg(swing.enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(QString::fromUtf8(pvt::waveform_name(swing.waveform)));
}

QString effect_label(const pvt::EffectConfig& effect, std::size_t index) {
    return QStringLiteral("%1. %2  [%3, %4, %5]")
        .arg(index + 1)
        .arg(QString::fromStdString(effect.name))
        .arg(QString::fromUtf8(pvt::effect_type_name(effect.type)))
        .arg(effect.enabled ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(effect.synchronized ? QStringLiteral("sync") : QStringLiteral("free"));
}

bool configuration_requires_alpha(const pvt::RenderConfig& config) {
    if (config.surface.enabled && config.surface.mapping != pvt::SurfaceMapping::Plane
        && config.surface.curvature > 0.0) {
        return true;
    }
    return std::any_of(config.effects.begin(), config.effects.end(), [](const auto& effect) {
        return effect.enabled && effect.intensity > 0.0 && effect.magnitude > 0.0
               && effect.type != pvt::EffectType::Glow
               && effect.edge_mode == pvt::EdgeMode::Alpha;
    });
}

void set_form_label(QFormLayout* form, QWidget* field, const QString& text) {
    if (auto* label = qobject_cast<QLabel*>(form->labelForField(field))) {
        label->setText(text);
    }
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), config_(pvt::default_config()) {
    setWindowTitle(tr("Procedural Visualizer Tool"));
    resize(1420, 860);

    preview_timer_ = new QTimer(this);
    preview_timer_->setSingleShot(true);
    preview_timer_->setInterval(70);
    playback_timer_ = new QTimer(this);
    preview_watcher_ = new QFutureWatcher<PreviewResult>(this);
    export_watcher_ = new QFutureWatcher<ExportResult>(this);

    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    auto* splitter = new QSplitter(Qt::Horizontal);
    preview_ = new PreviewWidget;
    tabs_ = new QTabWidget;
    tabs_->addTab(createWavePage(), tr("Waves"));
    tabs_->addTab(createSwingPage(), tr("Swings"));
    tabs_->addTab(createEffectPage(), tr("Effects"));
    tabs_->addTab(createSettingsPage(), tr("Render / Output"));
    tabs_->setMinimumWidth(440);
    splitter->addWidget(preview_);
    splitter->addWidget(tabs_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);
    outer->addWidget(createTimeline());
    setCentralWidget(central);

    status_ = new QLabel(tr("Ready"));
    statusBar()->addPermanentWidget(status_, 1);
    createToolbar();

    connect(preview_timer_, &QTimer::timeout, this, &MainWindow::startPreview);
    connect(playback_timer_, &QTimer::timeout, this, [this] {
        int next = timeline_->value() + 1;
        if (next > timeline_->maximum()) {
            next = 0;
        }
        timeline_->setValue(next);
    });
    connect(preview_watcher_, &QFutureWatcher<PreviewResult>::finished, this, [this] {
        const PreviewResult result = preview_watcher_->result();
        if (result.generation == preview_generation_) {
            if (result.error.isEmpty()) {
                preview_->setPreview(result.image);
                status_->setText(tr("Preview frame %1/%2")
                                     .arg(timeline_->value() + 1)
                                     .arg(config_.total_frames));
            } else {
                status_->setText(result.error);
            }
        }
        if (preview_deferred_) {
            preview_deferred_ = false;
            preview_timer_->start();
        }
    });
    connect(export_watcher_, &QFutureWatcher<ExportResult>::finished, this, [this] {
        const ExportResult result = export_watcher_->result();
        export_active_ = false;
        if (close_after_export_) {
            close_after_export_ = false;
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }
        if (result.ok) {
            status_->setText(tr("Export complete"));
            QMessageBox::information(this, tr("Export complete"),
                                     tr("The looping image sequence was exported successfully."));
        } else if (result.cancelled) {
            status_->setText(tr("Export cancelled"));
        } else {
            status_->setText(tr("Export failed"));
            QMessageBox::critical(this, tr("Export failed"), result.error);
        }
    });

    connectEditors();
    refreshAll();
    schedulePreview();
}

MainWindow::~MainWindow() {
    cancel_export_.store(true);
    preview_watcher_->waitForFinished();
    export_watcher_->waitForFinished();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (export_watcher_ != nullptr && export_watcher_->isRunning()) {
        close_after_export_ = true;
        cancel_export_.store(true);
        status_->setText(tr("Closing after the current export frame…"));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

QWidget* MainWindow::createWavePage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Drag numbered handles in the preview to place waves. A synchronized wave uses "
           "the shared swung clock; a free wave remains independently periodic."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    wave_list_ = new QListWidget;
    wave_list_->setAlternatingRowColors(true);
    layout->addWidget(wave_list_, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    for (auto* button : {add, duplicate, remove, up, down}) {
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);

    auto* properties = new QGroupBox(tr("Selected wave"));
    auto* form = new QFormLayout(properties);
    wave_name_ = new QLineEdit;
    wave_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    wave_name_->setValidator(new Utf8TextValidator(TextRule::Name, wave_name_));
    wave_enabled_ = new QCheckBox(tr("Enabled"));
    wave_sync_ = new QCheckBox(tr("Synchronized (optional)"));
    wave_x_ = real_editor(-100.0, 200.0, 3, 1.0);
    wave_y_ = real_editor(-100.0, 200.0, 3, 1.0);
    wave_amplitude_ = real_editor(0.0, 10.0);
    wave_frequency_ = real_editor(0.0, 1000.0);
    wave_cycles_ = integer_editor(-1000, 1000);
    wave_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    wave_direction_ = real_editor(0.0, 1.0, 4, 0.05);
    form->addRow(tr("Name"), wave_name_);
    form->addRow(wave_enabled_);
    form->addRow(wave_sync_);
    form->addRow(tr("X position (%)"), wave_x_);
    form->addRow(tr("Y position (%)"), wave_y_);
    form->addRow(tr("Amplitude"), wave_amplitude_);
    form->addRow(tr("Spatial frequency"), wave_frequency_);
    form->addRow(tr("Cycles per loop"), wave_cycles_);
    form->addRow(tr("Phase (degrees)"), wave_phase_);
    form->addRow(tr("Direction (0 horizontal, .5 radial, 1 vertical)"), wave_direction_);
    layout->addWidget(properties);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.waves.size() >= pvt::kMaximumWaves) {
            QMessageBox::warning(this, tr("Wave limit"), tr("The safety limit is 256 waves."));
            return;
        }
        auto wave = pvt::default_wave(config_.waves.size());
        wave.id = pvt::allocate_id(config_);
        const auto id = wave.id;
        config_.waves.push_back(std::move(wave));
        refreshWaveList(id);
        schedulePreview();
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedWaveIndex();
        if (!index || config_.waves.size() >= pvt::kMaximumWaves) {
            return;
        }
        auto wave = config_.waves[*index];
        wave.id = pvt::allocate_id(config_);
        append_copy_suffix(wave.name);
        const auto id = wave.id;
        config_.waves.insert(config_.waves.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
                             std::move(wave));
        refreshWaveList(id);
        schedulePreview();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedWaveIndex();
        if (!index) {
            return;
        }
        config_.waves.erase(config_.waves.begin() + static_cast<std::ptrdiff_t>(*index));
        refreshWaveList();
        schedulePreview();
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedWave(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedWave(1); });
    return page;
}

QWidget* MainWindow::createSwingPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Swing modulators reshape the shared synchronized clock. Add, remove, "
           "duplicate, and reorder them to layer loop-safe rhythm variations."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    swing_list_ = new QListWidget;
    swing_list_->setAlternatingRowColors(true);
    layout->addWidget(swing_list_, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    for (auto* button : {add, duplicate, remove, up, down}) {
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);

    auto* properties = new QGroupBox(tr("Selected swing"));
    auto* form = new QFormLayout(properties);
    swing_name_ = new QLineEdit;
    swing_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    swing_name_->setValidator(new Utf8TextValidator(TextRule::Name, swing_name_));
    swing_enabled_ = new QCheckBox(tr("Enabled"));
    swing_waveform_ = new QComboBox;
    add_enum_item(swing_waveform_, tr("Sine"), pvt::Waveform::Sine);
    add_enum_item(swing_waveform_, tr("Triangle"), pvt::Waveform::Triangle);
    add_enum_item(swing_waveform_, tr("Smooth pulse"), pvt::Waveform::SmoothPulse);
    add_enum_item(swing_waveform_, tr("Bounce"), pvt::Waveform::Bounce);
    swing_amount_ = real_editor(-2.0, 2.0, 4, 0.05);
    swing_cycles_ = integer_editor(0, 1000);
    swing_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    swing_shape_ = real_editor(0.0, 1.0, 4, 0.05);
    swing_amount_->setToolTip(
        tr("Strength of this timing modulation. Negative values invert the swing."));
    swing_shape_->setToolTip(
        tr("Changes the contour of shaped waveforms; sine and triangle ignore it."));
    form->addRow(tr("Name"), swing_name_);
    form->addRow(swing_enabled_);
    form->addRow(tr("Waveform"), swing_waveform_);
    form->addRow(tr("Amount"), swing_amount_);
    form->addRow(tr("Pulses per loop"), swing_cycles_);
    form->addRow(tr("Phase (degrees)"), swing_phase_);
    form->addRow(tr("Waveform shape"), swing_shape_);
    layout->addWidget(properties);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.swings.size() >= pvt::kMaximumSwings) {
            QMessageBox::warning(this, tr("Swing limit"),
                                 tr("The safety limit is 64 swing modulators."));
            return;
        }
        auto swing = pvt::default_swing(config_.swings.size());
        swing.id = pvt::allocate_id(config_);
        const auto id = swing.id;
        config_.swings.push_back(std::move(swing));
        refreshSwingList(id);
        schedulePreview();
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedSwingIndex();
        if (!index || config_.swings.size() >= pvt::kMaximumSwings) {
            return;
        }
        auto swing = config_.swings[*index];
        swing.id = pvt::allocate_id(config_);
        append_copy_suffix(swing.name);
        const auto id = swing.id;
        config_.swings.insert(
            config_.swings.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
            std::move(swing));
        refreshSwingList(id);
        schedulePreview();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedSwingIndex();
        if (!index) {
            return;
        }
        config_.swings.erase(config_.swings.begin() + static_cast<std::ptrdiff_t>(*index));
        refreshSwingList();
        schedulePreview();
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedSwing(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedSwing(1); });
    return page;
}

QWidget* MainWindow::createEffectPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Effects run in list order. Add, remove, duplicate, or reorder any quantity. "
           "All animation cycle counts are loop-safe integers."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    effect_list_ = new QListWidget;
    effect_list_->setAlternatingRowColors(true);
    layout->addWidget(effect_list_, 1);

    auto* add_row = new QHBoxLayout;
    add_effect_type_ = new QComboBox;
    for (const auto type : {pvt::EffectType::EndlessZoom, pvt::EffectType::Ripple,
                            pvt::EffectType::Shake, pvt::EffectType::FlagWave,
                            pvt::EffectType::Glow}) {
        add_enum_item(add_effect_type_, QString::fromUtf8(pvt::effect_type_name(type)), type);
    }
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    add_row->addWidget(add_effect_type_, 1);
    for (auto* button : {add, duplicate, remove, up, down}) {
        add_row->addWidget(button);
    }
    layout->addLayout(add_row);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* properties = new QGroupBox(tr("Selected effect"));
    effect_form_ = new QFormLayout(properties);
    effect_name_ = new QLineEdit;
    effect_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    effect_name_->setValidator(new Utf8TextValidator(TextRule::Name, effect_name_));
    effect_enabled_ = new QCheckBox(tr("Enabled"));
    effect_sync_ = new QCheckBox(tr("Synchronized (optional)"));
    effect_type_ = new QComboBox;
    for (const auto type : {pvt::EffectType::EndlessZoom, pvt::EffectType::Ripple,
                            pvt::EffectType::Shake, pvt::EffectType::FlagWave,
                            pvt::EffectType::Glow}) {
        add_enum_item(effect_type_, QString::fromUtf8(pvt::effect_type_name(type)), type);
    }
    effect_cycles_ = integer_editor(-1000, 1000);
    effect_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    effect_edge_ = new QComboBox;
    add_enum_item(effect_edge_, tr("Transparent alpha"), pvt::EdgeMode::Alpha);
    add_enum_item(effect_edge_, tr("Black"), pvt::EdgeMode::Black);
    add_enum_item(effect_edge_, tr("White"), pvt::EdgeMode::White);
    add_enum_item(effect_edge_, tr("Reflected pattern"), pvt::EdgeMode::Reflect);
    effect_intensity_ = real_editor(0.0, 100.0);
    effect_magnitude_ = real_editor(0.0, 10.0, 5, 0.005);
    effect_frequency_ = real_editor(0.0, 1000.0);
    effect_secondary_ = real_editor(-100.0, 100.0);
    effect_center_x_ = real_editor(-10.0, 10.0);
    effect_center_y_ = real_editor(-10.0, 10.0);
    effect_angle_ = real_editor(-36000.0, 36000.0, 2, 5.0);
    effect_radius_ = real_editor(0.0, 16384.0, 2, 1.0);
    effect_threshold_ = real_editor(0.0, 64.0);
    effect_knee_ = real_editor(0.0, 1.0);
    effect_form_->addRow(tr("Name"), effect_name_);
    effect_form_->addRow(effect_enabled_);
    effect_form_->addRow(effect_sync_);
    effect_form_->addRow(tr("Type"), effect_type_);
    effect_form_->addRow(tr("Cycles per loop"), effect_cycles_);
    effect_form_->addRow(tr("Phase (degrees)"), effect_phase_);
    effect_form_->addRow(tr("Blank-space handling"), effect_edge_);
    effect_form_->addRow(tr("Intensity / mix"), effect_intensity_);
    effect_form_->addRow(tr("Magnitude"), effect_magnitude_);
    effect_form_->addRow(tr("Frequency"), effect_frequency_);
    effect_form_->addRow(tr("Secondary variation"), effect_secondary_);
    effect_form_->addRow(tr("Center X"), effect_center_x_);
    effect_form_->addRow(tr("Center Y"), effect_center_y_);
    effect_form_->addRow(tr("Angle (degrees)"), effect_angle_);
    effect_form_->addRow(tr("Glow radius (pixels)"), effect_radius_);
    effect_form_->addRow(tr("Glow threshold"), effect_threshold_);
    effect_form_->addRow(tr("Glow soft knee"), effect_knee_);
    scroll->setWidget(properties);
    layout->addWidget(scroll, 2);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.effects.size() >= pvt::kMaximumEffects) {
            QMessageBox::warning(this, tr("Effect limit"),
                                 tr("The safety limit is 256 effects."));
            return;
        }
        const auto type = static_cast<pvt::EffectType>(add_effect_type_->currentData().toInt());
        auto effect = pvt::default_effect(type);
        effect.id = pvt::allocate_id(config_);
        const auto id = effect.id;
        config_.effects.push_back(std::move(effect));
        refreshEffectList(id);
        schedulePreview();
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedEffectIndex();
        if (!index || config_.effects.size() >= pvt::kMaximumEffects) {
            return;
        }
        auto effect = config_.effects[*index];
        effect.id = pvt::allocate_id(config_);
        append_copy_suffix(effect.name);
        const auto id = effect.id;
        config_.effects.insert(
            config_.effects.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
            std::move(effect));
        refreshEffectList(id);
        schedulePreview();
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedEffectIndex();
        if (!index) {
            return;
        }
        config_.effects.erase(config_.effects.begin() + static_cast<std::ptrdiff_t>(*index));
        refreshEffectList();
        schedulePreview();
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedEffect(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedEffect(1); });
    return page;
}

QWidget* MainWindow::createSettingsPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);

    auto* canvas_group = new QGroupBox(tr("Canvas and loop"));
    auto* canvas = new QFormLayout(canvas_group);
    width_ = integer_editor(16, 16384);
    height_ = integer_editor(16, 16384);
    block_size_ = integer_editor(1, 16384);
    frames_ = integer_editor(2, 1000000);
    fps_ = real_editor(1.0, 240.0, 3, 1.0);
    canvas->addRow(tr("Width"), width_);
    canvas->addRow(tr("Height"), height_);
    canvas->addRow(tr("Block size"), block_size_);
    canvas->addRow(tr("Frames per loop"), frames_);
    canvas->addRow(tr("Playback FPS"), fps_);
    layout->addWidget(canvas_group);

    auto* rhythm_group = new QGroupBox(tr("Master rhythm and color timing"));
    auto* rhythm = new QFormLayout(rhythm_group);
    phrase_warp_ = real_editor(0.0, 2.0, 4, 0.01);
    ghost_mix_ = real_editor(0.0, 1.0, 4, 0.01);
    ghost_lag_ = real_editor(-360.0, 360.0, 3, 1.0);
    phrase_warp_->setToolTip(
        tr("Periodic warp applied to the shared synchronized clock."));
    ghost_mix_->setToolTip(tr("Mix between the main and phase-lagged color signals."));
    ghost_lag_->setToolTip(tr("Phase separation of the ghost color signal."));
    rhythm->addRow(tr("Phrase warp"), phrase_warp_);
    rhythm->addRow(tr("Ghost mix"), ghost_mix_);
    rhythm->addRow(tr("Ghost lag (degrees)"), ghost_lag_);
    layout->addWidget(rhythm_group);

    auto* pattern_group = new QGroupBox(tr("Procedural features"));
    auto* pattern = new QFormLayout(pattern_group);
    displacement_enabled_ = new QCheckBox(tr("Displacement enabled"));
    displacement_ = real_editor(0.0, 1000.0, 2, 1.0);
    lighting_enabled_ = new QCheckBox(tr("Slope lighting enabled"));
    wave_depth_ = real_editor(0.0, 10.0);
    spiral_enabled_ = new QCheckBox(tr("Spiral enabled"));
    spiral_frequency_ = real_editor(0.0, 1000.0);
    spiral_arms_ = integer_editor(-100, 100);
    wall_enabled_ = new QCheckBox(tr("Wall reflection enabled"));
    wall_frequency_ = real_editor(0.0, 1000.0);
    wall_mix_ = real_editor(0.0, 5.0);
    hue_cycles_ = integer_editor(-100, 100);
    saturation_ = real_editor(0.0, 1.0);
    pattern->addRow(displacement_enabled_);
    pattern->addRow(tr("Displacement"), displacement_);
    pattern->addRow(lighting_enabled_);
    pattern->addRow(tr("Lighting depth"), wave_depth_);
    pattern->addRow(spiral_enabled_);
    pattern->addRow(tr("Spiral frequency"), spiral_frequency_);
    pattern->addRow(tr("Spiral arms"), spiral_arms_);
    pattern->addRow(wall_enabled_);
    pattern->addRow(tr("Wall frequency"), wall_frequency_);
    pattern->addRow(tr("Wall mix"), wall_mix_);
    pattern->addRow(tr("Hue cycles per loop"), hue_cycles_);
    pattern->addRow(tr("Saturation"), saturation_);
    layout->addWidget(pattern_group);

    auto* surface_group = new QGroupBox(tr("3D primitive wrapping"));
    auto* surface = new QFormLayout(surface_group);
    surface_enabled_ = new QCheckBox(tr("Surface mapping enabled"));
    surface_mapping_ = new QComboBox;
    add_enum_item(surface_mapping_, tr("Plane"), pvt::SurfaceMapping::Plane);
    add_enum_item(surface_mapping_, tr("Cylinder"), pvt::SurfaceMapping::Cylinder);
    add_enum_item(surface_mapping_, tr("Sphere"), pvt::SurfaceMapping::Sphere);
    add_enum_item(surface_mapping_, tr("Cube"), pvt::SurfaceMapping::Cube);
    surface_rotations_ = integer_editor(-1000, 1000);
    surface_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    surface_curvature_ = real_editor(0.0, 1.0);
    surface_lighting_ = real_editor(0.0, 10.0);
    surface->addRow(surface_enabled_);
    surface->addRow(tr("Primitive"), surface_mapping_);
    surface->addRow(tr("Rotations per loop"), surface_rotations_);
    surface->addRow(tr("Starting phase (degrees)"), surface_phase_);
    surface->addRow(tr("Curvature"), surface_curvature_);
    surface->addRow(tr("Lighting"), surface_lighting_);
    layout->addWidget(surface_group);

    auto* quantization_group = new QGroupBox(tr("Visual quantization"));
    auto* quantization = new QFormLayout(quantization_group);
    quantization_enabled_ = new QCheckBox(tr("Quantization enabled"));
    quantization_levels_ = integer_editor(2, 65536);
    quantization_mix_ = real_editor(0.0, 1.0);
    quantization_mode_ = new QComboBox;
    add_enum_item(quantization_mode_, tr("RGB"), pvt::QuantizationMode::Rgb);
    add_enum_item(quantization_mode_, tr("Luminance"), pvt::QuantizationMode::Luminance);
    add_enum_item(quantization_mode_, tr("Hue"), pvt::QuantizationMode::Hue);
    quantization->addRow(quantization_enabled_);
    quantization->addRow(tr("Levels"), quantization_levels_);
    quantization->addRow(tr("Mix"), quantization_mix_);
    quantization->addRow(tr("Mode"), quantization_mode_);
    layout->addWidget(quantization_group);

    auto* alpha_group = new QGroupBox(tr("Alpha channel"));
    auto* alpha = new QFormLayout(alpha_group);
    alpha_enabled_ = new QCheckBox(tr("Export an alpha channel"));
    alpha_enabled_->setToolTip(
        tr("Automatically enabled when a transparent-edge effect or a non-plane "
           "surface can create transparency."));
    alpha_minimum_ = real_editor(0.0, 1.0);
    alpha_maximum_ = real_editor(0.0, 1.0);
    alpha_frequency_ = real_editor(0.0, 1000.0);
    alpha_cycles_ = integer_editor(-1000, 1000);
    alpha_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    alpha->addRow(alpha_enabled_);
    alpha->addRow(tr("Minimum"), alpha_minimum_);
    alpha->addRow(tr("Maximum"), alpha_maximum_);
    alpha->addRow(tr("Spatial frequency"), alpha_frequency_);
    alpha->addRow(tr("Cycles per loop"), alpha_cycles_);
    alpha->addRow(tr("Starting phase (degrees)"), alpha_phase_);
    layout->addWidget(alpha_group);

    auto* output_group = new QGroupBox(tr("Export"));
    auto* output = new QFormLayout(output_group);
    bit_depth_ = new QComboBox;
    bit_depth_->addItem(tr("8-bit PNG"), 8);
    bit_depth_->addItem(tr("16-bit PNG"), 16);
    bit_depth_->addItem(tr("32-bit float EXR"), 32);
    dither_enabled_ = new QCheckBox(tr("Dither integer output"));
    dither_enabled_->setToolTip(
        tr("Float EXR never uses dithering. The integer-output preference is preserved."));
    dither_method_ = new QComboBox;
    add_enum_item(dither_method_, tr("Deterministic blue-noise-like"),
                  pvt::DitherMethod::BlueNoise);
    add_enum_item(dither_method_, tr("Ordered Bayer"), pvt::DitherMethod::OrderedBayer);
    add_enum_item(dither_method_, tr("Floyd-Steinberg"),
                  pvt::DitherMethod::FloydSteinberg);
    auto* directory_row = new QWidget;
    auto* directory_layout = new QHBoxLayout(directory_row);
    directory_layout->setContentsMargins(0, 0, 0, 0);
    output_directory_ = new QLineEdit;
    output_directory_->setMaxLength(static_cast<int>(kMaximumPathBytes));
    output_directory_->setValidator(
        new Utf8TextValidator(TextRule::OutputDirectory, output_directory_));
    auto* browse = new QPushButton(tr("Browse…"));
    directory_layout->addWidget(output_directory_, 1);
    directory_layout->addWidget(browse);
    prefix_ = new QLineEdit;
    prefix_->setMaxLength(static_cast<int>(kMaximumPrefixBytes));
    prefix_->setValidator(new Utf8TextValidator(TextRule::FilenamePrefix, prefix_));
    first_frame_ = integer_editor(0, 1000000000);
    filename_digits_ = integer_editor(1, 12);
    overwrite_ = new QCheckBox(tr("Overwrite matching frames"));
    output->addRow(tr("Bit depth"), bit_depth_);
    output->addRow(dither_enabled_);
    output->addRow(tr("Dither method"), dither_method_);
    output->addRow(tr("Directory"), directory_row);
    output->addRow(tr("Filename prefix"), prefix_);
    output->addRow(tr("First frame number"), first_frame_);
    output->addRow(tr("Minimum number digits"), filename_digits_);
    output->addRow(overwrite_);
    layout->addWidget(output_group);
    layout->addStretch();
    scroll->setWidget(contents);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(
            this, tr("Choose export directory"), output_directory_->text());
        if (!selected.isEmpty()) {
            output_directory_->setText(selected);
            updateOutputEditorValidity();
            applyGlobalEditor(output_directory_);
        }
    });
    return scroll;
}

QWidget* MainWindow::createTimeline() {
    auto* widget = new QWidget;
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 4, 0, 0);
    play_button_ = new QPushButton(tr("Play"));
    timeline_ = new QSlider(Qt::Horizontal);
    timeline_->setRange(0, config_.total_frames - 1);
    frame_label_ = new QLabel;
    frame_label_->setMinimumWidth(90);
    layout->addWidget(play_button_);
    layout->addWidget(new QLabel(tr("Frame")));
    layout->addWidget(timeline_, 1);
    layout->addWidget(frame_label_);

    connect(play_button_, &QPushButton::clicked, this, [this] {
        if (playback_timer_->isActive()) {
            playback_timer_->stop();
            play_button_->setText(tr("Play"));
        } else {
            playback_timer_->start(std::max(1, static_cast<int>(std::lround(1000.0 / config_.fps))));
            play_button_->setText(tr("Pause"));
        }
    });
    connect(timeline_, &QSlider::valueChanged, this, [this](int frame) {
        frame_label_->setText(tr("%1 / %2").arg(frame + 1).arg(config_.total_frames));
        schedulePreview();
    });
    frame_label_->setText(tr("1 / %1").arg(config_.total_frames));
    return widget;
}

void MainWindow::createToolbar() {
    auto* toolbar = addToolBar(tr("Project"));
    toolbar->setMovable(false);
    auto* new_action = toolbar->addAction(tr("Defaults"));
    auto* open_action = toolbar->addAction(tr("Load…"));
    auto* save_action = toolbar->addAction(tr("Save…"));
    toolbar->addSeparator();
    auto* export_action = toolbar->addAction(tr("Export sequence"));
    auto* cancel_action = toolbar->addAction(tr("Cancel export"));
    cancel_action->setEnabled(false);

    connect(new_action, &QAction::triggered, this, [this] {
        config_ = pvt::default_config();
        integer_dither_preference_ = config_.output.dither_enabled;
        refreshAll();
        schedulePreview();
    });
    connect(open_action, &QAction::triggered, this, &MainWindow::loadSetup);
    connect(save_action, &QAction::triggered, this, &MainWindow::saveSetup);
    connect(export_action, &QAction::triggered, this, [this, export_action, cancel_action] {
        if (export_watcher_->isRunning()) {
            return;
        }
        QString editor_error;
        if (!outputEditorsValid(&editor_error)) {
            QMessageBox::warning(this, tr("Invalid output text"), editor_error);
            return;
        }
        const auto validation = pvt::validate(config_);
        if (!validation.ok) {
            QMessageBox::warning(this, tr("Invalid setup"),
                                 QString::fromStdString(validation.message));
            return;
        }
        export_action->setEnabled(false);
        cancel_action->setEnabled(true);
        if (!startExport()) {
            export_action->setEnabled(true);
            cancel_action->setEnabled(false);
        }
    });
    connect(cancel_action, &QAction::triggered, this, [this, cancel_action] {
        cancel_export_.store(true);
        cancel_action->setEnabled(false);
        status_->setText(tr("Cancelling after the current frame…"));
    });
    connect(export_watcher_, &QFutureWatcher<ExportResult>::finished, this,
            [export_action, cancel_action] {
                export_action->setEnabled(true);
                cancel_action->setEnabled(false);
            });
}

void MainWindow::connectEditors() {
    connect(wave_list_, &QListWidget::currentRowChanged, this, [this] { loadSelectedWave(); });
    connect(swing_list_, &QListWidget::currentRowChanged, this,
            [this] { loadSelectedSwing(); });
    connect(effect_list_, &QListWidget::currentRowChanged, this,
            [this] { loadSelectedEffect(); });

    connect(wave_name_, &QLineEdit::textEdited, this, [this](const QString& name) {
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedWaveIndex()) {
            config_.waves[*index].name = name.toStdString();
            updateWaveListItem(*index);
        }
    });
    connect(wave_enabled_, &QCheckBox::toggled, this,
            [this] { applyWaveEditor(wave_enabled_); });
    connect(wave_sync_, &QCheckBox::toggled, this,
            [this] { applyWaveEditor(wave_sync_); });
    for (auto* editor : {wave_x_, wave_y_, wave_amplitude_, wave_frequency_, wave_phase_,
                         wave_direction_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyWaveEditor(editor); });
    }
    connect(wave_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyWaveEditor(wave_cycles_); });

    connect(swing_name_, &QLineEdit::textEdited, this, [this](const QString& name) {
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedSwingIndex()) {
            config_.swings[*index].name = name.toStdString();
            updateSwingListItem(*index);
        }
    });
    connect(swing_enabled_, &QCheckBox::toggled, this,
            [this] { applySwingEditor(swing_enabled_); });
    connect(swing_waveform_, &QComboBox::currentIndexChanged, this,
            [this] { applySwingEditor(swing_waveform_); });
    connect(swing_cycles_, &QSpinBox::valueChanged, this,
            [this] { applySwingEditor(swing_cycles_); });
    for (auto* editor : {swing_amount_, swing_phase_, swing_shape_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applySwingEditor(editor); });
    }

    connect(effect_name_, &QLineEdit::textEdited, this, [this](const QString& name) {
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedEffectIndex()) {
            config_.effects[*index].name = name.toStdString();
            updateEffectListItem(*index);
        }
    });
    connect(effect_enabled_, &QCheckBox::toggled, this,
            [this] { applyEffectEditor(effect_enabled_); });
    connect(effect_sync_, &QCheckBox::toggled, this,
            [this] { applyEffectEditor(effect_sync_); });
    connect(effect_type_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_type_); });
    connect(effect_edge_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_edge_); });
    connect(effect_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyEffectEditor(effect_cycles_); });
    for (auto* editor : {effect_phase_, effect_intensity_, effect_magnitude_,
                         effect_frequency_, effect_secondary_, effect_center_x_,
                         effect_center_y_, effect_angle_, effect_radius_, effect_threshold_,
                         effect_knee_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyEffectEditor(editor); });
    }

    for (auto* editor : {width_, height_, block_size_, frames_, spiral_arms_, hue_cycles_,
                         surface_rotations_, quantization_levels_, alpha_cycles_, first_frame_,
                         filename_digits_}) {
        connect(editor, &QSpinBox::valueChanged, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {fps_, displacement_, wave_depth_, spiral_frequency_,
                         wall_frequency_, wall_mix_, saturation_, surface_curvature_,
                         surface_lighting_, quantization_mix_, alpha_minimum_,
                         alpha_maximum_, alpha_frequency_, phrase_warp_, ghost_mix_, ghost_lag_,
                         surface_phase_, alpha_phase_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {displacement_enabled_, lighting_enabled_, spiral_enabled_,
                         wall_enabled_, surface_enabled_, quantization_enabled_,
                         alpha_enabled_, dither_enabled_, overwrite_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {surface_mapping_, quantization_mode_, bit_depth_, dither_method_}) {
        connect(editor, &QComboBox::currentIndexChanged, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    connect(output_directory_, &QLineEdit::textEdited, this, [this] {
        updateOutputEditorValidity();
        if (output_directory_->hasAcceptableInput()) {
            applyGlobalEditor(output_directory_);
        }
    });
    connect(prefix_, &QLineEdit::textEdited, this, [this] {
        updateOutputEditorValidity();
        if (prefix_->hasAcceptableInput()) {
            applyGlobalEditor(prefix_);
        }
    });

    connect(preview_, &PreviewWidget::waveSelected, this, [this](std::size_t index) {
        if (index < config_.waves.size()) {
            wave_list_->setCurrentRow(static_cast<int>(index));
            tabs_->setCurrentIndex(0);
        }
    });
    connect(preview_, &PreviewWidget::waveMoved, this,
            [this](std::size_t index, double x, double y) {
                if (index >= config_.waves.size()) {
                    return;
                }
                config_.waves[index].x_percent = x;
                config_.waves[index].y_percent = y;
                if (selectedWaveIndex() && *selectedWaveIndex() == index) {
                    const QSignalBlocker bx(wave_x_);
                    const QSignalBlocker by(wave_y_);
                    wave_x_->setValue(x);
                    wave_y_->setValue(y);
                }
                preview_->setConfiguration(config_);
                schedulePreview();
            });
}

void MainWindow::refreshAll() {
    refreshWaveList();
    refreshSwingList();
    refreshEffectList();
    loadGlobalEditors();
    updateTimelineState();
    preview_->setConfiguration(config_);
}

void MainWindow::updateTimelineState() {
    if (timeline_ == nullptr || frame_label_ == nullptr) {
        return;
    }
    const QSignalBlocker blocker(timeline_);
    timeline_->setMaximum(config_.total_frames - 1);
    timeline_->setValue(std::min(timeline_->value(), timeline_->maximum()));
    frame_label_->setText(
        tr("%1 / %2").arg(timeline_->value() + 1).arg(config_.total_frames));
    if (playback_timer_->isActive()) {
        playback_timer_->setInterval(
            std::max(1, static_cast<int>(std::lround(1000.0 / config_.fps))));
    }
}

void MainWindow::updateWaveListItem(std::size_t index) {
    if (index < config_.waves.size()) {
        if (auto* item = wave_list_->item(static_cast<int>(index))) {
            item->setText(wave_label(config_.waves[index], index));
            item->setData(Qt::UserRole,
                          QVariant::fromValue<qulonglong>(config_.waves[index].id));
        }
    }
}

void MainWindow::updateSwingListItem(std::size_t index) {
    if (index < config_.swings.size()) {
        if (auto* item = swing_list_->item(static_cast<int>(index))) {
            item->setText(swing_label(config_.swings[index], index));
            item->setData(Qt::UserRole,
                          QVariant::fromValue<qulonglong>(config_.swings[index].id));
        }
    }
}

void MainWindow::updateEffectListItem(std::size_t index) {
    if (index < config_.effects.size()) {
        if (auto* item = effect_list_->item(static_cast<int>(index))) {
            item->setText(effect_label(config_.effects[index], index));
            item->setData(Qt::UserRole,
                          QVariant::fromValue<qulonglong>(config_.effects[index].id));
        }
    }
}

void MainWindow::refreshWaveList(std::optional<std::uint64_t> selectedId) {
    if (!selectedId) {
        const auto current = selectedWaveIndex();
        if (current) {
            selectedId = config_.waves[*current].id;
        }
    }
    populating_ = true;
    wave_list_->clear();
    int selected_row = -1;
    for (std::size_t index = 0; index < config_.waves.size(); ++index) {
        const auto& wave = config_.waves[index];
        auto* item = new QListWidgetItem(wave_label(wave, index), wave_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(wave.id));
        if (selectedId && wave.id == *selectedId) {
            selected_row = static_cast<int>(index);
        }
    }
    if (selected_row < 0 && !config_.waves.empty()) {
        selected_row = 0;
    }
    wave_list_->setCurrentRow(selected_row);
    populating_ = false;
    loadSelectedWave();
    preview_->setConfiguration(config_);
}

void MainWindow::refreshSwingList(std::optional<std::uint64_t> selectedId) {
    if (!selectedId) {
        const auto current = selectedSwingIndex();
        if (current) {
            selectedId = config_.swings[*current].id;
        }
    }
    populating_ = true;
    swing_list_->clear();
    int selected_row = -1;
    for (std::size_t index = 0; index < config_.swings.size(); ++index) {
        const auto& swing = config_.swings[index];
        auto* item = new QListWidgetItem(swing_label(swing, index), swing_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(swing.id));
        if (selectedId && swing.id == *selectedId) {
            selected_row = static_cast<int>(index);
        }
    }
    if (selected_row < 0 && !config_.swings.empty()) {
        selected_row = 0;
    }
    swing_list_->setCurrentRow(selected_row);
    populating_ = false;
    loadSelectedSwing();
}

void MainWindow::refreshEffectList(std::optional<std::uint64_t> selectedId) {
    if (!selectedId) {
        const auto current = selectedEffectIndex();
        if (current) {
            selectedId = config_.effects[*current].id;
        }
    }
    populating_ = true;
    effect_list_->clear();
    int selected_row = -1;
    for (std::size_t index = 0; index < config_.effects.size(); ++index) {
        const auto& effect = config_.effects[index];
        auto* item = new QListWidgetItem(effect_label(effect, index), effect_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(effect.id));
        if (selectedId && effect.id == *selectedId) {
            selected_row = static_cast<int>(index);
        }
    }
    if (selected_row < 0 && !config_.effects.empty()) {
        selected_row = 0;
    }
    effect_list_->setCurrentRow(selected_row);
    populating_ = false;
    loadSelectedEffect();
}

std::optional<std::size_t> MainWindow::selectedWaveIndex() const {
    const int row = wave_list_ ? wave_list_->currentRow() : -1;
    if (row < 0 || static_cast<std::size_t>(row) >= config_.waves.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

std::optional<std::size_t> MainWindow::selectedSwingIndex() const {
    const int row = swing_list_ ? swing_list_->currentRow() : -1;
    if (row < 0 || static_cast<std::size_t>(row) >= config_.swings.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

std::optional<std::size_t> MainWindow::selectedEffectIndex() const {
    const int row = effect_list_ ? effect_list_->currentRow() : -1;
    if (row < 0 || static_cast<std::size_t>(row) >= config_.effects.size()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(row);
}

void MainWindow::loadSelectedWave() {
    if (populating_) {
        return;
    }
    const auto index = selectedWaveIndex();
    populating_ = true;
    const bool enabled = index.has_value();
    for (auto* widget : std::initializer_list<QWidget*>{wave_name_, wave_enabled_, wave_sync_,
                                                        wave_x_, wave_y_, wave_amplitude_,
                                                        wave_frequency_, wave_cycles_, wave_phase_,
                                                        wave_direction_}) {
        widget->setEnabled(enabled);
    }
    if (index) {
        const auto& wave = config_.waves[*index];
        wave_name_->setText(QString::fromStdString(wave.name));
        wave_enabled_->setChecked(wave.enabled);
        wave_sync_->setChecked(wave.synchronized);
        wave_x_->setValue(wave.x_percent);
        wave_y_->setValue(wave.y_percent);
        wave_amplitude_->setValue(wave.amplitude);
        wave_frequency_->setValue(wave.spatial_frequency);
        wave_cycles_->setValue(wave.cycles_per_loop);
        wave_phase_->setValue(wave.phase_degrees);
        wave_direction_->setValue(wave.direction);
    }
    populating_ = false;
    preview_->setSelectedWave(index);
}

void MainWindow::loadSelectedSwing() {
    if (populating_) {
        return;
    }
    const auto index = selectedSwingIndex();
    populating_ = true;
    const bool enabled = index.has_value();
    for (auto* widget : std::initializer_list<QWidget*>{
             swing_name_, swing_enabled_, swing_waveform_, swing_amount_, swing_cycles_,
             swing_phase_, swing_shape_}) {
        widget->setEnabled(enabled);
    }
    if (index) {
        const auto& swing = config_.swings[*index];
        swing_name_->setText(QString::fromStdString(swing.name));
        swing_enabled_->setChecked(swing.enabled);
        select_enum(swing_waveform_, swing.waveform);
        swing_amount_->setValue(swing.amount);
        swing_cycles_->setValue(swing.cycles_per_loop);
        swing_phase_->setValue(swing.phase_degrees);
        swing_shape_->setValue(swing.shape);
    }
    populating_ = false;
}

void MainWindow::updateEffectEditorVisibility() {
    if (effect_form_ == nullptr || effect_type_ == nullptr) {
        return;
    }
    const auto type = static_cast<pvt::EffectType>(effect_type_->currentData().toInt());
    const bool is_zoom = type == pvt::EffectType::EndlessZoom;
    const bool is_ripple = type == pvt::EffectType::Ripple;
    const bool is_shake = type == pvt::EffectType::Shake;
    const bool is_flag = type == pvt::EffectType::FlagWave;
    const bool is_glow = type == pvt::EffectType::Glow;
    const bool coordinate_effect = !is_glow;
    const bool has_center = is_zoom || is_ripple || is_flag;

    effect_form_->setRowVisible(effect_edge_, coordinate_effect);
    effect_form_->setRowVisible(effect_magnitude_, coordinate_effect);
    effect_form_->setRowVisible(effect_frequency_, coordinate_effect);
    effect_form_->setRowVisible(effect_secondary_, !is_zoom);
    effect_form_->setRowVisible(effect_center_x_, has_center);
    effect_form_->setRowVisible(effect_center_y_, has_center);
    effect_form_->setRowVisible(effect_angle_, is_shake || is_flag);
    effect_form_->setRowVisible(effect_radius_, is_glow);
    effect_form_->setRowVisible(effect_threshold_, is_glow);
    effect_form_->setRowVisible(effect_knee_, is_glow);

    effect_edge_->setToolTip(
        tr("Controls samples that move beyond the source image boundary."));
    effect_center_x_->setToolTip(tr("Normalized horizontal center; 0 is left and 1 is right."));
    effect_center_y_->setToolTip(tr("Normalized vertical center; 0 is top and 1 is bottom."));
    effect_radius_->setToolTip(tr("Glow blur radius in full-resolution output pixels."));
    effect_threshold_->setToolTip(tr("Linear-light brightness where glow begins."));
    effect_knee_->setToolTip(tr("Soft transition width around the glow threshold."));

    if (is_zoom) {
        set_form_label(effect_form_, effect_intensity_, tr("Mix / intensity"));
        set_form_label(effect_form_, effect_magnitude_, tr("Zoom strength"));
        set_form_label(effect_form_, effect_frequency_, tr("Octave multiplier"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_intensity_->setToolTip(tr("Blend between the source and looping zoom illusion."));
        effect_magnitude_->setToolTip(tr("Base zoom amount before the octave multiplier."));
        effect_frequency_->setToolTip(tr("Multiplies the zoom octave span; the renderer caps the combined span."));
    } else if (is_ripple) {
        set_form_label(effect_form_, effect_intensity_, tr("Ripple intensity"));
        set_form_label(effect_form_, effect_magnitude_, tr("Magnitude (short-edge fraction)"));
        set_form_label(effect_form_, effect_frequency_, tr("Spatial frequency"));
        set_form_label(effect_form_, effect_secondary_, tr("Distance attenuation"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_intensity_->setToolTip(tr("Scales the ripple displacement."));
        effect_magnitude_->setToolTip(tr("Peak displacement as a fraction of the shorter image edge."));
        effect_frequency_->setToolTip(tr("Number of spatial ripple oscillations."));
        effect_secondary_->setToolTip(tr("Values above 1 increasingly attenuate distant ripples."));
    } else if (is_shake) {
        set_form_label(effect_form_, effect_intensity_, tr("Shake intensity"));
        set_form_label(effect_form_, effect_magnitude_, tr("Magnitude (short-edge fraction)"));
        set_form_label(effect_form_, effect_frequency_, tr("Shake harmonic"));
        set_form_label(effect_form_, effect_secondary_, tr("Cross-axis harmonic mix"));
        set_form_label(effect_form_, effect_angle_, tr("Direction angle (degrees)"));
        effect_intensity_->setToolTip(tr("Scales the total shake displacement."));
        effect_magnitude_->setToolTip(tr("Peak displacement as a fraction of the shorter image edge."));
        effect_frequency_->setToolTip(tr("Rounded to a whole harmonic to preserve a seamless loop."));
        effect_secondary_->setToolTip(tr("Mix and direction of the secondary shake axis."));
    } else if (is_flag) {
        set_form_label(effect_form_, effect_intensity_, tr("Flag-wave intensity"));
        set_form_label(effect_form_, effect_magnitude_, tr("Magnitude (short-edge fraction)"));
        set_form_label(effect_form_, effect_frequency_, tr("Spatial frequency"));
        set_form_label(effect_form_, effect_secondary_, tr("Secondary harmonic mix"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        set_form_label(effect_form_, effect_angle_, tr("Wave angle (degrees)"));
        effect_intensity_->setToolTip(tr("Scales the flag-wave displacement."));
        effect_magnitude_->setToolTip(tr("Peak displacement as a fraction of the shorter image edge."));
        effect_frequency_->setToolTip(tr("Number of flag-wave oscillations across the image."));
        effect_secondary_->setToolTip(tr("Adds a half-frequency secondary fold to the flag wave."));
    } else {
        set_form_label(effect_form_, effect_intensity_, tr("Glow intensity"));
        set_form_label(effect_form_, effect_secondary_, tr("Pulse depth"));
        effect_intensity_->setToolTip(tr("Brightness added by the blurred highlight layer."));
        effect_secondary_->setToolTip(tr("How strongly the synchronized clock pulses glow intensity."));
    }
}

void MainWindow::loadSelectedEffect() {
    if (populating_) {
        return;
    }
    const auto index = selectedEffectIndex();
    populating_ = true;
    const bool enabled = index.has_value();
    for (auto* widget : std::initializer_list<QWidget*>{
             effect_name_, effect_enabled_, effect_sync_, effect_type_, effect_cycles_,
             effect_phase_, effect_edge_, effect_intensity_, effect_magnitude_,
             effect_frequency_, effect_secondary_, effect_center_x_, effect_center_y_,
             effect_angle_, effect_radius_, effect_threshold_, effect_knee_}) {
        widget->setEnabled(enabled);
    }
    if (index) {
        const auto& effect = config_.effects[*index];
        effect_name_->setText(QString::fromStdString(effect.name));
        effect_enabled_->setChecked(effect.enabled);
        effect_sync_->setChecked(effect.synchronized);
        select_enum(effect_type_, effect.type);
        effect_cycles_->setValue(effect.cycles_per_loop);
        effect_phase_->setValue(effect.phase_degrees);
        select_enum(effect_edge_, effect.edge_mode);
        effect_intensity_->setValue(effect.intensity);
        effect_magnitude_->setValue(effect.magnitude);
        effect_frequency_->setValue(effect.frequency);
        effect_secondary_->setValue(effect.secondary);
        effect_center_x_->setValue(effect.center_x);
        effect_center_y_->setValue(effect.center_y);
        effect_angle_->setValue(effect.angle_degrees);
        effect_radius_->setValue(effect.radius_pixels);
        effect_threshold_->setValue(effect.threshold);
        effect_knee_->setValue(effect.soft_knee);
    }
    updateEffectEditorVisibility();
    populating_ = false;
}

void MainWindow::loadGlobalEditors() {
    populating_ = true;
    width_->setValue(config_.width);
    height_->setValue(config_.height);
    block_size_->setValue(config_.block_size);
    frames_->setValue(config_.total_frames);
    fps_->setValue(config_.fps);
    phrase_warp_->setValue(config_.phrase_warp);
    ghost_mix_->setValue(config_.ghost_mix);
    ghost_lag_->setValue(config_.ghost_lag_degrees);
    displacement_enabled_->setChecked(config_.displacement_enabled);
    displacement_->setValue(config_.displacement);
    lighting_enabled_->setChecked(config_.lighting_enabled);
    wave_depth_->setValue(config_.wave_depth);
    spiral_enabled_->setChecked(config_.spiral_enabled);
    spiral_frequency_->setValue(config_.spiral_frequency);
    spiral_arms_->setValue(config_.spiral_arms);
    wall_enabled_->setChecked(config_.wall_reflection_enabled);
    wall_frequency_->setValue(config_.wall_frequency);
    wall_mix_->setValue(config_.wall_mix);
    hue_cycles_->setValue(config_.hue_cycles);
    saturation_->setValue(config_.saturation);
    surface_enabled_->setChecked(config_.surface.enabled);
    select_enum(surface_mapping_, config_.surface.mapping);
    surface_rotations_->setValue(config_.surface.rotations_per_loop);
    surface_phase_->setValue(config_.surface.phase_degrees);
    surface_curvature_->setValue(config_.surface.curvature);
    surface_lighting_->setValue(config_.surface.lighting);
    quantization_enabled_->setChecked(config_.quantization.enabled);
    quantization_levels_->setValue(config_.quantization.levels);
    quantization_mix_->setValue(config_.quantization.mix);
    select_enum(quantization_mode_, config_.quantization.mode);
    alpha_enabled_->setChecked(config_.alpha.enabled);
    alpha_minimum_->setValue(config_.alpha.minimum);
    alpha_maximum_->setValue(config_.alpha.maximum);
    alpha_frequency_->setValue(config_.alpha.spatial_frequency);
    alpha_cycles_->setValue(config_.alpha.cycles_per_loop);
    alpha_phase_->setValue(config_.alpha.phase_degrees);
    bit_depth_->setCurrentIndex(std::max(0, bit_depth_->findData(config_.output.bit_depth)));
    const bool float_output = config_.output.bit_depth == 32;
    if (!float_output) {
        integer_dither_preference_ = config_.output.dither_enabled;
    } else {
        config_.output.dither_enabled = false;
    }
    dither_enabled_->setChecked(float_output ? false : integer_dither_preference_);
    dither_enabled_->setEnabled(!float_output);
    select_enum(dither_method_, config_.output.dither_method);
    dither_method_->setEnabled(!float_output && integer_dither_preference_);
    output_directory_->setText(QString::fromStdString(config_.output.output_directory));
    prefix_->setText(QString::fromStdString(config_.output.filename_prefix));
    first_frame_->setValue(config_.output.first_frame_number);
    filename_digits_->setValue(config_.output.filename_digits);
    overwrite_->setChecked(config_.output.overwrite_existing);
    populating_ = false;
    updateOutputEditorValidity();
}

void MainWindow::applyWaveEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    const auto index = selectedWaveIndex();
    if (!index) {
        return;
    }
    auto& wave = config_.waves[*index];
    if (changed_editor == wave_enabled_) {
        wave.enabled = wave_enabled_->isChecked();
    } else if (changed_editor == wave_sync_) {
        wave.synchronized = wave_sync_->isChecked();
    } else if (changed_editor == wave_x_) {
        wave.x_percent = wave_x_->value();
    } else if (changed_editor == wave_y_) {
        wave.y_percent = wave_y_->value();
    } else if (changed_editor == wave_amplitude_) {
        wave.amplitude = wave_amplitude_->value();
    } else if (changed_editor == wave_frequency_) {
        wave.spatial_frequency = wave_frequency_->value();
    } else if (changed_editor == wave_cycles_) {
        wave.cycles_per_loop = wave_cycles_->value();
    } else if (changed_editor == wave_phase_) {
        wave.phase_degrees = wave_phase_->value();
    } else if (changed_editor == wave_direction_) {
        wave.direction = wave_direction_->value();
    } else {
        return;
    }
    updateWaveListItem(*index);
    preview_->setConfiguration(config_);
    schedulePreview();
}

void MainWindow::applySwingEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    const auto index = selectedSwingIndex();
    if (!index) {
        return;
    }
    auto& swing = config_.swings[*index];
    if (changed_editor == swing_enabled_) {
        swing.enabled = swing_enabled_->isChecked();
    } else if (changed_editor == swing_waveform_) {
        swing.waveform = static_cast<pvt::Waveform>(swing_waveform_->currentData().toInt());
    } else if (changed_editor == swing_amount_) {
        swing.amount = swing_amount_->value();
    } else if (changed_editor == swing_cycles_) {
        swing.cycles_per_loop = swing_cycles_->value();
    } else if (changed_editor == swing_phase_) {
        swing.phase_degrees = swing_phase_->value();
    } else if (changed_editor == swing_shape_) {
        swing.shape = swing_shape_->value();
    } else {
        return;
    }
    updateSwingListItem(*index);
    schedulePreview();
}

void MainWindow::applyEffectEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    const auto index = selectedEffectIndex();
    if (!index) {
        return;
    }
    auto& effect = config_.effects[*index];
    if (changed_editor == effect_enabled_) {
        effect.enabled = effect_enabled_->isChecked();
    } else if (changed_editor == effect_sync_) {
        effect.synchronized = effect_sync_->isChecked();
    } else if (changed_editor == effect_type_) {
        effect.type = static_cast<pvt::EffectType>(effect_type_->currentData().toInt());
    } else if (changed_editor == effect_cycles_) {
        effect.cycles_per_loop = effect_cycles_->value();
    } else if (changed_editor == effect_phase_) {
        effect.phase_degrees = effect_phase_->value();
    } else if (changed_editor == effect_edge_) {
        effect.edge_mode = static_cast<pvt::EdgeMode>(effect_edge_->currentData().toInt());
    } else if (changed_editor == effect_intensity_) {
        effect.intensity = effect_intensity_->value();
    } else if (changed_editor == effect_magnitude_) {
        effect.magnitude = effect_magnitude_->value();
    } else if (changed_editor == effect_frequency_) {
        effect.frequency = effect_frequency_->value();
    } else if (changed_editor == effect_secondary_) {
        effect.secondary = effect_secondary_->value();
    } else if (changed_editor == effect_center_x_) {
        effect.center_x = effect_center_x_->value();
    } else if (changed_editor == effect_center_y_) {
        effect.center_y = effect_center_y_->value();
    } else if (changed_editor == effect_angle_) {
        effect.angle_degrees = effect_angle_->value();
    } else if (changed_editor == effect_radius_) {
        effect.radius_pixels = effect_radius_->value();
    } else if (changed_editor == effect_threshold_) {
        effect.threshold = effect_threshold_->value();
    } else if (changed_editor == effect_knee_) {
        effect.soft_knee = effect_knee_->value();
    } else {
        return;
    }
    ensureAlphaForTransparency();
    updateEffectListItem(*index);
    updateEffectEditorVisibility();
    preview_->setConfiguration(config_);
    schedulePreview();
}

void MainWindow::applyGlobalEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    bool affects_preview = true;
    if (changed_editor == width_) {
        config_.width = width_->value();
    } else if (changed_editor == height_) {
        config_.height = height_->value();
    } else if (changed_editor == block_size_) {
        config_.block_size = block_size_->value();
    } else if (changed_editor == frames_) {
        config_.total_frames = frames_->value();
    } else if (changed_editor == fps_) {
        config_.fps = fps_->value();
    } else if (changed_editor == phrase_warp_) {
        config_.phrase_warp = phrase_warp_->value();
    } else if (changed_editor == ghost_mix_) {
        config_.ghost_mix = ghost_mix_->value();
    } else if (changed_editor == ghost_lag_) {
        config_.ghost_lag_degrees = ghost_lag_->value();
    } else if (changed_editor == displacement_enabled_) {
        config_.displacement_enabled = displacement_enabled_->isChecked();
    } else if (changed_editor == displacement_) {
        config_.displacement = displacement_->value();
    } else if (changed_editor == lighting_enabled_) {
        config_.lighting_enabled = lighting_enabled_->isChecked();
    } else if (changed_editor == wave_depth_) {
        config_.wave_depth = wave_depth_->value();
    } else if (changed_editor == spiral_enabled_) {
        config_.spiral_enabled = spiral_enabled_->isChecked();
    } else if (changed_editor == spiral_frequency_) {
        config_.spiral_frequency = spiral_frequency_->value();
    } else if (changed_editor == spiral_arms_) {
        config_.spiral_arms = spiral_arms_->value();
    } else if (changed_editor == wall_enabled_) {
        config_.wall_reflection_enabled = wall_enabled_->isChecked();
    } else if (changed_editor == wall_frequency_) {
        config_.wall_frequency = wall_frequency_->value();
    } else if (changed_editor == wall_mix_) {
        config_.wall_mix = wall_mix_->value();
    } else if (changed_editor == hue_cycles_) {
        config_.hue_cycles = hue_cycles_->value();
    } else if (changed_editor == saturation_) {
        config_.saturation = saturation_->value();
    } else if (changed_editor == surface_enabled_) {
        config_.surface.enabled = surface_enabled_->isChecked();
    } else if (changed_editor == surface_mapping_) {
        config_.surface.mapping =
            static_cast<pvt::SurfaceMapping>(surface_mapping_->currentData().toInt());
    } else if (changed_editor == surface_rotations_) {
        config_.surface.rotations_per_loop = surface_rotations_->value();
    } else if (changed_editor == surface_phase_) {
        config_.surface.phase_degrees = surface_phase_->value();
    } else if (changed_editor == surface_curvature_) {
        config_.surface.curvature = surface_curvature_->value();
    } else if (changed_editor == surface_lighting_) {
        config_.surface.lighting = surface_lighting_->value();
    } else if (changed_editor == quantization_enabled_) {
        config_.quantization.enabled = quantization_enabled_->isChecked();
    } else if (changed_editor == quantization_levels_) {
        config_.quantization.levels = quantization_levels_->value();
    } else if (changed_editor == quantization_mix_) {
        config_.quantization.mix = quantization_mix_->value();
    } else if (changed_editor == quantization_mode_) {
        config_.quantization.mode =
            static_cast<pvt::QuantizationMode>(quantization_mode_->currentData().toInt());
    } else if (changed_editor == alpha_enabled_) {
        config_.alpha.enabled = alpha_enabled_->isChecked();
    } else if (changed_editor == alpha_minimum_) {
        config_.alpha.minimum = alpha_minimum_->value();
    } else if (changed_editor == alpha_maximum_) {
        config_.alpha.maximum = alpha_maximum_->value();
    } else if (changed_editor == alpha_frequency_) {
        config_.alpha.spatial_frequency = alpha_frequency_->value();
    } else if (changed_editor == alpha_cycles_) {
        config_.alpha.cycles_per_loop = alpha_cycles_->value();
    } else if (changed_editor == alpha_phase_) {
        config_.alpha.phase_degrees = alpha_phase_->value();
    } else if (changed_editor == bit_depth_) {
        if (config_.output.bit_depth != 32) {
            integer_dither_preference_ = dither_enabled_->isChecked();
        }
        config_.output.bit_depth = bit_depth_->currentData().toInt();
        config_.output.dither_enabled = config_.output.bit_depth == 32
                                            ? false
                                            : integer_dither_preference_;
        affects_preview = false;
    } else if (changed_editor == dither_enabled_) {
        if (config_.output.bit_depth != 32) {
            integer_dither_preference_ = dither_enabled_->isChecked();
            config_.output.dither_enabled = integer_dither_preference_;
        }
        affects_preview = false;
    } else if (changed_editor == dither_method_) {
        config_.output.dither_method =
            static_cast<pvt::DitherMethod>(dither_method_->currentData().toInt());
        affects_preview = false;
    } else if (changed_editor == output_directory_) {
        if (!output_directory_->hasAcceptableInput()) {
            return;
        }
        config_.output.output_directory = output_directory_->text().toStdString();
        affects_preview = false;
    } else if (changed_editor == prefix_) {
        if (!prefix_->hasAcceptableInput()) {
            return;
        }
        config_.output.filename_prefix = prefix_->text().toStdString();
        affects_preview = false;
    } else if (changed_editor == first_frame_) {
        config_.output.first_frame_number = first_frame_->value();
        affects_preview = false;
    } else if (changed_editor == filename_digits_) {
        config_.output.filename_digits = filename_digits_->value();
        affects_preview = false;
    } else if (changed_editor == overwrite_) {
        config_.output.overwrite_existing = overwrite_->isChecked();
        affects_preview = false;
    } else {
        return;
    }

    ensureAlphaForTransparency();
    {
        const QSignalBlocker blocker(dither_enabled_);
        dither_enabled_->setChecked(config_.output.bit_depth == 32
                                        ? false
                                        : integer_dither_preference_);
    }
    dither_enabled_->setEnabled(config_.output.bit_depth != 32);
    dither_method_->setEnabled(config_.output.bit_depth != 32
                               && config_.output.dither_enabled);
    if (changed_editor == frames_ || changed_editor == fps_) {
        updateTimelineState();
    }
    if (affects_preview) {
        preview_->setConfiguration(config_);
        schedulePreview();
    }
}

void MainWindow::ensureAlphaForTransparency() {
    if (!configuration_requires_alpha(config_) || config_.alpha.enabled) {
        return;
    }
    config_.alpha.enabled = true;
    const QSignalBlocker blocker(alpha_enabled_);
    alpha_enabled_->setChecked(true);
    status_->setText(
        tr("Alpha export was enabled because the current setup can create transparency."));
}

bool MainWindow::outputEditorsValid(QString* error) const {
    if (output_directory_ == nullptr || !output_directory_->hasAcceptableInput()) {
        if (error != nullptr) {
            *error = tr("Output directory must contain 1 to %1 UTF-8 bytes without control characters.")
                         .arg(kMaximumPathBytes);
        }
        return false;
    }
    if (prefix_ == nullptr || !prefix_->hasAcceptableInput()) {
        if (error != nullptr) {
            *error = tr("Filename prefix must contain 1 to %1 UTF-8 bytes and cannot contain "
                        "<, >, :, \", /, \\, |, ?, or *.")
                         .arg(kMaximumPrefixBytes);
        }
        return false;
    }
    return true;
}

void MainWindow::updateOutputEditorValidity() {
    const bool directory_valid = output_directory_ != nullptr
                                 && output_directory_->hasAcceptableInput();
    const bool prefix_valid = prefix_ != nullptr && prefix_->hasAcceptableInput();
    if (output_directory_ != nullptr) {
        output_directory_->setStyleSheet(
            directory_valid ? QString() : QStringLiteral("border: 1px solid #c0392b;"));
        output_directory_->setToolTip(
            tr("Required; at most %1 UTF-8 bytes and no control characters.")
                .arg(kMaximumPathBytes));
    }
    if (prefix_ != nullptr) {
        prefix_->setStyleSheet(
            prefix_valid ? QString() : QStringLiteral("border: 1px solid #c0392b;"));
        prefix_->setToolTip(
            tr("Required; at most %1 UTF-8 bytes and no filename separator or reserved character.")
                .arg(kMaximumPrefixBytes));
    }
}

void MainWindow::moveSelectedWave(int direction) {
    const auto index = selectedWaveIndex();
    if (!index) {
        return;
    }
    const auto target = static_cast<std::ptrdiff_t>(*index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(config_.waves.size())) {
        return;
    }
    const auto id = config_.waves[*index].id;
    std::swap(config_.waves[*index], config_.waves[static_cast<std::size_t>(target)]);
    refreshWaveList(id);
    schedulePreview();
}

void MainWindow::moveSelectedSwing(int direction) {
    const auto index = selectedSwingIndex();
    if (!index) {
        return;
    }
    const auto target = static_cast<std::ptrdiff_t>(*index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(config_.swings.size())) {
        return;
    }
    const auto id = config_.swings[*index].id;
    std::swap(config_.swings[*index], config_.swings[static_cast<std::size_t>(target)]);
    refreshSwingList(id);
    schedulePreview();
}

void MainWindow::moveSelectedEffect(int direction) {
    const auto index = selectedEffectIndex();
    if (!index) {
        return;
    }
    const auto target = static_cast<std::ptrdiff_t>(*index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(config_.effects.size())) {
        return;
    }
    const auto id = config_.effects[*index].id;
    std::swap(config_.effects[*index], config_.effects[static_cast<std::size_t>(target)]);
    refreshEffectList(id);
    schedulePreview();
}

void MainWindow::schedulePreview() {
    ++preview_generation_;
    if (preview_watcher_ && preview_watcher_->isRunning()) {
        preview_deferred_ = true;
        return;
    }
    if (preview_timer_) {
        preview_timer_->start();
    }
}

void MainWindow::startPreview() {
    if (preview_watcher_->isRunning()) {
        preview_deferred_ = true;
        return;
    }
    status_->setText(tr("Rendering preview…"));
    const auto config = config_;
    const int frame = timeline_->value();
    const std::uint64_t generation = preview_generation_;
    try {
        preview_watcher_->setFuture(QtConcurrent::run(
            [config, frame, generation] { return generatePreview(config, frame, generation); }));
    } catch (const std::exception& exception) {
        status_->setText(
            tr("Preview could not start: %1").arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        status_->setText(tr("The background preview task could not be created."));
    }
}

MainWindow::PreviewResult MainWindow::generatePreview(pvt::RenderConfig config, int frame,
                                                       std::uint64_t generation) {
    PreviewResult result;
    result.generation = generation;
    try {
        const int source_short_edge = std::max(1, std::min(config.width, config.height));
        const double scale = std::min({1.0, 720.0 / static_cast<double>(config.width),
                                       480.0 / static_cast<double>(config.height)});
        const int preview_width =
            std::max(16, static_cast<int>(std::lround(config.width * scale)));
        const int preview_height =
            std::max(16, static_cast<int>(std::lround(config.height * scale)));
        const double pixel_scale =
            static_cast<double>(std::min(preview_width, preview_height)) /
            static_cast<double>(source_short_edge);

        // These controls are defined in output pixels. Scale them with the preview so
        // the low-resolution preview preserves their full-resolution proportions.
        config.displacement *= pixel_scale;
        for (auto& effect : config.effects) {
            if (effect.type == pvt::EffectType::Glow) {
                effect.radius_pixels *= pixel_scale;
            }
        }
        config.width = preview_width;
        config.height = preview_height;
        config.block_size =
            std::max(1, static_cast<int>(std::lround(config.block_size * scale)));
        pvt::Image image;
        std::string error;
        if (!pvt::render_frame(config, frame, image, &error)) {
            result.error = QString::fromStdString(error);
            return result;
        }
        result.image = QImage(image.width, image.height, QImage::Format_RGBA8888);
        if (result.image.isNull()) {
            result.error = tr("The preview image buffer could not be allocated.");
            return result;
        }
        for (int y = 0; y < image.height; ++y) {
            auto* row = result.image.scanLine(y);
            for (int x = 0; x < image.width; ++x) {
                const float* pixel = image.pixel(x, y);
                row[x * 4] = static_cast<unsigned char>(
                    std::lround(linear_to_srgb(pixel[0]) * 255.0F));
                row[x * 4 + 1] = static_cast<unsigned char>(
                    std::lround(linear_to_srgb(pixel[1]) * 255.0F));
                row[x * 4 + 2] = static_cast<unsigned char>(
                    std::lround(linear_to_srgb(pixel[2]) * 255.0F));
                row[x * 4 + 3] = static_cast<unsigned char>(
                    std::lround(std::clamp(pixel[3], 0.0F, 1.0F) * 255.0F));
            }
        }
    } catch (const std::exception& exception) {
        result.image = {};
        result.error = tr("Preview failed: %1").arg(QString::fromUtf8(exception.what()));
    } catch (...) {
        result.image = {};
        result.error = tr("Preview failed because of an unexpected error.");
    }
    return result;
}

bool MainWindow::startExport() {
    cancel_export_.store(false);
    export_active_ = true;
    status_->setText(tr("Exporting sequence…"));
    const auto config = config_;
    try {
        export_watcher_->setFuture(QtConcurrent::run([this, config] {
            ExportResult result;
            std::string error;
            try {
                result.ok = pvt::render_sequence(
                    config,
                    [this](int completed, int total) {
                        const int update_stride = std::max(1, total / 200);
                        if (completed == 0 || completed == total
                            || completed % update_stride == 0) {
                            QMetaObject::invokeMethod(
                                this,
                                [this, completed, total] {
                                    if (export_active_) {
                                        status_->setText(tr("Exporting frame %1/%2…")
                                                             .arg(completed)
                                                             .arg(total));
                                    }
                                },
                                Qt::QueuedConnection);
                        }
                        return !cancel_export_.load();
                    },
                    &cancel_export_, &error);
            } catch (const std::exception& exception) {
                error = std::string("Export failed: ") + exception.what();
            } catch (...) {
                error = "Export failed because of an unexpected error.";
            }
            result.cancelled = !result.ok && cancel_export_.load();
            result.error = QString::fromStdString(error);
            return result;
        }));
    } catch (const std::exception& exception) {
        export_active_ = false;
        status_->setText(tr("Export could not start"));
        QMessageBox::critical(this, tr("Export could not start"),
                              QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        export_active_ = false;
        status_->setText(tr("Export could not start"));
        QMessageBox::critical(this, tr("Export could not start"),
                              tr("The background export task could not be created."));
        return false;
    }
    return true;
}

bool MainWindow::loadSetupFile(const QString& path, QString* error) {
    auto loaded = config_;
    std::string load_error;
    if (!pvt::load_setup(path.toStdString(), loaded, &load_error)) {
        if (error != nullptr) {
            *error = QString::fromStdString(load_error);
        }
        return false;
    }
    config_ = std::move(loaded);
    refreshAll();
    schedulePreview();
    return true;
}

bool MainWindow::runSmokeChecks(QString* error) {
    const auto original = config_;
    auto expected = original;
    expected.surface.rotations_per_loop = 900;
    expected.surface.lighting = 9.0;
    expected.ghost_lag_degrees = 5.729612345678;
    if (!expected.waves.empty()) {
        expected.waves.front().x_percent = 29.166712345678;
    }
    if (!expected.swings.empty()) {
        expected.swings.front().phase_degrees = 137.50776405003785;
    }
    if (!expected.effects.empty()) {
        expected.effects.front().center_x = -9.0;
        expected.effects.front().center_y = 9.0;
        expected.effects.front().frequency = 1.123456789012;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        if (error != nullptr) {
            *error = tr("Could not create the GUI smoke-test directory.");
        }
        return false;
    }
    const QString path = directory.filePath(QStringLiteral("round-trip.pvt"));
    std::string save_error;
    if (!pvt::save_setup(expected, path.toStdString(), &save_error)) {
        if (error != nullptr) {
            *error = tr("Could not save the GUI smoke-test setup: %1")
                         .arg(QString::fromStdString(save_error));
        }
        return false;
    }

    config_.waves.clear();
    config_.swings.clear();
    config_.effects.clear();
    QString load_error;
    if (!loadSetupFile(path, &load_error)) {
        if (error != nullptr) {
            *error = tr("Could not reload the GUI smoke-test setup: %1").arg(load_error);
        }
        return false;
    }

    const auto same_ids = [](const auto& left, const auto& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (left[index].id != right[index].id) {
                return false;
            }
        }
        return true;
    };
    if (!same_ids(config_.waves, expected.waves)
        || !same_ids(config_.swings, expected.swings)
        || !same_ids(config_.effects, expected.effects) || !pvt::validate(config_).ok) {
        if (error != nullptr) {
            *error = tr("GUI setup loading did not preserve the configured stacks.");
        }
        return false;
    }

    if (surface_rotations_->value() != expected.surface.rotations_per_loop
        || surface_lighting_->value() != expected.surface.lighting
        || (!expected.effects.empty()
            && (effect_center_x_->value() != expected.effects.front().center_x
                || effect_center_y_->value() != expected.effects.front().center_y))) {
        if (error != nullptr) {
            *error = tr("GUI editors clamped values accepted by central validation.");
        }
        return false;
    }

    surface_enabled_->setChecked(!surface_enabled_->isChecked());
    if (config_.surface.rotations_per_loop != expected.surface.rotations_per_loop
        || config_.surface.lighting != expected.surface.lighting
        || config_.ghost_lag_degrees != expected.ghost_lag_degrees) {
        if (error != nullptr) {
            *error = tr("Editing an unrelated surface control changed loaded values.");
        }
        return false;
    }
    if (!expected.effects.empty()) {
        effect_enabled_->setChecked(!effect_enabled_->isChecked());
        if (config_.effects.front().center_x != expected.effects.front().center_x
            || config_.effects.front().center_y != expected.effects.front().center_y
            || config_.effects.front().frequency != expected.effects.front().frequency) {
            if (error != nullptr) {
                *error = tr("Editing an unrelated effect control changed loaded values.");
            }
            return false;
        }
    }
    if (!expected.waves.empty()) {
        wave_enabled_->setChecked(!wave_enabled_->isChecked());
        if (config_.waves.front().x_percent != expected.waves.front().x_percent) {
            if (error != nullptr) {
                *error = tr("Editing an unrelated wave control changed loaded precision.");
            }
            return false;
        }
    }
    if (!expected.swings.empty()) {
        swing_enabled_->setChecked(!swing_enabled_->isChecked());
        if (config_.swings.front().phase_degrees
            != expected.swings.front().phase_degrees) {
            if (error != nullptr) {
                *error = tr("Editing an unrelated swing control changed loaded precision.");
            }
            return false;
        }
    }

    const auto validator_rejects = [](const QLineEdit* editor, QString value) {
        int position = static_cast<int>(value.size());
        return editor->validator() != nullptr
               && editor->validator()->validate(value, position) != QValidator::Acceptable;
    };
    if (!validator_rejects(prefix_, QStringLiteral("bad/name"))
        || !validator_rejects(prefix_, QString(50, QChar(0x20ac)))
        || !validator_rejects(output_directory_, QString(1400, QChar(0x20ac)))
        || !validator_rejects(wave_name_, QString(100, QChar(0x20ac)))) {
        if (error != nullptr) {
            *error = tr("GUI text validators did not enforce portable UTF-8 byte limits.");
        }
        return false;
    }

    if (!expected.effects.empty()) {
        const int alpha_edge = effect_edge_->findData(static_cast<int>(pvt::EdgeMode::Alpha));
        if (alpha_edge < 0) {
            if (error != nullptr) {
                *error = tr("The transparent edge option is missing from the GUI.");
            }
            return false;
        }
        effect_edge_->setCurrentIndex(alpha_edge);
        if (!config_.alpha.enabled || !alpha_enabled_->isChecked()) {
            if (error != nullptr) {
                *error = tr("Transparent edge handling did not enable alpha export.");
            }
            return false;
        }
        alpha_enabled_->setChecked(false);
        if (!config_.alpha.enabled || !alpha_enabled_->isChecked()) {
            if (error != nullptr) {
                *error = tr("Alpha export could be disabled while active transparency required it.");
            }
            return false;
        }
    }
    frames_->setValue(12);
    if (config_.total_frames != 12 || !frame_label_->text().endsWith(QStringLiteral("/ 12"))) {
        if (error != nullptr) {
            *error = tr("The GUI timeline did not follow a frame-count edit.");
        }
        return false;
    }

    config_ = original;
    refreshAll();
    schedulePreview();
    return true;
}

void MainWindow::saveSetup() {
    QString editor_error;
    if (!outputEditorsValid(&editor_error)) {
        QMessageBox::warning(this, tr("Invalid output text"), editor_error);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Save setup"),
                                                       QStringLiteral("setup.pvt"),
                                                       tr("PVT setup (*.pvt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    std::string error;
    if (!pvt::save_setup(config_, path.toStdString(), &error)) {
        QMessageBox::critical(this, tr("Save failed"), QString::fromStdString(error));
        return;
    }
    status_->setText(tr("Saved %1").arg(path));
}

void MainWindow::loadSetup() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Load setup"), {},
                                                       tr("PVT setup (*.pvt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!loadSetupFile(path, &error)) {
        QMessageBox::critical(this, tr("Load failed"),
                              tr("The active setup was not changed.\n\n%1").arg(error));
        return;
    }
    status_->setText(tr("Loaded %1").arg(path));
}
