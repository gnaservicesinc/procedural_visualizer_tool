#include "main_window.h"

#include "application_settings_dialog.h"
#include "audio_processing_dialog.h"
#include "live_target_registry.h"
#include "live_workspace.h"
#include "preview_widget.h"
#include "video_export.h"
#include "video_export_dialog.h"
#include "../src/audio_analysis.h"
#include "../src/audio_playback.h"
#include "../src/config_codec.h"
#include "../src/displacement_surface.h"
#include "../src/palette_io.h"
#include "../src/project_bundle.h"
#include "../src/source_image.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorSpace>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFuture>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QDateTime>
#include <QToolBar>
#include <QUrl>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>
#include <QValidator>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <random>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::size_t kMaximumNameBytes =
    static_cast<std::size_t>((std::numeric_limits<int>::max)());
constexpr std::size_t kMaximumPathBytes = kMaximumNameBytes;
constexpr std::size_t kMaximumPrefixBytes = kMaximumNameBytes;
constexpr int kDefaultUndoLimit = 500;
constexpr int kMinimumUndoLimit = 0;
constexpr int kMaximumUndoLimit = (std::numeric_limits<int>::max)();
const double kMaximumRenderParameter =
    pvt::maximum_render_parameter_magnitude();
constexpr double kMinimumPositiveUiValue = 0.000001;
constexpr int kMinimumIntegerParameter = (std::numeric_limits<int>::min)();
constexpr int kMaximumIntegerParameter = (std::numeric_limits<int>::max)();
constexpr int kParticleSizeSliderSteps = 1000;
constexpr int kMicLiveClockSentinel = -1000;
constexpr double kParticleSizeSliderMinimum = 0.5;
constexpr double kParticleSizeSliderMaximum = 256.0;
// QDoubleSpinBox supplies double milliseconds while ClockConfig persists int64
// microseconds. Leave a conversion margin so rounding cannot overflow int64.
constexpr double kMaximumClockMilliseconds =
    static_cast<double>((std::numeric_limits<std::int64_t>::max)()
                        / INT64_C(1000000))
    * 1000.0;

double particle_radius_from_slider(int position) {
    const double unit = std::clamp(
        static_cast<double>(position)
            / static_cast<double>(kParticleSizeSliderSteps),
        0.0, 1.0);
    return kParticleSizeSliderMinimum * std::pow(
        kParticleSizeSliderMaximum / kParticleSizeSliderMinimum, unit);
}

int particle_slider_from_radius(double radius) {
    const double clamped = std::clamp(
        radius, kParticleSizeSliderMinimum, kParticleSizeSliderMaximum);
    const double unit = std::log(clamped / kParticleSizeSliderMinimum)
                        / std::log(kParticleSizeSliderMaximum
                                   / kParticleSizeSliderMinimum);
    return static_cast<int>(std::llround(
        unit * static_cast<double>(kParticleSizeSliderSteps)));
}

QString custom_new_project_defaults_path() {
    const QString root = QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
    return root.isEmpty()
               ? QString{}
               : QDir(root).filePath(QStringLiteral("new-project-default.zip"));
}

QString shell_single_quote(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString ffconcat_single_quote(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

bool write_video_concat_script(const QString& script_path,
                               const QStringList& chunks,
                               const QString& selected_movie_path,
                               QString* error) {
    if (chunks.size() < 2) {
        if (error != nullptr) *error = QObject::tr("A concat script needs at least two chunks.");
        return false;
    }
    const QFileInfo selected(selected_movie_path);
    const QString extension = selected.suffix();
    const QString default_name = selected.completeBaseName()
        + QStringLiteral("-reassembled")
        + (extension.isEmpty() ? QString{} : QLatin1Char('.') + extension);
    const QString directory_name = selected.fileName();
    QString script;
    script += QStringLiteral("#!/usr/bin/env bash\nset -euo pipefail\n\n");
    script += QStringLiteral(
        "if (( $# > 1 )); then\n"
        "  echo 'Usage: concat-script [output-file-or-directory]' >&2\n"
        "  exit 2\n"
        "fi\n"
        "if ! command -v ffmpeg >/dev/null 2>&1; then\n"
        "  echo 'ffmpeg was not found on PATH.' >&2\n"
        "  exit 127\n"
        "fi\n\n");
    script += QStringLiteral("output=%1\n").arg(shell_single_quote(default_name));
    script += QStringLiteral(
        "if (( $# == 1 )); then\n"
        "  if [[ -d \"$1\" ]]; then\n");
    script += QStringLiteral("    output=\"$1\"/%1\n")
                  .arg(shell_single_quote(directory_name));
    script += QStringLiteral(
        "  else\n"
        "    output=$1\n"
        "  fi\n"
        "fi\n\n"
        "input_file_list=$(mktemp \"${TMPDIR:-/tmp}/pvt-concat.XXXXXX\")\n"
        "trap 'rm -f \"$input_file_list\"' EXIT\n"
        "cat >\"$input_file_list\" <<'PVT_FFMPEG_CONCAT'\n"
        "ffconcat version 1.0\n");
    for (const QString& chunk : chunks) {
        script += QStringLiteral("file %1\n").arg(
            ffconcat_single_quote(QFileInfo(chunk).absoluteFilePath()));
    }
    script += QStringLiteral(
        "PVT_FFMPEG_CONCAT\n\n"
        "ffmpeg -f concat -safe 0 -i \"$input_file_list\" -c copy \"$output\"\n");

    QSaveFile output(script_path);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = QObject::tr("Could not create concat script %1: %2")
                         .arg(script_path, output.errorString());
        }
        return false;
    }
    const QByteArray bytes = script.toUtf8();
    if (output.write(bytes) != bytes.size() || !output.commit()) {
        if (error != nullptr) {
            *error = QObject::tr("Could not atomically write concat script %1: %2")
                         .arg(script_path, output.errorString());
        }
        return false;
    }
    QFile::setPermissions(
        script_path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner | QFileDevice::ReadGroup
            | QFileDevice::ExeGroup | QFileDevice::ReadOther
            | QFileDevice::ExeOther);
    return true;
}

#ifndef PVT_PROGRAM_VERSION
#  define PVT_PROGRAM_VERSION "development"
#endif

class LambdaUndoCommand final : public QUndoCommand {
public:
    LambdaUndoCommand(QString text, std::function<void()> undo,
                      std::function<void()> redo, QString merge_key)
        : QUndoCommand(std::move(text)), undo_(std::move(undo)),
          redo_(std::move(redo)), merge_key_(std::move(merge_key)) {}

    void undo() override {
        if (undo_) {
            undo_();
        }
    }

    void redo() override {
        // MainWindow records commands after applying the interactive edit. The
        // first redo performed by QUndoStack::push must therefore be a no-op.
        if (first_redo_) {
            first_redo_ = false;
            return;
        }
        if (redo_) {
            redo_();
        }
    }

    int id() const override {
        return merge_key_.isEmpty() ? -1 : 0x505654;
    }

    const QString& mergeKey() const noexcept {
        return merge_key_;
    }

    bool mergeWith(const QUndoCommand* other) override {
        const auto* command = dynamic_cast<const LambdaUndoCommand*>(other);
        if (command == nullptr || merge_key_.isEmpty()
            || command->merge_key_ != merge_key_) {
            return false;
        }
        redo_ = command->redo_;
        setText(command->text());
        return true;
    }

private:
    std::function<void()> undo_;
    std::function<void()> redo_;
    QString merge_key_;
    bool first_redo_ = true;
};

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> action) : action_(std::move(action)) {}
    ~ScopeExit() {
        if (action_) action_();
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> action_;
};

enum class TextRule {
    Name,
    ProjectName,
    OutputDirectory,
    OptionalPath,
    FilenamePrefix
};

bool valid_text(const QString& value, TextRule rule) {
    const QByteArray utf8 = value.toUtf8();
    const std::size_t size = static_cast<std::size_t>(utf8.size());
    const bool is_name = rule == TextRule::Name || rule == TextRule::ProjectName;
    const bool is_project_name = rule == TextRule::ProjectName;
    const bool is_optional_path = rule == TextRule::OptionalPath;
    const std::size_t maximum = is_name
                                    ? kMaximumNameBytes
                                    : (rule == TextRule::OutputDirectory
                                           || is_optional_path
                                           ? kMaximumPathBytes
                                           : kMaximumPrefixBytes);
    if ((is_project_name && utf8.isEmpty())
        || (!is_name && !is_optional_path && utf8.isEmpty()) || size > maximum) {
        return false;
    }
    for (const char32_t code_point : value.toUcs4()) {
        if ((code_point < 0x20U
             && (!is_name || code_point != static_cast<char32_t>('\t')
                 || is_project_name))
            || (code_point >= 0x7fU && code_point <= 0x9fU)) {
            return false;
        }
    }
    for (const char raw_character : utf8) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == 0U || character == 0x7fU
            || (character < 0x20U
                && (!is_name || character != '\t' || is_project_name))) {
            return false;
        }
        if (is_project_name) {
            switch (character) {
                case '/': case '\\':
                    return false;
                default:
                    break;
            }
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

double linear_to_display_srgb(double value) {
    const double encoded = value <= 0.0031308
                               ? 12.92 * value
                               : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return (std::clamp)(encoded, 0.0, 1.0);
}

QColor palette_display_color(const pvt::PaletteColor& value) {
    const bool linear = value.encoding == pvt::PaletteColorEncoding::Linear;
    const auto channel = [linear](double component) {
        return linear ? linear_to_display_srgb(component)
                      : (std::clamp)(component, 0.0, 1.0);
    };
    return QColor::fromRgbF(
        static_cast<float>(channel(value.red)),
        static_cast<float>(channel(value.green)),
        static_cast<float>(channel(value.blue)),
        static_cast<float>((std::clamp)(value.alpha, 0.0, 1.0)));
}

QString palette_io_summary_text(
    const QString& path, pvt::palette_io::PaletteFormat format,
    const pvt::palette_io::PaletteIoSummary& summary) {
    QString result = QObject::tr(
        "%1\n%2\n\nScanned: %3\nAccepted: %4\nFully transparent ignored: %5\nDuplicates ignored: %6\nSkipped: %7\nUnsupported: %8")
        .arg(QString::fromUtf8(pvt::palette_io::format_name(format)), path)
        .arg(static_cast<qulonglong>(summary.scanned))
        .arg(static_cast<qulonglong>(summary.accepted))
        .arg(static_cast<qulonglong>(summary.transparent_ignored))
        .arg(static_cast<qulonglong>(summary.duplicates_ignored))
        .arg(static_cast<qulonglong>(summary.skipped))
        .arg(static_cast<qulonglong>(summary.unsupported));
    QStringList losses;
    if (summary.names_lost) losses.push_back(QObject::tr("entry names were not preserved"));
    if (summary.alpha_lost) losses.push_back(QObject::tr("alpha was not preserved"));
    if (summary.precision_lost) losses.push_back(QObject::tr("numeric precision was reduced"));
    if (summary.encoding_converted) {
        losses.push_back(QObject::tr("color encoding was converted"));
    }
    if (!losses.isEmpty()) {
        result += QObject::tr("\n\nFormat limitations: %1.")
                      .arg(losses.join(QStringLiteral(", ")));
    }
    if (!summary.warnings.empty()) {
        result += QObject::tr("\n\nNotes:");
        for (const std::string& warning : summary.warnings) {
            result += QStringLiteral("\n• ") + QString::fromStdString(warning);
        }
    }
    return result;
}

QString palette_file_filter() {
    return QObject::tr(
        "All supported palettes (*.gpl *.kpl *.css *.py *.php *.java *.txt *.hex *.png *.exr);;"
        "GIMP palette (*.gpl);;Krita palette (*.kpl);;CSS stylesheet (*.css);;"
        "Python dictionary (*.py);;PHP dictionary (*.php);;Java map (*.java);;"
        "Hex text (*.txt *.hex);;PNG palette image (*.png);;HALF/FLOAT OpenEXR palette image (*.exr)");
}

class Utf8TextValidator final : public QValidator {
public:
    Utf8TextValidator(TextRule rule, QObject* parent)
        : QValidator(parent), rule_(rule) {}

    State validate(QString& input, int&) const override {
        if (valid_text(input, rule_)) {
            return Acceptable;
        }
        // Let users clear and replace a required field while they are editing;
        // editingFinished still rejects an empty project name.
        if (input.isEmpty()) {
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
    editor->setDecimals(decimals);
    editor->setRange(minimum, maximum);
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

enum EffectUiCategory {
    MovementEffects = 0,
    LightAndEnergyEffects,
    StylizeEffects,
    ParticleEffects,
    BlurEffects,
    EffectUiCategoryCount
};

int effect_ui_category(pvt::EffectType type) {
    switch (type) {
        case pvt::EffectType::EndlessZoom:
        case pvt::EffectType::Ripple:
        case pvt::EffectType::Shake:
        case pvt::EffectType::FlagWave:
        case pvt::EffectType::LensDistortion:
        case pvt::EffectType::Twirl:
            return MovementEffects;
        case pvt::EffectType::Glow:
        case pvt::EffectType::Starburst:
            return LightAndEnergyEffects;
        case pvt::EffectType::BlockScale:
        case pvt::EffectType::Glitch:
        case pvt::EffectType::EdgeDetect:
            return StylizeEffects;
        case pvt::EffectType::ParticleField:
            return ParticleEffects;
        case pvt::EffectType::Blur:
            return BlurEffects;
    }
    return StylizeEffects;
}

QString effect_ui_category_name(int category) {
    switch (category) {
        case MovementEffects: return QObject::tr("Movement & Distortion");
        case LightAndEnergyEffects: return QObject::tr("Light & Energy");
        case StylizeEffects: return QObject::tr("Stylize");
        case ParticleEffects: return QObject::tr("Particles");
        case BlurEffects: return QObject::tr("Blur");
        default: return QObject::tr("Effects");
    }
}

void populate_effect_types(QComboBox* combo, int category) {
    combo->clear();
    const auto add = [combo](pvt::EffectType type) {
        add_enum_item(combo, QString::fromUtf8(pvt::effect_type_name(type)), type);
    };
    switch (category) {
        case MovementEffects:
            add(pvt::EffectType::EndlessZoom);
            add(pvt::EffectType::Ripple);
            add(pvt::EffectType::Shake);
            add(pvt::EffectType::FlagWave);
            add(pvt::EffectType::LensDistortion);
            add(pvt::EffectType::Twirl);
            break;
        case LightAndEnergyEffects:
            add(pvt::EffectType::Glow);
            add(pvt::EffectType::Starburst);
            break;
        case StylizeEffects:
            add(pvt::EffectType::BlockScale);
            add(pvt::EffectType::Glitch);
            add(pvt::EffectType::EdgeDetect);
            break;
        case ParticleEffects:
            add(pvt::EffectType::ParticleField);
            break;
        case BlurEffects:
            add(pvt::EffectType::Blur);
            break;
        default:
            break;
    }
}

pvt::EffectConfig new_effect_for_ui(pvt::EffectType type) {
    auto effect = pvt::default_effect(type);
    // Surface placement is intentionally advanced and opt-in. Keep this UI
    // invariant independent of any renderer or project-file defaults.
    effect.space = pvt::EffectSpace::Texture;
    return effect;
}

void populate_audio_response_combo(QComboBox* combo) {
    const auto add = [combo](const QString& label,
                             pvt::AudioResponseMode value,
                             const QString& tooltip) {
        add_enum_item(combo, label, value);
        combo->setItemData(combo->count() - 1, tooltip, Qt::ToolTipRole);
    };
    add(QObject::tr("Default (use effective profile)"),
        pvt::AudioResponseMode::Default,
        QObject::tr("Inherit both the category switch and source from the effective project/layer profile."));
    add(QObject::tr("Beat"), pvt::AudioResponseMode::Beat,
        QObject::tr("Respond to the analyzed beat pulse and opt this item in."));
    add(QObject::tr("Onset"), pvt::AudioResponseMode::Onset,
        QObject::tr("Respond to detected note and transient attacks and opt this item in."));
    add(QObject::tr("Energy"), pvt::AudioResponseMode::Energy,
        QObject::tr("Respond to overall signal energy and opt this item in."));
    add(QObject::tr("Bass"), pvt::AudioResponseMode::Bass,
        QObject::tr("Respond to low-frequency energy and opt this item in."));
    add(QObject::tr("Midrange"), pvt::AudioResponseMode::Midrange,
        QObject::tr("Respond to midrange energy and opt this item in."));
    add(QObject::tr("Treble"), pvt::AudioResponseMode::Treble,
        QObject::tr("Respond to high-frequency energy and opt this item in."));
    add(QObject::tr("Spectral brightness"),
        pvt::AudioResponseMode::SpectralCentroid,
        QObject::tr("Respond to the normalized spectral centroid and opt this item in."));
    add(QObject::tr("Spectral noisiness"),
        pvt::AudioResponseMode::SpectralFlatness,
        QObject::tr("Respond to spectral flatness and opt this item in."));
    add(QObject::tr("Pitch color (tonality-weighted)"),
        pvt::AudioResponseMode::ChromaHue,
        QObject::tr("Respond to pitch-class hue weighted by tonal confidence and opt this item in."));
    add(QObject::tr("Tonal strength"),
        pvt::AudioResponseMode::ChromaStrength,
        QObject::tr("Respond to tonal confidence and opt this item in."));
    combo->insertSeparator(combo->count());
    add(QObject::tr("Profile source (force this item on)"),
        pvt::AudioResponseMode::Enabled,
        QObject::tr("Use the effective profile's source and opt this item in even when its category switch is off. The profile master switch must still be enabled."));
    add(QObject::tr("Ignore audio"), pvt::AudioResponseMode::Disabled,
        QObject::tr("Keep this item's authored value independent of audio response."));
}

void synchronize_block_scale_maximum_editor(QDoubleSpinBox* editor,
                                             double minimum,
                                             double& authored_maximum) {
    const QSignalBlocker blocker(editor);
    editor->setMinimum(minimum);
    if (authored_maximum < minimum) {
        authored_maximum = minimum;
    }
    editor->setValue(authored_maximum);
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

QString formatted_time(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return QStringLiteral("--:--.---");
    const auto total_milliseconds = static_cast<qint64>(
        std::llround(seconds * 1000.0));
    const qint64 hours = total_milliseconds / 3600000;
    const qint64 minutes = (total_milliseconds / 60000) % 60;
    const qint64 whole_seconds = (total_milliseconds / 1000) % 60;
    const qint64 milliseconds = total_milliseconds % 1000;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3.%4")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(whole_seconds, 2, 10, QLatin1Char('0'))
            .arg(milliseconds, 3, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2.%3")
        .arg(minutes)
        .arg(whole_seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

std::vector<double> music_beats_for_ui(const pvt::ClockConfig& clock) {
    std::vector<double> beats;
    const double offset = static_cast<double>(clock.beat_offset_microseconds)
                          / 1000000.0;
    const std::vector<double>* source_ptr = &clock.music.beat_times_seconds;
    if (!clock.frequency_stream_uuid.empty()) {
        const auto stream = std::find_if(
            clock.music.frequency_streams.cbegin(),
            clock.music.frequency_streams.cend(),
            [&clock](const pvt::MusicFrequencyStreamAnalysis& candidate) {
                return candidate.uuid == clock.frequency_stream_uuid;
            });
        if (stream != clock.music.frequency_streams.cend()) {
            source_ptr = &stream->beat_times_seconds;
        }
    }
    const auto& source = *source_ptr;
    if (clock.music_tempo == pvt::MusicTempoMode::Half) {
        beats.reserve((source.size() + 1U) / 2U);
        for (std::size_t index = 0U; index < source.size(); index += 2U) {
            beats.push_back(source[index] - offset);
        }
    } else if (clock.music_tempo == pvt::MusicTempoMode::Double) {
        beats.reserve(source.empty() ? 0U : source.size() * 2U - 1U);
        for (std::size_t index = 0U; index < source.size(); ++index) {
            beats.push_back(source[index] - offset);
            if (index + 1U < source.size()) {
                beats.push_back((source[index] + source[index + 1U]) * 0.5
                                - offset);
            }
        }
    } else {
        beats.reserve(source.size());
        for (const double beat : source) beats.push_back(beat - offset);
    }
    beats.erase(std::remove_if(
                    beats.begin(), beats.end(), [&clock](double beat) {
                        return !std::isfinite(beat) || beat < 0.0
                               || beat > clock.music.duration_seconds;
                    }),
                beats.end());
    return beats;
}

QString wave_label(const pvt::WaveConfig& wave, std::size_t index) {
    QString routing;
    if (wave.synchronized
        && wave.audio_response != pvt::AudioResponseMode::Default) {
        routing = QStringLiteral(", audio ")
                  + QString::fromUtf8(
                      pvt::audio_response_mode_name(wave.audio_response))
                        .toLower();
    }
    return QString::number(index + 1U) + QStringLiteral(". ")
           + QString::fromStdString(wave.name) + QStringLiteral("  [")
           + (wave.enabled ? QStringLiteral("on") : QStringLiteral("off"))
           + QStringLiteral(", ")
           + (wave.synchronized ? QStringLiteral("sync") : QStringLiteral("free"))
           + routing + QLatin1Char(']');
}

QString swing_label(const pvt::SwingConfig& swing, std::size_t index) {
    return QString::number(index + 1U) + QStringLiteral(". ")
           + QString::fromStdString(swing.name) + QStringLiteral("  [")
           + (swing.enabled ? QStringLiteral("on") : QStringLiteral("off"))
           + QStringLiteral(", ")
           + QString::fromUtf8(pvt::waveform_name(swing.waveform))
           + (swing.radius > 0.0 ? QStringLiteral(", local") : QStringLiteral(", global"))
           + QLatin1Char(']');
}

QString effect_label(const pvt::EffectConfig& effect, std::size_t index) {
    QString routing;
    if (effect.synchronized
        && effect.audio_response != pvt::AudioResponseMode::Default) {
        routing = QStringLiteral(", audio ")
                  + QString::fromUtf8(
                      pvt::audio_response_mode_name(effect.audio_response))
                        .toLower();
    }
    return QString::number(index + 1U) + QStringLiteral(". ")
           + QString::fromStdString(effect.name) + QStringLiteral("  [")
           + QString::fromUtf8(pvt::effect_type_name(effect.type))
           + QStringLiteral(", ")
           + QString::fromUtf8(pvt::effect_space_name(effect.space))
           + QStringLiteral(", ")
           + (effect.enabled ? QStringLiteral("on") : QStringLiteral("off"))
           + QStringLiteral(", ")
           + (effect.synchronized ? QStringLiteral("sync") : QStringLiteral("free"))
           + routing + QLatin1Char(']');
}

std::size_t saturating_add(std::size_t left, std::size_t right) {
    return right > std::numeric_limits<std::size_t>::max() - left
               ? std::numeric_limits<std::size_t>::max()
               : left + right;
}

std::size_t estimated_string_bytes(const std::string& value) {
    return saturating_add(sizeof(std::string), value.capacity() + 1U);
}

std::size_t estimated_compatibility_bytes(
    const pvt::ConfigCompatibility& compatibility) {
    std::size_t bytes = saturating_add(
        compatibility.records.capacity()
            * sizeof(pvt::PreservedConfigRecord),
        compatibility.repair_notes.capacity() * sizeof(std::string));
    for (const pvt::PreservedConfigRecord& record : compatibility.records) {
        bytes = saturating_add(bytes, estimated_string_bytes(record.key));
        bytes = saturating_add(bytes, estimated_string_bytes(record.value));
    }
    for (const std::string& note : compatibility.repair_notes) {
        bytes = saturating_add(bytes, estimated_string_bytes(note));
    }
    return bytes;
}

std::size_t estimated_audio_processing_bytes(
    const pvt::AudioInputProcessingConfig& processing) {
    std::size_t bytes = processing.equalizer_bands.capacity()
                        * sizeof(pvt::AudioEqualizerBandConfig);
    bytes = saturating_add(
        bytes, processing.frequency_streams.capacity()
                   * sizeof(pvt::AudioFrequencyStreamConfig));
    for (const auto& stream : processing.frequency_streams) {
        bytes = saturating_add(bytes, estimated_string_bytes(stream.uuid));
        bytes = saturating_add(bytes, estimated_string_bytes(stream.name));
    }
    return bytes;
}

std::size_t estimated_music_analysis_bytes(const pvt::MusicAnalysis& music) {
    std::size_t bytes = 0U;
    for (const std::string* value : {
             &music.analyzer_version, &music.source_sha256,
             &music.source_basename, &music.source_format}) {
        bytes = saturating_add(bytes, estimated_string_bytes(*value));
    }
    bytes = saturating_add(
        bytes, music.beat_times_seconds.capacity() * sizeof(double));
    bytes = saturating_add(
        bytes, music.tempo_points.capacity() * sizeof(pvt::MusicTempoPoint));
    bytes = saturating_add(
        bytes, music.feature_samples.capacity() * sizeof(pvt::MusicFeatureSample));
    bytes = saturating_add(
        bytes, music.frequency_streams.capacity()
                   * sizeof(pvt::MusicFrequencyStreamAnalysis));
    for (const auto& stream : music.frequency_streams) {
        bytes = saturating_add(bytes, estimated_string_bytes(stream.uuid));
        bytes = saturating_add(
            bytes, stream.beat_times_seconds.capacity() * sizeof(double));
        bytes = saturating_add(
            bytes, stream.tempo_points.capacity() * sizeof(pvt::MusicTempoPoint));
        bytes = saturating_add(
            bytes, stream.feature_samples.capacity()
                       * sizeof(pvt::MusicFeatureSample));
    }
    bytes = saturating_add(
        bytes, estimated_audio_processing_bytes(music.input_processing));
    return saturating_add(
        bytes, estimated_compatibility_bytes(music.compatibility));
}

std::size_t estimated_render_data_bytes(const pvt::RenderData& render) {
    std::size_t bytes = sizeof(pvt::RenderData);
    bytes = saturating_add(bytes,
                           render.waves.capacity() * sizeof(pvt::WaveConfig));
    bytes = saturating_add(bytes,
                           render.swings.capacity() * sizeof(pvt::SwingConfig));
    bytes = saturating_add(bytes,
                           render.effects.capacity() * sizeof(pvt::EffectConfig));
    for (const auto& wave : render.waves) {
        bytes = saturating_add(bytes, estimated_string_bytes(wave.name));
    }
    for (const auto& swing : render.swings) {
        bytes = saturating_add(bytes, estimated_string_bytes(swing.name));
    }
    for (const auto& effect : render.effects) {
        bytes = saturating_add(bytes, estimated_string_bytes(effect.name));
    }
    bytes = saturating_add(bytes, estimated_string_bytes(render.surface.obj_path));
    bytes = saturating_add(bytes, estimated_string_bytes(render.surface.obj_sha256));
    bytes = saturating_add(bytes, estimated_string_bytes(render.surface.obj_basename));
    bytes = saturating_add(
        bytes, estimated_string_bytes(
                   render.surface.plane_displacement.path));
    bytes = saturating_add(
        bytes, estimated_string_bytes(
                   render.surface.plane_displacement.sha256));
    bytes = saturating_add(
        bytes, estimated_string_bytes(
                   render.surface.plane_displacement.basename));
    bytes = saturating_add(bytes,
                           estimated_string_bytes(render.starting_image.path));
    bytes = saturating_add(bytes,
                           estimated_string_bytes(render.starting_image.sha256));
    bytes = saturating_add(bytes,
                           estimated_string_bytes(render.starting_image.basename));
    bytes = saturating_add(bytes, estimated_string_bytes(render.palette.name));
    bytes = saturating_add(
        bytes, render.palette.colors.capacity() * sizeof(pvt::PaletteColor));
    for (const auto& color : render.palette.colors) {
        bytes = saturating_add(bytes, estimated_string_bytes(color.name));
    }
    bytes = saturating_add(
        bytes, estimated_compatibility_bytes(render.source_compatibility));
    bytes = saturating_add(
        bytes, estimated_music_analysis_bytes(render.layer_clock.clock.music));
    bytes = saturating_add(
        bytes, estimated_audio_processing_bytes(
                   render.layer_clock.clock.audio_processing));
    return bytes;
}

std::size_t estimated_canvas_bytes(const pvt::CanvasLoopConfig& canvas) {
    const auto& music = canvas.clock.music;
    std::size_t bytes = sizeof(pvt::CanvasLoopConfig);
    bytes = saturating_add(
        bytes, estimated_string_bytes(canvas.clock.meter.expression));
    bytes = saturating_add(bytes, estimated_music_analysis_bytes(music));
    bytes = saturating_add(
        bytes, estimated_audio_processing_bytes(canvas.clock.audio_processing));
    bytes = saturating_add(
        bytes, estimated_compatibility_bytes(canvas.output_compatibility));
    bytes = saturating_add(
        bytes, canvas.motion_paths.capacity() * sizeof(pvt::CubicMotionPath));
    for (const auto& path : canvas.motion_paths) {
        bytes = saturating_add(bytes, estimated_string_bytes(path.name));
        bytes = saturating_add(
            bytes, path.nodes.capacity() * sizeof(pvt::CubicPathNode));
    }
    return bytes;
}

std::size_t estimated_attachment_bytes(
    const std::vector<pvt::ProjectAttachment>& attachments) {
    std::size_t bytes = saturating_add(
        sizeof(std::vector<pvt::ProjectAttachment>),
        attachments.capacity() * sizeof(pvt::ProjectAttachment));
    for (const auto& attachment : attachments) {
        for (const std::string* value : {
                 &attachment.reference_id, &attachment.sha256,
                 &attachment.basename, &attachment.local_path,
                 &attachment.bundle_path}) {
            bytes = saturating_add(bytes, estimated_string_bytes(*value));
        }
    }
    return bytes;
}

std::size_t estimated_output_bytes(const pvt::ExportConfig& output) {
    return saturating_add(
        saturating_add(sizeof(pvt::ExportConfig),
                       estimated_string_bytes(output.output_directory)),
        estimated_string_bytes(output.filename_prefix));
}

std::size_t estimated_project_bytes(const pvt::ProjectConfig& project) {
    std::size_t bytes = sizeof(pvt::ProjectConfig);
    bytes = saturating_add(bytes, estimated_string_bytes(project.uuid));
    bytes = saturating_add(bytes, estimated_string_bytes(project.name));
    bytes = saturating_add(bytes, estimated_canvas_bytes(project.canvas));
    bytes = saturating_add(bytes, estimated_output_bytes(project.output));
    bytes = saturating_add(bytes,
                           project.layers.capacity() * sizeof(pvt::LayerConfig));
    bytes = saturating_add(bytes,
                           project.groups.capacity() * sizeof(pvt::LayerGroup));
    for (const auto& group : project.groups) {
        bytes = saturating_add(bytes, estimated_string_bytes(group.uuid));
        bytes = saturating_add(bytes, estimated_string_bytes(group.name));
    }
    for (const auto& layer : project.layers) {
        bytes = saturating_add(bytes, estimated_string_bytes(layer.uuid));
        bytes = saturating_add(bytes, estimated_string_bytes(layer.name));
        bytes = saturating_add(bytes,
                               estimated_string_bytes(layer.group_uuid));
        bytes = saturating_add(bytes, estimated_render_data_bytes(layer.render));
    }
    return bytes;
}

bool attachments_equal(const std::vector<pvt::ProjectAttachment>& left,
                       const std::vector<pvt::ProjectAttachment>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.reference_id != b.reference_id || a.sha256 != b.sha256
            || a.basename != b.basename || a.size_bytes != b.size_bytes
            || a.local_path != b.local_path || a.bundle_path != b.bundle_path
            || a.externally_modified != b.externally_modified) {
            return false;
        }
    }
    return true;
}

bool render_data_equal(const pvt::RenderData& left,
                       const pvt::RenderData& right,
                       const std::vector<pvt::CubicMotionPath>* left_paths = nullptr,
                       const std::vector<pvt::CubicMotionPath>* right_paths = nullptr) {
    std::string left_bytes;
    std::string right_bytes;
    return pvt::detail::serialize_layer_config(
               left, left_bytes, nullptr, left_paths)
           && pvt::detail::serialize_layer_config(
               right, right_bytes, nullptr, right_paths)
           && left_bytes == right_bytes;
}

bool output_data_equal(const pvt::CanvasLoopConfig& left_canvas,
                       const pvt::ExportConfig& left_output,
                       const pvt::CanvasLoopConfig& right_canvas,
                       const pvt::ExportConfig& right_output) {
    std::string left_bytes;
    std::string right_bytes;
    return pvt::detail::serialize_render_output_config(
               left_canvas, left_output, left_bytes, nullptr)
           && pvt::detail::serialize_render_output_config(
               right_canvas, right_output, right_bytes, nullptr)
           && left_bytes == right_bytes;
}

bool project_config_equal(const pvt::ProjectConfig& left,
                          const pvt::ProjectConfig& right) {
    if (left.uuid != right.uuid || left.name != right.name
        || left.layers.size() != right.layers.size()
        || left.groups.size() != right.groups.size()
        || !output_data_equal(left.canvas, left.output,
                              right.canvas, right.output)) {
        return false;
    }
    for (std::size_t index = 0U; index < left.layers.size(); ++index) {
        const auto& a = left.layers[index];
        const auto& b = right.layers[index];
        if (a.uuid != b.uuid || a.file_id != b.file_id || a.name != b.name
            || a.enabled != b.enabled || a.blend_mode != b.blend_mode
            || a.alpha_mode != b.alpha_mode
            || a.group_uuid != b.group_uuid || a.opacity != b.opacity
            || !render_data_equal(a.render, b.render,
                                  &left.canvas.motion_paths,
                                  &right.canvas.motion_paths)) {
            return false;
        }
    }
    for (std::size_t index = 0U; index < left.groups.size(); ++index) {
        const auto& a = left.groups[index];
        const auto& b = right.groups[index];
        if (a.uuid != b.uuid || a.name != b.name
            || a.enabled != b.enabled || a.locked != b.locked) {
            return false;
        }
    }
    return true;
}

QString friendly_diff_value(const std::string& value) {
    const QString raw = QString::fromStdString(value);
    if (!raw.contains('.') && !raw.contains('e', Qt::CaseInsensitive)) return raw;
    bool numeric = false;
    const double parsed = raw.toDouble(&numeric);
    if (!numeric || !std::isfinite(parsed)) return raw;
    const QString compact = QString::number(parsed, 'g', 15);
    return compact.size() < raw.size() ? compact : raw;
}

bool layer_visible_in_project(const pvt::ProjectConfig& project,
                              const pvt::LayerConfig& layer);

bool configuration_requires_alpha(const pvt::RenderConfig& config) {
    if (config.post_process.invert_alpha_enabled
        && config.post_process.invert_alpha_mix > 0.0) {
        return true;
    }
    if (config.alpha.use_source_alpha) {
        if (config.starting_image.enabled) return true;
        if (config.palette.enabled
            && std::any_of(config.palette.colors.begin(), config.palette.colors.end(),
                           [](const pvt::PaletteColor& color) {
                               return color.alpha < 1.0;
                           })) {
            return true;
        }
    }
    if (!config.starting_image.enabled && !config.palette.enabled
        && config.starting_colors.include_alpha
        && config.starting_colors.alpha_minimum < 1.0) {
        return true;
    }
    if (config.surface.enabled
        && (config.surface.mapping != pvt::SurfaceMapping::Plane
            || config.surface.plane_displacement.enabled)
        && config.surface.curvature > 0.0) {
        return true;
    }
    if (config.motion.enabled) {
        const bool built_in_path_has_work =
            config.motion.path != pvt::LayerMotionPath::None
            && (std::fabs(config.motion.travel_x) > 1.0e-12
                || std::fabs(config.motion.travel_y) > 1.0e-12);
        const bool scale_has_work =
            config.motion.scale_pulse > 1.0e-12
            && (config.motion.cycles_y != 0
                || std::fmod(config.motion.phase_degrees, 180.0) != 0.0);
        if (built_in_path_has_work
            || config.motion.custom_path.enabled
            || std::fabs(config.motion.center_x - 0.5) > 1.0e-12
            || std::fabs(config.motion.center_y - 0.5) > 1.0e-12
            || config.motion.rotations_per_loop != 0
            || std::fmod(config.motion.rotation_offset_degrees, 360.0) != 0.0
            || scale_has_work) {
            return true;
        }
    }
    return std::any_of(config.effects.begin(), config.effects.end(), [](const auto& effect) {
        const bool active_blur = effect.type == pvt::EffectType::Blur
                                 && effect.radius_pixels > 0.0
                                 && effect.blur_maximum > 0.0;
        const bool active_coordinate = effect.intensity > 0.0
                                       && effect.magnitude > 0.0
                                       && effect.type != pvt::EffectType::Blur
                                       && ((effect.type
                                                != pvt::EffectType::LensDistortion
                                            && effect.type
                                                   != pvt::EffectType::Twirl)
                                           || effect.secondary != 0.0);
        return effect.enabled && (active_blur || active_coordinate)
               && effect.type != pvt::EffectType::Glow
               && effect.type != pvt::EffectType::BlockScale
               && effect.type != pvt::EffectType::ParticleField
               && effect.edge_mode == pvt::EdgeMode::Alpha;
    });
}

bool visible_stack_requires_alpha(
    const pvt::ProjectConfig& project,
    const pvt::RenderConfig* active_configuration = nullptr,
    std::string_view active_layer_uuid = {}) {
    bool guaranteed_opaque = false;
    for (const auto& layer : project.layers) {
        if (!layer_visible_in_project(project, layer) || layer.opacity <= 0.0) {
            continue;
        }
        const pvt::RenderConfig materialized =
            active_configuration != nullptr && layer.uuid == active_layer_uuid
                ? *active_configuration
                : pvt::apply_global_config(
                      project.canvas, project.output, layer.render);
        const bool erases_lower_layers =
            layer.blend_mode == pvt::BlendMode::Erase
            || layer.blend_mode == pvt::BlendMode::ColorEraseTones
            || layer.blend_mode == pvt::BlendMode::ColorEraseBrightness;
        if (erases_lower_layers) {
            const bool particle_can_synthesize_coverage = std::any_of(
                materialized.effects.begin(), materialized.effects.end(),
                [](const pvt::EffectConfig& effect) {
                    return effect.enabled
                           && effect.type == pvt::EffectType::ParticleField
                           && effect.intensity > 0.0
                           && effect.frequency >= 1.0
                           && effect.radius_pixels > 0.0;
                });
            const bool source_is_guaranteed_transparent =
                materialized.alpha.enabled
                && materialized.alpha.maximum == 0.0
                && !particle_can_synthesize_coverage;
            if (!source_is_guaranteed_transparent) {
                // Destination-out can remove coverage established by every
                // lower layer. A later ordinary opaque layer may establish it.
                guaranteed_opaque = false;
            }
            continue;
        }
        const bool procedural_transparency = materialized.alpha.enabled
            && (materialized.alpha.minimum < 1.0
                || materialized.alpha.maximum < 1.0);
        const bool source_guaranteed_opaque = layer.opacity >= 1.0
            && !procedural_transparency
            && !configuration_requires_alpha(materialized);
        // Source-over and destination-over have the same coverage union. Once
        // either ordinary operand covers the full canvas, their result does too.
        guaranteed_opaque = guaranteed_opaque || source_guaranteed_opaque;
    }
    return !guaranteed_opaque;
}

void scale_project_for_preview(pvt::ProjectConfig& project) {
    const int source_width = project.canvas.width;
    const int source_height = project.canvas.height;
    const int source_block_size = project.canvas.block_size;
    const int source_short_edge =
        std::max(1, std::min(project.canvas.width, project.canvas.height));
    const double scale = std::min(
        {1.0, 720.0 / static_cast<double>(project.canvas.width),
         480.0 / static_cast<double>(project.canvas.height)});
    const int preview_width =
        std::max(16, static_cast<int>(std::lround(project.canvas.width * scale)));
    const int preview_height =
        std::max(16, static_cast<int>(std::lround(project.canvas.height * scale)));
    const double pixel_scale =
        static_cast<double>(std::min(preview_width, preview_height))
        / static_cast<double>(source_short_edge);

    // These controls are defined in output pixels. Scale them with the preview
    // so the low-resolution image preserves their full-resolution proportions.
    for (auto& layer : project.layers) {
        layer.render.starting_colors.reference_width = source_width;
        layer.render.starting_colors.reference_height = source_height;
        layer.render.starting_colors.reference_block_size = source_block_size;
        layer.render.displacement *= pixel_scale;
        for (auto& effect : layer.render.effects) {
            if (effect.type == pvt::EffectType::Glow
                || effect.type == pvt::EffectType::ParticleField
                || effect.type == pvt::EffectType::Blur) {
                effect.radius_pixels *= pixel_scale;
            } else if (effect.type == pvt::EffectType::EdgeDetect) {
                effect.frequency = std::max(
                    1.0, std::round(effect.frequency * pixel_scale));
            }
        }
    }
    project.canvas.width = preview_width;
    project.canvas.height = preview_height;
    project.canvas.block_size = std::max(
        1, static_cast<int>(std::lround(project.canvas.block_size * scale)));
}

void set_form_label(QFormLayout* form, QWidget* field, const QString& text) {
    if (auto* label = qobject_cast<QLabel*>(form->labelForField(field))) {
        label->setText(text);
    }
}

bool clock_route_targets(const pvt::LiveClockInputConfig& route,
                         pvt::LiveClockTarget target,
                         const std::string& layer_uuid) {
    return route.target == target
        && (target == pvt::LiveClockTarget::Project
            || route.layer_uuid == layer_uuid);
}

const pvt::LiveClockInputConfig* effective_audio_clock_route(
    const pvt::LiveConfig& live, pvt::LiveClockTarget target,
    const std::string& layer_uuid) {
    const auto route = std::find_if(
        live.clock_inputs.begin(), live.clock_inputs.end(),
        [target, &layer_uuid](const pvt::LiveClockInputConfig& item) {
            return item.enabled
                && item.source == pvt::LiveClockInputSource::AudioStream
                && clock_route_targets(item, target, layer_uuid);
        });
    return route == live.clock_inputs.end() ? nullptr : &*route;
}

const pvt::LiveClockInputConfig* first_enabled_audio_clock_route(
    const pvt::LiveConfig& live) {
    const auto route = std::find_if(
        live.clock_inputs.begin(), live.clock_inputs.end(),
        [](const pvt::LiveClockInputConfig& item) {
            return item.enabled
                && item.source == pvt::LiveClockInputSource::AudioStream;
        });
    return route == live.clock_inputs.end() ? nullptr : &*route;
}

void remove_layer_live_clock_routes(pvt::LiveConfig& live,
                                    const std::string& layer_uuid) {
    live.clock_inputs.erase(
        std::remove_if(
            live.clock_inputs.begin(), live.clock_inputs.end(),
            [&layer_uuid](const pvt::LiveClockInputConfig& route) {
                return route.target == pvt::LiveClockTarget::Layer
                    && route.layer_uuid == layer_uuid;
            }),
        live.clock_inputs.end());
    live.midi_clock_outputs.erase(
        std::remove_if(
            live.midi_clock_outputs.begin(), live.midi_clock_outputs.end(),
            [&layer_uuid](const pvt::LiveMidiClockOutputConfig& output) {
                return output.source == pvt::LiveClockTarget::Layer
                    && output.layer_uuid == layer_uuid;
            }),
        live.midi_clock_outputs.end());
}

bool effective_active_clock_is_music(const pvt::RenderConfig& config,
                                     const std::string& active_layer_uuid) {
    const bool live_audio = effective_audio_clock_route(
                                config.live,
                                pvt::LiveClockTarget::Project, {})
                            != nullptr
        || effective_audio_clock_route(
               config.live, pvt::LiveClockTarget::Layer,
               active_layer_uuid)
               != nullptr;
    return live_audio
        || (config.layer_clock.enabled ? config.layer_clock.clock.mode
                                       : config.clock.mode)
               == pvt::ClockMode::Music;
}

QString existing_writable_directory(const QString& path, bool allow_root = false) {
    if (path.isEmpty()) {
        return {};
    }
    const QFileInfo information(path);
    if (!information.exists() || !information.isDir() || !information.isWritable()) {
        return {};
    }
    const QDir directory(information.absoluteFilePath());
    if (!allow_root && directory.isRoot()) {
        return {};
    }
    return QDir::cleanPath(directory.absolutePath());
}

bool equivalent_local_path(const QString& first, const QString& second) {
    if (first.isEmpty() || second.isEmpty()) return false;
    const QFileInfo first_info(first);
    const QFileInfo second_info(second);
    const QString first_canonical = first_info.canonicalFilePath();
    const QString second_canonical = second_info.canonicalFilePath();
    if (!first_canonical.isEmpty() && !second_canonical.isEmpty()) {
        return first_canonical == second_canonical;
    }
    return QDir::cleanPath(first_info.absoluteFilePath())
           == QDir::cleanPath(second_info.absoluteFilePath());
}

QString requested_working_directory() {
    const QStringList arguments = QCoreApplication::arguments();
    for (qsizetype index = 1; index < arguments.size(); ++index) {
        const QString& argument = arguments.at(index);
        QString candidate;
        if (argument == QStringLiteral("--working-directory")
            && index + 1 < arguments.size()) {
            candidate = arguments.at(index + 1);
        } else if (argument.startsWith(QStringLiteral("--working-directory="))) {
            candidate = argument.mid(QStringLiteral("--working-directory=").size());
        }
        if (!candidate.isEmpty()) {
            const QString absolute = QDir::isAbsolutePath(candidate)
                                         ? candidate
                                         : QDir::current().absoluteFilePath(candidate);
            if (const QString usable = existing_writable_directory(absolute);
                !usable.isEmpty()) {
                return usable;
            }
        }
    }
    return {};
}

QString stable_startup_directory() {
    if (const QString requested = requested_working_directory(); !requested.isEmpty()) {
        return requested;
    }
    if (const QString current = existing_writable_directory(QDir::currentPath());
        !current.isEmpty()) {
        return current;
    }
    const QString environment_working_directory = qEnvironmentVariable("PWD");
    if (const QString inherited =
            existing_writable_directory(environment_working_directory);
        !inherited.isEmpty()) {
        return inherited;
    }
    if (const QString home = existing_writable_directory(QDir::homePath(), true);
        !home.isEmpty()) {
        return home;
    }
    return QDir::homePath();
}

double random_real(QRandomGenerator& random, double minimum, double maximum) {
    return minimum + (maximum - minimum) * random.generateDouble();
}

int random_integer(QRandomGenerator& random, int minimum, int maximum) {
    return minimum
           + static_cast<int>(random.bounded(
               static_cast<quint32>(maximum - minimum + 1)));
}

bool random_chance(QRandomGenerator& random, double probability) {
    return random.generateDouble() < probability;
}

bool alias_project_attachment(pvt::ProjectDocument& document,
                              const std::string& source_reference,
                              const std::string& target_reference,
                              QString* error) {
    if (error != nullptr) error->clear();
    const auto source = std::find_if(
        document.attachments.begin(), document.attachments.end(),
        [&source_reference](const pvt::ProjectAttachment& attachment) {
            return attachment.reference_id == source_reference;
        });
    if (source == document.attachments.end()) {
        if (error != nullptr) *error = QObject::tr("The reusable asset is unavailable.");
        return false;
    }
    pvt::ProjectAttachment alias = *source;
    alias.reference_id = target_reference;
    const auto target = std::find_if(
        document.attachments.begin(), document.attachments.end(),
        [&target_reference](const pvt::ProjectAttachment& attachment) {
            return attachment.reference_id == target_reference;
        });
    if (target == document.attachments.end()) {
        if (document.attachments.size()
            >= pvt::kMaximumProjectAttachmentReferences) {
            if (error != nullptr) {
                *error = QObject::tr("The project attachment-reference limit has been reached.");
            }
            return false;
        }
        document.attachments.push_back(std::move(alias));
    } else {
        *target = std::move(alias);
    }
    document.dirty = true;
    return true;
}

bool layer_visible_in_project(const pvt::ProjectConfig& project,
                              const pvt::LayerConfig& layer) {
    if (!layer.enabled) return false;
    if (layer.group_uuid.empty()) return true;
    const auto group = std::find_if(
        project.groups.begin(), project.groups.end(),
        [&layer](const pvt::LayerGroup& value) {
            return value.uuid == layer.group_uuid;
        });
    return group == project.groups.end() || group->enabled;
}

std::vector<pvt::audio::PlaybackTrack> audible_project_tracks(
    const pvt::ProjectConfig& project, const pvt::ProjectDocument& document,
    double project_position_seconds) {
    std::vector<pvt::audio::PlaybackTrack> tracks;
    std::string frame_error;
    const int frames = pvt::effective_frame_count(project.canvas, &frame_error);
    if (frames < 1 || !(project.canvas.fps > 0.0)) return tracks;
    const double project_duration = static_cast<double>(frames)
                                    / project.canvas.fps;
    const double clock_duration =
        project.canvas.clock.mode == pvt::ClockMode::Music
                && project.canvas.clock.music.duration_seconds > 0.0
            ? project.canvas.clock.music.duration_seconds
            : project_duration;
    const auto append = [&](const std::string& path, double source_duration,
                            pvt::LayerClockScale mapping,
                            bool project_clock) {
        if (path.empty() || !(source_duration > 0.0)
            || project_position_seconds >= project_duration) return;
        pvt::audio::PlaybackTrack track;
        track.path = path;
        if (project_clock) {
            track.playback_rate = 1.0;
            // The master music determines the project duration. That duration
            // is rounded up to a whole video frame, so it can outlast the file
            // by a few milliseconds. Leave that sliver silent instead of
            // unexpectedly replaying the beginning of the track.
            if (project_position_seconds >= source_duration) return;
            track.source_position_seconds = project_position_seconds;
            track.stop_after_seconds = source_duration
                                       - project_position_seconds;
            tracks.push_back(std::move(track));
            return;
        }
        if (project_position_seconds >= clock_duration) return;
        switch (mapping) {
            case pvt::LayerClockScale::SmartLoopFit: {
                const int loops = std::max(
                    1, static_cast<int>(std::floor(
                           clock_duration / source_duration)));
                track.playback_rate = static_cast<double>(loops)
                                      * source_duration / clock_duration;
                track.source_position_seconds = std::fmod(
                    project_position_seconds * track.playback_rate,
                    source_duration);
                track.loop = true;
                track.stop_after_seconds = clock_duration
                                           - project_position_seconds;
                break;
            }
            case pvt::LayerClockScale::StraightFit:
                track.playback_rate = source_duration / clock_duration;
                track.source_position_seconds =
                    project_position_seconds * track.playback_rate;
                track.stop_after_seconds = clock_duration
                                           - project_position_seconds;
                break;
            case pvt::LayerClockScale::PlayOnce:
            case pvt::LayerClockScale::PlayOnceThenProject:
                if (project_position_seconds >= source_duration) return;
                track.source_position_seconds = project_position_seconds;
                track.stop_after_seconds = source_duration
                                           - project_position_seconds;
                break;
            case pvt::LayerClockScale::OriginalSpeedLoop:
                track.source_position_seconds = std::fmod(
                    project_position_seconds, source_duration);
                track.loop = true;
                track.stop_after_seconds = clock_duration
                                           - project_position_seconds;
                break;
        }
        tracks.push_back(std::move(track));
    };

    const auto& project_clock = project.canvas.clock;
    if (project_clock.mode == pvt::ClockMode::Music
        && !project_clock.data_only) {
        append(pvt::project_attachment_path(
                   document, pvt::kMusicSourceAttachmentId),
               project_clock.music.duration_seconds,
               pvt::LayerClockScale::OriginalSpeedLoop, true);
    }
    for (const pvt::LayerConfig& layer : project.layers) {
        const auto& local = layer.render.layer_clock;
        if (!layer_visible_in_project(project, layer) || !local.enabled
            || local.clock.mode != pvt::ClockMode::Music
            || local.clock.data_only) {
            continue;
        }
        append(pvt::project_attachment_path(
                   document, pvt::layer_music_attachment_id(layer.uuid)),
               local.clock.music.duration_seconds, local.scale, false);
    }
    return tracks;
}

int random_nonzero_cycles(QRandomGenerator& random, int maximum_magnitude) {
    const int magnitude = random_integer(random, 1, maximum_magnitude);
    return random_chance(random, 0.25) ? -magnitude : magnitude;
}

void randomize_wave_settings(pvt::WaveConfig& wave, QRandomGenerator& random) {
    wave.synchronized = random_chance(random, 0.7);
    wave.x_percent = random_real(random, 8.0, 92.0);
    wave.y_percent = random_real(random, 8.0, 92.0);
    wave.amplitude = random_real(random, 0.12, 0.8);
    wave.spatial_frequency = random_real(random, 2.0, 14.0);
    wave.cycles_per_loop = random_nonzero_cycles(random, 5);
    wave.phase_degrees = random_real(random, 0.0, 360.0);
    wave.direction = random_real(random, 0.0, 1.0);
}

void randomize_swing_settings(pvt::SwingConfig& swing, QRandomGenerator& random) {
    const double amount = random_real(random, 0.08, 0.42);
    swing.amount = random_chance(random, 0.2) ? -amount : amount;
    swing.cycles_per_loop = random_integer(random, 2, 16);
    swing.phase_degrees = random_real(random, 0.0, 360.0);
    swing.shape = random_real(random, 0.15, 0.85);
    swing.center_x = random_real(random, 0.15, 0.85);
    swing.center_y = random_real(random, 0.15, 0.85);
    swing.radius = random_chance(random, 0.35)
                       ? random_real(random, 0.12, 0.48) : 0.0;
}

void randomize_effect_settings(pvt::EffectConfig& effect, QRandomGenerator& random) {
    const auto id = effect.id;
    const auto name = effect.name;
    const auto type = effect.type;
    const bool enabled = effect.enabled;
    effect = pvt::default_effect(type);
    effect.id = id;
    effect.name = name;
    effect.enabled = enabled;
    effect.synchronized = random_chance(random, 0.7);
    // Mapping placement is deliberately opt-in. Randomization should not
    // promote a rarely useful advanced placement into an accidental default.
    effect.space = pvt::EffectSpace::Texture;
    effect.cycles_per_loop = random_nonzero_cycles(random, 4);
    effect.phase_degrees = random_real(random, 0.0, 360.0);
    const int edge = random_integer(random, 0, 5);
    effect.edge_mode = edge < 3
                           ? pvt::EdgeMode::Reflect
                           : static_cast<pvt::EdgeMode>(edge - 3);
    effect.area_radius = type != pvt::EffectType::BlockScale
                                 && random_chance(random, 0.3)
                             ? random_real(random, 0.12, 0.55) : 0.0;

    switch (type) {
        case pvt::EffectType::EndlessZoom:
            effect.intensity = random_real(random, 0.3, 0.85);
            effect.magnitude = random_real(random, 0.2, 0.9);
            effect.frequency = random_real(random, 0.8, 2.5);
            effect.center_x = random_real(random, 0.35, 0.65);
            effect.center_y = random_real(random, 0.35, 0.65);
            break;
        case pvt::EffectType::Ripple:
            effect.intensity = random_real(random, 0.35, 0.9);
            effect.magnitude = random_real(random, 0.01, 0.055);
            effect.frequency = random_real(random, 2.0, 10.0);
            effect.secondary = random_real(random, 1.0, 4.0);
            effect.center_x = random_real(random, 0.2, 0.8);
            effect.center_y = random_real(random, 0.2, 0.8);
            break;
        case pvt::EffectType::Shake:
            effect.intensity = random_real(random, 0.25, 0.7);
            effect.magnitude = random_real(random, 0.006, 0.035);
            effect.frequency = static_cast<double>(random_integer(random, 1, 7));
            effect.secondary = random_real(random, 0.35, 1.0);
            effect.center_x = random_real(random, 0.2, 0.8);
            effect.center_y = random_real(random, 0.2, 0.8);
            effect.angle_degrees = random_real(random, 0.0, 360.0);
            break;
        case pvt::EffectType::FlagWave:
            effect.intensity = random_real(random, 0.3, 0.85);
            effect.magnitude = random_real(random, 0.01, 0.055);
            effect.frequency = random_real(random, 1.5, 7.0);
            effect.secondary = random_real(random, 0.1, 0.8);
            effect.center_x = random_real(random, 0.25, 0.75);
            effect.center_y = random_real(random, 0.25, 0.75);
            effect.angle_degrees = random_real(random, 0.0, 360.0);
            break;
        case pvt::EffectType::Glow:
            effect.intensity = random_real(random, 0.55, 1.35);
            effect.secondary = random_real(random, 0.15, 0.8);
            effect.radius_pixels = random_real(random, 8.0, 56.0);
            effect.threshold = random_real(random, 0.2, 0.65);
            effect.soft_knee = random_real(random, 0.15, 0.6);
            effect.center_x = random_real(random, 0.2, 0.8);
            effect.center_y = random_real(random, 0.2, 0.8);
            break;
        case pvt::EffectType::BlockScale:
            effect.intensity = random_real(random, 0.65, 1.0);
            effect.magnitude = random_real(random, 0.5, 1.25);
            effect.frequency = effect.magnitude + random_real(random, 0.75, 5.0);
            effect.secondary = random_chance(random, 0.5)
                                   ? 0.0
                                   : static_cast<double>(random_integer(random, 1, 6));
            break;
        case pvt::EffectType::ParticleField:
            effect.particle_shape = static_cast<pvt::ParticleShape>(
                random_integer(random, 0, 4));
            effect.particle_profile = pvt::ParticleRenderProfile::Defined;
            effect.intensity = random_real(random, 0.45, 2.2);
            effect.magnitude = random_real(random, 0.05, 0.55);
            effect.frequency = static_cast<double>(random_integer(random, 24, 256));
            effect.secondary = random_real(random, 0.05, 0.85);
            effect.angle_degrees = random_real(random, -180.0, 180.0);
            effect.radius_pixels = random_real(random, 4.0, 28.0);
            effect.threshold = random_real(random, 0.25, 0.9);
            effect.soft_knee = random_real(random, 0.15, 0.8);
            effect.particle_size_variation = random_real(random, 0.0, 0.7);
            effect.particle_definition = random_real(random, 0.45, 1.0);
            effect.particle_twinkle = random_real(random, 0.0, 1.0);
            effect.particle_seed = random.generate64();
            if (effect.particle_seed == 0U) effect.particle_seed = 1U;
            effect.particle_orientation = static_cast<pvt::ParticleOrientation>(
                random_integer(random, 0, 2));
            effect.particle_rotation_degrees = random_real(
                random, -180.0, 180.0);
            break;
        case pvt::EffectType::Blur:
            effect.blur_type = static_cast<pvt::BlurType>(
                random_integer(random, 0, 4));
            effect.intensity = random_real(random, 0.2, 0.9);
            effect.radius_pixels = random_real(random, 2.0, 48.0);
            effect.blur_passes = random_integer(random, 1, 3);
            effect.blur_samples = 2 * random_integer(random, 2, 16) + 1;
            effect.blur_minimum = random_real(random, 0.0, 0.35);
            effect.blur_maximum = random_real(
                random, (std::max)(effect.blur_minimum, 0.45), 1.0);
            effect.center_x = random_real(random, 0.2, 0.8);
            effect.center_y = random_real(random, 0.2, 0.8);
            effect.angle_degrees = random_real(random, -180.0, 180.0);
            break;
        case pvt::EffectType::Glitch:
            effect.intensity = random_real(random, 0.3, 0.9);
            effect.magnitude = random_real(random, 0.008, 0.08);
            effect.frequency = static_cast<double>(
                random_integer(random, 8, 72));
            effect.secondary = random_real(random, 0.1, 0.9);
            effect.center_x = random_real(random, 0.2, 0.8);
            effect.center_y = random_real(random, 0.2, 0.8);
            break;
        case pvt::EffectType::Starburst:
            effect.intensity = random_real(random, 0.3, 0.9);
            effect.magnitude = random_real(random, 0.008, 0.08);
            effect.frequency = static_cast<double>(
                random_integer(random, 4, 32));
            effect.secondary = random_real(random, 0.15, 0.9);
            effect.center_x = random_real(random, 0.25, 0.75);
            effect.center_y = random_real(random, 0.25, 0.75);
            effect.angle_degrees = random_real(random, -180.0, 180.0);
            break;
        case pvt::EffectType::LensDistortion:
            effect.intensity = random_real(random, 0.3, 0.9);
            effect.magnitude = random_real(random, 0.04, 0.35);
            effect.frequency = random_real(random, 1.0, 4.0);
            effect.secondary = random_chance(random, 0.5) ? -1.0 : 1.0;
            effect.center_x = random_real(random, 0.3, 0.7);
            effect.center_y = random_real(random, 0.3, 0.7);
            break;
        case pvt::EffectType::EdgeDetect:
            effect.intensity = random_real(random, 0.35, 1.0);
            effect.magnitude = random_real(random, 0.6, 3.0);
            effect.frequency = static_cast<double>(random_integer(random, 1, 4));
            effect.secondary = random_real(random, 0.0, 0.25);
            effect.center_x = random_real(random, 0.3, 0.7);
            effect.center_y = random_real(random, 0.3, 0.7);
            break;
        case pvt::EffectType::Twirl:
            effect.intensity = random_real(random, 0.3, 0.95);
            effect.magnitude = random_real(random, 0.08, 0.75);
            effect.frequency = random_real(random, 0.5, 3.5);
            effect.secondary = random_chance(random, 0.5) ? -1.0 : 1.0;
            effect.center_x = random_real(random, 0.3, 0.7);
            effect.center_y = random_real(random, 0.3, 0.7);
            break;
    }
}

bool editor_change_is_continuous(const QObject* editor) {
    // Checkboxes and enum choices are discrete user decisions. Merging two of
    // them can turn an on->off sequence into a phantom Undo command whose
    // before and after values are both off.
    return qobject_cast<const QCheckBox*>(editor) == nullptr
           && qobject_cast<const QComboBox*>(editor) == nullptr
           && qobject_cast<const QGroupBox*>(editor) == nullptr;
}

void preserve_control_text_width(QWidget* widget) {
    if (auto* button = qobject_cast<QPushButton*>(widget)) {
        button->setMinimumWidth(button->sizeHint().width());
    } else if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        combo->setMinimumWidth(combo->sizeHint().width());
    }
}

void update_wrapped_label_height(QLabel* label) {
    if (label == nullptr || !label->wordWrap()) return;
    // The two-argument QSizePolicy constructor clears height-for-width. That
    // made the very next hasHeightForWidth() test false and left long help
    // labels one line tall inside otherwise correctly resizable scroll pages.
    // Preserve the wrapping contract explicitly and let the label contribute
    // its natural width to a horizontally scrolling fallback at extreme zoom.
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Preferred);
    policy.setVerticalPolicy(QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    const int width = label->contentsRect().width();
    if (width <= 0 || !label->hasHeightForWidth()) return;
    const int required = label->heightForWidth(width);
    if (required > 0 && label->minimumHeight() != required) {
        label->setMinimumHeight(required);
        label->updateGeometry();
        if (QWidget* parent = label->parentWidget()) parent->updateGeometry();
    }
}

void configure_readable_layouts(QWidget* root) {
    for (QFormLayout* form : root->findChildren<QFormLayout*>()) {
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(7);
    }
    for (QScrollArea* scroll : root->findChildren<QScrollArea*>()) {
        scroll->setFrameShape(QFrame::NoFrame);
    }
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        preserve_control_text_width(button);
    }
    for (QComboBox* combo : root->findChildren<QComboBox*>()) {
        preserve_control_text_width(combo);
    }
    for (QLabel* label : root->findChildren<QLabel*>()) {
        update_wrapped_label_height(label);
    }
}

void make_checkable_group_collapsible(QGroupBox* group) {
    if (group == nullptr || !group->isCheckable()) return;
    const auto set_expanded = [group](bool expanded) {
        // QGroupBox already owns the enable/disable semantics. Hiding only its
        // direct content widgets also collapses nested panels while leaving
        // the title checkbox visible as a compact, discoverable control.
        for (QWidget* child : group->findChildren<QWidget*>(
                 QString{}, Qt::FindDirectChildrenOnly)) {
            child->setVisible(expanded);
        }
        group->updateGeometry();
    };
    QObject::connect(group, &QGroupBox::toggled, group, set_expanded);
    set_expanded(group->isChecked());
}

pvt::ProjectDocument built_in_workbench_project_document() {
    // Keep the installed application's blank canvas aligned with the supplied
    // Untitled.zip concept without changing the richer public API defaults
    // used by existing library clients and tests.
    pvt::ProjectDocument document = pvt::default_project_document();
    auto& project = document.project;
    project.name = "Untitled";
    project.canvas.width = 1920;
    project.canvas.height = 1080;
    project.canvas.block_size = 1;
    project.canvas.total_frames = 300;
    project.canvas.fps = 60.0;
    project.canvas.clock = {};
    project.canvas.motion_paths.clear();
    project.canvas.audio_reactive_defaults = {};
    project.output = {};
    project.output.write_alpha = true;

    auto& layer = project.layers.front();
    layer.name = "Layer 1";
    layer.enabled = true;
    layer.opacity = 1.0;
    layer.blend_mode = pvt::BlendMode::Normal;
    layer.alpha_mode = pvt::AlphaMode::AlphaOver;
    auto& render = layer.render;
    render.waves.clear();
    render.swings.clear();
    auto swing = pvt::default_swing(0U);
    swing.id = 4U;
    render.swings.push_back(std::move(swing));
    render.swings_enabled = false;
    render.effects.clear();
    auto zoom = pvt::default_effect(pvt::EffectType::EndlessZoom);
    zoom.id = 5U;
    zoom.enabled = false;
    render.effects.push_back(std::move(zoom));
    render.layer_clock = {};
    render.audio_reactive = {};
    render.audio_reactive_override_enabled = false;
    render.phrase_warp = 0.0;
    render.ghost_mix = 0.0;
    render.ghost_lag_degrees = 0.0;
    render.displacement_enabled = false;
    render.lighting_enabled = false;
    render.spiral_enabled = false;
    render.wall_reflection_enabled = false;
    render.hue_cycles = 1;
    render.saturation = 1.0;
    render.starting_image = {};
    render.palette = {};
    render.surface = {};
    render.transform = {};
    render.motion = {};
    render.quantization = {};
    render.alpha = {};
    render.alpha.minimum = 0.0;
    render.alpha.maximum = 1.0;
    render.alpha.spatial_frequency = 1.99;
    render.alpha.cycles_per_loop = 6;
    render.alpha.use_source_alpha = true;
    render.starting_colors = {};
    document.dirty = false;
    return document;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), project_(pvt::default_project()),
      startup_working_directory_(stable_startup_directory()),
      last_dialog_directory_(existing_writable_directory(QDir::homePath(), true)) {
    if (last_dialog_directory_.isEmpty()) {
        last_dialog_directory_ = startup_working_directory_;
    }
    // Keep every relative resource path (output, setup references, and custom
    // meshes) anchored to the launch location even when a desktop launcher
    // originally assigned the process an unusable root working directory.
    (void)QDir::setCurrent(startup_working_directory_);
    document_ = makeNewProjectDocument(&custom_defaults_load_warning_);
    if (document_ == nullptr || document_->project.layers.empty()) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            built_in_workbench_project_document());
    }
    project_ = document_->project;
    document_->dirty = false;
    active_layer_uuid_ = project_.layers.front().uuid;
    loadActiveConfiguration();

    undo_stack_ = new QUndoStack(this);
    const int undo_limit = std::clamp(
        QSettings().value(QStringLiteral("preferences/undoLimit"), kDefaultUndoLimit).toInt(),
        kMinimumUndoLimit, kMaximumUndoLimit);
    undo_stack_->setUndoLimit(undo_limit);

    resize(1460, 900);

    preview_timer_ = new QTimer(this);
    preview_timer_->setSingleShot(true);
    preview_timer_->setInterval(70);
    playback_timer_ = new QTimer(this);
    playback_timer_->setTimerType(Qt::PreciseTimer);
    audio_playback_ = std::make_unique<pvt::audio::AudioPlayback>();
    preview_watcher_ = new QFutureWatcher<PreviewResult>(this);
    export_watcher_ = new QFutureWatcher<ExportResult>(this);
    music_analysis_watcher_ = new QFutureWatcher<MusicAnalysisResult>(this);
    project_io_watcher_ = new QFutureWatcher<ProjectIoResult>(this);
    version_diff_watcher_ = new QFutureWatcher<VersionDiffResult>(this);
    preview_cancel_ = std::make_shared<std::atomic_bool>(false);
    music_analysis_cancel_ = std::make_shared<std::atomic_bool>(false);

    editor_workspace_ = new QWidget;
    editor_workspace_->setObjectName(QStringLiteral("flowWorkbench"));
    auto* outer = new QVBoxLayout(editor_workspace_);
    auto* splitter = new QSplitter(Qt::Horizontal);
    preview_ = new PreviewWidget;
    wave_page_ = createWavePage();
    synchronization_page_ = createSynchronizationPage();
    effect_page_ = createEffectPage();
    source_page_ = createLayerSettingsPage();
    project_canvas_page_ = createOutputPage();
    history_page_ = createVersionsPage();
    project_sync_page_ = new QWidget;
    project_sync_page_->setObjectName(QStringLiteral("projectSynchronizationPage"));
    auto* project_sync_layout = new QVBoxLayout(project_sync_page_);
    auto* project_sync_help = new QLabel(tr(
        "Project Sync & Audio is edited in the persistent Drivers panel above. Expand it to configure the project clock and inherited audio profile alongside the active layer override."));
    project_sync_help->setWordWrap(true);
    project_sync_help->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    project_sync_layout->addWidget(project_sync_help);
    project_sync_layout->addStretch();

    // QTabWidget remains the stable page host used by the existing editor,
    // save, undo, and asynchronous history code. Its tab bar is deliberately
    // hidden: the layer workflow and project controls below are the only
    // user-facing navigation, so project history cannot masquerade as a
    // random layer-render stage.
    tabs_ = new QTabWidget;
    tabs_->setObjectName(QStringLiteral("workflowPageHost"));
    tabs_->setDocumentMode(true);
    tabs_->tabBar()->hide();
    tabs_->setAccessibleName(tr("Project settings and active-layer category editor"));
    tabs_->addTab(source_page_, tr("Starting Colors"));
    tabs_->addTab(effect_page_, tr("Layer Effects"));
    tabs_->addTab(surface_page_, tr("Modifiers"));
    tabs_->addTab(motion_page_, tr("Movement"));
    tabs_->addTab(finish_page_, tr("Post Effects"));
    tabs_->addTab(project_canvas_page_, tr("Canvas & Loop"));
    tabs_->addTab(project_sync_page_, tr("Project Sync & Audio"));
    tabs_->addTab(project_export_page_, tr("Export"));
    tabs_->addTab(history_page_, tr("History"));
    tabs_->setMinimumWidth(440);

    auto* inspector = new QWidget;
    inspector->setObjectName(QStringLiteral("flowWorkbenchInspector"));
    auto* inspector_layout = new QVBoxLayout(inspector);
    inspector_layout->setContentsMargins(0, 0, 0, 0);
    active_context_label_ = new QLabel;
    active_context_label_->setObjectName(QStringLiteral("activeWorkflowContext"));
    active_context_label_->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    active_context_label_->setWordWrap(true);
    active_context_label_->setAccessibleName(tr("Current workflow context"));
    inspector_layout->addWidget(active_context_label_);
    // The stage route and drivers describe the whole preview-to-inspector
    // workbench, so they span both panes like the selected reference instead
    // of being squeezed into the property inspector.
    outer->addWidget(createWorkflowNavigator());

    drivers_group_ = new QGroupBox(tr("Synchronization"));
    drivers_group_->setObjectName(QStringLiteral("driversStrip"));
    drivers_group_->setAccessibleName(tr("Project and active-layer synchronization"));
    drivers_group_->setFlat(true);
    auto* drivers_layout = new QVBoxLayout(drivers_group_);
    drivers_layout->setContentsMargins(8, 4, 8, 6);
    auto* drivers_row = new QHBoxLayout;
    driver_project_summary_ = new QLabel;
    driver_layer_summary_ = new QLabel;
    driver_swing_summary_ = new QLabel;
    driver_audio_summary_ = new QLabel;
    for (QLabel* summary : {driver_project_summary_, driver_layer_summary_,
                            driver_swing_summary_, driver_audio_summary_}) {
        summary->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        summary->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        summary->setMinimumHeight(26);
        summary->setWordWrap(false);
        drivers_row->addWidget(summary);
    }
    drivers_row->addStretch(1);
    drivers_expand_button_ = new QPushButton;
    drivers_expand_button_->setObjectName(QStringLiteral("editDriversButton"));
    drivers_expand_button_->setMinimumHeight(26);
    drivers_expand_button_->setAccessibleName(tr("Show or hide driver controls"));
    drivers_expand_button_->setToolTip(
        tr("Open project and active-layer clock, swing, rhythm, and audio-response controls."));
    drivers_row->addWidget(drivers_expand_button_);
    drivers_layout->addLayout(drivers_row);
    synchronization_page_->setObjectName(QStringLiteral("driverDetails"));
    synchronization_page_->setMinimumHeight(220);
    synchronization_page_->setMaximumHeight(380);
    drivers_layout->addWidget(synchronization_page_);
    outer->addWidget(drivers_group_);

    workflow_prerequisite_ = new QLabel;
    workflow_prerequisite_->setObjectName(QStringLiteral("workflowPrerequisite"));
    workflow_prerequisite_->setWordWrap(true);
    workflow_prerequisite_->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    workflow_prerequisite_->setAccessibleName(tr("Workflow status and prerequisites"));
    inspector_layout->addWidget(workflow_prerequisite_);
    inspector_layout->addWidget(tabs_, 1);

    splitter->addWidget(preview_);
    splitter->addWidget(inspector);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({720, 520});
    outer->addWidget(splitter, 1);
    outer->addWidget(createTimeline());
    workspace_stack_ = new QStackedWidget;
    workspace_stack_->setObjectName(QStringLiteral("applicationModeHost"));
    live_workspace_ = new LiveWorkspace(
        [this] { return project_; },
        [this] { return previewProjectSnapshot(); },
        [this] { return timeline_ == nullptr ? 0 : timeline_->value(); },
        [this] { return frameRenderOptions(); },
        [this] { return document_revision_; },
        [this] { return active_layer_uuid_; },
        [this](const pvt::LiveConfig& live, const QString& reason) {
            applyAuthoredLiveConfig(live, reason);
        });
    live_workspace_->setObjectName(QStringLiteral("livePerformanceWorkspace"));
    connect(live_workspace_, &LiveWorkspace::requestEditMode,
            this, [this] { setLiveMode(false); });
    connect(live_workspace_, &LiveWorkspace::livePreviewFrame,
            this, [this](const QImage& image) {
                if (preview_ != nullptr && live_workspace_ != nullptr
                    && live_workspace_->isRealtimeOutputActive()
                    && (workspace_stack_ == nullptr
                        || workspace_stack_->currentWidget() != live_workspace_)) {
                    preview_->setPreview(image);
                }
            });
    connect(live_workspace_, &LiveWorkspace::runtimeStatusChanged,
            this, [this](const QString& summary) {
                if (status_ != nullptr && live_workspace_ != nullptr
                    && live_workspace_->isRealtimeOutputActive()) {
                    status_->setText(summary);
                }
            });
    connect(live_workspace_, &LiveWorkspace::runtimeOutputSettingsChanged,
            this, &MainWindow::refreshLivePreviewOutputControls);
    connect(live_workspace_, &LiveWorkspace::audioInputsChanged,
            this, &MainWindow::refreshStandardMicControls);
    connect(live_workspace_, &LiveWorkspace::presentationActiveChanged,
            this, [this](bool active) {
                if (live_preview_output_action_ != nullptr) {
                    const QSignalBlocker blocker(live_preview_output_action_);
                    live_preview_output_action_->setChecked(active);
                }
                if (live_preview_output_button_ != nullptr) {
                    live_preview_output_button_->setText(
                        active ? tr("Stop Live Preview Output")
                               : tr("Start Live Preview Output"));
                }
                if (live_preview_output_status_ != nullptr) {
                    live_preview_output_status_->setText(
                        active
                            ? tr("Streaming the editor preview; performance inputs remain off.")
                            : tr("Stopped — start this to present the editor preview without entering LIVE."));
                }
                updateExportAvailability();
                if (!active) schedulePreview();
            });
    refreshLivePreviewOutputControls();
    refreshStandardMicControls();
    workspace_stack_->addWidget(editor_workspace_);
    workspace_stack_->addWidget(live_workspace_);
    workspace_stack_->setCurrentWidget(editor_workspace_);
    setCentralWidget(workspace_stack_);
    createLayerDock();

    status_ = new QLabel(tr("Ready"));
    statusBar()->addPermanentWidget(status_, 1);
    export_progress_ = new QProgressBar;
    export_progress_->setRange(0, 1000);
    export_progress_->setValue(0);
    export_progress_->setMaximumWidth(220);
    export_progress_->hide();
    statusBar()->addPermanentWidget(export_progress_);
    project_io_progress_ = new QProgressBar;
    project_io_progress_->setRange(0, 0);
    project_io_progress_->setMaximumWidth(180);
    project_io_progress_->setAccessibleName(tr("Project file operation in progress"));
    project_io_progress_->hide();
    statusBar()->addPermanentWidget(project_io_progress_);
    createToolbar();

    connect(preview_timer_, &QTimer::timeout, this, &MainWindow::startPreview);
    connect(playback_timer_, &QTimer::timeout, this, [this] {
        if (audio_playback_ != nullptr && audio_playback_->is_playing()) {
            const double position = audio_playback_->position_seconds();
            const int raw_frame = static_cast<int>(
                std::floor(position * config_.fps));
            if (raw_frame > timeline_->maximum()) {
                timeline_->setValue(0);
                startProjectAudioPlayback();
                return;
            }
            const int synchronized_frame = std::clamp(raw_frame,
                timeline_->minimum(), timeline_->maximum());
            timeline_->setValue(synchronized_frame);
            return;
        }
        int next = timeline_->value() + 1;
        if (next > timeline_->maximum()) {
            next = 0;
            timeline_->setValue(next);
            startProjectAudioPlayback();
            return;
        }
        timeline_->setValue(next);
    });
    connect(preview_watcher_, &QFutureWatcher<PreviewResult>::finished, this, [this] {
        PreviewResult result;
        try {
            result = preview_watcher_->result();
        } catch (const std::exception& exception) {
            result.error = tr("Unexpected preview error: %1")
                               .arg(QString::fromUtf8(exception.what()));
            result.generation = preview_task_generation_;
            result.document_revision = preview_task_document_revision_;
        } catch (...) {
            result.error = tr("Preview failed because of an unexpected error.");
            result.generation = preview_task_generation_;
            result.document_revision = preview_task_document_revision_;
        }
        if (result.document_revision == document_revision_
            && (result.generation == preview_generation_ || playback_timer_->isActive())) {
            if (result.error.isEmpty()) {
                if (playback_timer_->isActive() && last_previewed_frame_ >= 0
                    && result.frame != last_previewed_frame_) {
                    playback_preview_advanced_ = true;
                }
                last_previewed_frame_ = result.frame;
                const bool realtime_output = live_workspace_ != nullptr
                    && live_workspace_->isRealtimeOutputActive();
                if (!realtime_output) {
                    preview_->setPreview(result.image);
                    const int frame_count = std::max(1, effectiveFrameCount());
                    status_->setText(tr("Preview frame %1/%2")
                                         .arg(result.frame + 1)
                                         .arg(frame_count));
                }
            } else {
                if (live_workspace_ == nullptr
                    || !live_workspace_->isRealtimeOutputActive()) {
                    status_->setText(result.error);
                }
            }
        }
        if (preview_deferred_) {
            preview_deferred_ = false;
            if (playback_timer_->isActive()) {
                QTimer::singleShot(0, this, &MainWindow::startPreview);
            } else {
                preview_timer_->start();
            }
        }
    });
    connect(export_watcher_, &QFutureWatcher<ExportResult>::finished, this, [this] {
        ExportResult result;
        try {
            result = export_watcher_->result();
        } catch (const std::exception& exception) {
            result.error = tr("Unexpected export error: %1")
                               .arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            result.error = tr("Export failed because of an unexpected error.");
        }
        const bool automated_smoke = QCoreApplication::arguments().contains(
            QStringLiteral("--smoke-test"));
        // Clear the active guard before recomputing action availability. An
        // earlier split completion handler refreshed the actions first, saw an
        // active export, and left both export commands permanently disabled.
        finishExportUiState();
        if (close_after_export_) {
            close_after_export_ = false;
            QTimer::singleShot(0, this, &QWidget::close);
            return;
        }
        if (result.ok) {
            status_->setText(tr("Export complete"));
            if (!automated_smoke) {
                QMessageBox::information(this, tr("Export complete"),
                                         result.success_message.isEmpty()
                                             ? tr("The export completed successfully.")
                                             : result.success_message);
            }
        } else if (result.cancelled) {
            status_->setText(tr("Export cancelled"));
        } else {
            status_->setText(tr("Export failed"));
            if (!automated_smoke) {
                QMessageBox::critical(this, tr("Export failed"), result.error);
            }
        }
    });
    connect(music_analysis_watcher_,
            &QFutureWatcher<MusicAnalysisResult>::finished, this, [this] {
                MusicAnalysisResult result;
                try {
                    result = music_analysis_watcher_->result();
                } catch (const std::exception& exception) {
                    result.error = tr("Unexpected music-analysis error: %1")
                                       .arg(QString::fromUtf8(exception.what()));
                    result.layer_clock = music_analysis_layer_clock_;
                    result.layer_uuid = music_analysis_task_layer_uuid_;
                    result.generation = music_analysis_task_generation_;
                    result.document_revision =
                        music_analysis_task_document_revision_;
                    result.cancelled = music_analysis_cancel_ != nullptr
                        && music_analysis_cancel_->load(std::memory_order_relaxed);
                } catch (...) {
                    result.error = tr(
                        "Music analysis failed because of an unexpected error.");
                    result.layer_clock = music_analysis_layer_clock_;
                    result.layer_uuid = music_analysis_task_layer_uuid_;
                    result.generation = music_analysis_task_generation_;
                    result.document_revision =
                        music_analysis_task_document_revision_;
                    result.cancelled = music_analysis_cancel_ != nullptr
                        && music_analysis_cancel_->load(std::memory_order_relaxed);
                }
                music_analysis_active_ = false;
                finishMusicAnalysis(result);
                updateMusicTransactionGuards();
                updateSynchronizationState();
                updateExportAvailability();
                if (playback_timer_ != nullptr
                    && playback_timer_->isActive()) {
                    // Analysis temporarily stops the device. Resume the
                    // retained or newly committed global source from the
                    // current visual timeline, including after a failed or
                    // cancelled transaction.
                    startProjectAudioPlayback();
                }
            });
    connect(project_io_watcher_, &QFutureWatcher<ProjectIoResult>::finished,
            this, [this] {
                ProjectIoResult result;
                try {
                    result = project_io_watcher_->result();
                } catch (const std::exception& exception) {
                    result.operation = project_io_operation_;
                    result.path = project_io_path_;
                    result.error = tr("Unexpected project-file error: %1")
                                       .arg(QString::fromUtf8(exception.what()));
                } catch (...) {
                    result.operation = project_io_operation_;
                    result.path = project_io_path_;
                    result.error = tr(
                        "The project file operation failed because of an unexpected error.");
                }
                bool adopted = false;
                if (result.ok && result.document != nullptr) {
                    if (result.operation == ProjectIoOperation::Load) {
                        QString adoption_error;
                        adopted = adoptLoadedProject(
                            std::move(*result.document), &adoption_error);
                        if (!adopted) result.error = adoption_error;
                    } else {
                        finishProjectSave(std::move(*result.document),
                                          result.save_report, result.path);
                        adopted = true;
                    }
                }
                setProjectIoActive(false);
                if (adopted && result.operation == ProjectIoOperation::Load
                    && compatibility_warning_.isEmpty()) {
                    status_->setText(tr("Loaded %1").arg(result.path));
                } else if (!adopted) {
                    status_->setText(result.operation == ProjectIoOperation::Load
                                         ? tr("Load failed")
                                         : tr("Save failed"));
                    QMessageBox::critical(
                        this,
                        result.operation == ProjectIoOperation::Load
                            ? tr("Load failed") : tr("Save failed"),
                        result.operation == ProjectIoOperation::Load
                            ? tr("The active project was not changed.\n\n%1")
                                  .arg(result.error)
                            : result.error);
                }
                if (adopted && result.operation == ProjectIoOperation::Save) {
                    addRecentProject(result.path);
                }
                if (close_after_project_io_) {
                    close_after_project_io_ = false;
                    if (adopted) QTimer::singleShot(0, this, &QWidget::close);
                }
                std::function<void()> continuation =
                    std::move(project_io_success_continuation_);
                project_io_success_continuation_ = {};
                if (adopted && result.operation == ProjectIoOperation::Save
                    && continuation) {
                    continuation();
                }
            });
    connect(version_diff_watcher_, &QFutureWatcher<VersionDiffResult>::finished,
            this, [this] {
                VersionDiffResult result;
                try {
                    result = version_diff_watcher_->result();
                } catch (const std::exception& exception) {
                    result.error = tr("Unexpected version-comparison error: %1")
                                       .arg(QString::fromUtf8(exception.what()));
                    result.before = version_diff_task_before_;
                    result.after = version_diff_task_after_;
                    result.document_revision =
                        version_diff_task_document_revision_;
                } catch (...) {
                    result.error = tr(
                        "Version comparison failed because of an unexpected error.");
                    result.before = version_diff_task_before_;
                    result.after = version_diff_task_after_;
                    result.document_revision =
                        version_diff_task_document_revision_;
                }
                if (version_compare_ != nullptr) {
                    // The document may have been replaced while the snapshot
                    // comparison was running. Do not let completion from the
                    // old document re-enable comparison for an unsaved project.
                    version_compare_->setEnabled(
                        document_ != nullptr && version_before_ != nullptr
                        && version_after_ != nullptr
                        && version_before_->count() >= 2
                        && version_after_->count() >= 2);
                }
                if (version_diff_ == nullptr || document_ == nullptr
                    || result.document_revision != document_revision_
                    || version_before_->currentData().toULongLong() != result.before
                    || version_after_->currentData().toULongLong() != result.after) {
                    return;
                }
                if (!result.ok) {
                    version_diff_->setPlainText(
                        tr("Could not compare versions: %1").arg(result.error));
                    return;
                }
                if (result.differences.empty()) {
                    version_diff_->setPlainText(tr("No semantic project differences."));
                    return;
                }
                QStringList lines;
                lines.reserve(static_cast<qsizetype>(result.differences.size()));
                for (const auto& difference : result.differences) {
                    lines.push_back(tr("%1\n  %2\n→ %3")
                                        .arg(QString::fromStdString(difference.field),
                                             friendly_diff_value(difference.before),
                                             friendly_diff_value(difference.after)));
                }
                version_diff_->setPlainText(lines.join(QStringLiteral("\n\n")));
            });

    connectEditors();
    connect(drivers_expand_button_, &QPushButton::clicked, this, [this] {
        setDriversExpanded(synchronization_page_->isHidden());
    });
    connect(undo_stack_, &QUndoStack::cleanChanged, this,
            [this] { updateWindowTitle(); });
    refreshLayerList();
    refreshAll();
    undo_stack_->setClean();
    updateWindowTitle();
    updateCompatibilityWarning();
    restoreUserSettings();
    if (QCoreApplication::arguments().contains(QStringLiteral("--smoke-test"))) {
        // CI package validation must not enter a display-driver capability
        // probe or start a GPU preview. Headless OpenGL context creation can
        // block inside a vendor driver and cannot be bounded by Qt. The normal
        // application path still probes and reports the real accelerator.
        render_backend_ = pvt::RenderBackend::Cpu;
    }
    setDriversExpanded(false);
    setWorkflowStage(1);
    if (!custom_defaults_load_warning_.isEmpty()) {
        status_->setText(custom_defaults_load_warning_);
    }
    configure_readable_layouts(this);
    qApp->installEventFilter(this);
    QTimer::singleShot(0, this, [this] { configure_readable_layouts(this); });
    schedulePreview();
}

MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    if (live_workspace_ != nullptr
        && live_workspace_->isPresentationActive()) {
        live_workspace_->setPresentationActive(false);
    }
    restoreLiveWorkspace(false);
    if (audio_playback_ != nullptr) audio_playback_->stop();
    if (preview_cancel_ != nullptr) {
        preview_cancel_->store(true, std::memory_order_relaxed);
    }
    if (music_analysis_cancel_ != nullptr) {
        music_analysis_cancel_->store(true, std::memory_order_relaxed);
    }
    cancel_export_.store(true);
    preview_watcher_->waitForFinished();
    music_analysis_watcher_->waitForFinished();
    export_watcher_->waitForFinished();
    project_io_watcher_->waitForFinished();
}

QWidget* MainWindow::createWorkflowNavigator() {
    auto* navigator = new QWidget;
    navigator->setObjectName(QStringLiteral("workflowStageNavigator"));
    navigator->setAccessibleName(tr("Project and layer workspace categories"));
    auto* layout = new QHBoxLayout(navigator);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    const QStringList labels = {
        tr("Project"), tr("Starting Colors"), tr("Modifiers"),
        tr("Movement"), tr("Layer Effects"), tr("Post Effects"),
        tr("Export")};
    const QStringList descriptions = {
        tr("Edit project-wide canvas, loop, synchronization, audio, and history settings."),
        tr("Choose the image, generated colors, or starting palette that establishes the layer's pixels."),
        tr("Shape the active layer with procedural features, transforms, alpha, and optional surface mapping."),
        tr("Build motion with waves, seamless layer movement, and reusable paths."),
        tr("Edit effects in separate movement, light, stylize, particle, and blur categories."),
        tr("Apply final layer-local processing such as post-effects color quantization."),
        tr("Set output size, timing, encoding, destination, and start an export.")};
    workflow_stage_buttons_.clear();
    workflow_stage_buttons_.reserve(static_cast<std::size_t>(labels.size()));
    for (int index = 0; index < labels.size(); ++index) {
        QString button_label = labels.at(index);
        button_label.replace(QLatin1Char('&'), QStringLiteral("&&"));
        auto* button = new QPushButton(button_label);
        button->setObjectName(
            QStringLiteral("workflowStage%1").arg(index));
        button->setCheckable(true);
        button->setStyleSheet(QStringLiteral(
            "QPushButton:checked { background-color: palette(highlight); "
            "color: palette(highlighted-text); font-weight: 600; "
            "border: 1px solid palette(highlight); border-radius: 4px; "
            "padding: 4px 8px; }"));
        button->setToolTip(descriptions.at(index));
        button->setAccessibleName(
            tr("Category %1 of %2: %3")
                .arg(index + 1)
                .arg(labels.size())
                .arg(labels.at(index)));
        button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        connect(button, &QPushButton::clicked, this,
                [this, index] { setWorkflowStage(index); });
        workflow_stage_buttons_.push_back(button);
        layout->addWidget(button, index == 1 || index == 4 ? 2 : 1);
    }
    return navigator;
}

void MainWindow::setWorkflowStage(int stage) {
    if (tabs_ == nullptr || stage < 0 || stage >= 7) return;
    workflow_stage_index_ = stage;
    workflow_project_context_ = stage == 0 ? tr("Canvas & Loop")
                              : stage == 6 ? tr("Export") : QString{};
    for (std::size_t index = 0; index < workflow_stage_buttons_.size(); ++index) {
        const QSignalBlocker blocker(workflow_stage_buttons_[index]);
        workflow_stage_buttons_[index]->setChecked(
            index == static_cast<std::size_t>(stage));
    }

    QWidget* page = source_page_;
    switch (stage) {
        case 0: page = project_canvas_page_; break;
        case 1: page = source_page_; break;
        case 2: page = surface_page_; break;
        case 3: page = motion_page_; break;
        case 4: page = effect_page_; break;
        case 5: page = finish_page_; break;
        case 6: page = project_export_page_; break;
        default: return;
    }
    if (page != nullptr) tabs_->setCurrentWidget(page);

    if (preview_ != nullptr) {
        if (synchronization_page_ != nullptr
            && !synchronization_page_->isHidden()) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Swings);
        } else if (stage == 4) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Effects);
        } else {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Waves);
        }
    }
    updateWorkflowSummaries();
}

void MainWindow::setEffectCategory(int category) {
    category = std::clamp(category, 0, EffectUiCategoryCount - 1);
    effect_category_filter_ = category;
    if (effect_category_tabs_ != nullptr
        && effect_category_tabs_->currentIndex() != category) {
        const QSignalBlocker blocker(effect_category_tabs_);
        effect_category_tabs_->setCurrentIndex(category);
    }
    const bool was_populating = populating_;
    populating_ = true;
    if (add_effect_type_ != nullptr) {
        populate_effect_types(add_effect_type_, category);
    }
    if (effect_type_ != nullptr) {
        populate_effect_types(effect_type_, category);
    }
    populating_ = was_populating;
    if (effect_list_ != nullptr) refreshEffectList();
}

void MainWindow::showProjectSettingsPage(QWidget* page, const QString& title) {
    if (tabs_ == nullptr || page == nullptr) return;
    workflow_project_context_ = title;
    workflow_stage_index_ = page == project_export_page_ ? 6 : 0;
    for (std::size_t index = 0; index < workflow_stage_buttons_.size(); ++index) {
        const QSignalBlocker blocker(workflow_stage_buttons_[index]);
        workflow_stage_buttons_[index]->setChecked(
            index == static_cast<std::size_t>(workflow_stage_index_));
    }
    tabs_->setCurrentWidget(page);
    updateWorkflowSummaries();
}

void MainWindow::setDriversExpanded(bool expanded) {
    if (synchronization_page_ == nullptr || drivers_expand_button_ == nullptr) {
        return;
    }
    synchronization_page_->setVisible(expanded);
    drivers_expand_button_->setText(expanded ? tr("Collapse")
                                             : tr("Configure…"));
    drivers_group_->setMaximumHeight(expanded ? 500 : 78);
    drivers_expand_button_->setAccessibleDescription(
        expanded ? tr("Driver controls are expanded")
                 : tr("Driver controls are collapsed"));
    if (preview_ != nullptr) {
        if (expanded) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Swings);
        } else if (workflow_stage_index_ == 4) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Effects);
        } else {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Waves);
        }
    }
    updateWorkflowSummaries();
}

void MainWindow::updateWorkflowSummaries() {
    const auto stage_name = [this]() -> QString {
        if (workflow_stage_index_ >= 0
            && workflow_stage_index_
                   < static_cast<int>(workflow_stage_buttons_.size())) {
            QString label = workflow_stage_buttons_[
                static_cast<std::size_t>(workflow_stage_index_)]->text();
            label.replace(QStringLiteral("&&"), QStringLiteral("&"));
            return label;
        }
        return tr("Starting Colors");
    };
    const pvt::LayerConfig* layer = activeLayer();
    const pvt::LayerGroup* owning_group =
        layer != nullptr ? groupForLayer(*layer) : nullptr;
    const bool locked = owning_group != nullptr && owning_group->locked;

    if (active_context_label_ != nullptr) {
        if (!workflow_project_context_.isEmpty()) {
            active_context_label_->setText(
                tr("Project Settings  ›  %1").arg(workflow_project_context_));
        } else if (layer != nullptr) {
            active_context_label_->setText(
                tr("Active Layer  ›  %1  ›  %2")
                    .arg(QString::fromStdString(layer->name), stage_name()));
        } else {
            active_context_label_->setText(tr("No active layer"));
        }
    }

    const auto effect_count = [this](int category) {
        return static_cast<qulonglong>(std::count_if(
            config_.effects.cbegin(), config_.effects.cend(),
            [category](const pvt::EffectConfig& effect) {
                return effect_ui_category(effect.type) == category;
            }));
    };
    if (workflow_stage_buttons_.size() == 7U) {
        workflow_stage_buttons_[0]->setStatusTip(
            tr("%1 × %2 at %3 FPS")
                .arg(config_.width).arg(config_.height).arg(config_.fps));
        workflow_stage_buttons_[1]->setStatusTip(
            tr("%1 waves; %2 starting-palette colors")
                .arg(static_cast<qulonglong>(config_.waves.size()))
                .arg(static_cast<qulonglong>(config_.palette.colors.size())));
        workflow_stage_buttons_[2]->setStatusTip(
            config_.surface.enabled ? tr("Modifiers · optional surface mapping enabled")
                                    : tr("Modifiers · surface mapping off"));
        workflow_stage_buttons_[3]->setStatusTip(
            tr("%1 waves · whole-layer motion %2")
                .arg(static_cast<qulonglong>(config_.waves.size()))
                .arg(config_.motion.enabled ? tr("enabled") : tr("off")));
        workflow_stage_buttons_[4]->setStatusTip(
            tr("%1 categorized layer effects")
                .arg(static_cast<qulonglong>(config_.effects.size())));
        QStringList active_post_effects;
        if (config_.post_process.invert_rgb_enabled) {
            active_post_effects.append(tr("color invert"));
        }
        if (config_.post_process.invert_alpha_enabled) {
            active_post_effects.append(tr("alpha invert"));
        }
        if (config_.post_process.antialias_enabled) {
            active_post_effects.append(tr("edge antialiasing"));
        }
        if (config_.quantization.enabled) {
            active_post_effects.append(tr("quantization"));
        }
        workflow_stage_buttons_[5]->setStatusTip(
            active_post_effects.isEmpty()
                ? tr("Post effects bypassed")
                : tr("Active: %1").arg(
                      active_post_effects.join(QStringLiteral(" · "))));
        workflow_stage_buttons_[6]->setStatusTip(
            tr("%1-bit output · %2")
                .arg(project_.output.bit_depth)
                .arg(QString::fromStdString(project_.output.output_directory)));
    }

    if (driver_project_summary_ != nullptr) {
        const bool mic = standardMicRoute(false) != nullptr;
        driver_project_summary_->setText(mic
            ? tr("Project: Mic (Live) · offline %1")
                  .arg(QString::fromUtf8(
                      pvt::clock_mode_name(config_.clock.mode)))
            : tr("Project: %1")
                  .arg(QString::fromUtf8(
                      pvt::clock_mode_name(config_.clock.mode))));
        driver_project_summary_->setToolTip(
            tr("The project-wide clock is always the base timeline for synchronized items."));
    }
    if (driver_layer_summary_ != nullptr) {
        const bool mic = standardMicRoute(true) != nullptr;
        driver_layer_summary_->setText(
            mic ? tr("Layer: Mic (Live) · offline %1")
                      .arg(config_.layer_clock.enabled
                          ? QString::fromUtf8(pvt::clock_mode_name(
                                config_.layer_clock.clock.mode))
                          : tr("Off"))
            : config_.layer_clock.enabled
                ? (config_.layer_clock.mix_enabled
                       ? tr("Layer: %1 · %2")
                             .arg(QString::fromUtf8(pvt::clock_mode_name(
                                      config_.layer_clock.clock.mode)),
                                  QString::fromUtf8(
                                      pvt::layer_clock_mix_mode_name(
                                          config_.layer_clock.mix)))
                       : tr("Layer: %1 · replace")
                             .arg(QString::fromUtf8(pvt::clock_mode_name(
                                 config_.layer_clock.clock.mode))))
                : tr("Layer: Off"));
        driver_layer_summary_->setToolTip(
            config_.layer_clock.enabled
                ? (config_.layer_clock.mix_enabled
                       ? tr("Clock mixing is explicitly enabled. The selected policy combines project and layer phases.")
                       : tr("Mixing is off. The active layer clock replaces the project clock, preserving the historical behavior."))
                : tr("No active-layer clock is applied; the project clock remains unchanged."));
    }
    if (driver_swing_summary_ != nullptr) {
        const auto enabled_swings = static_cast<qulonglong>(std::count_if(
            config_.swings.cbegin(), config_.swings.cend(),
            [](const pvt::SwingConfig& swing) { return swing.enabled; }));
        driver_swing_summary_->setText(
            config_.swings_enabled
                ? tr("Swing: %1/%2")
                      .arg(enabled_swings)
                      .arg(static_cast<qulonglong>(config_.swings.size()))
                : tr("Swing: Off"));
    }
    if (driver_audio_summary_ != nullptr) {
        const pvt::AudioReactiveConfig& effective_audio =
            config_.audio_reactive_override_enabled
                ? config_.audio_reactive : config_.audio_reactive_defaults;
        driver_audio_summary_->setText(
            effective_audio.enabled
                ? (config_.audio_reactive_override_enabled
                       ? tr("Audio: Layer")
                       : tr("Audio: Project"))
                : tr("Audio: Off"));
    }
    if (effect_stage_summary_ != nullptr) {
        effect_stage_summary_->setText(
            tr("%1 · %2 effects · new effects start on Texture")
                .arg(effect_ui_category_name(effect_category_filter_))
                .arg(effect_count(effect_category_filter_)));
    }
    if (export_canvas_summary_ != nullptr) {
        export_canvas_summary_->setText(
            tr("%1 × %2 · %3 FPS · %4 frames · %5-bit %6")
                .arg(config_.width).arg(config_.height).arg(config_.fps)
                .arg(effectiveFrameCount()).arg(project_.output.bit_depth)
                .arg(project_.output.bit_depth == 32 ? tr("EXR") : tr("PNG")));
    }

    if (workflow_prerequisite_ != nullptr) {
        QString message;
        if (!workflow_project_context_.isEmpty()) {
            message = tr("These settings apply to the whole project and are independent of the selected layer stage.");
        } else if (selected_group_uuid_) {
            message = tr("Select a layer row to edit its render pipeline. Group controls remain in the Project & Layers panel.");
        } else if (layer == nullptr) {
            message = tr("Add or select a layer to begin editing this pipeline.");
        } else if (locked) {
            message = tr("This layer belongs to a locked group. Unlock the group in Project & Layers to edit layer stages.");
        } else if (workflow_project_context_.isEmpty()
                   && workflow_stage_index_ == 3 && !config_.motion.enabled) {
            message = tr("Whole-layer motion is off. Waves and reusable path editing remain available.");
        } else if (workflow_project_context_.isEmpty()
                   && workflow_stage_index_ == 4
                   && effect_list_ != nullptr && effect_list_->count() == 0) {
            message = tr("No %1 effects yet. Choose a type below to add one; it will start on Texture.")
                          .arg(effect_ui_category_name(effect_category_filter_));
        } else {
            message = tr("This category contains one focused part of the active layer; use the top navigation to switch without scrolling through unrelated controls.");
        }
        workflow_prerequisite_->setText(message);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (export_watcher_ != nullptr && export_watcher_->isRunning()) {
        close_after_export_ = true;
        cancel_export_.store(true);
        status_->setText(tr("Cancelling export before closing…"));
        event->ignore();
        return;
    }
    if (project_io_watcher_ != nullptr && project_io_watcher_->isRunning()) {
        close_after_project_io_ = true;
        status_->setText(tr("Finishing the project file operation before closing…"));
        event->ignore();
        return;
    }
    if (!confirmDiscardChanges()) {
        if (project_io_active_) close_after_project_io_ = true;
        event->ignore();
        return;
    }
    cancelMusicAnalysis();
    stopPlayback();
    if (live_workspace_ != nullptr
        && live_workspace_->isPresentationActive()) {
        live_workspace_->setPresentationActive(false);
    }
    restoreLiveWorkspace(false);
    saveUserSettings();
    QMainWindow::closeEvent(event);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == live_popout_window_ && event != nullptr
        && event->type() == QEvent::Close) {
        static_cast<QCloseEvent*>(event)->ignore();
        restoreLiveWorkspace(true);
        return true;
    }
    if (event != nullptr) {
        if (event->type() == QEvent::Wheel) {
            QWidget* editor = qobject_cast<QWidget*>(watched);
            while (editor != nullptr
                   && qobject_cast<QAbstractSpinBox*>(editor) == nullptr
                   && qobject_cast<QComboBox*>(editor) == nullptr) {
                editor = editor->parentWidget();
            }
            QWidget* const focus = QApplication::focusWidget();
            const bool editor_has_focus = editor != nullptr
                && (focus == editor || editor->isAncestorOf(focus));
            if (editor != nullptr && !editor_has_focus) {
                // Precision touchpads report two-finger scrolling as wheel
                // input. Qt's numeric and combo editors otherwise consume it,
                // take focus, and change the value while the user is merely
                // moving through a long settings page. Preserve intentional
                // wheel editing for the focused control; unfocused editors
                // pass the gesture to their containing scroll viewport.
                QAbstractScrollArea* scroll_area = nullptr;
                for (QWidget* parent = editor->parentWidget();
                     parent != nullptr && scroll_area == nullptr;
                     parent = parent->parentWidget()) {
                    scroll_area = qobject_cast<QAbstractScrollArea*>(parent);
                }
                if (scroll_area != nullptr && scroll_area->viewport() != nullptr) {
                    auto* wheel = static_cast<QWheelEvent*>(event);
                    QWidget* const viewport = scroll_area->viewport();
                    const QPointF global_position = wheel->globalPosition();
                    const QPointF local_position = viewport->mapFromGlobal(
                        global_position.toPoint());
                    QWheelEvent forwarded(
                        local_position, global_position,
                        wheel->pixelDelta(), wheel->angleDelta(),
                        wheel->buttons(), wheel->modifiers(), wheel->phase(),
                        wheel->inverted(), wheel->source(),
                        wheel->pointingDevice());
                    QCoreApplication::sendEvent(viewport, &forwarded);
                }
                return true;
            }
        }
        if (auto* label = qobject_cast<QLabel*>(watched)) {
            if (event->type() == QEvent::Resize
                || event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange) {
                update_wrapped_label_height(label);
            }
        }
        if (auto* widget = qobject_cast<QWidget*>(watched)) {
            if (event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange) {
                preserve_control_text_width(widget);
                if (tabs_ != nullptr && widget == tabs_->tabBar()
                    && !tabs_->tabBar()->isHidden()) {
                    tabs_->setMinimumWidth((std::max)(
                        440, tabs_->tabBar()->sizeHint().width() + 8));
                }
            }
        }
    }
    if (event != nullptr
        && (event->type() == QEvent::KeyPress
            || event->type() == QEvent::KeyRelease)) {
        auto* key = static_cast<QKeyEvent*>(event);
        auto* target = qobject_cast<QWidget*>(watched);
        if (key->key() == Qt::Key_Space && key->modifiers() == Qt::NoModifier
            && target != nullptr && target->window() == this
            && (live_workspace_ == nullptr
                || !live_workspace_->isLiveActive())) {
            QWidget* const focus = QApplication::focusWidget();
            const bool editing_text = qobject_cast<QLineEdit*>(focus) != nullptr
                                      || qobject_cast<QPlainTextEdit*>(focus) != nullptr;
            if (!editing_text) {
                if (event->type() == QEvent::KeyPress && !key->isAutoRepeat()) {
                    togglePlayback();
                }
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QWidget* MainWindow::createWavePage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Drag numbered handles in the preview to place waves. A synchronized wave uses "
           "the effective project or per-layer clock; a free wave remains independently periodic."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* output = new QGroupBox(tr("Wave output"));
    auto* output_layout = new QVBoxLayout(output);
    wave_output_status_ = new QLabel;
    wave_output_status_->setObjectName(QStringLiteral("waveOutputStatus"));
    wave_output_status_->setWordWrap(true);
    output_layout->addWidget(wave_output_status_);
    wave_displacement_enabled_ = new QCheckBox(
        tr("Displace the generated pattern"));
    wave_displacement_enabled_->setObjectName(
        QStringLiteral("waveDisplacementEnabled"));
    wave_displacement_enabled_->setToolTip(tr(
        "Uses wave slopes to move the generated pattern. This is the same layer setting shown under Modifiers > Procedural features."));
    wave_lighting_enabled_ = new QCheckBox(tr("Apply wave-slope lighting"));
    wave_lighting_enabled_->setObjectName(
        QStringLiteral("waveLightingEnabled"));
    wave_lighting_enabled_->setToolTip(tr(
        "Uses wave slopes to shade the generated pattern. This is the same layer setting shown under Modifiers > Procedural features."));
    output_layout->addWidget(wave_displacement_enabled_);
    output_layout->addWidget(wave_lighting_enabled_);
    layout->addWidget(output);

    wave_list_ = new QListWidget;
    wave_list_->setAlternatingRowColors(true);
    layout->addWidget(wave_list_, 1);

    auto* buttons = new QGridLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    buttons->addWidget(add, 0, 0);
    buttons->addWidget(duplicate, 0, 1);
    buttons->addWidget(remove, 0, 2);
    buttons->addWidget(up, 1, 0);
    buttons->addWidget(down, 1, 1);
    buttons->setColumnStretch(2, 1);
    layout->addLayout(buttons);

    auto* properties = new QGroupBox(tr("Selected wave"));
    wave_form_ = new QFormLayout(properties);
    wave_name_ = new QLineEdit;
    wave_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    wave_name_->setValidator(new Utf8TextValidator(TextRule::Name, wave_name_));
    wave_enabled_ = new QCheckBox(tr("Enabled"));
    wave_sync_ = new QCheckBox(tr("Use synchronized clock"));
    wave_audio_response_ = new QComboBox;
    populate_audio_response_combo(wave_audio_response_);
    wave_x_ = real_editor(-kMaximumRenderParameter,
                          kMaximumRenderParameter, 3, 1.0);
    wave_y_ = real_editor(-kMaximumRenderParameter,
                          kMaximumRenderParameter, 3, 1.0);
    wave_amplitude_ = real_editor(-kMaximumRenderParameter,
                                  kMaximumRenderParameter);
    wave_frequency_ = real_editor(0.0, kMaximumRenderParameter);
    wave_cycles_ = integer_editor(kMinimumIntegerParameter,
                                  kMaximumIntegerParameter);
    wave_phase_ = real_editor(-kMaximumRenderParameter,
                              kMaximumRenderParameter, 3, 1.0);
    wave_direction_ = real_editor(0.0, 1.0, 4, 0.05);
    wave_form_->addRow(tr("Name"), wave_name_);
    wave_form_->addRow(wave_enabled_);
    wave_form_->addRow(wave_sync_);
    wave_form_->addRow(tr("Audio response"), wave_audio_response_);
    wave_form_->addRow(tr("X position (%)"), wave_x_);
    wave_form_->addRow(tr("Y position (%)"), wave_y_);
    wave_form_->addRow(tr("Amplitude"), wave_amplitude_);
    wave_form_->addRow(tr("Spatial frequency"), wave_frequency_);
    wave_form_->addRow(tr("Cycles per loop"), wave_cycles_);
    wave_form_->addRow(tr("Phase (degrees)"), wave_phase_);
    wave_form_->addRow(tr("Direction (0 horizontal, .5 radial, 1 vertical)"), wave_direction_);
    wave_name_->setToolTip(tr("A descriptive layer-local name used in lists and semantic version diffs."));
    wave_enabled_->setToolTip(tr("Bypasses this wave without deleting or resetting its authored settings."));
    wave_sync_->setToolTip(tr("Uses the layer's swung synchronized clock. Clear it for an independent seamless clock."));
    wave_audio_response_->setToolTip(
        tr("Default inherits both the effective profile's Waves switch and Wave source. While the profile master is enabled, choosing Beat, Energy, or another feature opts this wave in and overrides that source. The final two choices force this wave on with the profile source or ignore audio. Clock synchronization is independent unless the profile explicitly limits response to synchronized items. Missing/null project data is Default."));
    wave_x_->setToolTip(tr("Horizontal wave source position. Values outside 0–100% place the source beyond the canvas."));
    wave_y_->setToolTip(tr("Vertical wave source position. Values outside 0–100% place the source beyond the canvas."));
    wave_amplitude_->setToolTip(tr("Peak contribution of this wave before optional audio modulation."));
    wave_frequency_->setToolTip(tr("Number of spatial oscillations across the normalized wave coordinate."));
    wave_cycles_->setToolTip(tr("Whole or signed motion cycles over one seamless project loop."));
    wave_phase_->setToolTip(tr("Authored phase offset; large values are allowed for procedural workflows."));
    wave_direction_->setToolTip(tr("Blends propagation from horizontal through radial to vertical."));
    layout->addWidget(properties);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.waves.size() >= pvt::kMaximumWaves) {
            QMessageBox::warning(this, tr("Wave limit"),
                                 tr("The Qt item-index limit is %1 waves.")
                                     .arg(pvt::kMaximumWaves));
            return;
        }
        auto before = captureActiveState();
        auto wave = pvt::default_wave(config_.waves.size());
        wave.id = pvt::allocate_id(config_);
        const auto id = wave.id;
        config_.waves.push_back(std::move(wave));
        syncActiveRender();
        refreshWaveList(id);
        schedulePreview();
        recordActiveStateChange(tr("Add wave"), std::move(before));
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedWaveIndex();
        if (!index || config_.waves.size() >= pvt::kMaximumWaves) {
            return;
        }
        auto before = captureActiveState();
        auto wave = config_.waves[*index];
        wave.id = pvt::allocate_id(config_);
        append_copy_suffix(wave.name);
        const auto id = wave.id;
        config_.waves.insert(config_.waves.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
                             std::move(wave));
        syncActiveRender();
        refreshWaveList(id);
        schedulePreview();
        recordActiveStateChange(tr("Duplicate wave"), std::move(before));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedWaveIndex();
        if (!index) {
            return;
        }
        auto before = captureActiveState();
        config_.waves.erase(config_.waves.begin() + static_cast<std::ptrdiff_t>(*index));
        syncActiveRender();
        refreshWaveList();
        schedulePreview();
        recordActiveStateChange(tr("Remove wave"), std::move(before));
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedWave(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedWave(1); });
    return page;
}

QWidget* MainWindow::createSynchronizationPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);

    clock_group_ = new QGroupBox(tr("Clock — project-wide"));
    auto* clock_layout = new QVBoxLayout(clock_group_);
    auto* clock_help = new QLabel(
        tr("The base clock drives every synchronized wave and effect in every layer. "
           "Frame, Time, and Time Signature modes can hold or interpolate the calculated "
           "clock between pulses; Music follows the embedded source's analyzed beat map. "
           "Mic (Live) follows a desktop input only while LIVE is running and preserves this authored clock for offline rendering."));
    clock_help->setWordWrap(true);
    clock_help->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    clock_layout->addWidget(clock_help);
    clock_form_ = new QFormLayout;
    clock_mode_ = new QComboBox;
    clock_mode_->setObjectName(QStringLiteral("clockMode"));
    for (const auto mode : {pvt::ClockMode::Default, pvt::ClockMode::Frame,
                            pvt::ClockMode::Time, pvt::ClockMode::Meter,
                            pvt::ClockMode::Music}) {
        add_enum_item(clock_mode_, QString::fromUtf8(pvt::clock_mode_name(mode)), mode);
    }
    clock_mode_->addItem(tr("Mic (Live)…"), kMicLiveClockSentinel);
    clock_mode_->setItemData(
        clock_mode_->count() - 1,
        tr("Use a detected microphone during LIVE while keeping the authored clock as the offline and CLI fallback."),
        Qt::ToolTipRole);
    project_mic_device_ = new QComboBox;
    project_mic_device_->setObjectName(QStringLiteral("projectMicDevice"));
    project_mic_refresh_ = new QPushButton(tr("Refresh"));
    project_mic_refresh_->setObjectName(QStringLiteral("projectMicRefresh"));
    project_mic_setup_ = new QPushButton(tr("Live controls…"));
    project_mic_setup_->setObjectName(QStringLiteral("projectMicSetup"));
    auto* project_mic_row = new QWidget;
    auto* project_mic_layout = new QHBoxLayout(project_mic_row);
    project_mic_layout->setContentsMargins(0, 0, 0, 0);
    project_mic_layout->addWidget(project_mic_device_, 1);
    project_mic_layout->addWidget(project_mic_refresh_);
    project_mic_layout->addWidget(project_mic_setup_);
    project_mic_status_ = new QLabel;
    project_mic_status_->setObjectName(QStringLiteral("projectMicStatus"));
    project_mic_status_->setWordWrap(true);
    clock_interpolation_ = new QComboBox;
    for (const auto value : {pvt::ClockInterpolation::Hold,
                             pvt::ClockInterpolation::Linear,
                             pvt::ClockInterpolation::Smoothstep}) {
        add_enum_item(clock_interpolation_,
                      QString::fromUtf8(pvt::clock_interpolation_name(value)), value);
    }
    clock_interpolation_->setToolTip(
        tr("Interpolates the evaluated clock/parameters, not rendered frames."));
    clock_fit_ = new QComboBox;
    for (const auto value : {pvt::ClockFit::Exact, pvt::ClockFit::FitSequence}) {
        add_enum_item(clock_fit_, QString::fromUtf8(pvt::clock_fit_name(value)), value);
    }
    clock_frame_interval_ = integer_editor(1, (std::numeric_limits<int>::max)());
    clock_frame_interval_->setObjectName(QStringLiteral("clockFrameInterval"));
    clock_time_interval_ms_ = real_editor(0.001, kMaximumClockMilliseconds, 3, 1.0);
    clock_time_interval_ms_->setSuffix(tr(" ms"));
    meter_expression_ = new QLineEdit;
    meter_expression_->setObjectName(QStringLiteral("meterExpression"));
    meter_expression_->setMaxLength((std::numeric_limits<int>::max)());
    meter_expression_->setPlaceholderText(tr("Examples: 7/8, 3+2+3/8, 5/4 | 6/4, 4/3"));
    meter_summary_ = new QLabel;
    meter_summary_->setWordWrap(true);
    meter_bpm_ = real_editor(
        kMinimumPositiveUiValue, kMaximumRenderParameter, 6, 1.0);
    meter_bpm_->setSuffix(tr(" BPM"));
    // Keep this aligned with the meter parser's bounded denominator domain.
    meter_tempo_note_ = integer_editor(1, (std::numeric_limits<int>::max)());
    meter_tempo_note_->setPrefix(tr("1/"));
    clock_reverse_ = new QCheckBox(tr("Reverse clock direction"));
    clock_phase_offset_ = real_editor(-kMaximumRenderParameter,
                                      kMaximumRenderParameter, 3, 1.0);
    clock_phase_offset_->setSuffix(QChar(0x00b0));
    music_tempo_mode_ = new QComboBox;
    for (const auto value : {pvt::MusicTempoMode::Half,
                             pvt::MusicTempoMode::Detected,
                             pvt::MusicTempoMode::Double}) {
        add_enum_item(music_tempo_mode_,
                      QString::fromUtf8(pvt::music_tempo_mode_name(value)), value);
    }
    music_beat_offset_ms_ = real_editor(-kMaximumClockMilliseconds,
                                        kMaximumClockMilliseconds, 3, 1.0);
    music_beat_offset_ms_->setSuffix(tr(" ms"));
    music_data_only_ = new QCheckBox(
        tr("Data only — mute during preview playback and movie export"));

    clock_form_->addRow(tr("Source"), clock_mode_);
    clock_form_->addRow(tr("Microphone"), project_mic_row);
    clock_form_->addRow(QString{}, project_mic_status_);
    clock_form_->addRow(tr("Between pulses"), clock_interpolation_);
    clock_form_->addRow(tr("Interval policy"), clock_fit_);
    clock_form_->addRow(tr("Pulse every"), clock_frame_interval_);
    clock_form_->addRow(tr("Pulse interval"), clock_time_interval_ms_);
    clock_form_->addRow(tr("Meter"), meter_expression_);
    clock_form_->addRow(tr("Parsed meter"), meter_summary_);
    clock_form_->addRow(tr("Tempo"), meter_bpm_);
    clock_form_->addRow(tr("Tempo note"), meter_tempo_note_);
    clock_form_->addRow(clock_reverse_);
    clock_form_->addRow(tr("Phase offset"), clock_phase_offset_);
    clock_form_->addRow(tr("Music tempo"), music_tempo_mode_);
    clock_form_->addRow(tr("Beat-grid offset"), music_beat_offset_ms_);
    clock_form_->addRow(music_data_only_);
    clock_layout->addLayout(clock_form_);

    auto* music_group = new QGroupBox(tr("Music source — embedded with project"));
    auto* music_layout = new QVBoxLayout(music_group);
    music_source_ = new QLineEdit;
    music_source_->setObjectName(QStringLiteral("musicSource"));
    music_source_->setReadOnly(true);
    music_source_->setPlaceholderText(tr("No analyzed music source"));
    music_layout->addWidget(music_source_);
    auto* music_processing_form = new QFormLayout;
    music_processing_ = new QPushButton(tr("Filters, EQ + Frequency Streams…"));
    music_frequency_stream_ = new QComboBox;
    music_frequency_stream_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    music_frequency_stream_->setMinimumContentsLength(24);
    music_processing_form->addRow(tr("Input processing"), music_processing_);
    music_processing_form->addRow(tr("Clock stream"), music_frequency_stream_);
    music_layout->addLayout(music_processing_form);
    auto* music_buttons = new QHBoxLayout;
    music_choose_ = new QPushButton(tr("Choose…"));
    music_relink_ = new QPushButton(tr("Relink…"));
    music_reanalyze_ = new QPushButton(tr("Reanalyze"));
    music_clear_ = new QPushButton(tr("Clear"));
    for (auto* button : {music_choose_, music_relink_, music_reanalyze_, music_clear_}) {
        music_buttons->addWidget(button);
    }
    music_layout->addLayout(music_buttons);
    music_summary_ = new QLabel;
    music_summary_->setObjectName(QStringLiteral("musicSummary"));
    music_summary_->setWordWrap(true);
    music_summary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    music_layout->addWidget(music_summary_);
    music_error_ = new QLabel;
    music_error_->setObjectName(QStringLiteral("musicError"));
    music_error_->setWordWrap(true);
    music_error_->setStyleSheet(
        QStringLiteral("QLabel { color: #ffb4ab; background: #4a2020; padding: 5px; }"));
    music_error_->hide();
    music_layout->addWidget(music_error_);
    auto* progress_row = new QHBoxLayout;
    music_progress_ = new QProgressBar;
    music_progress_->setObjectName(QStringLiteral("musicProgress"));
    music_progress_->setRange(0, 1000);
    music_progress_->setValue(0);
    music_progress_->hide();
    music_cancel_ = new QPushButton(tr("Cancel analysis"));
    music_cancel_->hide();
    progress_row->addWidget(music_progress_, 1);
    progress_row->addWidget(music_cancel_);
    music_layout->addLayout(progress_row);
    clock_layout->addWidget(music_group);
    layout->addWidget(clock_group_);

    layer_clock_group_ = new QGroupBox(tr("Clock — active layer"));
    layer_clock_group_->setCheckable(true);
    layer_clock_group_->setObjectName(QStringLiteral("layerClockGroup"));
    auto* layer_clock_layout = new QVBoxLayout(layer_clock_group_);
    auto* layer_clock_help = new QLabel(
        tr("With advanced mixing off, this replaces the project clock only for "
           "the active layer while remaining locked to the project timeline. "
           "Short music clips can repeat with minimal stretch, traverse once, "
           "or hand back to the project clock."));
    layer_clock_help->setWordWrap(true);
    layer_clock_layout->addWidget(layer_clock_help);
    layer_clock_form_ = new QFormLayout;
    layer_clock_mix_enabled_ = new QCheckBox(
        tr("Mix project and layer clocks (advanced)"));
    layer_clock_mix_enabled_->setObjectName(
        QStringLiteral("layerClockMixEnabled"));
    layer_clock_mix_enabled_->setToolTip(tr(
        "Off is the safe default and preserves the historical behavior: an "
        "enabled layer clock replaces the project clock. Turn this on only "
        "when you deliberately want both phases to contribute."));
    layer_clock_mix_mode_ = new QComboBox;
    layer_clock_mix_mode_->setObjectName(QStringLiteral("layerClockMixMode"));
    for (const auto value : {pvt::LayerClockMixMode::Replace,
                             pvt::LayerClockMixMode::Add,
                             pvt::LayerClockMixMode::Difference,
                             pvt::LayerClockMixMode::SoftXor,
                             pvt::LayerClockMixMode::BitwiseXor}) {
        add_enum_item(layer_clock_mix_mode_,
                      QString::fromUtf8(
                          pvt::layer_clock_mix_mode_name(value)),
                      value);
    }
    layer_clock_mix_mode_->setToolTip(tr(
        "Replace is unchanged from normal layer-clock behavior. Add and "
        "Difference combine wrapped phases. Soft XOR is continuous; Bitwise "
        "XOR combines 24-bit fixed-point phases and is intentionally "
        "experimental."));
    layer_clock_scale_ = new QComboBox;
    for (const auto value : {pvt::LayerClockScale::SmartLoopFit,
                             pvt::LayerClockScale::StraightFit,
                             pvt::LayerClockScale::PlayOnce,
                             pvt::LayerClockScale::PlayOnceThenProject,
                             pvt::LayerClockScale::OriginalSpeedLoop}) {
        add_enum_item(layer_clock_scale_,
                      QString::fromUtf8(pvt::layer_clock_scale_name(value)), value);
    }
    layer_clock_mode_ = new QComboBox;
    layer_clock_mode_->setObjectName(QStringLiteral("layerClockMode"));
    for (const auto mode : {pvt::ClockMode::Default, pvt::ClockMode::Frame,
                            pvt::ClockMode::Time, pvt::ClockMode::Meter,
                            pvt::ClockMode::Music}) {
        add_enum_item(layer_clock_mode_,
                      QString::fromUtf8(pvt::clock_mode_name(mode)), mode);
    }
    layer_clock_mode_->addItem(tr("Mic (Live)…"), kMicLiveClockSentinel);
    layer_clock_mode_->setItemData(
        layer_clock_mode_->count() - 1,
        tr("Use a detected microphone for this layer during LIVE while keeping its authored clock as the offline and CLI fallback."),
        Qt::ToolTipRole);
    layer_mic_device_ = new QComboBox;
    layer_mic_device_->setObjectName(QStringLiteral("layerMicDevice"));
    layer_mic_refresh_ = new QPushButton(tr("Refresh"));
    layer_mic_refresh_->setObjectName(QStringLiteral("layerMicRefresh"));
    layer_mic_setup_ = new QPushButton(tr("Live controls…"));
    layer_mic_setup_->setObjectName(QStringLiteral("layerMicSetup"));
    auto* layer_mic_row = new QWidget;
    auto* layer_mic_layout = new QHBoxLayout(layer_mic_row);
    layer_mic_layout->setContentsMargins(0, 0, 0, 0);
    layer_mic_layout->addWidget(layer_mic_device_, 1);
    layer_mic_layout->addWidget(layer_mic_refresh_);
    layer_mic_layout->addWidget(layer_mic_setup_);
    layer_mic_status_ = new QLabel;
    layer_mic_status_->setObjectName(QStringLiteral("layerMicStatus"));
    layer_mic_status_->setWordWrap(true);
    layer_clock_interpolation_ = new QComboBox;
    for (const auto value : {pvt::ClockInterpolation::Hold,
                             pvt::ClockInterpolation::Linear,
                             pvt::ClockInterpolation::Smoothstep}) {
        add_enum_item(layer_clock_interpolation_,
                      QString::fromUtf8(pvt::clock_interpolation_name(value)), value);
    }
    layer_clock_fit_ = new QComboBox;
    for (const auto value : {pvt::ClockFit::Exact,
                             pvt::ClockFit::FitSequence}) {
        add_enum_item(layer_clock_fit_,
                      QString::fromUtf8(pvt::clock_fit_name(value)), value);
    }
    layer_clock_frame_interval_ = integer_editor(
        1, (std::numeric_limits<int>::max)());
    layer_clock_time_interval_ms_ = real_editor(
        0.001, kMaximumClockMilliseconds, 3, 1.0);
    layer_clock_time_interval_ms_->setSuffix(tr(" ms"));
    layer_meter_expression_ = new QLineEdit;
    layer_meter_expression_->setMaxLength((std::numeric_limits<int>::max)());
    layer_meter_expression_->setPlaceholderText(
        tr("Examples: 7/8, 3+2+3/8, 5/4 | 6/4"));
    layer_meter_summary_ = new QLabel;
    layer_meter_summary_->setWordWrap(true);
    layer_meter_bpm_ = real_editor(
        kMinimumPositiveUiValue, kMaximumRenderParameter, 6, 1.0);
    layer_meter_bpm_->setSuffix(tr(" BPM"));
    layer_meter_tempo_note_ = integer_editor(
        1, (std::numeric_limits<int>::max)());
    layer_meter_tempo_note_->setPrefix(tr("1/"));
    layer_clock_reverse_ = new QCheckBox(tr("Reverse layer clock direction"));
    layer_clock_phase_offset_ = real_editor(-kMaximumRenderParameter,
                                            kMaximumRenderParameter, 3, 1.0);
    layer_clock_phase_offset_->setSuffix(QChar(0x00b0));
    layer_music_tempo_mode_ = new QComboBox;
    for (const auto value : {pvt::MusicTempoMode::Half,
                             pvt::MusicTempoMode::Detected,
                             pvt::MusicTempoMode::Double}) {
        add_enum_item(layer_music_tempo_mode_,
                      QString::fromUtf8(pvt::music_tempo_mode_name(value)), value);
    }
    layer_music_beat_offset_ms_ = real_editor(
        -kMaximumClockMilliseconds, kMaximumClockMilliseconds, 3, 1.0);
    layer_music_beat_offset_ms_->setSuffix(tr(" ms"));
    layer_music_data_only_ = new QCheckBox(
        tr("Data only — mute during preview playback and movie export"));
    layer_clock_form_->addRow(layer_clock_mix_enabled_);
    layer_clock_form_->addRow(tr("Mix operation"), layer_clock_mix_mode_);
    layer_clock_form_->addRow(tr("Duration mapping"), layer_clock_scale_);
    layer_clock_form_->addRow(tr("Source"), layer_clock_mode_);
    layer_clock_form_->addRow(tr("Microphone"), layer_mic_row);
    layer_clock_form_->addRow(QString{}, layer_mic_status_);
    layer_clock_form_->addRow(tr("Between pulses"), layer_clock_interpolation_);
    layer_clock_form_->addRow(tr("Interval policy"), layer_clock_fit_);
    layer_clock_form_->addRow(tr("Pulse every"), layer_clock_frame_interval_);
    layer_clock_form_->addRow(tr("Pulse interval"), layer_clock_time_interval_ms_);
    layer_clock_form_->addRow(tr("Meter"), layer_meter_expression_);
    layer_clock_form_->addRow(tr("Parsed meter"), layer_meter_summary_);
    layer_clock_form_->addRow(tr("Tempo"), layer_meter_bpm_);
    layer_clock_form_->addRow(tr("Tempo note"), layer_meter_tempo_note_);
    layer_clock_form_->addRow(layer_clock_reverse_);
    layer_clock_form_->addRow(tr("Phase offset"), layer_clock_phase_offset_);
    layer_clock_form_->addRow(tr("Music tempo"), layer_music_tempo_mode_);
    layer_clock_form_->addRow(tr("Beat-grid offset"),
                              layer_music_beat_offset_ms_);
    layer_clock_form_->addRow(layer_music_data_only_);
    layer_clock_layout->addLayout(layer_clock_form_);

    auto* layer_music_group = new QGroupBox(
        tr("Layer music source — embedded with project"));
    auto* layer_music_layout = new QVBoxLayout(layer_music_group);
    layer_music_source_ = new QLineEdit;
    layer_music_source_->setReadOnly(true);
    layer_music_source_->setPlaceholderText(tr("No analyzed layer music source"));
    layer_music_layout->addWidget(layer_music_source_);
    auto* layer_music_processing_form = new QFormLayout;
    layer_music_processing_ = new QPushButton(
        tr("Filters, EQ + Frequency Streams…"));
    layer_music_frequency_stream_ = new QComboBox;
    layer_music_frequency_stream_->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layer_music_frequency_stream_->setMinimumContentsLength(24);
    layer_music_processing_form->addRow(
        tr("Input processing"), layer_music_processing_);
    layer_music_processing_form->addRow(
        tr("Clock stream"), layer_music_frequency_stream_);
    layer_music_layout->addLayout(layer_music_processing_form);
    auto* layer_music_buttons = new QHBoxLayout;
    layer_music_choose_ = new QPushButton(tr("Choose…"));
    layer_music_relink_ = new QPushButton(tr("Relink…"));
    layer_music_reanalyze_ = new QPushButton(tr("Reanalyze"));
    layer_music_clear_ = new QPushButton(tr("Clear"));
    for (auto* button : {layer_music_choose_, layer_music_relink_,
                         layer_music_reanalyze_, layer_music_clear_}) {
        layer_music_buttons->addWidget(button);
    }
    layer_music_layout->addLayout(layer_music_buttons);
    layer_music_summary_ = new QLabel;
    layer_music_summary_->setWordWrap(true);
    layer_music_layout->addWidget(layer_music_summary_);
    layer_music_error_ = new QLabel;
    layer_music_error_->setWordWrap(true);
    layer_music_error_->setStyleSheet(
        QStringLiteral("QLabel { color: #ffb4ab; background: #4a2020; padding: 5px; }"));
    layer_music_error_->hide();
    layer_music_layout->addWidget(layer_music_error_);
    auto* layer_progress_row = new QHBoxLayout;
    layer_music_progress_ = new QProgressBar;
    layer_music_progress_->setRange(0, 1000);
    layer_music_progress_->hide();
    layer_music_cancel_ = new QPushButton(tr("Cancel analysis"));
    layer_music_cancel_->hide();
    layer_progress_row->addWidget(layer_music_progress_, 1);
    layer_progress_row->addWidget(layer_music_cancel_);
    layer_clock_layout->addWidget(layer_music_group);
    layout->addWidget(layer_clock_group_);
    // Keep the active transaction controls outside the layer-clock editor.
    // That editor is disabled while analysis owns its state; nesting Cancel
    // inside it made the only way to stop a long layer analysis unreachable.
    layout->addLayout(layer_progress_row);
    make_checkable_group_collapsible(layer_clock_group_);

    project_audio_response_group_ = new QGroupBox(
        tr("Audio response — project-wide defaults"));
    project_audio_response_group_->setCheckable(true);
    project_audio_response_group_->setObjectName(
        QStringLiteral("projectAudioResponseGroup"));
    auto* project_audio_layout = new QVBoxLayout(project_audio_response_group_);
    auto* project_audio_help = new QLabel(
        tr("Sets the response profile inherited by every layer that does not author an override. "
           "This is the fastest way to art-direct a consistent music relationship across a project."));
    project_audio_help->setWordWrap(true);
    project_audio_layout->addWidget(project_audio_help);
    auto* project_audio_form = new QFormLayout;
    project_audio_sync_only_ = new QCheckBox(
        tr("Only synchronized waves and effects"));
    project_audio_waves_enabled_ = new QCheckBox(
        tr("Modulate wave amplitude"));
    project_audio_wave_source_ = new QComboBox;
    project_audio_wave_amount_ = real_editor(-kMaximumRenderParameter,
                                             kMaximumRenderParameter, 3, 0.05);
    project_audio_effects_enabled_ = new QCheckBox(
        tr("Modulate effect strength"));
    project_audio_effect_source_ = new QComboBox;
    project_audio_effect_amount_ = real_editor(-kMaximumRenderParameter,
                                               kMaximumRenderParameter, 3, 0.05);
    project_audio_color_enabled_ = new QCheckBox(tr("Modulate color hue"));
    project_audio_color_source_ = new QComboBox;
    project_audio_color_amount_ = real_editor(-kMaximumRenderParameter,
                                              kMaximumRenderParameter, 2, 1.0);
    project_audio_color_amount_->setSuffix(QChar(0x00b0));

    audio_response_group_ = new QGroupBox(
        tr("Audio response — active-layer override"));
    audio_response_group_->setCheckable(true);
    audio_response_group_->setObjectName(QStringLiteral("audioResponseGroup"));
    auto* audio_layout = new QVBoxLayout(audio_response_group_);
    audio_response_effective_ = new QLabel;
    audio_response_effective_->setObjectName(
        QStringLiteral("audioResponseEffective"));
    audio_response_effective_->setWordWrap(true);
    audio_response_effective_->setFrameStyle(QFrame::StyledPanel
                                             | QFrame::Sunken);
    audio_response_effective_->setToolTip(
        tr("Shows whether this layer currently inherits the project profile or uses its own override."));
    audio_copy_project_ = new QPushButton(tr("Copy project settings into override"));
    audio_copy_project_->setToolTip(
        tr("Copies every project-wide response value here, then enables the layer override so you can refine it independently."));
    auto* audio_form = new QFormLayout;
    audio_response_enabled_ = new QCheckBox(
        tr("Enable audio response for this layer"));
    audio_sync_only_ = new QCheckBox(tr("Only synchronized waves and effects"));
    audio_waves_enabled_ = new QCheckBox(tr("Modulate wave amplitude"));
    audio_wave_source_ = new QComboBox;
    audio_wave_amount_ = real_editor(-kMaximumRenderParameter,
                                     kMaximumRenderParameter, 3, 0.05);
    audio_effects_enabled_ = new QCheckBox(tr("Modulate effect strength"));
    audio_effect_source_ = new QComboBox;
    audio_effect_amount_ = real_editor(-kMaximumRenderParameter,
                                       kMaximumRenderParameter, 3, 0.05);
    audio_color_enabled_ = new QCheckBox(tr("Modulate color hue"));
    audio_color_source_ = new QComboBox;
    audio_color_amount_ = real_editor(-kMaximumRenderParameter,
                                      kMaximumRenderParameter, 2, 1.0);
    audio_color_amount_->setSuffix(QChar(0x00b0));
    // MusicFeature has an explicit byte-sized ABI. Discover every named value
    // for both profiles so analyzer upgrades cannot make the editors drift.
    for (int raw = 0; raw <= (std::numeric_limits<std::uint8_t>::max)(); ++raw) {
        const auto feature = static_cast<pvt::MusicFeature>(raw);
        const char* const name = pvt::music_feature_name(feature);
        if (name == nullptr || std::string_view(name) == "Unknown") continue;
        const QString label = QString::fromUtf8(name);
        add_enum_item(project_audio_wave_source_, label, feature);
        add_enum_item(project_audio_effect_source_, label, feature);
        add_enum_item(project_audio_color_source_, label, feature);
        add_enum_item(audio_wave_source_, label, feature);
        add_enum_item(audio_effect_source_, label, feature);
        add_enum_item(audio_color_source_, label, feature);
    }
    project_audio_form->addRow(project_audio_sync_only_);
    project_audio_form->addRow(project_audio_waves_enabled_);
    project_audio_form->addRow(tr("Wave feature"), project_audio_wave_source_);
    project_audio_form->addRow(tr("Wave amount"), project_audio_wave_amount_);
    project_audio_form->addRow(project_audio_effects_enabled_);
    project_audio_form->addRow(tr("Effect feature"), project_audio_effect_source_);
    project_audio_form->addRow(tr("Effect amount"), project_audio_effect_amount_);
    project_audio_form->addRow(project_audio_color_enabled_);
    project_audio_form->addRow(tr("Color feature"), project_audio_color_source_);
    project_audio_form->addRow(tr("Hue range"), project_audio_color_amount_);
    project_audio_layout->addLayout(project_audio_form);

    audio_form->addRow(audio_response_enabled_);
    audio_form->addRow(audio_sync_only_);
    audio_form->addRow(audio_waves_enabled_);
    audio_form->addRow(tr("Wave feature"), audio_wave_source_);
    audio_form->addRow(tr("Wave amount"), audio_wave_amount_);
    audio_form->addRow(audio_effects_enabled_);
    audio_form->addRow(tr("Effect feature"), audio_effect_source_);
    audio_form->addRow(tr("Effect amount"), audio_effect_amount_);
    audio_form->addRow(audio_color_enabled_);
    audio_form->addRow(tr("Color feature"), audio_color_source_);
    audio_form->addRow(tr("Hue range"), audio_color_amount_);
    audio_layout->addLayout(audio_form);

    const auto set_audio_tooltips = [](
        QCheckBox* synchronized_only, QCheckBox* waves_enabled,
        QComboBox* wave_source, QDoubleSpinBox* wave_amount,
        QCheckBox* effects_enabled, QComboBox* effect_source,
        QDoubleSpinBox* effect_amount, QCheckBox* color_enabled,
        QComboBox* color_source, QDoubleSpinBox* color_amount) {
        synchronized_only->setToolTip(
            tr("When enabled, free-running waves/effects ignore audio. A synchronized item's Audio response can inherit the profile, select a specific feature, force the profile feature on, or ignore audio."));
        waves_enabled->setToolTip(
            tr("Default routing for wave amplitude. A synchronized wave can override this with its Audio response selector."));
        wave_source->setToolTip(
            tr("Analyzed music feature sampled at the layer's effective timeline position."));
        wave_amount->setToolTip(
            tr("Scales wave amplitude by 1 + feature × amount. Negative values duck on peaks; -1 can reduce a full-scale peak to zero."));
        effects_enabled->setToolTip(
            tr("Default routing for effect intensity. A synchronized effect can override this with its Audio response selector."));
        effect_source->setToolTip(
            tr("Analyzed feature used to modulate enabled effect intensity."));
        effect_amount->setToolTip(
            tr("Scales effect intensity by 1 + feature × amount. The renderer clamps the multiplier at zero."));
        color_enabled->setToolTip(
            tr("Rotates the layer's evaluated starting colors without rewriting the authored palette."));
        color_source->setToolTip(
            tr("Analyzed feature used as the hue-rotation control signal."));
        color_amount->setToolTip(
            tr("Maximum signed hue rotation at a full-scale feature value."));
    };
    set_audio_tooltips(
        project_audio_sync_only_, project_audio_waves_enabled_,
        project_audio_wave_source_, project_audio_wave_amount_,
        project_audio_effects_enabled_, project_audio_effect_source_,
        project_audio_effect_amount_, project_audio_color_enabled_,
        project_audio_color_source_, project_audio_color_amount_);
    set_audio_tooltips(
        audio_sync_only_, audio_waves_enabled_, audio_wave_source_,
        audio_wave_amount_, audio_effects_enabled_, audio_effect_source_,
        audio_effect_amount_, audio_color_enabled_, audio_color_source_,
        audio_color_amount_);
    project_audio_response_group_->setToolTip(
        tr("The canonical response profile for layers whose active-layer override is off. Missing/null project data resolves to the safe Default profile."));
    audio_response_group_->setToolTip(
        tr("Check this group to stop inheriting and author an independent response profile for the selected layer."));
    audio_response_enabled_->setToolTip(
        tr("Master switch stored inside this layer's override. Clear it to keep an explicit no-response layer while project defaults remain active elsewhere."));

    make_checkable_group_collapsible(project_audio_response_group_);
    make_checkable_group_collapsible(audio_response_group_);

    layout->addWidget(project_audio_response_group_);
    layout->addWidget(audio_response_effective_);
    layout->addWidget(audio_copy_project_, 0, Qt::AlignLeft);
    layout->addWidget(audio_response_group_);
    layout->addWidget(createSwingBlock());
    make_checkable_group_collapsible(swings_group_);
    layout->addStretch();
    scroll->setWidget(contents);

    connect(music_choose_, &QPushButton::clicked, this, &MainWindow::chooseMusicSource);
    connect(music_relink_, &QPushButton::clicked, this, &MainWindow::relinkMusicSource);
    connect(music_reanalyze_, &QPushButton::clicked, this,
            &MainWindow::reanalyzeMusicSource);
    connect(music_clear_, &QPushButton::clicked, this, &MainWindow::clearMusicSource);
    connect(music_cancel_, &QPushButton::clicked, this, [this] {
        cancelMusicAnalysis(tr("Cancelling music analysis…"));
    });
    connect(music_processing_, &QPushButton::clicked, this,
            [this] { editMusicInputProcessing(false); });
    connect(music_frequency_stream_, &QComboBox::currentIndexChanged, this,
            [this] { applyClockEditor(music_frequency_stream_); });
    connect(layer_music_choose_, &QPushButton::clicked,
            this, &MainWindow::chooseLayerMusicSource);
    connect(layer_music_relink_, &QPushButton::clicked,
            this, &MainWindow::relinkLayerMusicSource);
    connect(layer_music_reanalyze_, &QPushButton::clicked,
            this, &MainWindow::reanalyzeLayerMusicSource);
    connect(layer_music_clear_, &QPushButton::clicked,
            this, &MainWindow::clearLayerMusicSource);
    connect(layer_music_cancel_, &QPushButton::clicked, this, [this] {
        cancelMusicAnalysis(tr("Cancelling layer music analysis…"));
    });
    connect(layer_music_processing_, &QPushButton::clicked, this,
            [this] { editMusicInputProcessing(true); });
    connect(layer_music_frequency_stream_, &QComboBox::currentIndexChanged, this,
            [this] { applyClockEditor(layer_music_frequency_stream_); });
    return scroll;
}

QWidget* MainWindow::createSwingBlock() {
    swings_group_ = new QGroupBox(tr("Swings — active layer"));
    swings_group_->setCheckable(true);
    swings_group_->setObjectName(QStringLiteral("swingsGroup"));
    auto* page = swings_group_;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Swing modulators reshape the synchronized clock. Add, duplicate, remove, "
           "or reorder them for loop-safe rhythm. A Local radius above zero creates "
           "the numbered source/UV circle in the preview. Localized Swings drive source "
           "waves and Texture effects; Mapped-object effects use the global clock."));
    explanation->setWordWrap(true);
    explanation->setObjectName(QStringLiteral("swingExplanation"));
    // A vertically Preferred word-wrapped label may be compressed below its
    // height-for-width by the stretchable list beneath it. Treat its wrapped
    // size hint as a minimum so the final lines are never clipped.
    explanation->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    explanation->setMinimumWidth(0);
    explanation->setMinimumHeight(explanation->fontMetrics().lineSpacing() * 7);
    layout->addWidget(explanation);

    swing_list_ = new QListWidget;
    swing_list_->setAlternatingRowColors(true);
    layout->addWidget(swing_list_, 1);

    auto* buttons = new QGridLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    buttons->addWidget(add, 0, 0);
    buttons->addWidget(duplicate, 0, 1);
    buttons->addWidget(remove, 0, 2);
    buttons->addWidget(up, 1, 0);
    buttons->addWidget(down, 1, 1);
    buttons->setColumnStretch(2, 1);
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
    swing_amount_ = real_editor(-kMaximumRenderParameter,
                                kMaximumRenderParameter, 4, 0.05);
    swing_cycles_ = integer_editor(kMinimumIntegerParameter,
                                   kMaximumIntegerParameter);
    swing_phase_ = real_editor(-kMaximumRenderParameter,
                               kMaximumRenderParameter, 3, 1.0);
    swing_shape_ = real_editor(0.0, 1.0, 4, 0.05);
    swing_center_x_ = real_editor(-kMaximumRenderParameter,
                                  kMaximumRenderParameter, 4, 0.01);
    swing_center_y_ = real_editor(-kMaximumRenderParameter,
                                  kMaximumRenderParameter, 4, 0.01);
    swing_radius_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    swing_amount_->setToolTip(
        tr("Strength of this timing modulation. Negative values invert the swing."));
    swing_shape_->setToolTip(
        tr("Changes the contour of shaped waveforms; sine and triangle ignore it."));
    swing_radius_->setToolTip(
        tr("Fraction of the shorter canvas edge. Zero preserves whole-layer behavior; "
           "positive values create a feathered movable circle."));
    form->addRow(tr("Name"), swing_name_);
    form->addRow(swing_enabled_);
    form->addRow(tr("Waveform"), swing_waveform_);
    form->addRow(tr("Amount"), swing_amount_);
    form->addRow(tr("Pulses per loop"), swing_cycles_);
    form->addRow(tr("Phase (degrees)"), swing_phase_);
    form->addRow(tr("Waveform shape"), swing_shape_);
    form->addRow(tr("Center X (0–1)"), swing_center_x_);
    form->addRow(tr("Center Y (0–1)"), swing_center_y_);
    form->addRow(tr("Local radius (0 = whole layer)"), swing_radius_);
    layout->addWidget(properties);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.swings.size() >= pvt::kMaximumSwings) {
            QMessageBox::warning(this, tr("Swing limit"),
                                 tr("The Qt item-index limit is %1 swing modulators.")
                                     .arg(pvt::kMaximumSwings));
            return;
        }
        auto before = captureActiveState();
        auto swing = pvt::default_swing(config_.swings.size());
        swing.id = pvt::allocate_id(config_);
        const auto id = swing.id;
        config_.swings.push_back(std::move(swing));
        syncActiveRender();
        refreshSwingList(id);
        schedulePreview();
        recordActiveStateChange(tr("Add swing"), std::move(before));
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedSwingIndex();
        if (!index || config_.swings.size() >= pvt::kMaximumSwings) {
            return;
        }
        auto before = captureActiveState();
        auto swing = config_.swings[*index];
        swing.id = pvt::allocate_id(config_);
        append_copy_suffix(swing.name);
        const auto id = swing.id;
        config_.swings.insert(
            config_.swings.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
            std::move(swing));
        syncActiveRender();
        refreshSwingList(id);
        schedulePreview();
        recordActiveStateChange(tr("Duplicate swing"), std::move(before));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedSwingIndex();
        if (!index) {
            return;
        }
        auto before = captureActiveState();
        config_.swings.erase(config_.swings.begin() + static_cast<std::ptrdiff_t>(*index));
        syncActiveRender();
        refreshSwingList();
        schedulePreview();
        recordActiveStateChange(tr("Remove swing"), std::move(before));
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedSwing(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedSwing(1); });
    return page;
}

QWidget* MainWindow::createEffectPage() {
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("categorizedLayerEffectsPage"));
    auto* layout = new QVBoxLayout(page);
    effect_stage_summary_ = new QLabel;
    effect_stage_summary_->setObjectName(QStringLiteral("effectStageSummary"));
    effect_stage_summary_->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    effect_stage_summary_->setWordWrap(true);
    effect_stage_summary_->setAccessibleName(tr("Current effect stage"));
    layout->addWidget(effect_stage_summary_);
    auto* explanation = new QLabel(
        tr("Each effect appears in exactly one category based on its type. New effects "
           "start on Texture, which is the useful default. Mapped-surface placement remains "
           "an advanced per-effect option; it does not create a duplicate effects window. "
           "Centers and local radii remain draggable in the preview."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    effect_category_tabs_ = new QTabBar;
    effect_category_tabs_->setObjectName(QStringLiteral("effectCategoryTabs"));
    effect_category_tabs_->setAccessibleName(tr("Effect categories"));
    effect_category_tabs_->setExpanding(true);
    for (int category = 0; category < EffectUiCategoryCount; ++category) {
        QString label = effect_ui_category_name(category);
        label.replace(QLatin1Char('&'), QStringLiteral("&&"));
        effect_category_tabs_->addTab(label);
    }
    layout->addWidget(effect_category_tabs_);

    effect_list_ = new QListWidget;
    effect_list_->setAlternatingRowColors(true);
    layout->addWidget(effect_list_, 1);

    auto* add_row = new QGridLayout;
    add_effect_type_ = new QComboBox;
    populate_effect_types(add_effect_type_, effect_category_filter_);
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    add_row->addWidget(add_effect_type_, 0, 0, 1, 3);
    add_row->addWidget(add, 1, 0);
    add_row->addWidget(duplicate, 1, 1);
    add_row->addWidget(remove, 1, 2);
    add_row->addWidget(up, 2, 0);
    add_row->addWidget(down, 2, 1);
    add_row->setColumnStretch(2, 1);
    layout->addLayout(add_row);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* properties = new QGroupBox(tr("Selected effect"));
    effect_form_ = new QFormLayout(properties);
    effect_name_ = new QLineEdit;
    effect_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    effect_name_->setValidator(new Utf8TextValidator(TextRule::Name, effect_name_));
    effect_enabled_ = new QCheckBox(tr("Enabled"));
    effect_sync_ = new QCheckBox(tr("Synchronization"));
    effect_audio_response_ = new QComboBox;
    populate_audio_response_combo(effect_audio_response_);
    effect_type_ = new QComboBox;
    populate_effect_types(effect_type_, effect_category_filter_);
    effect_space_ = new QComboBox;
    add_enum_item(effect_space_, tr("Texture (default)"),
                  pvt::EffectSpace::Texture);
    add_enum_item(effect_space_, tr("Mapped surface (advanced)"),
                  pvt::EffectSpace::Surface);
    effect_cycles_ = integer_editor(kMinimumIntegerParameter,
                                    kMaximumIntegerParameter);
    effect_phase_ = real_editor(-kMaximumRenderParameter,
                                kMaximumRenderParameter, 3, 1.0);
    effect_edge_ = new QComboBox;
    add_enum_item(effect_edge_, tr("Transparent alpha"), pvt::EdgeMode::Alpha);
    add_enum_item(effect_edge_, tr("Black"), pvt::EdgeMode::Black);
    add_enum_item(effect_edge_, tr("White"), pvt::EdgeMode::White);
    add_enum_item(effect_edge_, tr("Reflected pattern"), pvt::EdgeMode::Reflect);
    effect_intensity_ = real_editor(0.0, kMaximumRenderParameter);
    effect_magnitude_ = real_editor(0.0, kMaximumRenderParameter, 6, 0.005);
    effect_frequency_ = real_editor(0.0, kMaximumRenderParameter);
    effect_secondary_ = real_editor(-kMaximumRenderParameter,
                                    kMaximumRenderParameter);
    effect_center_x_ = real_editor(-kMaximumRenderParameter,
                                   kMaximumRenderParameter);
    effect_center_y_ = real_editor(-kMaximumRenderParameter,
                                   kMaximumRenderParameter);
    effect_angle_ = real_editor(-kMaximumRenderParameter,
                                kMaximumRenderParameter, 2, 5.0);
    effect_radius_ = real_editor(0.0, kMaximumRenderParameter, 6, 1.0);
    effect_threshold_ = real_editor(0.0, kMaximumRenderParameter);
    effect_knee_ = real_editor(0.0, 1.0);
    effect_area_radius_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    effect_particle_shape_ = new QComboBox;
    for (const auto shape : {pvt::ParticleShape::Spark,
                             pvt::ParticleShape::SoftOrb,
                             pvt::ParticleShape::Ring,
                             pvt::ParticleShape::Diamond,
                             pvt::ParticleShape::Star}) {
        add_enum_item(effect_particle_shape_,
                      QString::fromUtf8(pvt::particle_shape_name(shape)), shape);
    }
    effect_particle_profile_ = new QComboBox;
    for (const auto profile : {pvt::ParticleRenderProfile::LegacyGlow,
                               pvt::ParticleRenderProfile::Defined}) {
        add_enum_item(
            effect_particle_profile_,
            QString::fromUtf8(pvt::particle_render_profile_name(profile)),
            profile);
    }
    effect_particle_size_scale_ = new QSlider(Qt::Horizontal);
    effect_particle_size_scale_->setRange(0, kParticleSizeSliderSteps);
    effect_particle_size_scale_->setPageStep(50);
    effect_particle_size_scale_->setAccessibleName(tr("Particle size scale"));
    effect_particle_size_variation_ = real_editor(0.0, 1.0, 3, 0.01);
    effect_particle_definition_ = real_editor(0.0, 1.0, 3, 0.01);
    effect_particle_twinkle_ = real_editor(0.0, 1.0, 3, 0.01);
    effect_particle_seed_ = new QLineEdit;
    effect_particle_seed_->setPlaceholderText(tr("0 uses effect ID"));
    effect_particle_reseed_ = new QPushButton(tr("Surprise me — reseed"));
    effect_particle_orientation_ = new QComboBox;
    for (const auto orientation : {pvt::ParticleOrientation::Fixed,
                                   pvt::ParticleOrientation::FollowMotion,
                                   pvt::ParticleOrientation::Random}) {
        add_enum_item(
            effect_particle_orientation_,
            QString::fromUtf8(pvt::particle_orientation_name(orientation)),
            orientation);
    }
    effect_particle_rotation_ = real_editor(
        -kMaximumRenderParameter, kMaximumRenderParameter, 2, 5.0);
    effect_blur_type_ = new QComboBox;
    for (const auto type : {pvt::BlurType::Gaussian, pvt::BlurType::Box,
                            pvt::BlurType::Directional, pvt::BlurType::Radial,
                            pvt::BlurType::Zoom}) {
        add_enum_item(effect_blur_type_,
                      QString::fromUtf8(pvt::blur_type_name(type)), type);
    }
    effect_blur_passes_ = integer_editor(1, kMaximumIntegerParameter);
    effect_blur_samples_ = integer_editor(2, kMaximumIntegerParameter);
    effect_blur_minimum_ = real_editor(0.0, 1.0, 4, 0.01);
    effect_blur_maximum_ = real_editor(0.0, 1.0, 4, 0.01);
    effect_form_->addRow(tr("Name"), effect_name_);
    effect_form_->addRow(effect_enabled_);
    effect_form_->addRow(effect_sync_);
    effect_form_->addRow(tr("Audio response"), effect_audio_response_);
    effect_form_->addRow(tr("Type"), effect_type_);
    effect_form_->addRow(tr("Placement"), effect_space_);
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
    effect_form_->addRow(tr("Local area radius (0 = whole layer)"),
                         effect_area_radius_);
    effect_form_->addRow(tr("Particle shape"), effect_particle_shape_);
    effect_form_->addRow(tr("Particle look"), effect_particle_profile_);
    effect_form_->addRow(tr("Particle size scale"),
                         effect_particle_size_scale_);
    effect_form_->addRow(tr("Size variation"),
                         effect_particle_size_variation_);
    effect_form_->addRow(tr("Shape definition"),
                         effect_particle_definition_);
    effect_form_->addRow(tr("Twinkle amount"), effect_particle_twinkle_);
    effect_form_->addRow(tr("Pattern seed"), effect_particle_seed_);
    effect_form_->addRow(effect_particle_reseed_);
    effect_form_->addRow(tr("Shape / trail orientation"),
                         effect_particle_orientation_);
    effect_form_->addRow(tr("Shape rotation (degrees)"),
                         effect_particle_rotation_);
    effect_form_->addRow(tr("Blur algorithm"), effect_blur_type_);
    effect_form_->addRow(tr("Blur passes"), effect_blur_passes_);
    effect_form_->addRow(tr("Samples per pass"), effect_blur_samples_);
    effect_form_->addRow(tr("Minimum blur mix"), effect_blur_minimum_);
    effect_form_->addRow(tr("Maximum blur mix"), effect_blur_maximum_);
    effect_name_->setToolTip(
        tr("A descriptive layer-local name used in the stack and semantic version diffs."));
    effect_enabled_->setToolTip(
        tr("Bypasses this effect without deleting it or changing its authored parameters."));
    effect_sync_->setToolTip(
        tr("On: use the effective project or per-layer clock. Off: use an independent seamless clock. Cycles per loop is the modulation count in either mode."));
    effect_audio_response_->setToolTip(
        tr("Default inherits both the effective profile's Effects switch and Effect source. While the profile master is enabled, choosing Beat, Energy, or another feature opts this effect in and overrides that source. The final two choices force this effect on with the profile source or ignore audio. Clock synchronization is independent unless the profile explicitly limits response to synchronized items. Missing/null project data is Default."));
    effect_type_->setToolTip(
        tr("Changes the effect algorithm while preserving identity, routing, timing, center, and local area."));
    effect_particle_profile_->setToolTip(
        tr("Defined silhouette keeps shapes crisp and visibly different. Legacy glow reproduces the softer pre-format-15 look."));
    effect_particle_size_scale_->setToolTip(
        tr("Logarithmic hands-on size control from 0.5 to 256 full-resolution output pixels. The exact radius editor accepts values beyond this slider."));
    effect_particle_size_variation_->setToolTip(
        tr("Deterministically varies each particle radius around the exact base radius."));
    effect_particle_definition_->setToolTip(
        tr("Sharpens arms, corners, points, and ring thickness in Defined silhouette mode."));
    effect_particle_twinkle_->setToolTip(
        tr("Zero is steady; one uses the full deterministic brightness animation."));
    effect_particle_seed_->setToolTip(
        tr("Unsigned 64-bit deterministic seed. Zero derives a stable pattern from the effect ID."));
    effect_particle_reseed_->setToolTip(
        tr("Creates a new deterministic layout without changing the other authored controls."));
    effect_particle_orientation_->setToolTip(
        tr("Fixed preserves the authored travel-angle alignment. Follow motion makes trails point behind the actual orbit. Random keeps motion-following trails and rotates each silhouette independently."));
    effect_space_->setToolTip(
        tr("Texture is the normal choice and works whether surface mapping is on or off. Mapped surface is an advanced after-mapping placement for the uncommon cases that need it."));
    effect_blur_type_->setToolTip(
        tr("Gaussian and Box use separable passes; Directional follows the authored angle; Radial rotates samples around the center; Zoom pulls samples toward the center."));
    effect_blur_passes_->setToolTip(
        tr("Repeats the complete blur. More passes create a broader, smoother result and require more GPU or CPU work."));
    effect_blur_samples_->setToolTip(
        tr("Samples used in each pass. Higher values improve smoothness at the cost of render time."));
    effect_blur_minimum_->setToolTip(
        tr("Lowest wet/dry mix reached on the effect's selected clock. Set equal to the maximum for constant blur."));
    effect_blur_maximum_->setToolTip(
        tr("Highest wet/dry mix reached on the effect's selected clock. Set equal to the minimum for constant blur."));
    effect_cycles_->setToolTip(
        tr("Signed effect modulations across the project loop; whole values preserve the seam. For Blur, this directly controls the number of minimum-to-maximum-to-minimum pulses."));
    effect_phase_->setToolTip(
        tr("Authored timing offset in degrees. Values beyond one rotation are retained."));
    scroll->setWidget(properties);
    layout->addWidget(scroll, 2);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.effects.size() >= pvt::kMaximumEffects) {
            QMessageBox::warning(this, tr("Effect limit"),
                                 tr("The Qt item-index limit is %1 effects.")
                                     .arg(pvt::kMaximumEffects));
            return;
        }
        auto before = captureActiveState();
        const auto type = static_cast<pvt::EffectType>(add_effect_type_->currentData().toInt());
        auto effect = new_effect_for_ui(type);
        effect.id = pvt::allocate_id(config_);
        const auto id = effect.id;
        config_.effects.push_back(std::move(effect));
        syncActiveRender();
        refreshEffectList(id);
        schedulePreview();
        recordActiveStateChange(tr("Add effect"), std::move(before));
    });
    connect(duplicate, &QPushButton::clicked, this, [this] {
        const auto index = selectedEffectIndex();
        if (!index || config_.effects.size() >= pvt::kMaximumEffects) {
            return;
        }
        auto before = captureActiveState();
        auto effect = config_.effects[*index];
        effect.id = pvt::allocate_id(config_);
        append_copy_suffix(effect.name);
        const auto id = effect.id;
        config_.effects.insert(
            config_.effects.begin() + static_cast<std::ptrdiff_t>(*index + 1U),
            std::move(effect));
        syncActiveRender();
        refreshEffectList(id);
        schedulePreview();
        recordActiveStateChange(tr("Duplicate effect"), std::move(before));
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto index = selectedEffectIndex();
        if (!index) {
            return;
        }
        auto before = captureActiveState();
        config_.effects.erase(config_.effects.begin() + static_cast<std::ptrdiff_t>(*index));
        syncActiveRender();
        refreshEffectList();
        schedulePreview();
        recordActiveStateChange(tr("Remove effect"), std::move(before));
    });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedEffect(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedEffect(1); });
    connect(effect_category_tabs_, &QTabBar::currentChanged,
            this, &MainWindow::setEffectCategory);
    return page;
}

QWidget* MainWindow::createLayerSettingsPage() {
    const auto make_stage_page = [](QWidget*& page) {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* contents = new QWidget;
        auto* layout = new QVBoxLayout(contents);
        scroll->setWidget(contents);
        page = scroll;
        return layout;
    };
    auto* source_layout = make_stage_page(source_page_);
    auto* surface_layout = make_stage_page(surface_page_);
    auto* motion_layout = make_stage_page(motion_page_);
    auto* finish_layout = make_stage_page(finish_page_);
    source_page_->setObjectName(QStringLiteral("startingColorsPage"));
    surface_page_->setObjectName(QStringLiteral("layerModifiersPage"));
    motion_page_->setObjectName(QStringLiteral("layerMovementPage"));
    finish_page_->setObjectName(QStringLiteral("postEffectsPage"));

    const auto add_stage_intro = [](QVBoxLayout* layout, const QString& title,
                                    const QString& description) {
        auto* heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
        heading->setTextFormat(Qt::RichText);
        heading->setAccessibleName(title);
        auto* explanation = new QLabel(description);
        explanation->setWordWrap(true);
        explanation->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(heading);
        layout->addWidget(explanation);
    };
    add_stage_intro(
        source_layout, tr("Starting Colors — establish the layer's pixels"),
        tr("Choose an embedded image or generated color pattern, then optionally constrain it with a starting palette. Shaping, alpha, mapping, and movement have their own categories."));
    add_stage_intro(
        surface_layout, tr("Active Layer Modifiers"),
        tr("Shape generated coordinates, transform the layer, control source/procedural alpha, and optionally wrap it on a 3D surface. Surface mapping is an advanced modifier, not a required workflow stage."));
    add_stage_intro(
        motion_layout, tr("Layer Movement"),
        tr("Combine local waves, seamless whole-layer movement, and reusable paths. Movement and distortion effect types are separated in Layer Effects instead of duplicated by mapping location."));
    add_stage_intro(
        finish_layout, tr("Post Effects"),
        tr("Finish the active layer with channel inversion, alpha-aware edge antialiasing, and optional color quantization. They run in that order before compositing; export encoding and destination settings remain in Export."));

    auto* rhythm_group = new QGroupBox(tr("Rhythm and color timing"));
    auto* rhythm = new QFormLayout(rhythm_group);
    phrase_warp_ = real_editor(-kMaximumRenderParameter,
                               kMaximumRenderParameter, 4, 0.01);
    ghost_mix_ = real_editor(0.0, 1.0, 4, 0.01);
    ghost_lag_ = real_editor(-kMaximumRenderParameter,
                             kMaximumRenderParameter, 3, 1.0);
    phrase_warp_->setToolTip(
        tr("Periodic warp applied to the effective project-wide or per-layer synchronized clock."));
    ghost_mix_->setToolTip(tr("Mix between the main and phase-lagged color signals."));
    ghost_lag_->setToolTip(tr("Phase separation of the ghost color signal."));
    rhythm->addRow(tr("Phrase warp"), phrase_warp_);
    rhythm->addRow(tr("Ghost mix"), ghost_mix_);
    rhythm->addRow(tr("Ghost lag (degrees)"), ghost_lag_);
    if (auto* driver_scroll = qobject_cast<QScrollArea*>(synchronization_page_)) {
        if (auto* driver_contents = driver_scroll->widget()) {
            if (auto* driver_layout = qobject_cast<QVBoxLayout*>(
                    driver_contents->layout())) {
                driver_layout->insertWidget(
                    (std::max)(0, driver_layout->count() - 1), rhythm_group);
            }
        }
    }

    starting_image_group_ = new QGroupBox(tr("Starting image source"));
    auto* source_form = new QFormLayout(starting_image_group_);
    starting_image_enabled_ = new QCheckBox(tr("Use embedded image as layer source"));
    starting_image_enabled_->setObjectName(
        QStringLiteral("startingImageEnabled"));
    source_form->addRow(starting_image_enabled_);
    auto* source_row = new QWidget;
    auto* source_row_layout = new QHBoxLayout(source_row);
    source_row_layout->setContentsMargins(0, 0, 0, 0);
    starting_image_path_ = new QLineEdit;
    starting_image_path_->setReadOnly(true);
    starting_image_path_->setPlaceholderText(tr("No embedded image selected"));
    starting_image_browse_ = new QPushButton(tr("Choose…"));
    starting_image_clear_ = new QPushButton(tr("Clear"));
    source_row_layout->addWidget(starting_image_path_, 1);
    source_row_layout->addWidget(starting_image_browse_);
    source_row_layout->addWidget(starting_image_clear_);
    starting_image_fit_ = new QComboBox;
    for (const auto fit : {pvt::StartingImageFit::Stretch,
                           pvt::StartingImageFit::Contain,
                           pvt::StartingImageFit::Cover,
                           pvt::StartingImageFit::Tile}) {
        add_enum_item(starting_image_fit_,
                      QString::fromUtf8(pvt::starting_image_fit_name(fit)), fit);
    }
    source_form->addRow(tr("Embedded image (PNG / OpenEXR)"), source_row);
    source_form->addRow(tr("Fit"), starting_image_fit_);
    starting_image_palette_dither_ = new QCheckBox(
        tr("Dither when quantizing this image to the starting palette"));
    starting_image_palette_dither_method_ = new QComboBox;
    for (const auto method : {pvt::DitherMethod::BlueNoise,
                              pvt::DitherMethod::OrderedBayer,
                              pvt::DitherMethod::FloydSteinberg}) {
        add_enum_item(starting_image_palette_dither_method_,
                      QString::fromUtf8(pvt::dither_method_name(method)), method);
    }
    source_form->addRow(starting_image_palette_dither_);
    source_form->addRow(tr("Source quantization dither"),
                        starting_image_palette_dither_method_);
    starting_image_group_->setToolTip(tr(
        "PNG samples and HALF/FLOAT OpenEXR channels are decoded directly to "
        "float32 without an 8-bit intermediate. The image controls where source colors appear. When a "
        "starting palette is enabled, the image is quantized to that palette "
        "before effects; the two options are intentionally composable."));
    source_layout->addWidget(starting_image_group_);

    auto* starting_colors_group = new QGroupBox(tr("Generated starting colors"));
    auto* starting_colors_layout = new QVBoxLayout(starting_colors_group);
    auto* starting_colors_form = new QFormLayout;
    starting_color_mode_ = new QComboBox;
    for (const auto mode : {pvt::StartingColorMode::ContinuousHue,
                            pvt::StartingColorMode::HorizontalRainbow,
                            pvt::StartingColorMode::VerticalRainbow,
                            pvt::StartingColorMode::DiagonalRainbow,
                            pvt::StartingColorMode::SpiralRainbow,
                            pvt::StartingColorMode::SquareSpiralRainbow,
                            pvt::StartingColorMode::Random}) {
        add_enum_item(starting_color_mode_,
                      QString::fromUtf8(pvt::starting_color_mode_name(mode)), mode);
    }
    starting_color_include_alpha_ = new QCheckBox(
        tr("Include alpha as a generated color dimension"));
    starting_color_include_alpha_->setToolTip(
        tr("When no starting image or palette is active, generated RGBA tuples "
           "differ by alpha as well as RGB. This setting applies directly; the "
           "separate palette/image source-alpha switch does not disable it."));
    starting_colors_form->addRow(tr("Pattern"), starting_color_mode_);
    starting_colors_form->addRow(starting_color_include_alpha_);
    starting_colors_layout->addLayout(starting_colors_form);

    auto* channels = new QGridLayout;
    channels->addWidget(new QLabel(tr("Channel")), 0, 0);
    channels->addWidget(new QLabel(tr("Minimum")), 0, 1);
    channels->addWidget(new QLabel(tr("Maximum")), 0, 2);
    const auto add_channel = [channels](int row, const QString& name,
                                        QDoubleSpinBox*& minimum,
                                        QDoubleSpinBox*& maximum) {
        minimum = real_editor(0.0, 1.0, 4, 0.01);
        maximum = real_editor(0.0, 1.0, 4, 0.01);
        channels->addWidget(new QLabel(name), row, 0);
        channels->addWidget(minimum, row, 1);
        channels->addWidget(maximum, row, 2);
    };
    add_channel(1, tr("Red"), starting_red_minimum_, starting_red_maximum_);
    add_channel(2, tr("Green"), starting_green_minimum_, starting_green_maximum_);
    add_channel(3, tr("Blue"), starting_blue_minimum_, starting_blue_maximum_);
    add_channel(4, tr("Alpha"), starting_alpha_minimum_, starting_alpha_maximum_);
    starting_colors_layout->addLayout(channels);
    auto* starting_colors_help = new QLabel(tr(
        "Used when the authored starting palette is off. Every choice in this "
        "box obeys its RGB/alpha Min/Max range. Channel resolution scales "
        "automatically with the full-resolution output and block size; ordered "
        "patterns walk an automatically sized RGB or RGBA lattice without "
        "repetition in broad horizontal, vertical, diagonal, or spiral fields. "
        "Random is the only shuffled color-static pattern. Generated values remain float32 "
        "through effects and compositing; only the chosen output format "
        "quantizes them. Preview and export use the same full-resolution "
        "coordinates."));
    starting_colors_help->setObjectName(QStringLiteral("startingColorsHelp"));
    starting_colors_help->setWordWrap(true);
    starting_colors_layout->addWidget(starting_colors_help);
    source_layout->addWidget(starting_colors_group);

    auto* pattern_group = new QGroupBox(tr("Procedural features"));
    auto* pattern = new QFormLayout(pattern_group);
    displacement_enabled_ = new QCheckBox(tr("Displacement enabled"));
    displacement_ = real_editor(0.0, kMaximumRenderParameter, 2, 1.0);
    lighting_enabled_ = new QCheckBox(tr("Slope lighting enabled"));
    wave_depth_ = real_editor(0.0, kMaximumRenderParameter);
    spiral_enabled_ = new QCheckBox(tr("Spiral enabled"));
    spiral_frequency_ = real_editor(0.0, kMaximumRenderParameter);
    spiral_arms_ = integer_editor(kMinimumIntegerParameter,
                                  kMaximumIntegerParameter);
    wall_enabled_ = new QCheckBox(tr("Wall reflection enabled"));
    wall_frequency_ = real_editor(0.0, kMaximumRenderParameter);
    wall_mix_ = real_editor(-kMaximumRenderParameter,
                            kMaximumRenderParameter);
    hue_cycles_ = integer_editor(kMinimumIntegerParameter,
                                 kMaximumIntegerParameter);
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
    surface_layout->addWidget(pattern_group);

    kaleidoscope_group_ = new QGroupBox(tr("Kaleidoscope shaping"));
    kaleidoscope_group_->setCheckable(true);
    kaleidoscope_group_->setObjectName(QStringLiteral("kaleidoscopeGroup"));
    kaleidoscope_group_->setToolTip(tr(
        "Folds generated source coordinates into mirrored radial segments "
        "before waves and later pipeline stages."));
    auto* kaleidoscope = new QFormLayout(kaleidoscope_group_);
    kaleidoscope_segments_ = integer_editor(2, kMaximumIntegerParameter);
    kaleidoscope_rotation_ = real_editor(-kMaximumRenderParameter,
                                         kMaximumRenderParameter, 3, 1.0);
    kaleidoscope_rotation_->setSuffix(QChar(0x00b0));
    kaleidoscope_mix_ = real_editor(0.0, 1.0, 4, 0.01);
    kaleidoscope->addRow(tr("Mirrored segments"), kaleidoscope_segments_);
    kaleidoscope->addRow(tr("Rotation"), kaleidoscope_rotation_);
    kaleidoscope->addRow(tr("Coordinate mix"), kaleidoscope_mix_);
    surface_layout->addWidget(kaleidoscope_group_);
    make_checkable_group_collapsible(kaleidoscope_group_);

    domain_warp_group_ = new QGroupBox(tr("Seamless domain warp"));
    domain_warp_group_->setCheckable(true);
    domain_warp_group_->setObjectName(QStringLiteral("domainWarpGroup"));
    domain_warp_group_->setToolTip(tr(
        "Distorts generated coordinates with deterministic loop-closed noise. "
        "It does not affect an embedded starting image."));
    auto* domain_warp = new QFormLayout(domain_warp_group_);
    domain_warp_strength_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    domain_warp_scale_ = real_editor(
        kMinimumPositiveUiValue, kMaximumRenderParameter, 6, 0.05);
    domain_warp_octaves_ = integer_editor(1, kMaximumIntegerParameter);
    domain_warp_cycles_ = integer_editor(kMinimumIntegerParameter,
                                         kMaximumIntegerParameter);
    domain_warp_seed_ = new QLineEdit;
    domain_warp_seed_->setObjectName(QStringLiteral("domainWarpSeed"));
    domain_warp_seed_->setMaxLength(20);
    domain_warp_seed_->setPlaceholderText(tr("Unsigned 64-bit seed"));
    domain_warp_random_seed_ = new QPushButton(tr("Randomize"));
    domain_warp_random_seed_->setToolTip(tr(
        "Choose a new operating-system random seed. The selected value is "
        "stored in the project, so preview and export remain deterministic."));
    auto* seed_row = new QWidget;
    auto* seed_layout = new QHBoxLayout(seed_row);
    seed_layout->setContentsMargins(0, 0, 0, 0);
    seed_layout->addWidget(domain_warp_seed_, 1);
    seed_layout->addWidget(domain_warp_random_seed_);
    domain_warp->addRow(tr("Strength"), domain_warp_strength_);
    domain_warp->addRow(tr("Spatial scale"), domain_warp_scale_);
    domain_warp->addRow(tr("Noise octaves"), domain_warp_octaves_);
    domain_warp->addRow(tr("Cycles per loop"), domain_warp_cycles_);
    domain_warp->addRow(tr("Seed"), seed_row);
    surface_layout->addWidget(domain_warp_group_);
    make_checkable_group_collapsible(domain_warp_group_);

    auto* transform_group = new QGroupBox(tr("Transform layer"));
    auto* transform = new QFormLayout(transform_group);
    transform_flip_horizontal_ = new QCheckBox(tr("Flip horizontally"));
    transform_flip_vertical_ = new QCheckBox(tr("Flip vertically"));
    transform_mirror_ = new QComboBox;
    for (const auto mode : {pvt::MirrorMode::None,
                            pvt::MirrorMode::LeftToRight,
                            pvt::MirrorMode::RightToLeft,
                            pvt::MirrorMode::TopToBottom,
                            pvt::MirrorMode::BottomToTop,
                            pvt::MirrorMode::FourWay}) {
        add_enum_item(transform_mirror_,
                      QString::fromUtf8(pvt::mirror_mode_name(mode)), mode);
    }
    transform_mirror_->setToolTip(
        tr("Copies an explicitly named source half into the opposite half. "
           "Four-way mirrors the top-left quadrant. Flips run afterward."));
    transform->addRow(transform_flip_horizontal_);
    transform->addRow(transform_flip_vertical_);
    transform->addRow(tr("Mirror symmetry"), transform_mirror_);
    surface_layout->addWidget(transform_group);

    motion_group_ = new QGroupBox(tr("Seamless layer motion"));
    motion_group_->setCheckable(true);
    motion_group_->setObjectName(QStringLiteral("layerMotionGroup"));
    auto* motion = new QFormLayout(motion_group_);
    motion_path_ = new QComboBox;
    for (const auto path : {pvt::LayerMotionPath::None,
                            pvt::LayerMotionPath::Orbit,
                            pvt::LayerMotionPath::FigureEight,
                            pvt::LayerMotionPath::Bounce,
                            pvt::LayerMotionPath::Lissajous}) {
        add_enum_item(motion_path_,
                      QString::fromUtf8(pvt::layer_motion_path_name(path)), path);
    }
    motion_center_x_ = real_editor(-kMaximumRenderParameter,
                                   kMaximumRenderParameter, 4, 0.01);
    motion_center_y_ = real_editor(-kMaximumRenderParameter,
                                   kMaximumRenderParameter, 4, 0.01);
    motion_travel_x_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    motion_travel_y_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    motion_cycles_x_ = integer_editor(kMinimumIntegerParameter,
                                      kMaximumIntegerParameter);
    motion_cycles_y_ = integer_editor(kMinimumIntegerParameter,
                                      kMaximumIntegerParameter);
    motion_phase_ = real_editor(-kMaximumRenderParameter,
                                kMaximumRenderParameter, 3, 1.0);
    motion_phase_->setSuffix(QChar(0x00b0));
    motion_rotations_ = integer_editor(kMinimumIntegerParameter,
                                       kMaximumIntegerParameter);
    motion_rotation_offset_ = real_editor(-kMaximumRenderParameter,
                                          kMaximumRenderParameter, 3, 1.0);
    motion_rotation_offset_->setSuffix(QChar(0x00b0));
    motion_rotation_offset_->setToolTip(tr(
        "Static rotation applied before per-loop rotation. This can turn a still layer without adding animation."));
    motion_scale_pulse_ = real_editor(0.0, kMaximumRenderParameter, 4, 0.01);
    motion_paths_edit_ = new QPushButton(tr("Edit reusable paths and bindings…"));
    motion_paths_edit_->setObjectName(QStringLiteral("motionPathsEditorButton"));
    motion_paths_edit_->setToolTip(tr(
        "Project-wide paths can drive this layer, individual waves, or effect centers. "
        "They remain editable while whole-layer motion is disabled."));
    motion->addRow(tr("Closed path"), motion_path_);
    motion->addRow(tr("Path center X"), motion_center_x_);
    motion->addRow(tr("Path center Y"), motion_center_y_);
    motion->addRow(tr("Horizontal travel"), motion_travel_x_);
    motion->addRow(tr("Vertical travel"), motion_travel_y_);
    motion->addRow(tr("Horizontal cycles"), motion_cycles_x_);
    motion->addRow(tr("Vertical cycles"), motion_cycles_y_);
    motion->addRow(tr("Starting phase"), motion_phase_);
    motion->addRow(tr("Rotations per loop"), motion_rotations_);
    motion->addRow(tr("Starting rotation"), motion_rotation_offset_);
    motion->addRow(tr("Scale pulse"), motion_scale_pulse_);
    motion_group_->setToolTip(
        tr("A lightweight path animator. Integer cycles, rotations, and scale "
           "pulses close exactly at the project loop seam."));
    motion_layout->addWidget(motion_group_);

    auto* reusable_paths_group = new QGroupBox(tr("Reusable motion paths"));
    auto* reusable_paths_layout = new QVBoxLayout(reusable_paths_group);
    auto* reusable_paths_help = new QLabel(tr(
        "Edit shared closed cubic paths and bind them to the whole layer, a wave, "
        "or an effect center. This library is independent of the whole-layer "
        "motion switch above."));
    reusable_paths_help->setWordWrap(true);
    reusable_paths_layout->addWidget(reusable_paths_help);
    reusable_paths_layout->addWidget(motion_paths_edit_, 0, Qt::AlignLeft);
    motion_layout->addWidget(reusable_paths_group);

    auto* palette_group = new QGroupBox(tr("Starting palette"));
    auto* palette_layout = new QVBoxLayout(palette_group);
    auto* palette_form = new QFormLayout;
    palette_enabled_ = new QCheckBox(tr("Use this palette for starting colors"));
    palette_name_ = new QLineEdit;
    palette_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    palette_name_->setValidator(new Utf8TextValidator(TextRule::Name, palette_name_));
    auto* preset_row = new QWidget;
    auto* preset_layout = new QHBoxLayout(preset_row);
    preset_layout->setContentsMargins(0, 0, 0, 0);
    palette_preset_ = new QComboBox;
    for (std::size_t index = 0U; index < pvt::kBuiltInPaletteCount; ++index) {
        const pvt::PaletteConfig preset = pvt::default_palette(index);
        palette_preset_->addItem(QString::fromStdString(preset.name),
                                 static_cast<qulonglong>(index));
    }
    auto* apply_preset = new QPushButton(tr("Use preset"));
    preset_layout->addWidget(palette_preset_, 1);
    preset_layout->addWidget(apply_preset);
    palette_form->addRow(palette_enabled_);
    palette_form->addRow(tr("Palette name"), palette_name_);
    palette_form->addRow(tr("Built-in palettes"), preset_row);
    palette_layout->addLayout(palette_form);
    palette_colors_ = new QListWidget;
    palette_colors_->setAlternatingRowColors(true);
    palette_colors_->setSelectionMode(QAbstractItemView::SingleSelection);
    palette_colors_->setMaximumHeight(150);
    palette_layout->addWidget(palette_colors_);
    auto* palette_buttons = new QHBoxLayout;
    auto* add_color = new QPushButton(tr("Add color…"));
    auto* edit_color = new QPushButton(tr("Edit color…"));
    auto* remove_color = new QPushButton(tr("Remove color"));
    auto* random_palette = new QPushButton(tr("Generate random…"));
    auto* save_palette = new QPushButton(tr("Save to Library…"));
    auto* load_palette = new QPushButton(tr("Load Reusable…"));
    auto* import_palette = new QPushButton(tr("Import File…"));
    auto* export_palette = new QPushButton(tr("Export As…"));
    palette_buttons->addWidget(add_color);
    palette_buttons->addWidget(edit_color);
    palette_buttons->addWidget(remove_color);
    palette_layout->addLayout(palette_buttons);
    auto* palette_library_buttons = new QHBoxLayout;
    palette_library_buttons->addWidget(random_palette);
    palette_library_buttons->addWidget(save_palette);
    palette_library_buttons->addWidget(load_palette);
    palette_library_buttons->addStretch();
    palette_layout->addLayout(palette_library_buttons);
    auto* palette_interchange_buttons = new QHBoxLayout;
    palette_interchange_buttons->addWidget(import_palette);
    palette_interchange_buttons->addWidget(export_palette);
    palette_interchange_buttons->addStretch();
    palette_layout->addLayout(palette_interchange_buttons);
    auto* palette_help = new QLabel(
        tr("Colors are embedded in this layer. Ordinary colors use sRGBA; imported "
           "linear/HDR entries retain their source encoding and exact finite values. "
           "The renderer selects the procedural starting colors from this palette. Lighting and "
           "effects may create other colors afterward. Use Post-effects "
           "color quantization below when you want to deliberately reduce final colors. "
           "File import/export supports GIMP GPL, Krita KPL, GIMP-style code/text, "
           "and PNG/EXR palette images."));
    palette_help->setWordWrap(true);
    palette_layout->addWidget(palette_help);
    source_layout->addWidget(palette_group);
    wave_page_->setObjectName(QStringLiteral("movementWavesEditor"));
    motion_layout->insertWidget(2, wave_page_);

    auto* surface_group = new QGroupBox(tr("3D surface wrapping"));
    auto* surface = new QFormLayout(surface_group);
    surface_enabled_ = new QCheckBox(tr("Surface mapping enabled"));
    surface_mapping_ = new QComboBox;
    add_enum_item(surface_mapping_, tr("Plane"), pvt::SurfaceMapping::Plane);
    add_enum_item(surface_mapping_, tr("Cylinder"), pvt::SurfaceMapping::Cylinder);
    add_enum_item(surface_mapping_, tr("Sphere"), pvt::SurfaceMapping::Sphere);
    add_enum_item(surface_mapping_, tr("Cube"), pvt::SurfaceMapping::Cube);
    add_enum_item(surface_mapping_, tr("Custom OBJ"), pvt::SurfaceMapping::CustomObj);
    surface_obj_row_ = new QWidget;
    auto* obj_path_layout = new QHBoxLayout(surface_obj_row_);
    obj_path_layout->setContentsMargins(0, 0, 0, 0);
    surface_obj_path_ = new QLineEdit;
    surface_obj_path_->setMaxLength(static_cast<int>(kMaximumPathBytes));
    surface_obj_path_->setValidator(
        new Utf8TextValidator(TextRule::OptionalPath, surface_obj_path_));
    surface_obj_path_->setPlaceholderText(tr("Path to a Wavefront .obj file"));
    surface_obj_path_->setToolTip(
        tr("Custom OBJ surfaces use authored texture coordinates when present, "
           "or automatic box projection otherwise. Relative paths start from "
           "the application's working directory."));
    surface_obj_browse_ = new QPushButton(tr("Browse…"));
    obj_path_layout->addWidget(surface_obj_path_, 1);
    obj_path_layout->addWidget(surface_obj_browse_);
    surface_rotations_ = integer_editor(kMinimumIntegerParameter,
                                        kMaximumIntegerParameter);
    surface_phase_ = real_editor(-kMaximumRenderParameter,
                                 kMaximumRenderParameter, 3, 1.0);
    surface_curvature_ = real_editor(0.0, 1.0);
    surface_lighting_ = real_editor(0.0, kMaximumRenderParameter);
    surface->addRow(surface_enabled_);
    surface->addRow(tr("Surface"), surface_mapping_);
    surface->addRow(tr("OBJ file"), surface_obj_row_);
    surface_obj_label_ = surface->labelForField(surface_obj_row_);
    surface->addRow(tr("Rotations per loop"), surface_rotations_);
    surface->addRow(tr("Starting phase (degrees)"), surface_phase_);
    surface->addRow(tr("Curvature"), surface_curvature_);
    surface->addRow(tr("Lighting"), surface_lighting_);

    surface_plane_displacement_group_ = new QGroupBox(
        tr("Plane displacement mesh"));
    surface_plane_displacement_group_->setObjectName(
        QStringLiteral("planeDisplacementGroup"));
    surface_plane_displacement_group_->setToolTip(tr(
        "Builds a cached subdivided plane at the current render resolution. "
        "Preview/live monitoring uses its lower working resolution; frame, "
        "video, and full-resolution live output build the corresponding full grid."));
    auto* plane_displacement = new QFormLayout(
        surface_plane_displacement_group_);
    surface_plane_displacement_enabled_ = new QCheckBox(
        tr("Displace Plane with height map"));
    surface_plane_displacement_enabled_->setObjectName(
        QStringLiteral("planeDisplacementEnabled"));
    auto* height_row = new QWidget;
    auto* height_layout = new QHBoxLayout(height_row);
    height_layout->setContentsMargins(0, 0, 0, 0);
    surface_plane_displacement_path_ = new QLineEdit;
    surface_plane_displacement_path_->setObjectName(
        QStringLiteral("planeDisplacementPath"));
    surface_plane_displacement_path_->setReadOnly(true);
    surface_plane_displacement_path_->setPlaceholderText(
        tr("Choose a high-precision PNG or OpenEXR height map"));
    surface_plane_displacement_browse_ = new QPushButton(tr("Choose…"));
    surface_plane_displacement_clear_ = new QPushButton(tr("Clear"));
    height_layout->addWidget(surface_plane_displacement_path_, 1);
    height_layout->addWidget(surface_plane_displacement_browse_);
    height_layout->addWidget(surface_plane_displacement_clear_);
    surface_plane_displacement_minimum_ = real_editor(
        -kMaximumRenderParameter, kMaximumRenderParameter, 4, 0.01);
    surface_plane_displacement_maximum_ = real_editor(
        -kMaximumRenderParameter, kMaximumRenderParameter, 4, 0.01);
    surface_plane_displacement_midpoint_ = real_editor(0.0, 1.0, 4, 0.01);
    surface_plane_displacement_ratio_ = integer_editor(
        1, (std::numeric_limits<int>::max)());
    surface_plane_displacement_ratio_->setSuffix(tr(" px/node"));
    surface_plane_displacement_ratio_->setToolTip(tr(
        "1 creates one vertex per render pixel. Larger values create fewer "
        "vertices and triangles. Both outer edges are always retained."));
    surface_plane_displacement_export_ = new QPushButton(
        tr("Export output-resolution OBJ…"));
    surface_plane_displacement_export_->setToolTip(tr(
        "Generate the same mesh used at the authored output resolution and "
        "save it as a Wavefront OBJ with UVs and smooth normals."));
    auto* displacement_help = new QLabel(tr(
        "PNG sample codes and OpenEXR samples are treated as linear height data. "
        "Single-channel EXR files are supported. The midpoint is the neutral "
        "(zero-height) sample; values below/above it interpolate to "
        "the signed minimum/maximum. A changed map or setting rebuilds the "
        "cached plane automatically."));
    displacement_help->setWordWrap(true);
    plane_displacement->addRow(surface_plane_displacement_enabled_);
    plane_displacement->addRow(tr("Height map"), height_row);
    plane_displacement->addRow(tr("Minimum displacement"),
                               surface_plane_displacement_minimum_);
    plane_displacement->addRow(tr("Maximum displacement"),
                               surface_plane_displacement_maximum_);
    plane_displacement->addRow(tr("Neutral midpoint"),
                               surface_plane_displacement_midpoint_);
    plane_displacement->addRow(tr("Pixel-to-node ratio"),
                               surface_plane_displacement_ratio_);
    plane_displacement->addRow(surface_plane_displacement_export_);
    plane_displacement->addRow(displacement_help);
    surface->addRow(surface_plane_displacement_group_);
    surface_layout->addWidget(surface_group);

    auto* post_process_group = new QGroupBox(
        tr("Inversion and edge antialiasing"));
    post_process_group->setObjectName(QStringLiteral("postProcessGroup"));
    post_process_group->setToolTip(tr(
        "Layer-local finishing effects. Inversion runs first, followed by "
        "alpha-aware edge antialiasing and then color quantization."));
    auto* post_process = new QFormLayout(post_process_group);
    post_invert_rgb_enabled_ = new QCheckBox(tr("Invert colors"));
    post_invert_rgb_enabled_->setObjectName(
        QStringLiteral("postInvertColors"));
    post_invert_rgb_enabled_->setToolTip(tr(
        "Invert each linear-light RGB channel around reference white. HDR and "
        "out-of-range working values remain unclamped."));
    post_invert_rgb_mix_ = real_editor(0.0, 1.0, 4, 0.01);
    post_invert_rgb_mix_->setObjectName(
        QStringLiteral("postInvertColorMix"));
    post_invert_rgb_mix_->setToolTip(tr(
        "Crossfade from the original color at 0 to fully inverted color at 1."));
    post_invert_alpha_enabled_ = new QCheckBox(tr("Invert alpha"));
    post_invert_alpha_enabled_->setObjectName(
        QStringLiteral("postInvertAlpha"));
    post_invert_alpha_enabled_->setToolTip(tr(
        "Invert layer transparency independently from its RGB channels."));
    post_invert_alpha_mix_ = real_editor(0.0, 1.0, 4, 0.01);
    post_invert_alpha_mix_->setObjectName(
        QStringLiteral("postInvertAlphaMix"));
    post_invert_alpha_mix_->setToolTip(tr(
        "Crossfade from the original alpha at 0 to fully inverted alpha at 1."));
    post_antialias_enabled_ = new QCheckBox(tr("Edge antialiasing"));
    post_antialias_enabled_->setObjectName(
        QStringLiteral("postEdgeAntialiasing"));
    post_antialias_enabled_->setToolTip(tr(
        "Smooth high-contrast edges in premultiplied-alpha space so hidden RGB "
        "cannot bleed through transparent pixels."));
    post_antialias_strength_ = real_editor(0.0, 1.0, 4, 0.01);
    post_antialias_strength_->setObjectName(
        QStringLiteral("postAntialiasStrength"));
    post_antialias_strength_->setToolTip(tr(
        "Amount of edge-aware neighborhood smoothing."));
    post_antialias_threshold_ = real_editor(0.0, 1.0, 4, 0.01);
    post_antialias_threshold_->setObjectName(
        QStringLiteral("postAntialiasThreshold"));
    post_antialias_threshold_->setToolTip(tr(
        "Minimum local contrast treated as an edge. Lower values smooth more pixels."));
    post_antialias_passes_ = integer_editor(1, kMaximumIntegerParameter);
    post_antialias_passes_->setObjectName(
        QStringLiteral("postAntialiasPasses"));
    post_antialias_passes_->setToolTip(tr(
        "Repeat the antialias pass for stronger smoothing. Additional passes cost frame time."));
    post_process->addRow(post_invert_rgb_enabled_);
    post_process->addRow(tr("Color invert mix"), post_invert_rgb_mix_);
    post_process->addRow(post_invert_alpha_enabled_);
    post_process->addRow(tr("Alpha invert mix"), post_invert_alpha_mix_);
    post_process->addRow(post_antialias_enabled_);
    post_process->addRow(tr("Strength"), post_antialias_strength_);
    post_process->addRow(tr("Edge threshold"), post_antialias_threshold_);
    post_process->addRow(tr("Passes"), post_antialias_passes_);
    finish_layout->addWidget(post_process_group);

    auto* quantization_group = new QGroupBox(tr("Post-effects color quantization"));
    auto* quantization = new QFormLayout(quantization_group);
    quantization_enabled_ = new QCheckBox(tr("Quantization enabled"));
    quantization_levels_ = integer_editor(2, kMaximumIntegerParameter);
    quantization_mix_ = real_editor(0.0, 1.0);
    quantization_mode_ = new QComboBox;
    add_enum_item(quantization_mode_, tr("RGB"), pvt::QuantizationMode::Rgb);
    add_enum_item(quantization_mode_, tr("Luminance"), pvt::QuantizationMode::Luminance);
    add_enum_item(quantization_mode_, tr("Hue"), pvt::QuantizationMode::Hue);
    quantization->addRow(quantization_enabled_);
    quantization->addRow(tr("Levels"), quantization_levels_);
    quantization->addRow(tr("Mix"), quantization_mix_);
    quantization->addRow(tr("Mode"), quantization_mode_);
    finish_layout->addWidget(quantization_group);

    auto* alpha_group = new QGroupBox(tr("Alpha channel"));
    auto* alpha = new QFormLayout(alpha_group);
    alpha_enabled_ = new QCheckBox(tr("Procedural alpha modulation"));
    alpha_enabled_->setToolTip(
        tr("Controls opacity generated by this layer. The project output channel "
           "selection is configured separately in Export."));
    alpha_minimum_ = real_editor(0.0, 1.0);
    alpha_maximum_ = real_editor(0.0, 1.0);
    alpha_frequency_ = real_editor(0.0, kMaximumRenderParameter);
    alpha_cycles_ = integer_editor(kMinimumIntegerParameter,
                                    kMaximumIntegerParameter);
    alpha_phase_ = real_editor(-kMaximumRenderParameter,
                               kMaximumRenderParameter, 3, 1.0);
    alpha_use_source_ = new QCheckBox(
        tr("Use alpha stored in starting palettes and image pixels"));
    alpha_use_source_->setToolTip(tr(
        "This does not change generated alpha or layer opacity. Turning it off "
        "ignores palette and image alpha non-destructively; those authored values "
        "remain available when re-enabled."));
    alpha->addRow(alpha_use_source_);
    alpha->addRow(alpha_enabled_);
    alpha->addRow(tr("Minimum"), alpha_minimum_);
    alpha->addRow(tr("Maximum"), alpha_maximum_);
    alpha->addRow(tr("Spatial frequency"), alpha_frequency_);
    alpha->addRow(tr("Cycles per loop"), alpha_cycles_);
    alpha->addRow(tr("Starting phase (degrees)"), alpha_phase_);
    // Source/procedural alpha is resolved while the base image is created,
    // but artist-facing navigation treats it as a layer modifier rather than
    // burying it inside the starting-color chooser.
    surface_layout->addWidget(alpha_group);

    source_layout->addStretch();
    surface_layout->addStretch();
    motion_layout->addStretch();
    finish_layout->addStretch();

    connect(surface_obj_browse_, &QPushButton::clicked, this, [this] {
        std::vector<const pvt::LayerConfig*> reusable;
        QStringList labels;
        for (const auto& layer : project_.layers) {
            if (layer.uuid == active_layer_uuid_ || layer.render.surface.obj_sha256.empty()) {
                continue;
            }
            reusable.push_back(&layer);
            labels.push_back(tr("Project layer: %1 — %2")
                                 .arg(QString::fromStdString(layer.name),
                                      QString::fromStdString(layer.render.surface.obj_basename)));
        }
        if (!reusable.empty()) {
            labels.push_back(tr("Load a different OBJ from disk…"));
            bool accepted = false;
            const QString selection = QInputDialog::getItem(
                this, tr("Choose custom OBJ asset"), tr("Matching project assets"),
                labels, 0, false, &accepted);
            if (!accepted) return;
            const qsizetype selected = labels.indexOf(selection);
            if (selected >= 0
                && static_cast<std::size_t>(selected) < reusable.size()) {
                if (document_ == nullptr) return;
                auto before = captureActiveState();
                const auto& source = *reusable[static_cast<std::size_t>(selected)];
                QString alias_error;
                const std::string target_reference =
                    pvt::surface_obj_attachment_id(active_layer_uuid_);
                if (!alias_project_attachment(
                        *document_, pvt::surface_obj_attachment_id(source.uuid),
                        target_reference, &alias_error)) {
                    QMessageBox::critical(this, tr("Could not reuse OBJ"), alias_error);
                    return;
                }
                config_.surface.obj_sha256 = source.render.surface.obj_sha256;
                config_.surface.obj_basename = source.render.surface.obj_basename;
                config_.surface.obj_path =
                    pvt::project_attachment_path(*document_, target_reference);
                config_.surface.mapping = pvt::SurfaceMapping::CustomObj;
                config_.surface.enabled = true;
                syncActiveRender();
                syncProjectGlobals();
                document_->project = project_;
                loadGlobalEditors();
                schedulePreview();
                recordActiveStateChange(tr("Reuse embedded custom OBJ"),
                                        std::move(before));
                status_->setText(tr("Reused %1 without duplicating its bytes.")
                                     .arg(QString::fromStdString(
                                         config_.surface.obj_basename)));
                return;
            }
        }
        QString preferred;
        if (!surface_obj_path_->text().isEmpty()) {
            const QString absolute = QDir::isAbsolutePath(surface_obj_path_->text())
                                         ? surface_obj_path_->text()
                                         : QDir(startup_working_directory_)
                                               .absoluteFilePath(surface_obj_path_->text());
            preferred = QFileInfo(absolute).absolutePath();
        }
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Choose custom OBJ mesh"), usableDialogDirectory(preferred),
            tr("Wavefront OBJ (*.obj);;All files (*)"));
        if (!selected.isEmpty()) {
            rememberDialogLocation(selected);
            if (!config_.surface.obj_sha256.empty()
                && QMessageBox::question(
                       this, tr("Replace embedded OBJ?"),
                       tr("Replace the active layer's current internal OBJ binding with %1?")
                           .arg(QFileInfo(selected).fileName()),
                       QMessageBox::Yes | QMessageBox::No,
                       QMessageBox::Yes) != QMessageBox::Yes) {
                return;
            }
            (void)setSurfaceObjSource(selected);
        }
    });
    connect(surface_plane_displacement_browse_, &QPushButton::clicked,
            this, [this] {
        QString preferred;
        const auto& displacement = config_.surface.plane_displacement;
        if (!displacement.path.empty()) {
            preferred = QFileInfo(
                QString::fromStdString(displacement.path)).absolutePath();
        }
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Choose plane displacement height map"),
            usableDialogDirectory(preferred),
            tr("PNG / OpenEXR height maps (*.png *.exr);;All files (*)"));
        if (!selected.isEmpty()) {
            rememberDialogLocation(selected);
            if (!displacement.sha256.empty()
                && QMessageBox::question(
                       this, tr("Replace embedded height map?"),
                       tr("Replace the active layer's plane-displacement map with %1?")
                           .arg(QFileInfo(selected).fileName()),
                       QMessageBox::Yes | QMessageBox::No,
                       QMessageBox::Yes) != QMessageBox::Yes) {
                return;
            }
            (void)setPlaneDisplacementSource(selected);
        }
    });
    connect(surface_plane_displacement_clear_, &QPushButton::clicked,
            this, [this] { (void)setPlaneDisplacementSource({}); });
    connect(surface_plane_displacement_export_, &QPushButton::clicked,
            this, &MainWindow::exportPlaneDisplacementObj);
    connect(starting_image_browse_, &QPushButton::clicked, this, [this] {
        std::vector<const pvt::LayerConfig*> reusable;
        QStringList labels;
        for (const auto& layer : project_.layers) {
            if (layer.uuid == active_layer_uuid_
                || layer.render.starting_image.sha256.empty()) {
                continue;
            }
            reusable.push_back(&layer);
            labels.push_back(tr("Project layer: %1 — %2")
                                 .arg(QString::fromStdString(layer.name),
                                      QString::fromStdString(
                                          layer.render.starting_image.basename)));
        }
        if (!reusable.empty()) {
            labels.push_back(tr("Load a different PNG or OpenEXR from disk…"));
            bool accepted = false;
            const QString selection = QInputDialog::getItem(
                this, tr("Choose starting image asset"),
                tr("Matching project assets"), labels, 0, false, &accepted);
            if (!accepted) return;
            const qsizetype selected = labels.indexOf(selection);
            if (selected >= 0
                && static_cast<std::size_t>(selected) < reusable.size()) {
                if (document_ == nullptr) return;
                auto before = captureActiveState();
                const auto& source = *reusable[static_cast<std::size_t>(selected)];
                QString alias_error;
                const std::string target_reference =
                    pvt::starting_image_attachment_id(active_layer_uuid_);
                if (!alias_project_attachment(
                        *document_, pvt::starting_image_attachment_id(source.uuid),
                        target_reference, &alias_error)) {
                    QMessageBox::critical(this, tr("Could not reuse image"), alias_error);
                    return;
                }
                config_.starting_image.enabled = true;
                config_.starting_image.sha256 = source.render.starting_image.sha256;
                config_.starting_image.basename = source.render.starting_image.basename;
                config_.starting_image.path =
                    pvt::project_attachment_path(*document_, target_reference);
                syncActiveRender();
                syncProjectGlobals();
                document_->project = project_;
                loadGlobalEditors();
                schedulePreview();
                recordActiveStateChange(tr("Reuse embedded starting image"),
                                        std::move(before));
                status_->setText(tr("Reused %1 without duplicating its bytes.")
                                     .arg(QString::fromStdString(
                                         config_.starting_image.basename)));
                return;
            }
        }
        const QString selected = QFileDialog::getOpenFileName(
            this, tr("Choose starting image"), usableDialogDirectory(),
            tr("High-precision image (*.png *.exr)"));
        if (!selected.isEmpty()) {
            rememberDialogLocation(selected);
            if (!config_.starting_image.sha256.empty()
                && QMessageBox::question(
                       this, tr("Replace embedded starting image?"),
                       tr("Replace the active layer's current internal image binding with %1?")
                           .arg(QFileInfo(selected).fileName()),
                       QMessageBox::Yes | QMessageBox::No,
                       QMessageBox::Yes) != QMessageBox::Yes) {
                return;
            }
            (void)setStartingImageSource(selected);
        }
    });
    connect(starting_image_clear_, &QPushButton::clicked, this, [this] {
        (void)setStartingImageSource({});
    });
    connect(apply_preset, &QPushButton::clicked, this, [this] {
        applyPalettePreset(static_cast<std::size_t>(
            palette_preset_->currentData().toULongLong()));
    });
    connect(add_color, &QPushButton::clicked,
            this, &MainWindow::addPaletteColor);
    connect(edit_color, &QPushButton::clicked,
            this, &MainWindow::editSelectedPaletteColor);
    connect(remove_color, &QPushButton::clicked,
            this, &MainWindow::removeSelectedPaletteColor);
    connect(random_palette, &QPushButton::clicked,
            this, &MainWindow::generateRandomPalette);
    connect(save_palette, &QPushButton::clicked,
            this, &MainWindow::savePaletteToLibrary);
    connect(load_palette, &QPushButton::clicked,
            this, &MainWindow::loadPaletteFromLibraryOrLayer);
    connect(import_palette, &QPushButton::clicked,
            this, &MainWindow::importPaletteFile);
    connect(export_palette, &QPushButton::clicked,
            this, &MainWindow::exportPaletteFile);
    connect(palette_colors_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { editSelectedPaletteColor(); });

    return source_page_;
}

QWidget* MainWindow::createOutputPage() {
    const auto make_project_page = [](QWidget*& page) {
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto* contents = new QWidget;
        auto* layout = new QVBoxLayout(contents);
        scroll->setWidget(contents);
        page = scroll;
        return layout;
    };
    auto* canvas_layout = make_project_page(project_canvas_page_);
    auto* export_layout = make_project_page(project_export_page_);
    project_canvas_page_->setObjectName(QStringLiteral("projectCanvasLoopPage"));
    project_export_page_->setObjectName(QStringLiteral("projectExportPage"));

    auto* canvas_intro = new QLabel(tr(
        "Project-wide image dimensions, block resolution, frame count, and playback rate. These values define the timeline shared by every layer."));
    canvas_intro->setWordWrap(true);
    canvas_layout->addWidget(canvas_intro);
    auto* export_intro = new QLabel(tr(
        "Everything needed to understand or start an export is collected here. Canvas size and frame timing are summarized below and remain editable in Project; encoding, destination, and file naming are editable on this page."));
    export_intro->setWordWrap(true);
    export_layout->addWidget(export_intro);

    auto* canvas_group = new QGroupBox(tr("Canvas and loop"));
    auto* canvas = new QFormLayout(canvas_group);
    width_ = integer_editor(16, (std::numeric_limits<int>::max)());
    height_ = integer_editor(16, (std::numeric_limits<int>::max)());
    block_size_ = integer_editor(1, (std::numeric_limits<int>::max)());
    frames_ = integer_editor(2, (std::numeric_limits<int>::max)());
    frames_->setObjectName(QStringLiteral("manualFrameCount"));
    fps_ = real_editor(
        kMinimumPositiveUiValue, kMaximumRenderParameter, 6, 1.0);
    effective_frames_ = new QLabel;
    effective_frames_->setObjectName(QStringLiteral("effectiveFrameCount"));
    effective_frames_->setWordWrap(true);
    canvas->addRow(tr("Width"), width_);
    canvas->addRow(tr("Height"), height_);
    canvas->addRow(tr("Block size"), block_size_);
    canvas->addRow(tr("Manual frames"), frames_);
    canvas->addRow(tr("Effective duration"), effective_frames_);
    canvas->addRow(tr("Playback FPS"), fps_);
    canvas_layout->addWidget(canvas_group);

    auto* export_canvas_group = new QGroupBox(tr("Canvas and timeline"));
    auto* export_canvas_layout = new QVBoxLayout(export_canvas_group);
    export_canvas_summary_ = new QLabel;
    export_canvas_summary_->setObjectName(QStringLiteral("exportCanvasSummary"));
    export_canvas_summary_->setWordWrap(true);
    export_canvas_layout->addWidget(export_canvas_summary_);
    auto* edit_canvas = new QPushButton(tr("Edit Canvas && Timeline…"));
    edit_canvas->setToolTip(
        tr("Open project-wide width, height, block size, frame count, and playback rate."));
    export_canvas_layout->addWidget(edit_canvas, 0, Qt::AlignLeft);
    export_layout->addWidget(export_canvas_group);

    auto* output_group = new QGroupBox(tr("Export"));
    auto* output = new QFormLayout(output_group);
    bit_depth_ = new QComboBox;
    bit_depth_->addItem(tr("8-bit PNG"), 8);
    bit_depth_->addItem(tr("16-bit PNG"), 16);
    bit_depth_->addItem(tr("32-bit float EXR"), 32);
    png_compression_ = integer_editor(0, 9);
    png_compression_->setToolTip(
        tr("PNG compression from 0 (fastest) to 9 (smallest files). Ignored for EXR."));
    dither_enabled_ = new QCheckBox(tr("Dither integer output"));
    dither_enabled_->setToolTip(
        tr("Float EXR never uses dithering. The integer-output preference is preserved."));
    dither_method_ = new QComboBox;
    add_enum_item(dither_method_, tr("Deterministic blue-noise-like"),
                  pvt::DitherMethod::BlueNoise);
    add_enum_item(dither_method_, tr("Ordered Bayer"), pvt::DitherMethod::OrderedBayer);
    add_enum_item(dither_method_, tr("Floyd-Steinberg"),
                  pvt::DitherMethod::FloydSteinberg);
    write_alpha_ = new QCheckBox(tr("Write final alpha channel"));
    write_alpha_->setToolTip(
        tr("Project-global RGB/RGBA selection. Automatically enabled when layering or "
           "render data can produce transparency."));

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
    first_frame_ = integer_editor(0, (std::numeric_limits<int>::max)());
    filename_digits_ = integer_editor(
        1, static_cast<int>(pvt::kMaximumOutputFilenameBytes));
    overwrite_ = new QCheckBox(tr("Overwrite existing output"));
    output->addRow(tr("Bit depth"), bit_depth_);
    output->addRow(tr("PNG compression (0 off, 9 max)"), png_compression_);
    output->addRow(dither_enabled_);
    output->addRow(tr("Dither method"), dither_method_);
    output->addRow(write_alpha_);
    output->addRow(tr("Directory"), directory_row);
    output->addRow(tr("Filename prefix"), prefix_);
    output->addRow(tr("First frame number"), first_frame_);
    output->addRow(tr("Minimum number digits"), filename_digits_);
    output->addRow(overwrite_);
    export_layout->addWidget(output_group);

    auto* live_preview_group = new QGroupBox(tr("Live Preview Output"));
    live_preview_group->setObjectName(QStringLiteral("livePreviewOutputGroup"));
    auto* live_preview_layout = new QVBoxLayout(live_preview_group);
    auto* live_preview_intro = new QLabel(tr(
        "Present the editor preview on another window or display without starting microphone, MIDI, OSC, scenes, routed clocks, or the LIVE performance workspace."));
    live_preview_intro->setWordWrap(true);
    live_preview_layout->addWidget(live_preview_intro);
    auto* live_preview_form = new QFormLayout;
    live_preview_screen_ = new QComboBox;
    live_preview_screen_->setObjectName(
        QStringLiteral("livePreviewOutputDisplay"));
    live_preview_quality_ = new QComboBox;
    live_preview_quality_->setObjectName(
        QStringLiteral("livePreviewOutputQuality"));
    live_preview_quality_->addItem(tr("Auto · frame-budget managed"), 0.0);
    live_preview_quality_->addItem(tr("Full resolution"), 1.0);
    live_preview_quality_->addItem(tr("75%"), 0.75);
    live_preview_quality_->addItem(tr("50%"), 0.5);
    live_preview_quality_->addItem(tr("25%"), 0.25);
    live_preview_fullscreen_ = new QCheckBox(tr("Full-screen output"));
    live_preview_fullscreen_->setObjectName(
        QStringLiteral("livePreviewOutputFullscreen"));
    live_preview_hide_cursor_ = new QCheckBox(tr("Hide pointer over output"));
    live_preview_hide_cursor_->setObjectName(
        QStringLiteral("livePreviewOutputHideCursor"));
    live_preview_form->addRow(tr("Display"), live_preview_screen_);
    live_preview_form->addRow(tr("Render quality"), live_preview_quality_);
    live_preview_form->addRow({}, live_preview_fullscreen_);
    live_preview_form->addRow({}, live_preview_hide_cursor_);
    live_preview_layout->addLayout(live_preview_form);
    live_preview_output_status_ = new QLabel(tr(
        "Stopped — start this to present the editor preview without entering LIVE."));
    live_preview_output_status_->setObjectName(
        QStringLiteral("livePreviewOutputStatus"));
    live_preview_output_status_->setWordWrap(true);
    live_preview_layout->addWidget(live_preview_output_status_);
    live_preview_output_button_ = new QPushButton(tr("Start Live Preview Output"));
    live_preview_output_button_->setObjectName(
        QStringLiteral("livePreviewOutputButton"));
    live_preview_layout->addWidget(live_preview_output_button_, 0, Qt::AlignLeft);
    export_layout->addWidget(live_preview_group);

    auto* export_actions = new QGroupBox(tr("Start export"));
    auto* export_actions_layout = new QHBoxLayout(export_actions);
    auto* export_frames = new QPushButton(tr("Export Frames…"));
    auto* export_video = new QPushButton(tr("Export Video…"));
    export_actions_layout->addWidget(export_frames);
    export_actions_layout->addWidget(export_video);
    export_actions_layout->addStretch();
    export_layout->addWidget(export_actions);
    canvas_layout->addStretch();
    export_layout->addStretch();

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString selected = QFileDialog::getExistingDirectory(
            this, tr("Choose export directory"),
            usableDialogDirectory(resolvedOutputDirectory(output_directory_->text())));
        if (!selected.isEmpty()) {
            rememberDialogLocation(selected);
            output_directory_->setText(selected);
            updateOutputEditorValidity();
            applyGlobalEditor(output_directory_);
        }
    });
    connect(edit_canvas, &QPushButton::clicked, this, [this] {
        showProjectSettingsPage(project_canvas_page_, tr("Canvas & Loop"));
    });
    connect(export_frames, &QPushButton::clicked, this, [this] {
        if (export_action_ != nullptr) export_action_->trigger();
    });
    connect(export_video, &QPushButton::clicked, this, [this] {
        if (video_export_action_ != nullptr) video_export_action_->trigger();
    });
    connect(live_preview_output_button_, &QPushButton::clicked, this, [this] {
        if (live_preview_output_action_ != nullptr) {
            live_preview_output_action_->trigger();
        }
    });
    connect(live_preview_screen_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (live_workspace_ != nullptr) {
                    live_workspace_->setSelectedOutputDisplayId(
                        live_preview_screen_->currentData().toString());
                }
            });
    connect(live_preview_quality_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (live_workspace_ != nullptr) {
                    live_workspace_->setOutputResolutionScale(
                        live_preview_quality_->currentData().toDouble());
                }
            });
    connect(live_preview_fullscreen_, &QCheckBox::toggled, this,
            [this](bool checked) {
                if (live_workspace_ != nullptr) {
                    live_workspace_->setPresentationFullscreen(checked);
                }
            });
    connect(live_preview_hide_cursor_, &QCheckBox::toggled, this,
            [this](bool checked) {
                if (live_workspace_ != nullptr) {
                    live_workspace_->setPresentationHideCursor(checked);
                }
            });
    return project_canvas_page_;
}

QWidget* MainWindow::createVersionsPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    version_summary_ = new QLabel;
    version_summary_->setWordWrap(true);
    layout->addWidget(version_summary_);

    version_list_ = new QListWidget;
    version_list_->setAlternatingRowColors(true);
    version_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(version_list_, 1);

    auto* actions = new QHBoxLayout;
    version_make_current_ = new QPushButton(tr("Make Current"));
    version_revert_ = new QPushButton(tr("Revert as New Version"));
    version_make_current_->setToolTip(
        tr("Point the bundle at this immutable version without deleting later versions."));
    version_revert_->setToolTip(
        tr("Copy this snapshot into a new highest-numbered version, preserving rollback."));
    actions->addWidget(version_make_current_);
    actions->addWidget(version_revert_);
    layout->addLayout(actions);

    auto* compare_group = new QGroupBox(tr("Semantic version diff"));
    auto* compare_layout = new QVBoxLayout(compare_group);
    auto* selectors = new QHBoxLayout;
    version_before_ = new QComboBox;
    version_after_ = new QComboBox;
    selectors->addWidget(new QLabel(tr("From")));
    selectors->addWidget(version_before_, 1);
    selectors->addWidget(new QLabel(tr("To")));
    selectors->addWidget(version_after_, 1);
    version_compare_ = new QPushButton(tr("Compare"));
    version_compare_->setToolTip(
        tr("Load and compare the selected snapshots in the background."));
    selectors->addWidget(version_compare_);
    compare_layout->addLayout(selectors);
    version_diff_ = new QPlainTextEdit;
    version_diff_->setReadOnly(true);
    version_diff_->setPlaceholderText(
        tr("Save at least two versions to compare project, output, layer, and render fields."));
    compare_layout->addWidget(version_diff_, 1);
    layout->addWidget(compare_group, 1);

    connect(version_compare_, &QPushButton::clicked,
            this, &MainWindow::startVersionDiff);
    const auto clear_stale_diff = [this] {
        if (version_diff_ != nullptr) {
            version_diff_->setPlainText(
                tr("Choose two versions and select Compare. Comparison runs in the background."));
        }
    };
    connect(version_before_, &QComboBox::currentIndexChanged,
            this, clear_stale_diff);
    connect(version_after_, &QComboBox::currentIndexChanged,
            this, clear_stale_diff);
    connect(version_make_current_, &QPushButton::clicked,
            this, &MainWindow::makeSelectedVersionCurrent);
    connect(version_revert_, &QPushButton::clicked,
            this, &MainWindow::revertSelectedVersion);
    connect(version_list_, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem*) { makeSelectedVersionCurrent(); });
    connect(version_list_, &QListWidget::currentRowChanged, this, [this] {
        const auto* item = version_list_->currentItem();
        const bool usable = item != nullptr && item->data(Qt::UserRole + 1).toBool();
        version_make_current_->setEnabled(usable);
        version_revert_->setEnabled(usable);
    });
    refreshVersionsPage();
    return page;
}

void MainWindow::refreshVersionsPage() {
    if (version_list_ == nullptr || document_ == nullptr) return;
    // Rebuilding hidden Versions controls must never navigate the user. Keep
    // the tab they selected even if a platform style or signal handler reacts
    // to the list/combo changes below.
    QWidget* const selected_page = tabs_ != nullptr ? tabs_->currentWidget()
                                                    : nullptr;
    const QVariant before_value = version_before_->currentData();
    const QVariant after_value = version_after_->currentData();
    const QSignalBlocker before_blocker(version_before_);
    const QSignalBlocker after_blocker(version_after_);
    version_list_->clear();
    version_before_->clear();
    version_after_->clear();
    int current_row = -1;
    int row = 0;
    for (auto iterator = document_->versions.rbegin();
         iterator != document_->versions.rend(); ++iterator, ++row) {
        const auto& version = *iterator;
        QString label = tr("Version %1 — %2 — %3 layer(s)")
                            .arg(version.number)
                            .arg(QString::fromStdString(version.saved_utc))
                            .arg(version.layer_count);
        if (version.number == document_->current_version) label.prepend(tr("CURRENT  "));
        if (!version.indexed) label.append(tr("  [unindexed version]"));
        if (!version.valid) {
            label.append(tr("  [integrity mismatch]"));
        } else if (version.externally_modified) {
            label.append(tr("  [external change / integrity mismatch]"));
        }
        if (!version.reason.empty()) {
            label.append(QStringLiteral("  — ") + QString::fromStdString(version.reason));
        }
        auto* item = new QListWidgetItem(label, version_list_);
        if (!version.integrity_message.empty()) {
            item->setToolTip(QString::fromStdString(version.integrity_message));
        }
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(version.number));
        item->setData(Qt::UserRole + 1, version.indexed && version.valid);
        const QString combo_label = tr("Version %1").arg(version.number);
        version_before_->addItem(combo_label, QVariant::fromValue<qulonglong>(version.number));
        version_after_->addItem(combo_label, QVariant::fromValue<qulonglong>(version.number));
        if (version.number == document_->current_version) current_row = row;
    }
    version_list_->setCurrentRow(current_row);
    const auto* current_item = version_list_->currentItem();
    const bool usable_version = current_item != nullptr
                                && current_item->data(Qt::UserRole + 1).toBool();
    version_make_current_->setEnabled(usable_version);
    version_revert_->setEnabled(usable_version);
    const int old_before = version_before_->findData(before_value);
    const int old_after = version_after_->findData(after_value);
    version_before_->setCurrentIndex(old_before >= 0 ? old_before
                                                     : std::min(1, version_before_->count() - 1));
    version_after_->setCurrentIndex(old_after >= 0 ? old_after
                                                   : (version_after_->count() > 0 ? 0 : -1));
    const bool can_compare = version_before_->count() >= 2
                             && version_after_->count() >= 2;
    version_before_->setEnabled(can_compare);
    version_after_->setEnabled(can_compare);
    version_compare_->setEnabled(
        can_compare
        && (version_diff_watcher_ == nullptr
            || !version_diff_watcher_->isRunning()));
    const QString source = document_->source_path.empty()
                               ? tr("Not saved as a bundle yet")
                               : QString::fromStdString(document_->source_path);
    version_summary_->setText(
        tr("%1\nProject UUID: %2\n%3 saved version(s). Versions are immutable; "
           "revert always creates another version.")
            .arg(source, QString::fromStdString(project_.uuid),
                 QString::number(document_->versions.size())));
    if (!compatibility_warning_.isEmpty()) {
        version_summary_->setText(version_summary_->text()
                                  + QStringLiteral("\n\n⚠ ")
                                  + compatibility_warning_);
    }
    if (can_compare) {
        version_diff_->setPlainText(
            tr("Choose two versions and select Compare. Comparison runs in the background."));
    } else {
        version_diff_->clear();
    }
    if (tabs_ != nullptr && selected_page != nullptr
        && tabs_->currentWidget() != selected_page) {
        tabs_->setCurrentWidget(selected_page);
    }
}

void MainWindow::refreshVersionDiff() {
    if (version_diff_ == nullptr) {
        return;
    }
    if (document_ == nullptr || version_before_->currentIndex() < 0
        || version_after_->currentIndex() < 0) {
        version_diff_->clear();
        return;
    }
    const auto before = version_before_->currentData().toULongLong();
    const auto after = version_after_->currentData().toULongLong();
    if (before == after) {
        version_diff_->setPlainText(tr("The same version is selected on both sides."));
        return;
    }
    std::vector<pvt::BundleDiffEntry> differences;
    std::string error;
    if (!pvt::diff_project_versions(*document_, before, after, differences, &error)) {
        version_diff_->setPlainText(tr("Could not compare versions: %1")
                                        .arg(QString::fromStdString(error)));
        return;
    }
    if (differences.empty()) {
        version_diff_->setPlainText(tr("No semantic project differences."));
        return;
    }
    QStringList lines;
    lines.reserve(static_cast<qsizetype>(differences.size()));
    for (const auto& difference : differences) {
        lines.push_back(tr("%1\n  %2\n→ %3")
                            .arg(QString::fromStdString(difference.field),
                                 friendly_diff_value(difference.before),
                                 friendly_diff_value(difference.after)));
    }
    version_diff_->setPlainText(lines.join(QStringLiteral("\n\n")));
}

void MainWindow::startVersionDiff() {
    if (version_diff_ == nullptr || version_diff_watcher_ == nullptr
        || version_diff_watcher_->isRunning()) {
        return;
    }
    if (document_ == nullptr || version_before_->currentIndex() < 0
        || version_after_->currentIndex() < 0) {
        version_diff_->clear();
        return;
    }
    const auto before = version_before_->currentData().toULongLong();
    const auto after = version_after_->currentData().toULongLong();
    if (before == after) {
        version_diff_->setPlainText(tr("The same version is selected on both sides."));
        return;
    }
    const std::uint64_t revision = document_revision_;
    try {
        auto snapshot = std::make_shared<pvt::ProjectDocument>(*document_);
        version_diff_task_before_ = before;
        version_diff_task_after_ = after;
        version_diff_task_document_revision_ = revision;
        version_compare_->setEnabled(false);
        version_diff_->setPlainText(tr("Comparing versions in the background…"));
        version_diff_watcher_->setFuture(QtConcurrent::run(
            [snapshot = std::move(snapshot), before, after, revision] {
                VersionDiffResult result;
                result.before = before;
                result.after = after;
                result.document_revision = revision;
                try {
                    std::string error;
                    result.ok = pvt::diff_project_versions(
                        *snapshot, before, after, result.differences, &error);
                    result.error = QString::fromStdString(error);
                } catch (const std::exception& exception) {
                    result.error = tr("Unexpected version-comparison error: %1")
                                       .arg(QString::fromUtf8(exception.what()));
                } catch (...) {
                    result.error = tr(
                        "Version comparison failed because of an unexpected error.");
                }
                return result;
            }));
    } catch (const std::exception& exception) {
        version_compare_->setEnabled(version_before_->count() >= 2
                                     && version_after_->count() >= 2);
        version_diff_->setPlainText(
            tr("Could not start version comparison: %1")
                .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        version_compare_->setEnabled(version_before_->count() >= 2
                                     && version_after_->count() >= 2);
        version_diff_->setPlainText(
            tr("Could not start the background version comparison."));
    }
}

void MainWindow::makeSelectedVersionCurrent() {
    if (document_ == nullptr || version_list_ == nullptr
        || version_list_->currentItem() == nullptr) return;
    if (!documentReplacementAllowed()) return;
    // Saving from the dirty confirmation refreshes the list, so capture the
    // user's target before that refresh can change the selection.
    const auto version = version_list_->currentItem()->data(Qt::UserRole).toULongLong();
    const auto resume = [this, version] {
        if (version_list_ == nullptr) return;
        for (int row = 0; row < version_list_->count(); ++row) {
            if (version_list_->item(row)->data(Qt::UserRole).toULongLong()
                == version) {
                version_list_->setCurrentRow(row);
                makeSelectedVersionCurrent();
                return;
            }
        }
    };
    if (!confirmDiscardChanges(resume)) return;
    stopPlayback();
    cancelMusicAnalysis();
    pvt::BundleSaveReport report;
    std::string error;
    if (!pvt::make_project_version_current(*document_, version, &report, &error)) {
        QMessageBox::critical(this, tr("Could not change version"),
                              QString::fromStdString(error));
        return;
    }
    project_ = document_->project;
    active_layer_uuid_ = project_.layers.back().uuid;
    solo_layer_uuid_.reset();
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    baseline_dirty_ = document_->dirty;
    current_project_path_ = QString::fromStdString(document_->source_path);
    updateCompatibilityWarning();
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    if (live_workspace_ != nullptr) live_workspace_->resetRealtimeFrame();
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
}

void MainWindow::revertSelectedVersion() {
    if (document_ == nullptr || version_list_ == nullptr
        || version_list_->currentItem() == nullptr) return;
    if (!documentReplacementAllowed()) return;
    const auto version = version_list_->currentItem()->data(Qt::UserRole).toULongLong();
    const auto resume = [this, version] {
        if (version_list_ == nullptr) return;
        for (int row = 0; row < version_list_->count(); ++row) {
            if (version_list_->item(row)->data(Qt::UserRole).toULongLong()
                == version) {
                version_list_->setCurrentRow(row);
                revertSelectedVersion();
                return;
            }
        }
    };
    if (!confirmDiscardChanges(resume)) return;
    const auto choice = QMessageBox::question(
        this, tr("Revert as a new version?"),
        tr("Create a new version copied from version %1 and make it current?").arg(version),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) return;
    stopPlayback();
    cancelMusicAnalysis();
    pvt::BundleSaveReport report;
    std::string error;
    if (!pvt::revert_project_as_new(*document_, version, &report, &error)) {
        QMessageBox::critical(this, tr("Could not revert version"),
                              QString::fromStdString(error));
        return;
    }
    project_ = document_->project;
    active_layer_uuid_ = project_.layers.back().uuid;
    solo_layer_uuid_.reset();
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    baseline_dirty_ = false;
    current_project_path_ = QString::fromStdString(document_->source_path);
    updateCompatibilityWarning();
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    if (live_workspace_ != nullptr) live_workspace_->resetRealtimeFrame();
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
}

void MainWindow::createLayerDock() {
    layers_dock_ = new QDockWidget(tr("Project && Layers"), this);
    layers_dock_->setObjectName(QStringLiteral("projectLayersDock"));
    layers_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    layers_dock_->setMinimumWidth(250);
    layers_dock_->setToolTip(
        tr("Drag the title bar to move or dock this panel. Double-click its title bar to toggle between floating and docked."));

    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);
    auto* project_form = new QFormLayout;
    project_name_ = new QLineEdit;
    project_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    project_name_->setValidator(
        new Utf8TextValidator(TextRule::ProjectName, project_name_));
    project_name_->setToolTip(
        tr("The display name appears in the window title and supplies the default bundle name."));
    project_form->addRow(tr("Project"), project_name_);
    layout->addLayout(project_form);

    auto* project_settings = new QGroupBox(tr("Project Settings"));
    project_settings->setObjectName(QStringLiteral("projectSettingsNavigator"));
    auto* project_settings_layout = new QGridLayout(project_settings);
    project_canvas_button_ = new QPushButton(tr("Canvas && Loop"));
    project_sync_button_ = new QPushButton(tr("Sync && Audio"));
    project_export_button_ = new QPushButton(tr("Export"));
    project_history_button_ = new QPushButton(tr("History"));
    project_canvas_button_->setToolTip(
        tr("Open project-wide dimensions, frame count, and playback rate."));
    project_sync_button_->setToolTip(
        tr("Open the persistent Drivers controls for project clock and inherited audio response."));
    project_export_button_->setToolTip(
        tr("Open project-wide image encoding and output destination settings."));
    project_history_button_->setToolTip(
        tr("Open immutable saved versions, rollback actions, and semantic comparison."));
    project_canvas_button_->setAccessibleName(tr("Project settings: Canvas and Loop"));
    project_sync_button_->setAccessibleName(tr("Project settings: Synchronization and Audio"));
    project_export_button_->setAccessibleName(tr("Project settings: Export"));
    project_history_button_->setAccessibleName(tr("Project settings: History"));
    project_settings_layout->addWidget(project_canvas_button_, 0, 0);
    project_settings_layout->addWidget(project_sync_button_, 0, 1);
    project_settings_layout->addWidget(project_export_button_, 1, 0);
    project_settings_layout->addWidget(project_history_button_, 1, 1);
    layout->addWidget(project_settings);

    compatibility_warning_label_ = new QLabel;
    compatibility_warning_label_->setWordWrap(true);
    compatibility_warning_label_->setStyleSheet(
        QStringLiteral("QLabel { background: #5b4815; color: #fff2b2; "
                       "border: 1px solid #c89b24; border-radius: 4px; padding: 6px; }"));
    compatibility_warning_label_->hide();
    layout->addWidget(compatibility_warning_label_);

    auto* explanation = new QLabel(
        tr("Top rows paint over the layers below them. Groups are contiguous folders "
           "that move, hide, lock, and solo their layers together. Solo is preview-only."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    layer_list_ = new QListWidget;
    layer_list_->setAlternatingRowColors(true);
    layer_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(layer_list_, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* duplicate = new QPushButton(tr("Duplicate"));
    auto* remove = new QPushButton(tr("Remove"));
    buttons->addWidget(add);
    buttons->addWidget(duplicate);
    buttons->addWidget(remove);
    layout->addLayout(buttons);
    auto* group_buttons = new QHBoxLayout;
    auto* add_group = new QPushButton(tr("Add group"));
    auto* remove_group = new QPushButton(tr("Remove group"));
    group_buttons->addWidget(add_group);
    group_buttons->addWidget(remove_group);
    layout->addLayout(group_buttons);
    auto* order_buttons = new QHBoxLayout;
    auto* up = new QPushButton(tr("Move up"));
    auto* down = new QPushButton(tr("Move down"));
    order_buttons->addWidget(up);
    order_buttons->addWidget(down);
    layout->addLayout(order_buttons);

    auto* selected = new QGroupBox(tr("Selected layer"));
    auto* selected_form = new QFormLayout(selected);
    layer_name_ = new QLineEdit;
    layer_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    layer_name_->setValidator(new Utf8TextValidator(TextRule::Name, layer_name_));
    layer_enabled_ = new QCheckBox(tr("Visible in preview and export"));
    layer_solo_ = new QCheckBox(tr("Solo in preview"));
    layer_blend_ = new QComboBox;
    for (const auto mode : {pvt::BlendMode::Normal, pvt::BlendMode::SoftLight,
                            pvt::BlendMode::GrainMerge, pvt::BlendMode::Overlay,
                            pvt::BlendMode::ColorDodge, pvt::BlendMode::LinearBurn,
                            pvt::BlendMode::ColorBurn, pvt::BlendMode::Difference,
                            pvt::BlendMode::Subtract, pvt::BlendMode::Multiply,
                            pvt::BlendMode::Add, pvt::BlendMode::Erase,
                            pvt::BlendMode::ColorEraseTones,
                            pvt::BlendMode::ColorEraseBrightness}) {
        QString label = QString::fromUtf8(pvt::blend_mode_name(mode));
        if (mode == pvt::BlendMode::Normal) {
            label = tr("Normal (none)");
        }
        add_enum_item(layer_blend_, label, mode);
    }
    layer_alpha_mode_ = new QComboBox;
    add_enum_item(layer_alpha_mode_, tr("Alpha Over"),
                  pvt::AlphaMode::AlphaOver);
    add_enum_item(layer_alpha_mode_, tr("Alpha Under"),
                  pvt::AlphaMode::AlphaUnder);
    layer_alpha_mode_->setToolTip(
        tr("Alpha Over paints this layer over the accumulated lower stack. "
           "Alpha Under places it beneath that stack after applying the selected blend."));
    layer_group_ = new QComboBox;
    layer_opacity_ = real_editor(0.0, 100.0, 1, 5.0);
    layer_opacity_->setSuffix(tr("%"));
    selected_form->addRow(tr("Name"), layer_name_);
    selected_form->addRow(layer_enabled_);
    selected_form->addRow(layer_solo_);
    selected_form->addRow(tr("Blend"), layer_blend_);
    selected_form->addRow(tr("Alpha Mode"), layer_alpha_mode_);
    selected_form->addRow(tr("Group"), layer_group_);
    selected_form->addRow(tr("Opacity"), layer_opacity_);
    layout->addWidget(selected);

    selected_group_box_ = new QGroupBox(tr("Selected group"));
    auto* group_form = new QFormLayout(selected_group_box_);
    group_name_ = new QLineEdit;
    group_name_->setMaxLength(static_cast<int>(kMaximumNameBytes));
    group_name_->setValidator(new Utf8TextValidator(TextRule::Name, group_name_));
    group_enabled_ = new QCheckBox(tr("Visible in preview and export"));
    group_solo_ = new QCheckBox(tr("Solo group in preview"));
    group_locked_ = new QCheckBox(tr("Lock group and contained layers"));
    group_form->addRow(tr("Name"), group_name_);
    group_form->addRow(group_enabled_);
    group_form->addRow(group_solo_);
    group_form->addRow(group_locked_);
    selected_group_box_->setEnabled(false);
    layout->addWidget(selected_group_box_);

    layers_dock_->setWidget(contents);
    // The selected Flow Workbench reference establishes project structure on
    // the left, the canvas in the center, and stage inspection on the right.
    // Users can still drag this dock to either side and saved layouts win.
    addDockWidget(Qt::LeftDockWidgetArea, layers_dock_);

    connect(layers_dock_, &QDockWidget::topLevelChanged, this,
            [this](bool floating) {
                if (floating && status_ != nullptr) {
                    status_->setText(
                        tr("Project & Layers is floating. Double-click its title bar to dock it, or use View > Restore Project & Layers Panel."));
                }
            });

    connect(layer_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (populating_ || row < 0) {
            return;
        }
        if (auto* item = layer_list_->item(row)) {
            const std::string uuid =
                item->data(Qt::UserRole).toString().toStdString();
            if (item->data(Qt::UserRole + 1).toBool()) {
                selectGroup(uuid);
            } else {
                selectLayer(uuid);
            }
        }
    });
    connect(project_name_, &QLineEdit::editingFinished,
            this, &MainWindow::finishProjectNameEdit);
    connect(project_canvas_button_, &QPushButton::clicked, this, [this] {
        showProjectSettingsPage(project_canvas_page_, tr("Canvas & Loop"));
    });
    connect(project_sync_button_, &QPushButton::clicked, this, [this] {
        showProjectSettingsPage(project_sync_page_, tr("Project Sync & Audio"));
        setDriversExpanded(true);
    });
    connect(project_export_button_, &QPushButton::clicked, this, [this] {
        showProjectSettingsPage(project_export_page_, tr("Export"));
    });
    connect(project_history_button_, &QPushButton::clicked, this, [this] {
        refreshVersionsPage();
        showProjectSettingsPage(history_page_, tr("History"));
    });
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget* previous, QWidget*) {
                if (previous != project_name_ || project_name_->hasAcceptableInput()) return;
                const QSignalBlocker blocker(project_name_);
                project_name_->setText(QString::fromStdString(project_.name));
                status_->setText(
                    tr("Project names cannot be empty or contain control characters or path separators."));
            });
    connect(layer_name_, &QLineEdit::editingFinished, this, [this] {
        if (populating_) return;
        auto* layer = activeLayer();
        const QString edited = layer_name_->text().trimmed();
        const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
        if (layer == nullptr || (group != nullptr && group->locked)
            || edited.isEmpty() || !valid_text(edited, TextRule::Name)) {
            loadLayerEditors();
            return;
        }
        const std::string uuid = layer->uuid;
        const std::string before = layer->name;
        const std::string after = edited.toStdString();
        if (before == after) return;
        layer->name = after;
        recordUndo(tr("Rename layer"),
                   [this, uuid, before] { if (auto* value = findLayer(uuid)) value->name = before; refreshLayerList(); noteDocumentChange(); },
                   [this, uuid, after] { if (auto* value = findLayer(uuid)) value->name = after; refreshLayerList(); noteDocumentChange(); });
        noteDocumentChange();
        refreshLayerList();
    });
    connect(layer_enabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_) return;
        auto* layer = activeLayer();
        const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
        if (layer == nullptr || (group != nullptr && group->locked)
            || layer->enabled == checked) return;
        const std::string uuid = layer->uuid;
        const bool before = layer->enabled;
        const auto before_output = project_.output;
        layer->enabled = checked;
        ensureAlphaForTransparency();
        syncProjectGlobals();
        const auto after_output = project_.output;
        recordUndo(checked ? tr("Show layer") : tr("Hide layer"),
                   [this, uuid, before, before_output] { if (auto* value = findLayer(uuid)) value->enabled = before; project_.output = before_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); if (playback_timer_->isActive()) startProjectAudioPlayback(); },
                   [this, uuid, checked, after_output] { if (auto* value = findLayer(uuid)) value->enabled = checked; project_.output = after_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); if (playback_timer_->isActive()) startProjectAudioPlayback(); });
        noteDocumentChange();
        refreshLayerList();
        schedulePreview();
        if (playback_timer_->isActive()) startProjectAudioPlayback();
    });
    connect(layer_solo_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_) return;
        solo_layer_uuid_ = checked ? std::optional<std::string>(active_layer_uuid_) : std::nullopt;
        if (checked) solo_group_uuid_.reset();
        ++document_revision_;
        refreshLayerList();
        schedulePreview();
        if (playback_timer_->isActive()) startProjectAudioPlayback();
    });
    connect(layer_blend_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_) return;
        auto* layer = activeLayer();
        const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
        if (layer == nullptr || (group != nullptr && group->locked)) return;
        const auto after = static_cast<pvt::BlendMode>(layer_blend_->currentData().toInt());
        const auto before = layer->blend_mode;
        if (before == after) return;
        const std::string uuid = layer->uuid;
        const auto before_output = project_.output;
        layer->blend_mode = after;
        ensureAlphaForTransparency();
        syncProjectGlobals();
        const auto after_output = project_.output;
        recordUndo(tr("Change layer blend"),
                   [this, uuid, before, before_output] {
                       if (auto* value = findLayer(uuid)) value->blend_mode = before;
                       project_.output = before_output;
                       loadActiveConfiguration();
                       refreshLayerList();
                       refreshAll();
                       noteDocumentChange();
                   },
                   [this, uuid, after, after_output] {
                       if (auto* value = findLayer(uuid)) value->blend_mode = after;
                       project_.output = after_output;
                       loadActiveConfiguration();
                       refreshLayerList();
                       refreshAll();
                       noteDocumentChange();
                   });
        noteDocumentChange();
        refreshLayerList();
    });
    connect(layer_alpha_mode_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_) return;
        auto* layer = activeLayer();
        const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
        if (layer == nullptr || (group != nullptr && group->locked)) return;
        const auto after = static_cast<pvt::AlphaMode>(
            layer_alpha_mode_->currentData().toInt());
        const auto before = layer->alpha_mode;
        if (before == after) return;
        const std::string uuid = layer->uuid;
        layer->alpha_mode = after;
        recordUndo(tr("Change layer alpha mode"),
                   [this, uuid, before] { if (auto* value = findLayer(uuid)) value->alpha_mode = before; refreshLayerList(); noteDocumentChange(); schedulePreview(); },
                   [this, uuid, after] { if (auto* value = findLayer(uuid)) value->alpha_mode = after; refreshLayerList(); noteDocumentChange(); schedulePreview(); });
        noteDocumentChange();
        refreshLayerList();
        schedulePreview();
    });
    connect(layer_group_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_) return;
        setActiveLayerGroup(layer_group_->currentData().toString().toStdString());
    });
    connect(layer_opacity_, &QDoubleSpinBox::valueChanged, this, [this](double percent) {
        if (populating_) return;
        auto* layer = activeLayer();
        const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
        if (layer == nullptr || (group != nullptr && group->locked)) return;
        const double after = percent * 0.01;
        const double before = layer->opacity;
        if (before == after) return;
        const std::string uuid = layer->uuid;
        const auto before_output = project_.output;
        layer->opacity = after;
        ensureAlphaForTransparency();
        syncProjectGlobals();
        const auto after_output = project_.output;
        const QString key = QStringLiteral("layer-opacity:") + QString::fromStdString(uuid);
        recordUndo(tr("Change layer opacity"),
                   [this, uuid, before, before_output] { if (auto* value = findLayer(uuid)) value->opacity = before; project_.output = before_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); },
                   [this, uuid, after, after_output] { if (auto* value = findLayer(uuid)) value->opacity = after; project_.output = after_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); }, key);
        noteDocumentChange();
        refreshLayerList();
        schedulePreview();
    });
    connect(add, &QPushButton::clicked, this, &MainWindow::addLayer);
    connect(duplicate, &QPushButton::clicked, this, &MainWindow::duplicateLayer);
    connect(remove, &QPushButton::clicked, this, &MainWindow::removeLayer);
    connect(add_group, &QPushButton::clicked, this, &MainWindow::addGroup);
    connect(remove_group, &QPushButton::clicked,
            this, &MainWindow::removeSelectedGroup);
    connect(up, &QPushButton::clicked, this, [this] {
        if (selected_group_uuid_) moveSelectedGroup(1);
        else moveActiveLayer(1);
    });
    connect(down, &QPushButton::clicked, this, [this] {
        if (selected_group_uuid_) moveSelectedGroup(-1);
        else moveActiveLayer(-1);
    });

    connect(group_name_, &QLineEdit::editingFinished, this, [this] {
        if (populating_ || !selected_group_uuid_) return;
        auto* group = findGroup(*selected_group_uuid_);
        const QString edited = group_name_->text().trimmed();
        if (group == nullptr || edited.isEmpty()
            || !valid_text(edited, TextRule::Name)) {
            loadLayerEditors();
            return;
        }
        const std::string after = edited.toStdString();
        if (after == group->name) return;
        auto before = captureProjectState();
        const std::string before_active = active_layer_uuid_;
        group->name = after;
        refreshLayerList();
        recordProjectStateChange(tr("Rename group"), std::move(before),
                                 before_active);
    });
    connect(group_enabled_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_ || !selected_group_uuid_) return;
        auto* group = findGroup(*selected_group_uuid_);
        if (group == nullptr || group->enabled == checked) return;
        auto before = captureProjectState();
        const std::string before_active = active_layer_uuid_;
        group->enabled = checked;
        ensureAlphaForTransparency();
        syncProjectGlobals();
        refreshLayerList();
        recordProjectStateChange(checked ? tr("Show group") : tr("Hide group"),
                                 std::move(before), before_active);
        schedulePreview();
        if (playback_timer_->isActive()) startProjectAudioPlayback();
    });
    connect(group_locked_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_ || !selected_group_uuid_) return;
        auto* group = findGroup(*selected_group_uuid_);
        if (group == nullptr || group->locked == checked) return;
        auto before = captureProjectState();
        const std::string before_active = active_layer_uuid_;
        group->locked = checked;
        refreshLayerList();
        recordProjectStateChange(checked ? tr("Lock group") : tr("Unlock group"),
                                 std::move(before), before_active);
    });
    connect(group_solo_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_) return;
        solo_group_uuid_ = checked && selected_group_uuid_
                               ? selected_group_uuid_ : std::nullopt;
        if (checked) solo_layer_uuid_.reset();
        ++document_revision_;
        refreshLayerList();
        schedulePreview();
        if (playback_timer_->isActive()) startProjectAudioPlayback();
    });
}

void MainWindow::restoreLayersDock(bool makeVisible) {
    if (layers_dock_ == nullptr) return;
    layers_dock_->setFloating(false);
    addDockWidget(Qt::LeftDockWidgetArea, layers_dock_);
    if (makeVisible) {
        layers_dock_->show();
        layers_dock_->raise();
        if (status_ != nullptr) {
            status_->setText(tr("Project & Layers restored to the left side."));
        }
    } else {
        layers_dock_->hide();
    }
}

QWidget* MainWindow::createTimeline() {
    auto* widget = new QWidget;
    widget->setObjectName(QStringLiteral("projectTransport"));
    widget->setAccessibleName(tr("Project timeline transport"));
    auto* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 4, 0, 0);
    play_button_ = new QPushButton(tr("Play"));
    previous_beat_ = new QPushButton(tr("Previous beat"));
    next_beat_ = new QPushButton(tr("Next beat"));
    timeline_ = new QSlider(Qt::Horizontal);
    timeline_->setObjectName(QStringLiteral("projectFrameTimeline"));
    timeline_->setAccessibleName(tr("Current project frame"));
    timeline_->setToolTip(
        tr("Scrub the single project-wide render timeline. Every visible layer is evaluated at this frame."));
    audio_volume_ = new QSlider(Qt::Horizontal);
    audio_volume_->setObjectName(QStringLiteral("previewAudioVolume"));
    audio_volume_->setRange(0, 100);
    audio_volume_->setValue(std::clamp(
        QSettings().value(QStringLiteral("preferences/previewAudioVolume"), 80)
            .toInt(), 0, 100));
    audio_volume_->setMaximumWidth(110);
    audio_volume_->setToolTip(
        tr("Monitoring volume for the synchronized mix of audible project and active-layer Music clocks. Data-only sources stay silent."));
    timeline_->setRange(0, std::max(1, effectiveFrameCount()) - 1);
    frame_label_ = new QLabel;
    frame_label_->setObjectName(QStringLiteral("timelineReadout"));
    frame_label_->setAccessibleName(tr("Current time and frame readout"));
    frame_label_->setMinimumWidth(270);
    layout->addWidget(play_button_);
    layout->addWidget(previous_beat_);
    layout->addWidget(next_beat_);
    layout->addWidget(new QLabel(tr("Project timeline")));
    layout->addWidget(timeline_, 1);
    layout->addWidget(frame_label_);
    layout->addWidget(new QLabel(tr("Audio")));
    layout->addWidget(audio_volume_);

    connect(play_button_, &QPushButton::clicked,
            this, &MainWindow::togglePlayback);
    connect(timeline_, &QSlider::valueChanged, this, [this](int frame) {
        Q_UNUSED(frame);
        updateTimelineReadout();
        schedulePreview();
    });
    connect(timeline_, &QSlider::sliderReleased, this, [this] {
        if (playback_timer_ != nullptr && playback_timer_->isActive()) {
            startProjectAudioPlayback();
        }
    });
    connect(previous_beat_, &QPushButton::clicked, this,
            [this] { navigateToBeat(-1); });
    connect(next_beat_, &QPushButton::clicked, this,
            [this] { navigateToBeat(1); });
    connect(audio_volume_, &QSlider::valueChanged, this, [this](int value) {
        if (audio_playback_ != nullptr) {
            audio_playback_->set_volume(static_cast<double>(value) / 100.0);
        }
    });
    if (audio_playback_ != nullptr) {
        audio_playback_->set_volume(
            static_cast<double>(audio_volume_->value()) / 100.0);
    }
    updateTimelineReadout();
    return widget;
}

void MainWindow::togglePlayback() {
    if (playback_timer_ == nullptr || play_button_ == nullptr) return;
    if (playback_timer_->isActive()) {
        stopPlayback();
    } else {
        playback_preview_advanced_ = false;
        startProjectAudioPlayback();
        playback_timer_->start(std::max(
            1, static_cast<int>(std::lround(1000.0 / config_.fps))));
        play_button_->setText(tr("Pause"));
    }
    schedulePreview();
}

void MainWindow::stopPlayback() {
    if (playback_timer_ != nullptr) playback_timer_->stop();
    if (audio_playback_ != nullptr) audio_playback_->stop();
    if (play_button_ != nullptr) play_button_->setText(tr("Play"));
}

void MainWindow::startProjectAudioPlayback() {
    if (audio_playback_ == nullptr) return;
    // This function is also the authoritative resynchronization point. Stop a
    // formerly valid Music source before checking the current clock so a mode
    // switch or undo cannot leave stale audio playing.
    audio_playback_->stop();
    if (timeline_ == nullptr || document_ == nullptr) {
        return;
    }
    const double position = static_cast<double>(timeline_->value()) / config_.fps;
    const pvt::ProjectConfig monitoring_project = previewProjectSnapshot();
    const auto tracks = audible_project_tracks(
        monitoring_project, *document_, position);
    if (tracks.empty()) {
        const bool requested_audio =
            (config_.clock.mode == pvt::ClockMode::Music
             && !config_.clock.data_only)
            || std::any_of(monitoring_project.layers.begin(),
                           monitoring_project.layers.end(),
                           [&monitoring_project](const pvt::LayerConfig& layer) {
                               return layer_visible_in_project(
                                          monitoring_project, layer)
                                      && layer.render.layer_clock.enabled
                                      && layer.render.layer_clock.clock.mode
                                             == pvt::ClockMode::Music
                                      && !layer.render.layer_clock.clock.data_only;
                           });
        if (!requested_audio) return;
        status_->setText(
            tr("Preview is playing silently because an audible embedded music source is unavailable or has already finished."));
        return;
    }
    std::string playback_error;
    if (!audio_playback_->start_mix(tracks, position, &playback_error)) {
        status_->setText(
            tr("Preview is playing silently: %1")
                .arg(QString::fromStdString(playback_error)));
        return;
    }
    audio_playback_->set_volume(
        audio_volume_ != nullptr
            ? static_cast<double>(audio_volume_->value()) / 100.0 : 0.8);
}

void MainWindow::openLiveMode() {
    setLiveMode(true);
}

void MainWindow::setLiveMode(bool live) {
    if (workspace_stack_ == nullptr || editor_workspace_ == nullptr
        || live_workspace_ == nullptr) {
        return;
    }
    workspace_stack_->setCurrentWidget(editor_workspace_);
    if (edit_mode_action_ != nullptr) {
        const QSignalBlocker blocker(edit_mode_action_);
        edit_mode_action_->setChecked(true);
    }
    if (live_mode_action_ != nullptr) {
        const QSignalBlocker blocker(live_mode_action_);
        live_mode_action_->setChecked(false);
    }
    if (!live) {
        if (live_popout_window_ != nullptr) {
            show();
            raise();
            activateWindow();
        }
        if (status_ != nullptr) {
            status_->setText(live_workspace_->isLiveActive()
                ? tr("Editing the live project — input, rendering, and stage output remain active in the LIVE window.")
                : tr("Edit mode — Flow Workbench ready."));
        }
        if (!live_workspace_->isLiveActive()) schedulePreview();
        return;
    }

    if (export_active_ || (export_watcher_ != nullptr
                           && export_watcher_->isRunning())
        || project_io_active_ || music_analysis_active_) {
        if (status_ != nullptr) {
            status_->setText(tr(
                "Finish or cancel the active export, project operation, or music analysis before opening LIVE."));
        }
        return;
    }
    if (live_workspace_->isPresentationActive()) {
        setLivePreviewOutputActive(false);
    }

    stopPlayback();
    live_workspace_->setProjectLiveConfig(project_.canvas.live);
    live_workspace_->refreshProjectSnapshot();
    if (!live_workspace_->isLiveActive()) live_workspace_->setLiveActive(true);
    showLiveWindow();
    updateExportAvailability();
}

void MainWindow::refreshLivePreviewOutputControls() {
    if (live_workspace_ == nullptr || live_preview_screen_ == nullptr
        || live_preview_quality_ == nullptr
        || live_preview_fullscreen_ == nullptr
        || live_preview_hide_cursor_ == nullptr) {
        return;
    }
    const QSignalBlocker screen_blocker(live_preview_screen_);
    const QSignalBlocker quality_blocker(live_preview_quality_);
    const QSignalBlocker fullscreen_blocker(live_preview_fullscreen_);
    const QSignalBlocker cursor_blocker(live_preview_hide_cursor_);
    const QString selected = live_workspace_->selectedOutputDisplayId();
    live_preview_screen_->clear();
    for (const auto& display : live_workspace_->availableOutputDisplays()) {
        live_preview_screen_->addItem(display.label, display.id);
    }
    int screen_index = live_preview_screen_->findData(selected);
    if (screen_index < 0 && live_preview_screen_->count() > 0) screen_index = 0;
    live_preview_screen_->setCurrentIndex(screen_index);
    const int quality_index = live_preview_quality_->findData(
        live_workspace_->outputResolutionScale());
    live_preview_quality_->setCurrentIndex(quality_index < 0 ? 0 : quality_index);
    live_preview_fullscreen_->setChecked(
        live_workspace_->presentationFullscreen());
    live_preview_hide_cursor_->setChecked(
        live_workspace_->presentationHideCursor());
}

void MainWindow::setLivePreviewOutputActive(bool active) {
    if (live_workspace_ == nullptr) return;
    if (active && (export_active_
                   || (export_watcher_ != nullptr
                       && export_watcher_->isRunning())
                   || project_io_active_ || music_analysis_active_)) {
        if (status_ != nullptr) {
            status_->setText(tr(
                "Finish the active export, project operation, or music analysis before starting Live Preview Output."));
        }
        if (live_preview_output_action_ != nullptr) {
            const QSignalBlocker blocker(live_preview_output_action_);
            live_preview_output_action_->setChecked(false);
        }
        return;
    }
    if (active && live_workspace_->isLiveActive()) {
        restoreLiveWorkspace(false);
    }
    if (active && preview_cancel_ != nullptr) {
        preview_cancel_->store(true, std::memory_order_relaxed);
    }
    live_workspace_->setPresentationActive(active);
    updateExportAvailability();
}

void MainWindow::showLiveWindow() {
    if (workspace_stack_ == nullptr || live_workspace_ == nullptr
        || editor_workspace_ == nullptr) return;
    if (live_popout_window_ != nullptr) {
        live_workspace_->show();
        live_popout_window_->show();
        live_popout_window_->raise();
        live_popout_window_->activateWindow();
        return;
    }

    // QStackedWidget::removeWidget deliberately hides the removed widget and
    // retains ownership. Detach it explicitly, then restore visibility after
    // QMainWindow adopts it as the central widget. Without the final show(),
    // the top-level window is present but its only content remains hidden.
    live_workspace_->hide();
    workspace_stack_->removeWidget(live_workspace_);
    live_workspace_->setParent(nullptr);
    workspace_stack_->setCurrentWidget(editor_workspace_);
    live_popout_window_ = new QMainWindow(this, Qt::Window);
    live_popout_window_->setObjectName(QStringLiteral("livePopoutWindow"));
    live_popout_window_->setAttribute(Qt::WA_DeleteOnClose, false);
    live_popout_window_->setWindowTitle(tr("Procedural Visualizer Tool — LIVE"));
    live_popout_window_->setCentralWidget(live_workspace_);
    live_popout_window_->resize(1180, 760);
    live_popout_window_->installEventFilter(this);
    live_popout_window_->show();
    live_workspace_->show();
    live_popout_window_->raise();
    live_popout_window_->activateWindow();
    if (status_ != nullptr) {
        status_->setText(tr(
            "Live opened in its own window; the editor preview follows the same routed clocks."));
    }
}

void MainWindow::restoreLiveWorkspace(bool resume_editor_preview) {
    if (workspace_stack_ == nullptr || editor_workspace_ == nullptr
        || live_workspace_ == nullptr) return;
    if (live_workspace_->isLiveActive()) live_workspace_->setLiveActive(false);

    if (live_popout_window_ != nullptr) {
        QMainWindow* window = live_popout_window_;
        live_popout_window_ = nullptr;
        window->removeEventFilter(this);
        QWidget* central = window->takeCentralWidget();
        if (central == live_workspace_) {
            live_workspace_->hide();
            live_workspace_->setParent(workspace_stack_);
            workspace_stack_->addWidget(live_workspace_);
        }
        window->hide();
        window->deleteLater();
    } else if (workspace_stack_->indexOf(live_workspace_) < 0) {
        live_workspace_->hide();
        live_workspace_->setParent(workspace_stack_);
        workspace_stack_->addWidget(live_workspace_);
    }

    workspace_stack_->setCurrentWidget(editor_workspace_);
    if (edit_mode_action_ != nullptr) {
        const QSignalBlocker blocker(edit_mode_action_);
        edit_mode_action_->setChecked(true);
    }
    if (live_mode_action_ != nullptr) {
        const QSignalBlocker blocker(live_mode_action_);
        live_mode_action_->setChecked(false);
    }
    if (status_ != nullptr) status_->setText(tr("Edit mode — Flow Workbench ready."));
    updateExportAvailability();
    if (resume_editor_preview) schedulePreview();
}

void MainWindow::applyAuthoredLiveConfig(const pvt::LiveConfig& live,
                                         const QString& reason) {
    pvt::ProjectConfig candidate = project_;
    candidate.canvas.live = live;
    const pvt::ValidationResult validation = pvt::validate(candidate);
    if (!validation.ok) {
        if (status_ != nullptr) {
            status_->setText(
                tr("Live setting was not applied: %1")
                    .arg(QString::fromStdString(validation.message)));
        }
        if (live_workspace_ != nullptr) {
            live_workspace_->setProjectLiveConfig(project_.canvas.live);
        }
        return;
    }

    ProjectDocumentState before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    config_.live = live;
    syncProjectGlobals();
    if (document_ != nullptr) document_->project = project_;
    recordProjectStateChange(
        reason.isEmpty() ? tr("Edit Live performance settings") : reason,
        std::move(before), before_active);
    if (live_workspace_ != nullptr) {
        live_workspace_->setProjectLiveConfig(project_.canvas.live);
        live_workspace_->refreshProjectSnapshot();
    }
    loadGlobalEditors();
    updateSynchronizationState();
    updateWorkflowSummaries();
    refreshStandardMicControls();
}

void MainWindow::createToolbar() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    auto* edit_menu = menuBar()->addMenu(tr("&Edit"));
    edit_menu->setObjectName(QStringLiteral("editMenu"));
    auto* view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->setObjectName(QStringLiteral("viewMenu"));
    auto* mode_menu = menuBar()->addMenu(tr("&Mode"));
    mode_menu->setObjectName(QStringLiteral("modeMenu"));
    auto* settings_menu = menuBar()->addMenu(tr("&Settings"));
    settings_menu->setObjectName(QStringLiteral("settingsMenu"));
    auto* help_menu = menuBar()->addMenu(tr("&Help"));
    help_menu->setObjectName(QStringLiteral("helpMenu"));
    auto* toolbar = addToolBar(tr("Project"));
    toolbar->setObjectName(QStringLiteral("projectToolbar"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    new_action_ = new QAction(tr("New Project"), this);
    open_action_ = new QAction(tr("Open / Import…"), this);
    open_folder_action_ = new QAction(tr("Open Bundle Folder…"), this);
    save_action_ = new QAction(tr("Save…"), this);
    save_as_action_ = new QAction(tr("Save As…"), this);
    new_action_->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    open_action_->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    save_action_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    open_action_->setObjectName(QStringLiteral("openImportAction"));
    open_folder_action_->setObjectName(
        QStringLiteral("openBundleFolderAction"));
    save_as_action_->setObjectName(QStringLiteral("saveAsAction"));
    new_action_->setShortcut(QKeySequence::New);
    open_action_->setShortcut(QKeySequence::Open);
    save_action_->setShortcut(QKeySequence::Save);
    save_as_action_->setShortcut(QKeySequence::SaveAs);
    file_menu->addActions({new_action_, open_action_, open_folder_action_});
    recent_projects_menu_ = file_menu->addMenu(tr("Open Recent"));
    recent_projects_menu_->setObjectName(QStringLiteral("recentProjectsMenu"));
    refreshRecentProjectsMenu();
    file_menu->addSeparator();
    file_menu->addActions({save_action_, save_as_action_});
    file_menu->addSeparator();
    toolbar->addAction(new_action_);
    toolbar->addAction(open_action_);
    toolbar->addAction(save_action_);
    toolbar->addSeparator();
    randomize_values_action_ = new QAction(tr("Randomize Values…"), this);
    randomize_values_action_->setToolTip(
        tr("Randomize bounded, loop-safe parameters while preserving each item's "
           "name, type, enabled state, and position in its stack."));
    randomize_mix_action_ = new QAction(tr("Random Mix…"), this);
    randomize_mix_action_->setToolTip(
        tr("Create a new bounded mix of waves, swing waveforms, effect types, and "
           "enabled items."));
    settings_menu->addSeparator();
    settings_menu->addAction(randomize_values_action_);
    settings_menu->addAction(randomize_mix_action_);
    export_settings_action_ = toolbar->addAction(tr("Export Settings…"));
    export_settings_action_->setObjectName(QStringLiteral("exportSettingsAction"));
    export_settings_action_->setToolTip(
        tr("Review canvas timing, bit depth, alpha, destination, and file naming before exporting."));
    current_frame_export_action_ =
        toolbar->addAction(tr("Export Current Frame…"));
    export_action_ = toolbar->addAction(tr("Export Frames"));
    video_export_action_ = toolbar->addAction(tr("Export Video…"));
    live_preview_output_action_ = toolbar->addAction(tr("Live Preview Output"));
    live_preview_output_action_->setObjectName(
        QStringLiteral("livePreviewOutputAction"));
    live_preview_output_action_->setCheckable(true);
    live_preview_output_action_->setToolTip(tr(
        "Stream the editor preview to the selected display without starting performance Live inputs."));
    cancel_export_action_ = toolbar->addAction(tr("Cancel export"));
    export_settings_action_->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    current_frame_export_action_->setIcon(
        style()->standardIcon(QStyle::SP_DesktopIcon));
    export_action_->setIcon(
        style()->standardIcon(QStyle::SP_DialogApplyButton));
    video_export_action_->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));
    live_preview_output_action_->setIcon(
        style()->standardIcon(QStyle::SP_DesktopIcon));
    cancel_export_action_->setIcon(
        style()->standardIcon(QStyle::SP_DialogCancelButton));
    cancel_export_action_->setEnabled(false);
    file_menu->addSeparator();
    file_menu->addAction(export_settings_action_);
    file_menu->addAction(current_frame_export_action_);
    file_menu->addAction(export_action_);
    file_menu->addAction(video_export_action_);
    file_menu->addAction(live_preview_output_action_);
    connect(live_preview_output_action_, &QAction::triggered,
            this, [this](bool checked) {
                setLivePreviewOutputActive(checked);
            });

    undo_action_ = undo_stack_->createUndoAction(this, tr("Undo"));
    redo_action_ = undo_stack_->createRedoAction(this, tr("Redo"));
    undo_action_->setShortcut(QKeySequence::Undo);
    redo_action_->setShortcut(QKeySequence::Redo);
    edit_menu->addAction(undo_action_);
    edit_menu->addAction(redo_action_);

    edit_mode_action_ = new QAction(tr("Edit Workbench"), this);
    live_mode_action_ = new QAction(tr("LIVE"), this);
    edit_mode_action_->setObjectName(QStringLiteral("editModeAction"));
    live_mode_action_->setObjectName(QStringLiteral("liveModeAction"));
    edit_mode_action_->setCheckable(true);
    live_mode_action_->setCheckable(false);
    edit_mode_action_->setChecked(true);
    edit_mode_action_->setShortcut(
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    live_mode_action_->setShortcut(
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    edit_mode_action_->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogContentsView));
    live_mode_action_->setIcon(
        style()->standardIcon(QStyle::SP_MediaPlay));
    live_mode_action_->setToolTip(tr(
        "Open the stage-focused Live controls in a separate window immediately. The editor remains available, and freeze, blackout, current scene, and captured input remain ephemeral."));
    auto* mode_group = new QActionGroup(this);
    mode_group->setExclusive(true);
    mode_group->addAction(edit_mode_action_);
    mode_group->addAction(live_mode_action_);
    mode_menu->addActions({edit_mode_action_, live_mode_action_});
    toolbar->addSeparator();
    toolbar->addAction(edit_mode_action_);
    toolbar->addAction(live_mode_action_);
    connect(edit_mode_action_, &QAction::triggered,
            this, [this] { setLiveMode(false); });
    connect(live_mode_action_, &QAction::triggered,
            this, [this] { setLiveMode(true); });

    settings_action_ = new QAction(tr("Application Settings…"), this);
    settings_action_->setObjectName(QStringLiteral("applicationSettingsAction"));
    settings_action_->setShortcut(QKeySequence::Preferences);
    settings_action_->setToolTip(
        tr("Configure preferences that apply to every project and persist across relaunches."));
    settings_action_->setIcon(
        style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    settings_menu->addAction(settings_action_);
    toolbar->addSeparator();
    toolbar->addAction(settings_action_);
    connect(settings_action_, &QAction::triggered,
            this, &MainWindow::showApplicationSettings);
    QAction* const layers_visibility = layers_dock_->toggleViewAction();
    layers_visibility->setText(tr("Project & Layers Panel"));
    layers_visibility->setStatusTip(
        tr("Show or hide the project and layer controls."));
    view_menu->addAction(layers_visibility);
    restore_layers_dock_action_ = new QAction(
        tr("Restore Project & Layers Panel"), this);
    restore_layers_dock_action_->setObjectName(
        QStringLiteral("restoreProjectLayersDockAction"));
    restore_layers_dock_action_->setStatusTip(
        tr("Show the Project & Layers panel and dock it on the left side."));
    view_menu->addAction(restore_layers_dock_action_);
    connect(restore_layers_dock_action_, &QAction::triggered, this,
            [this] { restoreLayersDock(true); });
    about_action_ = new QAction(tr("About PVT…"), this);
    about_action_->setObjectName(QStringLiteral("aboutPvtAction"));
    help_menu->addAction(about_action_);
    connect(about_action_, &QAction::triggered,
            this, &MainWindow::showAboutDialog);

    connect(new_action_, &QAction::triggered, this, [this] {
        if (!documentReplacementAllowed()) return;
        if (!confirmDiscardChanges(
                [this] { replaceWithNewProject(); })) return;
        replaceWithNewProject();
    });
    connect(open_action_, &QAction::triggered, this, &MainWindow::loadSetup);
    connect(open_folder_action_, &QAction::triggered, this, [this] {
        if (!documentReplacementAllowed()) return;
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Open unpacked project bundle"), usableDialogDirectory());
        if (path.isEmpty()) return;
        if (!confirmDiscardChanges(
                [this, path] { startProjectLoad(path); })) return;
        rememberDialogLocation(path);
        startProjectLoad(path);
    });
    connect(save_action_, &QAction::triggered, this, &MainWindow::saveSetup);
    connect(save_as_action_, &QAction::triggered, this, &MainWindow::saveSetupAs);
    connect(randomize_values_action_, &QAction::triggered, this, [this] {
        const auto choice = QMessageBox::question(
            this, tr("Randomize layer values?"),
            tr("This changes the values of every wave, swing, and effect in the "
               "active layer. The action can be undone. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice == QMessageBox::Yes) randomizeExistingStackSettings();
    });
    connect(randomize_mix_action_, &QAction::triggered, this, [this] {
        const auto choice = QMessageBox::question(
            this, tr("Create a random mix?"),
            tr("This replaces the active layer's wave, swing, and effect stacks "
               "with a new random mix. The action can be undone. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice == QMessageBox::Yes) randomizeStackComposition();
    });
    connect(export_settings_action_, &QAction::triggered, this, [this] {
        setWorkflowStage(6);
    });
    connect(export_action_, &QAction::triggered, this, [this] {
        if (export_watcher_->isRunning()) {
            return;
        }
        QString editor_error;
        if (!outputEditorsValid(&editor_error)) {
            setWorkflowStage(6);
            QMessageBox::warning(this, tr("Invalid output text"), editor_error);
            return;
        }
        const auto validation = pvt::validate(project_);
        if (!validation.ok) {
            QMessageBox::warning(this, tr("Invalid setup"),
                                 QString::fromStdString(validation.message));
            return;
        }
        export_action_->setEnabled(false);
        cancel_export_action_->setEnabled(true);
        if (!startExport()) {
            export_action_->setEnabled(true);
            cancel_export_action_->setEnabled(false);
        }
    });
    connect(current_frame_export_action_, &QAction::triggered, this, [this] {
        if (export_watcher_->isRunning()) return;
        QString editor_error;
        if (!outputEditorsValid(&editor_error)) {
            QMessageBox::warning(this, tr("Invalid output text"), editor_error);
            return;
        }
        const auto validation = pvt::validate(project_);
        if (!validation.ok) {
            QMessageBox::warning(this, tr("Invalid setup"),
                                 QString::fromStdString(validation.message));
            return;
        }
        const bool exr = project_.output.bit_depth == 32;
        const QString extension = exr ? QStringLiteral(".exr")
                                      : QStringLiteral(".png");
        const QString filename =
            QString::fromStdString(project_.name)
                + tr(" - frame %1").arg(timeline_->value() + 1)
                + extension;
        QString path = QFileDialog::getSaveFileName(
            this, tr("Export current full-resolution frame"),
            QDir(usableDialogDirectory()).filePath(filename),
            exr ? tr("OpenEXR image (*.exr)") : tr("PNG image (*.png)"));
        if (path.isEmpty()) return;
        if (!path.endsWith(extension, Qt::CaseInsensitive)) path += extension;
        rememberDialogLocation(path);
        current_frame_export_action_->setEnabled(false);
        cancel_export_action_->setEnabled(true);
        if (!startCurrentFrameExport(path)) {
            current_frame_export_action_->setEnabled(true);
            cancel_export_action_->setEnabled(false);
        }
    });
    connect(cancel_export_action_, &QAction::triggered, this, [this] {
        cancel_export_.store(true);
        cancel_export_action_->setEnabled(false);
        status_->setText(tr("Cancelling export…"));
    });
    connect(video_export_action_, &QAction::triggered,
            this, &MainWindow::startVideoExport);
}

void MainWindow::showAboutDialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("aboutPvtDialog"));
    dialog.setWindowTitle(tr("About PVT"));
    dialog.setModal(true);
    dialog.setMinimumWidth(520);

    auto* layout = new QVBoxLayout(&dialog);
    auto* title = new QLabel(
        tr("<h2>Procedural Visualizer Tool %1</h2>")
            .arg(QStringLiteral(PVT_PROGRAM_VERSION)));
    title->setTextFormat(Qt::RichText);
    layout->addWidget(title);

    auto* description = new QLabel(tr(
        "Create deterministic, seamlessly looping procedural art, layered "
        "compositions, image sequences, and music-synchronized visuals."));
    description->setWordWrap(true);
    layout->addWidget(description);

    auto* license = new QLabel(tr(
        "PVT is free software licensed under the GNU General Public License "
        "version 3 or later. It comes with no warranty. Third-party notices "
        "are included with source and packaged distributions."));
    license->setWordWrap(true);
    layout->addWidget(license);

    const auto add_link_button = [&dialog, layout](const QString& text,
                                                   const QString& url) {
        auto* button = new QPushButton(text);
        button->setProperty("pvtExternalUrl", url);
        QObject::connect(button, &QPushButton::clicked, &dialog,
                         [&dialog, url] {
            if (!QDesktopServices::openUrl(QUrl(url))) {
                QMessageBox::warning(
                    &dialog, QObject::tr("Could not open link"),
                    QObject::tr("Open this address in a browser:\n\n%1")
                        .arg(url));
            }
        });
        layout->addWidget(button);
    };
    add_link_button(tr("Project Website"),
                    QStringLiteral("https://github.com/gnaservicesinc/procedural_visualizer_tool"));
    add_link_button(tr("Report a Bug"),
                    QStringLiteral("https://github.com/gnaservicesinc/procedural_visualizer_tool/issues/new"));
    add_link_button(tr("Report a Vulnerability"),
                    QStringLiteral("https://github.com/gnaservicesinc/procedural_visualizer_tool/security/advisories/new"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showMotionPathEditor() {
    if (music_analysis_active_) return;

    const ProjectDocumentState before = captureProjectState();
    const std::string before_active_uuid = active_layer_uuid_;
    pvt::ProjectConfig edited_project = project_;
    std::vector<pvt::CubicMotionPath> paths = project_.canvas.motion_paths;
    pvt::RenderData render = static_cast<const pvt::RenderData&>(config_);

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("motionPathEditor"));
    dialog.setWindowTitle(tr("Reusable motion paths"));
    dialog.resize(980, 680);
    auto* outer = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(tr(
        "Paths are project-wide closed cubic loops. Geometry is shared, while each "
        "layer, wave, or effect owns its own clock, offset, direction, and tangent "
        "settings. The final node always connects back to the first."));
    explanation->setWordWrap(true);
    outer->addWidget(explanation);

    auto* splitter = new QSplitter(Qt::Vertical);
    outer->addWidget(splitter, 1);

    auto* geometry_page = new QWidget;
    auto* geometry_layout = new QVBoxLayout(geometry_page);
    auto* path_row = new QHBoxLayout;
    auto* path_selector = new QComboBox;
    path_selector->setObjectName(QStringLiteral("motionPathSelector"));
    auto* path_name = new QLineEdit;
    path_name->setMaxLength(static_cast<int>(kMaximumNameBytes));
    auto* add_ellipse = new QPushButton(tr("Add ellipse"));
    auto* remove_path = new QPushButton(tr("Remove path"));
    path_row->addWidget(new QLabel(tr("Path")));
    path_row->addWidget(path_selector, 1);
    path_row->addWidget(new QLabel(tr("Name")));
    path_row->addWidget(path_name, 1);
    path_row->addWidget(add_ellipse);
    path_row->addWidget(remove_path);
    geometry_layout->addLayout(path_row);

    auto* nodes = new QTableWidget;
    nodes->setObjectName(QStringLiteral("motionPathNodes"));
    nodes->setColumnCount(8);
    nodes->setHorizontalHeaderLabels(
        {tr("Node ID"), tr("X"), tr("Y"), tr("In X"), tr("In Y"),
         tr("Out X"), tr("Out Y"), tr("Handle mode")});
    nodes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    nodes->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    nodes->verticalHeader()->setVisible(false);
    nodes->setAlternatingRowColors(true);
    nodes->setSelectionBehavior(QAbstractItemView::SelectRows);
    geometry_layout->addWidget(nodes, 1);
    auto* node_row = new QHBoxLayout;
    auto* add_node = new QPushButton(tr("Add node"));
    auto* remove_node = new QPushButton(tr("Remove node"));
    auto* fit_handles = new QPushButton(tr("Fit selected handles"));
    node_row->addWidget(add_node);
    node_row->addWidget(remove_node);
    node_row->addWidget(fit_handles);
    node_row->addStretch(1);
    geometry_layout->addLayout(node_row);
    splitter->addWidget(geometry_page);

    auto* binding_page = new QGroupBox(tr("Consumer binding"));
    auto* binding = new QFormLayout(binding_page);
    auto* consumer = new QComboBox;
    consumer->setObjectName(QStringLiteral("motionPathConsumer"));
    consumer->addItem(tr("Whole active layer"), 0);
    for (std::size_t index = 0U; index < render.waves.size(); ++index) {
        consumer->addItem(
            tr("Wave: %1").arg(QString::fromStdString(render.waves[index].name)),
            static_cast<int>(index + 1U));
    }
    for (std::size_t index = 0U; index < render.effects.size(); ++index) {
        consumer->addItem(
            tr("Effect center: %1")
                .arg(QString::fromStdString(render.effects[index].name)),
            -1 - static_cast<int>(index));
    }
    auto* binding_enabled = new QCheckBox(tr("Follow a reusable path"));
    auto* binding_path = new QComboBox;
    auto* binding_sync = new QCheckBox(tr("Use synchronized project clock"));
    auto* binding_cycles = new QSpinBox;
    binding_cycles->setRange(kMinimumIntegerParameter,
                             kMaximumIntegerParameter);
    auto* binding_phase = new QDoubleSpinBox;
    binding_phase->setDecimals(3);
    binding_phase->setRange(-kMaximumRenderParameter,
                            kMaximumRenderParameter);
    binding_phase->setSuffix(QChar(0x00b0));
    auto* binding_reverse = new QCheckBox(tr("Reverse direction"));
    auto* binding_offset_x = new QDoubleSpinBox;
    auto* binding_offset_y = new QDoubleSpinBox;
    for (auto* editor : {binding_offset_x, binding_offset_y}) {
        editor->setDecimals(4);
        editor->setRange(-kMaximumRenderParameter,
                         kMaximumRenderParameter);
        editor->setSingleStep(0.01);
    }
    auto* binding_tangent = new QCheckBox(tr("Follow tangent orientation"));
    binding->addRow(tr("Target"), consumer);
    binding->addRow(binding_enabled);
    binding->addRow(tr("Path"), binding_path);
    binding->addRow(binding_sync);
    binding->addRow(tr("Cycles per loop"), binding_cycles);
    binding->addRow(tr("Starting phase"), binding_phase);
    binding->addRow(binding_reverse);
    binding->addRow(tr("Horizontal offset"), binding_offset_x);
    binding->addRow(tr("Vertical offset"), binding_offset_y);
    binding->addRow(binding_tangent);
    splitter->addWidget(binding_page);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    outer->addWidget(buttons);

    bool refreshing_geometry = false;
    bool refreshing_binding = false;
    bool alpha_enabled_for_path_edit = false;

    const auto current_path = [&]() -> pvt::CubicMotionPath* {
        const int index = path_selector->currentIndex();
        return index >= 0 && static_cast<std::size_t>(index) < paths.size()
                   ? &paths[static_cast<std::size_t>(index)] : nullptr;
    };
    const auto current_binding = [&]() -> pvt::PathBinding* {
        const int code = consumer->currentData().toInt();
        if (code == 0) return &render.motion.custom_path;
        if (code > 0) {
            const std::size_t index = static_cast<std::size_t>(code - 1);
            return index < render.waves.size() ? &render.waves[index].path : nullptr;
        }
        if (code < 0) {
            const std::size_t index = static_cast<std::size_t>(-1 - code);
            return index < render.effects.size() ? &render.effects[index].path : nullptr;
        }
        return nullptr;
    };

    const auto commit_nodes = [&]() -> bool {
        if (refreshing_geometry) return true;
        auto* path = current_path();
        if (path == nullptr || nodes->rowCount() != static_cast<int>(path->nodes.size())) {
            return path == nullptr;
        }
        for (int row = 0; row < nodes->rowCount(); ++row) {
            auto& node = path->nodes[static_cast<std::size_t>(row)];
            std::array<double*, 6U> destinations{{
                &node.x, &node.y, &node.in_x, &node.in_y,
                &node.out_x, &node.out_y}};
            for (int column = 1; column <= 6; ++column) {
                bool ok = false;
                const double value = nodes->item(row, column)->text().toDouble(&ok);
                if (!ok || !std::isfinite(value)
                    || value < -kMaximumRenderParameter
                    || value > kMaximumRenderParameter) {
                    return false;
                }
                *destinations[static_cast<std::size_t>(column - 1)] = value;
            }
            if (auto* mode = qobject_cast<QComboBox*>(nodes->cellWidget(row, 7))) {
                node.handle_mode = static_cast<pvt::PathHandleMode>(
                    mode->currentData().toInt());
            }
        }
        return true;
    };

    std::function<void()> refresh_nodes;
    refresh_nodes = [&] {
        refreshing_geometry = true;
        nodes->clearContents();
        const auto* path = current_path();
        nodes->setRowCount(path == nullptr ? 0 : static_cast<int>(path->nodes.size()));
        if (path != nullptr) {
            path_name->setText(QString::fromStdString(path->name));
            for (int row = 0; row < nodes->rowCount(); ++row) {
                const auto& node = path->nodes[static_cast<std::size_t>(row)];
                auto* id = new QTableWidgetItem(QString::number(node.id));
                id->setFlags(id->flags() & ~Qt::ItemIsEditable);
                nodes->setItem(row, 0, id);
                const std::array<double, 6U> values{{
                    node.x, node.y, node.in_x, node.in_y,
                    node.out_x, node.out_y}};
                for (int column = 1; column <= 6; ++column) {
                    nodes->setItem(row, column, new QTableWidgetItem(
                        QString::number(values[static_cast<std::size_t>(column - 1)],
                                        'g', 10)));
                }
                auto* mode = new QComboBox;
                mode->addItem(tr("Corner"), static_cast<int>(pvt::PathHandleMode::Corner));
                mode->addItem(tr("Auto smooth"), static_cast<int>(pvt::PathHandleMode::AutoSmooth));
                mode->addItem(tr("Smooth"), static_cast<int>(pvt::PathHandleMode::Smooth));
                mode->addItem(tr("Symmetric"), static_cast<int>(pvt::PathHandleMode::Symmetric));
                mode->setCurrentIndex(mode->findData(static_cast<int>(node.handle_mode)));
                connect(mode, &QComboBox::currentIndexChanged, &dialog,
                        [&, mode, row] {
                    if (refreshing_geometry) return;
                    if (auto* selected = current_path(); selected != nullptr
                        && static_cast<std::size_t>(row) < selected->nodes.size()) {
                        selected->nodes[static_cast<std::size_t>(row)].handle_mode =
                            static_cast<pvt::PathHandleMode>(mode->currentData().toInt());
                    }
                });
                nodes->setCellWidget(row, 7, mode);
            }
        } else {
            path_name->clear();
        }
        path_name->setEnabled(path != nullptr);
        remove_path->setEnabled(path != nullptr);
        add_node->setEnabled(path != nullptr);
        remove_node->setEnabled(path != nullptr && path->nodes.size() > 3U);
        fit_handles->setEnabled(path != nullptr);
        refreshing_geometry = false;
    };

    const auto refresh_binding_paths = [&] {
        const QSignalBlocker blocker(binding_path);
        binding_path->clear();
        for (const auto& path : paths) {
            binding_path->addItem(QString::fromStdString(path.name),
                                  QVariant::fromValue<qulonglong>(path.id));
        }
    };
    const auto load_binding = [&] {
        refreshing_binding = true;
        const auto* value = current_binding();
        refresh_binding_paths();
        if (value != nullptr) {
            binding_enabled->setChecked(value->enabled);
            const int path_index = binding_path->findData(
                QVariant::fromValue<qulonglong>(value->path_id));
            binding_path->setCurrentIndex(path_index >= 0 ? path_index : 0);
            binding_sync->setChecked(value->synchronized);
            binding_cycles->setValue(value->cycles_per_loop);
            binding_phase->setValue(value->phase_degrees);
            binding_reverse->setChecked(value->reverse);
            binding_offset_x->setValue(value->offset_x);
            binding_offset_y->setValue(value->offset_y);
            binding_tangent->setChecked(value->follow_tangent);
        }
        binding_enabled->setEnabled(!paths.empty());
        binding_path->setEnabled(!paths.empty());
        refreshing_binding = false;
    };
    const auto update_binding = [&] {
        if (refreshing_binding) return;
        auto* value = current_binding();
        if (value == nullptr) return;
        value->enabled = binding_enabled->isChecked() && !paths.empty();
        value->path_id = binding_path->currentIndex() >= 0
                             ? binding_path->currentData().toULongLong() : 0U;
        value->synchronized = binding_sync->isChecked();
        value->cycles_per_loop = binding_cycles->value();
        value->phase_degrees = binding_phase->value();
        value->reverse = binding_reverse->isChecked();
        value->offset_x = binding_offset_x->value();
        value->offset_y = binding_offset_y->value();
        value->follow_tangent = binding_tangent->isChecked();
    };

    const auto refresh_paths = [&](std::uint64_t selected_id = 0U) {
        const QSignalBlocker blocker(path_selector);
        path_selector->clear();
        int selected = -1;
        for (std::size_t index = 0U; index < paths.size(); ++index) {
            path_selector->addItem(QString::fromStdString(paths[index].name),
                                   QVariant::fromValue<qulonglong>(paths[index].id));
            if (paths[index].id == selected_id) selected = static_cast<int>(index);
        }
        if (selected < 0 && !paths.empty()) selected = 0;
        path_selector->setCurrentIndex(selected);
        refresh_nodes();
        load_binding();
    };

    connect(path_selector, &QComboBox::currentIndexChanged, &dialog,
            [&] { refresh_nodes(); });
    connect(path_name, &QLineEdit::editingFinished, &dialog, [&] {
        if (refreshing_geometry) return;
        auto* path = current_path();
        if (path == nullptr) return;
        if (!valid_text(path_name->text(), TextRule::Name)) {
            path_name->setText(QString::fromStdString(path->name));
            return;
        }
        path->name = path_name->text().toStdString();
        const QSignalBlocker blocker(path_selector);
        path_selector->setItemText(path_selector->currentIndex(), path_name->text());
        load_binding();
    });
    connect(nodes, &QTableWidget::itemChanged, &dialog,
            [&](QTableWidgetItem*) { (void)commit_nodes(); });
    connect(add_ellipse, &QPushButton::clicked, &dialog, [&] {
        if (paths.size() >= pvt::kMaximumMotionPaths) {
            QMessageBox::warning(
                &dialog, tr("Path limit reached"),
                tr("The signed-int UI/API path index is exhausted."));
            return;
        }
        std::uint64_t maximum = 0U;
        for (const auto& path : paths) {
            maximum = std::max(maximum, path.id);
            for (const auto& node : path.nodes) maximum = std::max(maximum, node.id);
        }
        if (maximum > std::numeric_limits<std::uint64_t>::max() - 5U) return;
        const std::uint64_t id = maximum + 1U;
        paths.push_back(pvt::default_ellipse_path(id, id + 1U,
                                                  "Ellipse " + std::to_string(paths.size() + 1U)));
        refresh_paths(id);
    });
    connect(remove_path, &QPushButton::clicked, &dialog, [&] {
        const int index = path_selector->currentIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= paths.size()) return;
        const std::uint64_t removed_id = paths[static_cast<std::size_t>(index)].id;
        paths.erase(paths.begin() + index);
        const auto clear_removed = [removed_id](pvt::PathBinding& value) {
            if (value.path_id == removed_id) {
                value.enabled = false;
                value.path_id = 0U;
            }
        };
        clear_removed(render.motion.custom_path);
        for (auto& wave : render.waves) clear_removed(wave.path);
        for (auto& effect : render.effects) clear_removed(effect.path);
        for (auto& layer : edited_project.layers) {
            clear_removed(layer.render.motion.custom_path);
            for (auto& wave : layer.render.waves) clear_removed(wave.path);
            for (auto& effect : layer.render.effects) clear_removed(effect.path);
        }
        refresh_paths();
    });
    connect(add_node, &QPushButton::clicked, &dialog, [&] {
        auto* path = current_path();
        if (path == nullptr || path->nodes.size() >= pvt::kMaximumMotionPathNodes) return;
        (void)commit_nodes();
        std::uint64_t id = 0U;
        for (const auto& node : path->nodes) id = std::max(id, node.id);
        if (id == std::numeric_limits<std::uint64_t>::max()) return;
        pvt::CubicPathNode node;
        node.id = id + 1U;
        if (!path->nodes.empty()) {
            node.x = (path->nodes.back().x + path->nodes.front().x) * 0.5;
            node.y = (path->nodes.back().y + path->nodes.front().y) * 0.5;
        }
        node.handle_mode = pvt::PathHandleMode::AutoSmooth;
        path->nodes.push_back(node);
        refresh_nodes();
        nodes->selectRow(nodes->rowCount() - 1);
    });
    connect(remove_node, &QPushButton::clicked, &dialog, [&] {
        auto* path = current_path();
        const int row = nodes->currentRow();
        if (path == nullptr || path->nodes.size() <= 3U || row < 0
            || static_cast<std::size_t>(row) >= path->nodes.size()) return;
        path->nodes.erase(path->nodes.begin() + row);
        refresh_nodes();
    });
    connect(fit_handles, &QPushButton::clicked, &dialog, [&] {
        auto* path = current_path();
        const int row = nodes->currentRow();
        if (path == nullptr || row < 0
            || static_cast<std::size_t>(row) >= path->nodes.size()) return;
        (void)commit_nodes();
        const std::size_t index = static_cast<std::size_t>(row);
        auto& node = path->nodes[index];
        const auto& previous = path->nodes[(index + path->nodes.size() - 1U)
                                           % path->nodes.size()];
        const auto& next = path->nodes[(index + 1U) % path->nodes.size()];
        double tangent_x = next.x - previous.x;
        double tangent_y = next.y - previous.y;
        const double tangent_length = std::hypot(tangent_x, tangent_y);
        if (tangent_length <= 1.0e-12) return;
        tangent_x /= tangent_length;
        tangent_y /= tangent_length;
        double in_length = std::hypot(node.in_x, node.in_y);
        double out_length = std::hypot(node.out_x, node.out_y);
        const double auto_in = std::hypot(node.x - previous.x,
                                          node.y - previous.y) / 3.0;
        const double auto_out = std::hypot(next.x - node.x,
                                           next.y - node.y) / 3.0;
        if (node.handle_mode == pvt::PathHandleMode::Corner) return;
        if (node.handle_mode == pvt::PathHandleMode::AutoSmooth) {
            in_length = auto_in;
            out_length = auto_out;
        } else if (node.handle_mode == pvt::PathHandleMode::Symmetric) {
            const double shared = in_length + out_length > 1.0e-12
                                      ? (in_length + out_length) * 0.5
                                      : (auto_in + auto_out) * 0.5;
            in_length = shared;
            out_length = shared;
        } else {
            if (in_length <= 1.0e-12) in_length = auto_in;
            if (out_length <= 1.0e-12) out_length = auto_out;
        }
        node.in_x = -tangent_x * in_length;
        node.in_y = -tangent_y * in_length;
        node.out_x = tangent_x * out_length;
        node.out_y = tangent_y * out_length;
        refresh_nodes();
        nodes->selectRow(row);
    });

    connect(consumer, &QComboBox::currentIndexChanged, &dialog, load_binding);
    connect(binding_enabled, &QCheckBox::toggled, &dialog, update_binding);
    connect(binding_path, &QComboBox::currentIndexChanged, &dialog, update_binding);
    connect(binding_sync, &QCheckBox::toggled, &dialog, update_binding);
    connect(binding_cycles, &QSpinBox::valueChanged, &dialog, update_binding);
    connect(binding_phase, &QDoubleSpinBox::valueChanged, &dialog, update_binding);
    connect(binding_reverse, &QCheckBox::toggled, &dialog, update_binding);
    connect(binding_offset_x, &QDoubleSpinBox::valueChanged, &dialog, update_binding);
    connect(binding_offset_y, &QDoubleSpinBox::valueChanged, &dialog, update_binding);
    connect(binding_tangent, &QCheckBox::toggled, &dialog, update_binding);

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!commit_nodes()) {
            QMessageBox::warning(&dialog, tr("Invalid path node"),
                                 tr("Every path coordinate and handle must be finite within the renderer's numeric representation."));
            return;
        }
        if (auto* path = current_path(); path != nullptr) {
            if (!valid_text(path_name->text(), TextRule::Name)) {
                QMessageBox::warning(&dialog, tr("Invalid path name"),
                                     tr("Give the selected path a non-empty portable name."));
                return;
            }
            path->name = path_name->text().toStdString();
        }
        edited_project.canvas.motion_paths = paths;
        const auto active = std::find_if(
            edited_project.layers.begin(), edited_project.layers.end(),
            [this](const pvt::LayerConfig& layer) {
                return layer.uuid == active_layer_uuid_;
            });
        if (active != edited_project.layers.end()) active->render = render;
        if (!edited_project.output.write_alpha
            && visible_stack_requires_alpha(edited_project)) {
            // The dialog is the only editor for whole-layer reusable-path
            // bindings. Establish the output invariant before validation so
            // enabling a path cannot trap the user behind an "invalid RGB"
            // error with no reachable way to accept the corrective change.
            edited_project.output.write_alpha = true;
            alpha_enabled_for_path_edit = true;
        }
        const pvt::ValidationResult validation = pvt::validate(edited_project);
        if (!validation.ok) {
            QMessageBox::warning(&dialog, tr("Invalid path configuration"),
                                 QString::fromStdString(validation.message));
            return;
        }
        dialog.accept();
    });

    refresh_paths();
    if (dialog.exec() != QDialog::Accepted) return;

    project_ = std::move(edited_project);
    loadActiveConfiguration();
    if (document_ != nullptr) document_->project = project_;
    loadLayerEditors();
    loadGlobalEditors();
    preview_->setConfiguration(config_);
    schedulePreview();
    recordProjectStateChange(tr("Edit reusable motion paths"), before,
                             before_active_uuid);
    status_->setText(
        alpha_enabled_for_path_edit
            ? tr("Updated reusable motion paths and active-layer bindings. Final alpha output was enabled because the edited stack can be transparent.")
            : tr("Updated reusable motion paths and active-layer bindings."));
}

void MainWindow::connectEditors() {
    connect(wave_list_, &QListWidget::currentRowChanged, this, [this] { loadSelectedWave(); });
    connect(swing_list_, &QListWidget::currentRowChanged, this,
            [this] { loadSelectedSwing(); });
    connect(effect_list_, &QListWidget::currentRowChanged, this,
            [this] { loadSelectedEffect(); });

    connect(wave_name_, &QLineEdit::editingFinished, this, [this] {
        const QString name = wave_name_->text();
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedWaveIndex()) {
            if (config_.waves[*index].name == name.toStdString()) return;
            auto before = captureActiveState();
            config_.waves[*index].name = name.toStdString();
            syncActiveRender();
            updateWaveListItem(*index);
            recordActiveStateChange(tr("Rename wave"), std::move(before));
        }
    });
    connect(wave_enabled_, &QCheckBox::toggled, this,
            [this] { applyWaveEditor(wave_enabled_); });
    connect(wave_sync_, &QCheckBox::toggled, this,
            [this] { applyWaveEditor(wave_sync_); });
    connect(wave_audio_response_, &QComboBox::currentIndexChanged, this,
            [this] { applyWaveEditor(wave_audio_response_); });
    for (auto* editor : {wave_x_, wave_y_, wave_amplitude_, wave_frequency_, wave_phase_,
                         wave_direction_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyWaveEditor(editor); });
    }
    connect(wave_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyWaveEditor(wave_cycles_); });
    for (auto* editor : {wave_displacement_enabled_,
                         wave_lighting_enabled_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyGlobalEditor(editor); });
    }

    connect(swing_name_, &QLineEdit::editingFinished, this, [this] {
        const QString name = swing_name_->text();
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedSwingIndex()) {
            if (config_.swings[*index].name == name.toStdString()) return;
            auto before = captureActiveState();
            config_.swings[*index].name = name.toStdString();
            syncActiveRender();
            updateSwingListItem(*index);
            recordActiveStateChange(tr("Rename swing"), std::move(before));
        }
    });
    connect(swing_enabled_, &QCheckBox::toggled, this,
            [this] { applySwingEditor(swing_enabled_); });
    connect(swings_group_, &QGroupBox::toggled, this,
            [this] { applySwingEditor(swings_group_); });
    connect(swing_waveform_, &QComboBox::currentIndexChanged, this,
            [this] { applySwingEditor(swing_waveform_); });
    connect(swing_cycles_, &QSpinBox::valueChanged, this,
            [this] { applySwingEditor(swing_cycles_); });
    for (auto* editor : {swing_amount_, swing_phase_, swing_shape_,
                         swing_center_x_, swing_center_y_, swing_radius_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applySwingEditor(editor); });
    }

    connect(effect_name_, &QLineEdit::editingFinished, this, [this] {
        const QString name = effect_name_->text();
        if (populating_ || !valid_text(name, TextRule::Name)) {
            return;
        }
        if (const auto index = selectedEffectIndex()) {
            if (config_.effects[*index].name == name.toStdString()) return;
            auto before = captureActiveState();
            config_.effects[*index].name = name.toStdString();
            syncActiveRender();
            updateEffectListItem(*index);
            recordActiveStateChange(tr("Rename effect"), std::move(before));
        }
    });
    connect(effect_enabled_, &QCheckBox::toggled, this,
            [this] { applyEffectEditor(effect_enabled_); });
    connect(effect_sync_, &QCheckBox::toggled, this,
            [this] { applyEffectEditor(effect_sync_); });
    connect(effect_audio_response_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_audio_response_); });
    connect(effect_type_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_type_); });
    connect(effect_blur_type_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_blur_type_); });
    connect(effect_particle_shape_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_particle_shape_); });
    connect(effect_particle_profile_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_particle_profile_); });
    connect(effect_particle_orientation_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_particle_orientation_); });
    connect(effect_particle_size_scale_, &QSlider::valueChanged, this,
            [this] { applyEffectEditor(effect_particle_size_scale_); });
    connect(effect_particle_seed_, &QLineEdit::editingFinished, this,
            [this] { applyEffectEditor(effect_particle_seed_); });
    connect(effect_particle_reseed_, &QPushButton::clicked, this,
            [this] { applyEffectEditor(effect_particle_reseed_); });
    connect(effect_space_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_space_); });
    connect(effect_edge_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_edge_); });
    connect(effect_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyEffectEditor(effect_cycles_); });
    for (auto* editor : {effect_blur_passes_, effect_blur_samples_}) {
        connect(editor, &QSpinBox::valueChanged, this,
                [this, editor] { applyEffectEditor(editor); });
    }
    for (auto* editor : {effect_phase_, effect_intensity_, effect_magnitude_,
                         effect_frequency_, effect_secondary_, effect_center_x_,
                         effect_center_y_, effect_angle_, effect_radius_, effect_threshold_,
                         effect_knee_, effect_area_radius_, effect_blur_minimum_,
                         effect_blur_maximum_, effect_particle_size_variation_,
                         effect_particle_definition_, effect_particle_twinkle_,
                         effect_particle_rotation_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyEffectEditor(editor); });
    }

    for (auto* editor : {clock_mode_, clock_interpolation_, clock_fit_,
                         music_tempo_mode_}) {
        connect(editor, &QComboBox::currentIndexChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    connect(project_mic_device_, qOverload<int>(&QComboBox::activated), this,
            [this](int) { applyStandardMicDeviceBinding(false); });
    connect(project_mic_refresh_, &QPushButton::clicked, this, [this] {
        if (live_workspace_ != nullptr) live_workspace_->refreshAudioInputs();
        refreshStandardMicControls();
    });
    connect(project_mic_setup_, &QPushButton::clicked, this,
            [this] { revealStandardMicSetup(false); });
    for (auto* editor : {clock_time_interval_ms_, meter_bpm_,
                         clock_phase_offset_, music_beat_offset_ms_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    for (auto* editor : {clock_frame_interval_, meter_tempo_note_}) {
        connect(editor, &QSpinBox::valueChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    connect(clock_reverse_, &QCheckBox::toggled, this,
            [this] { applyClockEditor(clock_reverse_); });
    connect(music_data_only_, &QCheckBox::toggled, this,
            [this] { applyClockEditor(music_data_only_); });
    connect(meter_expression_, &QLineEdit::editingFinished, this,
            [this] { applyClockEditor(meter_expression_); });
    connect(meter_expression_, &QLineEdit::textEdited, this, [this] {
        std::string description;
        std::string error;
        const bool valid = pvt::describe_meter(
            meter_expression_->text().toStdString(), description, &error);
        meter_summary_->setText(valid ? QString::fromStdString(description)
                                      : QString::fromStdString(error));
        meter_summary_->setStyleSheet(valid ? QString{} : QStringLiteral("color: #d32f2f;"));
    });

    connect(layer_clock_group_, &QGroupBox::toggled, this,
            [this] { applyClockEditor(layer_clock_group_); });
    connect(layer_clock_mix_enabled_, &QCheckBox::toggled, this,
            [this] { applyClockEditor(layer_clock_mix_enabled_); });
    for (auto* editor : {layer_clock_mix_mode_, layer_clock_scale_,
                         layer_clock_mode_,
                         layer_clock_interpolation_, layer_clock_fit_,
                         layer_music_tempo_mode_}) {
        connect(editor, &QComboBox::currentIndexChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    connect(layer_mic_device_, qOverload<int>(&QComboBox::activated), this,
            [this](int) { applyStandardMicDeviceBinding(true); });
    connect(layer_mic_refresh_, &QPushButton::clicked, this, [this] {
        if (live_workspace_ != nullptr) live_workspace_->refreshAudioInputs();
        refreshStandardMicControls();
    });
    connect(layer_mic_setup_, &QPushButton::clicked, this,
            [this] { revealStandardMicSetup(true); });
    for (auto* editor : {layer_clock_time_interval_ms_, layer_meter_bpm_,
                         layer_clock_phase_offset_, layer_music_beat_offset_ms_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    for (auto* editor : {layer_clock_frame_interval_, layer_meter_tempo_note_}) {
        connect(editor, &QSpinBox::valueChanged, this,
                [this, editor] { applyClockEditor(editor); });
    }
    connect(layer_clock_reverse_, &QCheckBox::toggled, this,
            [this] { applyClockEditor(layer_clock_reverse_); });
    connect(layer_music_data_only_, &QCheckBox::toggled, this,
            [this] { applyClockEditor(layer_music_data_only_); });
    connect(layer_meter_expression_, &QLineEdit::editingFinished, this,
            [this] { applyClockEditor(layer_meter_expression_); });
    connect(layer_meter_expression_, &QLineEdit::textEdited, this, [this] {
        std::string description;
        std::string error;
        const bool valid = pvt::describe_meter(
            layer_meter_expression_->text().toStdString(), description, &error);
        layer_meter_summary_->setText(valid ? QString::fromStdString(description)
                                            : QString::fromStdString(error));
        layer_meter_summary_->setStyleSheet(
            valid ? QString{} : QStringLiteral("color: #d32f2f;"));
    });

    connect(project_audio_response_group_, &QGroupBox::toggled, this,
            [this] { applyAudioReactiveEditor(project_audio_response_group_); });
    for (auto* editor : {project_audio_sync_only_,
                         project_audio_waves_enabled_,
                         project_audio_effects_enabled_,
                         project_audio_color_enabled_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }
    for (auto* editor : {project_audio_wave_source_,
                         project_audio_effect_source_,
                         project_audio_color_source_}) {
        connect(editor, &QComboBox::currentIndexChanged, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }
    for (auto* editor : {project_audio_wave_amount_,
                         project_audio_effect_amount_,
                         project_audio_color_amount_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }

    connect(audio_response_group_, &QGroupBox::toggled, this,
            [this] { applyAudioReactiveEditor(audio_response_group_); });
    for (auto* editor : {audio_response_enabled_, audio_sync_only_,
                         audio_waves_enabled_,
                         audio_effects_enabled_, audio_color_enabled_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }
    for (auto* editor : {audio_wave_source_, audio_effect_source_,
                         audio_color_source_}) {
        connect(editor, &QComboBox::currentIndexChanged, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }
    for (auto* editor : {audio_wave_amount_, audio_effect_amount_,
                         audio_color_amount_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyAudioReactiveEditor(editor); });
    }
    connect(audio_copy_project_, &QPushButton::clicked, this, [this] {
        if (populating_) return;
        auto before = captureActiveState();
        config_.audio_reactive = config_.audio_reactive_defaults;
        config_.audio_reactive_override_enabled = true;
        syncActiveRender();
        loadGlobalEditors();
        preview_->setConfiguration(config_);
        schedulePreview();
        status_->setText(tr("Copied project audio response into the active-layer override."));
        recordActiveStateChange(tr("Copy project audio response"),
                                std::move(before));
    });

    for (auto* editor : {width_, height_, block_size_, frames_, spiral_arms_, hue_cycles_,
                         kaleidoscope_segments_, domain_warp_octaves_,
                         domain_warp_cycles_,
                         surface_rotations_, surface_plane_displacement_ratio_,
                         post_antialias_passes_,
                         quantization_levels_, alpha_cycles_, first_frame_,
                         filename_digits_, png_compression_, motion_cycles_x_,
                         motion_cycles_y_, motion_rotations_}) {
        connect(editor, &QSpinBox::valueChanged, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {fps_, displacement_, wave_depth_, spiral_frequency_,
                         wall_frequency_, wall_mix_, saturation_, surface_curvature_,
                         kaleidoscope_rotation_, kaleidoscope_mix_,
                         domain_warp_strength_, domain_warp_scale_,
                         surface_lighting_, surface_plane_displacement_minimum_,
                         surface_plane_displacement_maximum_,
                         surface_plane_displacement_midpoint_,
                         post_invert_rgb_mix_,
                         post_invert_alpha_mix_, post_antialias_strength_,
                         post_antialias_threshold_, quantization_mix_, alpha_minimum_,
                         alpha_maximum_, alpha_frequency_, phrase_warp_, ghost_mix_, ghost_lag_,
                         surface_phase_, alpha_phase_, motion_center_x_, motion_center_y_,
                         motion_travel_x_, motion_travel_y_, motion_phase_,
                         motion_rotation_offset_, motion_scale_pulse_,
                         starting_red_minimum_,
                         starting_red_maximum_, starting_green_minimum_,
                         starting_green_maximum_, starting_blue_minimum_,
                         starting_blue_maximum_, starting_alpha_minimum_,
                         starting_alpha_maximum_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {displacement_enabled_, lighting_enabled_, spiral_enabled_,
                         wall_enabled_, surface_enabled_, post_invert_rgb_enabled_,
                         surface_plane_displacement_enabled_,
                         post_invert_alpha_enabled_, post_antialias_enabled_,
                         quantization_enabled_,
                         alpha_enabled_, dither_enabled_, write_alpha_, overwrite_,
                         transform_flip_horizontal_, transform_flip_vertical_,
                         palette_enabled_, starting_image_palette_dither_,
                         starting_color_include_alpha_, alpha_use_source_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    connect(motion_group_, &QGroupBox::toggled, this,
            [this] { applyGlobalEditor(motion_group_); });
    connect(kaleidoscope_group_, &QGroupBox::toggled, this,
            [this] { applyGlobalEditor(kaleidoscope_group_); });
    connect(domain_warp_group_, &QGroupBox::toggled, this,
            [this] { applyGlobalEditor(domain_warp_group_); });
    connect(domain_warp_seed_, &QLineEdit::editingFinished, this,
            [this] { applyGlobalEditor(domain_warp_seed_); });
    connect(domain_warp_random_seed_, &QPushButton::clicked, this, [this] {
        domain_warp_seed_->setText(
            QString::number(QRandomGenerator::system()->generate64()));
        applyGlobalEditor(domain_warp_seed_);
    });
    connect(motion_paths_edit_, &QPushButton::clicked, this,
            &MainWindow::showMotionPathEditor);
    connect(starting_image_enabled_, &QCheckBox::toggled, this,
            [this] { applyGlobalEditor(starting_image_enabled_); });
    for (auto* editor : {surface_mapping_, quantization_mode_, bit_depth_, dither_method_,
                         transform_mirror_, motion_path_, starting_image_fit_,
                         starting_image_palette_dither_method_, starting_color_mode_}) {
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
    connect(surface_obj_path_, &QLineEdit::editingFinished, this, [this] {
        if (!surface_obj_path_->hasAcceptableInput()) {
            const QSignalBlocker blocker(surface_obj_path_);
            surface_obj_path_->setText(
                QString::fromStdString(config_.surface.obj_path));
            status_->setText(tr("The custom OBJ path is invalid."));
            return;
        }
        (void)setSurfaceObjSource(surface_obj_path_->text());
    });
    connect(palette_name_, &QLineEdit::editingFinished, this, [this] {
        if (palette_name_->hasAcceptableInput()) {
            applyGlobalEditor(palette_name_);
        }
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        QWidget* page = tabs_->widget(index);
        if (synchronization_page_ != nullptr
            && !synchronization_page_->isHidden()) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Swings);
        } else if (page == effect_page_) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Effects);
        } else if (page == motion_page_) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Waves);
        }
    });

    connect(preview_, &PreviewWidget::waveSelected, this, [this](std::size_t index) {
        if (index < config_.waves.size()) {
            wave_list_->setCurrentRow(static_cast<int>(index));
            setWorkflowStage(3);
        }
    });
    connect(preview_, &PreviewWidget::waveDragStarted, this,
            [this](std::size_t index) {
                wave_drag_state_.reset();
                if (index >= config_.waves.size()) return;
                ItemDragState state;
                state.layer_uuid = active_layer_uuid_;
                state.item_id = config_.waves[index].id;
                state.before = captureActiveState();
                wave_drag_state_ = std::move(state);
            });
    connect(preview_, &PreviewWidget::waveMoved, this,
            [this](std::size_t index, double x, double y) {
                if (!wave_drag_state_ || index >= config_.waves.size()
                    || wave_drag_state_->layer_uuid != active_layer_uuid_
                    || wave_drag_state_->item_id != config_.waves[index].id) {
                    return;
                }
                if (config_.waves[index].x_percent == x
                    && config_.waves[index].y_percent == y) {
                    return;
                }
                config_.waves[index].x_percent = x;
                config_.waves[index].y_percent = y;
                syncActiveRender();
                wave_drag_state_->moved = true;
                if (selectedWaveIndex() && *selectedWaveIndex() == index) {
                    const QSignalBlocker bx(wave_x_);
                    const QSignalBlocker by(wave_y_);
                    wave_x_->setValue(x);
                    wave_y_->setValue(y);
                }
                preview_->setConfiguration(config_);
                // Reject any older preview immediately, but wait until the
                // gesture finishes before creating a dirty undo command.
                ++document_revision_;
                schedulePreview();
            });
    connect(preview_, &PreviewWidget::waveDragFinished, this,
            [this](std::size_t) {
                if (!wave_drag_state_) return;
                ItemDragState state = std::move(*wave_drag_state_);
                wave_drag_state_.reset();
                if (!state.moved || state.layer_uuid != active_layer_uuid_) return;
                recordActiveStateChange(tr("Move wave"), std::move(state.before));
            });

    connect(preview_, &PreviewWidget::swingSelected, this,
            [this](std::size_t index) {
                if (index < config_.swings.size()) {
                    swing_list_->setCurrentRow(static_cast<int>(index));
                    setDriversExpanded(true);
                }
            });
    connect(preview_, &PreviewWidget::swingDragStarted, this,
            [this](std::size_t index) {
                swing_drag_state_.reset();
                if (index >= config_.swings.size()) return;
                ItemDragState state;
                state.layer_uuid = active_layer_uuid_;
                state.item_id = config_.swings[index].id;
                state.before = captureActiveState();
                swing_drag_state_ = std::move(state);
            });
    connect(preview_, &PreviewWidget::swingMoved, this,
            [this](std::size_t index, double x, double y) {
                if (!swing_drag_state_ || index >= config_.swings.size()
                    || swing_drag_state_->layer_uuid != active_layer_uuid_
                    || swing_drag_state_->item_id != config_.swings[index].id) {
                    return;
                }
                auto& swing = config_.swings[index];
                if (swing.center_x == x && swing.center_y == y) return;
                swing.center_x = x;
                swing.center_y = y;
                syncActiveRender();
                swing_drag_state_->moved = true;
                if (selectedSwingIndex() && *selectedSwingIndex() == index) {
                    const QSignalBlocker bx(swing_center_x_);
                    const QSignalBlocker by(swing_center_y_);
                    swing_center_x_->setValue(x);
                    swing_center_y_->setValue(y);
                }
                preview_->setConfiguration(config_);
                ++document_revision_;
                schedulePreview();
            });
    connect(preview_, &PreviewWidget::swingDragFinished, this,
            [this](std::size_t) {
                if (!swing_drag_state_) return;
                ItemDragState state = std::move(*swing_drag_state_);
                swing_drag_state_.reset();
                if (!state.moved || state.layer_uuid != active_layer_uuid_) return;
                recordActiveStateChange(tr("Move swing area"),
                                        std::move(state.before));
            });

    connect(preview_, &PreviewWidget::effectSelected, this,
            [this](std::size_t index) {
                if (index < config_.effects.size()) {
                    const auto& effect = config_.effects[index];
                    setWorkflowStage(4);
                    setEffectCategory(effect_ui_category(effect.type));
                    refreshEffectList(effect.id);
                }
            });
    connect(preview_, &PreviewWidget::effectDragStarted, this,
            [this](std::size_t index) {
                effect_drag_state_.reset();
                if (index >= config_.effects.size()) return;
                ItemDragState state;
                state.layer_uuid = active_layer_uuid_;
                state.item_id = config_.effects[index].id;
                state.before = captureActiveState();
                effect_drag_state_ = std::move(state);
            });
    connect(preview_, &PreviewWidget::effectMoved, this,
            [this](std::size_t index, double x, double y) {
                if (!effect_drag_state_ || index >= config_.effects.size()
                    || effect_drag_state_->layer_uuid != active_layer_uuid_
                    || effect_drag_state_->item_id != config_.effects[index].id) {
                    return;
                }
                auto& effect = config_.effects[index];
                if (effect.center_x == x && effect.center_y == y) return;
                effect.center_x = x;
                effect.center_y = y;
                syncActiveRender();
                effect_drag_state_->moved = true;
                if (selectedEffectIndex() && *selectedEffectIndex() == index) {
                    const QSignalBlocker bx(effect_center_x_);
                    const QSignalBlocker by(effect_center_y_);
                    effect_center_x_->setValue(x);
                    effect_center_y_->setValue(y);
                }
                preview_->setConfiguration(config_);
                ++document_revision_;
                schedulePreview();
            });
    connect(preview_, &PreviewWidget::effectDragFinished, this,
            [this](std::size_t) {
                if (!effect_drag_state_) return;
                ItemDragState state = std::move(*effect_drag_state_);
                effect_drag_state_.reset();
                if (!state.moved || state.layer_uuid != active_layer_uuid_) return;
                recordActiveStateChange(tr("Move effect area"),
                                        std::move(state.before));
            });
}

pvt::LayerConfig* MainWindow::findLayer(const std::string& uuid) {
    const auto found = std::find_if(project_.layers.begin(), project_.layers.end(),
                                    [&uuid](const auto& layer) {
                                        return layer.uuid == uuid;
                                    });
    return found == project_.layers.end() ? nullptr : &*found;
}

const pvt::LayerConfig* MainWindow::findLayer(const std::string& uuid) const {
    const auto found = std::find_if(project_.layers.begin(), project_.layers.end(),
                                    [&uuid](const auto& layer) {
                                        return layer.uuid == uuid;
                                    });
    return found == project_.layers.end() ? nullptr : &*found;
}

pvt::LayerGroup* MainWindow::findGroup(const std::string& uuid) {
    const auto found = std::find_if(project_.groups.begin(), project_.groups.end(),
                                    [&uuid](const auto& group) {
                                        return group.uuid == uuid;
                                    });
    return found == project_.groups.end() ? nullptr : &*found;
}

const pvt::LayerGroup* MainWindow::findGroup(const std::string& uuid) const {
    const auto found = std::find_if(project_.groups.begin(), project_.groups.end(),
                                    [&uuid](const auto& group) {
                                        return group.uuid == uuid;
                                    });
    return found == project_.groups.end() ? nullptr : &*found;
}

const pvt::LayerGroup* MainWindow::groupForLayer(
    const pvt::LayerConfig& layer) const {
    return layer.group_uuid.empty() ? nullptr : findGroup(layer.group_uuid);
}

pvt::LayerConfig* MainWindow::activeLayer() {
    return findLayer(active_layer_uuid_);
}

const pvt::LayerConfig* MainWindow::activeLayer() const {
    return findLayer(active_layer_uuid_);
}

void MainWindow::loadActiveConfiguration() {
    if (project_.layers.empty()) {
        project_.layers.push_back(pvt::default_layer(0));
    }
    if (activeLayer() == nullptr) {
        active_layer_uuid_ = project_.layers.back().uuid;
    }
    const auto* layer = activeLayer();
    if (layer != nullptr) {
        config_ = pvt::apply_global_config(project_.canvas, project_.output,
                                           layer->render);
    }
    integer_dither_preference_ = project_.output.bit_depth == 32
                                     ? integer_dither_preference_
                                     : project_.output.dither_enabled;
}

void MainWindow::syncActiveRender() {
    if (auto* layer = activeLayer()) {
        layer->render = static_cast<const pvt::RenderData&>(config_);
    }
}

void MainWindow::syncProjectGlobals() {
    project_.canvas.width = config_.width;
    project_.canvas.height = config_.height;
    project_.canvas.block_size = config_.block_size;
    project_.canvas.total_frames = config_.total_frames;
    project_.canvas.fps = config_.fps;
    project_.canvas.clock = config_.clock;
    project_.canvas.audio_reactive_defaults =
        config_.audio_reactive_defaults;
    project_.canvas.live = config_.live;
    project_.canvas.motion_paths = config_.motion_paths;
    project_.canvas.output_compatibility = config_.output_compatibility;
    project_.output = config_.output;
}

void MainWindow::selectLayer(const std::string& uuid) {
    if (findLayer(uuid) == nullptr) {
        loadLayerEditors();
        return;
    }
    selected_group_uuid_.reset();
    if (uuid == active_layer_uuid_) {
        refreshLayerList();
        return;
    }
    active_layer_uuid_ = uuid;
    loadActiveConfiguration();
    refreshAll();
    refreshLayerList();
    schedulePreview();
}

void MainWindow::selectGroup(const std::string& uuid) {
    if (findGroup(uuid) == nullptr) return;
    selected_group_uuid_ = uuid;
    refreshLayerList();
}

void MainWindow::refreshLayerList() {
    if (layer_list_ == nullptr) {
        return;
    }
    const bool was_populating = populating_;
    populating_ = true;
    project_name_->setText(QString::fromStdString(project_.name));
    layer_list_->clear();
    int selected_row = -1;
    int row = 0;
    std::unordered_set<std::string> emitted_groups;
    for (std::size_t display = 0; display < project_.layers.size(); ++display) {
        const std::size_t index = project_.layers.size() - 1U - display;
        const auto& layer = project_.layers[index];
        const pvt::LayerGroup* group = groupForLayer(layer);
        if (group != nullptr && emitted_groups.insert(group->uuid).second) {
            QString group_detail = QStringLiteral("▾ ")
                + QString::fromStdString(group->name)
                + QStringLiteral("  [")
                + (group->enabled ? tr("on") : tr("off"));
            if (group->locked) group_detail += tr(", locked");
            group_detail += QStringLiteral("]");
            if (solo_group_uuid_ && *solo_group_uuid_ == group->uuid) {
                group_detail.append(tr("  [SOLO]"));
            }
            auto* group_item = new QListWidgetItem(group_detail, layer_list_);
            QFont group_font = group_item->font();
            group_font.setBold(true);
            group_item->setFont(group_font);
            group_item->setData(Qt::UserRole,
                                QString::fromStdString(group->uuid));
            group_item->setData(Qt::UserRole + 1, true);
            group_item->setToolTip(
                tr("Layer group. Visibility, lock, solo, and movement apply to every contained layer."));
            if (selected_group_uuid_ && *selected_group_uuid_ == group->uuid) {
                selected_row = row;
            }
            ++row;
        }
        QString detail = (group != nullptr ? QStringLiteral("    ↳ ") : QString{})
                         + QString::fromStdString(layer.name)
                         + QStringLiteral("  [")
                         + (layer.enabled ? tr("on") : tr("off"))
                         + QStringLiteral(", ")
                         + (index == 0U
                                ? tr("base")
                                : QString::fromUtf8(
                                      pvt::blend_mode_name(layer.blend_mode)))
                         + QStringLiteral(", ")
                         + QString::fromUtf8(pvt::alpha_mode_name(
                               layer.alpha_mode))
                         + QStringLiteral(", ")
                         + QString::number(layer.opacity * 100.0, 'f', 0)
                         + QStringLiteral("%]");
        if (solo_layer_uuid_ && *solo_layer_uuid_ == layer.uuid) {
            detail.append(tr("  [SOLO]"));
        }
        auto* item = new QListWidgetItem(detail, layer_list_);
        item->setData(Qt::UserRole, QString::fromStdString(layer.uuid));
        item->setData(Qt::UserRole + 1, false);
        item->setToolTip(index == 0U
                             ? tr("Bottom/base layer. Its blend mode has no lower layer to affect.")
                             : tr("This layer is composited over every enabled layer below it. Eraser modes remove lower-layer coverage without affecting layers above."));
        if (!selected_group_uuid_ && layer.uuid == active_layer_uuid_) {
            selected_row = row;
        }
        ++row;
    }
    layer_list_->setCurrentRow(selected_row);
    populating_ = was_populating;
    loadLayerEditors();
    updateWindowTitle();
}

void MainWindow::loadLayerEditors() {
    if (layer_name_ == nullptr) {
        return;
    }
    const bool was_populating = populating_;
    populating_ = true;
    const auto* layer = activeLayer();
    const auto* owning_group = layer != nullptr ? groupForLayer(*layer) : nullptr;
    const bool locked = owning_group != nullptr && owning_group->locked;
    const bool available = layer != nullptr && !selected_group_uuid_;
    const bool project_actions_available = !music_analysis_active_
                                           && !project_io_active_;
    const bool layer_actions_available = available && !locked
                                         && project_actions_available;
    for (auto* widget : std::initializer_list<QWidget*>{
             layer_name_, layer_enabled_, layer_blend_, layer_alpha_mode_,
             layer_group_, layer_opacity_}) {
        widget->setEnabled(available && !locked);
    }
    layer_solo_->setEnabled(available);
    if (randomize_values_action_ != nullptr) {
        randomize_values_action_->setEnabled(layer_actions_available);
    }
    if (randomize_mix_action_ != nullptr) {
        randomize_mix_action_->setEnabled(layer_actions_available);
    }
    layer_group_->clear();
    layer_group_->addItem(tr("Ungrouped"), QString{});
    for (const auto& group : project_.groups) {
        layer_group_->addItem(QString::fromStdString(group.name),
                              QString::fromStdString(group.uuid));
    }
    if (layer != nullptr) {
        layer_name_->setText(QString::fromStdString(layer->name));
        layer_enabled_->setChecked(layer->enabled);
        layer_solo_->setChecked(solo_layer_uuid_ && *solo_layer_uuid_ == layer->uuid);
        select_enum(layer_blend_, layer->blend_mode);
        select_enum(layer_alpha_mode_, layer->alpha_mode);
        const int group_index = layer_group_->findData(
            QString::fromStdString(layer->group_uuid));
        layer_group_->setCurrentIndex(std::max(0, group_index));
        layer_opacity_->setValue(layer->opacity * 100.0);
        const auto index = static_cast<std::size_t>(
            std::distance(static_cast<const pvt::LayerConfig*>(project_.layers.data()),
                          layer));
        layer_blend_->setEnabled(index != 0U);
        if (locked || !available) layer_blend_->setEnabled(false);
        layer_blend_->setToolTip(index == 0U
                                     ? tr("The bottom layer has nothing beneath it to blend with.")
                                     : tr("Blend applies this layer over the enabled layers below. Eraser modes are destination-out masks and never paint over later layers above."));
    }
    const pvt::LayerGroup* selected_group = selected_group_uuid_
        ? findGroup(*selected_group_uuid_) : nullptr;
    selected_group_box_->setEnabled(selected_group != nullptr);
    if (selected_group != nullptr) {
        group_name_->setText(QString::fromStdString(selected_group->name));
        group_enabled_->setChecked(selected_group->enabled);
        group_solo_->setChecked(solo_group_uuid_
                                && *solo_group_uuid_ == selected_group->uuid);
        group_locked_->setChecked(selected_group->locked);
    } else {
        group_name_->clear();
        group_enabled_->setChecked(false);
        group_solo_->setChecked(false);
        group_locked_->setChecked(false);
    }
    if (tabs_ != nullptr) {
        for (int index = 0; index < tabs_->count(); ++index) {
            QWidget* page = tabs_->widget(index);
            const bool layer_page = page == source_page_ || page == effect_page_
                                    || page == surface_page_
                                    || page == motion_page_
                                    || page == finish_page_;
            page->setEnabled(layer_page ? layer_actions_available
                                        : project_actions_available);
        }
    }
    for (std::size_t index = 0; index < workflow_stage_buttons_.size(); ++index) {
        const bool layer_stage = index >= 1U && index <= 5U;
        workflow_stage_buttons_[index]->setEnabled(
            layer_stage ? layer_actions_available : project_actions_available);
    }
    // This combo is populated dynamically after construction and again after
    // every layer/group change. Recompute its readable floor after the item
    // model changes instead of retaining the empty-combo size hint.
    preserve_control_text_width(layer_group_);
    populating_ = was_populating;
    updateSynchronizationState();
    updateWorkflowSummaries();
}

void MainWindow::addLayer() {
    if (project_.layers.size() >= pvt::kMaximumLayers) {
        QMessageBox::warning(
            this, tr("Layer limit"),
            tr("The signed-int UI/API layer index is exhausted."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    auto layer = pvt::default_layer(project_.layers.size());
    layer.uuid = pvt::generate_uuid();
    layer.file_id = pvt::allocate_layer_file_id(project_);
    layer.name = tr("Layer %1").arg(project_.layers.size() + 1U).toStdString();
    active_layer_uuid_ = layer.uuid;
    selected_group_uuid_.reset();
    project_.layers.push_back(std::move(layer));
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    recordProjectStateChange(tr("Add layer"), std::move(before), before_active);
    schedulePreview();
}

void MainWindow::duplicateLayer() {
    if (selected_group_uuid_) return;
    const auto* source = activeLayer();
    if (source == nullptr || project_.layers.size() >= pvt::kMaximumLayers) {
        return;
    }
    const auto* source_group = groupForLayer(*source);
    if (source_group != nullptr && source_group->locked) {
        status_->setText(tr("Unlock the group before duplicating one of its layers."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    const auto source_index = static_cast<std::size_t>(
        std::distance(static_cast<const pvt::LayerConfig*>(project_.layers.data()),
                      source));
    auto layer = *source;
    layer.uuid = pvt::generate_uuid();
    layer.file_id = pvt::allocate_layer_file_id(project_);
    append_copy_suffix(layer.name);
    const bool copy_obj = !layer.render.surface.obj_sha256.empty();
    const bool copy_height =
        !layer.render.surface.plane_displacement.sha256.empty();
    const bool copy_music =
        !layer.render.layer_clock.clock.music.source_sha256.empty();
    const bool copy_image = !layer.render.starting_image.sha256.empty();
    std::unique_ptr<pvt::ProjectDocument> staged_document;
    if (copy_obj || copy_height || copy_music || copy_image) {
        if (document_ == nullptr) {
            QMessageBox::critical(this, tr("Could not duplicate layer"),
                                  tr("The project attachment registry is unavailable."));
            return;
        }
        try {
            staged_document =
                std::make_unique<pvt::ProjectDocument>(*document_);
        } catch (const std::exception& exception) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The attachment transaction could not be staged: %1")
                    .arg(QString::fromUtf8(exception.what())));
            return;
        } catch (...) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The attachment transaction could not be staged."));
            return;
        }
    }
    if (copy_obj) {
        const pvt::ProjectAttachment* source_attachment =
            pvt::find_project_attachment(
                *staged_document,
                pvt::surface_obj_attachment_id(source->uuid));
        if (source_attachment == nullptr || source_attachment->local_path.empty()) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The embedded custom OBJ source is unavailable."));
            return;
        }
        const std::string source_attachment_path = source_attachment->local_path;
        pvt::ProjectAttachment duplicate_attachment;
        std::string attachment_error;
        if (!pvt::attach_project_file(
                *staged_document, pvt::surface_obj_attachment_id(layer.uuid),
                source_attachment_path, &duplicate_attachment,
                &attachment_error)) {
            QMessageBox::critical(this, tr("Could not duplicate layer"),
                                  QString::fromStdString(attachment_error));
            return;
        }
        layer.render.surface.obj_path = duplicate_attachment.local_path;
        layer.render.surface.obj_sha256 = duplicate_attachment.sha256;
        layer.render.surface.obj_basename = duplicate_attachment.basename;
    }
    if (copy_height) {
        const pvt::ProjectAttachment* source_attachment =
            pvt::find_project_attachment(
                *staged_document,
                pvt::plane_displacement_attachment_id(source->uuid));
        if (source_attachment == nullptr
            || source_attachment->local_path.empty()) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The embedded plane-displacement height map is unavailable."));
            return;
        }
        pvt::ProjectAttachment duplicate_attachment;
        std::string attachment_error;
        if (!pvt::attach_project_file(
                *staged_document,
                pvt::plane_displacement_attachment_id(layer.uuid),
                source_attachment->local_path, &duplicate_attachment,
                &attachment_error)) {
            QMessageBox::critical(this, tr("Could not duplicate layer"),
                                  QString::fromStdString(attachment_error));
            return;
        }
        auto& displacement = layer.render.surface.plane_displacement;
        displacement.path = duplicate_attachment.local_path;
        displacement.sha256 = duplicate_attachment.sha256;
        displacement.basename = duplicate_attachment.basename;
    }
    if (copy_music) {
        const pvt::ProjectAttachment* source_attachment =
            pvt::find_project_attachment(
                *staged_document,
                pvt::layer_music_attachment_id(source->uuid));
        if (source_attachment == nullptr || source_attachment->local_path.empty()) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The embedded active-layer music source is unavailable."));
            return;
        }
        const std::string source_attachment_path = source_attachment->local_path;
        pvt::ProjectAttachment duplicate_attachment;
        std::string attachment_error;
        if (!pvt::attach_project_file(
                *staged_document,
                pvt::layer_music_attachment_id(layer.uuid),
                source_attachment_path, &duplicate_attachment,
                &attachment_error)) {
            QMessageBox::critical(this, tr("Could not duplicate layer"),
                                  QString::fromStdString(attachment_error));
            return;
        }
        layer.render.layer_clock.clock.music.source_sha256 =
            duplicate_attachment.sha256;
        layer.render.layer_clock.clock.music.source_basename =
            duplicate_attachment.basename;
    }
    if (copy_image) {
        const pvt::ProjectAttachment* source_attachment =
            pvt::find_project_attachment(
                *staged_document,
                pvt::starting_image_attachment_id(source->uuid));
        if (source_attachment == nullptr || source_attachment->local_path.empty()) {
            QMessageBox::critical(
                this, tr("Could not duplicate layer"),
                tr("The embedded starting image is unavailable."));
            return;
        }
        pvt::ProjectAttachment duplicate_attachment;
        std::string attachment_error;
        if (!pvt::attach_project_file(
                *staged_document,
                pvt::starting_image_attachment_id(layer.uuid),
                source_attachment->local_path, &duplicate_attachment,
                &attachment_error)) {
            QMessageBox::critical(this, tr("Could not duplicate layer"),
                                  QString::fromStdString(attachment_error));
            return;
        }
        layer.render.starting_image.path = duplicate_attachment.local_path;
        layer.render.starting_image.sha256 = duplicate_attachment.sha256;
        layer.render.starting_image.basename = duplicate_attachment.basename;
    }
    if (staged_document != nullptr) {
        document_ = std::move(staged_document);
    }
    active_layer_uuid_ = layer.uuid;
    selected_group_uuid_.reset();
    project_.layers.insert(project_.layers.begin()
                               + static_cast<std::ptrdiff_t>(source_index + 1U),
                           std::move(layer));
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    recordProjectStateChange(tr("Duplicate layer"), std::move(before), before_active);
    schedulePreview();
    if (playback_timer_->isActive()) startProjectAudioPlayback();
}

void MainWindow::removeLayer() {
    if (selected_group_uuid_) return;
    if (project_.layers.size() <= 1U) {
        QMessageBox::information(this, tr("Keep one layer"),
                                 tr("A project must always contain at least one layer."));
        return;
    }
    auto* layer = activeLayer();
    if (layer == nullptr) return;
    const auto* owning_group = groupForLayer(*layer);
    if (owning_group != nullptr && owning_group->locked) {
        status_->setText(tr("Unlock the group before removing one of its layers."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    const std::string removed_group_uuid = layer->group_uuid;
    const auto index = static_cast<std::size_t>(
        std::distance(project_.layers.data(), layer));
    if (document_ != nullptr) {
        std::unique_ptr<pvt::ProjectDocument> staged_document;
        try {
            staged_document =
                std::make_unique<pvt::ProjectDocument>(*document_);
        } catch (const std::exception& exception) {
            QMessageBox::critical(
                this, tr("Could not remove layer"),
                tr("The attachment transaction could not be staged: %1")
                    .arg(QString::fromUtf8(exception.what())));
            return;
        } catch (...) {
            QMessageBox::critical(
                this, tr("Could not remove layer"),
                tr("The attachment transaction could not be staged."));
            return;
        }
        for (const std::string& reference_id : {
                 pvt::surface_obj_attachment_id(layer->uuid),
                 pvt::plane_displacement_attachment_id(layer->uuid),
                 pvt::starting_image_attachment_id(layer->uuid),
                 pvt::layer_music_attachment_id(layer->uuid)}) {
            std::string detach_error;
            if (!pvt::detach_project_file(
                    *staged_document, reference_id, &detach_error)) {
                QMessageBox::critical(this, tr("Could not remove layer"),
                                      QString::fromStdString(detach_error));
                return;
            }
        }
        document_ = std::move(staged_document);
    }
    remove_layer_live_clock_routes(project_.canvas.live, before_active);
    project_.layers.erase(project_.layers.begin() + static_cast<std::ptrdiff_t>(index));
    if (!removed_group_uuid.empty()
        && std::none_of(project_.layers.begin(), project_.layers.end(),
                        [&removed_group_uuid](const pvt::LayerConfig& value) {
                            return value.group_uuid == removed_group_uuid;
                        })) {
        project_.groups.erase(
            std::remove_if(project_.groups.begin(), project_.groups.end(),
                           [&removed_group_uuid](const pvt::LayerGroup& value) {
                               return value.uuid == removed_group_uuid;
                           }),
            project_.groups.end());
        if (solo_group_uuid_ && *solo_group_uuid_ == removed_group_uuid) {
            solo_group_uuid_.reset();
        }
        if (selected_group_uuid_ && *selected_group_uuid_ == removed_group_uuid) {
            selected_group_uuid_.reset();
        }
    }
    const std::size_t replacement = std::min(index, project_.layers.size() - 1U);
    active_layer_uuid_ = project_.layers[replacement].uuid;
    if (solo_layer_uuid_ && *solo_layer_uuid_ == before_active) {
        solo_layer_uuid_.reset();
    }
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    recordProjectStateChange(tr("Remove layer"), std::move(before), before_active);
    schedulePreview();
    if (playback_timer_->isActive()) startProjectAudioPlayback();
}

void MainWindow::moveActiveLayer(int direction) {
    if (selected_group_uuid_) return;
    auto* layer = activeLayer();
    if (layer == nullptr || direction == 0) return;
    const pvt::LayerGroup* group = groupForLayer(*layer);
    if (group != nullptr && group->locked) {
        status_->setText(tr("Unlock the group before moving one of its layers."));
        return;
    }
    const auto index = static_cast<std::ptrdiff_t>(
        std::distance(project_.layers.data(), layer));
    const auto target = index + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(project_.layers.size())) return;
    const std::string target_group =
        project_.layers[static_cast<std::size_t>(target)].group_uuid;
    if (group != nullptr && target_group != group->uuid) {
        status_->setText(
            tr("Move the group to keep its folder contiguous, or change this layer's Group field first."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    if (group == nullptr && !target_group.empty()) {
        const pvt::LayerGroup* target_definition = findGroup(target_group);
        if (target_definition != nullptr && target_definition->locked) {
            status_->setText(tr("Unlock the adjacent group before moving across it."));
            return;
        }
        std::size_t group_begin = static_cast<std::size_t>(target);
        std::size_t group_end = group_begin;
        while (group_begin > 0U
               && project_.layers[group_begin - 1U].group_uuid
                      == target_group) --group_begin;
        while (group_end + 1U < project_.layers.size()
               && project_.layers[group_end + 1U].group_uuid
                      == target_group) ++group_end;
        if (direction > 0) {
            std::rotate(project_.layers.begin() + index,
                        project_.layers.begin() + index + 1,
                        project_.layers.begin()
                            + static_cast<std::ptrdiff_t>(group_end + 1U));
        } else {
            std::rotate(project_.layers.begin()
                            + static_cast<std::ptrdiff_t>(group_begin),
                        project_.layers.begin() + index,
                        project_.layers.begin() + index + 1);
        }
    } else {
        std::swap(project_.layers[static_cast<std::size_t>(index)],
                  project_.layers[static_cast<std::size_t>(target)]);
    }
    refreshLayerList();
    recordProjectStateChange(direction > 0 ? tr("Move layer up") : tr("Move layer down"),
                             std::move(before), before_active);
    schedulePreview();
}

void MainWindow::addGroup() {
    auto* layer = activeLayer();
    if (layer == nullptr || selected_group_uuid_) return;
    if (!layer->group_uuid.empty()) {
        status_->setText(
            tr("This layer is already grouped. Choose Ungrouped first or select another layer."));
        return;
    }
    if (project_.groups.size() >= pvt::kMaximumLayerGroups) {
        QMessageBox::warning(
            this, tr("Group limit"),
            tr("The signed-int UI/API group index is exhausted."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    pvt::LayerGroup group;
    group.uuid = pvt::generate_uuid();
    group.name = tr("Group %1").arg(project_.groups.size() + 1U).toStdString();
    layer->group_uuid = group.uuid;
    project_.groups.push_back(group);
    selected_group_uuid_ = group.uuid;
    refreshLayerList();
    recordProjectStateChange(tr("Add group"), std::move(before), before_active);
}

void MainWindow::removeSelectedGroup() {
    if (!selected_group_uuid_) return;
    auto* group = findGroup(*selected_group_uuid_);
    if (group == nullptr) return;
    if (group->locked) {
        status_->setText(tr("Unlock the group before removing it."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    const std::string uuid = group->uuid;
    for (auto& layer : project_.layers) {
        if (layer.group_uuid == uuid) layer.group_uuid.clear();
    }
    project_.groups.erase(
        std::remove_if(project_.groups.begin(), project_.groups.end(),
                       [&uuid](const pvt::LayerGroup& value) {
                           return value.uuid == uuid;
                       }),
        project_.groups.end());
    if (solo_group_uuid_ && *solo_group_uuid_ == uuid) {
        solo_group_uuid_.reset();
    }
    selected_group_uuid_.reset();
    refreshLayerList();
    recordProjectStateChange(tr("Remove group and keep layers"),
                             std::move(before), before_active);
    schedulePreview();
}

void MainWindow::moveSelectedGroup(int direction) {
    if (!selected_group_uuid_ || direction == 0) return;
    const auto* group = findGroup(*selected_group_uuid_);
    if (group == nullptr) return;
    if (group->locked) {
        status_->setText(tr("Unlock the group before moving it."));
        return;
    }
    std::size_t begin = project_.layers.size();
    std::size_t end = 0U;
    for (std::size_t index = 0U; index < project_.layers.size(); ++index) {
        if (project_.layers[index].group_uuid == group->uuid) {
            begin = std::min(begin, index);
            end = std::max(end, index);
        }
    }
    if (begin == project_.layers.size()) return;
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    if (direction > 0) {
        if (end + 1U >= project_.layers.size()) return;
        std::size_t adjacent_end = end + 1U;
        const std::string adjacent_group =
            project_.layers[adjacent_end].group_uuid;
        if (!adjacent_group.empty()) {
            const auto* adjacent = findGroup(adjacent_group);
            if (adjacent != nullptr && adjacent->locked) {
                status_->setText(tr("Unlock the adjacent group before moving across it."));
                return;
            }
            while (adjacent_end + 1U < project_.layers.size()
                   && project_.layers[adjacent_end + 1U].group_uuid
                          == adjacent_group) ++adjacent_end;
        }
        std::rotate(project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(begin),
                    project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(end + 1U),
                    project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(adjacent_end + 1U));
    } else {
        if (begin == 0U) return;
        std::size_t adjacent_begin = begin - 1U;
        const std::string adjacent_group =
            project_.layers[adjacent_begin].group_uuid;
        if (!adjacent_group.empty()) {
            const auto* adjacent = findGroup(adjacent_group);
            if (adjacent != nullptr && adjacent->locked) {
                status_->setText(tr("Unlock the adjacent group before moving across it."));
                return;
            }
            while (adjacent_begin > 0U
                   && project_.layers[adjacent_begin - 1U].group_uuid
                          == adjacent_group) --adjacent_begin;
        }
        std::rotate(project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(adjacent_begin),
                    project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(begin),
                    project_.layers.begin()
                        + static_cast<std::ptrdiff_t>(end + 1U));
    }
    refreshLayerList();
    recordProjectStateChange(direction > 0 ? tr("Move group up")
                                           : tr("Move group down"),
                             std::move(before), before_active);
    schedulePreview();
}

void MainWindow::setActiveLayerGroup(const std::string& group_uuid) {
    auto* layer = activeLayer();
    if (layer == nullptr || layer->group_uuid == group_uuid) return;
    const auto* source_group = groupForLayer(*layer);
    const auto* target_group = group_uuid.empty() ? nullptr
                                                   : findGroup(group_uuid);
    if ((!group_uuid.empty() && target_group == nullptr)
        || (source_group != nullptr && source_group->locked)
        || (target_group != nullptr && target_group->locked)) {
        loadLayerEditors();
        status_->setText(tr("Unlock both source and destination groups before moving a layer."));
        return;
    }
    auto before = captureProjectState();
    const std::string before_active = active_layer_uuid_;
    const std::string source_uuid = layer->group_uuid;
    const std::size_t original_index = static_cast<std::size_t>(
        std::distance(project_.layers.data(), layer));
    pvt::LayerConfig moved = std::move(*layer);
    project_.layers.erase(project_.layers.begin()
                          + static_cast<std::ptrdiff_t>(original_index));
    moved.group_uuid = group_uuid;

    const bool source_still_used = !source_uuid.empty()
        && std::any_of(project_.layers.begin(), project_.layers.end(),
                       [&source_uuid](const pvt::LayerConfig& value) {
                           return value.group_uuid == source_uuid;
                       });
    if (!source_uuid.empty() && !source_still_used) {
        project_.groups.erase(
            std::remove_if(project_.groups.begin(), project_.groups.end(),
                           [&source_uuid](const pvt::LayerGroup& value) {
                               return value.uuid == source_uuid;
                           }),
            project_.groups.end());
        if (solo_group_uuid_ && *solo_group_uuid_ == source_uuid) {
            solo_group_uuid_.reset();
        }
    }

    std::size_t insertion = std::min(original_index, project_.layers.size());
    if (!group_uuid.empty()) {
        for (std::size_t index = 0U; index < project_.layers.size(); ++index) {
            if (project_.layers[index].group_uuid == group_uuid) {
                insertion = index + 1U;
            }
        }
    } else if (source_still_used) {
        for (std::size_t index = 0U; index < project_.layers.size(); ++index) {
            if (project_.layers[index].group_uuid == source_uuid) {
                insertion = index + 1U;
            }
        }
    }
    project_.layers.insert(project_.layers.begin()
                               + static_cast<std::ptrdiff_t>(insertion),
                           std::move(moved));
    refreshLayerList();
    recordProjectStateChange(group_uuid.empty() ? tr("Ungroup layer")
                                                 : tr("Move layer into group"),
                             std::move(before), before_active);
    schedulePreview();
}

bool MainWindow::setSurfaceObjSource(const QString& source_path) {
    if (populating_ || activeLayer() == nullptr) return false;
    if (!source_path.isEmpty()
        && source_path.toStdString() == config_.surface.obj_path) return true;
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            pvt::default_project_document());
        document_->project = project_;
    }

    auto before = captureActiveState();
    const std::string reference_id =
        pvt::surface_obj_attachment_id(active_layer_uuid_);
    std::string attachment_error;
    if (source_path.isEmpty()) {
        if (!pvt::detach_project_file(*document_, reference_id,
                                      &attachment_error)) {
            QMessageBox::critical(this, tr("Could not clear custom OBJ"),
                                  QString::fromStdString(attachment_error));
            return false;
        }
        config_.surface.obj_path.clear();
        config_.surface.obj_sha256.clear();
        config_.surface.obj_basename.clear();
        if (config_.surface.mapping == pvt::SurfaceMapping::CustomObj) {
            config_.surface.mapping = pvt::SurfaceMapping::Plane;
        }
    } else {
        const QString resolved = QDir::isAbsolutePath(source_path)
            ? QDir::cleanPath(source_path)
            : QDir::cleanPath(
                  QDir(startup_working_directory_).absoluteFilePath(source_path));
        pvt::ProjectAttachment attached;
        if (!pvt::attach_project_file(
                *document_, reference_id, resolved.toStdString(), &attached,
                &attachment_error)) {
            const QSignalBlocker blocker(surface_obj_path_);
            surface_obj_path_->setText(
                QString::fromStdString(config_.surface.obj_path));
            QMessageBox::critical(this, tr("Could not embed custom OBJ"),
                                  QString::fromStdString(attachment_error));
            return false;
        }
        config_.surface.obj_path = attached.local_path;
        config_.surface.obj_sha256 = attached.sha256;
        config_.surface.obj_basename = attached.basename;
        config_.surface.mapping = pvt::SurfaceMapping::CustomObj;
    }

    syncActiveRender();
    document_->project = project_;
    document_->dirty = true;
    {
        const QSignalBlocker path_blocker(surface_obj_path_);
        const QSignalBlocker mapping_blocker(surface_mapping_);
        surface_obj_path_->setText(
            QString::fromStdString(config_.surface.obj_path));
        select_enum(surface_mapping_, config_.surface.mapping);
    }
    preview_->setConfiguration(config_);
    schedulePreview();
    recordActiveStateChange(
        source_path.isEmpty() ? tr("Clear custom OBJ")
                              : tr("Embed custom OBJ"),
        std::move(before));
    status_->setText(source_path.isEmpty()
        ? tr("Cleared the active layer's embedded custom OBJ.")
        : tr("Embedded %1 for the active layer.")
              .arg(QString::fromStdString(config_.surface.obj_basename)));
    return true;
}

bool MainWindow::setPlaneDisplacementSource(const QString& source_path) {
    if (populating_ || activeLayer() == nullptr) return false;
    const QString suffix = QFileInfo(source_path).suffix();
    if (!source_path.isEmpty()
        && suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0
        && suffix.compare(QStringLiteral("exr"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(
            this, tr("Unsupported height map"),
            tr("Plane displacement accepts full-precision PNG and HALF/FLOAT "
               "scanline OpenEXR height maps."));
        return false;
    }
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            pvt::default_project_document());
        document_->project = project_;
    }

    auto before = captureActiveState();
    auto& displacement = config_.surface.plane_displacement;
    const std::string reference_id =
        pvt::plane_displacement_attachment_id(active_layer_uuid_);
    std::string attachment_error;
    if (source_path.isEmpty()) {
        if (!pvt::detach_project_file(
                *document_, reference_id, &attachment_error)) {
            QMessageBox::critical(
                this, tr("Could not clear height map"),
                QString::fromStdString(attachment_error));
            return false;
        }
        displacement.enabled = false;
        displacement.path.clear();
        displacement.sha256.clear();
        displacement.basename.clear();
    } else {
        const QString resolved = QDir::isAbsolutePath(source_path)
            ? QDir::cleanPath(source_path)
            : QDir::cleanPath(
                  QDir(startup_working_directory_).absoluteFilePath(
                      source_path));
        if (!pvt::detail::validate_data_image_source(
                resolved.toStdString(), &attachment_error)) {
            QMessageBox::critical(
                this, tr("Could not decode height map"),
                QString::fromStdString(attachment_error));
            return false;
        }
        pvt::ProjectAttachment attached;
        if (!pvt::attach_project_file(
                *document_, reference_id, resolved.toStdString(), &attached,
                &attachment_error)) {
            QMessageBox::critical(
                this, tr("Could not embed height map"),
                QString::fromStdString(attachment_error));
            return false;
        }
        displacement.enabled = true;
        displacement.path = attached.local_path;
        displacement.sha256 = attached.sha256;
        displacement.basename = attached.basename;
        config_.surface.enabled = true;
        config_.surface.mapping = pvt::SurfaceMapping::Plane;
    }

    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    document_->project = project_;
    document_->dirty = true;
    {
        const QSignalBlocker use_blocker(
            surface_plane_displacement_enabled_);
        const QSignalBlocker surface_blocker(surface_enabled_);
        const QSignalBlocker mapping_blocker(surface_mapping_);
        surface_plane_displacement_enabled_->setChecked(
            displacement.enabled);
        surface_plane_displacement_path_->setText(
            QString::fromStdString(displacement.basename));
        surface_enabled_->setChecked(config_.surface.enabled);
        select_enum(surface_mapping_, config_.surface.mapping);
    }
    preview_->setConfiguration(config_);
    updateSurfaceEditorState();
    schedulePreview();
    recordActiveStateChange(
        source_path.isEmpty() ? tr("Clear plane height map")
                              : tr("Embed plane height map"),
        std::move(before));
    status_->setText(
        source_path.isEmpty()
            ? tr("Cleared the active layer's plane-displacement height map.")
            : tr("Embedded %1 and rebuilt the active layer's displacement plane.")
                  .arg(QString::fromStdString(displacement.basename)));
    return true;
}

void MainWindow::exportPlaneDisplacementObj() {
    const auto& displacement = config_.surface.plane_displacement;
    if (displacement.path.empty()) {
        QMessageBox::information(
            this, tr("No height map"),
            tr("Choose a plane-displacement height map before exporting its mesh."));
        return;
    }
    QString suggested = QString::fromStdString(displacement.basename);
    const QFileInfo suggested_info(suggested);
    if (!suggested_info.completeBaseName().isEmpty()) {
        suggested = suggested_info.completeBaseName();
    }
    suggested += QStringLiteral("-displaced-%1x%2.obj")
                     .arg(config_.width)
                     .arg(config_.height);
    QString destination = QFileDialog::getSaveFileName(
        this, tr("Export output-resolution displacement plane"),
        usableDialogDirectory({}) + QDir::separator() + suggested,
        tr("Wavefront OBJ (*.obj)"));
    if (destination.isEmpty()) return;
    if (!destination.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive)) {
        destination += QStringLiteral(".obj");
    }
    rememberDialogLocation(destination);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::shared_ptr<const pvt::detail::ObjMesh> mesh;
    std::string mesh_error;
    const bool loaded = pvt::detail::load_displacement_plane_mesh(
        displacement, config_.width, config_.height, mesh, nullptr,
        &mesh_error);
    if (!loaded || !mesh) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(
            this, tr("Could not build displacement plane"),
            QString::fromStdString(mesh_error));
        return;
    }

    QSaveFile file(destination);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QApplication::restoreOverrideCursor();
        QMessageBox::critical(
            this, tr("Could not export OBJ"), file.errorString());
        return;
    }
    QTextStream stream(&file);
    stream.setLocale(QLocale::c());
    stream.setRealNumberNotation(QTextStream::SmartNotation);
    stream.setRealNumberPrecision(17);
    stream << "# Procedural Visualizer Tool displacement plane\n"
           << "# render_resolution " << config_.width << ' '
           << config_.height << "\n"
           << "# pixels_per_node " << displacement.pixels_per_node << "\n"
           << "o PVT_Displacement_Plane\n";
    for (std::size_t index = 0U; index < mesh->positions.size(); ++index) {
        const auto& position = mesh->positions[index];
        const double scale = mesh->normalization_scale;
        stream << "v "
               << (position.x - mesh->normalization_center.x) * scale << ' '
               << (position.y - mesh->normalization_center.y) * scale << ' '
               << (position.z - mesh->normalization_center.z) * scale << '\n';
        if ((index & 16383U) == 0U) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
    for (const auto& uv : mesh->texcoords) {
        stream << "vt " << uv.x << ' ' << uv.y << '\n';
    }
    for (const auto& normal : mesh->normals) {
        stream << "vn " << normal.x << ' ' << normal.y << ' '
               << normal.z << '\n';
    }
    for (std::size_t index = 0U; index < mesh->triangles.size(); ++index) {
        stream << "f";
        for (const auto& corner : mesh->triangles[index].corners) {
            const qulonglong position =
                static_cast<qulonglong>(corner.position) + 1U;
            const qulonglong texcoord =
                static_cast<qulonglong>(corner.texcoord) + 1U;
            const qulonglong normal =
                static_cast<qulonglong>(corner.normal) + 1U;
            stream << ' ' << position << '/' << texcoord << '/' << normal;
        }
        stream << '\n';
        if ((index & 16383U) == 0U) {
            QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
    }
    stream.flush();
    const bool written = stream.status() == QTextStream::Ok
                         && file.commit();
    const QString file_error = file.errorString();
    QApplication::restoreOverrideCursor();
    if (!written) {
        QMessageBox::critical(
            this, tr("Could not export OBJ"), file_error);
        return;
    }
    status_->setText(
        tr("Exported %1 vertices and %2 triangles to %3.")
            .arg(static_cast<qulonglong>(mesh->positions.size()))
            .arg(static_cast<qulonglong>(mesh->triangles.size()))
            .arg(destination));
}

bool MainWindow::setStartingImageSource(const QString& source_path) {
    if (populating_ || activeLayer() == nullptr) return false;
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            pvt::default_project_document());
        document_->project = project_;
    }
    const QString suffix = QFileInfo(source_path).suffix();
    if (!source_path.isEmpty()
        && suffix.compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0
        && suffix.compare(QStringLiteral("exr"), Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, tr("Unsupported starting image"),
                             tr("PVT accepts full-precision PNG and HALF/FLOAT "
                                "scanline OpenEXR starting images. Samples are "
                                "decoded directly to float32."));
        return false;
    }

    auto before = captureActiveState();
    const std::string reference_id =
        pvt::starting_image_attachment_id(active_layer_uuid_);
    std::string attachment_error;
    if (source_path.isEmpty()) {
        if (!pvt::detach_project_file(*document_, reference_id,
                                      &attachment_error)) {
            QMessageBox::critical(this, tr("Could not clear starting image"),
                                  QString::fromStdString(attachment_error));
            return false;
        }
        config_.starting_image = {};
    } else {
        const QString resolved = QDir::isAbsolutePath(source_path)
            ? QDir::cleanPath(source_path)
            : QDir::cleanPath(
                  QDir(startup_working_directory_).absoluteFilePath(source_path));
        if (!pvt::detail::validate_starting_image_source(
                resolved.toStdString(), &attachment_error)) {
            QMessageBox::critical(this, tr("Could not decode starting image"),
                                  QString::fromStdString(attachment_error));
            return false;
        }
        pvt::ProjectAttachment attached;
        if (!pvt::attach_project_file(
                *document_, reference_id, resolved.toStdString(), &attached,
                &attachment_error)) {
            QMessageBox::critical(this, tr("Could not embed starting image"),
                                  QString::fromStdString(attachment_error));
            return false;
        }
        config_.starting_image.enabled = true;
        config_.starting_image.path = attached.local_path;
        config_.starting_image.sha256 = attached.sha256;
        config_.starting_image.basename = attached.basename;
    }

    syncActiveRender();
    syncProjectGlobals();
    document_->project = project_;
    document_->dirty = true;
    {
        const QSignalBlocker enabled_blocker(starting_image_enabled_);
        starting_image_enabled_->setChecked(config_.starting_image.enabled);
        starting_image_path_->setText(
            QString::fromStdString(config_.starting_image.basename));
    }
    preview_->setConfiguration(config_);
    updateOutputEditorValidity();
    schedulePreview();
    recordActiveStateChange(
        source_path.isEmpty() ? tr("Clear starting image")
                              : tr("Embed starting image"),
        std::move(before));
    status_->setText(source_path.isEmpty()
        ? tr("Cleared the active layer's starting image.")
        : tr("Embedded %1 as the active layer's starting image.")
              .arg(QString::fromStdString(config_.starting_image.basename)));
    return true;
}

MainWindow::ActiveDocumentState MainWindow::captureActiveState() const {
    ActiveDocumentState state;
    if (const auto* layer = activeLayer()) {
        state.render = layer->render;
    }
    state.canvas = project_.canvas;
    state.output = project_.output;
    if (document_ != nullptr) {
        state.attachments = document_->attachments;
        state.attachment_cache = document_->attachment_cache;
    }
    return state;
}

MainWindow::ProjectDocumentState MainWindow::captureProjectState() const {
    ProjectDocumentState state;
    state.project = project_;
    if (document_ != nullptr) {
        state.attachments = document_->attachments;
        state.attachment_cache = document_->attachment_cache;
    }
    return state;
}

void MainWindow::restoreActiveState(const std::string& layer_uuid,
                                    const ActiveDocumentState& state) {
    restoring_undo_ = true;
    if (auto* layer = findLayer(layer_uuid)) {
        layer->render = state.render;
    }
    project_.canvas = state.canvas;
    project_.output = state.output;
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            pvt::default_project_document());
    }
    document_->attachments = state.attachments;
    document_->attachment_cache = state.attachment_cache;
    document_->project = project_;
    if (findLayer(active_layer_uuid_) == nullptr) {
        active_layer_uuid_ = layer_uuid;
    }
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    if (playback_timer_ != nullptr && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
    restoring_undo_ = false;
    noteDocumentChange();
    schedulePreview();
}

void MainWindow::recordActiveStateChange(const QString& text,
                                         ActiveDocumentState before,
                                         const QString& merge_key) {
    if (restoring_undo_) return;
    // Attachment pickers, reusable-path edits, and other compound controls can
    // change transparency without passing through the individual alpha-aware
    // editor branches. Keep the final-output invariant inside the transaction
    // so Undo/Redo restores the matching output choice as well.
    ensureAlphaForTransparency();
    syncProjectGlobals();
    const std::string uuid = active_layer_uuid_;
    ActiveDocumentState after = captureActiveState();
    if (render_data_equal(before.render, after.render,
                          &before.canvas.motion_paths,
                          &after.canvas.motion_paths)
        && output_data_equal(before.canvas, before.output,
                             after.canvas, after.output)
        && attachments_equal(before.attachments, after.attachments)) {
        return;
    }
    const std::size_t before_bytes = saturating_add(
        saturating_add(estimated_render_data_bytes(before.render),
                       estimated_canvas_bytes(before.canvas)),
        saturating_add(estimated_output_bytes(before.output),
                       estimated_attachment_bytes(before.attachments)));
    const std::size_t after_bytes = saturating_add(
        saturating_add(estimated_render_data_bytes(after.render),
                       estimated_canvas_bytes(after.canvas)),
        saturating_add(estimated_output_bytes(after.output),
                       estimated_attachment_bytes(after.attachments)));
    const std::size_t payload_bytes = saturating_add(
        saturating_add(before_bytes, after_bytes),
        estimated_string_bytes(uuid));
    auto before_state = std::make_shared<ActiveDocumentState>(std::move(before));
    auto after_state = std::make_shared<ActiveDocumentState>(std::move(after));
    recordUndo(text,
               [this, uuid, before_state] {
                   restoreActiveState(uuid, *before_state);
               },
               [this, uuid, after_state] {
                   restoreActiveState(uuid, *after_state);
               },
               merge_key, payload_bytes);
    noteDocumentChange();
}

void MainWindow::restoreProjectState(const ProjectDocumentState& state,
                                     const std::string& active_uuid) {
    cancelMusicAnalysis();
    restoring_undo_ = true;
    project_ = state.project;
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            pvt::default_project_document());
    }
    document_->attachments = state.attachments;
    document_->attachment_cache = state.attachment_cache;
    document_->project = project_;
    if (project_.layers.empty()) {
        project_.layers.push_back(pvt::default_layer(0));
    }
    active_layer_uuid_ = findLayer(active_uuid) != nullptr
                             ? active_uuid : project_.layers.back().uuid;
    if (solo_layer_uuid_ && findLayer(*solo_layer_uuid_) == nullptr) {
        solo_layer_uuid_.reset();
    }
    if (solo_group_uuid_ && findGroup(*solo_group_uuid_) == nullptr) {
        solo_group_uuid_.reset();
    }
    if (selected_group_uuid_ && findGroup(*selected_group_uuid_) == nullptr) {
        selected_group_uuid_.reset();
    }
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    if (playback_timer_ != nullptr && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
    restoring_undo_ = false;
    noteDocumentChange();
    schedulePreview();
}

void MainWindow::recordProjectStateChange(const QString& text,
                                          ProjectDocumentState before,
                                          const std::string& before_active_uuid) {
    if (restoring_undo_) return;
    // Layer removal and layer/group reordering can expose an eraser above the
    // last full-coverage layer. Re-evaluate after every structural transaction
    // rather than relying only on scalar editor callbacks.
    ensureAlphaForTransparency();
    syncProjectGlobals();
    ProjectDocumentState after = captureProjectState();
    if (project_config_equal(before.project, after.project)
        && attachments_equal(before.attachments, after.attachments)
        && before_active_uuid == active_layer_uuid_) {
        return;
    }
    const std::string after_active = active_layer_uuid_;
    const std::size_t payload_bytes = saturating_add(
        saturating_add(
            saturating_add(estimated_project_bytes(before.project),
                           estimated_attachment_bytes(before.attachments)),
            saturating_add(estimated_project_bytes(after.project),
                           estimated_attachment_bytes(after.attachments))),
        saturating_add(estimated_string_bytes(before_active_uuid),
                       estimated_string_bytes(after_active)));
    auto before_state = std::make_shared<ProjectDocumentState>(std::move(before));
    auto after_state = std::make_shared<ProjectDocumentState>(std::move(after));
    recordUndo(text,
               [this, before_state, before_active_uuid] {
                   restoreProjectState(*before_state, before_active_uuid);
               },
               [this, after_state, after_active] {
                   restoreProjectState(*after_state, after_active);
               }, {}, payload_bytes);
    noteDocumentChange();
}

void MainWindow::recordUndo(const QString& text, std::function<void()> undo,
                            std::function<void()> redo, const QString& merge_key,
                            std::size_t estimated_payload_bytes) {
    if (restoring_undo_ || undo_stack_ == nullptr) return;
    if (estimated_payload_bytes == 0U) {
        estimated_payload_bytes = 1024U;
    }
    bool merges_with_top = false;
    if (!merge_key.isEmpty() && undo_stack_->index() == undo_stack_->count()
        && undo_stack_->count() > 0) {
        const auto* top = dynamic_cast<const LambdaUndoCommand*>(
            undo_stack_->command(undo_stack_->count() - 1));
        merges_with_top = top != nullptr && top->mergeKey() == merge_key;
    }
    try {
        undo_stack_->push(new LambdaUndoCommand(
            text, std::move(undo), std::move(redo), merge_key));
        if (!merges_with_top) {
            undo_history_estimated_bytes_ = saturating_add(
                undo_history_estimated_bytes_, estimated_payload_bytes);
        }
    } catch (const std::bad_alloc&) {
        clearUndoHistory(true);
        baseline_dirty_ = true;
        if (status_ != nullptr) {
            status_->setText(
                tr("Change kept, but there was not enough memory to record Undo."));
        }
        updateWindowTitle();
    }
}

void MainWindow::clearUndoHistory(bool preserve_dirty_state) {
    if (undo_stack_ == nullptr) return;
    const bool history_was_dirty = !undo_stack_->isClean();
    undo_stack_->clear();
    undo_history_estimated_bytes_ = 0U;
    if (preserve_dirty_state && history_was_dirty) {
        baseline_dirty_ = true;
    }
}

void MainWindow::noteDocumentChange() {
    ++document_revision_;
    updateWindowTitle();
    // Every in-flight preview is revision-gated. Even metadata/output-only
    // edits must queue a replacement, otherwise their revision bump could
    // reject the only pending image and leave an empty/stale preview.
    schedulePreview();
}

void MainWindow::updateWindowTitle() {
    const QString name = QString::fromStdString(project_.name.empty()
                                                    ? std::string("Untitled Fire")
                                                    : project_.name);
    setWindowFilePath(current_project_path_);
    setWindowModified(hasUnsavedChanges());
    setWindowTitle(tr("%1[*] — PVT %2")
                       .arg(name, QStringLiteral(PVT_PROGRAM_VERSION)));
}

void MainWindow::updateCompatibilityWarning() {
    compatibility_warning_.clear();
    if (document_ != nullptr) {
        const pvt::ProjectRecoveryInfo recovery =
            pvt::project_recovery_info(document_->project);
        if (recovery.preserved_fields != 0U || !recovery.notes.empty()) {
            compatibility_warning_ = tr(
                "Recovered this save by applying every safe setting and repairing "
                "missing or unusable data. Preserved %1 original/unrecognized "
                "field(s); %2 were not safe to use. Saving keeps them.")
                .arg(recovery.preserved_fields)
                .arg(recovery.rejected_fields);
        }
    }
    if (compatibility_warning_label_ != nullptr) {
        compatibility_warning_label_->setText(compatibility_warning_);
        compatibility_warning_label_->setVisible(!compatibility_warning_.isEmpty());
    }
    if (!compatibility_warning_.isEmpty() && status_ != nullptr) {
        status_->setText(compatibility_warning_);
    }
}

bool MainWindow::hasUnsavedChanges() const {
    return baseline_dirty_
           || (undo_stack_ != nullptr && !undo_stack_->isClean());
}

bool MainWindow::confirmDiscardChanges(std::function<void()> after_save) {
    if (!hasUnsavedChanges()) {
        return true;
    }
    const auto choice = QMessageBox::warning(
        this, tr("Unsaved project changes"),
        tr("Save the changes to “%1” before continuing?")
            .arg(QString::fromStdString(project_.name)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);
    if (choice == QMessageBox::Cancel) return false;
    if (choice == QMessageBox::Discard) return true;
    saveSetup();
    if (project_io_active_) {
        project_io_success_continuation_ = std::move(after_save);
    }
    return !hasUnsavedChanges();
}

bool MainWindow::documentReplacementAllowed(QString* error) {
    if (project_io_active_) {
        const QString message =
            tr("Finish the active project load or save before replacing the project or version.");
        if (error != nullptr) {
            *error = message;
        } else if (status_ != nullptr) {
            status_->setText(message);
        }
        return false;
    }
    if (export_watcher_ == nullptr || !export_watcher_->isRunning()) {
        return true;
    }
    const QString message =
        tr("Cancel or finish the active export before replacing the project or version.");
    if (error != nullptr) {
        *error = message;
    } else if (status_ != nullptr) {
        status_->setText(message);
    }
    return false;
}

void MainWindow::addRecentProject(const QString& path) {
    if (path.isEmpty()) return;
    const QString absolute_path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (absolute_path.isEmpty()) return;
    QSettings settings;
    QJsonArray entries;
    const QJsonDocument saved = QJsonDocument::fromJson(
        settings.value(QStringLiteral("recentProjects/entries")).toByteArray());
    if (saved.isArray()) entries = saved.array();
    for (qsizetype index = entries.size() - 1; index >= 0; --index) {
        if (QDir::cleanPath(entries.at(index).toObject()
                                .value(QStringLiteral("path")).toString())
                == absolute_path) {
            entries.removeAt(index);
        }
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), QString::fromStdString(project_.name));
    entry.insert(QStringLiteral("path"), absolute_path);
    entries.prepend(entry);
    settings.setValue(QStringLiteral("recentProjects/entries"),
                      QJsonDocument(entries).toJson(QJsonDocument::Compact));
    refreshRecentProjectsMenu();
}

void MainWindow::refreshRecentProjectsMenu() {
    if (recent_projects_menu_ == nullptr) return;
    recent_projects_menu_->clear();
    if (recent_project_limit_ <= 0) {
        auto* disabled = recent_projects_menu_->addAction(tr("Recent projects disabled"));
        disabled->setEnabled(false);
        return;
    }
    QSettings settings;
    const QJsonDocument saved = QJsonDocument::fromJson(
        settings.value(QStringLiteral("recentProjects/entries")).toByteArray());
    const QJsonArray entries = saved.isArray() ? saved.array() : QJsonArray{};
    int shown = 0;
    for (const QJsonValue& value : entries) {
        if (shown >= recent_project_limit_) break;
        const QJsonObject entry = value.toObject();
        const QString path = QDir::cleanPath(
            entry.value(QStringLiteral("path")).toString());
        if (path.isEmpty() || !QDir::isAbsolutePath(path)) continue;
        QString name = entry.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) name = QFileInfo(path).completeBaseName();
        auto* action = recent_projects_menu_->addAction(
            tr("%1 — %2").arg(name, QDir::toNativeSeparators(path)));
        action->setToolTip(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (!QFileInfo::exists(path)) {
                QMessageBox::warning(
                    this, tr("Recent project is unavailable"),
                    tr("The project no longer exists at:\n%1\n\nIts recent entry will be removed.")
                        .arg(path));
                QSettings settings;
                QJsonArray retained;
                const QJsonDocument saved = QJsonDocument::fromJson(
                    settings.value(QStringLiteral("recentProjects/entries")).toByteArray());
                for (const QJsonValue& value : saved.array()) {
                    if (QDir::cleanPath(value.toObject()
                                            .value(QStringLiteral("path")).toString())
                        != path) {
                        retained.append(value);
                    }
                }
                settings.setValue(
                    QStringLiteral("recentProjects/entries"),
                    QJsonDocument(retained).toJson(QJsonDocument::Compact));
                refreshRecentProjectsMenu();
                return;
            }
            if (!documentReplacementAllowed()) return;
            if (!confirmDiscardChanges(
                    [this, path] { startProjectLoad(path); })) return;
            startProjectLoad(path);
        });
        ++shown;
    }
    if (shown == 0) {
        auto* empty = recent_projects_menu_->addAction(tr("No recent projects"));
        empty->setEnabled(false);
    }
    recent_projects_menu_->addSeparator();
    auto* clear = recent_projects_menu_->addAction(tr("Clear Recent Projects"));
    clear->setEnabled(!entries.isEmpty());
    connect(clear, &QAction::triggered, this, [this] {
        QSettings().remove(QStringLiteral("recentProjects/entries"));
        refreshRecentProjectsMenu();
    });
}

void MainWindow::restoreUserSettings() {
    QSettings settings;
    recent_project_limit_ = (std::clamp)(
        settings.value(QStringLiteral("preferences/recentProjectLimit"), 10).toInt(),
        0, (std::numeric_limits<int>::max)());
    const int saved_backend = settings.value(
        QStringLiteral("preferences/renderBackend"),
        static_cast<int>(pvt::RenderBackend::CpuAndGpu)).toInt();
    render_backend_ = saved_backend >= static_cast<int>(pvt::RenderBackend::Cpu)
                              && saved_backend <= static_cast<int>(pvt::RenderBackend::Gpu)
                          ? static_cast<pvt::RenderBackend>(saved_backend)
                          : pvt::RenderBackend::CpuAndGpu;
    const QString saved_directory = settings.value(QStringLiteral("paths/lastDialogDirectory"))
                                        .toString();
    if (!existing_writable_directory(saved_directory, true).isEmpty()) {
        last_dialog_directory_ = saved_directory;
    }
    restoreGeometry(settings.value(QStringLiteral("ui/mainWindow/geometry")).toByteArray());
    const QByteArray state =
        settings.value(QStringLiteral("ui/mainWindow/state")).toByteArray();
    const bool state_restored = state.isEmpty() || restoreState(state);
    if (!state_restored) {
        restoreLayersDock(true);
    } else if (layers_dock_ != nullptr && layers_dock_->isFloating()) {
        const QRect dock_geometry = layers_dock_->frameGeometry();
        const QList<QScreen*> screens = QGuiApplication::screens();
        const bool on_screen = std::any_of(
            screens.begin(), screens.end(),
            [&dock_geometry](const QScreen* screen) {
                return screen != nullptr
                       && screen->availableGeometry().intersects(dock_geometry);
            });
        if (!on_screen) {
            const bool was_hidden = layers_dock_->isHidden();
            restoreLayersDock(!was_hidden);
        }
    }
    refreshRecentProjectsMenu();
}

void MainWindow::saveUserSettings() {
    QSettings settings;
    settings.setValue(QStringLiteral("paths/lastDialogDirectory"), last_dialog_directory_);
    settings.setValue(QStringLiteral("ui/mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/mainWindow/state"), saveState());
    if (undo_stack_ != nullptr) {
        settings.setValue(QStringLiteral("preferences/undoLimit"),
                          undo_stack_->undoLimit());
    }
    settings.setValue(QStringLiteral("preferences/renderBackend"),
                      static_cast<int>(render_backend_));
    settings.setValue(QStringLiteral("preferences/recentProjectLimit"),
                      recent_project_limit_);
    if (audio_volume_ != nullptr) {
        settings.setValue(QStringLiteral("preferences/previewAudioVolume"),
                          audio_volume_->value());
    }
}

void MainWindow::showApplicationSettings() {
    if (undo_stack_ == nullptr) return;
    const int current_undo_limit = undo_stack_->undoLimit();
    const pvt::RenderBackend current_backend = render_backend_;
    const int current_recent_limit = recent_project_limit_;
    ApplicationSettingsDialog dialog(current_undo_limit, current_backend,
                                     current_recent_limit,
                                     hasCustomNewProjectDefaults(), this);
    configure_readable_layouts(&dialog);
    if (dialog.exec() != QDialog::Accepted) return;

    const int requested_undo_limit = dialog.undoLimit();
    const pvt::RenderBackend requested_backend = dialog.renderBackend();
    const int requested_recent_limit = dialog.recentProjectLimit();
    const auto defaults_action = dialog.newProjectDefaultsAction();
    const bool undo_limit_changed = requested_undo_limit != current_undo_limit;
    const bool backend_changed = requested_backend != current_backend;
    const bool recent_limit_changed = requested_recent_limit != current_recent_limit;
    if (!undo_limit_changed && !backend_changed && !recent_limit_changed
        && defaults_action
               == ApplicationSettingsDialog::NewProjectDefaultsAction::Keep) {
        return;
    }

    if (undo_limit_changed && undo_stack_->count() > 0) {
        const auto choice = QMessageBox::question(
            this, tr("Clear undo history?"),
            tr("Applying the new history limit clears this session's undo and redo "
               "history. Other settings in this dialog will also be applied. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
    }

    if (undo_limit_changed) {
        clearUndoHistory(true);
        undo_stack_->setUndoLimit(requested_undo_limit);
        undo_stack_->setClean();
        updateWindowTitle();
    }
    if (backend_changed) {
        render_backend_ = requested_backend;
        if (preview_cancel_ != nullptr) {
            preview_cancel_->store(true, std::memory_order_relaxed);
        }
        if (live_workspace_ != nullptr
            && live_workspace_->isRealtimeOutputActive()) {
            live_workspace_->resetRealtimeFrame();
        }
        schedulePreview();
    }
    if (recent_limit_changed) {
        recent_project_limit_ = requested_recent_limit;
        refreshRecentProjectsMenu();
    }

    QSettings settings;
    settings.setValue(QStringLiteral("preferences/undoLimit"),
                      requested_undo_limit);
    settings.setValue(QStringLiteral("preferences/renderBackend"),
                      static_cast<int>(requested_backend));
    settings.setValue(QStringLiteral("preferences/recentProjectLimit"),
                      requested_recent_limit);
    settings.sync();

    QString defaults_message;
    QString defaults_error;
    if (defaults_action
        == ApplicationSettingsDialog::NewProjectDefaultsAction::SaveCurrentProject) {
        const auto choice = QMessageBox::question(
            this, tr("Replace new-project default?"),
            tr("Save the current project — including all layers and embedded assets — "
               "as the template used by every future New Project command?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice == QMessageBox::Yes) {
            if (saveCurrentProjectAsDefaults(&defaults_error)) {
                defaults_message = tr(" Current project saved as the new-project default.");
            }
        }
    } else if (defaults_action
               == ApplicationSettingsDialog::NewProjectDefaultsAction::RestoreBuiltIn) {
        const auto choice = QMessageBox::question(
            this, tr("Restore built-in default?"),
            tr("Future new projects will use the program's built-in starting settings. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice == QMessageBox::Yes) {
            if (restoreBuiltInProjectDefaults(&defaults_error)) {
                defaults_message = tr(" Built-in new-project default restored.");
            }
        }
    }
    if (!defaults_error.isEmpty()) {
        QMessageBox::critical(this, tr("Could not update new-project defaults"),
                              defaults_error);
    }
    status_->setText(
        tr("Application settings saved — %1 undo steps, %2 rendering, %3 recent projects.%4")
            .arg(requested_undo_limit)
            .arg(QString::fromUtf8(
                pvt::render_backend_name(requested_backend)))
            .arg(requested_recent_limit)
            .arg(defaults_message));
}

bool MainWindow::hasCustomNewProjectDefaults() const {
    const QString path = custom_new_project_defaults_path();
    const QFileInfo information(path);
    return QSettings().value(
               QStringLiteral("preferences/customNewProjectDefaults"), false)
               .toBool()
           && !path.isEmpty() && information.exists() && information.isFile()
           && !information.isSymLink();
}

std::unique_ptr<pvt::ProjectDocument> MainWindow::makeNewProjectDocument(
    QString* warning) const {
    if (warning != nullptr) warning->clear();
    if (!hasCustomNewProjectDefaults()) {
        return std::make_unique<pvt::ProjectDocument>(
            built_in_workbench_project_document());
    }
    pvt::ProjectDocument saved_template;
    std::string load_error;
    const QString path = custom_new_project_defaults_path();
    if (!pvt::load_project_document(path.toStdString(), saved_template,
                                    &load_error)) {
        if (warning != nullptr) {
            *warning = tr("Custom new-project defaults could not be loaded; using the built-in template. %1")
                           .arg(QString::fromStdString(load_error));
        }
        return std::make_unique<pvt::ProjectDocument>(
            built_in_workbench_project_document());
    }
    auto fresh = std::make_unique<pvt::ProjectDocument>();
    std::string copy_error;
    if (!pvt::make_independent_project_copy(saved_template, *fresh,
                                            &copy_error)) {
        if (warning != nullptr) {
            *warning = tr("Custom new-project defaults were valid but could not be detached; using the built-in template. %1")
                           .arg(QString::fromStdString(copy_error));
        }
        return std::make_unique<pvt::ProjectDocument>(
            built_in_workbench_project_document());
    }
    fresh->dirty = false;
    return fresh;
}

bool MainWindow::saveCurrentProjectAsDefaults(QString* error) {
    if (error != nullptr) error->clear();
    syncActiveRender();
    syncProjectGlobals();
    pvt::ProjectDocument source = document_ != nullptr
                                      ? *document_
                                      : pvt::default_project_document();
    source.project = project_;
    pvt::ProjectDocument detached;
    std::string operation_error;
    if (!pvt::make_independent_project_copy(source, detached,
                                            &operation_error)) {
        if (error != nullptr) *error = QString::fromStdString(operation_error);
        return false;
    }

    const QString destination = custom_new_project_defaults_path();
    if (destination.isEmpty()) {
        if (error != nullptr) {
            *error = tr("The operating system did not provide an application-settings directory.");
        }
        return false;
    }
    const QString directory = QFileInfo(destination).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error != nullptr) *error = tr("Could not create the defaults directory.");
        return false;
    }
    QTemporaryDir staging(
        QDir(directory).filePath(QStringLiteral(".pvt-defaults-XXXXXX")));
    if (!staging.isValid()) {
        if (error != nullptr) *error = tr("Could not create a defaults staging directory.");
        return false;
    }
    const QString staged_path = staging.filePath(QStringLiteral("template.zip"));
    if (!pvt::save_project_document(detached, staged_path.toStdString(),
                                    nullptr, &operation_error)) {
        if (error != nullptr) *error = QString::fromStdString(operation_error);
        return false;
    }
    QFile input(staged_path);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = tr("Could not open the defaults bundle for atomic installation.");
        return false;
    }
    while (!input.atEnd()) {
        const QByteArray chunk = input.read(1024 * 1024);
        if (chunk.isEmpty() && input.error() != QFileDevice::NoError) {
            output.cancelWriting();
            if (error != nullptr) *error = tr("Could not read the staged defaults bundle.");
            return false;
        }
        if (output.write(chunk) != chunk.size()) {
            output.cancelWriting();
            if (error != nullptr) *error = tr("Could not write the new defaults bundle.");
            return false;
        }
    }
    if (!output.commit()) {
        if (error != nullptr) *error = tr("Could not atomically install the new defaults bundle.");
        return false;
    }
    pvt::ProjectDocument verified;
    if (!pvt::load_project_document(destination.toStdString(), verified,
                                    &operation_error)) {
        if (error != nullptr) {
            *error = tr("The installed defaults bundle failed validation: %1")
                         .arg(QString::fromStdString(operation_error));
        }
        return false;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("preferences/customNewProjectDefaults"), true);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = tr("The template was saved, but its activation preference could not be persisted.");
        }
        return false;
    }
    return true;
}

bool MainWindow::restoreBuiltInProjectDefaults(QString* error) {
    if (error != nullptr) error->clear();
    QSettings settings;
    settings.setValue(QStringLiteral("preferences/customNewProjectDefaults"), false);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = tr("Could not persist the built-in-default selection.");
        }
        return false;
    }
    return true;
}

void MainWindow::replaceWithNewProject() {
    cancelMusicAnalysis();
    stopPlayback();
    QString warning;
    document_ = makeNewProjectDocument(&warning);
    if (document_ == nullptr || document_->project.layers.empty()) {
        document_ = std::make_unique<pvt::ProjectDocument>(
            built_in_workbench_project_document());
    }
    project_ = document_->project;
    document_->dirty = false;
    active_layer_uuid_ = project_.layers.front().uuid;
    solo_layer_uuid_.reset();
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    current_project_path_.clear();
    imported_legacy_path_.clear();
    baseline_dirty_ = false;
    updateCompatibilityWarning();
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    if (live_workspace_ != nullptr) live_workspace_->resetRealtimeFrame();
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
    status_->setText(warning.isEmpty()
                         ? (hasCustomNewProjectDefaults()
                                ? tr("Started a new project from your custom defaults.")
                                : tr("Started a new project from the built-in defaults."))
                         : warning);
}

void MainWindow::refreshAll() {
    if (music_error_ != nullptr && !music_analysis_active_) {
        music_error_->hide();
    }
    if (layer_music_error_ != nullptr && !music_analysis_active_) {
        layer_music_error_->hide();
    }
    refreshWaveList();
    refreshSwingList();
    refreshEffectList();
    loadGlobalEditors();
    updateTimelineState();
    updateSynchronizationState();
    updateExportAvailability();
    preview_->setConfiguration(config_);
    if (live_workspace_ != nullptr) {
        live_workspace_->setProjectLiveConfig(project_.canvas.live);
        live_workspace_->refreshProjectSnapshot();
    }
}

void MainWindow::updateTimelineState() {
    if (timeline_ == nullptr || frame_label_ == nullptr) {
        return;
    }
    QString frame_error;
    int count = effectiveFrameCount(&frame_error);
    if (count < 1) count = std::max(2, config_.total_frames);
    const QSignalBlocker blocker(timeline_);
    timeline_->setMaximum(count - 1);
    timeline_->setValue(std::min(timeline_->value(), timeline_->maximum()));
    updateTimelineReadout();
    if (audio_volume_ != nullptr) {
        const bool has_audio = document_ != nullptr
            && !audible_project_tracks(
                    previewProjectSnapshot(), *document_,
                    static_cast<double>(timeline_->value()) / config_.fps)
                    .empty();
        audio_volume_->setEnabled(has_audio);
    }
    if (playback_timer_->isActive()) {
        playback_timer_->setInterval(
            std::max(1, static_cast<int>(std::lround(1000.0 / config_.fps))));
    }
}

int MainWindow::effectiveFrameCount(QString* error) const {
    std::string frame_error;
    const int count = pvt::effective_frame_count(config_, &frame_error);
    if (error != nullptr) *error = QString::fromStdString(frame_error);
    return count;
}

bool MainWindow::musicRenderReady() const {
    if (config_.clock.music.analyzer_version.empty()
        || config_.clock.music.source_sha256.empty()) {
        return false;
    }
    pvt::RenderConfig probe = config_;
    probe.clock.mode = pvt::ClockMode::Music;
    std::string error;
    return pvt::effective_frame_count(probe, &error) > 0;
}

void MainWindow::updateTimelineReadout() {
    if (timeline_ == nullptr || frame_label_ == nullptr) return;
    const int count = std::max(1, timeline_->maximum() + 1);
    const int frame = timeline_->value();
    const double seconds = static_cast<double>(frame) / config_.fps;
    QString text = tr("Time %1  ·  Frame %2 / %3")
                       .arg(formatted_time(seconds))
                       .arg(frame + 1)
                       .arg(count);
    if (config_.clock.mode == pvt::ClockMode::Music && musicRenderReady()) {
        const auto beats = music_beats_for_ui(config_.clock);
        const auto found = std::upper_bound(beats.begin(), beats.end(), seconds);
        if (found != beats.begin()) {
            const auto beat = static_cast<std::size_t>(
                std::distance(beats.begin(), found));
            text.append(tr("  ·  Beat %1").arg(static_cast<qulonglong>(beat)));
        }
    }
    frame_label_->setText(text);
}

void MainWindow::navigateToBeat(int direction) {
    if (timeline_ == nullptr || direction == 0 || !musicRenderReady()) return;
    const auto beats = music_beats_for_ui(config_.clock);
    if (beats.empty()) return;
    const double now = static_cast<double>(timeline_->value()) / config_.fps;
    double destination = beats.front();
    if (direction > 0) {
        const auto next = std::upper_bound(beats.begin(), beats.end(), now + 1.0e-9);
        destination = next == beats.end() ? beats.back() : *next;
    } else {
        const auto previous = std::lower_bound(beats.begin(), beats.end(), now - 1.0e-9);
        if (previous == beats.begin()) {
            destination = beats.front();
        } else {
            destination = *std::prev(previous);
        }
    }
    timeline_->setValue(std::clamp(
        static_cast<int>(std::llround(destination * config_.fps)),
        timeline_->minimum(), timeline_->maximum()));
    if (playback_timer_ != nullptr && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
}

void MainWindow::updateSynchronizationState() {
    if (clock_mode_ == nullptr) return;
    const auto mode = config_.clock.mode;
    const bool editable = !music_analysis_active_;
    const auto* active_layer = activeLayer();
    const auto* owning_group = active_layer != nullptr
                                   ? groupForLayer(*active_layer) : nullptr;
    const bool layer_accessible = active_layer != nullptr
                                  && !selected_group_uuid_
                                  && (owning_group == nullptr
                                      || !owning_group->locked);
    const bool layer_editable = editable && layer_accessible;
    const bool project_mic = standardMicRoute(false) != nullptr;
    const bool layer_mic = standardMicRoute(true) != nullptr;
    const bool pulse_clock = mode == pvt::ClockMode::Frame
                             || mode == pvt::ClockMode::Time
                             || mode == pvt::ClockMode::Meter
                             || mode == pvt::ClockMode::Music;
    clock_mode_->setEnabled(editable);
    project_mic_device_->setEnabled(editable);
    project_mic_refresh_->setEnabled(editable);
    project_mic_setup_->setEnabled(editable && project_mic);
    music_data_only_->setEnabled(editable);
    music_processing_->setEnabled(editable);
    music_frequency_stream_->setEnabled(
        editable && !config_.clock.music.source_sha256.empty()
        && !config_.clock.audio_processing.frequency_streams.empty());
    clock_interpolation_->setEnabled(editable && pulse_clock);
    clock_fit_->setEnabled(editable
                           && (mode == pvt::ClockMode::Frame
                               || mode == pvt::ClockMode::Time
                               || mode == pvt::ClockMode::Meter));
    for (QWidget* editor : std::initializer_list<QWidget*>{
             clock_frame_interval_, clock_time_interval_ms_, meter_expression_,
             meter_bpm_, meter_tempo_note_, clock_reverse_,
             clock_phase_offset_, music_tempo_mode_, music_beat_offset_ms_}) {
        editor->setEnabled(editable);
    }
    swings_group_->setEnabled(layer_editable);
    const bool music_audio_available = effective_active_clock_is_music(
        config_, active_layer_uuid_);
    project_audio_response_group_->setVisible(music_audio_available);
    audio_response_group_->setVisible(music_audio_available);
    audio_response_effective_->setVisible(music_audio_available);
    audio_copy_project_->setVisible(music_audio_available);
    wave_form_->setRowVisible(wave_audio_response_, music_audio_available);
    effect_form_->setRowVisible(effect_audio_response_, music_audio_available);
    project_audio_response_group_->setEnabled(editable
                                              && music_audio_available);
    audio_response_group_->setEnabled(layer_editable
                                      && music_audio_available);
    audio_response_effective_->setEnabled(layer_editable
                                          && music_audio_available);
    audio_copy_project_->setEnabled(layer_editable
                                    && music_audio_available);
    const bool layer_override = config_.audio_reactive_override_enabled;
    const pvt::AudioReactiveConfig& effective_audio =
        layer_override ? config_.audio_reactive
                       : config_.audio_reactive_defaults;
    audio_response_effective_->setText(
        layer_override
            ? tr("Effective routing: active-layer override — %1")
                  .arg(effective_audio.enabled ? tr("enabled")
                                               : tr("disabled"))
            : tr("Effective routing: project-wide defaults — %1. Check this group to override them for this layer.")
                  .arg(effective_audio.enabled ? tr("enabled")
                                               : tr("disabled")));
    if (const auto wave = selectedWaveIndex()) {
        wave_audio_response_->setEnabled(
            layer_editable && music_audio_available);
    }
    if (const auto effect = selectedEffectIndex()) {
        effect_audio_response_->setEnabled(
            layer_editable && music_audio_available);
    }
    clock_form_->setRowVisible(clock_frame_interval_, mode == pvt::ClockMode::Frame);
    clock_form_->setRowVisible(clock_time_interval_ms_, mode == pvt::ClockMode::Time);
    const bool meter = mode == pvt::ClockMode::Meter;
    clock_form_->setRowVisible(meter_expression_, meter);
    clock_form_->setRowVisible(meter_summary_, meter);
    clock_form_->setRowVisible(meter_bpm_, meter);
    clock_form_->setRowVisible(meter_tempo_note_, meter);
    const bool music = mode == pvt::ClockMode::Music || music_analysis_active_;
    clock_form_->setRowVisible(music_tempo_mode_, music);
    clock_form_->setRowVisible(music_beat_offset_ms_, music);
    clock_form_->setRowVisible(music_data_only_, music);
    const bool beat_navigation = mode == pvt::ClockMode::Music
                                 && musicRenderReady();
    if (previous_beat_ != nullptr) previous_beat_->setEnabled(beat_navigation);
    if (next_beat_ != nullptr) next_beat_->setEnabled(beat_navigation);

    std::string meter_description;
    std::string meter_error;
    const bool meter_valid = pvt::describe_meter(
        config_.clock.meter.expression, meter_description, &meter_error);
    meter_summary_->setText(QString::fromStdString(
        meter_valid ? meter_description : meter_error));
    meter_summary_->setStyleSheet(
        meter_valid ? QString{} : QStringLiteral("color: #d32f2f;"));

    if (frames_ != nullptr) {
        const bool derived_music_frames = mode == pvt::ClockMode::Music
                                          && musicRenderReady();
        frames_->setEnabled(!derived_music_frames);
        frames_->setToolTip(derived_music_frames
            ? tr("This saved manual frame count is preserved but ignored while render-ready Music determines duration.")
            : QString{});
    }
    if (effective_frames_ != nullptr) {
        QString frame_error;
        const int count = effectiveFrameCount(&frame_error);
        if (count > 0) {
            effective_frames_->setText(
                tr("%1 frames · %2 at %3 FPS")
                    .arg(count)
                    .arg(formatted_time(static_cast<double>(count) / config_.fps))
                    .arg(config_.fps, 0, 'f', 3));
        } else {
            effective_frames_->setText(frame_error);
        }
    }
    const bool has_music = !config_.clock.music.source_sha256.empty();
    music_relink_->setEnabled(has_music && !music_analysis_active_);
    music_reanalyze_->setEnabled(has_music && !music_analysis_active_
                                 && !currentMusicSourcePath().isEmpty());
    music_clear_->setEnabled(has_music && !music_analysis_active_);
    music_choose_->setEnabled(mode == pvt::ClockMode::Music
                              && !music_analysis_active_);
    music_cancel_->setVisible(music_analysis_active_
                              && !music_analysis_layer_clock_);
    music_progress_->setVisible(music_analysis_active_
                                && !music_analysis_layer_clock_);

    const auto& local = config_.layer_clock;
    const auto local_mode = local.clock.mode;
    const bool local_enabled = local.enabled && layer_editable;
    layer_clock_group_->setEnabled(layer_editable);
    layer_music_processing_->setEnabled(layer_editable);
    layer_music_frequency_stream_->setEnabled(
        layer_editable && !local.clock.music.source_sha256.empty()
        && !local.clock.audio_processing.frequency_streams.empty());
    layer_clock_mix_enabled_->setEnabled(local_enabled);
    layer_clock_mix_mode_->setEnabled(local_enabled && local.mix_enabled);
    layer_clock_form_->setRowVisible(
        layer_clock_scale_, local_mode == pvt::ClockMode::Music);
    layer_clock_scale_->setEnabled(
        local_enabled && local_mode == pvt::ClockMode::Music);
    // The Mic sentinel must remain reachable even when the authored offline
    // layer-clock toggle is off. Other deterministic controls still follow
    // the authored toggle.
    layer_clock_mode_->setEnabled(layer_editable);
    layer_mic_device_->setEnabled(layer_editable);
    layer_mic_refresh_->setEnabled(layer_editable);
    layer_mic_setup_->setEnabled(layer_editable && layer_mic);
    const bool local_pulse = local_mode == pvt::ClockMode::Frame
                             || local_mode == pvt::ClockMode::Time
                             || local_mode == pvt::ClockMode::Meter
                             || local_mode == pvt::ClockMode::Music;
    layer_clock_interpolation_->setEnabled(local_enabled && local_pulse);
    layer_clock_fit_->setEnabled(
        local_enabled && (local_mode == pvt::ClockMode::Frame
                          || local_mode == pvt::ClockMode::Time
                          || local_mode == pvt::ClockMode::Meter));
    layer_clock_form_->setRowVisible(
        layer_clock_frame_interval_, local_mode == pvt::ClockMode::Frame);
    layer_clock_form_->setRowVisible(
        layer_clock_time_interval_ms_, local_mode == pvt::ClockMode::Time);
    const bool local_meter = local_mode == pvt::ClockMode::Meter;
    layer_clock_form_->setRowVisible(layer_meter_expression_, local_meter);
    layer_clock_form_->setRowVisible(layer_meter_summary_, local_meter);
    layer_clock_form_->setRowVisible(layer_meter_bpm_, local_meter);
    layer_clock_form_->setRowVisible(layer_meter_tempo_note_, local_meter);
    const bool local_music = local_mode == pvt::ClockMode::Music
                             || (music_analysis_active_
                                 && music_analysis_layer_clock_);
    layer_clock_form_->setRowVisible(layer_music_tempo_mode_, local_music);
    layer_clock_form_->setRowVisible(layer_music_beat_offset_ms_, local_music);
    layer_clock_form_->setRowVisible(layer_music_data_only_, local_music);
    for (QWidget* editor : std::initializer_list<QWidget*>{
             layer_clock_frame_interval_, layer_clock_time_interval_ms_,
             layer_meter_expression_, layer_meter_bpm_, layer_meter_tempo_note_,
             layer_clock_reverse_, layer_clock_phase_offset_,
             layer_music_tempo_mode_, layer_music_beat_offset_ms_,
             layer_music_data_only_}) {
        editor->setEnabled(local_enabled);
    }
    std::string layer_meter_description;
    std::string layer_meter_error;
    const bool layer_meter_valid = pvt::describe_meter(
        local.clock.meter.expression, layer_meter_description,
        &layer_meter_error);
    layer_meter_summary_->setText(QString::fromStdString(
        layer_meter_valid ? layer_meter_description : layer_meter_error));
    layer_meter_summary_->setStyleSheet(
        layer_meter_valid ? QString{} : QStringLiteral("color: #d32f2f;"));

    const bool has_layer_music = !local.clock.music.source_sha256.empty();
    layer_music_relink_->setEnabled(has_layer_music && layer_editable);
    layer_music_reanalyze_->setEnabled(
        has_layer_music && layer_editable
        && !currentMusicSourcePath(true).isEmpty());
    layer_music_clear_->setEnabled(has_layer_music && layer_editable);
    layer_music_choose_->setEnabled(local_enabled
                                    && local_mode == pvt::ClockMode::Music);
    layer_music_cancel_->setVisible(music_analysis_active_
                                    && music_analysis_layer_clock_);
    layer_music_progress_->setVisible(music_analysis_active_
                                      && music_analysis_layer_clock_);

    QString duration_warning;
    QString frame_error;
    const int master_frames = effectiveFrameCount(&frame_error);
    const double master_duration =
        config_.clock.mode == pvt::ClockMode::Music
                && config_.clock.music.duration_seconds > 0.0
            ? config_.clock.music.duration_seconds
            : (master_frames > 0
                   ? static_cast<double>(master_frames) / config_.fps : 0.0);
    const bool one_shot_outlasts_project = has_layer_music
        && master_duration > 0.0
        && local.clock.music.duration_seconds > master_duration + 1.0e-9;
    if (one_shot_outlasts_project) {
        duration_warning = tr(
            "This source outlasts the project. Play Once will show the available prefix; Play Once Then Project will not reach its project-clock transition before this project ends.");
    }
    if (auto* model = qobject_cast<QStandardItemModel*>(
            layer_clock_scale_->model())) {
        for (const auto value : {pvt::LayerClockScale::PlayOnce,
                                 pvt::LayerClockScale::PlayOnceThenProject}) {
            const int index = layer_clock_scale_->findData(
                static_cast<int>(value));
            if (index >= 0 && model->item(index) != nullptr) {
                model->item(index)->setEnabled(true);
            }
        }
    }
    layer_clock_scale_->setToolTip(duration_warning);
    updateMusicSummary();
    updateWorkflowSummaries();
}

void MainWindow::updateMusicTransactionGuards() {
    const bool editable = !music_analysis_active_ && !project_io_active_;
    for (QAction* action : {new_action_, open_action_, open_folder_action_,
                            save_action_, save_as_action_}) {
        if (action != nullptr) action->setEnabled(editable);
    }
    const auto* active_layer = activeLayer();
    const auto* owning_group = active_layer != nullptr
                                   ? groupForLayer(*active_layer) : nullptr;
    const bool layer_editable = editable && active_layer != nullptr
                                && !selected_group_uuid_
                                && (owning_group == nullptr
                                    || !owning_group->locked);
    for (QAction* action : {randomize_values_action_, randomize_mix_action_}) {
        if (action != nullptr) action->setEnabled(layer_editable);
    }
    if (undo_action_ != nullptr) {
        undo_action_->setEnabled(editable && undo_stack_ != nullptr
                                 && undo_stack_->canUndo());
    }
    if (redo_action_ != nullptr) {
        redo_action_->setEnabled(editable && undo_stack_ != nullptr
                                 && undo_stack_->canRedo());
    }
    if (layers_dock_ != nullptr) layers_dock_->setEnabled(editable);
    if (surface_obj_path_ != nullptr) surface_obj_path_->setEnabled(editable);
    if (surface_obj_browse_ != nullptr) surface_obj_browse_->setEnabled(editable);
    if (starting_image_group_ != nullptr) starting_image_group_->setEnabled(editable);
    if (tabs_ != nullptr) {
        for (int index = 0; index < tabs_->count(); ++index) {
            QWidget* page = tabs_->widget(index);
            const bool layer_page = page == source_page_ || page == effect_page_
                                    || page == surface_page_
                                    || page == motion_page_
                                    || page == finish_page_;
            page->setEnabled(layer_page ? layer_editable : editable);
        }
    }
    if (synchronization_page_ != nullptr) {
        synchronization_page_->setEnabled(editable || music_analysis_active_);
    }
    for (std::size_t index = 0; index < workflow_stage_buttons_.size(); ++index) {
        const bool layer_stage = index >= 1U && index <= 5U;
        workflow_stage_buttons_[index]->setEnabled(
            layer_stage ? layer_editable : editable);
    }
    updateSynchronizationState();
    updateExportAvailability();
    updateWorkflowSummaries();
}

void MainWindow::updateMusicSummary() {
    if (music_summary_ == nullptr || music_source_ == nullptr
        || layer_music_summary_ == nullptr || layer_music_source_ == nullptr) {
        return;
    }
    const auto summarize = [this](const pvt::MusicAnalysis& music,
                                  bool layer_clock, QLineEdit* source,
                                  QLabel* summary, QLabel* error_label) {
        source->setText(QString::fromStdString(music.source_basename));
        if (music.source_sha256.empty()) {
            summary->setText(tr(
                "Choose a WAV, FLAC, or MP3 source. It will be analyzed first and embedded by content in the project bundle."));
            if (!music_analysis_active_) error_label->hide();
            return;
        }
        const double feature_density = music.duration_seconds > 0.0
            ? static_cast<double>(music.feature_samples.size())
                  / music.duration_seconds
            : 0.0;
        double minimum_tempo = music.detected_bpm;
        double maximum_tempo = music.detected_bpm;
        if (!music.tempo_points.empty()) {
            const auto tempos = std::minmax_element(
                music.tempo_points.begin(), music.tempo_points.end(),
                [](const pvt::MusicTempoPoint& left,
                   const pvt::MusicTempoPoint& right) {
                    return left.bpm < right.bpm;
                });
            minimum_tempo = tempos.first->bpm;
            maximum_tempo = tempos.second->bpm;
        }
        const QString tempo_map = music.tempo_points.empty()
            ? tr("No adaptive tempo map")
            : tr("%1 tempo point(s), %2–%3 BPM")
                  .arg(static_cast<qulonglong>(music.tempo_points.size()))
                  .arg(minimum_tempo, 0, 'f', 2)
                  .arg(maximum_tempo, 0, 'f', 2);
        const int source_frames = static_cast<int>(
            std::ceil(music.duration_seconds * config_.fps));
        const QString mapping = layer_clock
            ? tr(" · %1")
                  .arg(QString::fromUtf8(pvt::layer_clock_scale_name(
                      config_.layer_clock.scale)))
            : QString{};
        summary->setText(
            tr("%1 · %2 Hz · %3 channel(s) · %4%5\n"
               "Detected %6 BPM (%7% confidence) · %8 beat(s) · %9\n"
               "%10 features/s · %11 source frame(s) at %12 FPS")
                .arg(QString::fromStdString(music.source_format))
                .arg(music.source_sample_rate)
                .arg(music.source_channel_count)
                .arg(formatted_time(music.duration_seconds))
                .arg(mapping)
                .arg(music.detected_bpm, 0, 'f', 2)
                .arg(music.tempo_confidence * 100.0, 0, 'f', 0)
                .arg(static_cast<qulonglong>(music.beat_times_seconds.size()))
                .arg(tempo_map)
                .arg(feature_density, 0, 'f', 1)
                .arg(source_frames)
                .arg(config_.fps, 0, 'f', 3));
        if (currentMusicSourcePath(layer_clock).isEmpty()) {
            error_label->setText(tr(
                "The analyzed source is not available locally. Relink the matching file before reanalysis."));
            error_label->show();
        } else if (!music_analysis_active_) {
            error_label->hide();
        }
    };
    summarize(config_.clock.music, false, music_source_, music_summary_,
              music_error_);
    summarize(config_.layer_clock.clock.music, true, layer_music_source_,
              layer_music_summary_, layer_music_error_);
}

void MainWindow::updateExportAvailability() {
    const bool realtime_output = live_workspace_ != nullptr
        && live_workspace_->isRealtimeOutputActive();
    const bool transaction_idle = !music_analysis_active_ && !project_io_active_;
    const QString realtime_tooltip = tr(
        "Stop LIVE or Live Preview Output before exporting so the realtime display keeps its frame budget.");
    bit_depth_->setEnabled(true);
    png_compression_->setEnabled(config_.output.bit_depth != 32);
    dither_enabled_->setEnabled(config_.output.bit_depth != 32);
    dither_method_->setEnabled(config_.output.bit_depth != 32
                               && config_.output.dither_enabled);
    write_alpha_->setEnabled(true);
    first_frame_->setEnabled(true);
    filename_digits_->setEnabled(true);
    if (export_action_ != nullptr && !export_active_) {
        export_action_->setEnabled(transaction_idle && !realtime_output);
        export_action_->setToolTip(realtime_output ? realtime_tooltip : QString{});
    }
    if (current_frame_export_action_ != nullptr && !export_active_) {
        current_frame_export_action_->setEnabled(transaction_idle
                                                  && !realtime_output);
        current_frame_export_action_->setToolTip(
            realtime_output
                ? realtime_tooltip
                : tr("Render the timeline's current frame at the full canvas resolution using the selected PNG/EXR quality settings."));
    }
    if (video_export_action_ != nullptr && !export_active_) {
        static const pvt::video::Capabilities video =
            pvt::video::capabilities();
        video_export_action_->setEnabled(transaction_idle
                                         && !realtime_output
                                         && video.available);
        video_export_action_->setToolTip(
            realtime_output
                ? realtime_tooltip
                : (video.available
                       ? tr("Export a native macOS QuickTime movie without FFmpeg.")
                       : QString::fromStdString(video.status)));
    }
    if (live_preview_output_action_ != nullptr) {
        const bool presentation_active = live_workspace_ != nullptr
            && live_workspace_->isPresentationActive();
        live_preview_output_action_->setEnabled(
            presentation_active || (!export_active_ && transaction_idle));
    }
    if (live_preview_output_button_ != nullptr) {
        const bool presentation_active = live_workspace_ != nullptr
            && live_workspace_->isPresentationActive();
        live_preview_output_button_->setEnabled(
            presentation_active || (!export_active_ && transaction_idle));
    }
    if (live_mode_action_ != nullptr) {
        live_mode_action_->setEnabled(!export_active_ && transaction_idle);
    }
}

void MainWindow::finishExportUiState() {
    export_active_ = false;
    if (export_progress_ != nullptr) export_progress_->hide();
    updateExportAvailability();
    if (cancel_export_action_ != nullptr) {
        cancel_export_action_->setEnabled(false);
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
    if (index >= config_.effects.size() || effect_list_ == nullptr) return;
    const auto id = config_.effects[index].id;
    for (int row = 0; row < effect_list_->count(); ++row) {
        auto* item = effect_list_->item(row);
        if (item != nullptr
            && item->data(Qt::UserRole).toULongLong() == id) {
            item->setText(effect_label(config_.effects[index], index));
            return;
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
    updateWaveOutputState();
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
    preview_->setConfiguration(config_);
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
        if (effect_ui_category(effect.type) != effect_category_filter_) {
            continue;
        }
        auto* item = new QListWidgetItem(effect_label(effect, index), effect_list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(effect.id));
        if (selectedId && effect.id == *selectedId) {
            selected_row = effect_list_->count() - 1;
        }
    }
    if (selected_row < 0 && effect_list_->count() > 0) {
        selected_row = 0;
    }
    effect_list_->setCurrentRow(selected_row);
    populating_ = false;
    loadSelectedEffect();
    preview_->setConfiguration(config_);
    updateWorkflowSummaries();
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
    const QListWidgetItem* item = effect_list_ != nullptr
                                      ? effect_list_->currentItem() : nullptr;
    if (item == nullptr) return std::nullopt;
    const std::uint64_t id = item->data(Qt::UserRole).toULongLong();
    const auto found = std::find_if(
        config_.effects.cbegin(), config_.effects.cend(),
        [id](const pvt::EffectConfig& effect) { return effect.id == id; });
    if (found == config_.effects.cend()) return std::nullopt;
    return static_cast<std::size_t>(
        std::distance(config_.effects.cbegin(), found));
}

void MainWindow::loadSelectedWave() {
    if (populating_) {
        return;
    }
    const auto index = selectedWaveIndex();
    populating_ = true;
    const bool enabled = index.has_value();
    for (auto* widget : std::initializer_list<QWidget*>{wave_name_, wave_enabled_, wave_sync_,
                                                        wave_audio_response_,
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
        select_enum(wave_audio_response_, wave.audio_response);
        wave_audio_response_->setEnabled(
            effective_active_clock_is_music(config_, active_layer_uuid_));
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
             swing_phase_, swing_shape_, swing_center_x_, swing_center_y_,
             swing_radius_}) {
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
        swing_center_x_->setValue(swing.center_x);
        swing_center_y_->setValue(swing.center_y);
        swing_radius_->setValue(swing.radius);
    }
    populating_ = false;
    preview_->setSelectedSwing(index);
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
    const bool is_block_scale = type == pvt::EffectType::BlockScale;
    const bool is_particles = type == pvt::EffectType::ParticleField;
    const bool is_blur = type == pvt::EffectType::Blur;
    const bool is_glitch = type == pvt::EffectType::Glitch;
    const bool is_starburst = type == pvt::EffectType::Starburst;
    const bool is_lens = type == pvt::EffectType::LensDistortion;
    const bool is_edge_detect = type == pvt::EffectType::EdgeDetect;
    const bool is_twirl = type == pvt::EffectType::Twirl;
    const bool coordinate_effect = !is_glow && !is_block_scale && !is_particles && !is_blur;
    const bool has_center = !is_block_scale;
    const auto blur_type = static_cast<pvt::BlurType>(
        effect_blur_type_->currentData().toInt());
    const auto particle_profile = static_cast<pvt::ParticleRenderProfile>(
        effect_particle_profile_->currentData().toInt());

    effect_form_->setRowVisible(effect_edge_, coordinate_effect || is_blur);
    effect_form_->setRowVisible(effect_intensity_, !is_blur);
    effect_form_->setRowVisible(effect_magnitude_,
                                coordinate_effect || is_block_scale || is_particles);
    effect_form_->setRowVisible(effect_frequency_,
                                coordinate_effect || is_block_scale || is_particles);
    effect_form_->setRowVisible(effect_secondary_, !is_zoom);
    effect_form_->setRowVisible(effect_center_x_, has_center);
    effect_form_->setRowVisible(effect_center_y_, has_center);
    effect_form_->setRowVisible(effect_angle_, is_shake || is_flag
                                                   || is_starburst || is_particles
                                                   || (is_blur && blur_type == pvt::BlurType::Directional));
    effect_form_->setRowVisible(effect_radius_, is_glow || is_particles || is_blur);
    effect_form_->setRowVisible(effect_threshold_, is_glow || is_particles);
    effect_form_->setRowVisible(effect_knee_, is_glow || is_particles);
    effect_form_->setRowVisible(effect_area_radius_, !is_block_scale);
    effect_form_->setRowVisible(effect_particle_shape_, is_particles);
    effect_form_->setRowVisible(effect_particle_profile_, is_particles);
    effect_form_->setRowVisible(effect_particle_size_scale_, is_particles);
    effect_form_->setRowVisible(effect_particle_size_variation_, is_particles);
    effect_form_->setRowVisible(effect_particle_definition_, is_particles);
    effect_form_->setRowVisible(effect_particle_twinkle_, is_particles);
    effect_form_->setRowVisible(effect_particle_seed_, is_particles);
    effect_form_->setRowVisible(effect_particle_reseed_, is_particles);
    effect_form_->setRowVisible(effect_particle_orientation_, is_particles);
    effect_form_->setRowVisible(effect_particle_rotation_, is_particles);
    effect_form_->setRowVisible(effect_blur_type_, is_blur);
    effect_form_->setRowVisible(effect_blur_passes_, is_blur);
    effect_form_->setRowVisible(effect_blur_samples_, is_blur);
    effect_form_->setRowVisible(effect_blur_minimum_, is_blur);
    effect_form_->setRowVisible(effect_blur_maximum_, is_blur);

    effect_radius_->setRange(
        is_particles ? kMinimumPositiveUiValue : 0.0,
        kMaximumRenderParameter);
    effect_threshold_->setRange(
        0.0, is_particles ? 1.0 : kMaximumRenderParameter);

    effect_edge_->setToolTip(
        tr("Controls samples that move beyond the source image boundary."));
    effect_center_x_->setToolTip(tr("Normalized horizontal center; 0 is left and 1 is right."));
    effect_center_y_->setToolTip(tr("Normalized vertical center; 0 is top and 1 is bottom."));
    effect_radius_->setToolTip(
        is_blur ? tr("Blur radius in full-resolution output pixels.")
                : (is_particles
                       ? tr("Exact base particle radius in full-resolution output pixels.")
                       : tr("Glow blur radius in full-resolution output pixels.")));
    effect_area_radius_->setToolTip(
        tr("Fraction of the shorter canvas edge. Zero affects the whole layer; "
           "positive values create a feathered draggable circle."));
    effect_threshold_->setToolTip(tr("Linear-light brightness where glow begins."));
    effect_knee_->setToolTip(tr("Soft transition width around the glow threshold."));
    effect_particle_definition_->setEnabled(
        is_particles
        && particle_profile == pvt::ParticleRenderProfile::Defined);

    if (is_block_scale) {
        effect_intensity_->setRange(0.0, 1.0);
        effect_magnitude_->setRange(
            kMinimumPositiveUiValue, kMaximumRenderParameter);
        effect_frequency_->setRange(effect_magnitude_->value(),
                                    kMaximumRenderParameter);
        effect_secondary_->setRange(
            0.0, static_cast<double>(kMaximumIntegerParameter));
        effect_secondary_->setDecimals(0);
        effect_secondary_->setSingleStep(1.0);
    } else if (is_particles) {
        effect_intensity_->setRange(0.0, kMaximumRenderParameter);
        effect_magnitude_->setRange(0.0, kMaximumRenderParameter);
        effect_frequency_->setRange(
            1.0, static_cast<double>((std::numeric_limits<int>::max)()));
        effect_frequency_->setDecimals(0);
        effect_frequency_->setSingleStep(1.0);
        effect_secondary_->setRange(0.0, 1.0);
        effect_secondary_->setDecimals(4);
        effect_secondary_->setSingleStep(0.01);
    } else if (is_glitch || is_starburst || is_edge_detect) {
        effect_intensity_->setRange(0.0, 1.0);
        effect_magnitude_->setRange(0.0, kMaximumRenderParameter);
        effect_frequency_->setRange(
            1.0, is_glitch
                     ? static_cast<double>(kMaximumIntegerParameter)
                     : kMaximumRenderParameter);
        effect_frequency_->setDecimals(0);
        effect_frequency_->setSingleStep(1.0);
        effect_secondary_->setRange(0.0, 1.0);
        effect_secondary_->setDecimals(4);
        effect_secondary_->setSingleStep(0.01);
    } else if (is_lens || is_twirl) {
        effect_intensity_->setRange(0.0, 1.0);
        effect_magnitude_->setRange(0.0, kMaximumRenderParameter);
        effect_frequency_->setRange(0.25, kMaximumRenderParameter);
        effect_frequency_->setDecimals(4);
        effect_frequency_->setSingleStep(0.05);
        effect_secondary_->setRange(-1.0, 1.0);
        effect_secondary_->setDecimals(4);
        effect_secondary_->setSingleStep(0.05);
    } else {
        effect_intensity_->setRange(0.0, kMaximumRenderParameter);
        effect_magnitude_->setRange(0.0, kMaximumRenderParameter);
        effect_frequency_->setRange(0.0, kMaximumRenderParameter);
        effect_frequency_->setDecimals(4);
        effect_frequency_->setSingleStep(0.01);
        effect_secondary_->setRange(-kMaximumRenderParameter,
                                    kMaximumRenderParameter);
        effect_secondary_->setDecimals(4);
        effect_secondary_->setSingleStep(0.01);
    }

    if (is_zoom) {
        set_form_label(effect_form_, effect_intensity_, tr("Mix / zoom depth"));
        set_form_label(effect_form_, effect_magnitude_, tr("Zoom strength"));
        set_form_label(effect_form_, effect_frequency_, tr("Octave multiplier"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_intensity_->setToolTip(
            tr("From 0 to 1, blends between the source and looping zoom. Values above 1 deepen the zoom span, so positive Audio Response remains visible at the default full blend."));
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
    } else if (is_glow) {
        set_form_label(effect_form_, effect_intensity_, tr("Glow intensity"));
        set_form_label(effect_form_, effect_secondary_, tr("Pulse depth"));
        effect_intensity_->setToolTip(tr("Brightness added by the blurred highlight layer."));
        effect_secondary_->setToolTip(tr("How strongly the synchronized clock pulses glow intensity."));
    } else if (is_blur) {
        set_form_label(effect_form_, effect_radius_, tr("Blur radius (pixels)"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        set_form_label(effect_form_, effect_angle_, tr("Direction angle (degrees)"));
        effect_center_x_->setToolTip(
            tr("Center for Radial and Zoom blur, and for an optional local-area mask."));
        effect_center_y_->setToolTip(
            tr("Center for Radial and Zoom blur, and for an optional local-area mask."));
        effect_angle_->setToolTip(tr("Sampling direction for Directional blur."));
    } else if (is_block_scale) {
        set_form_label(effect_form_, effect_intensity_, tr("Pixel-block mix"));
        set_form_label(effect_form_, effect_magnitude_, tr("Minimum size multiplier"));
        set_form_label(effect_form_, effect_frequency_, tr("Maximum size multiplier"));
        set_form_label(effect_form_, effect_secondary_, tr("Quantization steps (0 smooth)"));
        effect_intensity_->setToolTip(
            tr("Blend between the incoming image and the animated block grouping."));
        effect_magnitude_->setToolTip(
            tr("Smallest multiplier applied to the canvas block-size setting."));
        effect_frequency_->setToolTip(
            tr("Largest multiplier applied to the canvas block-size setting."));
        effect_secondary_->setToolTip(
            tr("Zero changes smoothly; a whole value snaps the motion into that many intervals."));
    } else if (is_glitch) {
        set_form_label(effect_form_, effect_intensity_, tr("Glitch mix"));
        set_form_label(effect_form_, effect_magnitude_,
                       tr("Horizontal displacement"));
        set_form_label(effect_form_, effect_frequency_,
                       tr("Scanline bands"));
        set_form_label(effect_form_, effect_secondary_, tr("RGB split"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_frequency_->setToolTip(
            tr("Whole horizontal band count. Each band receives a deterministic loop-safe offset."));
        effect_secondary_->setToolTip(
            tr("Separates color channels inside displaced bands."));
    } else if (is_starburst) {
        set_form_label(effect_form_, effect_intensity_, tr("Starburst mix"));
        set_form_label(effect_form_, effect_magnitude_,
                       tr("Radial displacement"));
        set_form_label(effect_form_, effect_frequency_, tr("Ray count"));
        set_form_label(effect_form_, effect_secondary_, tr("Ray sharpness"));
        set_form_label(effect_form_, effect_angle_,
                       tr("Ray rotation (degrees)"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_frequency_->setToolTip(
            tr("Whole radial ray count around the authored center."));
        effect_secondary_->setToolTip(
            tr("Sharpens the transition between neighboring rays."));
    } else if (is_lens) {
        set_form_label(effect_form_, effect_intensity_, tr("Lens mix"));
        set_form_label(effect_form_, effect_magnitude_, tr("Lens bend"));
        set_form_label(effect_form_, effect_frequency_, tr("Radial exponent"));
        set_form_label(effect_form_, effect_secondary_,
                       tr("Direction (−1 barrel, +1 pincushion)"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_secondary_->setToolTip(
            tr("Negative values bow outward (barrel); positive values pinch inward."));
    } else if (is_edge_detect) {
        set_form_label(effect_form_, effect_intensity_, tr("Edge mix"));
        set_form_label(effect_form_, effect_magnitude_, tr("Edge gain"));
        set_form_label(effect_form_, effect_frequency_, tr("Sampling radius (pixels)"));
        set_form_label(effect_form_, effect_secondary_, tr("Edge threshold"));
        set_form_label(effect_form_, effect_center_x_, tr("Area center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Area center Y (0–1)"));
        effect_magnitude_->setToolTip(
            tr("Amplifies Sobel edges in linear light; HDR edges remain available."));
        effect_frequency_->setToolTip(
            tr("Whole pixel distance used by the edge detector."));
        effect_secondary_->setToolTip(
            tr("Suppresses gradients below this threshold."));
    } else if (is_twirl) {
        set_form_label(effect_form_, effect_intensity_, tr("Twirl mix"));
        set_form_label(effect_form_, effect_magnitude_, tr("Maximum turns"));
        set_form_label(effect_form_, effect_frequency_, tr("Radial falloff"));
        set_form_label(effect_form_, effect_secondary_, tr("Direction / depth"));
        set_form_label(effect_form_, effect_center_x_, tr("Center X (0–1)"));
        set_form_label(effect_form_, effect_center_y_, tr("Center Y (0–1)"));
        effect_magnitude_->setToolTip(
            tr("Maximum rotation near the center, measured in turns."));
        effect_secondary_->setToolTip(
            tr("Negative values reverse direction; zero is neutral."));
    } else {
        set_form_label(effect_form_, effect_intensity_, tr("Spark brightness"));
        set_form_label(effect_form_, effect_magnitude_, tr("Travel per loop"));
        set_form_label(effect_form_, effect_frequency_, tr("Particle count"));
        set_form_label(effect_form_, effect_secondary_, tr("Trail amount"));
        set_form_label(effect_form_, effect_angle_, tr("Travel angle (degrees)"));
        set_form_label(effect_form_, effect_radius_,
                       tr("Exact base radius (output pixels)"));
        set_form_label(effect_form_, effect_threshold_, tr("White-hot core"));
        set_form_label(effect_form_, effect_knee_, tr("Glow softness"));
        effect_intensity_->setToolTip(
            tr("Adds HDR ember light over the incoming layer."));
        effect_frequency_->setToolTip(
            tr("A deterministic whole particle count fitting the signed-int renderer index."));
        effect_secondary_->setToolTip(
            tr("Extends a fading trail behind each moving spark."));
        effect_radius_->setToolTip(
            tr("Exact base radius in full-resolution output pixels. Use the size slider for playful exploration and this editor for precise renders."));
        effect_threshold_->setToolTip(
            tr("Moves the particle color from warm ember toward a white-hot core."));
        effect_knee_->setToolTip(
            tr("Controls the softness of each particle's glow falloff."));
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
             effect_name_, effect_enabled_, effect_sync_, effect_audio_response_,
             effect_type_, effect_space_,
             effect_cycles_,
             effect_phase_, effect_edge_, effect_intensity_, effect_magnitude_,
             effect_frequency_, effect_secondary_, effect_center_x_, effect_center_y_,
             effect_angle_, effect_radius_, effect_threshold_, effect_knee_,
             effect_area_radius_, effect_particle_shape_, effect_particle_profile_,
             effect_particle_size_scale_, effect_particle_size_variation_,
             effect_particle_definition_, effect_particle_twinkle_,
             effect_particle_seed_, effect_particle_reseed_,
             effect_particle_orientation_, effect_particle_rotation_,
             effect_blur_type_, effect_blur_passes_,
             effect_blur_samples_, effect_blur_minimum_, effect_blur_maximum_}) {
        widget->setEnabled(enabled);
    }
    if (index) {
        const auto& effect = config_.effects[*index];
        effect_name_->setText(QString::fromStdString(effect.name));
        effect_enabled_->setChecked(effect.enabled);
        effect_sync_->setChecked(effect.synchronized);
        select_enum(effect_audio_response_, effect.audio_response);
        effect_audio_response_->setEnabled(
            effective_active_clock_is_music(config_, active_layer_uuid_));
        select_enum(effect_type_, effect.type);
        select_enum(effect_space_, effect.space);
        updateEffectEditorVisibility();
        effect_cycles_->setValue(effect.cycles_per_loop);
        effect_phase_->setValue(effect.phase_degrees);
        select_enum(effect_edge_, effect.edge_mode);
        effect_intensity_->setValue(effect.intensity);
        effect_magnitude_->setValue(effect.magnitude);
        // Block Scale's maximum editor has a dynamic lower bound equal to the
        // loaded minimum. Refresh it here so the previously selected effect
        // cannot clamp this effect's otherwise valid maximum.
        updateEffectEditorVisibility();
        effect_frequency_->setValue(effect.frequency);
        effect_secondary_->setValue(effect.secondary);
        effect_center_x_->setValue(effect.center_x);
        effect_center_y_->setValue(effect.center_y);
        effect_angle_->setValue(effect.angle_degrees);
        effect_radius_->setValue(effect.radius_pixels);
        effect_threshold_->setValue(effect.threshold);
        effect_knee_->setValue(effect.soft_knee);
        effect_area_radius_->setValue(effect.area_radius);
        select_enum(effect_particle_shape_, effect.particle_shape);
        select_enum(effect_particle_profile_, effect.particle_profile);
        effect_particle_size_scale_->setValue(
            particle_slider_from_radius(effect.radius_pixels));
        effect_particle_size_variation_->setValue(
            effect.particle_size_variation);
        effect_particle_definition_->setValue(effect.particle_definition);
        effect_particle_twinkle_->setValue(effect.particle_twinkle);
        effect_particle_seed_->setText(
            QString::number(static_cast<qulonglong>(effect.particle_seed)));
        select_enum(effect_particle_orientation_, effect.particle_orientation);
        effect_particle_rotation_->setValue(
            effect.particle_rotation_degrees);
        select_enum(effect_blur_type_, effect.blur_type);
        effect_blur_passes_->setValue(effect.blur_passes);
        effect_blur_samples_->setValue(effect.blur_samples);
        effect_blur_minimum_->setValue(effect.blur_minimum);
        effect_blur_maximum_->setValue(effect.blur_maximum);
    }
    updateEffectEditorVisibility();
    populating_ = false;
    preview_->setSelectedEffect(index);
}

void MainWindow::loadGlobalEditors() {
    populating_ = true;
    width_->setValue(config_.width);
    height_->setValue(config_.height);
    block_size_->setValue(config_.block_size);
    frames_->setValue(config_.total_frames);
    fps_->setValue(config_.fps);
    if (standardMicRoute(false) != nullptr) {
        clock_mode_->setCurrentIndex(
            std::max(0, clock_mode_->findData(kMicLiveClockSentinel)));
    } else {
        select_enum(clock_mode_, config_.clock.mode);
    }
    select_enum(clock_interpolation_, config_.clock.interpolation);
    select_enum(clock_fit_, config_.clock.fit);
    clock_frame_interval_->setValue(config_.clock.frame_interval);
    clock_time_interval_ms_->setValue(
        static_cast<double>(config_.clock.time_interval_microseconds) / 1000.0);
    meter_expression_->setText(QString::fromStdString(config_.clock.meter.expression));
    meter_bpm_->setValue(config_.clock.meter.bpm);
    meter_tempo_note_->setValue(config_.clock.meter.tempo_note_denominator);
    clock_reverse_->setChecked(config_.clock.reverse);
    clock_phase_offset_->setValue(config_.clock.phase_offset_degrees);
    select_enum(music_tempo_mode_, config_.clock.music_tempo);
    music_beat_offset_ms_->setValue(
        static_cast<double>(config_.clock.beat_offset_microseconds) / 1000.0);
    music_data_only_->setChecked(config_.clock.data_only);
    const auto load_frequency_stream = [](QComboBox* combo,
                                          const pvt::ClockConfig& clock) {
        combo->clear();
        combo->addItem(QObject::tr("Full filtered signal"), QString{});
        for (const auto& stream : clock.audio_processing.frequency_streams) {
            combo->addItem(
                QObject::tr("%1 (%2–%3 Hz)")
                    .arg(QString::fromStdString(stream.name))
                    .arg(stream.low_hz, 0, 'g', 6)
                    .arg(stream.high_hz, 0, 'g', 6),
                QString::fromStdString(stream.uuid));
        }
        const int selected = combo->findData(
            QString::fromStdString(clock.frequency_stream_uuid));
        combo->setCurrentIndex(selected >= 0 ? selected : 0);
    };
    load_frequency_stream(music_frequency_stream_, config_.clock);
    layer_clock_group_->setChecked(config_.layer_clock.enabled);
    layer_clock_mix_enabled_->setChecked(config_.layer_clock.mix_enabled);
    select_enum(layer_clock_mix_mode_, config_.layer_clock.mix);
    select_enum(layer_clock_scale_, config_.layer_clock.scale);
    if (standardMicRoute(true) != nullptr) {
        layer_clock_mode_->setCurrentIndex(
            std::max(0,
                     layer_clock_mode_->findData(kMicLiveClockSentinel)));
    } else {
        select_enum(layer_clock_mode_, config_.layer_clock.clock.mode);
    }
    select_enum(layer_clock_interpolation_,
                config_.layer_clock.clock.interpolation);
    select_enum(layer_clock_fit_, config_.layer_clock.clock.fit);
    layer_clock_frame_interval_->setValue(
        config_.layer_clock.clock.frame_interval);
    layer_clock_time_interval_ms_->setValue(
        static_cast<double>(config_.layer_clock.clock.time_interval_microseconds)
        / 1000.0);
    layer_meter_expression_->setText(
        QString::fromStdString(config_.layer_clock.clock.meter.expression));
    layer_meter_bpm_->setValue(config_.layer_clock.clock.meter.bpm);
    layer_meter_tempo_note_->setValue(
        config_.layer_clock.clock.meter.tempo_note_denominator);
    layer_clock_reverse_->setChecked(config_.layer_clock.clock.reverse);
    layer_clock_phase_offset_->setValue(
        config_.layer_clock.clock.phase_offset_degrees);
    select_enum(layer_music_tempo_mode_,
                config_.layer_clock.clock.music_tempo);
    layer_music_beat_offset_ms_->setValue(
        static_cast<double>(
            config_.layer_clock.clock.beat_offset_microseconds) / 1000.0);
    layer_music_data_only_->setChecked(
        config_.layer_clock.clock.data_only);
    load_frequency_stream(layer_music_frequency_stream_,
                          config_.layer_clock.clock);
    swings_group_->setChecked(config_.swings_enabled);
    project_audio_response_group_->setChecked(
        config_.audio_reactive_defaults.enabled);
    project_audio_sync_only_->setChecked(
        config_.audio_reactive_defaults.synchronized_only);
    project_audio_waves_enabled_->setChecked(
        config_.audio_reactive_defaults.waves_enabled);
    select_enum(project_audio_wave_source_,
                config_.audio_reactive_defaults.wave_source);
    project_audio_wave_amount_->setValue(
        config_.audio_reactive_defaults.wave_amount);
    project_audio_effects_enabled_->setChecked(
        config_.audio_reactive_defaults.effects_enabled);
    select_enum(project_audio_effect_source_,
                config_.audio_reactive_defaults.effect_source);
    project_audio_effect_amount_->setValue(
        config_.audio_reactive_defaults.effect_amount);
    project_audio_color_enabled_->setChecked(
        config_.audio_reactive_defaults.color_enabled);
    select_enum(project_audio_color_source_,
                config_.audio_reactive_defaults.color_source);
    project_audio_color_amount_->setValue(
        config_.audio_reactive_defaults.color_amount_degrees);

    audio_response_group_->setChecked(
        config_.audio_reactive_override_enabled);
    audio_response_enabled_->setChecked(config_.audio_reactive.enabled);
    audio_sync_only_->setChecked(config_.audio_reactive.synchronized_only);
    audio_waves_enabled_->setChecked(config_.audio_reactive.waves_enabled);
    select_enum(audio_wave_source_, config_.audio_reactive.wave_source);
    audio_wave_amount_->setValue(config_.audio_reactive.wave_amount);
    audio_effects_enabled_->setChecked(config_.audio_reactive.effects_enabled);
    select_enum(audio_effect_source_, config_.audio_reactive.effect_source);
    audio_effect_amount_->setValue(config_.audio_reactive.effect_amount);
    audio_color_enabled_->setChecked(config_.audio_reactive.color_enabled);
    select_enum(audio_color_source_, config_.audio_reactive.color_source);
    audio_color_amount_->setValue(config_.audio_reactive.color_amount_degrees);
    phrase_warp_->setValue(config_.phrase_warp);
    ghost_mix_->setValue(config_.ghost_mix);
    ghost_lag_->setValue(config_.ghost_lag_degrees);
    displacement_enabled_->setChecked(config_.displacement_enabled);
    wave_displacement_enabled_->setChecked(config_.displacement_enabled);
    displacement_->setValue(config_.displacement);
    lighting_enabled_->setChecked(config_.lighting_enabled);
    wave_lighting_enabled_->setChecked(config_.lighting_enabled);
    wave_depth_->setValue(config_.wave_depth);
    spiral_enabled_->setChecked(config_.spiral_enabled);
    spiral_frequency_->setValue(config_.spiral_frequency);
    spiral_arms_->setValue(config_.spiral_arms);
    wall_enabled_->setChecked(config_.wall_reflection_enabled);
    wall_frequency_->setValue(config_.wall_frequency);
    wall_mix_->setValue(config_.wall_mix);
    hue_cycles_->setValue(config_.hue_cycles);
    saturation_->setValue(config_.saturation);
    kaleidoscope_group_->setChecked(
        config_.starting_colors.kaleidoscope.enabled);
    kaleidoscope_segments_->setValue(
        config_.starting_colors.kaleidoscope.mirrored_segments);
    kaleidoscope_rotation_->setValue(
        config_.starting_colors.kaleidoscope.rotation_degrees);
    kaleidoscope_mix_->setValue(config_.starting_colors.kaleidoscope.mix);
    domain_warp_group_->setChecked(
        config_.starting_colors.domain_warp.enabled);
    domain_warp_strength_->setValue(
        config_.starting_colors.domain_warp.strength);
    domain_warp_scale_->setValue(config_.starting_colors.domain_warp.scale);
    domain_warp_octaves_->setValue(
        config_.starting_colors.domain_warp.octaves);
    domain_warp_cycles_->setValue(
        config_.starting_colors.domain_warp.cycles_per_loop);
    domain_warp_seed_->setText(QString::number(static_cast<qulonglong>(
        config_.starting_colors.domain_warp.seed)));
    starting_image_enabled_->setChecked(config_.starting_image.enabled);
    starting_image_path_->setText(
        QString::fromStdString(config_.starting_image.basename));
    select_enum(starting_image_fit_, config_.starting_image.fit);
    starting_image_palette_dither_->setChecked(
        config_.starting_image.palette_dither_enabled);
    select_enum(starting_image_palette_dither_method_,
                config_.starting_image.palette_dither_method);
    starting_image_palette_dither_method_->setEnabled(
        config_.starting_image.palette_dither_enabled);
    surface_enabled_->setChecked(config_.surface.enabled);
    select_enum(surface_mapping_, config_.surface.mapping);
    surface_obj_path_->setText(QString::fromStdString(config_.surface.obj_path));
    surface_rotations_->setValue(config_.surface.rotations_per_loop);
    surface_phase_->setValue(config_.surface.phase_degrees);
    surface_curvature_->setValue(config_.surface.curvature);
    surface_lighting_->setValue(config_.surface.lighting);
    const auto& plane_displacement =
        config_.surface.plane_displacement;
    surface_plane_displacement_enabled_->setChecked(
        plane_displacement.enabled);
    surface_plane_displacement_path_->setText(
        QString::fromStdString(plane_displacement.basename));
    surface_plane_displacement_minimum_->setValue(
        plane_displacement.minimum);
    surface_plane_displacement_maximum_->setValue(
        plane_displacement.maximum);
    surface_plane_displacement_midpoint_->setValue(
        plane_displacement.midpoint);
    surface_plane_displacement_ratio_->setValue(
        plane_displacement.pixels_per_node);
    transform_flip_horizontal_->setChecked(config_.transform.flip_horizontal);
    transform_flip_vertical_->setChecked(config_.transform.flip_vertical);
    select_enum(transform_mirror_, config_.transform.mirror);
    motion_group_->setChecked(config_.motion.enabled);
    select_enum(motion_path_, config_.motion.path);
    motion_center_x_->setValue(config_.motion.center_x);
    motion_center_y_->setValue(config_.motion.center_y);
    motion_travel_x_->setValue(config_.motion.travel_x);
    motion_travel_y_->setValue(config_.motion.travel_y);
    motion_cycles_x_->setValue(config_.motion.cycles_x);
    motion_cycles_y_->setValue(config_.motion.cycles_y);
    motion_phase_->setValue(config_.motion.phase_degrees);
    motion_rotations_->setValue(config_.motion.rotations_per_loop);
    motion_rotation_offset_->setValue(
        config_.motion.rotation_offset_degrees);
    motion_scale_pulse_->setValue(config_.motion.scale_pulse);
    palette_enabled_->setChecked(config_.palette.enabled);
    palette_name_->setText(QString::fromStdString(config_.palette.name));
    refreshPaletteEditor();
    select_enum(starting_color_mode_, config_.starting_colors.mode);
    starting_color_include_alpha_->setChecked(
        config_.starting_colors.include_alpha);
    starting_red_minimum_->setValue(config_.starting_colors.red_minimum);
    starting_red_maximum_->setValue(config_.starting_colors.red_maximum);
    starting_green_minimum_->setValue(config_.starting_colors.green_minimum);
    starting_green_maximum_->setValue(config_.starting_colors.green_maximum);
    starting_blue_minimum_->setValue(config_.starting_colors.blue_minimum);
    starting_blue_maximum_->setValue(config_.starting_colors.blue_maximum);
    starting_alpha_minimum_->setValue(config_.starting_colors.alpha_minimum);
    starting_alpha_maximum_->setValue(config_.starting_colors.alpha_maximum);
    post_invert_rgb_enabled_->setChecked(
        config_.post_process.invert_rgb_enabled);
    post_invert_rgb_mix_->setValue(config_.post_process.invert_rgb_mix);
    post_invert_alpha_enabled_->setChecked(
        config_.post_process.invert_alpha_enabled);
    post_invert_alpha_mix_->setValue(config_.post_process.invert_alpha_mix);
    post_antialias_enabled_->setChecked(
        config_.post_process.antialias_enabled);
    post_antialias_strength_->setValue(
        config_.post_process.antialias_strength);
    post_antialias_threshold_->setValue(
        config_.post_process.antialias_threshold);
    post_antialias_passes_->setValue(config_.post_process.antialias_passes);
    updateSurfaceEditorState();
    updatePostProcessEditorState();
    quantization_enabled_->setChecked(config_.quantization.enabled);
    quantization_levels_->setValue(config_.quantization.levels);
    quantization_mix_->setValue(config_.quantization.mix);
    select_enum(quantization_mode_, config_.quantization.mode);
    alpha_enabled_->setChecked(config_.alpha.enabled);
    alpha_use_source_->setChecked(config_.alpha.use_source_alpha);
    alpha_minimum_->setValue(config_.alpha.minimum);
    alpha_maximum_->setValue(config_.alpha.maximum);
    alpha_frequency_->setValue(config_.alpha.spatial_frequency);
    alpha_cycles_->setValue(config_.alpha.cycles_per_loop);
    alpha_phase_->setValue(config_.alpha.phase_degrees);
    bit_depth_->setCurrentIndex(std::max(0, bit_depth_->findData(config_.output.bit_depth)));
    const bool float_output = config_.output.bit_depth == 32;
    png_compression_->setValue(config_.output.png_compression_level);
    png_compression_->setEnabled(!float_output);
    if (!float_output) {
        integer_dither_preference_ = config_.output.dither_enabled;
    } else {
        config_.output.dither_enabled = false;
    }
    dither_enabled_->setChecked(float_output ? false : integer_dither_preference_);
    dither_enabled_->setEnabled(!float_output);
    select_enum(dither_method_, config_.output.dither_method);
    dither_method_->setEnabled(!float_output && integer_dither_preference_);
    write_alpha_->setChecked(config_.output.write_alpha);
    output_directory_->setText(QString::fromStdString(config_.output.output_directory));
    prefix_->setText(QString::fromStdString(config_.output.filename_prefix));
    first_frame_->setValue(config_.output.first_frame_number);
    filename_digits_->setValue(config_.output.filename_digits);
    overwrite_->setChecked(config_.output.overwrite_existing);
    populating_ = false;
    refreshStandardMicControls();
    updateWaveOutputState();
    updateOutputEditorValidity();
    updateSynchronizationState();
}

void MainWindow::updateWaveOutputState() {
    if (wave_output_status_ == nullptr
        || wave_displacement_enabled_ == nullptr
        || wave_lighting_enabled_ == nullptr) {
        return;
    }
    {
        const QSignalBlocker displacement_blocker(
            wave_displacement_enabled_);
        const QSignalBlocker lighting_blocker(wave_lighting_enabled_);
        wave_displacement_enabled_->setChecked(
            config_.displacement_enabled);
        wave_lighting_enabled_->setChecked(config_.lighting_enabled);
    }
    const bool has_enabled_wave = std::any_of(
        config_.waves.cbegin(), config_.waves.cend(),
        [](const pvt::WaveConfig& wave) { return wave.enabled; });
    if (!has_enabled_wave) {
        wave_output_status_->setText(tr(
            "Add or enable a wave, then choose at least one output below."));
        wave_output_status_->setStyleSheet(QString{});
    } else if (!config_.displacement_enabled
               && !config_.lighting_enabled) {
        wave_output_status_->setText(tr(
            "This layer's enabled waves cannot affect pixels because both wave outputs are off."));
        wave_output_status_->setStyleSheet(
            QStringLiteral("color: #ff6b6b; font-weight: 600;"));
    } else if (config_.displacement_enabled
               && config_.lighting_enabled) {
        wave_output_status_->setText(tr(
            "Enabled waves drive both generated-pattern displacement and slope lighting."));
        wave_output_status_->setStyleSheet(QString{});
    } else if (config_.displacement_enabled) {
        wave_output_status_->setText(tr(
            "Enabled waves drive generated-pattern displacement."));
        wave_output_status_->setStyleSheet(QString{});
    } else {
        wave_output_status_->setText(tr(
            "Enabled waves drive slope lighting."));
        wave_output_status_->setStyleSheet(QString{});
    }
}

void MainWindow::updateSurfaceEditorState() {
    if (surface_mapping_ == nullptr || surface_obj_row_ == nullptr
        || surface_plane_displacement_group_ == nullptr) {
        return;
    }
    const auto mapping = static_cast<pvt::SurfaceMapping>(
        surface_mapping_->currentData().toInt());
    const bool custom_obj = mapping == pvt::SurfaceMapping::CustomObj;
    const bool plane = mapping == pvt::SurfaceMapping::Plane;
    surface_obj_row_->setVisible(custom_obj);
    if (surface_obj_label_ != nullptr) {
        surface_obj_label_->setVisible(custom_obj);
    }
    surface_plane_displacement_group_->setVisible(plane);
    const bool has_height_map =
        !config_.surface.plane_displacement.path.empty()
        || !config_.surface.plane_displacement.sha256.empty();
    surface_plane_displacement_clear_->setEnabled(has_height_map);
    surface_plane_displacement_export_->setEnabled(has_height_map);
    surface_plane_displacement_path_->setToolTip(
        has_height_map
            ? tr("Embedded project asset: %1")
                  .arg(QString::fromStdString(
                      config_.surface.plane_displacement.basename))
            : tr("Choose remains available while displacement use is off."));
    surface_curvature_->setToolTip(
        plane && has_height_map
            ? tr("Crossfade from the original flat image at 0 to the fully projected displacement mesh at 1.")
            : tr("Crossfade from the original flat image at 0 to the fully mapped 3D surface at 1."));
}

void MainWindow::updatePostProcessEditorState() {
    if (post_invert_rgb_enabled_ == nullptr
        || post_invert_rgb_mix_ == nullptr
        || post_invert_alpha_enabled_ == nullptr
        || post_invert_alpha_mix_ == nullptr
        || post_antialias_enabled_ == nullptr
        || post_antialias_strength_ == nullptr
        || post_antialias_threshold_ == nullptr
        || post_antialias_passes_ == nullptr) {
        return;
    }
    post_invert_rgb_mix_->setEnabled(post_invert_rgb_enabled_->isChecked());
    post_invert_alpha_mix_->setEnabled(
        post_invert_alpha_enabled_->isChecked());
    const bool antialiasing = post_antialias_enabled_->isChecked();
    post_antialias_strength_->setEnabled(antialiasing);
    post_antialias_threshold_->setEnabled(antialiasing);
    post_antialias_passes_->setEnabled(antialiasing);
}

void MainWindow::refreshPaletteEditor() {
    if (palette_colors_ == nullptr) return;
    const int previous_row = palette_colors_->currentRow();
    const QSignalBlocker blocker(palette_colors_);
    palette_colors_->clear();
    for (std::size_t index = 0U; index < config_.palette.colors.size(); ++index) {
        const pvt::PaletteColor& value = config_.palette.colors[index];
        const QColor color = palette_display_color(value);
        const QString entry_name = value.name.empty()
                                       ? tr("Unnamed")
                                       : QString::fromStdString(value.name);
        auto* item = new QListWidgetItem(
            tr("%1. %2 — %3 · %4 · RGBA(%5, %6, %7, %8)")
                .arg(static_cast<qulonglong>(index + 1U))
                .arg(entry_name)
                .arg(color.name(QColor::HexArgb).toUpper())
                .arg(QString::fromUtf8(
                    pvt::palette_color_encoding_name(value.encoding)))
                .arg(value.red, 0, 'g', 7)
                .arg(value.green, 0, 'g', 7)
                .arg(value.blue, 0, 'g', 7)
                .arg(value.alpha, 0, 'g', 7),
            palette_colors_);
        item->setBackground(color);
        const double luminance = 0.2126 * color.redF()
                                 + 0.7152 * color.greenF()
                                 + 0.0722 * color.blueF();
        item->setForeground(luminance > 0.5 ? Qt::black : Qt::white);
        if (value.encoding == pvt::PaletteColorEncoding::Linear) {
            item->setToolTip(tr(
                "Linear-light values are preserved exactly. This list swatch is tone-clipped to the displayable sRGB range."));
        }
        item->setData(Qt::UserRole, static_cast<qulonglong>(index));
    }
    if (!config_.palette.colors.empty()) {
        palette_colors_->setCurrentRow(std::clamp(
            previous_row, 0,
            static_cast<int>(config_.palette.colors.size()) - 1));
    }
}

void MainWindow::applyPalettePreset(std::size_t index) {
    if (populating_) return;
    auto before = captureActiveState();
    const bool was_enabled = config_.palette.enabled;
    config_.palette = pvt::default_palette(index);
    config_.palette.enabled = was_enabled;
    syncActiveRender();
    loadGlobalEditors();
    schedulePreview();
    recordActiveStateChange(tr("Use palette preset"), std::move(before));
}

void MainWindow::savePaletteToLibrary() {
    if (config_.palette.colors.empty()) {
        QMessageBox::information(this, tr("Nothing to save"),
                                 tr("Add at least one palette color first."));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Save palette"), tr("Library name"), QLineEdit::Normal,
        QString::fromStdString(config_.palette.name), &accepted).trimmed();
    if (!accepted) return;
    if (name.isEmpty() || !valid_text(name, TextRule::Name)) {
        QMessageBox::warning(this, tr("Invalid palette name"),
                             tr("Enter a non-empty palette name without control characters."));
        return;
    }

    QSettings settings;
    QJsonArray entries;
    const QByteArray existing_bytes = settings.value(
        QStringLiteral("paletteLibrary/entries")).toByteArray();
    const QJsonDocument existing =
        static_cast<std::size_t>(existing_bytes.size())
                <= pvt::palette_io::kMaximumPaletteFileBytes
            ? QJsonDocument::fromJson(existing_bytes)
            : QJsonDocument{};
    if (existing.isArray()) entries = existing.array();
    for (qsizetype index = entries.size() - 1; index >= 0; --index) {
        if (entries.at(index).toObject().value(QStringLiteral("name")).toString()
                .compare(name, Qt::CaseInsensitive) == 0) {
            const auto answer = QMessageBox::question(
                this, tr("Replace saved palette?"),
                tr("A palette named “%1” already exists. Replace it?").arg(name));
            if (answer != QMessageBox::Yes) return;
            entries.removeAt(index);
        }
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("schema"), 2);
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("columns"),
                 static_cast<double>(config_.palette.columns));
    QJsonArray colors;
    for (const auto& color : config_.palette.colors) {
        QJsonObject encoded;
        encoded.insert(QStringLiteral("rgba"),
                       QJsonArray{color.red, color.green, color.blue,
                                  color.alpha});
        encoded.insert(QStringLiteral("name"),
                       QString::fromStdString(color.name));
        encoded.insert(
            QStringLiteral("encoding"),
            color.encoding == pvt::PaletteColorEncoding::Linear
                ? QStringLiteral("linear")
                : QStringLiteral("srgb"));
        colors.append(encoded);
    }
    entry.insert(QStringLiteral("colors"), colors);
    entries.append(entry);
    const QByteArray serialized =
        QJsonDocument(entries).toJson(QJsonDocument::Compact);
    if (static_cast<std::size_t>(serialized.size())
        > pvt::palette_io::kMaximumPaletteFileBytes) {
        QMessageBox::warning(
            this, tr("Palette library limit"),
            tr("The local palette library would exceed its 64 MiB safety limit. Export this palette to a file instead."));
        return;
    }
    settings.setValue(QStringLiteral("paletteLibrary/entries"), serialized);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::critical(this, tr("Could not save palette"),
                              tr("The local palette library could not be written."));
        return;
    }
    status_->setText(tr("Saved “%1” to the local palette library.").arg(name));
}

void MainWindow::loadPaletteFromLibraryOrLayer() {
    struct Choice {
        QString label;
        pvt::PaletteConfig palette;
    };
    std::vector<Choice> choices;
    for (const auto& layer : project_.layers) {
        if (layer.uuid == active_layer_uuid_ || layer.render.palette.colors.empty()) {
            continue;
        }
        choices.push_back({
            tr("Project layer: %1 — %2")
                .arg(QString::fromStdString(layer.name),
                     QString::fromStdString(layer.render.palette.name)),
            layer.render.palette});
    }

    const QByteArray saved_bytes = QSettings().value(
        QStringLiteral("paletteLibrary/entries")).toByteArray();
    const QJsonDocument saved = static_cast<std::size_t>(saved_bytes.size())
                                    <= pvt::palette_io::kMaximumPaletteFileBytes
                                ? QJsonDocument::fromJson(saved_bytes)
                                : QJsonDocument{};
    if (saved.isArray()) {
        for (const QJsonValue& value : saved.array()) {
            const QJsonObject entry = value.toObject();
            const int schema = entry.value(QStringLiteral("schema")).toInt(1);
            const QString name = entry.value(QStringLiteral("name")).toString().trimmed();
            const QJsonArray colors = entry.value(QStringLiteral("colors")).toArray();
            if ((schema != 1 && schema != 2) || name.isEmpty() || colors.isEmpty()
                || colors.size() > static_cast<qsizetype>(pvt::kMaximumPaletteColors)) {
                continue;
            }
            pvt::PaletteConfig palette;
            palette.enabled = true;
            palette.name = name.toUtf8().toStdString();
            if (schema == 2) {
                const double columns = entry.value(QStringLiteral("columns"))
                                           .toDouble(-1.0);
                if (!std::isfinite(columns) || columns < 0.0
                    || columns > static_cast<double>(pvt::kMaximumPaletteColors)
                    || std::floor(columns) != columns) {
                    continue;
                }
                palette.columns = static_cast<std::size_t>(columns);
            }
            bool valid = true;
            for (const QJsonValue& color_value : colors) {
                const QJsonObject encoded = color_value.toObject();
                const QJsonArray rgba = schema == 1
                                            ? color_value.toArray()
                                            : encoded.value(QStringLiteral("rgba")).toArray();
                if (rgba.size() != 4) {
                    valid = false;
                    break;
                }
                pvt::PaletteColor color;
                color.red = rgba.at(0).toDouble(-1.0);
                color.green = rgba.at(1).toDouble(-1.0);
                color.blue = rgba.at(2).toDouble(-1.0);
                color.alpha = rgba.at(3).toDouble(-1.0);
                if (schema == 2) {
                    color.name = encoded.value(QStringLiteral("name"))
                                     .toString().toUtf8().toStdString();
                    const QString encoding = encoded.value(
                        QStringLiteral("encoding")).toString();
                    if (encoding == QStringLiteral("linear")) {
                        color.encoding = pvt::PaletteColorEncoding::Linear;
                    } else if (encoding != QStringLiteral("srgb")) {
                        valid = false;
                        break;
                    }
                }
                const double maximum = color.encoding
                                               == pvt::PaletteColorEncoding::Linear
                                           ? static_cast<double>(
                                                 (std::numeric_limits<float>::max)())
                                           : 1.0;
                const double minimum = color.encoding
                                               == pvt::PaletteColorEncoding::Linear
                                           ? -maximum
                                           : 0.0;
                if (!std::isfinite(color.red) || color.red < minimum
                    || color.red > maximum || !std::isfinite(color.green)
                    || color.green < minimum || color.green > maximum
                    || !std::isfinite(color.blue) || color.blue < minimum
                    || color.blue > maximum || !std::isfinite(color.alpha)
                    || color.alpha < 0.0 || color.alpha > 1.0
                    || !valid_text(QString::fromUtf8(
                                       color.name.data(),
                                       static_cast<qsizetype>(color.name.size())),
                                   TextRule::Name)) {
                    valid = false;
                    break;
                }
                palette.colors.push_back(color);
            }
            if (valid) {
                choices.push_back({tr("Saved palette: %1").arg(name),
                                   std::move(palette)});
            }
        }
    }

    if (choices.empty()) {
        QMessageBox::information(
            this, tr("No reusable palettes"),
            tr("No other layer or locally saved palette is available yet."));
        return;
    }
    QStringList labels;
    for (const auto& choice : choices) labels.push_back(choice.label);
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        this, tr("Load starting palette"), tr("Palette"), labels, 0, false,
        &accepted);
    if (!accepted) return;
    const qsizetype index = labels.indexOf(selected);
    if (index < 0 || static_cast<std::size_t>(index) >= choices.size()) return;
    auto before = captureActiveState();
    config_.palette = choices[static_cast<std::size_t>(index)].palette;
    config_.palette.enabled = true;
    syncActiveRender();
    loadGlobalEditors();
    schedulePreview();
    recordActiveStateChange(tr("Load reusable palette"), std::move(before));
    status_->setText(tr("Loaded %1.").arg(selected));
}

void MainWindow::importPaletteFile() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import starting palette"), usableDialogDirectory(),
        palette_file_filter());
    if (path.isEmpty()) return;
    rememberDialogLocation(path);

    const std::string utf8_path = path.toUtf8().toStdString();
    const auto format = pvt::palette_io::format_from_path(utf8_path);
    pvt::palette_io::PaletteDocument document;
    pvt::palette_io::PaletteIoSummary summary;
    std::string error;
    if (!pvt::palette_io::import_palette(
            utf8_path, format, document, summary, &error)) {
        QMessageBox::critical(
            this, tr("Could not import palette"),
            tr("%1\n\n%2")
                .arg(path, QString::fromStdString(error)));
        return;
    }

    pvt::PaletteConfig imported;
    imported.enabled = true;
    imported.name = document.name.empty() ? std::string("Imported Palette")
                                          : document.name;
    imported.columns = document.columns.value_or(0U);
    imported.colors.reserve(document.entries.size());
    for (const auto& entry : document.entries) {
        pvt::PaletteColor color;
        color.red = entry.red;
        color.green = entry.green;
        color.blue = entry.blue;
        color.alpha = entry.alpha;
        color.name = entry.name;
        color.encoding = entry.source_encoding
                                 == pvt::palette_io::ColorEncoding::Linear
                             ? pvt::PaletteColorEncoding::Linear
                             : pvt::PaletteColorEncoding::Srgb;
        imported.colors.push_back(std::move(color));
    }

    const bool append_fits = imported.colors.size()
        <= pvt::kMaximumPaletteColors - config_.palette.colors.size();
    QMessageBox decision(this);
    decision.setIcon(QMessageBox::Information);
    decision.setWindowTitle(tr("Palette import summary"));
    decision.setText(tr("Review the import before changing this layer."));
    decision.setInformativeText(
        palette_io_summary_text(path, format, summary)
        + tr("\n\nReplace the current palette or append these values?"));
    QAbstractButton* replace = decision.addButton(
        tr("Replace Palette"), QMessageBox::AcceptRole);
    QAbstractButton* append = decision.addButton(
        tr("Append Values"), QMessageBox::ActionRole);
    QAbstractButton* cancel = decision.addButton(QMessageBox::Cancel);
    append->setEnabled(append_fits);
    if (!append_fits) {
        append->setToolTip(tr(
            "Appending would exceed the signed-int palette index limit."));
    }
    decision.setDefaultButton(
        qobject_cast<QPushButton*>(replace));
    decision.exec();
    if (decision.clickedButton() == cancel || decision.clickedButton() == nullptr) {
        return;
    }

    const bool appending = decision.clickedButton() == append;
    pvt::RenderConfig candidate = config_;
    if (!appending) {
        candidate.palette = std::move(imported);
    } else {
        const bool was_empty = candidate.palette.colors.empty();
        if (was_empty) {
            candidate.palette.name = imported.name;
            candidate.palette.columns = imported.columns;
        } else {
            // Once two independently laid-out palettes are concatenated there
            // is no honest single source grid width to retain.
            candidate.palette.columns = 0U;
        }
        candidate.palette.colors.insert(
            candidate.palette.colors.end(),
            std::make_move_iterator(imported.colors.begin()),
            std::make_move_iterator(imported.colors.end()));
        candidate.palette.enabled = true;
    }
    // palette_io deliberately preserves finite linear/HDR source values. The
    // renderer has a tighter float-domain contract, so validate the complete
    // candidate transaction before changing the project. Account for the same
    // automatic alpha-output adjustment that will be applied after commit.
    if (visible_stack_requires_alpha(project_, &candidate,
                                     active_layer_uuid_)) {
        candidate.output.write_alpha = true;
    }
    const pvt::ValidationResult validation = pvt::validate(candidate);
    if (!validation.ok) {
        QMessageBox::critical(
            this, tr("Palette cannot be used by this project"),
            tr("The file was imported without changing the layer, but the "
               "result would violate a renderer limit:\n\n%1")
                .arg(QString::fromStdString(validation.message)));
        return;
    }

    auto before = captureActiveState();
    config_.palette = std::move(candidate.palette);
    syncActiveRender();
    ensureAlphaForTransparency();
    loadGlobalEditors();
    schedulePreview();
    recordActiveStateChange(appending ? tr("Append imported palette")
                                      : tr("Import palette"),
                            std::move(before));
    status_->setText(
        tr("Imported %1 palette value(s) from %2.")
            .arg(static_cast<qulonglong>(summary.accepted))
            .arg(QFileInfo(path).fileName()));
}

void MainWindow::exportPaletteFile() {
    if (config_.palette.colors.empty()) {
        QMessageBox::information(this, tr("Nothing to export"),
                                 tr("Add at least one palette color first."));
        return;
    }
    QString selected_filter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export starting palette"),
        QDir(usableDialogDirectory()).filePath(QStringLiteral("palette.gpl")),
        palette_file_filter(), &selected_filter);
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) {
        QString suffix = QStringLiteral(".gpl");
        if (selected_filter.contains(QStringLiteral("*.kpl"))) suffix = QStringLiteral(".kpl");
        else if (selected_filter.contains(QStringLiteral("*.css"))) suffix = QStringLiteral(".css");
        else if (selected_filter.contains(QStringLiteral("*.py"))) suffix = QStringLiteral(".py");
        else if (selected_filter.contains(QStringLiteral("*.php"))) suffix = QStringLiteral(".php");
        else if (selected_filter.contains(QStringLiteral("*.java"))) suffix = QStringLiteral(".java");
        else if (selected_filter.contains(QStringLiteral("*.txt"))) suffix = QStringLiteral(".txt");
        else if (selected_filter.contains(QStringLiteral("*.png"))) suffix = QStringLiteral(".png");
        else if (selected_filter.contains(QStringLiteral("*.exr"))) suffix = QStringLiteral(".exr");
        path += suffix;
    }
    rememberDialogLocation(path);
    const bool exists = QFileInfo::exists(path);
    if (exists
        && QMessageBox::question(
               this, tr("Replace palette file?"),
               tr("%1 already exists. Replace it atomically?").arg(path),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    pvt::palette_io::PaletteDocument document;
    document.name = config_.palette.name;
    if (config_.palette.columns > 0U) {
        document.columns = config_.palette.columns;
    }
    document.entries.reserve(config_.palette.colors.size());
    for (std::size_t index = 0U; index < config_.palette.colors.size(); ++index) {
        const pvt::PaletteColor& color = config_.palette.colors[index];
        pvt::palette_io::PaletteEntry entry;
        entry.name = color.name;
        entry.red = color.red;
        entry.green = color.green;
        entry.blue = color.blue;
        entry.alpha = color.alpha;
        entry.source_encoding = color.encoding == pvt::PaletteColorEncoding::Linear
                                    ? pvt::palette_io::ColorEncoding::Linear
                                    : pvt::palette_io::ColorEncoding::SRGB;
        entry.source_order = index;
        document.entries.push_back(std::move(entry));
    }

    const std::string utf8_path = path.toUtf8().toStdString();
    const auto format = pvt::palette_io::format_from_path(utf8_path);
    pvt::palette_io::PaletteIoSummary summary;
    std::string error;
    if (!pvt::palette_io::export_palette(
            utf8_path, format, document, exists, summary, &error)) {
        QMessageBox::critical(
            this, tr("Could not export palette"),
            tr("%1\n\n%2").arg(path, QString::fromStdString(error)));
        return;
    }
    QMessageBox::information(
        this, tr("Palette exported"),
        palette_io_summary_text(path, format, summary));
    status_->setText(
        tr("Exported %1 palette value(s) to %2.")
            .arg(static_cast<qulonglong>(summary.accepted))
            .arg(QFileInfo(path).fileName()));
}

void MainWindow::generateRandomPalette() {
    QSettings settings;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Generate constrained random palette"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* count = integer_editor(1, kMaximumIntegerParameter);
    count->setValue(settings.value(QStringLiteral("paletteRandom/count"), 8).toInt());
    auto* seed_method = new QComboBox;
    seed_method->addItem(tr("Manual deterministic seed"), 0);
    seed_method->addItem(tr("Very poor: fixed seed 1"), 1);
    seed_method->addItem(tr("Poor: wall-clock seconds"), 2);
    seed_method->addItem(tr("Fast pseudorandom seed"), 3);
    seed_method->addItem(tr("Secure operating-system entropy"), 4);
    seed_method->setCurrentIndex((std::clamp)(
        settings.value(QStringLiteral("paletteRandom/seedMethod"), 4).toInt(), 0, 4));
    auto* seed = new QLineEdit(
        settings.value(QStringLiteral("paletteRandom/manualSeed"),
                       QStringLiteral("1")).toString());
    seed->setPlaceholderText(tr("Unsigned 64-bit integer"));
    auto* warmup = integer_editor(0, kMaximumIntegerParameter);
    warmup->setValue(settings.value(QStringLiteral("paletteRandom/warmup"), 0).toInt());
    warmup->setToolTip(tr(
        "Discards this many generator outputs before palette creation. Each draw is "
        "already uniform; this is exposed for experimentation and reproducibility."));
    auto* aesthetic = new QCheckBox(tr("Use aesthetic relationship rules"));
    aesthetic->setChecked(settings.value(
        QStringLiteral("paletteRandom/aesthetic"), true).toBool());
    auto* rules_button = new QPushButton(tr("Choose relationship rules…"));
    form->addRow(tr("Number of colors"), count);
    form->addRow(tr("Seed source"), seed_method);
    form->addRow(tr("Manual seed"), seed);
    form->addRow(tr("Generator warm-up draws"), warmup);
    form->addRow(aesthetic);
    form->addRow(rules_button);
    layout->addLayout(form);

    auto* channel_grid = new QGridLayout;
    channel_grid->addWidget(new QLabel(tr("Channel")), 0, 0);
    channel_grid->addWidget(new QLabel(tr("Minimum")), 0, 1);
    channel_grid->addWidget(new QLabel(tr("Maximum")), 0, 2);
    std::array<QDoubleSpinBox*, 4> minimums{};
    std::array<QDoubleSpinBox*, 4> maximums{};
    const std::array<QString, 4> channel_names = {
        tr("Red"), tr("Green"), tr("Blue"), tr("Alpha")};
    for (int channel = 0; channel < 4; ++channel) {
        minimums[static_cast<std::size_t>(channel)] =
            real_editor(0.0, 1.0, 4, 0.01);
        maximums[static_cast<std::size_t>(channel)] =
            real_editor(0.0, 1.0, 4, 0.01);
        minimums[static_cast<std::size_t>(channel)]->setValue(
            settings.value(QStringLiteral("paletteRandom/min%1").arg(channel),
                           channel == 3 ? 1.0 : 0.0).toDouble());
        maximums[static_cast<std::size_t>(channel)]->setValue(
            settings.value(QStringLiteral("paletteRandom/max%1").arg(channel),
                           1.0).toDouble());
        channel_grid->addWidget(new QLabel(channel_names[static_cast<std::size_t>(channel)]),
                                channel + 1, 0);
        channel_grid->addWidget(minimums[static_cast<std::size_t>(channel)],
                                channel + 1, 1);
        channel_grid->addWidget(maximums[static_cast<std::size_t>(channel)],
                                channel + 1, 2);
    }
    layout->addLayout(channel_grid);

    std::array<bool, 5> rules = {
        settings.value(QStringLiteral("paletteRandom/ruleMonochrome"), true).toBool(),
        settings.value(QStringLiteral("paletteRandom/ruleComplementary"), true).toBool(),
        settings.value(QStringLiteral("paletteRandom/ruleAnalogous"), true).toBool(),
        settings.value(QStringLiteral("paletteRandom/ruleContrast"), true).toBool(),
        settings.value(QStringLiteral("paletteRandom/ruleAlpha"), true).toBool()};
    connect(rules_button, &QPushButton::clicked, &dialog, [&dialog, &rules] {
        QDialog rules_dialog(&dialog);
        rules_dialog.setWindowTitle(QObject::tr("Aesthetic relationship rules"));
        auto* rules_layout = new QVBoxLayout(&rules_dialog);
        const std::array<QString, 5> labels = {
            QObject::tr("Monochrome RGB shades with a small complementary accent"),
            QObject::tr("Complementary RGB hue pairs"),
            QObject::tr("Analogous RGB hues within a narrow neighborhood"),
            QObject::tr("Alternating light and dark RGB luminance"),
            QObject::tr("Even alpha cadence from the allowed minimum to maximum")};
        std::array<QCheckBox*, 5> boxes{};
        for (std::size_t index = 0; index < boxes.size(); ++index) {
            boxes[index] = new QCheckBox(labels[index]);
            boxes[index]->setChecked(rules[index]);
            rules_layout->addWidget(boxes[index]);
        }
        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QObject::connect(buttons, &QDialogButtonBox::accepted,
                         &rules_dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected,
                         &rules_dialog, &QDialog::reject);
        rules_layout->addWidget(buttons);
        if (rules_dialog.exec() == QDialog::Accepted) {
            for (std::size_t index = 0; index < boxes.size(); ++index) {
                rules[index] = boxes[index]->isChecked();
            }
        }
    });
    const auto update_seed_state = [seed_method, seed] {
        seed->setEnabled(seed_method->currentData().toInt() == 0);
    };
    connect(seed_method, &QComboBox::currentIndexChanged, &dialog,
            update_seed_state);
    update_seed_state();

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Generate"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) return;

    std::array<double, 4> mins{};
    std::array<double, 4> maxes{};
    for (std::size_t channel = 0; channel < mins.size(); ++channel) {
        mins[channel] = minimums[channel]->value();
        maxes[channel] = maximums[channel]->value();
        if (mins[channel] > maxes[channel]) {
            QMessageBox::warning(this, tr("Invalid channel range"),
                                 tr("Every channel minimum must be no greater than its maximum."));
            return;
        }
    }

    bool seed_ok = false;
    quint64 selected_seed = seed->text().trimmed().toULongLong(&seed_ok, 0);
    const int method = seed_method->currentData().toInt();
    if (method == 0 && !seed_ok) {
        QMessageBox::warning(this, tr("Invalid seed"),
                             tr("The manual seed must be an unsigned 64-bit integer."));
        return;
    }
    if (method == 1) selected_seed = 1;
    if (method == 2) selected_seed = static_cast<quint64>(QDateTime::currentSecsSinceEpoch());
    if (method == 3) selected_seed = QRandomGenerator::global()->generate64();
    if (method == 4) selected_seed = QRandomGenerator::system()->generate64();

    settings.setValue(QStringLiteral("paletteRandom/count"), count->value());
    settings.setValue(QStringLiteral("paletteRandom/seedMethod"), method);
    settings.setValue(QStringLiteral("paletteRandom/manualSeed"), seed->text().trimmed());
    settings.setValue(QStringLiteral("paletteRandom/warmup"), warmup->value());
    settings.setValue(QStringLiteral("paletteRandom/aesthetic"), aesthetic->isChecked());
    settings.setValue(QStringLiteral("paletteRandom/ruleMonochrome"), rules[0]);
    settings.setValue(QStringLiteral("paletteRandom/ruleComplementary"), rules[1]);
    settings.setValue(QStringLiteral("paletteRandom/ruleAnalogous"), rules[2]);
    settings.setValue(QStringLiteral("paletteRandom/ruleContrast"), rules[3]);
    settings.setValue(QStringLiteral("paletteRandom/ruleAlpha"), rules[4]);
    for (int channel = 0; channel < 4; ++channel) {
        settings.setValue(QStringLiteral("paletteRandom/min%1").arg(channel),
                          mins[static_cast<std::size_t>(channel)]);
        settings.setValue(QStringLiteral("paletteRandom/max%1").arg(channel),
                          maxes[static_cast<std::size_t>(channel)]);
    }

    std::mt19937_64 random(selected_seed);
    random.discard(static_cast<unsigned long long>(warmup->value()));
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const auto ranged = [&unit, &random](double minimum, double maximum) {
        return minimum + (maximum - minimum) * unit(random);
    };
    std::vector<int> enabled_rules;
    if (aesthetic->isChecked()) {
        for (int index = 0; index < static_cast<int>(rules.size()); ++index) {
            if (rules[static_cast<std::size_t>(index)]) enabled_rules.push_back(index);
        }
    }
    int selected_rule = -1;
    if (!enabled_rules.empty()) {
        std::uniform_int_distribution<std::size_t> choose(
            0, enabled_rules.size() - 1U);
        selected_rule = enabled_rules[choose(random)];
    }

    const double base_hue = unit(random);
    const auto clamp_channel = [&mins, &maxes](double value, std::size_t channel) {
        return (std::clamp)(value, mins[channel], maxes[channel]);
    };
    pvt::PaletteConfig palette;
    palette.enabled = true;
    palette.name = tr("Random %1").arg(selected_seed).toStdString();
    palette.colors.reserve(static_cast<std::size_t>(count->value()));
    for (int index = 0; index < count->value(); ++index) {
        double red = ranged(mins[0], maxes[0]);
        double green = ranged(mins[1], maxes[1]);
        double blue = ranged(mins[2], maxes[2]);
        double alpha = ranged(mins[3], maxes[3]);
        if (selected_rule >= 0 && selected_rule <= 3) {
            double hue = base_hue;
            double saturation = ranged(0.45, 0.95);
            double value = ranged(0.25, 1.0);
            if (selected_rule == 0) {
                const int accent_count = (std::max)(1, count->value() / 5);
                if (index >= count->value() - accent_count) {
                    hue = std::fmod(base_hue + 0.5, 1.0);
                }
                value = 0.18 + 0.78 * (static_cast<double>(index + 1)
                                      / static_cast<double>(count->value()));
            } else if (selected_rule == 1) {
                hue = std::fmod(base_hue + (index % 2 == 0 ? 0.0 : 0.5), 1.0);
            } else if (selected_rule == 2) {
                hue = std::fmod(base_hue - 1.0 / 12.0
                                    + (1.0 / 6.0) * unit(random) + 1.0,
                                1.0);
            } else if (selected_rule == 3) {
                value = index % 2 == 0 ? ranged(0.12, 0.38)
                                       : ranged(0.72, 1.0);
                hue = unit(random);
            }
            const QColor related = QColor::fromHsvF(
                static_cast<float>(hue), static_cast<float>(saturation),
                static_cast<float>(value));
            red = clamp_channel(related.redF(), 0);
            green = clamp_channel(related.greenF(), 1);
            blue = clamp_channel(related.blueF(), 2);
        }
        if (selected_rule == 4) {
            alpha = count->value() == 1
                        ? mins[3]
                        : mins[3] + (maxes[3] - mins[3])
                                      * static_cast<double>(index)
                                      / static_cast<double>(count->value() - 1);
        }
        pvt::PaletteColor color;
        color.red = red;
        color.green = green;
        color.blue = blue;
        color.alpha = alpha;
        palette.colors.push_back(std::move(color));
    }

    auto before = captureActiveState();
    config_.palette = std::move(palette);
    syncActiveRender();
    loadGlobalEditors();
    schedulePreview();
    recordActiveStateChange(tr("Generate random palette"), std::move(before));
    status_->setText(tr("Generated %1 colors with seed %2%3.")
                         .arg(count->value())
                         .arg(selected_seed)
                         .arg(selected_rule >= 0
                                  ? tr(" and one selected relationship rule")
                                  : QString()));
}

void MainWindow::addPaletteColor() {
    if (config_.palette.colors.size() >= pvt::kMaximumPaletteColors) {
        QMessageBox::warning(
            this, tr("Palette limit"),
            tr("The signed-int UI/API palette-color index is exhausted."));
        return;
    }
    const QColor initial = config_.palette.colors.empty()
                               ? QColor(Qt::white)
                               : palette_display_color(
                                     config_.palette.colors.back());
    const QColor chosen = QColorDialog::getColor(
        initial, this, tr("Add palette color"), QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    bool name_accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Name palette color"),
        tr("Optional entry name (preserved by GPL and KPL)"),
        QLineEdit::Normal, QString{}, &name_accepted);
    if (!name_accepted) return;
    if (!valid_text(name, TextRule::Name)) {
        QMessageBox::warning(this, tr("Invalid color name"),
                             tr("Use UTF-8 text without control characters."));
        return;
    }
    auto before = captureActiveState();
    pvt::PaletteColor added;
    added.red = chosen.redF();
    added.green = chosen.greenF();
    added.blue = chosen.blueF();
    added.alpha = chosen.alphaF();
    added.name = name.toUtf8().toStdString();
    added.encoding = pvt::PaletteColorEncoding::Srgb;
    config_.palette.colors.push_back(std::move(added));
    syncActiveRender();
    refreshPaletteEditor();
    palette_colors_->setCurrentRow(
        static_cast<int>(config_.palette.colors.size()) - 1);
    schedulePreview();
    recordActiveStateChange(tr("Add palette color"), std::move(before));
}

void MainWindow::editSelectedPaletteColor() {
    const int row = palette_colors_ != nullptr ? palette_colors_->currentRow() : -1;
    if (row < 0 || static_cast<std::size_t>(row) >= config_.palette.colors.size()) {
        return;
    }
    const auto& current = config_.palette.colors[static_cast<std::size_t>(row)];
    const bool converts_linear =
        current.encoding == pvt::PaletteColorEncoding::Linear;
    if (converts_linear
        && QMessageBox::question(
               this, tr("Convert linear/HDR color?"),
               tr("The standard color picker edits display sRGB. Continuing converts this one entry to sRGB and clips values outside the displayable range. The other imported values remain unchanged."),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    const QColor chosen = QColorDialog::getColor(
        palette_display_color(current),
        this, tr("Edit palette color"), QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    bool name_accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Name palette color"),
        tr("Optional entry name (preserved by GPL and KPL)"),
        QLineEdit::Normal, QString::fromStdString(current.name),
        &name_accepted);
    if (!name_accepted) return;
    if (!valid_text(name, TextRule::Name)) {
        QMessageBox::warning(this, tr("Invalid color name"),
                             tr("Use UTF-8 text without control characters."));
        return;
    }
    auto before = captureActiveState();
    pvt::PaletteColor edited;
    edited.red = chosen.redF();
    edited.green = chosen.greenF();
    edited.blue = chosen.blueF();
    edited.alpha = chosen.alphaF();
    edited.name = name.toUtf8().toStdString();
    edited.encoding = pvt::PaletteColorEncoding::Srgb;
    config_.palette.colors[static_cast<std::size_t>(row)] = std::move(edited);
    syncActiveRender();
    refreshPaletteEditor();
    palette_colors_->setCurrentRow(row);
    schedulePreview();
    recordActiveStateChange(tr("Edit palette color"), std::move(before));
    if (converts_linear) {
        status_->setText(tr("Converted the edited entry to display sRGB."));
    }
}

void MainWindow::removeSelectedPaletteColor() {
    const int row = palette_colors_ != nullptr ? palette_colors_->currentRow() : -1;
    if (row < 0 || static_cast<std::size_t>(row) >= config_.palette.colors.size()) {
        return;
    }
    auto before = captureActiveState();
    config_.palette.colors.erase(
        config_.palette.colors.begin() + static_cast<std::ptrdiff_t>(row));
    if (config_.palette.colors.empty()) {
        config_.palette.enabled = false;
        const QSignalBlocker blocker(palette_enabled_);
        palette_enabled_->setChecked(false);
        status_->setText(tr("The empty palette was disabled."));
    }
    syncActiveRender();
    refreshPaletteEditor();
    schedulePreview();
    recordActiveStateChange(tr("Remove palette color"), std::move(before));
}

void MainWindow::applyWaveEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    const auto index = selectedWaveIndex();
    if (!index) {
        return;
    }
    auto before = captureActiveState();
    auto& wave = config_.waves[*index];
    if (changed_editor == wave_enabled_) {
        wave.enabled = wave_enabled_->isChecked();
    } else if (changed_editor == wave_sync_) {
        wave.synchronized = wave_sync_->isChecked();
        wave_audio_response_->setEnabled(
            wave.synchronized
            && effective_active_clock_is_music(config_, active_layer_uuid_));
    } else if (changed_editor == wave_audio_response_) {
        wave.audio_response = static_cast<pvt::AudioResponseMode>(
            wave_audio_response_->currentData().toInt());
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
    syncActiveRender();
    updateWaveListItem(*index);
    updateWaveOutputState();
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("wave:%1:%2:%3")
                                  .arg(QString::fromStdString(active_layer_uuid_))
                                  .arg(wave.id)
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(tr("Edit wave"), std::move(before), key);
}

void MainWindow::applySwingEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    if (changed_editor == swings_group_) {
        auto before = captureActiveState();
        config_.swings_enabled = swings_group_->isChecked();
        syncActiveRender();
        preview_->setConfiguration(config_);
        schedulePreview();
        recordActiveStateChange(tr("Toggle swing block"), std::move(before));
        return;
    }
    const auto index = selectedSwingIndex();
    if (!index) {
        return;
    }
    auto before = captureActiveState();
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
    } else if (changed_editor == swing_center_x_) {
        swing.center_x = swing_center_x_->value();
    } else if (changed_editor == swing_center_y_) {
        swing.center_y = swing_center_y_->value();
    } else if (changed_editor == swing_radius_) {
        swing.radius = swing_radius_->value();
    } else {
        return;
    }
    syncActiveRender();
    updateSwingListItem(*index);
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("swing:%1:%2:%3")
                                  .arg(QString::fromStdString(active_layer_uuid_))
                                  .arg(swing.id)
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(tr("Edit swing"), std::move(before), key);
}

const pvt::LiveClockInputConfig* MainWindow::standardMicRoute(
    bool layerTarget) const {
    return effective_audio_clock_route(
        config_.live,
        layerTarget ? pvt::LiveClockTarget::Layer
                    : pvt::LiveClockTarget::Project,
        layerTarget ? active_layer_uuid_ : std::string{});
}

std::optional<std::string> MainWindow::ensureStandardMicRoute(
    bool layerTarget, QString* error) {
    if (error != nullptr) error->clear();
    if (layerTarget && activeLayer() == nullptr) {
        if (error != nullptr) {
            *error = tr("Select an editable layer before assigning its microphone clock.");
        }
        return std::nullopt;
    }
    pvt::LiveConfig next = config_.live;
    const pvt::LiveClockTarget target = layerTarget
        ? pvt::LiveClockTarget::Layer : pvt::LiveClockTarget::Project;
    const std::string layer_uuid = layerTarget ? active_layer_uuid_
                                               : std::string{};
    const bool first_mic_route = std::none_of(
        next.clock_inputs.begin(), next.clock_inputs.end(),
        [](const pvt::LiveClockInputConfig& route) {
            return route.enabled
                && route.source == pvt::LiveClockInputSource::AudioStream;
        });
    const auto usable_audio_endpoint = [&next](const std::string& uuid) {
        const auto endpoint = std::find_if(
            next.endpoints.begin(), next.endpoints.end(),
            [&uuid](const pvt::LiveEndpointConfig& item) {
                return item.uuid == uuid;
            });
        return endpoint != next.endpoints.end()
            && endpoint->protocol == pvt::LiveEndpointProtocol::Audio
            && (endpoint->direction == pvt::LiveEndpointDirection::Input
                || endpoint->direction
                       == pvt::LiveEndpointDirection::Bidirectional);
    };

    // The desktop runtime currently owns one capture engine. Standard Mic
    // routes therefore share one logical role instead of pretending that two
    // independently bound microphones can be sampled simultaneously.
    std::string role_uuid;
    for (const auto& route : next.clock_inputs) {
        if (route.enabled
            && route.source == pvt::LiveClockInputSource::AudioStream
            && usable_audio_endpoint(route.endpoint_uuid)) {
            role_uuid = route.endpoint_uuid;
            break;
        }
    }
    if (role_uuid.empty()) {
        for (const auto& route : next.clock_inputs) {
            if (clock_route_targets(route, target, layer_uuid)
                && route.source == pvt::LiveClockInputSource::AudioStream
                && usable_audio_endpoint(route.endpoint_uuid)) {
                role_uuid = route.endpoint_uuid;
                break;
            }
        }
    }
    if (role_uuid.empty()) {
        const auto endpoint = std::find_if(
            next.endpoints.begin(), next.endpoints.end(),
            [](const pvt::LiveEndpointConfig& item) {
                return item.protocol == pvt::LiveEndpointProtocol::Audio
                    && (item.direction == pvt::LiveEndpointDirection::Input
                        || item.direction
                               == pvt::LiveEndpointDirection::Bidirectional);
            });
        if (endpoint != next.endpoints.end()) role_uuid = endpoint->uuid;
    }
    if (role_uuid.empty()) {
        pvt::LiveEndpointConfig endpoint;
        endpoint.uuid = pvt::generate_uuid();
        endpoint.name = "Desktop microphone";
        endpoint.protocol = pvt::LiveEndpointProtocol::Audio;
        endpoint.direction = pvt::LiveEndpointDirection::Input;
        role_uuid = endpoint.uuid;
        next.endpoints.push_back(std::move(endpoint));
    }

    for (auto& route : next.clock_inputs) {
        if (route.enabled && clock_route_targets(route, target, layer_uuid)) {
            route.enabled = false;
        }
    }
    auto route = std::find_if(
        next.clock_inputs.begin(), next.clock_inputs.end(),
        [target, &layer_uuid](const pvt::LiveClockInputConfig& item) {
            return item.source == pvt::LiveClockInputSource::AudioStream
                && clock_route_targets(item, target, layer_uuid);
        });
    if (route == next.clock_inputs.end()) {
        pvt::LiveClockInputConfig created;
        created.target = target;
        created.layer_uuid = layer_uuid;
        created.source = pvt::LiveClockInputSource::AudioStream;
        next.clock_inputs.push_back(std::move(created));
        route = std::prev(next.clock_inputs.end());
    }
    route->enabled = true;
    route->target = target;
    route->layer_uuid = layer_uuid;
    route->source = pvt::LiveClockInputSource::AudioStream;
    route->endpoint_uuid = role_uuid;
    route->audio_channel = 0;
    next.enabled = true;

    const pvt::ValidationResult live_validation = pvt::validate(next);
    if (!live_validation.ok) {
        if (error != nullptr) {
            *error = QString::fromStdString(live_validation.message);
        }
        return std::nullopt;
    }
    config_.live = std::move(next);
    if (first_mic_route) {
        if (layerTarget) {
            config_.audio_reactive_override_enabled = true;
            config_.audio_reactive.enabled = true;
        } else {
            config_.audio_reactive_defaults.enabled = true;
        }
    }
    return role_uuid;
}

void MainWindow::removeStandardMicRoute(bool layerTarget) {
    const pvt::LiveClockTarget target = layerTarget
        ? pvt::LiveClockTarget::Layer : pvt::LiveClockTarget::Project;
    const std::string layer_uuid = layerTarget ? active_layer_uuid_
                                               : std::string{};
    for (auto& route : config_.live.clock_inputs) {
        if (route.enabled
            && route.source == pvt::LiveClockInputSource::AudioStream
            && clock_route_targets(route, target, layer_uuid)) {
            // Retain the disabled advanced record (stream, holdover, channel)
            // so selecting a deterministic clock does not destroy setup work.
            route.enabled = false;
        }
    }
}

void MainWindow::refreshStandardMicControls() {
    if (live_workspace_ == nullptr || project_mic_device_ == nullptr
        || layer_mic_device_ == nullptr) {
        return;
    }
    const auto choices = live_workspace_->availableAudioInputs();
    const auto populate = [this, &choices](
                              bool layerTarget, QComboBox* combo,
                              QPushButton* setup, QLabel* status) {
        const auto* route = standardMicRoute(layerTarget);
        const auto* shared_route = route != nullptr
            ? route : first_enabled_audio_clock_route(config_.live);
        QString binding;
        QString binding_label;
        bool available = true;
        if (shared_route != nullptr) {
            binding = live_workspace_->audioInputBinding(
                shared_route->endpoint_uuid);
            binding_label = live_workspace_->audioInputBindingLabel(
                shared_route->endpoint_uuid);
            available = live_workspace_->audioInputBindingAvailable(
                shared_route->endpoint_uuid);
        } else {
            QSettings settings;
            binding = settings.value(
                QStringLiteral("live/standardMicPendingDevice")).toString();
            binding_label = settings.value(
                QStringLiteral("live/standardMicPendingDeviceName"),
                tr("System default")).toString();
        }
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto& choice : choices) {
            combo->addItem(choice.label, choice.id);
        }
        int selected = combo->findData(binding);
        if (!binding.isEmpty() && selected < 0) {
            combo->insertItem(1, binding_label.startsWith(tr("Unavailable"))
                                     ? binding_label
                                     : tr("Unavailable · %1").arg(binding_label),
                              binding);
            selected = 1;
            available = false;
        }
        combo->setCurrentIndex(selected >= 0 ? selected : 0);
        setup->setEnabled(route != nullptr);
        if (route == nullptr && shared_route != nullptr) {
            status->setText(tr(
                "All standard Mic clocks share this input. Changing it also changes the existing Mic clock; choose Mic (Live)… above to add this scope."));
            status->setStyleSheet(available ? QString{}
                                            : QStringLiteral("color: #b26a00;"));
        } else if (route == nullptr) {
            status->setText(tr(
                "Choose Mic (Live)… above to use this input; System default works immediately."));
            status->setStyleSheet(QString{});
        } else if (!available) {
            status->setText(tr(
                "Saved input unavailable — LIVE will not silently switch to System default."));
            status->setStyleSheet(QStringLiteral("color: #b26a00;"));
        } else {
            status->setText(tr(
                "LIVE beat clock · offline fallback remains %1.")
                    .arg(QString::fromUtf8(pvt::clock_mode_name(
                        layerTarget ? config_.layer_clock.clock.mode
                                    : config_.clock.mode))));
            status->setStyleSheet(QString{});
        }
    };
    populate(false, project_mic_device_, project_mic_setup_,
             project_mic_status_);
    populate(true, layer_mic_device_, layer_mic_setup_, layer_mic_status_);
}

void MainWindow::applyStandardMicDeviceBinding(bool layerTarget) {
    QComboBox* combo = layerTarget ? layer_mic_device_
                                   : project_mic_device_;
    if (combo == nullptr) return;
    const QString runtime_id = combo->currentData().toString();
    QString label = combo->currentText();
    const QString default_suffix = tr(" · default");
    if (label.endsWith(default_suffix)) label.chop(default_suffix.size());
    const auto* route = standardMicRoute(layerTarget);
    const auto* shared_route = route != nullptr
        ? route : first_enabled_audio_clock_route(config_.live);
    if (shared_route != nullptr) {
        live_workspace_->setAudioInputBinding(shared_route->endpoint_uuid,
                                              runtime_id, label);
    } else {
        QSettings settings;
        settings.setValue(QStringLiteral("live/standardMicPendingDevice"),
                          runtime_id);
        settings.setValue(
            QStringLiteral("live/standardMicPendingDeviceName"), label);
    }
    refreshStandardMicControls();
}

void MainWindow::revealStandardMicSetup(bool layerTarget) {
    const auto* route = standardMicRoute(layerTarget);
    if (route == nullptr || live_workspace_ == nullptr) return;
    if (export_active_
        || (export_watcher_ != nullptr && export_watcher_->isRunning())
        || project_io_active_ || music_analysis_active_) {
        if (status_ != nullptr) {
            status_->setText(tr(
                "Finish the active export, project operation, or music analysis before starting microphone LIVE controls."));
        }
        return;
    }
    const std::string role_uuid = route->endpoint_uuid;
    openLiveMode();
    if (live_workspace_->isLiveActive()) {
        live_workspace_->revealAudioInputSetup(role_uuid);
    }
}

void MainWindow::applyClockEditor(const QObject* changed_editor) {
    if (populating_) return;

    auto before = captureActiveState();
    const bool layer_editor =
        changed_editor == layer_clock_group_
        || changed_editor == layer_clock_mix_enabled_
        || changed_editor == layer_clock_mix_mode_
        || changed_editor == layer_clock_scale_
        || changed_editor == layer_clock_mode_
        || changed_editor == layer_clock_interpolation_
        || changed_editor == layer_clock_fit_
        || changed_editor == layer_clock_frame_interval_
        || changed_editor == layer_clock_time_interval_ms_
        || changed_editor == layer_meter_expression_
        || changed_editor == layer_meter_bpm_
        || changed_editor == layer_meter_tempo_note_
        || changed_editor == layer_clock_reverse_
        || changed_editor == layer_clock_phase_offset_
        || changed_editor == layer_music_tempo_mode_
        || changed_editor == layer_music_beat_offset_ms_
        || changed_editor == layer_music_data_only_
        || changed_editor == layer_music_frequency_stream_;
    if (layer_editor) {
        auto& local = config_.layer_clock;
        auto& clock = local.clock;
        if (changed_editor == layer_clock_group_) {
            local.enabled = layer_clock_group_->isChecked();
        } else if (changed_editor == layer_clock_mix_enabled_) {
            local.mix_enabled = layer_clock_mix_enabled_->isChecked();
        } else if (changed_editor == layer_clock_mix_mode_) {
            local.mix = static_cast<pvt::LayerClockMixMode>(
                layer_clock_mix_mode_->currentData().toInt());
        } else if (changed_editor == layer_clock_scale_) {
            local.scale = static_cast<pvt::LayerClockScale>(
                layer_clock_scale_->currentData().toInt());
        } else if (changed_editor == layer_clock_mode_) {
            const int requested_value = layer_clock_mode_->currentData().toInt();
            if (requested_value == kMicLiveClockSentinel) {
                if (export_active_
                    || (export_watcher_ != nullptr
                        && export_watcher_->isRunning())
                    || project_io_active_ || music_analysis_active_) {
                    const QSignalBlocker blocker(layer_clock_mode_);
                    select_enum(layer_clock_mode_, clock.mode);
                    status_->setText(tr(
                        "Finish the active export or project operation before enabling a microphone clock."));
                    return;
                }
                const QString device_id = layer_mic_device_->currentData()
                                              .toString();
                const QString device_label = layer_mic_device_->currentText();
                QString route_error;
                const auto role = ensureStandardMicRoute(true, &route_error);
                if (!role.has_value()) {
                    const QSignalBlocker blocker(layer_clock_mode_);
                    select_enum(layer_clock_mode_, clock.mode);
                    status_->setText(tr("Layer Mic clock was not enabled: %1")
                                         .arg(route_error));
                    return;
                }
                syncActiveRender();
                syncProjectGlobals();
                updateSynchronizationState();
                preview_->setConfiguration(config_);
                schedulePreview();
                recordActiveStateChange(
                    tr("Use microphone for active-layer clock"),
                    std::move(before));
                if (live_workspace_ != nullptr) {
                    live_workspace_->setProjectLiveConfig(
                        project_.canvas.live);
                    live_workspace_->refreshProjectSnapshot();
                    // The visible choice always wins. When another standard
                    // Mic route already exists both controls explicitly show
                    // the same shared role, so this is deterministic rather
                    // than silently ignoring the new scope's selection.
                    live_workspace_->setAudioInputBinding(
                        *role, device_id, device_label);
                }
                refreshStandardMicControls();
                revealStandardMicSetup(true);
                return;
            }
            const auto requested = static_cast<pvt::ClockMode>(requested_value);
            if (requested == pvt::ClockMode::Music
                && (clock.music.source_sha256.empty()
                    || clock.music.beat_times_seconds.empty())) {
                {
                    const QSignalBlocker blocker(layer_clock_mode_);
                    if (standardMicRoute(true) != nullptr) {
                        layer_clock_mode_->setCurrentIndex(std::max(
                            0, layer_clock_mode_->findData(
                                   kMicLiveClockSentinel)));
                    } else {
                        select_enum(layer_clock_mode_, clock.mode);
                    }
                }
                chooseLayerMusicSource();
                return;
            }
            removeStandardMicRoute(true);
            clock.mode = requested;
        } else if (changed_editor == layer_clock_interpolation_) {
            clock.interpolation = static_cast<pvt::ClockInterpolation>(
                layer_clock_interpolation_->currentData().toInt());
        } else if (changed_editor == layer_clock_fit_) {
            clock.fit = static_cast<pvt::ClockFit>(
                layer_clock_fit_->currentData().toInt());
        } else if (changed_editor == layer_clock_frame_interval_) {
            clock.frame_interval = layer_clock_frame_interval_->value();
        } else if (changed_editor == layer_clock_time_interval_ms_) {
            clock.time_interval_microseconds = static_cast<std::int64_t>(
                std::llround(layer_clock_time_interval_ms_->value() * 1000.0));
        } else if (changed_editor == layer_meter_expression_) {
            std::string description;
            std::string meter_error;
            const std::string expression =
                layer_meter_expression_->text().toStdString();
            if (!pvt::describe_meter(expression, description, &meter_error)) {
                const QSignalBlocker blocker(layer_meter_expression_);
                layer_meter_expression_->setText(
                    QString::fromStdString(clock.meter.expression));
                layer_meter_summary_->setText(QString::fromStdString(meter_error));
                layer_meter_summary_->setStyleSheet(
                    QStringLiteral("color: #d32f2f;"));
                status_->setText(tr("The layer meter expression was not changed."));
                return;
            }
            clock.meter.expression = expression;
        } else if (changed_editor == layer_meter_bpm_) {
            clock.meter.bpm = layer_meter_bpm_->value();
        } else if (changed_editor == layer_meter_tempo_note_) {
            clock.meter.tempo_note_denominator =
                layer_meter_tempo_note_->value();
        } else if (changed_editor == layer_clock_reverse_) {
            clock.reverse = layer_clock_reverse_->isChecked();
        } else if (changed_editor == layer_clock_phase_offset_) {
            clock.phase_offset_degrees = layer_clock_phase_offset_->value();
        } else if (changed_editor == layer_music_tempo_mode_) {
            clock.music_tempo = static_cast<pvt::MusicTempoMode>(
                layer_music_tempo_mode_->currentData().toInt());
        } else if (changed_editor == layer_music_beat_offset_ms_) {
            clock.beat_offset_microseconds = static_cast<std::int64_t>(
                std::llround(layer_music_beat_offset_ms_->value() * 1000.0));
        } else if (changed_editor == layer_music_data_only_) {
            clock.data_only = layer_music_data_only_->isChecked();
        } else if (changed_editor == layer_music_frequency_stream_) {
            clock.frequency_stream_uuid =
                layer_music_frequency_stream_->currentData().toString().toStdString();
        }
        syncActiveRender();
        if (changed_editor == layer_clock_mode_) syncProjectGlobals();
        updateSynchronizationState();
        preview_->setConfiguration(config_);
        schedulePreview();
        if ((changed_editor == layer_clock_group_
             || changed_editor == layer_clock_scale_
             || changed_editor == layer_clock_mode_
             || changed_editor == layer_music_data_only_)
            && playback_timer_ != nullptr && playback_timer_->isActive()) {
            startProjectAudioPlayback();
        }
        recordActiveStateChange(
            tr("Edit active-layer clock"), std::move(before),
            editor_change_is_continuous(changed_editor)
                ? QStringLiteral("layer-clock:%1:%2")
                      .arg(QString::fromStdString(active_layer_uuid_))
                      .arg(reinterpret_cast<quintptr>(changed_editor))
                : QString{});
        if (changed_editor == layer_clock_mode_
            && live_workspace_ != nullptr) {
            live_workspace_->setProjectLiveConfig(project_.canvas.live);
            live_workspace_->refreshProjectSnapshot();
            refreshStandardMicControls();
        }
        return;
    }
    if (changed_editor == clock_mode_) {
        const int requested_value = clock_mode_->currentData().toInt();
        if (requested_value == kMicLiveClockSentinel) {
            if (export_active_
                || (export_watcher_ != nullptr
                    && export_watcher_->isRunning())
                || project_io_active_ || music_analysis_active_) {
                const QSignalBlocker blocker(clock_mode_);
                select_enum(clock_mode_, config_.clock.mode);
                status_->setText(tr(
                    "Finish the active export or project operation before enabling a microphone clock."));
                return;
            }
            const QString device_id = project_mic_device_->currentData()
                                          .toString();
            const QString device_label = project_mic_device_->currentText();
            QString route_error;
            const auto role = ensureStandardMicRoute(false, &route_error);
            if (!role.has_value()) {
                const QSignalBlocker blocker(clock_mode_);
                select_enum(clock_mode_, config_.clock.mode);
                status_->setText(tr("Project Mic clock was not enabled: %1")
                                     .arg(route_error));
                return;
            }
            syncActiveRender();
            syncProjectGlobals();
            updateSynchronizationState();
            preview_->setConfiguration(config_);
            schedulePreview();
            recordActiveStateChange(
                tr("Use microphone for project clock"), std::move(before));
            if (live_workspace_ != nullptr) {
                live_workspace_->setProjectLiveConfig(project_.canvas.live);
                live_workspace_->refreshProjectSnapshot();
                live_workspace_->setAudioInputBinding(
                    *role, device_id, device_label);
            }
            refreshStandardMicControls();
            revealStandardMicSetup(false);
            return;
        }
        const auto requested = static_cast<pvt::ClockMode>(requested_value);
        if (requested == pvt::ClockMode::Music && !musicRenderReady()) {
            {
                const QSignalBlocker blocker(clock_mode_);
                if (standardMicRoute(false) != nullptr) {
                    clock_mode_->setCurrentIndex(std::max(
                        0, clock_mode_->findData(kMicLiveClockSentinel)));
                } else {
                    select_enum(clock_mode_, config_.clock.mode);
                }
            }
            chooseMusicSource();
            return;
        }
        removeStandardMicRoute(false);
        config_.clock.mode = requested;
    } else if (changed_editor == clock_interpolation_) {
        config_.clock.interpolation = static_cast<pvt::ClockInterpolation>(
            clock_interpolation_->currentData().toInt());
    } else if (changed_editor == clock_fit_) {
        config_.clock.fit = static_cast<pvt::ClockFit>(
            clock_fit_->currentData().toInt());
    } else if (changed_editor == clock_frame_interval_) {
        config_.clock.frame_interval = clock_frame_interval_->value();
    } else if (changed_editor == clock_time_interval_ms_) {
        config_.clock.time_interval_microseconds = static_cast<std::int64_t>(
            std::llround(clock_time_interval_ms_->value() * 1000.0));
    } else if (changed_editor == meter_expression_) {
        std::string description;
        std::string meter_error;
        const std::string expression = meter_expression_->text().toStdString();
        if (!pvt::describe_meter(expression, description, &meter_error)) {
            const QSignalBlocker blocker(meter_expression_);
            meter_expression_->setText(
                QString::fromStdString(config_.clock.meter.expression));
            meter_summary_->setText(QString::fromStdString(meter_error));
            meter_summary_->setStyleSheet(QStringLiteral("color: #d32f2f;"));
            status_->setText(tr("The meter expression was not changed."));
            return;
        }
        config_.clock.meter.expression = expression;
    } else if (changed_editor == meter_bpm_) {
        config_.clock.meter.bpm = meter_bpm_->value();
    } else if (changed_editor == meter_tempo_note_) {
        config_.clock.meter.tempo_note_denominator = meter_tempo_note_->value();
    } else if (changed_editor == clock_reverse_) {
        config_.clock.reverse = clock_reverse_->isChecked();
    } else if (changed_editor == clock_phase_offset_) {
        config_.clock.phase_offset_degrees = clock_phase_offset_->value();
    } else if (changed_editor == music_tempo_mode_) {
        config_.clock.music_tempo = static_cast<pvt::MusicTempoMode>(
            music_tempo_mode_->currentData().toInt());
    } else if (changed_editor == music_beat_offset_ms_) {
        config_.clock.beat_offset_microseconds = static_cast<std::int64_t>(
            std::llround(music_beat_offset_ms_->value() * 1000.0));
    } else if (changed_editor == music_data_only_) {
        config_.clock.data_only = music_data_only_->isChecked();
    } else if (changed_editor == music_frequency_stream_) {
        config_.clock.frequency_stream_uuid =
            music_frequency_stream_->currentData().toString().toStdString();
    } else {
        return;
    }

    syncProjectGlobals();
    updateSynchronizationState();
    updateTimelineState();
    if ((changed_editor == clock_mode_ || changed_editor == music_data_only_)
        && playback_timer_ != nullptr
        && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("clock:%1")
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(tr("Edit synchronized clock"), std::move(before), key);
    if (changed_editor == clock_mode_ && live_workspace_ != nullptr) {
        live_workspace_->setProjectLiveConfig(project_.canvas.live);
        live_workspace_->refreshProjectSnapshot();
        refreshStandardMicControls();
    }
}

void MainWindow::applyAudioReactiveEditor(const QObject* changed_editor) {
    if (populating_) return;
    auto before = captureActiveState();
    const bool project_scope =
        changed_editor == project_audio_response_group_
        || changed_editor == project_audio_sync_only_
        || changed_editor == project_audio_waves_enabled_
        || changed_editor == project_audio_wave_source_
        || changed_editor == project_audio_wave_amount_
        || changed_editor == project_audio_effects_enabled_
        || changed_editor == project_audio_effect_source_
        || changed_editor == project_audio_effect_amount_
        || changed_editor == project_audio_color_enabled_
        || changed_editor == project_audio_color_source_
        || changed_editor == project_audio_color_amount_;
    auto& audio = project_scope ? config_.audio_reactive_defaults
                                : config_.audio_reactive;
    if (changed_editor == project_audio_response_group_) {
        audio.enabled = project_audio_response_group_->isChecked();
    } else if (changed_editor == project_audio_sync_only_) {
        audio.synchronized_only = project_audio_sync_only_->isChecked();
    } else if (changed_editor == project_audio_waves_enabled_) {
        audio.waves_enabled = project_audio_waves_enabled_->isChecked();
    } else if (changed_editor == project_audio_wave_source_) {
        audio.wave_source = static_cast<pvt::MusicFeature>(
            project_audio_wave_source_->currentData().toInt());
    } else if (changed_editor == project_audio_wave_amount_) {
        audio.wave_amount = project_audio_wave_amount_->value();
    } else if (changed_editor == project_audio_effects_enabled_) {
        audio.effects_enabled = project_audio_effects_enabled_->isChecked();
    } else if (changed_editor == project_audio_effect_source_) {
        audio.effect_source = static_cast<pvt::MusicFeature>(
            project_audio_effect_source_->currentData().toInt());
    } else if (changed_editor == project_audio_effect_amount_) {
        audio.effect_amount = project_audio_effect_amount_->value();
    } else if (changed_editor == project_audio_color_enabled_) {
        audio.color_enabled = project_audio_color_enabled_->isChecked();
    } else if (changed_editor == project_audio_color_source_) {
        audio.color_source = static_cast<pvt::MusicFeature>(
            project_audio_color_source_->currentData().toInt());
    } else if (changed_editor == project_audio_color_amount_) {
        audio.color_amount_degrees = project_audio_color_amount_->value();
    } else if (changed_editor == audio_response_group_) {
        config_.audio_reactive_override_enabled =
            audio_response_group_->isChecked();
    } else if (changed_editor == audio_response_enabled_) {
        audio.enabled = audio_response_enabled_->isChecked();
    } else if (changed_editor == audio_sync_only_) {
        audio.synchronized_only = audio_sync_only_->isChecked();
    } else if (changed_editor == audio_waves_enabled_) {
        audio.waves_enabled = audio_waves_enabled_->isChecked();
    } else if (changed_editor == audio_wave_source_) {
        audio.wave_source = static_cast<pvt::MusicFeature>(
            audio_wave_source_->currentData().toInt());
    } else if (changed_editor == audio_wave_amount_) {
        audio.wave_amount = audio_wave_amount_->value();
    } else if (changed_editor == audio_effects_enabled_) {
        audio.effects_enabled = audio_effects_enabled_->isChecked();
    } else if (changed_editor == audio_effect_source_) {
        audio.effect_source = static_cast<pvt::MusicFeature>(
            audio_effect_source_->currentData().toInt());
    } else if (changed_editor == audio_effect_amount_) {
        audio.effect_amount = audio_effect_amount_->value();
    } else if (changed_editor == audio_color_enabled_) {
        audio.color_enabled = audio_color_enabled_->isChecked();
    } else if (changed_editor == audio_color_source_) {
        audio.color_source = static_cast<pvt::MusicFeature>(
            audio_color_source_->currentData().toInt());
    } else if (changed_editor == audio_color_amount_) {
        audio.color_amount_degrees = audio_color_amount_->value();
    } else {
        return;
    }
    if (project_scope) {
        syncProjectGlobals();
    } else {
        syncActiveRender();
    }
    updateSynchronizationState();
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("audio-response:%1:%2")
                                  .arg(project_scope
                                           ? QStringLiteral("project")
                                           : QString::fromStdString(
                                                 active_layer_uuid_))
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(
        project_scope ? tr("Edit project audio response")
                      : tr("Edit layer audio response"),
        std::move(before), key);
}

void MainWindow::applyEffectEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    const auto index = selectedEffectIndex();
    if (!index) {
        return;
    }
    auto before = captureActiveState();
    auto& effect = config_.effects[*index];
    if (changed_editor == effect_enabled_) {
        effect.enabled = effect_enabled_->isChecked();
    } else if (changed_editor == effect_sync_) {
        effect.synchronized = effect_sync_->isChecked();
        effect_audio_response_->setEnabled(
            effect.synchronized
            && effective_active_clock_is_music(config_, active_layer_uuid_));
    } else if (changed_editor == effect_audio_response_) {
        effect.audio_response = static_cast<pvt::AudioResponseMode>(
            effect_audio_response_->currentData().toInt());
    } else if (changed_editor == effect_space_) {
        effect.space = static_cast<pvt::EffectSpace>(
            effect_space_->currentData().toInt());
    } else if (changed_editor == effect_type_) {
        const auto old_type = effect.type;
        const auto new_type =
            static_cast<pvt::EffectType>(effect_type_->currentData().toInt());
        if (new_type != old_type) {
            const auto id = effect.id;
            const bool enabled = effect.enabled;
            const bool synchronized = effect.synchronized;
            const auto audio_response = effect.audio_response;
            const auto space = effect.space;
            const int cycles = effect.cycles_per_loop;
            const double phase = effect.phase_degrees;
            const double center_x = effect.center_x;
            const double center_y = effect.center_y;
            const double area_radius = effect.area_radius;
            const std::string old_default_name = pvt::effect_type_name(old_type);
            const bool used_default_name = effect.name == old_default_name;
            const std::string custom_name = effect.name;
            effect = pvt::default_effect(new_type);
            effect.id = id;
            effect.enabled = enabled;
            effect.synchronized = synchronized;
            effect.audio_response = audio_response;
            effect.space = space;
            effect.cycles_per_loop = cycles;
            effect.phase_degrees = phase;
            effect.center_x = center_x;
            effect.center_y = center_y;
            effect.area_radius = area_radius;
            if (!used_default_name) {
                effect.name = custom_name;
            }
            loadSelectedEffect();
        }
    } else if (changed_editor == effect_blur_type_) {
        effect.blur_type = static_cast<pvt::BlurType>(
            effect_blur_type_->currentData().toInt());
        if (effect.blur_type == pvt::BlurType::Gaussian
            && effect.blur_samples % 2 == 0) {
            ++effect.blur_samples;
            const QSignalBlocker blocker(effect_blur_samples_);
            effect_blur_samples_->setValue(effect.blur_samples);
        }
    } else if (changed_editor == effect_particle_shape_) {
        effect.particle_shape = static_cast<pvt::ParticleShape>(
            effect_particle_shape_->currentData().toInt());
    } else if (changed_editor == effect_particle_profile_) {
        effect.particle_profile = static_cast<pvt::ParticleRenderProfile>(
            effect_particle_profile_->currentData().toInt());
    } else if (changed_editor == effect_particle_orientation_) {
        effect.particle_orientation = static_cast<pvt::ParticleOrientation>(
            effect_particle_orientation_->currentData().toInt());
    } else if (changed_editor == effect_particle_size_scale_) {
        effect.radius_pixels = particle_radius_from_slider(
            effect_particle_size_scale_->value());
        const QSignalBlocker blocker(effect_radius_);
        effect_radius_->setValue(effect.radius_pixels);
    } else if (changed_editor == effect_particle_size_variation_) {
        effect.particle_size_variation =
            effect_particle_size_variation_->value();
    } else if (changed_editor == effect_particle_definition_) {
        effect.particle_definition = effect_particle_definition_->value();
    } else if (changed_editor == effect_particle_twinkle_) {
        effect.particle_twinkle = effect_particle_twinkle_->value();
    } else if (changed_editor == effect_particle_seed_) {
        bool ok = false;
        const qulonglong parsed = effect_particle_seed_->text().trimmed()
                                      .toULongLong(&ok, 0);
        if (!ok) {
            const QSignalBlocker blocker(effect_particle_seed_);
            effect_particle_seed_->setText(
                QString::number(static_cast<qulonglong>(effect.particle_seed)));
            statusBar()->showMessage(
                tr("Particle seed must be an unsigned 64-bit whole number."),
                5000);
            return;
        }
        effect.particle_seed = static_cast<std::uint64_t>(parsed);
    } else if (changed_editor == effect_particle_reseed_) {
        effect.particle_seed = QRandomGenerator::global()->generate64();
        if (effect.particle_seed == 0U) effect.particle_seed = 1U;
        const QSignalBlocker blocker(effect_particle_seed_);
        effect_particle_seed_->setText(
            QString::number(static_cast<qulonglong>(effect.particle_seed)));
    } else if (changed_editor == effect_particle_rotation_) {
        effect.particle_rotation_degrees = effect_particle_rotation_->value();
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
        if (effect.type == pvt::EffectType::BlockScale) {
            synchronize_block_scale_maximum_editor(
                effect_frequency_, effect.magnitude, effect.frequency);
        }
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
        if (effect.type == pvt::EffectType::ParticleField) {
            const QSignalBlocker blocker(effect_particle_size_scale_);
            effect_particle_size_scale_->setValue(
                particle_slider_from_radius(effect.radius_pixels));
        }
    } else if (changed_editor == effect_threshold_) {
        effect.threshold = effect_threshold_->value();
    } else if (changed_editor == effect_knee_) {
        effect.soft_knee = effect_knee_->value();
    } else if (changed_editor == effect_area_radius_) {
        effect.area_radius = effect_area_radius_->value();
    } else if (changed_editor == effect_blur_passes_) {
        effect.blur_passes = effect_blur_passes_->value();
    } else if (changed_editor == effect_blur_samples_) {
        int samples = effect_blur_samples_->value();
        if (effect.blur_type == pvt::BlurType::Gaussian && samples % 2 == 0) {
            ++samples;
            const QSignalBlocker blocker(effect_blur_samples_);
            effect_blur_samples_->setValue(samples);
        }
        effect.blur_samples = samples;
    } else if (changed_editor == effect_blur_minimum_) {
        effect.blur_minimum = effect_blur_minimum_->value();
        if (effect.blur_maximum < effect.blur_minimum) {
            effect.blur_maximum = effect.blur_minimum;
            const QSignalBlocker blocker(effect_blur_maximum_);
            effect_blur_maximum_->setValue(effect.blur_maximum);
        }
    } else if (changed_editor == effect_blur_maximum_) {
        effect.blur_maximum = effect_blur_maximum_->value();
        if (effect.blur_minimum > effect.blur_maximum) {
            effect.blur_minimum = effect.blur_maximum;
            const QSignalBlocker blocker(effect_blur_minimum_);
            effect_blur_minimum_->setValue(effect.blur_minimum);
        }
    } else {
        return;
    }
    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    updateEffectListItem(*index);
    updateEffectEditorVisibility();
    updateWorkflowSummaries();
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("effect:%1:%2:%3")
                                  .arg(QString::fromStdString(active_layer_uuid_))
                                  .arg(effect.id)
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(tr("Edit effect"), std::move(before), key);
}

void MainWindow::applyGlobalEditor(const QObject* changed_editor) {
    if (populating_) {
        return;
    }
    if (changed_editor == surface_obj_path_) {
        (void)setSurfaceObjSource(surface_obj_path_->text());
        return;
    }
    auto before = captureActiveState();
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
    } else if (changed_editor == displacement_enabled_
               || changed_editor == wave_displacement_enabled_) {
        config_.displacement_enabled =
            changed_editor == displacement_enabled_
                ? displacement_enabled_->isChecked()
                : wave_displacement_enabled_->isChecked();
        const QSignalBlocker modifier_blocker(displacement_enabled_);
        const QSignalBlocker wave_blocker(wave_displacement_enabled_);
        displacement_enabled_->setChecked(config_.displacement_enabled);
        wave_displacement_enabled_->setChecked(
            config_.displacement_enabled);
    } else if (changed_editor == displacement_) {
        config_.displacement = displacement_->value();
    } else if (changed_editor == lighting_enabled_
               || changed_editor == wave_lighting_enabled_) {
        config_.lighting_enabled =
            changed_editor == lighting_enabled_
                ? lighting_enabled_->isChecked()
                : wave_lighting_enabled_->isChecked();
        const QSignalBlocker modifier_blocker(lighting_enabled_);
        const QSignalBlocker wave_blocker(wave_lighting_enabled_);
        lighting_enabled_->setChecked(config_.lighting_enabled);
        wave_lighting_enabled_->setChecked(config_.lighting_enabled);
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
    } else if (changed_editor == kaleidoscope_group_) {
        config_.starting_colors.kaleidoscope.enabled =
            kaleidoscope_group_->isChecked();
    } else if (changed_editor == kaleidoscope_segments_) {
        config_.starting_colors.kaleidoscope.mirrored_segments =
            kaleidoscope_segments_->value();
    } else if (changed_editor == kaleidoscope_rotation_) {
        config_.starting_colors.kaleidoscope.rotation_degrees =
            kaleidoscope_rotation_->value();
    } else if (changed_editor == kaleidoscope_mix_) {
        config_.starting_colors.kaleidoscope.mix =
            kaleidoscope_mix_->value();
    } else if (changed_editor == domain_warp_group_) {
        config_.starting_colors.domain_warp.enabled =
            domain_warp_group_->isChecked();
    } else if (changed_editor == domain_warp_strength_) {
        config_.starting_colors.domain_warp.strength =
            domain_warp_strength_->value();
    } else if (changed_editor == domain_warp_scale_) {
        config_.starting_colors.domain_warp.scale =
            domain_warp_scale_->value();
    } else if (changed_editor == domain_warp_octaves_) {
        config_.starting_colors.domain_warp.octaves =
            domain_warp_octaves_->value();
    } else if (changed_editor == domain_warp_cycles_) {
        config_.starting_colors.domain_warp.cycles_per_loop =
            domain_warp_cycles_->value();
    } else if (changed_editor == domain_warp_seed_) {
        bool valid_seed = false;
        const quint64 seed = domain_warp_seed_->text().trimmed().toULongLong(
            &valid_seed, 10);
        if (!valid_seed) {
            const QSignalBlocker blocker(domain_warp_seed_);
            domain_warp_seed_->setText(QString::number(
                static_cast<qulonglong>(
                    config_.starting_colors.domain_warp.seed)));
            status_->setText(tr(
                "Domain-warp seed must be an unsigned 64-bit integer."));
            return;
        }
        config_.starting_colors.domain_warp.seed =
            static_cast<std::uint64_t>(seed);
    } else if (changed_editor == starting_image_enabled_) {
        if (starting_image_enabled_->isChecked()
            && config_.starting_image.path.empty()) {
            const QSignalBlocker blocker(starting_image_enabled_);
            starting_image_enabled_->setChecked(false);
            status_->setText(tr("Choose a PNG or OpenEXR image before enabling the starting image."));
            return;
        }
        config_.starting_image.enabled = starting_image_enabled_->isChecked();
    } else if (changed_editor == starting_image_fit_) {
        config_.starting_image.fit = static_cast<pvt::StartingImageFit>(
            starting_image_fit_->currentData().toInt());
    } else if (changed_editor == starting_image_palette_dither_) {
        config_.starting_image.palette_dither_enabled =
            starting_image_palette_dither_->isChecked();
        starting_image_palette_dither_method_->setEnabled(
            config_.starting_image.palette_dither_enabled);
    } else if (changed_editor == starting_image_palette_dither_method_) {
        config_.starting_image.palette_dither_method =
            static_cast<pvt::DitherMethod>(
                starting_image_palette_dither_method_->currentData().toInt());
    } else if (changed_editor == transform_flip_horizontal_) {
        config_.transform.flip_horizontal =
            transform_flip_horizontal_->isChecked();
    } else if (changed_editor == transform_flip_vertical_) {
        config_.transform.flip_vertical = transform_flip_vertical_->isChecked();
    } else if (changed_editor == transform_mirror_) {
        config_.transform.mirror = static_cast<pvt::MirrorMode>(
            transform_mirror_->currentData().toInt());
    } else if (changed_editor == motion_group_) {
        config_.motion.enabled = motion_group_->isChecked();
        if (config_.motion.enabled
            && config_.motion.path == pvt::LayerMotionPath::None) {
            config_.motion.path = pvt::LayerMotionPath::Orbit;
            const QSignalBlocker blocker(motion_path_);
            select_enum(motion_path_, config_.motion.path);
        }
    } else if (changed_editor == motion_path_) {
        config_.motion.path = static_cast<pvt::LayerMotionPath>(
            motion_path_->currentData().toInt());
    } else if (changed_editor == motion_center_x_) {
        config_.motion.center_x = motion_center_x_->value();
    } else if (changed_editor == motion_center_y_) {
        config_.motion.center_y = motion_center_y_->value();
    } else if (changed_editor == motion_travel_x_) {
        config_.motion.travel_x = motion_travel_x_->value();
    } else if (changed_editor == motion_travel_y_) {
        config_.motion.travel_y = motion_travel_y_->value();
    } else if (changed_editor == motion_cycles_x_) {
        config_.motion.cycles_x = motion_cycles_x_->value();
    } else if (changed_editor == motion_cycles_y_) {
        config_.motion.cycles_y = motion_cycles_y_->value();
    } else if (changed_editor == motion_phase_) {
        config_.motion.phase_degrees = motion_phase_->value();
    } else if (changed_editor == motion_rotations_) {
        config_.motion.rotations_per_loop = motion_rotations_->value();
    } else if (changed_editor == motion_rotation_offset_) {
        config_.motion.rotation_offset_degrees =
            motion_rotation_offset_->value();
    } else if (changed_editor == motion_scale_pulse_) {
        config_.motion.scale_pulse = motion_scale_pulse_->value();
    } else if (changed_editor == palette_enabled_) {
        if (palette_enabled_->isChecked() && config_.palette.colors.empty()) {
            config_.palette = pvt::default_palette(0U);
            const QSignalBlocker name_blocker(palette_name_);
            palette_name_->setText(QString::fromStdString(config_.palette.name));
            refreshPaletteEditor();
            status_->setText(tr("Loaded the Ember preset because a starting palette needs at least one color."));
        } else {
            config_.palette.enabled = palette_enabled_->isChecked();
            status_->setText(config_.palette.enabled
                                 ? tr("Starting palette enabled.")
                                 : tr("Starting palette disabled."));
        }
    } else if (changed_editor == palette_name_) {
        if (!palette_name_->hasAcceptableInput()) return;
        config_.palette.name = palette_name_->text().toStdString();
    } else if (changed_editor == starting_color_mode_) {
        config_.starting_colors.mode = static_cast<pvt::StartingColorMode>(
            starting_color_mode_->currentData().toInt());
    } else if (changed_editor == starting_color_include_alpha_) {
        config_.starting_colors.include_alpha =
            starting_color_include_alpha_->isChecked();
    } else if (changed_editor == starting_red_minimum_) {
        config_.starting_colors.red_minimum = starting_red_minimum_->value();
        if (config_.starting_colors.red_maximum
            < config_.starting_colors.red_minimum) {
            config_.starting_colors.red_maximum = config_.starting_colors.red_minimum;
            const QSignalBlocker blocker(starting_red_maximum_);
            starting_red_maximum_->setValue(config_.starting_colors.red_maximum);
        }
    } else if (changed_editor == starting_red_maximum_) {
        config_.starting_colors.red_maximum = starting_red_maximum_->value();
        if (config_.starting_colors.red_minimum
            > config_.starting_colors.red_maximum) {
            config_.starting_colors.red_minimum = config_.starting_colors.red_maximum;
            const QSignalBlocker blocker(starting_red_minimum_);
            starting_red_minimum_->setValue(config_.starting_colors.red_minimum);
        }
    } else if (changed_editor == starting_green_minimum_) {
        config_.starting_colors.green_minimum = starting_green_minimum_->value();
        if (config_.starting_colors.green_maximum
            < config_.starting_colors.green_minimum) {
            config_.starting_colors.green_maximum = config_.starting_colors.green_minimum;
            const QSignalBlocker blocker(starting_green_maximum_);
            starting_green_maximum_->setValue(config_.starting_colors.green_maximum);
        }
    } else if (changed_editor == starting_green_maximum_) {
        config_.starting_colors.green_maximum = starting_green_maximum_->value();
        if (config_.starting_colors.green_minimum
            > config_.starting_colors.green_maximum) {
            config_.starting_colors.green_minimum = config_.starting_colors.green_maximum;
            const QSignalBlocker blocker(starting_green_minimum_);
            starting_green_minimum_->setValue(config_.starting_colors.green_minimum);
        }
    } else if (changed_editor == starting_blue_minimum_) {
        config_.starting_colors.blue_minimum = starting_blue_minimum_->value();
        if (config_.starting_colors.blue_maximum
            < config_.starting_colors.blue_minimum) {
            config_.starting_colors.blue_maximum = config_.starting_colors.blue_minimum;
            const QSignalBlocker blocker(starting_blue_maximum_);
            starting_blue_maximum_->setValue(config_.starting_colors.blue_maximum);
        }
    } else if (changed_editor == starting_blue_maximum_) {
        config_.starting_colors.blue_maximum = starting_blue_maximum_->value();
        if (config_.starting_colors.blue_minimum
            > config_.starting_colors.blue_maximum) {
            config_.starting_colors.blue_minimum = config_.starting_colors.blue_maximum;
            const QSignalBlocker blocker(starting_blue_minimum_);
            starting_blue_minimum_->setValue(config_.starting_colors.blue_minimum);
        }
    } else if (changed_editor == starting_alpha_minimum_) {
        config_.starting_colors.alpha_minimum = starting_alpha_minimum_->value();
        if (config_.starting_colors.alpha_maximum
            < config_.starting_colors.alpha_minimum) {
            config_.starting_colors.alpha_maximum = config_.starting_colors.alpha_minimum;
            const QSignalBlocker blocker(starting_alpha_maximum_);
            starting_alpha_maximum_->setValue(config_.starting_colors.alpha_maximum);
        }
    } else if (changed_editor == starting_alpha_maximum_) {
        config_.starting_colors.alpha_maximum = starting_alpha_maximum_->value();
        if (config_.starting_colors.alpha_minimum
            > config_.starting_colors.alpha_maximum) {
            config_.starting_colors.alpha_minimum = config_.starting_colors.alpha_maximum;
            const QSignalBlocker blocker(starting_alpha_minimum_);
            starting_alpha_minimum_->setValue(config_.starting_colors.alpha_minimum);
        }
    } else if (changed_editor == surface_enabled_) {
        config_.surface.enabled = surface_enabled_->isChecked();
    } else if (changed_editor == surface_mapping_) {
        config_.surface.mapping =
            static_cast<pvt::SurfaceMapping>(surface_mapping_->currentData().toInt());
        if (config_.surface.mapping != pvt::SurfaceMapping::Plane
            && config_.surface.plane_displacement.enabled) {
            config_.surface.plane_displacement.enabled = false;
            const QSignalBlocker blocker(
                surface_plane_displacement_enabled_);
            surface_plane_displacement_enabled_->setChecked(false);
        }
    } else if (changed_editor == surface_plane_displacement_enabled_) {
        if (surface_plane_displacement_enabled_->isChecked()
            && config_.surface.plane_displacement.path.empty()) {
            const QSignalBlocker blocker(
                surface_plane_displacement_enabled_);
            surface_plane_displacement_enabled_->setChecked(false);
            status_->setText(
                tr("Choose a PNG or OpenEXR height map before enabling plane displacement."));
            return;
        }
        config_.surface.plane_displacement.enabled =
            surface_plane_displacement_enabled_->isChecked();
        if (config_.surface.plane_displacement.enabled) {
            config_.surface.enabled = true;
            config_.surface.mapping = pvt::SurfaceMapping::Plane;
            const QSignalBlocker enabled_blocker(surface_enabled_);
            const QSignalBlocker mapping_blocker(surface_mapping_);
            surface_enabled_->setChecked(true);
            select_enum(surface_mapping_, pvt::SurfaceMapping::Plane);
        }
    } else if (changed_editor == surface_plane_displacement_minimum_) {
        config_.surface.plane_displacement.minimum =
            surface_plane_displacement_minimum_->value();
        if (config_.surface.plane_displacement.maximum
            < config_.surface.plane_displacement.minimum) {
            config_.surface.plane_displacement.maximum =
                config_.surface.plane_displacement.minimum;
            const QSignalBlocker blocker(
                surface_plane_displacement_maximum_);
            surface_plane_displacement_maximum_->setValue(
                config_.surface.plane_displacement.maximum);
        }
    } else if (changed_editor == surface_plane_displacement_maximum_) {
        config_.surface.plane_displacement.maximum =
            surface_plane_displacement_maximum_->value();
        if (config_.surface.plane_displacement.minimum
            > config_.surface.plane_displacement.maximum) {
            config_.surface.plane_displacement.minimum =
                config_.surface.plane_displacement.maximum;
            const QSignalBlocker blocker(
                surface_plane_displacement_minimum_);
            surface_plane_displacement_minimum_->setValue(
                config_.surface.plane_displacement.minimum);
        }
    } else if (changed_editor == surface_plane_displacement_midpoint_) {
        config_.surface.plane_displacement.midpoint =
            surface_plane_displacement_midpoint_->value();
    } else if (changed_editor == surface_plane_displacement_ratio_) {
        config_.surface.plane_displacement.pixels_per_node =
            surface_plane_displacement_ratio_->value();
    } else if (changed_editor == surface_rotations_) {
        config_.surface.rotations_per_loop = surface_rotations_->value();
    } else if (changed_editor == surface_phase_) {
        config_.surface.phase_degrees = surface_phase_->value();
    } else if (changed_editor == surface_curvature_) {
        config_.surface.curvature = surface_curvature_->value();
    } else if (changed_editor == surface_lighting_) {
        config_.surface.lighting = surface_lighting_->value();
    } else if (changed_editor == post_invert_rgb_enabled_) {
        config_.post_process.invert_rgb_enabled =
            post_invert_rgb_enabled_->isChecked();
    } else if (changed_editor == post_invert_rgb_mix_) {
        config_.post_process.invert_rgb_mix = post_invert_rgb_mix_->value();
    } else if (changed_editor == post_invert_alpha_enabled_) {
        config_.post_process.invert_alpha_enabled =
            post_invert_alpha_enabled_->isChecked();
    } else if (changed_editor == post_invert_alpha_mix_) {
        config_.post_process.invert_alpha_mix = post_invert_alpha_mix_->value();
    } else if (changed_editor == post_antialias_enabled_) {
        config_.post_process.antialias_enabled =
            post_antialias_enabled_->isChecked();
    } else if (changed_editor == post_antialias_strength_) {
        config_.post_process.antialias_strength =
            post_antialias_strength_->value();
    } else if (changed_editor == post_antialias_threshold_) {
        config_.post_process.antialias_threshold =
            post_antialias_threshold_->value();
    } else if (changed_editor == post_antialias_passes_) {
        config_.post_process.antialias_passes =
            post_antialias_passes_->value();
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
    } else if (changed_editor == alpha_use_source_) {
        config_.alpha.use_source_alpha = alpha_use_source_->isChecked();
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
    } else if (changed_editor == png_compression_) {
        config_.output.png_compression_level = png_compression_->value();
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
    } else if (changed_editor == write_alpha_) {
        config_.output.write_alpha = write_alpha_->isChecked();
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
    syncActiveRender();
    syncProjectGlobals();
    {
        const QSignalBlocker blocker(dither_enabled_);
        dither_enabled_->setChecked(config_.output.bit_depth == 32
                                        ? false
                                        : integer_dither_preference_);
    }
    dither_enabled_->setEnabled(config_.output.bit_depth != 32);
    dither_method_->setEnabled(config_.output.bit_depth != 32
                               && config_.output.dither_enabled);
    starting_image_palette_dither_method_->setEnabled(
        config_.starting_image.palette_dither_enabled);
    updateSurfaceEditorState();
    updatePostProcessEditorState();
    updateWaveOutputState();
    png_compression_->setEnabled(config_.output.bit_depth != 32);
    if (changed_editor == post_invert_rgb_enabled_
        || changed_editor == post_invert_alpha_enabled_
        || changed_editor == post_antialias_enabled_
        || changed_editor == quantization_enabled_) {
        updateWorkflowSummaries();
    }
    if (changed_editor == frames_ || changed_editor == fps_) {
        updateTimelineState();
        updateSynchronizationState();
        if (playback_timer_ != nullptr && playback_timer_->isActive()) {
            startProjectAudioPlayback();
        }
    }
    updateExportAvailability();
    if (affects_preview) {
        preview_->setConfiguration(config_);
        schedulePreview();
    }
    const QString key = editor_change_is_continuous(changed_editor)
                            ? QStringLiteral("setting:%1:%2")
                                  .arg(QString::fromStdString(active_layer_uuid_))
                                  .arg(reinterpret_cast<quintptr>(changed_editor))
                            : QString{};
    recordActiveStateChange(tr("Edit project setting"), std::move(before), key);
}

void MainWindow::ensureAlphaForTransparency() {
    const bool project_requests_alpha = visible_stack_requires_alpha(
        project_, &config_, active_layer_uuid_);
    if (project_requests_alpha && !config_.output.write_alpha) {
        config_.output.write_alpha = true;
        const QSignalBlocker blocker(write_alpha_);
        write_alpha_->setChecked(true);
        status_->setText(
            tr("Final alpha output was enabled because the visible stack can be transparent."));
    }
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
    auto before = captureActiveState();
    const auto id = config_.waves[*index].id;
    std::swap(config_.waves[*index], config_.waves[static_cast<std::size_t>(target)]);
    syncActiveRender();
    refreshWaveList(id);
    schedulePreview();
    recordActiveStateChange(direction < 0 ? tr("Move wave up") : tr("Move wave down"),
                            std::move(before));
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
    auto before = captureActiveState();
    const auto id = config_.swings[*index].id;
    std::swap(config_.swings[*index], config_.swings[static_cast<std::size_t>(target)]);
    syncActiveRender();
    refreshSwingList(id);
    schedulePreview();
    recordActiveStateChange(direction < 0 ? tr("Move swing up") : tr("Move swing down"),
                            std::move(before));
}

void MainWindow::moveSelectedEffect(int direction) {
    const auto index = selectedEffectIndex();
    if (!index || direction == 0) return;
    std::ptrdiff_t target = static_cast<std::ptrdiff_t>(*index) + direction;
    while (target >= 0
           && target < static_cast<std::ptrdiff_t>(config_.effects.size())
           && effect_ui_category(
                  config_.effects[static_cast<std::size_t>(target)].type)
                  != effect_category_filter_) {
        target += direction;
    }
    if (target < 0
        || target >= static_cast<std::ptrdiff_t>(config_.effects.size())) return;
    auto before = captureActiveState();
    const auto id = config_.effects[*index].id;
    std::swap(config_.effects[*index], config_.effects[static_cast<std::size_t>(target)]);
    syncActiveRender();
    refreshEffectList(id);
    schedulePreview();
    recordActiveStateChange(direction < 0 ? tr("Move effect up") : tr("Move effect down"),
                            std::move(before));
}

void MainWindow::randomizeExistingStackSettings() {
    const auto* layer = activeLayer();
    const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
    if (selected_group_uuid_ || layer == nullptr
        || (group != nullptr && group->locked)) {
        status_->setText(tr("Select an unlocked layer before randomizing it."));
        return;
    }
    auto before = captureActiveState();
    auto& random = *QRandomGenerator::global();
    const pvt::RenderConfig stable = config_;
    bool randomized = false;
    for (int attempt = 0; attempt < 32 && !randomized; ++attempt) {
        config_ = stable;
        for (auto& wave : config_.waves) {
            randomize_wave_settings(wave, random);
        }
        for (auto& swing : config_.swings) {
            randomize_swing_settings(swing, random);
        }
        for (auto& effect : config_.effects) {
            randomize_effect_settings(effect, random);
        }
        randomized = pvt::validate(config_).ok;
    }
    if (!randomized) {
        config_ = stable;
        status_->setText(tr(
            "Could not find a safe randomized value set for this canvas; "
            "the layer was left unchanged."));
        return;
    }
    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    refreshAll();
    schedulePreview();
    status_->setText(
        tr("Randomized values for %1 waves, %2 swings, and %3 effects; "
           "counts, types, names, and enabled states were preserved.")
            .arg(config_.waves.size())
            .arg(config_.swings.size())
            .arg(config_.effects.size()));
    recordActiveStateChange(tr("Randomize layer values"), std::move(before));
}

void MainWindow::randomizeStackComposition() {
    const auto* layer = activeLayer();
    const auto* group = layer != nullptr ? groupForLayer(*layer) : nullptr;
    if (selected_group_uuid_ || layer == nullptr
        || (group != nullptr && group->locked)) {
        status_->setText(tr("Select an unlocked layer before randomizing it."));
        return;
    }
    auto before = captureActiveState();
    auto& random = *QRandomGenerator::global();
    const pvt::RenderConfig stable = config_;
    bool randomized = false;
    for (int attempt = 0; attempt < 32 && !randomized; ++attempt) {
        config_.waves.clear();
        config_.swings.clear();
        config_.effects.clear();

        const int wave_count = random_integer(random, 2, 6);
        bool has_enabled_wave = false;
        for (int index = 0; index < wave_count; ++index) {
            auto wave = pvt::default_wave(static_cast<std::size_t>(index));
            wave.id = pvt::allocate_id(config_);
            wave.enabled = random_chance(random, 0.82);
            has_enabled_wave = has_enabled_wave || wave.enabled;
            randomize_wave_settings(wave, random);
            config_.waves.push_back(std::move(wave));
        }
        if (!has_enabled_wave) {
            config_.waves.front().enabled = true;
        }

        const int swing_count = random_integer(random, 0, 3);
        bool has_enabled_swing = false;
        for (int index = 0; index < swing_count; ++index) {
            auto swing = pvt::default_swing(static_cast<std::size_t>(index));
            swing.id = pvt::allocate_id(config_);
            swing.enabled = random_chance(random, 0.75);
            swing.waveform = static_cast<pvt::Waveform>(
                random_integer(random, 0, 3));
            has_enabled_swing = has_enabled_swing || swing.enabled;
            randomize_swing_settings(swing, random);
            config_.swings.push_back(std::move(swing));
        }
        if (!has_enabled_swing && !config_.swings.empty()) {
            config_.swings.front().enabled = true;
        }

        std::array<pvt::EffectType, 13> effect_types = {
            pvt::EffectType::EndlessZoom, pvt::EffectType::Ripple,
            pvt::EffectType::Shake, pvt::EffectType::FlagWave,
            pvt::EffectType::Glow, pvt::EffectType::BlockScale,
            pvt::EffectType::ParticleField, pvt::EffectType::Blur,
            pvt::EffectType::Glitch, pvt::EffectType::Starburst,
            pvt::EffectType::LensDistortion, pvt::EffectType::EdgeDetect,
            pvt::EffectType::Twirl};
        constexpr int kMaximumRandomEffects = 6;
        const int effect_count = random_integer(
            random, 1, kMaximumRandomEffects);
        bool has_enabled_effect = false;
        for (int index = 0; index < effect_count; ++index) {
            const int selected = random_integer(
                random, index, static_cast<int>(effect_types.size()) - 1);
            std::swap(effect_types[static_cast<std::size_t>(index)],
                      effect_types[static_cast<std::size_t>(selected)]);
            auto effect = pvt::default_effect(
                effect_types[static_cast<std::size_t>(index)]);
            effect.id = pvt::allocate_id(config_);
            effect.enabled = random_chance(random, 0.65);
            has_enabled_effect = has_enabled_effect || effect.enabled;
            randomize_effect_settings(effect, random);
            config_.effects.push_back(std::move(effect));
        }
        if (!has_enabled_effect) {
            config_.effects.front().enabled = true;
        }
        randomized = pvt::validate(config_).ok;
    }
    const bool used_safe_fallback = !randomized;
    if (used_safe_fallback) {
        config_ = stable;
        config_.waves.clear();
        config_.swings.clear();
        config_.effects.clear();
        for (std::size_t index = 0U; index < 2U; ++index) {
            auto wave = pvt::default_wave(index);
            wave.id = pvt::allocate_id(config_);
            wave.enabled = true;
            config_.waves.push_back(std::move(wave));
        }
        auto effect = pvt::default_effect(pvt::EffectType::Glow);
        effect.id = pvt::allocate_id(config_);
        effect.enabled = true;
        config_.effects.push_back(std::move(effect));
    }

    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    refreshAll();
    schedulePreview();
    status_->setText(used_safe_fallback
        ? tr("Random choices exceeded this canvas's safe rendering workload; "
             "a fresh default layer mix was created instead.")
        : tr("Created a new mix with %1 waves, %2 swings, and %3 effects.")
              .arg(config_.waves.size())
              .arg(config_.swings.size())
              .arg(config_.effects.size()));
    recordActiveStateChange(tr("Randomize layer mix"), std::move(before));
}

QString MainWindow::resolvedOutputDirectory(const QString& path) const {
    if (QDir::isAbsolutePath(path)) {
        return QDir::cleanPath(path);
    }
    return QDir::cleanPath(QDir(startup_working_directory_).absoluteFilePath(path));
}

QString MainWindow::usableDialogDirectory(const QString& preferred) const {
    const auto nearest_existing_directory = [this](QString candidate) {
        if (candidate.isEmpty()) {
            return QString{};
        }
        if (!QDir::isAbsolutePath(candidate)) {
            candidate = resolvedOutputDirectory(candidate);
        }
        QDir directory(candidate);
        while (true) {
            const QFileInfo information(directory.absolutePath());
            if (information.exists() && information.isDir() && !directory.isRoot()) {
                return QDir::cleanPath(directory.absolutePath());
            }
            if (directory.isRoot() || !directory.cdUp()) {
                return QString{};
            }
        }
    };

    for (const QString& candidate : {preferred, last_dialog_directory_,
                                     QDir::homePath(), startup_working_directory_}) {
        if (const QString usable = nearest_existing_directory(candidate);
            !usable.isEmpty()) {
            return usable;
        }
    }
    return QDir::homePath();
}

void MainWindow::rememberDialogLocation(const QString& selectedPath) {
    if (selectedPath.isEmpty()) {
        return;
    }
    const QFileInfo information(selectedPath);
    const QString candidate = information.exists() && information.isDir()
                                  ? information.absoluteFilePath()
                                  : information.absolutePath();
    const QFileInfo directory_information(candidate);
    const QDir directory(candidate);
    if (directory_information.exists() && directory_information.isDir()
        && !directory.isRoot()) {
        last_dialog_directory_ = QDir::cleanPath(directory.absolutePath());
    }
}

QString MainWindow::currentMusicSourcePath(bool layer_clock) const {
    if (document_ == nullptr) return {};
    const std::string attachment_id = layer_clock
        ? pvt::layer_music_attachment_id(active_layer_uuid_)
        : std::string(pvt::kMusicSourceAttachmentId);
    return QString::fromStdString(pvt::project_attachment_path(
        *document_, attachment_id));
}

void MainWindow::chooseMusicSource() {
    if (music_analysis_active_) return;
    std::vector<const pvt::LayerConfig*> reusable;
    QStringList labels;
    for (const auto& layer : project_.layers) {
        if (layer.render.layer_clock.clock.music.source_sha256.empty()) continue;
        reusable.push_back(&layer);
        labels.push_back(tr("Layer clock: %1 — %2")
                             .arg(QString::fromStdString(layer.name),
                                  QString::fromStdString(
                                      layer.render.layer_clock.clock.music.source_basename)));
    }
    if (!reusable.empty()) {
        labels.push_back(tr("Analyze a different audio file from disk…"));
        bool accepted = false;
        const QString selection = QInputDialog::getItem(
            this, tr("Choose project music asset"), tr("Matching project assets"),
            labels, 0, false, &accepted);
        if (!accepted) return;
        const qsizetype selected = labels.indexOf(selection);
        if (selected >= 0
            && static_cast<std::size_t>(selected) < reusable.size()) {
            if (document_ == nullptr) return;
            auto before = captureActiveState();
            const auto& source = *reusable[static_cast<std::size_t>(selected)];
            QString alias_error;
            if (!alias_project_attachment(
                    *document_, pvt::layer_music_attachment_id(source.uuid),
                    pvt::kMusicSourceAttachmentId, &alias_error)) {
                QMessageBox::critical(this, tr("Could not reuse audio"), alias_error);
                return;
            }
            const bool first_source = config_.clock.music.source_sha256.empty();
            config_.clock.music = source.render.layer_clock.clock.music;
            config_.clock.audio_processing =
                source.render.layer_clock.clock.audio_processing;
            config_.clock.frequency_stream_uuid.clear();
            config_.clock.mode = pvt::ClockMode::Music;
            config_.clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
            if (first_source) config_.audio_reactive_defaults.enabled = true;
            syncActiveRender();
            syncProjectGlobals();
            document_->project = project_;
            document_->dirty = true;
            recordActiveStateChange(tr("Reuse embedded project music"),
                                    std::move(before));
            loadGlobalEditors();
            updateTimelineState();
            updateExportAvailability();
            if (playback_timer_->isActive()) startProjectAudioPlayback();
            status_->setText(tr("Reused %1 without reanalysis or duplicate bytes.")
                                 .arg(QString::fromStdString(
                                     config_.clock.music.source_basename)));
            return;
        }
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose music source"), usableDialogDirectory(),
        tr("Supported audio (*.wav *.wave *.flac *.mp3);;WAV audio (*.wav *.wave);;FLAC audio (*.flac);;MP3 audio (*.mp3);;All files (*)"));
    if (path.isEmpty()) return;
    if (!config_.clock.music.source_sha256.empty()
        && QMessageBox::question(
               this, tr("Replace project music asset?"),
               tr("Analyze %1 and replace the current internal project-music binding?")
                   .arg(QFileInfo(path).fileName()),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::Yes) != QMessageBox::Yes) {
        return;
    }
    rememberDialogLocation(path);
    (void)startMusicAnalysis(path, MusicAnalysisAction::Choose);
}

void MainWindow::relinkMusicSource() {
    if (music_analysis_active_ || config_.clock.music.source_sha256.empty()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Relink embedded music source"), usableDialogDirectory(),
        tr("Supported audio (*.wav *.wave *.flac *.mp3);;All files (*)"));
    if (path.isEmpty()) return;
    rememberDialogLocation(path);
    (void)startMusicAnalysis(path, MusicAnalysisAction::Relink);
}

void MainWindow::reanalyzeMusicSource() {
    if (music_analysis_active_) return;
    const QString path = currentMusicSourcePath();
    if (path.isEmpty()) {
        music_error_->setText(
            tr("The embedded music source is unavailable. Relink the matching file first."));
        music_error_->show();
        return;
    }
    (void)startMusicAnalysis(path, MusicAnalysisAction::Reanalyze);
}

void MainWindow::chooseLayerMusicSource() {
    if (music_analysis_active_) return;
    struct ReusableMusic {
        QString label;
        std::string reference_id;
        const pvt::MusicAnalysis* analysis = nullptr;
    };
    std::vector<ReusableMusic> reusable;
    if (!project_.canvas.clock.music.source_sha256.empty()) {
        reusable.push_back({
            tr("Project clock — %1").arg(QString::fromStdString(
                project_.canvas.clock.music.source_basename)),
            pvt::kMusicSourceAttachmentId, &project_.canvas.clock.music});
    }
    for (const auto& layer : project_.layers) {
        if (layer.uuid == active_layer_uuid_
            || layer.render.layer_clock.clock.music.source_sha256.empty()) {
            continue;
        }
        reusable.push_back({
            tr("Layer clock: %1 — %2")
                .arg(QString::fromStdString(layer.name),
                     QString::fromStdString(
                         layer.render.layer_clock.clock.music.source_basename)),
            pvt::layer_music_attachment_id(layer.uuid),
            &layer.render.layer_clock.clock.music});
    }
    if (!reusable.empty()) {
        QStringList labels;
        for (const auto& source : reusable) labels.push_back(source.label);
        labels.push_back(tr("Analyze a different audio file from disk…"));
        bool accepted = false;
        const QString selection = QInputDialog::getItem(
            this, tr("Choose active-layer music asset"),
            tr("Matching project assets"), labels, 0, false, &accepted);
        if (!accepted) return;
        const qsizetype selected = labels.indexOf(selection);
        if (selected >= 0
            && static_cast<std::size_t>(selected) < reusable.size()) {
            if (document_ == nullptr) return;
            auto before = captureActiveState();
            const auto& source = reusable[static_cast<std::size_t>(selected)];
            QString alias_error;
            if (!alias_project_attachment(
                    *document_, source.reference_id,
                    pvt::layer_music_attachment_id(active_layer_uuid_),
                    &alias_error)) {
                QMessageBox::critical(this, tr("Could not reuse audio"), alias_error);
                return;
            }
            config_.layer_clock.clock.music = *source.analysis;
            config_.layer_clock.clock.audio_processing =
                source.analysis->input_processing;
            config_.layer_clock.clock.frequency_stream_uuid.clear();
            config_.layer_clock.clock.mode = pvt::ClockMode::Music;
            config_.layer_clock.clock.music_swing_policy =
                pvt::MusicSwingPolicy::KeepAll;
            config_.layer_clock.enabled = true;
            syncActiveRender();
            syncProjectGlobals();
            document_->project = project_;
            document_->dirty = true;
            recordActiveStateChange(tr("Reuse embedded active-layer music"),
                                    std::move(before));
            loadGlobalEditors();
            updateTimelineState();
            updateExportAvailability();
            if (playback_timer_->isActive()) startProjectAudioPlayback();
            status_->setText(tr("Reused %1 without reanalysis or duplicate bytes.")
                                 .arg(QString::fromStdString(
                                     config_.layer_clock.clock.music.source_basename)));
            return;
        }
    }
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose active-layer music source"), usableDialogDirectory(),
        tr("Supported audio (*.wav *.wave *.flac *.mp3);;WAV audio (*.wav *.wave);;FLAC audio (*.flac);;MP3 audio (*.mp3);;All files (*)"));
    if (path.isEmpty()) return;
    if (!config_.layer_clock.clock.music.source_sha256.empty()
        && QMessageBox::question(
               this, tr("Replace active-layer music asset?"),
               tr("Analyze %1 and replace the active layer's internal audio binding?")
                   .arg(QFileInfo(path).fileName()),
               QMessageBox::Yes | QMessageBox::No,
               QMessageBox::Yes) != QMessageBox::Yes) {
        return;
    }
    rememberDialogLocation(path);
    (void)startMusicAnalysis(path, MusicAnalysisAction::Choose, true);
}

void MainWindow::relinkLayerMusicSource() {
    if (music_analysis_active_
        || config_.layer_clock.clock.music.source_sha256.empty()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Relink active-layer music source"), usableDialogDirectory(),
        tr("Supported audio (*.wav *.wave *.flac *.mp3);;All files (*)"));
    if (path.isEmpty()) return;
    rememberDialogLocation(path);
    (void)startMusicAnalysis(path, MusicAnalysisAction::Relink, true);
}

void MainWindow::reanalyzeLayerMusicSource() {
    if (music_analysis_active_) return;
    const QString path = currentMusicSourcePath(true);
    if (path.isEmpty()) {
        layer_music_error_->setText(tr(
            "The embedded layer source is unavailable. Relink the matching file first."));
        layer_music_error_->show();
        return;
    }
    (void)startMusicAnalysis(path, MusicAnalysisAction::Reanalyze, true);
}

void MainWindow::editMusicInputProcessing(bool layer_clock) {
    if (music_analysis_active_) return;
    pvt::ClockConfig& clock = layer_clock
        ? config_.layer_clock.clock : config_.clock;
    AudioProcessingDialog dialog(
        clock.audio_processing,
        layer_clock ? tr("active-layer music") : tr("project music"), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const pvt::AudioInputProcessingConfig processing = dialog.processing();
    const auto same_processing = [](const pvt::AudioInputProcessingConfig& left,
                                    const pvt::AudioInputProcessingConfig& right) {
        if (left.high_pass_enabled != right.high_pass_enabled
            || left.high_pass_hz != right.high_pass_hz
            || left.low_pass_enabled != right.low_pass_enabled
            || left.low_pass_hz != right.low_pass_hz
            || left.equalizer_enabled != right.equalizer_enabled
            || left.equalizer_bands.size() != right.equalizer_bands.size()
            || left.frequency_streams.size() != right.frequency_streams.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.equalizer_bands.size(); ++index) {
            const auto& a = left.equalizer_bands[index];
            const auto& b = right.equalizer_bands[index];
            if (a.frequency_hz != b.frequency_hz || a.gain_db != b.gain_db) return false;
        }
        for (std::size_t index = 0; index < left.frequency_streams.size(); ++index) {
            const auto& a = left.frequency_streams[index];
            const auto& b = right.frequency_streams[index];
            if (a.uuid != b.uuid || a.name != b.name
                || a.low_hz != b.low_hz || a.high_hz != b.high_hz) return false;
        }
        return true;
    };
    if (same_processing(clock.audio_processing, processing)) return;

    if (!clock.music.source_sha256.empty()) {
        const QString path = currentMusicSourcePath(layer_clock);
        if (path.isEmpty()) {
            QMessageBox::warning(
                this, tr("Audio input processing"),
                tr("The embedded music bytes are unavailable. Relink the source "
                   "before changing pre-analysis filters or frequency streams."));
            return;
        }
        (void)startMusicAnalysis(path, MusicAnalysisAction::Reanalyze,
                                 layer_clock, processing);
        return;
    }

    auto before = captureActiveState();
    clock.audio_processing = processing;
    clock.frequency_stream_uuid.clear();
    if (layer_clock) syncActiveRender();
    else syncProjectGlobals();
    recordActiveStateChange(
        layer_clock ? tr("Edit active-layer music input processing")
                    : tr("Edit project music input processing"),
        std::move(before));
    loadGlobalEditors();
    status_->setText(tr(
        "Input processing saved. It will run before analysis when music is chosen."));
}

bool MainWindow::startMusicAnalysis(const QString& source_path,
                                    MusicAnalysisAction action,
                                    bool layer_clock,
                                    const std::optional<pvt::AudioInputProcessingConfig>&
                                        processing_override) {
    if (source_path.isEmpty() || music_analysis_watcher_ == nullptr
        || music_analysis_watcher_->isRunning()) {
        return false;
    }
    music_analysis_active_ = true;
    if (live_workspace_ != nullptr
        && live_workspace_->isPresentationActive()) {
        // Presentation owns the realtime renderer. Stop it before beginning a
        // transaction just as the inverse entry path refuses to start output
        // during analysis. Performance Live remains independently authorable.
        setLivePreviewOutputActive(false);
    }
    if (audio_playback_ != nullptr) audio_playback_->stop();
    music_analysis_layer_clock_ = layer_clock;
    const std::uint64_t generation = ++music_analysis_generation_;
    const std::uint64_t revision = document_revision_;
    const std::string layer_uuid = active_layer_uuid_;
    music_analysis_task_generation_ = generation;
    music_analysis_task_document_revision_ = revision;
    music_analysis_task_layer_uuid_ = layer_uuid;
    const std::string attachment_id = layer_clock
        ? pvt::layer_music_attachment_id(layer_uuid)
        : std::string(pvt::kMusicSourceAttachmentId);
    QProgressBar* const progress_widget = layer_clock
        ? layer_music_progress_ : music_progress_;
    QPushButton* const cancel_widget = layer_clock
        ? layer_music_cancel_ : music_cancel_;
    QLabel* const error_widget = layer_clock ? layer_music_error_ : music_error_;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    music_analysis_cancel_ = cancel;
    std::shared_ptr<pvt::ProjectDocument> staged_document;
    try {
        staged_document = std::make_shared<pvt::ProjectDocument>(
            document_ != nullptr ? *document_
                                 : pvt::default_project_document());
        staged_document->project = project_;
    } catch (const std::exception& exception) {
        music_analysis_active_ = false;
        error_widget->setText(
            tr("Music analysis could not start: %1")
                .arg(QString::fromUtf8(exception.what())));
        error_widget->show();
        updateMusicTransactionGuards();
        return false;
    } catch (...) {
        music_analysis_active_ = false;
        error_widget->setText(
            tr("The project could not be staged for music analysis."));
        error_widget->show();
        updateMusicTransactionGuards();
        return false;
    }
    progress_widget->setRange(0, 0);
    progress_widget->show();
    cancel_widget->show();
    error_widget->hide();
    status_->setText(action == MusicAnalysisAction::Relink
                         ? tr("Verifying music source…")
                         : tr("Analyzing music source…"));
    updateMusicTransactionGuards();
    updateSynchronizationState();

    const pvt::MusicAnalysis existing = layer_clock
        ? config_.layer_clock.clock.music : config_.clock.music;
    const pvt::AudioInputProcessingConfig processing = processing_override.value_or(
        layer_clock ? config_.layer_clock.clock.audio_processing
                    : config_.clock.audio_processing);
    try {
        music_analysis_watcher_->setFuture(QtConcurrent::run(
            [this, source_path, action, generation, revision, existing,
             cancel, staged_document, layer_clock, layer_uuid,
             attachment_id, processing]() mutable {
                MusicAnalysisResult result;
                result.source_path = source_path;
                result.action = action;
                result.generation = generation;
                result.document_revision = revision;
                result.layer_clock = layer_clock;
                result.layer_uuid = layer_uuid;
                result.processing = processing;
                std::string error;
                const auto progress =
                    [this, generation, revision,
                     layer_clock](std::uint64_t completed,
                                  std::uint64_t total) {
                        QMetaObject::invokeMethod(
                            this,
                            [this, generation, revision, completed, total,
                             layer_clock] {
                                if (generation != music_analysis_generation_
                                    || revision != document_revision_
                                    || music_progress_ == nullptr
                                    || layer_music_progress_ == nullptr) {
                                    return;
                                }
                                QProgressBar* const progress = layer_clock
                                    ? layer_music_progress_ : music_progress_;
                                if (total == 0U) {
                                    progress->setRange(0, 0);
                                } else {
                                    progress->setRange(0, 1000);
                                    progress->setValue(static_cast<int>(
                                        std::min<std::uint64_t>(
                                            1000U, completed * 1000U / total)));
                                }
                            },
                            Qt::QueuedConnection);
                        return true;
                    };
                if (action == MusicAnalysisAction::Relink) {
                    result.ok = pvt::audio::verify_music_source(
                        source_path.toStdString(), existing.source_sha256,
                        progress, cancel.get(), &error);
                    result.verified_only = result.ok;
                    result.analysis = existing;
                } else {
                    result.ok = pvt::audio::analyze_music_file(
                        source_path.toStdString(), processing, result.analysis,
                        progress, cancel.get(), &error);
                    if (result.ok && action == MusicAnalysisAction::Reanalyze
                        && result.analysis.source_sha256
                               != existing.source_sha256) {
                        result.ok = false;
                        error = "The embedded source changed while it was being reanalyzed.";
                    }
                }
                if (result.ok && !cancel->load(std::memory_order_relaxed)) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, generation, revision, layer_clock] {
                            if (generation != music_analysis_generation_
                                || revision != document_revision_) return;
                            status_->setText(tr("Embedding analyzed music in the project cache…"));
                            (layer_clock ? layer_music_progress_
                                         : music_progress_)->setRange(0, 0);
                        },
                        Qt::QueuedConnection);
                    if (!pvt::attach_project_file(
                            *staged_document, attachment_id,
                            source_path.toStdString(), &result.attached,
                            &error)) {
                        result.ok = false;
                    } else if (result.attached.sha256
                               != result.analysis.source_sha256) {
                        result.ok = false;
                        error = "The music source changed after analysis; the final bytes were rejected.";
                    } else {
                        result.staged_document = staged_document;
                    }
                }
                if (result.ok && cancel->load(std::memory_order_relaxed)) {
                    result.ok = false;
                }
                result.cancelled = !result.ok
                    && cancel->load(std::memory_order_relaxed);
                result.error = QString::fromStdString(error);
                return result;
            }));
    } catch (const std::exception& exception) {
        music_analysis_active_ = false;
        error_widget->setText(
            tr("Music analysis could not start: %1")
                .arg(QString::fromUtf8(exception.what())));
        error_widget->show();
        updateMusicTransactionGuards();
        updateSynchronizationState();
        return false;
    } catch (...) {
        music_analysis_active_ = false;
        error_widget->setText(tr("The background music-analysis task could not be created."));
        error_widget->show();
        updateMusicTransactionGuards();
        updateSynchronizationState();
        return false;
    }
    return true;
}

void MainWindow::cancelMusicAnalysis(const QString& message) {
    if (music_analysis_cancel_ != nullptr) {
        music_analysis_cancel_->store(true, std::memory_order_relaxed);
    }
    if (music_analysis_active_) {
        ++music_analysis_generation_;
        if (!message.isEmpty() && status_ != nullptr) status_->setText(message);
    }
}

void MainWindow::finishMusicAnalysis(const MusicAnalysisResult& result) {
    QProgressBar* const progress_widget = result.layer_clock
        ? layer_music_progress_ : music_progress_;
    QPushButton* const cancel_widget = result.layer_clock
        ? layer_music_cancel_ : music_cancel_;
    QLabel* const error_widget = result.layer_clock
        ? layer_music_error_ : music_error_;
    progress_widget->hide();
    cancel_widget->hide();
    if (result.generation != music_analysis_generation_) {
        return;
    }
    if (result.document_revision != document_revision_
        || (result.layer_clock && result.layer_uuid != active_layer_uuid_)) {
        error_widget->setText(
            tr("The completed analysis was discarded because the project changed while it was running."));
        error_widget->show();
        status_->setText(tr("Discarded stale music analysis."));
        return;
    }
    if (result.cancelled) {
        status_->setText(tr("Music analysis cancelled; the previous source was kept."));
        return;
    }
    if (!result.ok) {
        error_widget->setText(
            result.error.isEmpty() ? tr("Music analysis failed.") : result.error);
        error_widget->show();
        status_->setText(tr("Music analysis failed; the previous source was kept."));
        return;
    }

    if (result.staged_document == nullptr
        || result.attached.sha256 != result.analysis.source_sha256) {
        error_widget->setText(
            tr("The analyzed source did not produce a complete staged project attachment."));
        error_widget->show();
        status_->setText(tr("Rejected an incomplete music import transaction."));
        return;
    }

    auto before = captureActiveState();
    pvt::ClockConfig& target_clock = result.layer_clock
        ? config_.layer_clock.clock : config_.clock;
    const bool first_music_source = target_clock.music.source_sha256.empty();
    auto committed_document =
        std::make_unique<pvt::ProjectDocument>(*result.staged_document);
    document_ = std::move(committed_document);
    if (!result.verified_only) {
        target_clock.audio_processing = result.processing;
        target_clock.music = result.analysis;
        const auto selected = std::find_if(
            target_clock.audio_processing.frequency_streams.cbegin(),
            target_clock.audio_processing.frequency_streams.cend(),
            [&target_clock](const pvt::AudioFrequencyStreamConfig& stream) {
                return stream.uuid == target_clock.frequency_stream_uuid;
            });
        if (selected == target_clock.audio_processing.frequency_streams.cend()) {
            target_clock.frequency_stream_uuid.clear();
        }
    }
    target_clock.music.source_sha256 = result.attached.sha256;
    target_clock.music.source_basename = result.attached.basename;
    if (result.action == MusicAnalysisAction::Choose) {
        // A first project source enables the shared default once so inheriting
        // layers immediately demonstrate the feature. Explicit layer
        // overrides remain authoritative, and later imports/mode changes never
        // force either profile back on.
        if (!result.layer_clock && first_music_source) {
            config_.audio_reactive_defaults.enabled = true;
        }
        target_clock.mode = pvt::ClockMode::Music;
        target_clock.music_swing_policy = pvt::MusicSwingPolicy::KeepAll;
        if (result.layer_clock) config_.layer_clock.enabled = true;
    }
    syncActiveRender();
    syncProjectGlobals();
    document_->project = project_;
    document_->dirty = true;
    rememberDialogLocation(result.source_path);
    recordActiveStateChange(
        result.action == MusicAnalysisAction::Relink
            ? (result.layer_clock ? tr("Relink active-layer music")
                                  : tr("Relink embedded music"))
            : (result.action == MusicAnalysisAction::Reanalyze
                   ? (result.layer_clock ? tr("Reanalyze active-layer music")
                                         : tr("Reanalyze embedded music"))
                   : (result.layer_clock ? tr("Embed active-layer music")
                                         : tr("Embed music source"))),
        std::move(before));
    loadGlobalEditors();
    updateTimelineState();
    updateExportAvailability();
    error_widget->hide();
    status_->setText(result.action == MusicAnalysisAction::Relink
                         ? tr("Verified and embedded the relinked music source.")
                         : tr("Analyzed and embedded %1.")
                               .arg(QString::fromStdString(
                                   target_clock.music.source_basename)));
}

void MainWindow::clearMusicSource() {
    if (music_analysis_active_ || config_.clock.music.source_sha256.empty()
        || document_ == nullptr) {
        return;
    }
    if (audio_playback_ != nullptr) audio_playback_->stop();
    auto before = captureActiveState();
    std::string error;
    if (!pvt::detach_project_file(
            *document_, pvt::kMusicSourceAttachmentId, &error)) {
        QMessageBox::critical(this, tr("Could not clear music source"),
                              QString::fromStdString(error));
        return;
    }
    config_.clock.music = {};
    if (config_.clock.mode == pvt::ClockMode::Music) {
        config_.clock.mode = pvt::ClockMode::Default;
    }
    syncProjectGlobals();
    document_->project = project_;
    document_->dirty = true;
    recordActiveStateChange(tr("Clear embedded music"), std::move(before));
    loadGlobalEditors();
    updateTimelineState();
    updateExportAvailability();
    if (playback_timer_ != nullptr && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
    music_error_->hide();
    status_->setText(tr("Cleared the embedded music source; manual frames were preserved."));
}

void MainWindow::clearLayerMusicSource() {
    auto& clock = config_.layer_clock.clock;
    if (music_analysis_active_ || clock.music.source_sha256.empty()
        || document_ == nullptr) {
        return;
    }
    auto before = captureActiveState();
    std::string error;
    if (!pvt::detach_project_file(
            *document_, pvt::layer_music_attachment_id(active_layer_uuid_),
            &error)) {
        QMessageBox::critical(this, tr("Could not clear layer music source"),
                              QString::fromStdString(error));
        return;
    }
    clock.music = {};
    if (clock.mode == pvt::ClockMode::Music) {
        clock.mode = pvt::ClockMode::Default;
    }
    syncActiveRender();
    document_->project = project_;
    document_->dirty = true;
    recordActiveStateChange(tr("Clear active-layer music"), std::move(before));
    loadGlobalEditors();
    updateTimelineState();
    updateExportAvailability();
    if (playback_timer_ != nullptr && playback_timer_->isActive()) {
        startProjectAudioPlayback();
    }
    layer_music_error_->hide();
    status_->setText(tr(
        "Cleared the active-layer music source; the layer-clock settings were preserved."));
}

void MainWindow::schedulePreview() {
    ++preview_generation_;
    // Realtime output owns the preview while active. Performance Live includes
    // transient routes and clocks; presentation output uses the exact editor
    // snapshot and timeline frame. A second editor render would only compete
    // for the same CPU/GPU and could overwrite a newer delivered frame.
    if (live_workspace_ != nullptr
        && live_workspace_->isRealtimeOutputActive()) {
        if (live_workspace_->isPresentationActive()) {
            live_workspace_->requestRealtimeFrame();
        }
        preview_deferred_ = false;
        if (preview_watcher_ != nullptr && preview_watcher_->isRunning()
            && preview_cancel_ != nullptr) {
            preview_cancel_->store(true, std::memory_order_relaxed);
        }
        return;
    }
    if (preview_watcher_ && preview_watcher_->isRunning()) {
        preview_deferred_ = true;
        // Playback deliberately lets the current frame finish so a fast timer
        // cannot starve every preview. Ordinary edits cancel stale work at the
        // renderer's cooperative row/chunk checkpoints.
        if ((playback_timer_ == nullptr || !playback_timer_->isActive())
            && preview_cancel_ != nullptr) {
            preview_cancel_->store(true, std::memory_order_relaxed);
        }
        return;
    }
    if (preview_timer_) {
        if (playback_timer_ && playback_timer_->isActive()) {
            preview_timer_->stop();
            startPreview();
        } else {
            preview_timer_->start();
        }
    }
}

pvt::FrameRenderOptions MainWindow::frameRenderOptions() const {
    pvt::FrameRenderOptions options;
    options.backend = render_backend_;
    // Two shared working sets let Metal overlap submission without letting a
    // fast playback timer or frame-parallel export build an unbounded queue.
    options.maximum_gpu_frames_in_flight = 2U;
    return options;
}

void MainWindow::startPreview() {
    if (preview_watcher_->isRunning()) {
        preview_deferred_ = true;
        return;
    }
    status_->setText(tr("Rendering preview…"));
    try {
        auto project = previewProjectSnapshot();
        const int frame = timeline_->value();
        const std::uint64_t generation = preview_generation_;
        const std::uint64_t revision = document_revision_;
        preview_task_generation_ = generation;
        preview_task_document_revision_ = revision;
        const pvt::FrameRenderOptions render_options = frameRenderOptions();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        preview_cancel_ = cancel;
        preview_watcher_->setFuture(QtConcurrent::run(
            [project = std::move(project), frame, generation, revision,
             test_delay_ms = preview_test_delay_ms_, render_options,
             cancel]() mutable {
                return generatePreview(std::move(project), frame, generation, revision,
                                       test_delay_ms, render_options, cancel);
            }));
    } catch (const std::exception& exception) {
        status_->setText(
            tr("Preview could not start: %1").arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        status_->setText(tr("The background preview task could not be created."));
    }
}

pvt::ProjectConfig MainWindow::previewProjectSnapshot() const {
    auto project = project_;
    if (solo_group_uuid_) {
        for (auto& group : project.groups) {
            if (group.uuid == *solo_group_uuid_) group.enabled = true;
        }
        for (auto& layer : project.layers) {
            layer.enabled = layer.enabled
                            && layer.group_uuid == *solo_group_uuid_;
        }
        project.output.write_alpha = true;
    } else if (solo_layer_uuid_) {
        for (auto& layer : project.layers) {
            layer.enabled = layer.uuid == *solo_layer_uuid_;
        }
        if (const auto* soloed = findLayer(*solo_layer_uuid_)) {
            for (auto& group : project.groups) {
                if (group.uuid == soloed->group_uuid) group.enabled = true;
            }
        }
        // Solo is a preview-only projection of the document. A transparent
        // soloed layer may normally sit over an opaque RGB base, so allow the
        // temporary snapshot to carry alpha even when final export does not.
        project.output.write_alpha = true;
    }
    return project;
}

MainWindow::PreviewResult MainWindow::generatePreview(pvt::ProjectConfig project, int frame,
                                                       std::uint64_t generation,
                                                       std::uint64_t document_revision,
                                                       int test_delay_ms,
                                                       pvt::FrameRenderOptions render_options,
                                                       const std::shared_ptr<std::atomic_bool>& cancel) {
    PreviewResult result;
    result.frame = frame;
    result.generation = generation;
    result.document_revision = document_revision;
    try {
        const auto cancelled = [&cancel] {
            return cancel != nullptr
                   && cancel->load(std::memory_order_relaxed);
        };
        for (int remaining = test_delay_ms; remaining > 0;) {
            if (cancelled()) {
                result.error = tr("Preview cancelled.");
                return result;
            }
            const int interval = std::min(remaining, 5);
            QThread::msleep(static_cast<unsigned long>(interval));
            remaining -= interval;
        }
        if (cancelled()) {
            result.error = tr("Preview cancelled.");
            return result;
        }
        scale_project_for_preview(project);
        pvt::Image image;
        std::string error;
        if (!pvt::render_project_frame(project, frame, render_options, image,
                                       cancel != nullptr ? cancel.get() : nullptr,
                                       &error)) {
            result.error = QString::fromStdString(error);
            return result;
        }
        if (cancelled()) {
            result.error = tr("Preview cancelled.");
            return result;
        }
        result.image = QImage(image.width, image.height, QImage::Format_RGBA8888);
        if (result.image.isNull()) {
            result.error = tr("The preview image buffer could not be allocated.");
            return result;
        }
        result.image.setColorSpace(QColorSpace::SRgb);
        for (int y = 0; y < image.height; ++y) {
            if (cancelled()) {
                result.image = {};
                result.error = tr("Preview cancelled.");
                return result;
            }
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

bool MainWindow::startCurrentFrameExport(const QString& path) {
    if (live_workspace_ != nullptr
        && live_workspace_->isRealtimeOutputActive()) {
        status_->setText(tr(
            "Stop LIVE or Live Preview Output before exporting."));
        updateExportAvailability();
        return false;
    }
    cancel_export_.store(false);
    status_->setText(tr("Rendering current frame at full resolution…"));
    export_progress_->setRange(0, 0);
    export_progress_->show();
    try {
        auto project = project_;
        project.output.overwrite_existing = true; // The save dialog confirmed it.
        const int frame = timeline_->value();
        const pvt::FrameRenderOptions render_options = frameRenderOptions();
        const std::string output_path = path.toUtf8().toStdString();
        export_active_ = true;
        if (export_action_ != nullptr) export_action_->setEnabled(false);
        if (video_export_action_ != nullptr) video_export_action_->setEnabled(false);
        if (current_frame_export_action_ != nullptr) {
            current_frame_export_action_->setEnabled(false);
        }
        export_watcher_->setFuture(QtConcurrent::run(
            [this, project = std::move(project), frame, render_options,
             output_path]() mutable {
                ExportResult result;
                std::string error;
                try {
                    pvt::Image image;
                    result.ok = pvt::render_project_frame(
                        project, frame, render_options, image,
                        &cancel_export_, &error);
                    if (result.ok && !cancel_export_.load()) {
                        pvt::RenderConfig writer = pvt::default_config();
                        writer.width = project.canvas.width;
                        writer.height = project.canvas.height;
                        writer.block_size = project.canvas.block_size;
                        writer.total_frames = project.canvas.total_frames;
                        writer.fps = project.canvas.fps;
                        writer.output = project.output;
                        result.ok = pvt::write_image(
                            output_path, image, writer,
                            static_cast<std::uint32_t>(frame), &error);
                    }
                } catch (const std::exception& exception) {
                    error = std::string("Current-frame export failed: ")
                            + exception.what();
                    result.ok = false;
                } catch (...) {
                    error = "Current-frame export failed because of an unexpected error.";
                    result.ok = false;
                }
                result.cancelled = !result.ok && cancel_export_.load();
                result.error = QString::fromStdString(error);
                if (result.ok) {
                    result.success_message =
                        tr("The current full-resolution frame was exported to:\n%1")
                            .arg(QString::fromUtf8(output_path.c_str()));
                }
                return result;
            }));
    } catch (const std::exception& exception) {
        finishExportUiState();
        status_->setText(tr("Current-frame export could not start"));
        QMessageBox::critical(this, tr("Export could not start"),
                              QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        finishExportUiState();
        status_->setText(tr("Current-frame export could not start"));
        QMessageBox::critical(
            this, tr("Export could not start"),
            tr("The background current-frame export task could not be created."));
        return false;
    }
    return true;
}

bool MainWindow::startExport() {
    if (live_workspace_ != nullptr
        && live_workspace_->isRealtimeOutputActive()) {
        status_->setText(tr(
            "Stop LIVE or Live Preview Output before exporting."));
        updateExportAvailability();
        return false;
    }
    cancel_export_.store(false);
    status_->setText(tr("Exporting image sequence…"));
    export_progress_->setRange(0, 1000);
    export_progress_->setValue(0);
    export_progress_->show();
    try {
        auto project = project_;
        pvt::SequenceRenderOptions render_options;
        render_options.frame = frameRenderOptions();
        project.output.output_directory =
            resolvedOutputDirectory(QString::fromStdString(project.output.output_directory))
                .toStdString();
        export_active_ = true;
        if (video_export_action_ != nullptr) video_export_action_->setEnabled(false);
        if (current_frame_export_action_ != nullptr) {
            current_frame_export_action_->setEnabled(false);
        }
        export_watcher_->setFuture(QtConcurrent::run(
            [this, project = std::move(project), render_options]() mutable {
                ExportResult result;
                std::string error;
                try {
                    result.ok = pvt::render_project_sequence(
                        project,
                        render_options,
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
                                            export_progress_->setValue(
                                                total > 0
                                                    ? completed * 1000 / total
                                                    : 0);
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
                if (result.ok) {
                    result.success_message =
                        tr("The image sequence was exported successfully.");
                }
                return result;
            }));
    } catch (const std::exception& exception) {
        finishExportUiState();
        status_->setText(tr("Export could not start"));
        QMessageBox::critical(this, tr("Export could not start"),
                              QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        finishExportUiState();
        status_->setText(tr("Export could not start"));
        QMessageBox::critical(this, tr("Export could not start"),
                              tr("The background export task could not be created."));
        return false;
    }
    return true;
}

bool MainWindow::startVideoExport() {
    if (export_watcher_ == nullptr || export_watcher_->isRunning()) return false;
    if (live_workspace_ != nullptr
        && live_workspace_->isRealtimeOutputActive()) {
        status_->setText(tr(
            "Stop LIVE or Live Preview Output before exporting."));
        updateExportAvailability();
        return false;
    }
    const pvt::video::Capabilities available = pvt::video::capabilities();
    if (!available.available) {
        QMessageBox::information(this, tr("Video export unavailable"),
                                 QString::fromStdString(available.status));
        return false;
    }
    const auto validation = pvt::validate(project_);
    if (!validation.ok) {
        QMessageBox::warning(this, tr("Invalid setup"),
                             QString::fromStdString(validation.message));
        return false;
    }
    const std::vector<pvt::audio::PlaybackTrack> audible_tracks =
        document_ != nullptr
            ? audible_project_tracks(project_, *document_, 0.0)
            : std::vector<pvt::audio::PlaybackTrack>{};
    const bool has_music = !audible_tracks.empty();
    pvt::video::Capabilities dialog_capabilities = available;
    if ((project_.canvas.width & 1) != 0
        || (project_.canvas.height & 1) != 0) {
        dialog_capabilities.prores_4444 = false;
        dialog_capabilities.prores_4444_xq = false;
        dialog_capabilities.hevc = false;
        dialog_capabilities.hevc_alpha = false;
        dialog_capabilities.status =
            "This canvas has an odd width or height, so lossless PNG is the "
            "only compatible native movie format. ProRes and HEVC require even dimensions.";
    }
    VideoExportDialog dialog(dialog_capabilities, project_.output.write_alpha,
                             has_music, this);
    configure_readable_layouts(&dialog);
    if (dialog.exec() != QDialog::Accepted) return false;
    pvt::video::Options options = dialog.options();

    QString base_name = QString::fromStdString(
        pvt::portable_project_filename(project_.name));
    if (base_name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) {
        base_name.chop(4);
    }
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export native video"),
        QDir(usableDialogDirectory()).filePath(base_name + QStringLiteral(".mov")),
        tr("QuickTime movie (*.mov)"));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path.append(QStringLiteral(".mov"));
    rememberDialogLocation(path);
    const int total_frames = effectiveFrameCount();
    if (total_frames < 1) return false;
    int chunk_size = total_frames;
    if (options.chunk_mode == pvt::video::ChunkMode::FrameCount) {
        chunk_size = options.chunk_frames;
    } else if (options.chunk_mode
               == pvt::video::ChunkMode::MaximumSeconds) {
        chunk_size = std::max(
            1, static_cast<int>(std::floor(
                   options.chunk_maximum_seconds * project_.canvas.fps
                   + 1.0e-9)));
    }
    chunk_size = std::clamp(chunk_size, 1, total_frames);
    const bool chunked = options.chunk_mode
                             != pvt::video::ChunkMode::SingleMovie
                         && chunk_size < total_frames;
    QStringList output_paths;
    QString concat_script_path;
    if (chunked) {
        const QFileInfo requested(path);
        const QDir directory = requested.dir();
        const QString suffix = requested.suffix();
        const QString stem = requested.completeBaseName();
        const int chunk_count = (total_frames + chunk_size - 1) / chunk_size;
        const int digits = std::max(
            4, static_cast<int>(QString::number(chunk_count).size()));
        for (int index = 0; index < chunk_count; ++index) {
            output_paths.push_back(directory.filePath(
                QStringLiteral("%1.part-%2.%3")
                    .arg(stem)
                    .arg(index + 1, digits, 10, QLatin1Char('0'))
                    .arg(suffix)));
        }
        concat_script_path = directory.filePath(
            stem + QStringLiteral("-concat.sh"));
    } else {
        output_paths.push_back(path);
    }

    QStringList collisions;
    for (const QString& output_path : output_paths) {
        const QFileInfo information(output_path);
        if (information.isDir()) {
            QMessageBox::critical(
                this, tr("Invalid video destination"),
                tr("A video output path is a directory: %1").arg(output_path));
            return false;
        }
        if (information.exists() || information.isSymLink()) {
            collisions.push_back(output_path);
        }
    }
    if (!concat_script_path.isEmpty()) {
        const QFileInfo script_info(concat_script_path);
        if (script_info.isDir()) {
            QMessageBox::critical(
                this, tr("Invalid concat-script destination"),
                tr("The concat-script path is a directory: %1")
                    .arg(concat_script_path));
            return false;
        }
        if (script_info.exists() || script_info.isSymLink()) {
            collisions.push_back(concat_script_path);
        }
    }
    if (!collisions.isEmpty()) {
        const auto replace = QMessageBox::warning(
            this, chunked ? tr("Replace existing chunk outputs?")
                          : tr("Replace existing video?"),
            tr("%1 selected output file(s) already exist. Each movie remains "
               "untouched unless its complete replacement is ready. Replace them?")
                .arg(collisions.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (replace != QMessageBox::Yes) return false;
        options.overwrite_existing = true;
    }
    options.music_source_path.clear();
    options.include_project_music = options.include_project_music && has_music;
    options.frame = frameRenderOptions();

    cancel_export_.store(false);
    export_active_ = true;
    export_action_->setEnabled(false);
    if (current_frame_export_action_ != nullptr) {
        current_frame_export_action_->setEnabled(false);
    }
    video_export_action_->setEnabled(false);
    cancel_export_action_->setEnabled(true);
    export_progress_->setRange(0, 1000);
    export_progress_->setValue(0);
    export_progress_->show();
    status_->setText(tr("Exporting native video…"));
    try {
        auto project = project_;
        export_watcher_->setFuture(QtConcurrent::run(
            [this, project = std::move(project), path,
             output_paths = std::move(output_paths), concat_script_path,
             chunk_size, total_frames,
             options = std::move(options), audible_tracks]() mutable {
                ExportResult result;
                pvt::video::Report report;
                std::string export_error;
                try {
                    QTemporaryDir audio_mix_directory;
                    if (options.include_project_music) {
                        if (!audio_mix_directory.isValid()) {
                            export_error =
                                "Could not create a private temporary directory for the movie audio mix.";
                        } else {
                            QMetaObject::invokeMethod(
                                this, [this] {
                                    status_->setText(tr(
                                        "Preparing synchronized movie audio…"));
                                }, Qt::QueuedConnection);
                            std::string frame_error;
                            const int frame_count = pvt::effective_frame_count(
                                project.canvas, &frame_error);
                            const double duration = frame_count > 0
                                ? static_cast<double>(frame_count)
                                      / project.canvas.fps
                                : 0.0;
                            const QString mix_path =
                                audio_mix_directory.filePath(
                                    QStringLiteral("project-audio-mix.wav"));
                            if (pvt::audio::write_mix_wav(
                                    audible_tracks, duration,
                                    mix_path.toStdString(), &cancel_export_,
                                    &export_error)) {
                                options.music_source_path =
                                    mix_path.toStdString();
                            }
                        }
                    }
                    result.ok = export_error.empty();
                    for (int chunk_index = 0;
                         result.ok && chunk_index < output_paths.size();
                         ++chunk_index) {
                        pvt::video::Options segment = options;
                        const int first = output_paths.size() == 1
                            ? 0 : chunk_index * chunk_size;
                        const int count = output_paths.size() == 1
                            ? total_frames
                            : std::min(chunk_size, total_frames - first);
                        segment.first_frame = first;
                        segment.frame_count = count;
                        pvt::video::Report segment_report;
                        result.ok = pvt::video::export_project(
                            project, output_paths[chunk_index].toStdString(),
                            segment,
                            [this, first, total_frames, chunk_index,
                             chunk_count = output_paths.size()](int completed,
                                                                 int) {
                                const int global_completed = first + completed;
                                const int stride = std::max(1, total_frames / 200);
                                if (completed == 0
                                    || global_completed == total_frames
                                    || global_completed % stride == 0) {
                                    QMetaObject::invokeMethod(
                                        this,
                                        [this, global_completed, total_frames,
                                         chunk_index, chunk_count] {
                                            if (!export_active_) return;
                                            status_->setText(
                                                tr("Exporting video frame %1/%2 (chunk %3/%4)…")
                                                    .arg(global_completed)
                                                    .arg(total_frames)
                                                    .arg(chunk_index + 1)
                                                    .arg(chunk_count));
                                            export_progress_->setValue(
                                                global_completed * 1000
                                                / total_frames);
                                        },
                                        Qt::QueuedConnection);
                                }
                                return !cancel_export_.load();
                            },
                            &cancel_export_, &segment_report, &export_error);
                        if (result.ok) {
                            if (chunk_index == 0) {
                                report = segment_report;
                            } else {
                                report.render_workers = std::max(
                                    report.render_workers,
                                    segment_report.render_workers);
                                report.included_audio = report.included_audio
                                                        || segment_report.included_audio;
                                report.hardware_available =
                                    report.hardware_available
                                    || segment_report.hardware_available;
                                report.hardware_required =
                                    report.hardware_required
                                    || segment_report.hardware_required;
                            }
                        } else if (output_paths.size() > 1) {
                            export_error = "Video chunk "
                                + std::to_string(chunk_index + 1) + " of "
                                + std::to_string(output_paths.size())
                                + " failed: " + export_error;
                        }
                    }
                    if (result.ok && output_paths.size() > 1) {
                        QString script_error;
                        result.ok = write_video_concat_script(
                            concat_script_path, output_paths, path,
                            &script_error);
                        if (!result.ok) {
                            export_error = script_error.toStdString();
                        }
                    }
                } catch (const std::exception& exception) {
                    export_error = std::string("Video export failed: ")
                                   + exception.what();
                } catch (...) {
                    export_error = "Video export failed because of an unexpected error.";
                }
                result.cancelled = !result.ok && cancel_export_.load();
                result.error = QString::fromStdString(export_error);
                if (result.ok) {
                    QString details = QString::fromStdString(report.format_name);
                    details.append(tr(", %1 parallel render worker(s)")
                                       .arg(report.render_workers));
                    if (report.included_audio) {
                        details.append(tr(", synchronized audio mix included"));
                    }
                    if (options.codec != pvt::video::Codec::PngLossless) {
                        details.append(report.hardware_required
                                           ? tr(", hardware encoding required")
                                           : (report.hardware_available
                                                  ? tr(", hardware encoder available/preferred")
                                                  : tr(", software fallback allowed")));
                    }
                    result.success_message = output_paths.size() > 1
                        ? tr("Exported %1 native QuickTime chunks and wrote the executable concat script:\n%2\n\nRun it directly for the default relative output, or pass an output file or directory as its one optional argument.\n\n%3")
                              .arg(output_paths.size())
                              .arg(concat_script_path, details)
                        : tr("The native QuickTime movie was exported to %1.\n\n%2")
                              .arg(path, details);
                }
                return result;
            }));
    } catch (const std::exception& exception) {
        finishExportUiState();
        QMessageBox::critical(this, tr("Video export could not start"),
                              QString::fromUtf8(exception.what()));
        return false;
    } catch (...) {
        finishExportUiState();
        QMessageBox::critical(
            this, tr("Video export could not start"),
            tr("The background video-export task could not be created."));
        return false;
    }
    return true;
}

bool MainWindow::loadSetupFile(const QString& path, QString* error) {
    return loadProjectPath(path, error);
}

void MainWindow::openProject(const QString& path) {
    if (path.isEmpty()) return;
    startProjectLoad(QFileInfo(path).absoluteFilePath());
}

bool MainWindow::loadProjectPath(const QString& path, QString* error) {
    cancelMusicAnalysis();
    stopPlayback();
    pvt::ProjectDocument loaded;
    std::string load_error;
    const QFileInfo source_info(path);
    const bool legacy = source_info.isFile()
                        && source_info.suffix().compare(
                               QStringLiteral("pvt"), Qt::CaseInsensitive) == 0;
    const bool ok = legacy
                        ? pvt::import_legacy_setup(path.toStdString(), loaded, &load_error)
                        : pvt::load_project_document(path.toStdString(), loaded, &load_error);
    if (!ok) {
        if (error != nullptr) *error = QString::fromStdString(load_error);
        return false;
    }
    return adoptLoadedProject(std::move(loaded), error);
}

bool MainWindow::adoptLoadedProject(pvt::ProjectDocument loaded,
                                    QString* error) {
    if (loaded.project.layers.empty()) {
        if (error != nullptr) *error = tr("The loaded project contains no layers.");
        return false;
    }
    document_ = std::make_unique<pvt::ProjectDocument>(std::move(loaded));
    project_ = document_->project;
    active_layer_uuid_ = project_.layers.front().uuid;
    solo_layer_uuid_.reset();
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    current_project_path_ = QString::fromStdString(document_->source_path);
    imported_legacy_path_ = QString::fromStdString(document_->imported_from_path);
    baseline_dirty_ = document_->dirty || document_->legacy_import;
    updateCompatibilityWarning();
    loadActiveConfiguration();
    if (undo_stack_ != nullptr) {
        clearUndoHistory(false);
        undo_stack_->setClean();
    }
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    if (live_workspace_ != nullptr) live_workspace_->resetRealtimeFrame();
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
    addRecentProject(current_project_path_.isEmpty()
                         ? imported_legacy_path_ : current_project_path_);
    return true;
}

void MainWindow::setProjectIoActive(bool active, const QString& message) {
    project_io_active_ = active;
    if (active && live_workspace_ != nullptr
        && live_workspace_->isPresentationActive()) {
        setLivePreviewOutputActive(false);
    }
    if (project_io_progress_ != nullptr) {
        project_io_progress_->setVisible(active);
    }
    updateMusicTransactionGuards();
    if (active && status_ != nullptr && !message.isEmpty()) {
        status_->setText(message);
    }
}

void MainWindow::startProjectLoad(const QString& path) {
    if (path.isEmpty() || project_io_watcher_ == nullptr
        || project_io_watcher_->isRunning()) {
        return;
    }
    rememberDialogLocation(path);
    cancelMusicAnalysis();
    stopPlayback();
    project_io_operation_ = ProjectIoOperation::Load;
    project_io_path_ = path;
    setProjectIoActive(true, tr("Loading %1 in the background…").arg(path));
    try {
        project_io_watcher_->setFuture(QtConcurrent::run([path] {
            ProjectIoResult result;
            result.operation = ProjectIoOperation::Load;
            result.path = path;
            try {
                result.document = std::make_shared<pvt::ProjectDocument>();
                std::string error;
                const QFileInfo source_info(path);
                const bool legacy = source_info.isFile()
                                    && source_info.suffix().compare(
                                           QStringLiteral("pvt"),
                                           Qt::CaseInsensitive) == 0;
                result.ok = legacy
                                ? pvt::import_legacy_setup(
                                      path.toStdString(), *result.document, &error)
                                : pvt::load_project_document(
                                      path.toStdString(), *result.document, &error);
                result.error = QString::fromStdString(error);
            } catch (const std::exception& exception) {
                result.error = tr("Unexpected project-load error: %1")
                                   .arg(QString::fromUtf8(exception.what()));
            } catch (...) {
                result.error = tr(
                    "Project loading failed because of an unexpected error.");
            }
            return result;
        }));
    } catch (const std::exception& exception) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Load failed"),
            tr("The background project-load task could not start: %1")
                .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Load failed"),
            tr("The background project-load task could not be created."));
    }
}

bool MainWindow::runSmokeChecks(QString* error) {
    const auto* settings_menu =
        findChild<QMenu*>(QStringLiteral("settingsMenu"));
    const auto* edit_menu = findChild<QMenu*>(QStringLiteral("editMenu"));
    const auto* view_menu = findChild<QMenu*>(QStringLiteral("viewMenu"));
    const auto* mode_menu = findChild<QMenu*>(QStringLiteral("modeMenu"));
    const auto* project_toolbar =
        findChild<QToolBar*>(QStringLiteral("projectToolbar"));
    const auto* live_tabs = live_workspace_ != nullptr
        ? live_workspace_->findChild<QTabWidget*>() : nullptr;
    const auto* live_freeze = live_workspace_ != nullptr
        ? live_workspace_->findChild<QPushButton*>(
              QStringLiteral("freezeButton")) : nullptr;
    const auto* live_blackout = live_workspace_ != nullptr
        ? live_workspace_->findChild<QPushButton*>(
              QStringLiteral("blackoutButton")) : nullptr;
    auto* edit_live_project = live_workspace_ != nullptr
        ? live_workspace_->findChild<QPushButton*>(
              QStringLiteral("editLiveProjectButton")) : nullptr;
    const auto* live_audio_period = live_workspace_ != nullptr
        ? live_workspace_->findChild<QSpinBox*>(
              QStringLiteral("liveAudioPeriodFrames")) : nullptr;
    const auto* live_audio_gain = live_workspace_ != nullptr
        ? live_workspace_->findChild<QDoubleSpinBox*>(
              QStringLiteral("liveAudioGainValue")) : nullptr;
    const auto* live_audio_sensitivity = live_workspace_ != nullptr
        ? live_workspace_->findChild<QDoubleSpinBox*>(
              QStringLiteral("liveAudioSensitivityValue")) : nullptr;
    auto* live_stage_output = live_workspace_ != nullptr
        ? live_workspace_->findChild<QPushButton*>(
              QStringLiteral("liveStageOutputButton")) : nullptr;
    auto* presentation_display = findChild<QComboBox*>(
        QStringLiteral("livePreviewOutputDisplay"));
    auto* presentation_quality = findChild<QComboBox*>(
        QStringLiteral("livePreviewOutputQuality"));
    auto* presentation_fullscreen = findChild<QCheckBox*>(
        QStringLiteral("livePreviewOutputFullscreen"));
    auto* presentation_cursor = findChild<QCheckBox*>(
        QStringLiteral("livePreviewOutputHideCursor"));
    auto* presentation_button = findChild<QPushButton*>(
        QStringLiteral("livePreviewOutputButton"));
    auto* presentation_status = findChild<QLabel*>(
        QStringLiteral("livePreviewOutputStatus"));
    const auto output_displays = live_workspace_ != nullptr
        ? live_workspace_->availableOutputDisplays()
        : QVector<LiveWorkspace::OutputDisplayChoice>{};
    bool output_display_identities_valid = !output_displays.isEmpty();
    for (qsizetype index = 0; index < output_displays.size(); ++index) {
        output_display_identities_valid = output_display_identities_valid
            && !output_displays[index].id.isEmpty()
            && !output_displays[index].label.isEmpty();
        for (qsizetype other = 0; other < index; ++other) {
            output_display_identities_valid = output_display_identities_valid
                && output_displays[index].id != output_displays[other].id;
        }
    }
    if (settings_action_ == nullptr || settings_menu == nullptr
        || edit_menu == nullptr || view_menu == nullptr || mode_menu == nullptr
        || project_toolbar == nullptr
        || !settings_menu->actions().contains(settings_action_)
        || !project_toolbar->actions().contains(settings_action_)
        || randomize_values_action_ == nullptr || randomize_mix_action_ == nullptr
        || layers_dock_ == nullptr || restore_layers_dock_action_ == nullptr
        || !view_menu->actions().contains(restore_layers_dock_action_)
        || edit_mode_action_ == nullptr || live_mode_action_ == nullptr
        || !mode_menu->actions().contains(edit_mode_action_)
        || !mode_menu->actions().contains(live_mode_action_)
        || !project_toolbar->actions().contains(live_mode_action_)
        || live_mode_action_->isCheckable()
        || live_preview_output_action_ == nullptr
        || !live_preview_output_action_->isCheckable()
        || !project_toolbar->actions().contains(live_preview_output_action_)
        || presentation_display == nullptr
        || presentation_display->count() < 1
        || !output_display_identities_valid
        || presentation_quality == nullptr
        || presentation_quality->count() != 5
        || presentation_fullscreen == nullptr
        || presentation_cursor == nullptr
        || presentation_button == nullptr
        || presentation_status == nullptr
        || presentation_status->text().isEmpty()
        || workspace_stack_ == nullptr || workspace_stack_->count() != 2
        || editor_workspace_ == nullptr || live_workspace_ == nullptr
        || workspace_stack_->indexOf(editor_workspace_) < 0
        || workspace_stack_->indexOf(live_workspace_) < 0
        || workspace_stack_->currentWidget() != editor_workspace_
        || live_workspace_->isLiveActive()
        || live_tabs == nullptr || live_tabs->count() != 3
        || live_tabs->tabText(0) != tr("Rig")
        || live_tabs->tabText(1) != tr("Control Map")
        || live_tabs->tabText(2) != tr("Scenes")
        || edit_live_project == nullptr
        || music_processing_ == nullptr || music_frequency_stream_ == nullptr
        || layer_music_processing_ == nullptr
        || layer_music_frequency_stream_ == nullptr
        || live_audio_period == nullptr
        || live_audio_period->minimum() != 1
        || live_audio_period->maximum() <= 2048
        || live_audio_gain == nullptr || live_audio_gain->maximum() <= 400.0
        || live_audio_sensitivity == nullptr
        || live_audio_sensitivity->minimum() != 0.0
        || live_audio_sensitivity->maximum() <= 400.0
        || live_freeze == nullptr || !live_freeze->isCheckable()
        || live_blackout == nullptr || !live_blackout->isCheckable()
        || live_stage_output == nullptr || !live_stage_output->isCheckable()
        || export_action_ == nullptr || current_frame_export_action_ == nullptr
        || video_export_action_ == nullptr
        || cancel_export_action_ == nullptr || export_progress_ == nullptr
        || !project_toolbar->actions().contains(current_frame_export_action_)
        || !settings_menu->actions().contains(randomize_values_action_)
        || !settings_menu->actions().contains(randomize_mix_action_)
        || project_toolbar->actions().contains(randomize_values_action_)
        || project_toolbar->actions().contains(randomize_mix_action_)) {
        if (error != nullptr) {
            *error = tr("The toolbar, Edit/LIVE mode host, or guarded settings actions are incomplete or exposed in the wrong place.");
        }
        return false;
    }

    const auto stage_output_window = []() -> QWidget* {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (widget != nullptr
                && widget->objectName() == QStringLiteral("stageOutputWindow")) {
                return widget;
            }
        }
        return nullptr;
    };

    // Presentation output is intentionally independent from performance Live.
    // Keep requesting while the first frame renders: an implementation that
    // cancels on every tick will never deliver, while the controller's
    // one-in-flight/one-latest policy completes and then coalesces the rest.
    {
        QSettings output_settings;
        const QString quality_key = QStringLiteral("live/resolutionScale");
        const QString fullscreen_key = QStringLiteral("previewOutput/fullscreen");
        const QString cursor_key = QStringLiteral("previewOutput/hideCursor");
        const bool had_quality = output_settings.contains(quality_key);
        const bool had_fullscreen = output_settings.contains(fullscreen_key);
        const bool had_cursor = output_settings.contains(cursor_key);
        const QVariant saved_quality = output_settings.value(quality_key);
        const QVariant saved_fullscreen = output_settings.value(fullscreen_key);
        const QVariant saved_cursor = output_settings.value(cursor_key);
        const pvt::RenderBackend saved_render_backend = render_backend_;
        ScopeExit restore_output_settings([
            this, quality_key, fullscreen_key, cursor_key, had_quality,
            had_fullscreen, had_cursor, saved_quality, saved_fullscreen,
            saved_cursor, saved_render_backend] {
            render_backend_ = saved_render_backend;
            live_workspace_->setOutputResolutionScale(
                had_quality ? saved_quality.toDouble() : 0.0);
            live_workspace_->setPresentationFullscreen(
                had_fullscreen ? saved_fullscreen.toBool() : true);
            live_workspace_->setPresentationHideCursor(
                had_cursor ? saved_cursor.toBool() : true);
            QSettings settings;
            if (!had_quality) settings.remove(quality_key);
            if (!had_fullscreen) settings.remove(fullscreen_key);
            if (!had_cursor) settings.remove(cursor_key);
            refreshLivePreviewOutputControls();
        });

        // Keep this lifecycle/coalescing check independent of whether a user
        // persisted strict GPU on a build that intentionally lacks Metal.
        render_backend_ = pvt::RenderBackend::Cpu;
        live_workspace_->setOutputResolutionScale(0.25);
        live_workspace_->setPresentationFullscreen(false);
        live_workspace_->setPresentationHideCursor(false);
        refreshLivePreviewOutputControls();
        if (presentation_quality->currentData().toDouble() != 0.25
            || presentation_fullscreen->isChecked()
            || presentation_cursor->isChecked()
            || QSettings().value(quality_key).toDouble() != 0.25) {
            if (error != nullptr) {
                *error = tr("Live Preview Output settings did not persist or synchronize with LIVE quality controls.");
            }
            return false;
        }

        int delivered_frames = 0;
        QEventLoop delivery_loop;
        const QMetaObject::Connection delivery_connection = connect(
            live_workspace_, &LiveWorkspace::livePreviewFrame, &delivery_loop,
            [&delivered_frames, &delivery_loop](const QImage&) {
                ++delivered_frames;
                delivery_loop.quit();
            });
        live_preview_output_action_->trigger();
        QTimer pressure;
        pressure.setInterval(1);
        connect(&pressure, &QTimer::timeout, live_workspace_,
                [this] { live_workspace_->requestRealtimeFrame(); });
        pressure.start();
        QTimer::singleShot(5000, &delivery_loop, &QEventLoop::quit);
        delivery_loop.exec();
        pressure.stop();
        disconnect(delivery_connection);
        QApplication::processEvents();
        QWidget* const presentation_stage = stage_output_window();
        const bool observed_presentation =
            live_workspace_->isPresentationActive();
        const bool observed_performance = live_workspace_->isLiveActive();
        const bool observed_companion = live_popout_window_ != nullptr;
        const bool observed_stage = presentation_stage != nullptr;
        const bool observed_visible = presentation_stage != nullptr
            && presentation_stage->isVisible();
        const bool observed_checked = live_preview_output_action_->isChecked();
        if (delivered_frames == 0 || !observed_presentation
            || observed_performance || observed_companion || !observed_stage
            || !observed_visible || !observed_checked) {
            live_workspace_->setPresentationActive(false);
            if (error != nullptr) {
                *error = tr(
                    "Live Preview Output did not coalesce repeated requests into a visible presentation-only frame (frames=%1, presentation=%2, performance=%3, companion=%4, stage=%5, visible=%6, checked=%7).")
                    .arg(delivered_frames)
                    .arg(observed_presentation)
                    .arg(observed_performance)
                    .arg(observed_companion)
                    .arg(observed_stage)
                    .arg(observed_visible)
                    .arg(observed_checked);
            }
            return false;
        }
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(presentation_stage, &escape);
        QApplication::processEvents();
        if (live_workspace_->isPresentationActive()
            || live_workspace_->isLiveActive()
            || presentation_stage->isVisible()
            || live_preview_output_action_->isChecked()
            || live_popout_window_ != nullptr) {
            live_workspace_->setPresentationActive(false);
            if (error != nullptr) {
                *error = tr("Escape did not fully stop presentation-only output.");
            }
            return false;
        }
    }

    // Realtime presentation and project transactions have symmetric
    // admission. Beginning a load/save must drain presentation first, and a
    // direct LIVE request cannot use that transition window to start the
    // performance runtime while the transaction is active.
    live_workspace_->setPresentationActive(true);
    QApplication::processEvents();
    setProjectIoActive(true, QString{});
    QApplication::processEvents();
    setLiveMode(true);
    QApplication::processEvents();
    const bool transaction_output_guarded =
        !live_workspace_->isPresentationActive()
        && !live_workspace_->isLiveActive()
        && live_mode_action_ != nullptr
        && !live_mode_action_->isEnabled();
    setProjectIoActive(false);
    if (!transaction_output_guarded) {
        live_workspace_->setPresentationActive(false);
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr(
                "A project transaction did not stop presentation output and block the reverse transition into performance LIVE.");
        }
        return false;
    }

    // LIVE is a one-click companion window, not a navigation mode. Exercise
    // the actual toolbar action and require its central workspace to become
    // visible after QStackedWidget releases it; a top-level shell with a hidden
    // central widget is the original blank-window regression.
    live_mode_action_->trigger();
    QApplication::processEvents();
    QMainWindow* const automatic_live_window = live_popout_window_;
    if (automatic_live_window == nullptr
        || automatic_live_window->centralWidget() != live_workspace_
        || !automatic_live_window->isVisible()
        || !live_workspace_->isVisible()
        || live_workspace_->size().isEmpty()
        || workspace_stack_->currentWidget() != editor_workspace_
        || workspace_stack_->indexOf(live_workspace_) >= 0
        || !live_workspace_->isLiveActive()
        || !edit_mode_action_->isChecked()
        || live_mode_action_->isChecked()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("Opening LIVE did not create a populated, visible companion window while preserving the editor.");
        }
        return false;
    }

    // Dismissing only the stage surface must not tear down performance Live.
    live_stage_output->click();
    QApplication::processEvents();
    QWidget* const performance_stage = stage_output_window();
    if (performance_stage == nullptr || !performance_stage->isVisible()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("Performance Live could not open its stage output.");
        }
        return false;
    }
    QKeyEvent performance_escape(
        QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(performance_stage, &performance_escape);
    QApplication::processEvents();
    if (!live_workspace_->isLiveActive() || performance_stage->isVisible()
        || live_stage_output->isChecked()
        || live_popout_window_ != automatic_live_window
        || !automatic_live_window->isVisible()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("Escape from the performance stage incorrectly stopped LIVE.");
        }
        return false;
    }

    // Authoring is a focus change, not a transport stop. Exercise the actual
    // Live-header button so a future UI refactor cannot quietly reintroduce
    // teardown while MIDI/OSC edits continue to work.
    edit_live_project->click();
    if (workspace_stack_->currentWidget() != editor_workspace_
        || live_popout_window_ != automatic_live_window
        || !automatic_live_window->isVisible()
        || !live_workspace_->isLiveActive()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("Opening the project editor interrupted the Live runtime.");
        }
        return false;
    }

    // Closing the companion window is an explicit stop. The workspace must be
    // returned to its private stack slot so opening LIVE again is repeatable,
    // and no hidden runtime may continue capturing audio or preventing sleep.
    automatic_live_window->close();
    QApplication::processEvents();
    if (live_popout_window_ != nullptr
        || workspace_stack_->count() != 2
        || workspace_stack_->indexOf(live_workspace_) < 0
        || workspace_stack_->currentWidget() != editor_workspace_
        || live_workspace_->isLiveActive()
        || !edit_mode_action_->isChecked()
        || live_mode_action_->isChecked()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("Closing the LIVE window did not stop and restore its workspace cleanly.");
        }
        return false;
    }

    live_mode_action_->trigger();
    QApplication::processEvents();
    QMainWindow* const reopened_live_window = live_popout_window_;
    if (reopened_live_window == nullptr
        || reopened_live_window->centralWidget() != live_workspace_
        || !reopened_live_window->isVisible()
        || !live_workspace_->isVisible()
        || !live_workspace_->isLiveActive()) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("LIVE could not be reopened after its companion window was closed.");
        }
        return false;
    }
    reopened_live_window->close();
    QApplication::processEvents();
    if (live_popout_window_ != nullptr || live_workspace_->isLiveActive()
        || workspace_stack_->indexOf(live_workspace_) < 0) {
        restoreLiveWorkspace(false);
        if (error != nullptr) {
            *error = tr("The reopened LIVE window did not clean up correctly.");
        }
        return false;
    }

    layers_dock_->setFloating(true);
    layers_dock_->hide();
    restore_layers_dock_action_->trigger();
    if (layers_dock_->isFloating() || layers_dock_->isHidden()
        || dockWidgetArea(layers_dock_) != Qt::LeftDockWidgetArea) {
        if (error != nullptr) {
            *error = tr("The Project & Layers panel could not be restored from a floating or hidden state.");
        }
        return false;
    }

    // Completion must clear the guard before action availability is refreshed.
    // This directly covers the signal-ordering regression that left both export
    // commands disabled after the success dialog was dismissed.
    export_active_ = true;
    export_action_->setEnabled(false);
    current_frame_export_action_->setEnabled(false);
    video_export_action_->setEnabled(false);
    cancel_export_action_->setEnabled(true);
    export_progress_->show();
    finishExportUiState();
    const bool expected_video_enabled =
        !music_analysis_active_ && pvt::video::capabilities().available;
    if (export_active_ || !export_action_->isEnabled()
        || !current_frame_export_action_->isEnabled()
        || video_export_action_->isEnabled() != expected_video_enabled
        || cancel_export_action_->isEnabled() || !export_progress_->isHidden()) {
        if (error != nullptr) {
            *error = tr("Export completion did not restore the export actions and clear the cancel state.");
        }
        return false;
    }
    for (const QAction* action : edit_menu->actions()) {
        if (action->text() == tr("Undo History Limit…")
            || (action->menu() != nullptr
                && action->menu()->title() == tr("Rendering Backend"))) {
            if (error != nullptr) {
                *error = tr("A program preference is still exposed in the Edit menu.");
            }
            return false;
        }
    }

    QSettings saved_settings;
    const int expected_undo_limit = std::clamp(
        saved_settings.value(QStringLiteral("preferences/undoLimit"),
                             kDefaultUndoLimit).toInt(),
        kMinimumUndoLimit, kMaximumUndoLimit);
    const int saved_backend = saved_settings.value(
        QStringLiteral("preferences/renderBackend"),
        static_cast<int>(pvt::RenderBackend::CpuAndGpu)).toInt();
    const bool automated_smoke = QCoreApplication::arguments().contains(
        QStringLiteral("--smoke-test"));
    pvt::RenderBackend expected_backend = pvt::RenderBackend::Cpu;
    if (!automated_smoke) {
        expected_backend =
            saved_backend >= static_cast<int>(pvt::RenderBackend::Cpu)
                    && saved_backend <= static_cast<int>(pvt::RenderBackend::Gpu)
                ? static_cast<pvt::RenderBackend>(saved_backend)
                : pvt::RenderBackend::CpuAndGpu;
    }
    if (undo_stack_ == nullptr || undo_stack_->undoLimit() != expected_undo_limit
        || render_backend_ != expected_backend
        || frameRenderOptions().backend != render_backend_) {
        if (error != nullptr) {
            *error = tr("Application preferences were not restored from per-user settings.");
        }
        return false;
    }

    {
        pvt::RendererCapabilities smoke_capabilities;
        smoke_capabilities.opengl_surface_compiled = true;
        smoke_capabilities.opengl_surface_status =
            "Hardware capability probe omitted during deterministic smoke test.";
        ApplicationSettingsDialog settings_dialog(
            expected_undo_limit, expected_backend,
            recent_project_limit_,
            hasCustomNewProjectDefaults(), this, &smoke_capabilities);
        configure_readable_layouts(&settings_dialog);
        auto* tabs = settings_dialog.findChild<QTabWidget*>(
            QStringLiteral("applicationSettingsTabs"));
        auto* undo_limit = settings_dialog.findChild<QSpinBox*>(
            QStringLiteral("undoLimitPreference"));
        const auto* backend = settings_dialog.findChild<QComboBox*>(
            QStringLiteral("renderBackendPreference"));
        const auto* recent_limit = settings_dialog.findChild<QSpinBox*>(
            QStringLiteral("recentProjectLimitPreference"));
        const auto* save_defaults = settings_dialog.findChild<QPushButton*>(
            QStringLiteral("saveCurrentProjectDefaults"));
        const auto* restore_defaults = settings_dialog.findChild<QPushButton*>(
            QStringLiteral("restoreBuiltInDefaults"));
        const auto* settings_scroll = settings_dialog.findChild<QScrollArea*>(
            QStringLiteral("applicationSettingsScroll"));
        const auto* capability_status = settings_dialog.findChild<QLabel*>(
            QStringLiteral("rendererCapabilityStatus"));
        settings_dialog.show();
        QApplication::processEvents();
        bool wheel_edit_safe = false;
        if (undo_limit != nullptr && tabs != nullptr) {
            const int before_wheel = undo_limit->value();
            tabs->setFocus(Qt::OtherFocusReason);
            const QPoint local = undo_limit->rect().center();
            const QPoint global = undo_limit->mapToGlobal(local);
            QWheelEvent wheel(
                QPointF(local), QPointF(global), QPoint(0, -24), QPoint(),
                Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false,
                Qt::MouseEventSynthesizedBySystem);
            QApplication::sendEvent(undo_limit, &wheel);
            QWidget* const focus_after_wheel = QApplication::focusWidget();
            wheel_edit_safe = undo_limit->value() == before_wheel
                && focus_after_wheel != undo_limit
                && !undo_limit->isAncestorOf(focus_after_wheel);
        }
        QScreen* settings_screen = settings_dialog.screen();
        const bool settings_fit_screen = settings_screen == nullptr
            || (settings_dialog.width()
                    <= settings_screen->availableGeometry().width() - 16
                && settings_dialog.height()
                    <= settings_screen->availableGeometry().height() - 16);
        settings_dialog.hide();
        if (tabs == nullptr || tabs->count() < 2 || undo_limit == nullptr
            || recent_limit == nullptr
            || backend == nullptr || backend->count() != 3
            || save_defaults == nullptr || restore_defaults == nullptr
            || settings_scroll == nullptr || !settings_scroll->widgetResizable()
            || capability_status == nullptr
            || capability_status->text().isEmpty()
            || !undo_limit->specialValueText().isEmpty()
            || !recent_limit->specialValueText().isEmpty()
            || !wheel_edit_safe
            || !settings_fit_screen
            || settings_dialog.undoLimit() != expected_undo_limit
            || settings_dialog.recentProjectLimit() != recent_project_limit_
            || settings_dialog.renderBackend() != expected_backend) {
            if (error != nullptr) {
                *error = tr("The extensible Application Settings dialog is incomplete or malformed.");
            }
            return false;
        }
    }
    const auto* live_last_good_timeout = live_workspace_->findChild<QSpinBox*>(
        QStringLiteral("liveLastGoodTimeout"));
    if (swing_radius_ == nullptr || effect_area_radius_ == nullptr
        || png_compression_ == nullptr || live_last_good_timeout == nullptr
        || !swing_radius_->specialValueText().isEmpty()
        || !effect_area_radius_->specialValueText().isEmpty()
        || !png_compression_->specialValueText().isEmpty()
        || !live_last_good_timeout->specialValueText().isEmpty()) {
        if (error != nullptr) {
            *error = tr("A numeric editor still replaces zero with words.");
        }
        return false;
    }
    // Preference restoration is verified above. Keep the remainder of this
    // deterministic smoke suite independent of whether the current machine
    // can satisfy a user's persisted strict-GPU preference.
    render_backend_ = pvt::RenderBackend::Cpu;
    if (motion_group_ == nullptr || motion_paths_edit_ == nullptr
        || motion_group_->isAncestorOf(motion_paths_edit_)) {
        if (error != nullptr) {
            *error = tr("Reusable paths are trapped inside the whole-layer motion prerequisite.");
        }
        return false;
    }
    {
        const bool original_motion_enabled = motion_group_->isChecked();
        const QSignalBlocker blocker(motion_group_);
        motion_group_->setChecked(false);
        const bool path_editor_reachable = motion_paths_edit_->isEnabled();
        motion_group_->setChecked(original_motion_enabled);
        if (!path_editor_reachable) {
            if (error != nullptr) {
                *error = tr("Reusable paths cannot be edited while whole-layer motion is disabled.");
            }
            return false;
        }
    }
    if (wave_output_status_ == nullptr
        || wave_displacement_enabled_ == nullptr
        || wave_lighting_enabled_ == nullptr
        || wave_output_status_->text().isEmpty()) {
        if (error != nullptr) {
            *error = tr("The Wave page does not expose its output prerequisites.");
        }
        return false;
    }
    if (surface_mapping_ == nullptr || surface_obj_row_ == nullptr
        || surface_plane_displacement_group_ == nullptr
        || surface_plane_displacement_enabled_ == nullptr
        || surface_plane_displacement_path_ == nullptr
        || surface_plane_displacement_browse_ == nullptr
        || surface_plane_displacement_clear_ == nullptr
        || surface_plane_displacement_minimum_ == nullptr
        || surface_plane_displacement_maximum_ == nullptr
        || surface_plane_displacement_midpoint_ == nullptr
        || surface_plane_displacement_ratio_ == nullptr
        || surface_plane_displacement_export_ == nullptr) {
        if (error != nullptr) {
            *error = tr("The Plane displacement editor is incomplete.");
        }
        return false;
    }
    {
        const int original_mapping = surface_mapping_->currentIndex();
        const QSignalBlocker blocker(surface_mapping_);
        select_enum(surface_mapping_, pvt::SurfaceMapping::Plane);
        updateSurfaceEditorState();
        const bool plane_editor_reachable =
            !surface_plane_displacement_group_->isHidden()
            && surface_plane_displacement_group_->isAncestorOf(
                surface_plane_displacement_browse_)
            && surface_plane_displacement_browse_->isEnabled()
            && surface_obj_row_->isHidden();
        select_enum(surface_mapping_, pvt::SurfaceMapping::CustomObj);
        updateSurfaceEditorState();
        const bool custom_obj_exclusive =
            surface_plane_displacement_group_->isHidden()
            && !surface_obj_row_->isHidden();
        surface_mapping_->setCurrentIndex(original_mapping);
        updateSurfaceEditorState();
        if (!plane_editor_reachable || !custom_obj_exclusive) {
            if (error != nullptr) {
                *error = tr("Plane displacement is unreachable while off or overlaps the Custom OBJ source editor.");
            }
            return false;
        }
    }
    {
        pvt::ProjectConfig live_probe = pvt::default_project();
        auto& live_surface = live_probe.layers.front().render.surface;
        live_surface.enabled = true;
        live_surface.mapping = pvt::SurfaceMapping::Plane;
        live_surface.plane_displacement.enabled = true;
        live_surface.plane_displacement.path = "embedded-height.png";
        const QString prefix = QStringLiteral("layer/%1/surface.")
                                   .arg(QString::fromStdString(
                                       live_probe.layers.front().uuid));
        const auto targets = buildLiveTargetRegistry(live_probe);
        const auto target = [&targets, &prefix](const QString& suffix) {
            return std::find_if(
                targets.begin(), targets.end(),
                [&prefix, &suffix](const LiveTargetDescriptor& item) {
                    return item.path == prefix + suffix;
                });
        };
        const auto enabled = target(QStringLiteral("plane_displacement.enabled"));
        const auto minimum = target(QStringLiteral("plane_displacement.minimum"));
        const auto maximum = target(QStringLiteral("plane_displacement.maximum"));
        const auto midpoint = target(QStringLiteral("plane_displacement.midpoint"));
        const auto mapping = target(QStringLiteral("mapping"));
        if (enabled == targets.end() || minimum == targets.end()
            || maximum == targets.end() || midpoint == targets.end()
            || mapping == targets.end()
            || !minimum->apply(live_probe, -0.75)
            || live_probe.layers.front().render.surface.plane_displacement.minimum
                   != -0.75
            || !mapping->apply(
                live_probe, static_cast<double>(pvt::SurfaceMapping::Cylinder))
            || live_probe.layers.front().render.surface.plane_displacement.enabled
            || !enabled->apply(live_probe, 1.0)
            || !live_probe.layers.front().render.surface.enabled
            || live_probe.layers.front().render.surface.mapping
                   != pvt::SurfaceMapping::Plane
            || !live_probe.layers.front().render.surface.plane_displacement.enabled) {
            if (error != nullptr) {
                *error = tr("Live Plane displacement targets are incomplete or can create an invalid surface combination.");
            }
            return false;
        }
    }
    pvt::RenderConfig alpha_probe = pvt::default_config();
    alpha_probe.starting_image.enabled = true;
    alpha_probe.alpha.use_source_alpha = false;
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("An opaque-decoded starting image unnecessarily forced alpha output.");
        }
        return false;
    }
    alpha_probe.surface.enabled = true;
    alpha_probe.surface.mapping = pvt::SurfaceMapping::Plane;
    alpha_probe.surface.plane_displacement.enabled = true;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A displaced Plane did not request alpha for its mesh exterior.");
        }
        return false;
    }
    alpha_probe.surface.curvature = 0.0;
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A neutral displaced Plane unnecessarily forced alpha output.");
        }
        return false;
    }
    alpha_probe.alpha.use_source_alpha = true;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A source-alpha starting image did not request alpha output.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.alpha.use_source_alpha = false;
    alpha_probe.starting_colors.include_alpha = true;
    alpha_probe.starting_colors.alpha_minimum = 0.5;
    alpha_probe.starting_colors.alpha_maximum = 0.5;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("Generated alpha was incorrectly disabled by the palette/image source-alpha switch.");
        }
        return false;
    }
    const pvt::ProjectDocument built_in = built_in_workbench_project_document();
    if (built_in.project.layers.empty()
        || !built_in.project.layers.front().render.alpha.use_source_alpha) {
        if (error != nullptr) {
            *error = tr("The built-in first layer disagrees with normal layer alpha defaults.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.motion.enabled = true;
    alpha_probe.motion.path = pvt::LayerMotionPath::Orbit;
    alpha_probe.motion.travel_x = 0.0;
    alpha_probe.motion.travel_y = 0.0;
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A zero-travel built-in motion path unnecessarily forced alpha output.");
        }
        return false;
    }
    alpha_probe.motion.path = pvt::LayerMotionPath::None;
    alpha_probe.motion.scale_pulse = 0.5;
    alpha_probe.motion.cycles_y = 0;
    alpha_probe.motion.phase_degrees = 180.0;
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A constant neutral scale pulse unnecessarily forced alpha output.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.effects.clear();
    auto neutral_lens = pvt::default_effect(
        pvt::EffectType::LensDistortion);
    neutral_lens.enabled = true;
    neutral_lens.edge_mode = pvt::EdgeMode::Alpha;
    neutral_lens.secondary = 0.0;
    alpha_probe.effects.push_back(neutral_lens);
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A neutral lens distortion unnecessarily forced alpha output.");
        }
        return false;
    }
    alpha_probe.effects.front().secondary = 1.0;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("An active lens distortion with transparent edges did not request alpha output.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.motion.enabled = true;
    alpha_probe.motion.custom_path.enabled = true;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A reusable whole-layer path was not recognized as alpha-expanding motion.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.motion.enabled = true;
    alpha_probe.motion.rotation_offset_degrees = 15.0;
    if (!configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("A static whole-layer rotation was not recognized as alpha-expanding motion.");
        }
        return false;
    }
    pvt::ProjectConfig eraser_probe = pvt::default_project();
    pvt::LayerConfig zero_alpha_eraser = pvt::default_layer(1U);
    zero_alpha_eraser.blend_mode = pvt::BlendMode::Erase;
    zero_alpha_eraser.render.alpha.enabled = true;
    zero_alpha_eraser.render.alpha.minimum = 0.0;
    zero_alpha_eraser.render.alpha.maximum = 0.0;
    eraser_probe.layers.push_back(zero_alpha_eraser);
    if (visible_stack_requires_alpha(eraser_probe)) {
        if (error != nullptr) {
            *error = tr("A guaranteed zero-alpha eraser unnecessarily forced alpha output.");
        }
        return false;
    }
    auto particle = pvt::default_effect(pvt::EffectType::ParticleField);
    particle.enabled = true;
    eraser_probe.layers.back().render.effects.push_back(particle);
    if (!visible_stack_requires_alpha(eraser_probe)) {
        if (error != nullptr) {
            *error = tr("A particle-producing eraser failed to request alpha output.");
        }
        return false;
    }
    alpha_probe = pvt::default_config();
    alpha_probe.surface.enabled = true;
    alpha_probe.surface.mapping = pvt::SurfaceMapping::Plane;
    alpha_probe.surface.rotations_per_loop = 1;
    if (configuration_requires_alpha(alpha_probe)) {
        if (error != nullptr) {
            *error = tr("Reflected plane rotation unnecessarily forced alpha output.");
        }
        return false;
    }
    if (motion_rotation_offset_ == nullptr) {
        if (error != nullptr) {
            *error = tr("Layer motion is missing its starting-rotation editor.");
        }
        return false;
    }
    const double original_rotation_offset =
        config_.motion.rotation_offset_degrees;
    const double edited_rotation_offset =
        std::fabs(original_rotation_offset - 17.5) > 1.0e-12 ? 17.5 : -17.5;
    motion_rotation_offset_->setValue(edited_rotation_offset);
    if (std::fabs(config_.motion.rotation_offset_degrees
                  - edited_rotation_offset) > 1.0e-12) {
        if (error != nullptr) {
            *error = tr("The starting-rotation editor did not update layer motion.");
        }
        return false;
    }
    motion_rotation_offset_->setValue(original_rotation_offset);
    bool inspected_motion_path_editor = false;
    QTimer::singleShot(0, this, [this, &inspected_motion_path_editor] {
        if (auto* dialog = findChild<QDialog*>(
                QStringLiteral("motionPathEditor"))) {
            inspected_motion_path_editor =
                dialog->findChild<QComboBox*>(
                    QStringLiteral("motionPathSelector")) != nullptr
                && dialog->findChild<QTableWidget*>(
                    QStringLiteral("motionPathNodes")) != nullptr
                && dialog->findChild<QComboBox*>(
                    QStringLiteral("motionPathConsumer")) != nullptr;
            dialog->reject();
        }
    });
    showMotionPathEditor();
    if (!inspected_motion_path_editor) {
        if (error != nullptr) {
            *error = tr("The reusable motion-path editor is incomplete or malformed.");
        }
        return false;
    }
    bool inspected_about = false;
    QTimer::singleShot(0, this, [this, &inspected_about] {
        if (auto* dialog = findChild<QDialog*>(
                QStringLiteral("aboutPvtDialog"))) {
            bool website = false;
            bool bug = false;
            bool vulnerability = false;
            bool version = false;
            for (const QPushButton* button :
                 dialog->findChildren<QPushButton*>()) {
                const QString url = button->property("pvtExternalUrl").toString();
                website = website || url.endsWith(
                    QStringLiteral("/procedural_visualizer_tool"));
                bug = bug || url.endsWith(
                    QStringLiteral("/procedural_visualizer_tool/issues/new"));
                vulnerability = vulnerability || url.endsWith(
                    QStringLiteral("/procedural_visualizer_tool/security/advisories/new"));
            }
            for (const QLabel* label : dialog->findChildren<QLabel*>()) {
                version = version || label->text().contains(
                    QStringLiteral(PVT_PROGRAM_VERSION));
            }
            inspected_about = website && bug && vulnerability
                              && version;
            dialog->reject();
        }
    });
    showAboutDialog();
    if (!inspected_about) {
        if (error != nullptr) {
            *error = tr("About PVT is missing its version or reporting links.");
        }
        return false;
    }
    if (findChild<QComboBox*>(QStringLiteral("exportTarget")) != nullptr) {
        if (error != nullptr) {
            *error = tr("The removed MP4 export target is still exposed by the GUI.");
        }
        return false;
    }
    // Native control metrics can settle when the first top-level companion or
    // dialog is shown. Reapply the readability floors after those smoke paths
    // so this pre-show test observes the same final metrics as normal startup.
    configure_readable_layouts(this);
    for (QFormLayout* form : findChildren<QFormLayout*>()) {
        if (form->rowWrapPolicy() != QFormLayout::WrapLongRows) {
            if (error != nullptr) {
                *error = tr("A form can still compress a long label/control row instead of wrapping it.");
            }
            return false;
        }
    }
    for (QPushButton* button : findChildren<QPushButton*>()) {
        if (button->minimumWidth() < button->sizeHint().width()) {
            if (error != nullptr) {
                *error = tr("A button can still shrink below the width required by its text: %1")
                             .arg(button->text());
            }
            return false;
        }
    }
    for (QComboBox* combo : findChildren<QComboBox*>()) {
        if (combo->minimumWidth() < combo->sizeHint().width()) {
            if (error != nullptr) {
                *error = tr("A choice control can still shrink below its readable width: %1 (%2 < %3, current: %4)")
                             .arg(combo->objectName())
                             .arg(combo->minimumWidth())
                             .arg(combo->sizeHint().width())
                             .arg(combo->currentText());
            }
            return false;
        }
    }
    const QString home_directory =
        existing_writable_directory(QDir::homePath(), true);
    const QString saved_dialog_directory = existing_writable_directory(
        QSettings().value(QStringLiteral("paths/lastDialogDirectory")).toString(), true);
    const QString expected_dialog_directory = !saved_dialog_directory.isEmpty()
                                                  ? saved_dialog_directory
                                                  : home_directory;
    if (!expected_dialog_directory.isEmpty()
        && !QDir(expected_dialog_directory).isRoot()
        && usableDialogDirectory() != QDir::cleanPath(expected_dialog_directory)) {
        if (error != nullptr) {
            *error = tr("The first file dialog did not restore its saved location or home fallback.");
        }
        return false;
    }
    if (QDir(resolvedOutputDirectory(QStringLiteral("."))).isRoot()) {
        if (error != nullptr) {
            *error = tr("A relative dot output directory resolved to filesystem root.");
        }
        return false;
    }
    {
        const QSignalBlocker enabled_blocker(starting_image_enabled_);
        const bool was_checked = starting_image_enabled_->isChecked();
        starting_image_enabled_->setChecked(false);
        const bool source_can_be_chosen = !starting_image_group_->isCheckable()
                                          && starting_image_browse_->isEnabled();
        starting_image_enabled_->setChecked(was_checked);
        if (!source_can_be_chosen) {
            if (error != nullptr) {
                *error = tr("The starting-image chooser became unreachable while its source was disabled.");
            }
            return false;
        }
    }

    const auto original = config_;
    auto expected = original;
    expected.surface.rotations_per_loop = 900;
    expected.surface.lighting = 9.0;
    expected.surface.obj_path = "meshes/smoke test.obj";
    expected.ghost_lag_degrees = 5.729612345678;
    expected.output.png_compression_level = 9;
    expected.output.write_alpha = true;
    expected.alpha.use_source_alpha = false;
    expected.starting_colors.mode = pvt::StartingColorMode::Random;
    expected.starting_colors.include_alpha = true;
    expected.starting_colors.red_steps = 17;
    expected.starting_colors.green_steps = 19;
    expected.starting_colors.blue_steps = 23;
    expected.starting_colors.alpha_steps = 29;
    expected.starting_colors.red_minimum = 0.125;
    expected.starting_colors.red_maximum = 0.875;
    expected.starting_colors.alpha_minimum = 0.25;
    expected.starting_colors.alpha_maximum = 0.75;
    expected.starting_image.palette_dither_enabled = true;
    expected.starting_image.palette_dither_method =
        pvt::DitherMethod::FloydSteinberg;
    if (!expected.waves.empty()) {
        expected.waves.front().x_percent = 29.166712345678;
    }
    if (!expected.swings.empty()) {
        expected.swings.front().phase_degrees = 137.50776405003785;
        expected.swings.front().center_x = 0.2345;
        expected.swings.front().center_y = 0.7654;
        expected.swings.front().radius = 0.3141;
    }
    if (!expected.effects.empty()) {
        expected.effects.front().center_x = -9.0;
        expected.effects.front().center_y = 9.0;
        expected.effects.front().frequency = 1.123456789012;
        expected.effects.front().space = pvt::EffectSpace::Surface;
        expected.effects.front().area_radius = 0.2718;
    }
    expected.palette = pvt::default_palette(3U);
    expected.transform.flip_vertical = true;
    expected.transform.mirror = pvt::MirrorMode::RightToLeft;
    QTemporaryDir directory;
    if (!directory.isValid()) {
        if (error != nullptr) {
            *error = tr("Could not create the GUI smoke-test directory.");
        }
        return false;
    }
    {
        const QString script_path = directory.filePath(
            QStringLiteral("video concat smoke.sh"));
        const QString movie_path = directory.filePath(
            QStringLiteral("final movie.mov"));
        const QStringList chunks = {
            directory.filePath(QStringLiteral("part one.mov")),
            directory.filePath(QStringLiteral("part 'two'.mov"))};
        QString script_error;
        if (!write_video_concat_script(
                script_path, chunks, movie_path, &script_error)) {
            if (error != nullptr) {
                *error = tr("The video concat script could not be generated: %1")
                             .arg(script_error);
            }
            return false;
        }
        QFile script(script_path);
        if (!script.open(QIODevice::ReadOnly)) {
            if (error != nullptr) *error = tr("The generated concat script could not be read.");
            return false;
        }
        const QString contents = QString::fromUtf8(script.readAll());
        const QFileDevice::Permissions permissions = QFile::permissions(script_path);
        const bool absolute_inputs = std::all_of(
            chunks.cbegin(), chunks.cend(), [&contents](const QString& chunk) {
                return contents.contains(ffconcat_single_quote(
                    QFileInfo(chunk).absoluteFilePath()));
            });
        const bool expected_command = contents.contains(
            QStringLiteral("ffmpeg -f concat -safe 0 -i \"$input_file_list\" -c copy \"$output\""));
        const bool relative_default = contents.contains(
            QStringLiteral("output='final movie-reassembled.mov'"));
        const bool argument_override = contents.contains(
            QStringLiteral("if [[ -d \"$1\" ]]"))
            && contents.contains(QStringLiteral("output=$1"));
        bool valid_syntax = true;
        bool executable = true;
#ifndef Q_OS_WIN
        QProcess syntax;
        syntax.start(QStringLiteral("/bin/bash"),
                     {QStringLiteral("-n"), script_path});
        valid_syntax = syntax.waitForFinished(10000)
                       && syntax.exitStatus() == QProcess::NormalExit
                       && syntax.exitCode() == 0;
        executable = permissions & QFileDevice::ExeOwner;
#else
        Q_UNUSED(permissions);
#endif
        if (!absolute_inputs || !expected_command || !relative_default
            || !argument_override || !valid_syntax
            || !executable) {
            if (error != nullptr) {
                *error = tr("The generated concat script is not portable, executable, or syntactically valid.");
            }
            return false;
        }
    }
    const auto original_project = project_;
    const std::optional<pvt::ProjectDocument> original_document =
        document_ != nullptr
            ? std::optional<pvt::ProjectDocument>(*document_)
            : std::nullopt;
    const std::string original_active_uuid = active_layer_uuid_;
    const auto original_solo_uuid = solo_layer_uuid_;
    const auto original_solo_group_uuid = solo_group_uuid_;
    const auto original_selected_group_uuid = selected_group_uuid_;
    const bool original_baseline_dirty = baseline_dirty_;
    const bool original_undo_dirty = undo_stack_ != nullptr && !undo_stack_->isClean();
    const bool original_dither_preference = integer_dither_preference_;
    const QString original_project_path = current_project_path_;
    const QString original_legacy_path = imported_legacy_path_;
    const QString original_dialog_directory = last_dialog_directory_;
    const QString original_status = status_ != nullptr ? status_->text() : QString();
    QSettings smoke_settings;
    const QString recent_entries_key = QStringLiteral("recentProjects/entries");
    const bool had_recent_entries = smoke_settings.contains(recent_entries_key);
    const QVariant original_recent_entries = smoke_settings.value(recent_entries_key);
    ScopeExit restore_state([this, original_project, original_document,
                             original_active_uuid, original_solo_uuid,
                             original_solo_group_uuid,
                             original_selected_group_uuid,
                             original_baseline_dirty, original_undo_dirty,
                             original_dither_preference, original_project_path,
                             original_legacy_path, original_dialog_directory,
                             original_status, recent_entries_key,
                             had_recent_entries, original_recent_entries] {
        preview_test_delay_ms_ = 0;
        independent_copy_test_path_.clear();
        if (playback_timer_ != nullptr) playback_timer_->stop();
        project_ = original_project;
        document_ = original_document
                        ? std::make_unique<pvt::ProjectDocument>(*original_document)
                        : nullptr;
        active_layer_uuid_ = original_active_uuid;
        solo_layer_uuid_ = original_solo_uuid;
        solo_group_uuid_ = original_solo_group_uuid;
        selected_group_uuid_ = original_selected_group_uuid;
        baseline_dirty_ = original_baseline_dirty || original_undo_dirty;
        integer_dither_preference_ = original_dither_preference;
        current_project_path_ = original_project_path;
        imported_legacy_path_ = original_legacy_path;
        last_dialog_directory_ = original_dialog_directory;
        wave_drag_state_.reset();
        swing_drag_state_.reset();
        effect_drag_state_.reset();
        loadActiveConfiguration();
        if (undo_stack_ != nullptr) {
            clearUndoHistory(false);
            undo_stack_->setClean();
        }
        ++document_revision_;
        updateCompatibilityWarning();
        refreshLayerList();
        refreshAll();
        refreshVersionsPage();
        updateWindowTitle();
        QSettings settings;
        if (had_recent_entries) {
            settings.setValue(recent_entries_key, original_recent_entries);
        } else {
            settings.remove(recent_entries_key);
        }
        refreshRecentProjectsMenu();
        if (status_ != nullptr) status_->setText(original_status);
        schedulePreview();
    });

    std::unordered_set<int> categorized_effect_types;
    int categorized_effect_entries = 0;
    bool categorized_effects_start_on_texture = true;
    QComboBox effect_catalog_probe;
    for (int category = 0; category < EffectUiCategoryCount; ++category) {
        populate_effect_types(&effect_catalog_probe, category);
        for (int index = 0; index < effect_catalog_probe.count(); ++index) {
            const int value = effect_catalog_probe.itemData(index).toInt();
            categorized_effect_types.insert(value);
            ++categorized_effect_entries;
            categorized_effects_start_on_texture =
                categorized_effects_start_on_texture
                && new_effect_for_ui(static_cast<pvt::EffectType>(value)).space
                       == pvt::EffectSpace::Texture;
        }
    }
    const bool effect_catalog_complete =
        categorized_effect_entries == 13
        && categorized_effect_types.size() == 13U
        && categorized_effects_start_on_texture;

    auto* const starting_colors_help = findChild<QLabel*>(
        QStringLiteral("startingColorsHelp"));
    if (starting_colors_help != nullptr) {
        starting_colors_help->resize(420, starting_colors_help->height());
        update_wrapped_label_height(starting_colors_help);
    }
    const int starting_colors_help_width = starting_colors_help != nullptr
        ? starting_colors_help->contentsRect().width() : 0;
    const int starting_colors_help_height =
        starting_colors_help != nullptr && starting_colors_help_width > 0
        ? starting_colors_help->heightForWidth(starting_colors_help_width) : 0;

    if (tabs_ == nullptr || tabs_->count() != 9
        || !tabs_->tabBar()->isHidden()
        || tabs_->indexOf(source_page_) < 0
        || tabs_->indexOf(effect_page_) < 0
        || tabs_->indexOf(surface_page_) < 0
        || tabs_->indexOf(motion_page_) < 0
        || tabs_->indexOf(finish_page_) < 0
        || tabs_->indexOf(project_canvas_page_) < 0
        || tabs_->indexOf(project_sync_page_) < 0
        || tabs_->indexOf(project_export_page_) < 0
        || tabs_->indexOf(history_page_) < 0
        || workflow_stage_buttons_.size() != 7U
        || workflow_stage_buttons_[0]->text() != tr("Project")
        || workflow_stage_buttons_[1]->text() != tr("Starting Colors")
        || workflow_stage_buttons_[2]->text() != tr("Modifiers")
        || workflow_stage_buttons_[3]->text() != tr("Movement")
        || workflow_stage_buttons_[4]->text() != tr("Layer Effects")
        || workflow_stage_buttons_[5]->text() != tr("Post Effects")
        || workflow_stage_buttons_[6]->text() != tr("Export")
        || wave_page_ == nullptr || !motion_page_->isAncestorOf(wave_page_)
        || effect_category_tabs_ == nullptr
        || effect_category_tabs_->count() != EffectUiCategoryCount
        || !effect_catalog_complete
        || add_effect_type_->count() != 6
        || static_cast<pvt::EffectType>(
               add_effect_type_->itemData(0).toInt())
               != pvt::EffectType::EndlessZoom
        || export_canvas_summary_ == nullptr
        || export_settings_action_ == nullptr
        || synchronization_page_ == nullptr
        || drivers_group_ == nullptr
        || !drivers_group_->isAncestorOf(synchronization_page_)
        || layer_clock_group_ == nullptr
        || layer_music_cancel_ == nullptr
        || layer_clock_group_->isAncestorOf(layer_music_cancel_)
        || drivers_expand_button_ == nullptr
        || driver_project_summary_ == nullptr
        || driver_layer_summary_ == nullptr
        || driver_swing_summary_ == nullptr
        || driver_audio_summary_ == nullptr
        || project_canvas_button_ == nullptr
        || project_sync_button_ == nullptr
        || project_export_button_ == nullptr
        || project_history_button_ == nullptr
        || swings_group_ == nullptr || swings_group_->parentWidget() == nullptr
        || project_audio_response_group_ == nullptr
        || audio_response_group_ == nullptr
        || audio_response_effective_ == nullptr
        || audio_copy_project_ == nullptr
        || wave_sync_ == nullptr
        || wave_sync_->text() != tr("Use synchronized clock")
        || wave_form_ == nullptr
        || effect_sync_ == nullptr
        || effect_sync_->text() != tr("Synchronization")
        || effect_blur_type_ == nullptr || effect_blur_type_->count() != 5
        || effect_particle_shape_ == nullptr
        || effect_particle_shape_->count() != 5
        || effect_particle_profile_ == nullptr
        || effect_particle_profile_->count() != 2
        || effect_particle_orientation_ == nullptr
        || effect_particle_orientation_->count() != 3
        || effect_particle_size_scale_ == nullptr
        || effect_particle_size_variation_ == nullptr
        || effect_particle_definition_ == nullptr
        || effect_particle_twinkle_ == nullptr
        || effect_particle_seed_ == nullptr
        || effect_particle_reseed_ == nullptr
        || effect_particle_rotation_ == nullptr
        || effect_blur_passes_ == nullptr || effect_blur_samples_ == nullptr
        || effect_blur_minimum_ == nullptr || effect_blur_maximum_ == nullptr
        || starting_color_mode_ == nullptr
        || starting_color_mode_->findData(
               static_cast<int>(pvt::StartingColorMode::SpiralRainbow)) < 0
        || starting_color_mode_->findData(
               static_cast<int>(pvt::StartingColorMode::SquareSpiralRainbow)) < 0
        || starting_color_include_alpha_ == nullptr
        || starting_colors_help == nullptr
        || !starting_colors_help->wordWrap()
        || !starting_colors_help->sizePolicy().hasHeightForWidth()
        || starting_colors_help_height <= 0
        || starting_colors_help->minimumHeight()
               < starting_colors_help_height
        || alpha_use_source_ == nullptr
        || post_invert_rgb_enabled_ == nullptr
        || post_invert_rgb_mix_ == nullptr
        || post_invert_alpha_enabled_ == nullptr
        || post_invert_alpha_mix_ == nullptr
        || post_antialias_enabled_ == nullptr
        || post_antialias_strength_ == nullptr
        || post_antialias_threshold_ == nullptr
        || post_antialias_passes_ == nullptr
        || post_invert_rgb_mix_->minimum() != 0.0
        || post_invert_rgb_mix_->maximum() != 1.0
        || post_invert_alpha_mix_->minimum() != 0.0
        || post_invert_alpha_mix_->maximum() != 1.0
        || post_antialias_strength_->minimum() != 0.0
        || post_antialias_strength_->maximum() != 1.0
        || post_antialias_threshold_->minimum() != 0.0
        || post_antialias_threshold_->maximum() != 1.0
        || post_antialias_passes_->minimum() != 1
        || post_antialias_passes_->maximum() != kMaximumIntegerParameter
        || wave_audio_response_ == nullptr
        || effect_audio_response_ == nullptr
        || wave_audio_response_->count() < 13
        || effect_audio_response_->count() != wave_audio_response_->count()
        || wave_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Default)) < 0
        || wave_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Beat)) < 0
        || wave_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Energy)) < 0
        || wave_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Enabled)) < 0
        || wave_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Disabled)) < 0
        || wave_audio_response_->itemText(
               wave_audio_response_->findData(
                   static_cast<int>(pvt::AudioResponseMode::Enabled)))
               != tr("Profile source (force this item on)")
        || effect_audio_response_->findData(
               static_cast<int>(pvt::AudioResponseMode::Beat)) < 0
        || wave_audio_response_->itemData(
               wave_audio_response_->findData(
                   static_cast<int>(pvt::AudioResponseMode::Beat)),
               Qt::ToolTipRole).toString().isEmpty()
        || wave_audio_response_->toolTip().isEmpty()
        || effect_audio_response_->toolTip().isEmpty()
        || project_audio_response_group_->toolTip().isEmpty()
        || audio_response_group_->toolTip().isEmpty()
        || audio_wave_source_->count() < 6
        || audio_wave_source_->count() != audio_effect_source_->count()
        || audio_wave_source_->count() != audio_color_source_->count()) {
        if (error != nullptr) {
            *error = tr("The Flow Workbench stages, persistent Drivers strip, or project settings navigation were not constructed correctly.");
        }
        return false;
    }

    auto* const finish_scroll = qobject_cast<QScrollArea*>(finish_page_);
    auto* const finish_contents = finish_scroll != nullptr
                                      ? finish_scroll->widget() : nullptr;
    auto* const finish_groups = finish_contents != nullptr
                                    ? qobject_cast<QVBoxLayout*>(
                                          finish_contents->layout())
                                    : nullptr;
    QWidget* const post_process_group =
        post_invert_rgb_enabled_->parentWidget();
    QWidget* const quantization_group = quantization_enabled_->parentWidget();
    if (finish_groups == nullptr || post_process_group == nullptr
        || quantization_group == nullptr
        || finish_groups->indexOf(post_process_group) < 0
        || finish_groups->indexOf(quantization_group) < 0
        || finish_groups->indexOf(post_process_group)
               >= finish_groups->indexOf(quantization_group)) {
        if (error != nullptr) {
            *error = tr("Post effects are not arranged in renderer order before quantization.");
        }
        return false;
    }

    bool bypass_dependencies = false;
    bool active_dependencies = false;
    {
        const QSignalBlocker rgb_blocker(post_invert_rgb_enabled_);
        const QSignalBlocker alpha_blocker(post_invert_alpha_enabled_);
        const QSignalBlocker antialias_blocker(post_antialias_enabled_);
        post_invert_rgb_enabled_->setChecked(false);
        post_invert_alpha_enabled_->setChecked(false);
        post_antialias_enabled_->setChecked(false);
        updatePostProcessEditorState();
        bypass_dependencies = !post_invert_rgb_mix_->isEnabled()
                              && !post_invert_alpha_mix_->isEnabled()
                              && !post_antialias_strength_->isEnabled()
                              && !post_antialias_threshold_->isEnabled()
                              && !post_antialias_passes_->isEnabled();
        post_invert_rgb_enabled_->setChecked(true);
        post_invert_alpha_enabled_->setChecked(true);
        post_antialias_enabled_->setChecked(true);
        updatePostProcessEditorState();
        active_dependencies = post_invert_rgb_mix_->isEnabled()
                              && post_invert_alpha_mix_->isEnabled()
                              && post_antialias_strength_->isEnabled()
                              && post_antialias_threshold_->isEnabled()
                              && post_antialias_passes_->isEnabled();
    }
    loadGlobalEditors();
    pvt::RenderConfig alpha_invert_probe = pvt::default_config();
    alpha_invert_probe.post_process.invert_alpha_enabled = true;
    if (!bypass_dependencies || !active_dependencies
        || !configuration_requires_alpha(alpha_invert_probe)) {
        if (error != nullptr) {
            *error = tr("Post-effect controls, dependencies, or alpha-output safety are not wired correctly.");
        }
        return false;
    }

    pvt::ProjectConfig preview_scale_probe = pvt::default_project();
    preview_scale_probe.canvas.width = 3840;
    preview_scale_probe.canvas.height = 2160;
    preview_scale_probe.canvas.block_size = 16;
    preview_scale_probe.layers.front().render.displacement = 40.0;
    preview_scale_probe.layers.front().render.effects.clear();
    auto preview_particle = pvt::default_effect(pvt::EffectType::ParticleField);
    preview_particle.radius_pixels = 40.0;
    auto preview_glow = pvt::default_effect(pvt::EffectType::Glow);
    preview_glow.radius_pixels = 20.0;
    auto preview_blur = pvt::default_effect(pvt::EffectType::Blur);
    preview_blur.radius_pixels = 12.0;
    preview_scale_probe.layers.front().render.effects = {
        preview_particle, preview_glow, preview_blur};
    scale_project_for_preview(preview_scale_probe);
    if (preview_scale_probe.canvas.width != 720
        || preview_scale_probe.canvas.height != 405
        || preview_scale_probe.canvas.block_size != 3
        || std::abs(preview_scale_probe.layers.front().render.displacement
                    - 7.5) > 1.0e-12
        || std::abs(preview_scale_probe.layers.front().render.effects[0U]
                        .radius_pixels - 7.5) > 1.0e-12
        || std::abs(preview_scale_probe.layers.front().render.effects[1U]
                        .radius_pixels - 3.75) > 1.0e-12) {
        if (error != nullptr) {
            *error = tr("Output-pixel controls were not scaled consistently for preview rendering.");
        }
        return false;
    }
    if (std::abs(preview_scale_probe.layers.front().render.effects[2U]
                     .radius_pixels - 2.25) > 1.0e-12) {
        if (error != nullptr) {
            *error = tr("Output-pixel controls were not scaled consistently for preview rendering.");
        }
        return false;
    }

    const int previous_effect_category = effect_category_filter_;
    bool effect_ranges_valid = true;
    bool particle_ranges_valid = false;
    bool glow_ranges_valid = false;
    bool blur_ranges_valid = false;
    bool block_ranges_valid = false;
    {
        const QSignalBlocker type_blocker(effect_type_);
        const QSignalBlocker intensity_blocker(effect_intensity_);
        const QSignalBlocker magnitude_blocker(effect_magnitude_);
        const QSignalBlocker frequency_blocker(effect_frequency_);
        const QSignalBlocker secondary_blocker(effect_secondary_);
        const QSignalBlocker radius_blocker(effect_radius_);
        const QSignalBlocker threshold_blocker(effect_threshold_);
        populate_effect_types(effect_type_, ParticleEffects);
        const int particle_type = effect_type_->findData(
            static_cast<int>(pvt::EffectType::ParticleField));
        effect_type_->setCurrentIndex(particle_type);
        updateEffectEditorVisibility();
        particle_ranges_valid = particle_type >= 0
                                && effect_radius_->minimum() > 0.0
                                && effect_threshold_->minimum() == 0.0
                                && effect_threshold_->maximum() == 1.0;

        populate_effect_types(effect_type_, LightAndEnergyEffects);
        const int glow_type = effect_type_->findData(
            static_cast<int>(pvt::EffectType::Glow));
        effect_type_->setCurrentIndex(glow_type);
        updateEffectEditorVisibility();
        glow_ranges_valid = glow_type >= 0
                            && effect_radius_->minimum() == 0.0
                            && std::abs(effect_threshold_->maximum()
                                        - kMaximumRenderParameter)
                                   < 0.0001;

        populate_effect_types(effect_type_, BlurEffects);
        const int blur_type = effect_type_->findData(
            static_cast<int>(pvt::EffectType::Blur));
        effect_type_->setCurrentIndex(blur_type);
        updateEffectEditorVisibility();
        blur_ranges_valid = blur_type >= 0
                            && effect_radius_->minimum() == 0.0
                            && effect_blur_passes_->minimum() == 1
                            && effect_blur_passes_->maximum()
                                   == kMaximumIntegerParameter
                            && effect_blur_samples_->minimum() == 2
                            && effect_blur_samples_->maximum()
                                   == kMaximumIntegerParameter;
        double block_scale_maximum = 1.5;
        synchronize_block_scale_maximum_editor(
            effect_frequency_, 2.0, block_scale_maximum);
        block_ranges_valid = effect_frequency_->minimum() == 2.0
                             && effect_frequency_->value() == 2.0
                             && block_scale_maximum == 2.0;
        synchronize_block_scale_maximum_editor(
            effect_frequency_, 0.25, block_scale_maximum);
        block_ranges_valid = block_ranges_valid
                             && effect_frequency_->minimum() == 0.25;
        effect_ranges_valid = particle_ranges_valid && glow_ranges_valid
                              && blur_ranges_valid && block_ranges_valid;
    }
    setEffectCategory(previous_effect_category);
    if (!effect_ranges_valid) {
        if (error != nullptr) {
            *error = tr("Effect editor ranges do not match validation (particle %1, glow %2, blur %3, block %4).")
                .arg(particle_ranges_valid)
                .arg(glow_ranges_valid)
                .arg(blur_ranges_valid)
                .arg(block_ranges_valid);
        }
        return false;
    }

    const pvt::ProjectConfig synchronization_project = project_;
    const pvt::RenderConfig synchronization_config = config_;
    const std::string synchronization_layer = active_layer_uuid_;

    // Exercise the GUI-only Mic sentinel through the same signal path a user
    // takes, but detach the companion temporarily so this remains a
    // hardware-free smoke test. The authored enum must stay deterministic,
    // the export gate must refuse admission, and the first accepted selection
    // must be one undo transaction.
    bool mic_sentinel_valid = true;
    QString mic_sentinel_detail;
    LiveWorkspace* const smoke_live_workspace = live_workspace_;
    const bool smoke_export_active = export_active_;
    live_workspace_ = nullptr;
    for (auto& route : config_.live.clock_inputs) {
        if (route.source == pvt::LiveClockInputSource::AudioStream) {
            route.enabled = false;
        }
    }
    config_.clock.mode = pvt::ClockMode::Frame;
    syncActiveRender();
    syncProjectGlobals();
    loadGlobalEditors();
    const int mic_sentinel_index = clock_mode_->findData(
        kMicLiveClockSentinel);
    const int mic_undo_before = undo_stack_->count();
    const int mic_undo_index_before = undo_stack_->index();
    if (mic_sentinel_index < 0) {
        mic_sentinel_valid = false;
        mic_sentinel_detail = tr("the project Mic sentinel is missing");
    } else {
        export_active_ = true;
        clock_mode_->setCurrentIndex(mic_sentinel_index);
        export_active_ = smoke_export_active;
        mic_sentinel_valid = standardMicRoute(false) == nullptr
            && config_.clock.mode == pvt::ClockMode::Frame
            && clock_mode_->currentData().toInt()
                   == static_cast<int>(pvt::ClockMode::Frame)
            && undo_stack_->count() == mic_undo_before
            && undo_stack_->index() == mic_undo_index_before;
        if (!mic_sentinel_valid) {
            mic_sentinel_detail = tr(
                "the export admission guard changed a clock or undo state");
        }
    }
    if (mic_sentinel_valid) {
        clock_mode_->setCurrentIndex(mic_sentinel_index);
        const auto* project_route = standardMicRoute(false);
        mic_sentinel_valid = project_route != nullptr
            && config_.clock.mode == pvt::ClockMode::Frame
            && config_.live.enabled
            && config_.audio_reactive_defaults.enabled
            && clock_mode_->currentData().toInt() == kMicLiveClockSentinel
            && undo_stack_->count() == mic_undo_before + 1
            && undo_stack_->index() == mic_undo_index_before + 1;
        if (!mic_sentinel_valid) {
            mic_sentinel_detail = tr(
                "first Mic selection did not preserve the authored clock or create exactly one undo entry");
        } else {
            const std::string project_role = project_route->endpoint_uuid;
            const bool layer_enabled_before = config_.layer_clock.enabled;
            const pvt::ClockMode layer_mode_before =
                config_.layer_clock.clock.mode;
            QString route_error;
            const auto layer_role = ensureStandardMicRoute(
                true, &route_error);
            syncActiveRender();
            syncProjectGlobals();
            mic_sentinel_valid = layer_role.has_value()
                && *layer_role == project_role
                && standardMicRoute(true) != nullptr
                && config_.layer_clock.enabled == layer_enabled_before
                && config_.layer_clock.clock.mode == layer_mode_before
                && pvt::validate(project_).ok;
            if (!mic_sentinel_valid) {
                mic_sentinel_detail = route_error.isEmpty()
                    ? tr("project/layer Mic routes did not share one role or altered the offline layer clock")
                    : route_error;
            } else {
                const int deterministic_index = clock_mode_->findData(
                    static_cast<int>(pvt::ClockMode::Default));
                clock_mode_->setCurrentIndex(deterministic_index);
                mic_sentinel_valid = standardMicRoute(false) == nullptr
                    && standardMicRoute(true) != nullptr
                    && config_.clock.mode == pvt::ClockMode::Default
                    && pvt::validate(project_).ok;
                if (!mic_sentinel_valid) {
                    mic_sentinel_detail = tr(
                        "a deterministic project clock removed the wrong Live route or produced an invalid project");
                }
            }
        }
    }
    export_active_ = smoke_export_active;
    live_workspace_ = smoke_live_workspace;

    // The layer-deletion invariant is pure and hardware free: removing a
    // layer must remove every clock input and MIDI output that names it while
    // preserving project-wide routes. Validate a complete project before and
    // after the synthetic deletion when the layer bound permits it.
    bool layer_route_cleanup_valid = true;
    if (synchronization_project.layers.size() < pvt::kMaximumLayers) {
        pvt::ProjectConfig deletion = synchronization_project;
        pvt::LayerConfig disposable = deletion.layers.front();
        disposable.uuid = pvt::generate_uuid();
        disposable.file_id = pvt::allocate_layer_file_id(deletion);
        disposable.group_uuid.clear();
        disposable.name = "Live route cleanup smoke layer";
        const std::string deleted_uuid = disposable.uuid;
        deletion.layers.push_back(std::move(disposable));
        deletion.canvas.live = {};
        deletion.canvas.live.enabled = true;

        pvt::LiveEndpointConfig audio_endpoint;
        audio_endpoint.uuid = pvt::generate_uuid();
        audio_endpoint.name = "Smoke audio input";
        audio_endpoint.protocol = pvt::LiveEndpointProtocol::Audio;
        audio_endpoint.direction = pvt::LiveEndpointDirection::Input;
        pvt::LiveEndpointConfig midi_endpoint;
        midi_endpoint.uuid = pvt::generate_uuid();
        midi_endpoint.name = "Smoke layer MIDI output";
        midi_endpoint.protocol = pvt::LiveEndpointProtocol::Midi;
        midi_endpoint.direction = pvt::LiveEndpointDirection::Output;
        pvt::LiveEndpointConfig project_midi_endpoint = midi_endpoint;
        project_midi_endpoint.uuid = pvt::generate_uuid();
        project_midi_endpoint.name = "Smoke project MIDI output";
        deletion.canvas.live.endpoints = {
            audio_endpoint, midi_endpoint, project_midi_endpoint};

        pvt::LiveClockInputConfig layer_input;
        layer_input.enabled = true;
        layer_input.target = pvt::LiveClockTarget::Layer;
        layer_input.layer_uuid = deleted_uuid;
        layer_input.source = pvt::LiveClockInputSource::AudioStream;
        layer_input.endpoint_uuid = audio_endpoint.uuid;
        pvt::LiveClockInputConfig project_input = layer_input;
        project_input.target = pvt::LiveClockTarget::Project;
        project_input.layer_uuid.clear();
        pvt::LiveMidiClockOutputConfig layer_output;
        layer_output.enabled = true;
        layer_output.source = pvt::LiveClockTarget::Layer;
        layer_output.layer_uuid = deleted_uuid;
        layer_output.endpoint_uuid = midi_endpoint.uuid;
        pvt::LiveMidiClockOutputConfig project_output = layer_output;
        project_output.source = pvt::LiveClockTarget::Project;
        project_output.layer_uuid.clear();
        project_output.endpoint_uuid = project_midi_endpoint.uuid;
        deletion.canvas.live.clock_inputs = {layer_input, project_input};
        deletion.canvas.live.midi_clock_outputs = {
            layer_output, project_output};

        const bool valid_before_cleanup = pvt::validate(deletion).ok;
        remove_layer_live_clock_routes(deletion.canvas.live, deleted_uuid);
        deletion.layers.pop_back();
        const bool kept_project_input =
            deletion.canvas.live.clock_inputs.size() == 1U
            && deletion.canvas.live.clock_inputs.front().target
                   == pvt::LiveClockTarget::Project;
        const bool kept_project_output =
            deletion.canvas.live.midi_clock_outputs.size() == 1U
            && deletion.canvas.live.midi_clock_outputs.front().source
                   == pvt::LiveClockTarget::Project;
        layer_route_cleanup_valid = valid_before_cleanup
            && kept_project_input && kept_project_output
            && pvt::validate(deletion).ok;
    }

    project_ = synchronization_project;
    config_ = synchronization_config;
    active_layer_uuid_ = synchronization_layer;
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    if (!mic_sentinel_valid || !layer_route_cleanup_valid) {
        if (error != nullptr) {
            *error = !mic_sentinel_valid
                ? tr("Mic clock smoke failed: %1").arg(mic_sentinel_detail)
                : tr("Deleting a layer did not clean its Live clock routes without disturbing project routes.");
        }
        return false;
    }

    const auto set_clock_mode_for_smoke = [this](pvt::ClockMode mode) {
        config_.clock.mode = mode;
        const QSignalBlocker blocker(clock_mode_);
        select_enum(clock_mode_, mode);
        updateSynchronizationState();
    };
    config_.layer_clock.enabled = false;
    set_clock_mode_for_smoke(pvt::ClockMode::Default);
    if (clock_interpolation_->isEnabled()
        || !clock_frame_interval_->isHidden()
        || !clock_time_interval_ms_->isHidden()
        || !meter_expression_->isHidden()
        || music_choose_->isEnabled()
        || !project_audio_response_group_->isHidden()
        || !audio_response_group_->isHidden()
        || !audio_response_effective_->isHidden()
        || !audio_copy_project_->isHidden()
        || wave_form_->isRowVisible(wave_audio_response_)
        || effect_form_->isRowVisible(effect_audio_response_)) {
        if (error != nullptr) {
            *error = tr("Default clock mode did not hide pulse-only and music-only controls.");
        }
        return false;
    }
    set_clock_mode_for_smoke(pvt::ClockMode::Frame);
    if (clock_frame_interval_->isHidden()
        || !clock_time_interval_ms_->isHidden()
        || !meter_expression_->isHidden()) {
        if (error != nullptr) {
            *error = tr("Frame clock mode did not expose only its frame interval.");
        }
        return false;
    }
    set_clock_mode_for_smoke(pvt::ClockMode::Time);
    if (!clock_frame_interval_->isHidden()
        || clock_time_interval_ms_->isHidden()
        || !meter_expression_->isHidden()) {
        if (error != nullptr) {
            *error = tr("Time clock mode did not expose only its elapsed-time interval.");
        }
        return false;
    }
    set_clock_mode_for_smoke(pvt::ClockMode::Meter);
    if (!clock_frame_interval_->isHidden()
        || !clock_time_interval_ms_->isHidden()
        || meter_expression_->isHidden() || meter_bpm_->isHidden()
        || meter_tempo_note_->isHidden()) {
        if (error != nullptr) {
            *error = tr("Meter clock mode did not expose its meter and tempo controls.");
        }
        return false;
    }
    set_clock_mode_for_smoke(pvt::ClockMode::Music);
    if (project_audio_response_group_->isHidden()
        || audio_response_group_->isHidden()
        || audio_response_effective_->isHidden()
        || audio_copy_project_->isHidden()
        || !wave_form_->isRowVisible(wave_audio_response_)
        || !effect_form_->isRowVisible(effect_audio_response_)) {
        if (error != nullptr) {
            *error = tr("Music clock mode did not expose Audio Response controls.");
        }
        return false;
    }
    config_.layer_clock.enabled = true;
    config_.layer_clock.clock.mode = pvt::ClockMode::Frame;
    updateSynchronizationState();
    if (!project_audio_response_group_->isHidden()
        || !audio_response_group_->isHidden()
        || wave_form_->isRowVisible(wave_audio_response_)
        || effect_form_->isRowVisible(effect_audio_response_)) {
        if (error != nullptr) {
            *error = tr("A non-Music active-layer clock did not hide Audio Response controls.");
        }
        return false;
    }
    config_.clock.mode = pvt::ClockMode::Default;
    config_.layer_clock.clock.mode = pvt::ClockMode::Music;
    updateSynchronizationState();
    if (project_audio_response_group_->isHidden()
        || audio_response_group_->isHidden()
        || !wave_form_->isRowVisible(wave_audio_response_)
        || !effect_form_->isRowVisible(effect_audio_response_)) {
        if (error != nullptr) {
            *error = tr("A Music active-layer override did not expose Audio Response controls.");
        }
        return false;
    }
    config_.layer_clock.enabled = false;

    pvt::MusicAnalysis synthetic_music;
    synthetic_music.analyzer_version = "gui-smoke-v1";
    synthetic_music.source_sha256 = std::string(64U, 'a');
    synthetic_music.source_basename = "synthetic.wav";
    synthetic_music.source_format = "WAV float32";
    synthetic_music.source_frame_count = 60000U;
    synthetic_music.source_sample_rate = 48000U;
    synthetic_music.source_channel_count = 2U;
    synthetic_music.duration_seconds = 1.25;
    synthetic_music.detected_bpm = 120.0;
    synthetic_music.tempo_confidence = 0.8;
    synthetic_music.beat_times_seconds = {0.0, 0.5, 1.0};
    synthetic_music.tempo_points = {{0.0, 120.0, 0.8}};
    synthetic_music.feature_samples.resize(4U);
    config_.total_frames = 17;
    config_.fps = 24.0;
    config_.clock.music = synthetic_music;
    config_.clock.mode = pvt::ClockMode::Music;
    syncProjectGlobals();
    loadGlobalEditors();
    updateTimelineState();
    if (effectiveFrameCount() != 30 || frames_->value() != 17
        || frames_->isEnabled() || timeline_->maximum() != 29
        || !frame_label_->text().contains(QStringLiteral("/ 30"))
        || !music_choose_->isEnabled()
        || !previous_beat_->isEnabled() || !next_beat_->isEnabled()) {
        if (error != nullptr) {
            *error = tr("Render-ready Music did not preserve manual frames while deriving the exact timeline length.");
        }
        return false;
    }
    timeline_->setValue(0);
    navigateToBeat(1);
    if (timeline_->value() != 12
        || !frame_label_->text().contains(tr("Beat 2"))) {
        if (error != nullptr) {
            *error = tr("Music beat navigation or the time/beat readout was incorrect.");
        }
        return false;
    }

    // A first successful project import enables the shared default so all
    // inheriting layers become useful without creating surprise overrides.
    // Once the user turns that default off, ordinary clock-mode changes and
    // replacement imports must preserve the explicit choice.
    config_.clock.music = {};
    config_.audio_reactive.enabled = false;
    config_.audio_reactive_override_enabled = false;
    config_.audio_reactive_defaults.enabled = false;
    syncActiveRender();
    syncProjectGlobals();
    MusicAnalysisResult imported_result;
    imported_result.ok = true;
    imported_result.analysis = synthetic_music;
    imported_result.analysis.source_sha256 = std::string(64U, 'b');
    imported_result.action = MusicAnalysisAction::Choose;
    imported_result.generation = music_analysis_generation_;
    imported_result.document_revision = document_revision_;
    imported_result.attached.sha256 = imported_result.analysis.source_sha256;
    imported_result.attached.basename = imported_result.analysis.source_basename;
    imported_result.staged_document =
        std::make_shared<pvt::ProjectDocument>(*document_);
    finishMusicAnalysis(imported_result);
    if (config_.clock.mode != pvt::ClockMode::Music
        || !config_.audio_reactive_defaults.enabled
        || config_.audio_reactive_override_enabled
        || activeLayer() == nullptr
        || activeLayer()->render.audio_reactive_override_enabled
        || !project_audio_response_group_->isChecked()
        || audio_response_group_->isChecked()
        || !audio_response_effective_->text().contains(
            tr("project-wide defaults"), Qt::CaseInsensitive)
        || !audio_copy_project_->isEnabled()) {
        if (error != nullptr) {
            *error = tr("A first music import did not enable inheritable project-wide Audio Response correctly.");
        }
        return false;
    }
    project_audio_response_group_->setChecked(false);
    const int default_clock = clock_mode_->findData(
        static_cast<int>(pvt::ClockMode::Default));
    const int music_clock = clock_mode_->findData(
        static_cast<int>(pvt::ClockMode::Music));
    clock_mode_->setCurrentIndex(default_clock);
    clock_mode_->setCurrentIndex(music_clock);
    if (config_.audio_reactive_defaults.enabled
        || project_audio_response_group_->isChecked()
        || config_.audio_reactive_override_enabled) {
        if (error != nullptr) {
            *error = tr("Returning to Music overrode the user's Audio Response choice.");
        }
        return false;
    }
    MusicAnalysisResult replacement_result = imported_result;
    replacement_result.analysis.source_sha256 = std::string(64U, 'c');
    replacement_result.attached.sha256 = replacement_result.analysis.source_sha256;
    replacement_result.generation = music_analysis_generation_;
    replacement_result.document_revision = document_revision_;
    replacement_result.staged_document =
        std::make_shared<pvt::ProjectDocument>(*document_);
    finishMusicAnalysis(replacement_result);
    if (config_.audio_reactive_defaults.enabled
        || project_audio_response_group_->isChecked()
        || config_.audio_reactive_override_enabled) {
        if (error != nullptr) {
            *error = tr("Replacing music overrode the user's Audio Response choice.");
        }
        return false;
    }

    const std::string music_digest_before_stale =
        config_.clock.music.source_sha256;
    MusicAnalysisResult stale_result;
    stale_result.ok = true;
    stale_result.analysis = synthetic_music;
    stale_result.analysis.source_sha256 = std::string(64U, 'b');
    stale_result.action = MusicAnalysisAction::Choose;
    stale_result.generation = music_analysis_generation_ + 1U;
    stale_result.document_revision = document_revision_;
    finishMusicAnalysis(stale_result);
    stale_result.generation = music_analysis_generation_;
    stale_result.document_revision = document_revision_ + 1U;
    finishMusicAnalysis(stale_result);
    if (config_.clock.music.source_sha256 != music_digest_before_stale) {
        if (error != nullptr) {
            *error = tr("A stale music-analysis completion changed the active project.");
        }
        return false;
    }

    project_ = synchronization_project;
    config_ = synchronization_config;
    active_layer_uuid_ = synchronization_layer;
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();

    const bool original_swings_enabled = config_.swings_enabled;
    swings_group_->setChecked(!original_swings_enabled);
    if (config_.swings_enabled == original_swings_enabled
        || activeLayer() == nullptr
        || activeLayer()->render.swings_enabled != config_.swings_enabled) {
        if (error != nullptr) {
            *error = tr("The active-layer Swings master toggle was not synchronized.");
        }
        return false;
    }
    undo_stack_->undo();
    if (config_.swings_enabled != original_swings_enabled) {
        if (error != nullptr) {
            *error = tr("Undo did not restore the active-layer Swings master toggle.");
        }
        return false;
    }
    // Exercise Audio Response editing under the only effective clock where it
    // is valid, then restore the smoke document's original clock ownership.
    const pvt::ClockMode audio_test_project_clock = config_.clock.mode;
    const pvt::LayerClockConfig audio_test_layer_clock = config_.layer_clock;
    config_.clock.mode = pvt::ClockMode::Music;
    config_.layer_clock.enabled = false;
    syncActiveRender();
    syncProjectGlobals();
    updateSynchronizationState();

    const bool original_audio_override =
        config_.audio_reactive_override_enabled;
    audio_response_group_->setChecked(!original_audio_override);
    if (config_.audio_reactive_override_enabled == original_audio_override
        || activeLayer() == nullptr
        || activeLayer()->render.audio_reactive_override_enabled
               != config_.audio_reactive_override_enabled) {
        if (error != nullptr) {
            *error = tr("The active-layer Audio Response override toggle was not synchronized.");
        }
        return false;
    }
    undo_stack_->undo();
    if (config_.audio_reactive_override_enabled != original_audio_override) {
        if (error != nullptr) {
            *error = tr("Undo did not restore the active-layer Audio Response override toggle.");
        }
        return false;
    }

    const bool original_project_audio =
        config_.audio_reactive_defaults.enabled;
    project_audio_response_group_->setChecked(!original_project_audio);
    if (config_.audio_reactive_defaults.enabled == original_project_audio
        || project_.canvas.audio_reactive_defaults.enabled
               != config_.audio_reactive_defaults.enabled) {
        if (error != nullptr) {
            *error = tr("The project-wide Audio Response toggle was not synchronized.");
        }
        return false;
    }
    undo_stack_->undo();
    if (config_.audio_reactive_defaults.enabled != original_project_audio) {
        if (error != nullptr) {
            *error = tr("Undo did not restore the project-wide Audio Response toggle.");
        }
        return false;
    }

    const pvt::AudioReactiveConfig original_layer_audio =
        config_.audio_reactive;
    audio_copy_project_->click();
    if (!config_.audio_reactive_override_enabled
        || !audio_response_group_->isChecked()
        || config_.audio_reactive.enabled
               != config_.audio_reactive_defaults.enabled
        || config_.audio_reactive.wave_amount
               != config_.audio_reactive_defaults.wave_amount) {
        if (error != nullptr) {
            *error = tr("Copying project Audio Response into a layer override failed.");
        }
        return false;
    }
    undo_stack_->undo();
    if (config_.audio_reactive_override_enabled != original_audio_override
        || config_.audio_reactive.enabled != original_layer_audio.enabled
        || config_.audio_reactive.wave_amount
               != original_layer_audio.wave_amount) {
        if (error != nullptr) {
            *error = tr("Undo did not restore the pre-copy layer Audio Response state.");
        }
        return false;
    }

    // Per-item routing stays editable for free-running and synchronized items;
    // synchronization is an independent timing choice. The profile's explicit
    // synchronized-only switch remains available when that policy is wanted.
    if (const auto wave = selectedWaveIndex()) {
        const bool synchronized = config_.waves[*wave].synchronized;
        config_.waves[*wave].synchronized = false;
        loadSelectedWave();
        const bool enabled_when_free = wave_audio_response_->isEnabled();
        config_.waves[*wave].synchronized = true;
        loadSelectedWave();
        const bool enabled_when_synchronized =
            wave_audio_response_->isEnabled();
        config_.waves[*wave].synchronized = synchronized;
        loadSelectedWave();
        if (!enabled_when_free || !enabled_when_synchronized) {
            if (error != nullptr) {
                *error = tr("Wave Audio Response was artificially gated by synchronization state.");
            }
            return false;
        }
    }
    if (const auto effect = selectedEffectIndex()) {
        const bool synchronized = config_.effects[*effect].synchronized;
        config_.effects[*effect].synchronized = false;
        loadSelectedEffect();
        const bool enabled_when_free = effect_audio_response_->isEnabled();
        config_.effects[*effect].synchronized = true;
        loadSelectedEffect();
        const bool enabled_when_synchronized =
            effect_audio_response_->isEnabled();
        config_.effects[*effect].synchronized = synchronized;
        loadSelectedEffect();
        if (!enabled_when_free || !enabled_when_synchronized) {
            if (error != nullptr) {
                *error = tr("Effect Audio Response was artificially gated by synchronization state.");
            }
            return false;
        }
    }
    config_.clock.mode = audio_test_project_clock;
    config_.layer_clock = audio_test_layer_clock;
    syncActiveRender();
    syncProjectGlobals();
    updateSynchronizationState();

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
    if (!baseline_dirty_ || !hasUnsavedChanges() || !isWindowModified()
        || !undo_stack_->isClean() || !current_project_path_.isEmpty()
        || imported_legacy_path_ != path || document_ == nullptr
        || !document_->legacy_import || !config_.output.write_alpha) {
        if (error != nullptr) {
            *error = tr("Legacy import did not remain a dirty, Save-As-only project or lost its alpha-output flag.");
        }
        return false;
    }
    if (!windowTitle().startsWith(QString::fromStdString(project_.name))
        || !windowTitle().contains(tr("PVT"))) {
        if (error != nullptr) {
            *error = tr("The project name was not reflected in the window title.");
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
        || surface_obj_path_->text().toStdString() != expected.surface.obj_path
        || png_compression_->value() != expected.output.png_compression_level
        || !palette_enabled_->isChecked()
        || palette_name_->text().toStdString() != expected.palette.name
        || palette_colors_->count()
               != static_cast<int>(expected.palette.colors.size())
        || alpha_use_source_->isChecked()
        || !starting_color_include_alpha_->isChecked()
        || static_cast<pvt::StartingColorMode>(
               starting_color_mode_->currentData().toInt())
               != expected.starting_colors.mode
        || starting_red_minimum_->value()
               != expected.starting_colors.red_minimum
        || starting_red_maximum_->value()
               != expected.starting_colors.red_maximum
        || starting_alpha_minimum_->value()
               != expected.starting_colors.alpha_minimum
        || starting_alpha_maximum_->value()
               != expected.starting_colors.alpha_maximum
        || !starting_image_palette_dither_->isChecked()
        || static_cast<pvt::DitherMethod>(
               starting_image_palette_dither_method_->currentData().toInt())
               != expected.starting_image.palette_dither_method
        || !transform_flip_vertical_->isChecked()
        || static_cast<pvt::MirrorMode>(transform_mirror_->currentData().toInt())
               != expected.transform.mirror
        || (!expected.swings.empty()
            && (swing_center_x_->value() != expected.swings.front().center_x
                || swing_center_y_->value() != expected.swings.front().center_y
                || swing_radius_->value() != expected.swings.front().radius))
        || (!expected.effects.empty()
            && (effect_center_x_->value() != expected.effects.front().center_x
                || effect_center_y_->value() != expected.effects.front().center_y
                || effect_area_radius_->value()
                       != expected.effects.front().area_radius
                || static_cast<pvt::EffectSpace>(
                       effect_space_->currentData().toInt())
                       != expected.effects.front().space))) {
        if (error != nullptr) {
            *error = tr("GUI editors clamped values accepted by central validation.");
        }
        return false;
    }

    // Palette enablement is an independent source-stage toggle. Exercise the
    // widget, active layer synchronization, Undo/Redo, and preset application
    // directly so a future force-on regression cannot hide behind persistence.
    const int palette_undo_index = undo_stack_->index();
    palette_enabled_->setChecked(false);
    if (palette_enabled_->isChecked() || config_.palette.enabled
        || activeLayer() == nullptr || activeLayer()->render.palette.enabled
        || undo_stack_->index() != palette_undo_index + 1) {
        if (error != nullptr) {
            *error = tr("Turning off the starting palette did not update the active layer or Undo history.");
        }
        return false;
    }
    undo_stack_->undo();
    if (!palette_enabled_->isChecked() || !config_.palette.enabled
        || activeLayer() == nullptr || !activeLayer()->render.palette.enabled) {
        if (error != nullptr) {
            *error = tr("Undo did not restore starting-palette enablement.");
        }
        return false;
    }
    undo_stack_->redo();
    if (palette_enabled_->isChecked() || config_.palette.enabled
        || activeLayer() == nullptr || activeLayer()->render.palette.enabled) {
        if (error != nullptr) {
            *error = tr("Redo did not turn the starting palette back off.");
        }
        return false;
    }
    applyPalettePreset(3U);
    if (palette_enabled_->isChecked() || config_.palette.enabled
        || config_.palette.name != pvt::default_palette(3U).name) {
        if (error != nullptr) {
            *error = tr("Applying a preset silently re-enabled the starting palette.");
        }
        return false;
    }
    palette_enabled_->setChecked(true);
    if (!config_.palette.enabled || activeLayer() == nullptr
        || !activeLayer()->render.palette.enabled
        || undo_stack_->index() != palette_undo_index + 2) {
        if (error != nullptr) {
            *error = tr("Turning the starting palette back on did not create an independent Undo step.");
        }
        return false;
    }
    undo_stack_->undo();
    if (palette_enabled_->isChecked() || config_.palette.enabled
        || activeLayer() == nullptr || activeLayer()->render.palette.enabled) {
        if (error != nullptr) {
            *error = tr("Consecutive palette toggles merged into a phantom Undo step.");
        }
        return false;
    }
    undo_stack_->redo();
    if (!palette_enabled_->isChecked() || !config_.palette.enabled
        || activeLayer() == nullptr || !activeLayer()->render.palette.enabled) {
        if (error != nullptr) {
            *error = tr("Redo did not restore the independently recorded palette toggle.");
        }
        return false;
    }

    const bool expected_surface_enabled = config_.surface.enabled;
    surface_enabled_->setChecked(!expected_surface_enabled);
    if (config_.surface.enabled == expected_surface_enabled) {
        if (error != nullptr) {
            *error = tr("The surface enable toggle did not update its backing configuration.");
        }
        return false;
    }
    if (config_.surface.rotations_per_loop != expected.surface.rotations_per_loop
        || config_.surface.lighting != expected.surface.lighting
        || config_.ghost_lag_degrees != expected.ghost_lag_degrees) {
        if (error != nullptr) {
            *error = tr("Editing an unrelated surface control changed loaded values.");
        }
        return false;
    }
    surface_enabled_->setChecked(expected_surface_enabled);
    if (!expected.effects.empty()) {
        const bool expected_effect_enabled = config_.effects.front().enabled;
        effect_enabled_->setChecked(!expected_effect_enabled);
        if (config_.effects.front().enabled == expected_effect_enabled) {
            if (error != nullptr) {
                *error = tr("The effect enable toggle did not update its backing configuration.");
            }
            return false;
        }
        if (config_.effects.front().center_x != expected.effects.front().center_x
            || config_.effects.front().center_y != expected.effects.front().center_y
            || config_.effects.front().frequency != expected.effects.front().frequency) {
            if (error != nullptr) {
                *error = tr("Editing an unrelated effect control changed loaded values.");
            }
            return false;
        }
        effect_enabled_->setChecked(expected_effect_enabled);
    }
    if (!expected.waves.empty()) {
        const bool expected_wave_enabled = config_.waves.front().enabled;
        wave_enabled_->setChecked(!expected_wave_enabled);
        if (config_.waves.front().enabled == expected_wave_enabled) {
            if (error != nullptr) {
                *error = tr("The wave enable toggle did not update its backing configuration.");
            }
            return false;
        }
        if (config_.waves.front().x_percent != expected.waves.front().x_percent) {
            if (error != nullptr) {
                *error = tr("Editing an unrelated wave control changed loaded precision.");
            }
            return false;
        }
        wave_enabled_->setChecked(expected_wave_enabled);

        const bool expected_displacement = config_.displacement_enabled;
        const bool expected_lighting = config_.lighting_enabled;
        const bool restored_wave_enabled = config_.waves.front().enabled;
        config_.waves.front().enabled = true;
        config_.displacement_enabled = false;
        config_.lighting_enabled = false;
        updateWaveOutputState();
        if (wave_displacement_enabled_->isChecked()
            || wave_lighting_enabled_->isChecked()
            || !wave_output_status_->text().contains(
                tr("cannot affect pixels"))) {
            if (error != nullptr) {
                *error = tr("The Wave page did not expose its silent-output state.");
            }
            return false;
        }
        config_.displacement_enabled = true;
        updateWaveOutputState();
        if (!wave_displacement_enabled_->isChecked()
            || wave_output_status_->text().contains(
                tr("cannot affect pixels"))) {
            if (error != nullptr) {
                *error = tr("The Wave page output controls did not update the layer.");
            }
            return false;
        }
        config_.displacement_enabled = expected_displacement;
        config_.lighting_enabled = expected_lighting;
        config_.waves.front().enabled = restored_wave_enabled;
        updateWaveOutputState();
    }
    if (!expected.swings.empty()) {
        const bool expected_swing_enabled = config_.swings.front().enabled;
        swing_enabled_->setChecked(!expected_swing_enabled);
        if (config_.swings.front().enabled == expected_swing_enabled
            || config_.swings.front().phase_degrees
                   != expected.swings.front().phase_degrees) {
            if (error != nullptr) {
                *error = tr("The Swing enable toggle failed or changed unrelated precision.");
            }
            return false;
        }
        swing_enabled_->setChecked(expected_swing_enabled);
    }

    const auto validator_rejects = [](const QLineEdit* editor, QString value) {
        int position = static_cast<int>(value.size());
        return editor->validator() != nullptr
               && editor->validator()->validate(value, position) != QValidator::Acceptable;
    };
    const auto validator_accepts = [](const QLineEdit* editor, QString value) {
        int position = static_cast<int>(value.size());
        return editor->validator() != nullptr
               && editor->validator()->validate(value, position) == QValidator::Acceptable;
    };
    if (!validator_rejects(prefix_, QStringLiteral("bad/name"))
        || !validator_accepts(prefix_, QString(50, QChar(0x20ac)))
        || !validator_accepts(output_directory_, QString(1400, QChar(0x20ac)))
        || !validator_accepts(wave_name_, QString(100, QChar(0x20ac)))
        || !validator_rejects(project_name_, QString())
        || !validator_rejects(project_name_, QStringLiteral("bad/name"))
        || !validator_rejects(project_name_, QString(QChar(0x0085)))
        || !validator_accepts(project_name_, QString(100, QChar(0x20ac)))
        || !validator_accepts(project_name_, QStringLiteral("CON: Fire. "))) {
        if (error != nullptr) {
            *error = tr("GUI text validators did not preserve semantic checks while allowing text beyond the former policy caps.");
        }
        return false;
    }
    pvt::WaveConfig label_wave = pvt::default_wave();
    pvt::SwingConfig label_swing = pvt::default_swing();
    pvt::EffectConfig label_effect = pvt::default_effect(pvt::EffectType::Ripple);
    label_wave.name = "literal %2 %3 %4";
    label_swing.name = "literal %2 %3 %4";
    label_effect.name = "literal %2 %3 %4 %5";
    if (!wave_label(label_wave, 0U).contains(QStringLiteral("literal %2 %3 %4"))
        || !swing_label(label_swing, 0U).contains(QStringLiteral("literal %2 %3 %4"))
        || !effect_label(label_effect, 0U).contains(
               QStringLiteral("literal %2 %3 %4 %5"))) {
        if (error != nullptr) {
            *error = tr("A valid item name containing Qt placeholder text was corrupted in a list label.");
        }
        return false;
    }
    const QString portable_name = QString::fromStdString(
        pvt::portable_project_filename("CON: Fire. "));
    if (!portable_name.endsWith(QStringLiteral(".zip"))
        || portable_name.contains('/') || portable_name.contains('\\')) {
        if (error != nullptr) {
            *error = tr("The display-name-to-bundle-name sanitizer was not portable.");
        }
        return false;
    }

    if (!expected.effects.empty()) {
        const int alpha_edge = effect_edge_->findData(static_cast<int>(pvt::EdgeMode::Alpha));
        const int ripple_type = effect_type_->findData(static_cast<int>(pvt::EffectType::Ripple));
        if (alpha_edge < 0 || ripple_type < 0) {
            if (error != nullptr) {
                *error = tr("The transparent edge option is missing from the GUI.");
            }
            return false;
        }
        effect_type_->setCurrentIndex(ripple_type);
        effect_enabled_->setChecked(true);
        effect_intensity_->setValue(1.0);
        effect_magnitude_->setValue(0.05);
        alpha_enabled_->setChecked(false);
        write_alpha_->setChecked(false);
        effect_edge_->setCurrentIndex(alpha_edge);
        if (!config_.output.write_alpha || !write_alpha_->isChecked()
            || config_.alpha.enabled || alpha_enabled_->isChecked()) {
            if (error != nullptr) {
                *error = tr("Transparent edge handling did not enable final alpha without altering procedural alpha.");
            }
            return false;
        }
        write_alpha_->setChecked(false);
        if (!config_.output.write_alpha || !write_alpha_->isChecked()
            || config_.alpha.enabled || alpha_enabled_->isChecked()) {
            if (error != nullptr) {
                *error = tr("Final alpha could be disabled while active transparency required it.");
            }
            return false;
        }
    }
    frames_->setValue(12);
    if (config_.total_frames != 12
        || !frame_label_->text().contains(QStringLiteral(" / 12"))) {
        if (error != nullptr) {
            *error = tr("The GUI timeline did not follow a frame-count edit.");
        }
        return false;
    }

    const bool swings_checked_before_space = swings_group_->isChecked();
    swings_group_->setFocus(Qt::OtherFocusReason);
    QKeyEvent space_press(QEvent::KeyPress, Qt::Key_Space,
                          Qt::NoModifier, QStringLiteral(" "));
    QKeyEvent space_release(QEvent::KeyRelease, Qt::Key_Space,
                            Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(swings_group_, &space_press);
    QCoreApplication::sendEvent(swings_group_, &space_release);
    if (!playback_timer_->isActive()
        || swings_group_->isChecked() != swings_checked_before_space) {
        if (error != nullptr) {
            *error = tr("Space did not start playback without toggling the focused group.");
        }
        return false;
    }
    QKeyEvent second_space_press(QEvent::KeyPress, Qt::Key_Space,
                                 Qt::NoModifier, QStringLiteral(" "));
    QKeyEvent second_space_release(QEvent::KeyRelease, Qt::Key_Space,
                                   Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(swings_group_, &second_space_press);
    QCoreApplication::sendEvent(swings_group_, &second_space_release);
    if (playback_timer_->isActive()
        || swings_group_->isChecked() != swings_checked_before_space) {
        if (error != nullptr) {
            *error = tr("Space did not pause playback without toggling the focused group.");
        }
        return false;
    }

    // Exercise the asynchronous path that originally displayed one stale
    // frame until Pause. Success requires two different completed frames to be
    // installed while the playback timer is still active. The smoke-only delay
    // makes every render span several 240 FPS ticks, reproducing the stale
    // generation race deterministically.
    fps_->setValue(240.0);
    preview_test_delay_ms_ = 25;
    playback_preview_advanced_ = false;
    play_button_->click();
    QElapsedTimer playback_wait;
    playback_wait.start();
    while (!playback_preview_advanced_ && playback_wait.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    if (playback_timer_->isActive()) {
        play_button_->click();
    }
    preview_test_delay_ms_ = 0;
    if (!playback_preview_advanced_) {
        if (error != nullptr) {
            *error = tr("Playback did not install advancing preview frames.");
        }
        return false;
    }
    auto cancelled_preview_token = std::make_shared<std::atomic_bool>(true);
    const PreviewResult cancelled_preview = generatePreview(
        previewProjectSnapshot(), 0, preview_generation_, document_revision_, 25,
        frameRenderOptions(), cancelled_preview_token);
    if (!cancelled_preview.image.isNull()
        || !cancelled_preview.error.contains(tr("cancel"), Qt::CaseInsensitive)) {
        if (error != nullptr) {
            *error = tr("A stale preview did not honor its cancellation token.");
        }
        return false;
    }

    const auto before_value_randomization = config_;
    randomizeExistingStackSettings();
    const auto same_structure = [](const auto& before, const auto& after,
                                   const auto& same_item) {
        if (before.size() != after.size()) {
            return false;
        }
        for (std::size_t index = 0U; index < before.size(); ++index) {
            if (!same_item(before[index], after[index])) {
                return false;
            }
        }
        return true;
    };
    const bool waves_preserved = same_structure(
        before_value_randomization.waves, config_.waves,
        [](const pvt::WaveConfig& before, const pvt::WaveConfig& after) {
            return before.id == after.id && before.name == after.name
                   && before.enabled == after.enabled;
        });
    const bool swings_preserved = same_structure(
        before_value_randomization.swings, config_.swings,
        [](const pvt::SwingConfig& before, const pvt::SwingConfig& after) {
            return before.id == after.id && before.name == after.name
                   && before.enabled == after.enabled
                   && before.waveform == after.waveform;
        });
    const bool effects_preserved = same_structure(
        before_value_randomization.effects, config_.effects,
        [](const pvt::EffectConfig& before, const pvt::EffectConfig& after) {
            return before.id == after.id && before.name == after.name
                   && before.enabled == after.enabled && before.type == after.type;
        });
    if (!waves_preserved || !swings_preserved || !effects_preserved
        || !pvt::validate(config_).ok) {
        if (error != nullptr) {
            *error = tr("Randomize values changed stack identity or made it invalid.");
        }
        return false;
    }

    randomizeStackComposition();
    std::unordered_set<std::uint64_t> randomized_ids;
    std::unordered_set<int> randomized_effect_types;
    bool unique_randomized_items = true;
    const auto remember_id = [&randomized_ids, &unique_randomized_items](auto id) {
        unique_randomized_items = unique_randomized_items
                                  && id != 0U && randomized_ids.insert(id).second;
    };
    for (const auto& wave : config_.waves) remember_id(wave.id);
    for (const auto& swing : config_.swings) remember_id(swing.id);
    for (const auto& effect : config_.effects) {
        remember_id(effect.id);
        unique_randomized_items = unique_randomized_items
            && randomized_effect_types.insert(static_cast<int>(effect.type)).second;
    }
    const bool enabled_wave = std::any_of(
        config_.waves.begin(), config_.waves.end(),
        [](const auto& wave) { return wave.enabled; });
    const bool enabled_effect = std::any_of(
        config_.effects.begin(), config_.effects.end(),
        [](const auto& effect) { return effect.enabled; });
    const pvt::ValidationResult randomized_validation = pvt::validate(config_);
    if (config_.waves.size() < 2U || config_.waves.size() > 6U
        || config_.swings.size() > 3U
        || config_.effects.empty() || config_.effects.size() > 6U
        || !enabled_wave || !enabled_effect || !unique_randomized_items
        || !randomized_validation.ok) {
        if (error != nullptr) {
            *error = randomized_validation.ok
                ? tr("Randomize mix produced an invalid stack composition.")
                : tr("Randomize mix produced invalid values: %1")
                      .arg(QString::fromStdString(
                          randomized_validation.message));
        }
        return false;
    }

    // Exercise the project document UI on a deterministic clean project.
    project_ = pvt::default_project();
    document_ = std::make_unique<pvt::ProjectDocument>(pvt::default_project_document());
    document_->project = project_;
    active_layer_uuid_ = project_.layers.front().uuid;
    solo_layer_uuid_.reset();
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    current_project_path_.clear();
    imported_legacy_path_.clear();
    baseline_dirty_ = false;
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    updateCompatibilityWarning();
    refreshLayerList();
    refreshAll();

    // Embedded resources are part of the document state, not path-only render
    // settings. Exercise immediate caching, attachment-aware Undo/Redo, clear,
    // and concurrent materialization through two copied documents that share
    // the synchronized immutable-byte cache used by music-analysis staging.
    const QString obj_path = directory.filePath(
        QStringLiteral("attachment smoke.obj"));
    const QByteArray obj_bytes(
        "v -1 -1 0\n"
        "v 1 -1 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0.5 1\n"
        "f 1/1 2/2 3/3\n");
    QFile obj_file(obj_path);
    if (!obj_file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || obj_file.write(obj_bytes) != obj_bytes.size()
        || !obj_file.flush()) {
        if (error != nullptr) {
            *error = tr("Could not create the embedded-attachment smoke fixture.");
        }
        return false;
    }
    obj_file.close();
    clearUndoHistory(false);
    undo_stack_->setClean();
    const std::string obj_reference =
        pvt::surface_obj_attachment_id(active_layer_uuid_);
    if (!setSurfaceObjSource(obj_path)) {
        if (error != nullptr) {
            *error = tr("The GUI could not embed a custom OBJ immediately.");
        }
        return false;
    }
    const pvt::ProjectAttachment* embedded_obj =
        pvt::find_project_attachment(*document_, obj_reference);
    if (embedded_obj == nullptr || embedded_obj->sha256.empty()
        || embedded_obj->basename != "attachment smoke.obj"
        || embedded_obj->local_path.empty()
        || !QFileInfo::exists(QString::fromStdString(embedded_obj->local_path))
        || config_.surface.obj_sha256 != embedded_obj->sha256
        || config_.surface.obj_path != embedded_obj->local_path
        || config_.surface.mapping != pvt::SurfaceMapping::CustomObj
        || undo_stack_->count() != 1) {
        if (error != nullptr) {
            *error = tr("Custom OBJ embedding did not update render and attachment state atomically.");
        }
        return false;
    }

    const QString concurrent_obj_path = directory.filePath(
        QStringLiteral("concurrent cache.obj"));
    const QByteArray concurrent_obj_bytes = obj_bytes + QByteArray("# second digest\n");
    QFile concurrent_obj_file(concurrent_obj_path);
    if (!concurrent_obj_file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || concurrent_obj_file.write(concurrent_obj_bytes)
               != concurrent_obj_bytes.size()
        || !concurrent_obj_file.flush()) {
        if (error != nullptr) {
            *error = tr("Could not create the concurrent attachment smoke fixture.");
        }
        return false;
    }
    concurrent_obj_file.close();
    struct ConcurrentAttachmentResult {
        bool ok = false;
        std::string path;
        std::string error;
    };
    const auto attach_concurrently =
        [concurrent_obj_path](pvt::ProjectDocument candidate,
                              std::string reference) {
            ConcurrentAttachmentResult result;
            pvt::ProjectAttachment attached;
            result.ok = pvt::attach_project_file(
                candidate, reference, concurrent_obj_path.toStdString(),
                &attached, &result.error);
            result.path = std::move(attached.local_path);
            return result;
        };
    auto first_attach = QtConcurrent::run(
        attach_concurrently, *document_, std::string("smoke.concurrent.one"));
    auto second_attach = QtConcurrent::run(
        attach_concurrently, *document_, std::string("smoke.concurrent.two"));
    first_attach.waitForFinished();
    second_attach.waitForFinished();
    const ConcurrentAttachmentResult first_result = first_attach.result();
    const ConcurrentAttachmentResult second_result = second_attach.result();
    if (!first_result.ok || !second_result.ok || first_result.path.empty()
        || first_result.path != second_result.path
        || !QFileInfo::exists(QString::fromStdString(first_result.path))) {
        if (error != nullptr) {
            *error = tr("Copied documents could not safely share concurrent attachment materialization: %1 %2")
                         .arg(QString::fromStdString(first_result.error),
                              QString::fromStdString(second_result.error));
        }
        return false;
    }

    undo_stack_->undo();
    if (pvt::find_project_attachment(*document_, obj_reference) != nullptr
        || !config_.surface.obj_sha256.empty()
        || !config_.surface.obj_path.empty()
        || config_.surface.mapping == pvt::SurfaceMapping::CustomObj) {
        if (error != nullptr) {
            *error = tr("Undo did not remove both custom OBJ render metadata and its attachment reference.");
        }
        return false;
    }
    undo_stack_->redo();
    embedded_obj = pvt::find_project_attachment(*document_, obj_reference);
    if (embedded_obj == nullptr || embedded_obj->sha256.empty()
        || config_.surface.obj_sha256 != embedded_obj->sha256
        || config_.surface.obj_path != embedded_obj->local_path) {
        if (error != nullptr) {
            *error = tr("Redo did not restore the embedded custom OBJ attachment.");
        }
        return false;
    }
    if (!setSurfaceObjSource(QString{})) {
        if (error != nullptr) *error = tr("The embedded custom OBJ could not be cleared.");
        return false;
    }
    if (pvt::find_project_attachment(*document_, obj_reference) != nullptr
        || !config_.surface.obj_sha256.empty()) {
        if (error != nullptr) {
            *error = tr("Clearing a custom OBJ left attachment or digest state behind.");
        }
        return false;
    }
    undo_stack_->undo();
    if (pvt::find_project_attachment(*document_, obj_reference) == nullptr
        || config_.surface.obj_sha256.empty()) {
        if (error != nullptr) {
            *error = tr("Undo did not restore a cleared custom OBJ attachment.");
        }
        return false;
    }
    undo_stack_->redo();
    if (pvt::find_project_attachment(*document_, obj_reference) != nullptr
        || !config_.surface.obj_sha256.empty()) {
        if (error != nullptr) {
            *error = tr("Redo did not clear the custom OBJ attachment again.");
        }
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();

    const QString height_path = directory.filePath(
        QStringLiteral("attachment height smoke.png"));
    QImage height_fixture(7, 5, QImage::Format_RGBA8888);
    for (int y = 0; y < height_fixture.height(); ++y) {
        for (int x = 0; x < height_fixture.width(); ++x) {
            const int level = (x + y) * 255
                              / (height_fixture.width()
                                 + height_fixture.height() - 2);
            height_fixture.setPixel(x, y, qRgba(level, level, level, 255));
        }
    }
    if (!height_fixture.save(height_path, "PNG")) {
        if (error != nullptr) {
            *error = tr("Could not create the Plane height-map smoke fixture.");
        }
        return false;
    }
    const std::string height_reference =
        pvt::plane_displacement_attachment_id(active_layer_uuid_);
    if (!setPlaneDisplacementSource(height_path)) {
        if (error != nullptr) {
            *error = tr("The GUI could not embed a Plane height map immediately.");
        }
        return false;
    }
    const pvt::ProjectAttachment* embedded_height =
        pvt::find_project_attachment(*document_, height_reference);
    if (embedded_height == nullptr || embedded_height->sha256.empty()
        || embedded_height->basename != "attachment height smoke.png"
        || embedded_height->local_path.empty()
        || config_.surface.plane_displacement.sha256
               != embedded_height->sha256
        || config_.surface.plane_displacement.path
               != embedded_height->local_path
        || !config_.surface.plane_displacement.enabled
        || !config_.surface.enabled
        || config_.surface.mapping != pvt::SurfaceMapping::Plane
        || !config_.output.write_alpha
        || !surface_plane_displacement_export_->isEnabled()
        || undo_stack_->count() != 1) {
        if (error != nullptr) {
            *error = tr("Plane height-map embedding did not update geometry, alpha, and attachment state atomically.");
        }
        return false;
    }
    undo_stack_->undo();
    if (pvt::find_project_attachment(*document_, height_reference) != nullptr
        || !config_.surface.plane_displacement.sha256.empty()
        || config_.surface.plane_displacement.enabled) {
        if (error != nullptr) {
            *error = tr("Undo did not remove both Plane displacement metadata and its attachment reference.");
        }
        return false;
    }
    undo_stack_->redo();
    embedded_height = pvt::find_project_attachment(
        *document_, height_reference);
    if (embedded_height == nullptr
        || config_.surface.plane_displacement.sha256
               != embedded_height->sha256
        || !config_.surface.plane_displacement.enabled) {
        if (error != nullptr) {
            *error = tr("Redo did not restore the embedded Plane height map.");
        }
        return false;
    }
    if (!setPlaneDisplacementSource(QString{})) {
        if (error != nullptr) {
            *error = tr("The embedded Plane height map could not be cleared.");
        }
        return false;
    }
    if (pvt::find_project_attachment(*document_, height_reference) != nullptr
        || !config_.surface.plane_displacement.sha256.empty()
        || config_.surface.plane_displacement.enabled) {
        if (error != nullptr) {
            *error = tr("Clearing a Plane height map left attachment or enabled state behind.");
        }
        return false;
    }
    undo_stack_->undo();
    if (pvt::find_project_attachment(*document_, height_reference) == nullptr
        || config_.surface.plane_displacement.sha256.empty()
        || !config_.surface.plane_displacement.enabled) {
        if (error != nullptr) {
            *error = tr("Undo did not restore a cleared Plane height map.");
        }
        return false;
    }
    undo_stack_->redo();
    if (pvt::find_project_attachment(*document_, height_reference) != nullptr
        || !config_.surface.plane_displacement.sha256.empty()) {
        if (error != nullptr) {
            *error = tr("Redo did not clear the Plane height map again.");
        }
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();

    const std::string original_layer_label_name = project_.layers.front().name;
    project_.layers.front().name = "literal %2 %3 %4";
    refreshLayerList();
    if (layer_list_->count() != 1
        || !layer_list_->item(0)->text().contains(
               QStringLiteral("literal %2 %3 %4"))) {
        if (error != nullptr) {
            *error = tr("A valid layer name containing Qt placeholder text was corrupted.");
        }
        return false;
    }
    project_.layers.front().name = original_layer_label_name;
    refreshLayerList();

    const QString display_name = QStringLiteral("CON: Ember. ");
    project_name_->setText(display_name);
    if (!QMetaObject::invokeMethod(project_name_, "editingFinished",
                                   Qt::DirectConnection)
        || QString::fromStdString(project_.name) != display_name
        || !windowTitle().startsWith(display_name)
        || undo_stack_->count() != 1) {
        if (error != nullptr) {
            *error = tr("Project naming, title binding, or rename undo registration failed.");
        }
        return false;
    }
    undo_stack_->undo();
    if (QString::fromStdString(project_.name) == display_name) {
        if (error != nullptr) *error = tr("Project rename did not undo.");
        return false;
    }
    undo_stack_->redo();
    if (QString::fromStdString(project_.name) != display_name) {
        if (error != nullptr) *error = tr("Project rename did not redo.");
        return false;
    }

    clearUndoHistory(false);
    undo_stack_->setClean();
    write_alpha_->setChecked(false);
    phrase_warp_->setValue(0.25);
    const std::string base_uuid = active_layer_uuid_;
    const double base_phrase_warp = config_.phrase_warp;
    addLayer();
    if (project_.layers.size() != 2U || active_layer_uuid_ != project_.layers.back().uuid
        || project_.output.write_alpha || write_alpha_->isChecked()
        || layer_list_->count() != 2
        || layer_list_->item(0)->data(Qt::UserRole).toString().toStdString()
               != project_.layers.back().uuid
        || config_.phrase_warp == base_phrase_warp) {
        if (error != nullptr) {
            *error = tr("Adding an opaque layer did not preserve paint order, isolation, or the valid RGB output choice.");
        }
        return false;
    }
    const std::string top_uuid = active_layer_uuid_;
    phrase_warp_->setValue(0.75);
    width_->setValue(333);
    const int add_blend = layer_blend_->findData(static_cast<int>(pvt::BlendMode::Add));
    if (add_blend < 0) {
        if (error != nullptr) *error = tr("The Add layer blend mode is missing.");
        return false;
    }
    layer_blend_->setCurrentIndex(add_blend);
    const int before_solo_undo_count = undo_stack_->count();
    layer_solo_->setChecked(true);
    if (!solo_layer_uuid_ || *solo_layer_uuid_ != top_uuid
        || undo_stack_->count() != before_solo_undo_count
        || !findLayer(top_uuid)->enabled) {
        if (error != nullptr) *error = tr("Layer Solo was not session-only.");
        return false;
    }
    layer_solo_->setChecked(false);

    selectLayer(base_uuid);
    if (std::abs(config_.phrase_warp - base_phrase_warp) > 1e-12
        || config_.width != 333 || layer_blend_->isEnabled()) {
        if (error != nullptr) {
            *error = tr("Switching layers did not isolate Render data or preserve global Canvas data.");
        }
        return false;
    }
    selectLayer(top_uuid);
    if (std::abs(config_.phrase_warp - 0.75) > 1e-12
        || config_.width != 333 || !layer_blend_->isEnabled()
        || findLayer(top_uuid)->blend_mode != pvt::BlendMode::Add) {
        if (error != nullptr) *error = tr("The top layer did not reload its Render data.");
        return false;
    }

    const int alpha_under = layer_alpha_mode_->findData(
        static_cast<int>(pvt::AlphaMode::AlphaUnder));
    if (alpha_under < 0) {
        if (error != nullptr) *error = tr("The Alpha Under layer mode is missing.");
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();
    layer_alpha_mode_->setCurrentIndex(alpha_under);
    if (findLayer(top_uuid)->alpha_mode != pvt::AlphaMode::AlphaUnder
        || undo_stack_->count() != 1) {
        if (error != nullptr) {
            *error = tr("Changing Alpha Mode did not update the selected layer as one undoable edit.");
        }
        return false;
    }
    undo_stack_->undo();
    if (findLayer(top_uuid)->alpha_mode != pvt::AlphaMode::AlphaOver) {
        if (error != nullptr) *error = tr("Alpha Mode did not undo to Alpha Over.");
        return false;
    }
    undo_stack_->redo();

    addGroup();
    if (project_.groups.size() != 1U || !selected_group_uuid_
        || project_.layers.back().group_uuid != project_.groups.front().uuid
        || layer_list_->count() != 3 || !selected_group_box_->isEnabled()) {
        if (error != nullptr) {
            *error = tr("Adding a group did not create a selected folder around the active layer.");
        }
        return false;
    }
    const std::string group_uuid = project_.groups.front().uuid;
    group_solo_->setChecked(true);
    const pvt::ProjectConfig group_solo_snapshot = previewProjectSnapshot();
    if (!solo_group_uuid_ || *solo_group_uuid_ != group_uuid
        || std::count_if(group_solo_snapshot.layers.begin(),
                         group_solo_snapshot.layers.end(),
                         [](const pvt::LayerConfig& layer) {
                             return layer.enabled;
                         }) != 1) {
        if (error != nullptr) *error = tr("Group Solo did not isolate the folder in preview.");
        return false;
    }
    group_solo_->setChecked(false);
    group_locked_->setChecked(true);
    selectLayer(top_uuid);
    if (!project_.groups.front().locked || layer_alpha_mode_->isEnabled()
        || source_page_->isEnabled() || randomize_values_action_->isEnabled()
        || randomize_mix_action_->isEnabled()
        || layer_clock_group_->isEnabled() || swings_group_->isEnabled()
        || !clock_mode_->isEnabled()
        || !workflow_stage_buttons_[0]->isEnabled()
        || workflow_stage_buttons_[1]->isEnabled()
        || !workflow_stage_buttons_[6]->isEnabled()) {
        if (error != nullptr) *error = tr("Locking a group did not protect its contained layer editors.");
        return false;
    }
    selectGroup(group_uuid);
    if (!workflow_stage_buttons_[0]->isEnabled()
        || workflow_stage_buttons_[1]->isEnabled()
        || !workflow_stage_buttons_[6]->isEnabled()
        || !clock_mode_->isEnabled() || layer_clock_group_->isEnabled()
        || swings_group_->isEnabled()) {
        if (error != nullptr) {
            *error = tr("Selecting a group did not keep project controls reachable while protecting layer controls.");
        }
        return false;
    }
    group_locked_->setChecked(false);
    addLayer();
    const std::string temporary_uuid = active_layer_uuid_;
    selectGroup(group_uuid);
    moveSelectedGroup(1);
    if (project_.layers.back().group_uuid != group_uuid) {
        if (error != nullptr) *error = tr("A layer group did not move as one folder block.");
        return false;
    }
    moveSelectedGroup(-1);
    selectLayer(temporary_uuid);
    removeLayer();
    selectGroup(group_uuid);
    removeSelectedGroup();
    if (!project_.groups.empty()
        || std::any_of(project_.layers.begin(), project_.layers.end(),
                       [](const pvt::LayerConfig& layer) {
                           return !layer.group_uuid.empty();
                       })) {
        if (error != nullptr) *error = tr("Removing a group did not preserve and ungroup its layers.");
        return false;
    }
    selectLayer(top_uuid);

    const QString current_frame_path =
        QDir(directory.path()).filePath(QStringLiteral("current-frame.png"));
    // The packaged smoke runs on hosted macOS machines that may expose a Metal
    // device without a usable display-backed command queue.  Exercise the
    // complete full-resolution export and image-writer path deterministically
    // on CPU here; dedicated backend tests cover CPU/Metal parity separately.
    const pvt::RenderBackend saved_render_backend = render_backend_;
    const int saved_export_width = project_.canvas.width;
    const int saved_export_height = project_.canvas.height;
    constexpr int smoke_export_width = 96;
    constexpr int smoke_export_height = 64;
    project_.canvas.width = smoke_export_width;
    project_.canvas.height = smoke_export_height;
    config_.width = smoke_export_width;
    config_.height = smoke_export_height;
    render_backend_ = pvt::RenderBackend::Cpu;
    const bool current_frame_export_started =
        startCurrentFrameExport(current_frame_path);
    render_backend_ = saved_render_backend;
    if (!current_frame_export_started) {
        project_.canvas.width = saved_export_width;
        project_.canvas.height = saved_export_height;
        config_.width = saved_export_width;
        config_.height = saved_export_height;
        if (error != nullptr) *error = tr("Current-frame export could not start during smoke testing.");
        return false;
    }
    while (export_watcher_->isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    const QImage current_frame_image(current_frame_path);
    project_.canvas.width = saved_export_width;
    project_.canvas.height = saved_export_height;
    config_.width = saved_export_width;
    config_.height = saved_export_height;
    if (export_active_ || current_frame_image.isNull()
        || current_frame_image.width() != smoke_export_width
        || current_frame_image.height() != smoke_export_height) {
        if (error != nullptr) {
            *error = tr("Export Current Frame did not write the full canvas dimensions or restore export state.");
        }
        return false;
    }

    // A translucent/transparent top over an opaque base is still opaque and
    // may intentionally export RGB. Once the base is hidden, the same
    // non-active top layer must force RGBA.
    write_alpha_->setChecked(false);
    layer_opacity_->setValue(50.0);
    if (project_.output.write_alpha || write_alpha_->isChecked()) {
        if (error != nullptr) {
            *error = tr("A translucent top layer over an opaque base unnecessarily forced RGBA.");
        }
        return false;
    }
    layer_opacity_->setValue(100.0);
    alpha_minimum_->setValue(0.5);
    alpha_maximum_->setValue(0.5);
    alpha_enabled_->setChecked(true);
    selectLayer(base_uuid);
    layer_enabled_->setChecked(false);
    if (!project_.output.write_alpha || !write_alpha_->isChecked()
        || !findLayer(top_uuid)->render.alpha.enabled
        || findLayer(base_uuid)->render.alpha.enabled) {
        if (error != nullptr) {
            *error = tr("Transparency in a non-active visible layer was not enforced globally.");
        }
        return false;
    }
    const int undo_count_before_rejected_alpha = undo_stack_->count();
    write_alpha_->setChecked(false);
    if (!project_.output.write_alpha || !write_alpha_->isChecked()) {
        if (error != nullptr) *error = tr("A transparent-only stack allowed RGB export.");
        return false;
    }
    if (undo_stack_->count() != undo_count_before_rejected_alpha) {
        if (error != nullptr) {
            *error = tr("A rejected final-alpha edit created a no-op Undo entry.");
        }
        return false;
    }
    layer_enabled_->setChecked(true);
    write_alpha_->setChecked(false);
    if (project_.output.write_alpha || write_alpha_->isChecked()) {
        if (error != nullptr) *error = tr("An opaque base did not restore the RGB export option.");
        return false;
    }

    selectLayer(top_uuid);
    const int erase_blend = layer_blend_->findData(
        static_cast<int>(pvt::BlendMode::Erase));
    if (erase_blend < 0) {
        if (error != nullptr) *error = tr("The Erase layer blend mode is missing.");
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();
    const std::uint64_t preview_before_eraser = preview_generation_;
    layer_blend_->setCurrentIndex(erase_blend);
    if (findLayer(top_uuid)->blend_mode != pvt::BlendMode::Erase
        || !project_.output.write_alpha || !write_alpha_->isChecked()
        || undo_stack_->count() != 1
        || preview_generation_ <= preview_before_eraser) {
        if (error != nullptr) {
            *error = tr("An eraser above an opaque base did not enable RGBA as one previewed edit.");
        }
        return false;
    }
    undo_stack_->undo();
    if (findLayer(top_uuid)->blend_mode != pvt::BlendMode::Add
        || project_.output.write_alpha || write_alpha_->isChecked()) {
        if (error != nullptr) {
            *error = tr("Undoing an eraser did not restore its matching RGB output state.");
        }
        return false;
    }
    undo_stack_->redo();
    if (findLayer(top_uuid)->blend_mode != pvt::BlendMode::Erase
        || !project_.output.write_alpha || !write_alpha_->isChecked()) {
        if (error != nullptr) {
            *error = tr("Redoing an eraser did not restore its matching RGBA output state.");
        }
        return false;
    }
    undo_stack_->undo();

    layer_solo_->setChecked(true);
    const pvt::ProjectConfig solo_snapshot = previewProjectSnapshot();
    if (!solo_snapshot.output.write_alpha || !pvt::validate(solo_snapshot).ok
        || std::count_if(solo_snapshot.layers.begin(), solo_snapshot.layers.end(),
                         [](const auto& layer) { return layer.enabled; }) != 1) {
        if (error != nullptr) {
            *error = tr("Soloing a transparent layer did not produce a valid preview-only RGBA snapshot.");
        }
        return false;
    }
    layer_solo_->setChecked(false);
    clearUndoHistory(false);
    undo_stack_->setClean();
    moveActiveLayer(-1);
    if (undo_stack_->count() != 1 || project_.layers.front().uuid != top_uuid
        || layer_list_->item(0)->data(Qt::UserRole).toString().toStdString()
               != project_.layers.back().uuid) {
        if (error != nullptr) *error = tr("Layer reorder or top-first display order failed.");
        return false;
    }
    undo_stack_->undo();
    if (project_.layers.back().uuid != top_uuid) {
        if (error != nullptr) *error = tr("Layer reorder did not undo.");
        return false;
    }
    undo_stack_->redo();
    if (project_.layers.front().uuid != top_uuid) {
        if (error != nullptr) *error = tr("Layer reorder did not redo.");
        return false;
    }
    undo_stack_->undo();

    duplicateLayer();
    const std::string duplicate_uuid = active_layer_uuid_;
    std::unordered_set<std::string> layer_uuids;
    std::unordered_set<std::uint64_t> layer_file_ids;
    for (const auto& layer : project_.layers) {
        layer_uuids.insert(layer.uuid);
        layer_file_ids.insert(layer.file_id);
    }
    if (project_.layers.size() != 3U || duplicate_uuid == top_uuid
        || layer_uuids.size() != project_.layers.size()
        || layer_file_ids.size() != project_.layers.size()) {
        if (error != nullptr) *error = tr("Layer duplication did not allocate unique identities.");
        return false;
    }
    removeLayer();
    selectLayer(top_uuid);
    removeLayer();
    if (project_.layers.size() != 1U || project_.layers.front().uuid != base_uuid) {
        if (error != nullptr) *error = tr("Layer removal selected the wrong survivor.");
        return false;
    }
    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget)) {
                if (auto* button = message->button(QMessageBox::Ok)) button->click();
                return;
            }
        }
    });
    removeLayer();
    if (project_.layers.size() != 1U) {
        if (error != nullptr) *error = tr("The GUI allowed removal of the final layer.");
        return false;
    }

    if (config_.waves.empty()) {
        if (error != nullptr) *error = tr("The default project has no draggable wave.");
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();
    const double original_wave_x = config_.waves.front().x_percent;
    const double original_wave_y = config_.waves.front().y_percent;
    preview_->waveDragStarted(0U);
    preview_->waveDragFinished(0U);
    if (undo_stack_->count() != 0
        || config_.waves.front().x_percent != original_wave_x
        || config_.waves.front().y_percent != original_wave_y) {
        if (error != nullptr) {
            *error = tr("Clicking a wave handle without dragging changed the project.");
        }
        return false;
    }
    preview_->waveDragStarted(0U);
    preview_->waveMoved(0U, 20.0, 30.0);
    preview_->waveMoved(0U, 25.0, 35.0);
    preview_->waveDragFinished(0U);
    preview_->waveDragStarted(0U);
    preview_->waveMoved(0U, 40.0, 45.0);
    preview_->waveDragFinished(0U);
    if (undo_stack_->count() != 2) {
        if (error != nullptr) *error = tr("Separate wave drags were merged into one undo step.");
        return false;
    }
    undo_stack_->undo();
    if (std::abs(config_.waves.front().x_percent - 25.0) > 1e-12
        || std::abs(config_.waves.front().y_percent - 35.0) > 1e-12) {
        if (error != nullptr) *error = tr("The latest wave drag did not undo as one gesture.");
        return false;
    }
    undo_stack_->undo();
    if (std::abs(config_.waves.front().x_percent - original_wave_x) > 1e-12
        || std::abs(config_.waves.front().y_percent - original_wave_y) > 1e-12) {
        if (error != nullptr) *error = tr("The earlier wave drag did not have its own undo step.");
        return false;
    }
    undo_stack_->redo();
    undo_stack_->redo();
    if (std::abs(config_.waves.front().x_percent - 40.0) > 1e-12
        || std::abs(config_.waves.front().y_percent - 45.0) > 1e-12) {
        if (error != nullptr) *error = tr("Wave drag redo did not restore the final gesture.");
        return false;
    }

    clearUndoHistory(false);
    undo_stack_->setClean();
    baseline_dirty_ = false;
    const QString bundle_path = directory.filePath(QStringLiteral("smoke-project.zip"));
    setWorkflowStage(4);
    QWidget* const tab_before_save = tabs_->currentWidget();
    const int stage_before_save = workflow_stage_index_;
    const int category_before_save = effect_category_filter_;
    if (!saveProjectPath(bundle_path) || document_ == nullptr
        || document_->versions.size() != 1U || hasUnsavedChanges()) {
        if (error != nullptr) *error = tr("The GUI could not create the first bundle version.");
        return false;
    }
    if (tabs_->currentWidget() != tab_before_save
        || workflow_stage_index_ != stage_before_save
        || effect_category_filter_ != category_before_save
        || !workflow_stage_buttons_[4]->isChecked()) {
        if (error != nullptr) {
            *error = tr("Saving navigated away from the user's selected workflow stage.");
        }
        return false;
    }

    const std::string saved_project_name = project_.name;
    const auto choose_rename_prompt_button = [](const QString& object_name) {
        QTimer::singleShot(0, [object_name] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* message = qobject_cast<QMessageBox*>(widget)) {
                    if (auto* button = message->findChild<QPushButton*>(object_name)) {
                        button->click();
                        return;
                    }
                }
            }
        });
    };
    project_name_->setText(QStringLiteral("Canceled Saved Rename"));
    choose_rename_prompt_button(QStringLiteral("projectRenameCancel"));
    if (!QMetaObject::invokeMethod(project_name_, "editingFinished",
                                   Qt::DirectConnection)
        || project_.name != saved_project_name
        || project_name_->text().toStdString() != saved_project_name
        || current_project_path_ != bundle_path || hasUnsavedChanges()
        || undo_stack_->count() != 0) {
        if (error != nullptr) {
            *error = tr("Canceling the saved-project rename changed the active project.");
        }
        return false;
    }
    project_name_->setText(QStringLiteral("Same Bundle, New Display Name"));
    choose_rename_prompt_button(QStringLiteral("projectRenameKeepFilename"));
    if (!QMetaObject::invokeMethod(project_name_, "editingFinished",
                                   Qt::DirectConnection)
        || project_.name != "Same Bundle, New Display Name"
        || current_project_path_ != bundle_path || !hasUnsavedChanges()
        || undo_stack_->count() != 1) {
        if (error != nullptr) {
            *error = tr("Keeping the bundle filename did not register an undoable project rename.");
        }
        return false;
    }
    undo_stack_->undo();
    if (project_.name != saved_project_name || hasUnsavedChanges()
        || project_name_->text().toStdString() != saved_project_name) {
        if (error != nullptr) {
            *error = tr("Undoing the in-place project rename did not restore the saved state.");
        }
        return false;
    }

    const auto first_version = document_->versions.front().number;
    const std::string second_version_name = "Second Version Display Name";
    applyProjectNameChange(saved_project_name, second_version_name);
    phrase_warp_->setValue(config_.phrase_warp < 1.8
                               ? config_.phrase_warp + 0.1
                               : config_.phrase_warp - 0.1);
    if (!saveProjectPath(bundle_path) || document_->versions.size() != 2U) {
        if (error != nullptr) *error = tr("The GUI did not append a changed bundle version.");
        return false;
    }
    const auto second_version = document_->versions.back().number;
    const int before_version_index = version_before_->findData(
        QVariant::fromValue<qulonglong>(first_version));
    const int after_version_index = version_after_->findData(
        QVariant::fromValue<qulonglong>(second_version));
    version_before_->setCurrentIndex(before_version_index);
    version_after_->setCurrentIndex(after_version_index);
    refreshVersionDiff();
    if (before_version_index < 0 || after_version_index < 0
        || version_diff_->toPlainText().isEmpty()
        || version_diff_->toPlainText().contains(tr("Could not compare"))) {
        if (error != nullptr) *error = tr("The Versions page could not show a semantic diff.");
        return false;
    }
    std::vector<pvt::BundleVersionInfo> validated_versions;
    std::string validation_error;
    if (!pvt::validate_project_bundle(bundle_path.toStdString(), &validated_versions,
                                      &validation_error)
        || validated_versions.size() != document_->versions.size()) {
        if (error != nullptr) {
            *error = tr("The saved GUI bundle did not validate: %1")
                         .arg(QString::fromStdString(validation_error));
        }
        return false;
    }

    const QString stay_copy_path =
        directory.filePath(QStringLiteral("renamed-copy-stay.zip"));
    const std::string original_project_uuid = project_.uuid;
    const QString original_name = QString::fromStdString(project_.name);
    project_name_->setText(QStringLiteral("Renamed Copy While Staying"));
    independent_copy_test_path_ = stay_copy_path;
    choose_rename_prompt_button(QStringLiteral("projectRenameSaveAndStay"));
    if (!QMetaObject::invokeMethod(project_name_, "editingFinished",
                                   Qt::DirectConnection)) {
        if (error != nullptr) {
            *error = tr("The saved-project rename prompt could not dispatch Save Copy and Stay.");
        }
        return false;
    }
    pvt::ProjectDocument stay_copy;
    std::string stay_load_error;
    if (!pvt::load_project_document(
            stay_copy_path.toStdString(), stay_copy, &stay_load_error)
        || stay_copy.versions.size() != 1U || stay_copy.current_version != 0U
        || stay_copy.project.name != "Renamed Copy While Staying"
        || stay_copy.project.uuid == original_project_uuid
        || project_.uuid != original_project_uuid
        || QString::fromStdString(project_.name) != original_name
        || project_name_->text() != original_name
        || current_project_path_ != bundle_path
        || document_->versions.size() != 2U || hasUnsavedChanges()
        || !independent_copy_test_path_.isEmpty()) {
        if (error != nullptr) {
            *error = tr("Save Copy and Stay did not restore the old displayed name, preserve the original project, or reset the copy history.");
        }
        return false;
    }

    const auto select_version_row = [this](std::uint64_t version) {
        for (int row = 0; row < version_list_->count(); ++row) {
            if (version_list_->item(row)->data(Qt::UserRole).toULongLong() == version) {
                version_list_->setCurrentRow(row);
                return true;
            }
        }
        return false;
    };
    if (!select_version_row(first_version)) {
        if (error != nullptr) *error = tr("The first saved version is missing from the Versions page.");
        return false;
    }
    phrase_warp_->setValue(config_.phrase_warp < 1.7
                               ? config_.phrase_warp + 0.2
                               : config_.phrase_warp - 0.2);
    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget)) {
                if (auto* button = message->button(QMessageBox::Save)) button->click();
                return;
            }
        }
    });
    if (!playback_timer_->isActive()) togglePlayback();
    makeSelectedVersionCurrent();
    while (project_io_watcher_->isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    if (document_->current_version != first_version || hasUnsavedChanges()
        || project_.name != saved_project_name
        || project_name_->text().toStdString() != saved_project_name
        || !windowTitle().startsWith(
            QString::fromStdString(saved_project_name))
        || playback_timer_->isActive()
        || play_button_->text() != tr("Play")) {
        if (error != nullptr) {
            *error = tr("Make Current did not replace the saved name, title, playback, or clean version state.");
        }
        return false;
    }

    const std::size_t version_count_before_revert = document_->versions.size();
    if (!select_version_row(second_version)) {
        if (error != nullptr) *error = tr("The second saved version is missing from the Versions page.");
        return false;
    }
    QTimer::singleShot(0, [] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* message = qobject_cast<QMessageBox*>(widget)) {
                if (auto* button = message->button(QMessageBox::Yes)) button->click();
                return;
            }
        }
    });
    revertSelectedVersion();
    if (document_->versions.size() != version_count_before_revert + 1U
        || document_->current_version <= second_version || hasUnsavedChanges()
        || undo_stack_->count() != 0
        || project_.name != second_version_name
        || project_name_->text().toStdString() != second_version_name
        || !windowTitle().startsWith(
            QString::fromStdString(second_version_name))) {
        if (error != nullptr) {
            *error = tr("Revert did not create and cleanly select a new immutable version "
                        "(versions %1, expected %2; current %3, source %4; dirty %5; undo %6).")
                         .arg(document_->versions.size())
                         .arg(version_count_before_revert + 1U)
                         .arg(document_->current_version)
                         .arg(second_version)
                         .arg(hasUnsavedChanges())
                         .arg(undo_stack_->count());
        }
        return false;
    }

    const QString opened_copy_path =
        directory.filePath(QStringLiteral("renamed-copy-open.zip"));
    const std::vector<std::string> old_layer_uuids = [&] {
        std::vector<std::string> values;
        values.reserve(project_.layers.size());
        for (const pvt::LayerConfig& layer : project_.layers) {
            values.push_back(layer.uuid);
        }
        return values;
    }();
    project_name_->setText(QStringLiteral("Renamed Copy Opened"));
    independent_copy_test_path_ = opened_copy_path;
    choose_rename_prompt_button(QStringLiteral("projectRenameSaveAndOpen"));
    if (!QMetaObject::invokeMethod(project_name_, "editingFinished",
                                   Qt::DirectConnection)
        || current_project_path_ != opened_copy_path
        || project_.name != "Renamed Copy Opened"
        || project_.uuid == original_project_uuid
        || document_->versions.size() != 1U
        || document_->current_version != 0U || hasUnsavedChanges()
        || project_name_->text() != QStringLiteral("Renamed Copy Opened")
        || !independent_copy_test_path_.isEmpty()) {
        if (error != nullptr) {
            *error = tr("Save As and Open did not replace the active document cleanly.");
        }
        return false;
    }
    for (const pvt::LayerConfig& layer : project_.layers) {
        if (std::find(old_layer_uuids.begin(), old_layer_uuids.end(), layer.uuid)
            != old_layer_uuids.end()) {
            if (error != nullptr) {
                *error = tr("An opened independent copy retained an original layer UUID.");
            }
            return false;
        }
    }

    document_->created_with_version = "999.0.0";
    updateCompatibilityWarning();
    if (!compatibility_warning_.isEmpty()
        || !compatibility_warning_label_->isHidden()) {
        if (error != nullptr) {
            *error = tr("A version number alone incorrectly produced a save-risk warning.");
        }
        return false;
    }
    document_->project.canvas.output_compatibility.records.push_back(
        {"future.output.sparkle", "maximum", false});
    updateCompatibilityWarning();
    if (compatibility_warning_.isEmpty() || compatibility_warning_label_->isHidden()
        || !compatibility_warning_.contains(tr("keeps"),
                                            Qt::CaseInsensitive)) {
        if (error != nullptr) {
            *error = tr("Preserved future data did not produce an accurate recovery notice.");
        }
        return false;
    }

    // Replacing a saved document must also replace every Versions-page view
    // and action target. Otherwise the old rows remain clickable even though
    // document_ now points at an unrelated unsaved project.
    if (version_list_->count() < 1) {
        if (error != nullptr) {
            *error = tr("The Versions page regression setup has no saved version to replace.");
        }
        return false;
    }
    replaceWithNewProject();
    if (!document_->versions.empty() || !current_project_path_.isEmpty()
        || hasUnsavedChanges() || undo_stack_->count() != 0
        || version_list_->count() != 0 || version_before_->count() != 0
        || version_after_->count() != 0 || version_before_->isEnabled()
        || version_after_->isEnabled() || version_compare_->isEnabled()
        || version_make_current_->isEnabled() || version_revert_->isEnabled()
        || !version_diff_->toPlainText().isEmpty()
        || !version_summary_->text().contains(tr("Not saved as a bundle yet"))
        || !version_summary_->text().contains(
            QString::fromStdString(project_.uuid))) {
        if (error != nullptr) {
            *error = tr("New Project retained stale version rows, selectors, actions, or bundle details.");
        }
        return false;
    }
    // Leave screenshot-mode smoke runs in the same calm, first-launch state a
    // user sees instead of exposing whichever stage an earlier assertion used.
    setWorkflowStage(1);
    setDriversExpanded(false);
    return true;
}

void MainWindow::finishProjectNameEdit() {
    if (populating_) return;
    const QString edited = project_name_->text();
    const auto restore_editor = [this] {
        const QSignalBlocker blocker(project_name_);
        project_name_->setText(QString::fromStdString(project_.name));
    };
    if (!valid_text(edited, TextRule::ProjectName)) {
        restore_editor();
        status_->setText(
            tr("Use a nonempty project name without control characters or path separators."));
        return;
    }

    const std::string before = project_.name;
    const std::string after = edited.toStdString();
    if (before == after) return;
    if (current_project_path_.isEmpty()) {
        applyProjectNameChange(before, after);
        return;
    }

    const SavedProjectRenameAction action =
        promptForSavedProjectRename(before, after);
    if (action == SavedProjectRenameAction::Cancel) {
        restore_editor();
        status_->setText(tr("Project rename canceled; no project was changed."));
        return;
    }
    if (action == SavedProjectRenameAction::KeepBundleFilename) {
        applyProjectNameChange(before, after);
        status_->setText(
            tr("Project renamed. The existing bundle filename is unchanged; Save will append the rename as a new version."));
        return;
    }

    const bool open_copy = action == SavedProjectRenameAction::SaveCopyAndOpen;
    if (open_copy && !documentReplacementAllowed()) {
        restore_editor();
        return;
    }
    const QString path = chooseIndependentCopyPath(after);
    if (path.isEmpty()) {
        restore_editor();
        status_->setText(tr("Project copy canceled; the current project was not changed."));
        return;
    }
    QString copy_error;
    if (!saveIndependentRenamedCopy(after, path, open_copy, &copy_error)) {
        restore_editor();
        QMessageBox::critical(
            this, tr("Could not create independent project"), copy_error);
        return;
    }
    if (!open_copy) {
        restore_editor();
    }
}

void MainWindow::applyProjectNameChange(const std::string& before,
                                        const std::string& after) {
    if (before == after) return;
    project_.name = after;
    recordUndo(
        tr("Rename project"),
        [this, before] {
            project_.name = before;
            refreshLayerList();
            noteDocumentChange();
        },
        [this, after] {
            project_.name = after;
            refreshLayerList();
            noteDocumentChange();
        });
    noteDocumentChange();
    refreshLayerList();
}

MainWindow::SavedProjectRenameAction MainWindow::promptForSavedProjectRename(
    const std::string& before, const std::string& after) {
    QMessageBox prompt(this);
    prompt.setIcon(QMessageBox::Question);
    prompt.setWindowTitle(tr("Rename saved project"));
    const QString old_name = QString::fromStdString(before);
    const QString new_name = QString::fromStdString(after);
    const QString bundle_name = QFileInfo(current_project_path_).fileName();
    prompt.setText(QStringLiteral("“") + old_name
                   + tr("” is currently saved as “") + bundle_name
                   + QStringLiteral("”."));
    prompt.setInformativeText(
        tr("Keep the existing filename and record the rename on the next Save, "
           "or create a new independent bundle named from “")
        + new_name
        + tr("”. An independent copy contains only the current working state as "
             "version 0 and receives new project and layer UUIDs."));

    auto* keep = prompt.addButton(
        tr("Keep Existing Filename"), QMessageBox::AcceptRole);
    auto* open_copy = prompt.addButton(
        tr("Save As and Open"), QMessageBox::ActionRole);
    auto* stay = prompt.addButton(
        tr("Save Copy, Stay Here"), QMessageBox::ActionRole);
    auto* cancel = prompt.addButton(QMessageBox::Cancel);
    keep->setObjectName(QStringLiteral("projectRenameKeepFilename"));
    open_copy->setObjectName(QStringLiteral("projectRenameSaveAndOpen"));
    stay->setObjectName(QStringLiteral("projectRenameSaveAndStay"));
    cancel->setObjectName(QStringLiteral("projectRenameCancel"));
    keep->setToolTip(
        tr("Rename this project but leave its current bundle path unchanged."));
    open_copy->setToolTip(
        tr("Create and switch to an independent one-version project copy."));
    stay->setToolTip(
        tr("Create the independent copy, then continue editing this project under its old name."));
    if (export_watcher_ != nullptr && export_watcher_->isRunning()) {
        open_copy->setEnabled(false);
        open_copy->setToolTip(
            tr("Finish or cancel the active export before switching projects."));
    }
    prompt.setDefaultButton(keep);
    prompt.setEscapeButton(cancel);
    prompt.exec();

    if (prompt.clickedButton() == keep) {
        return SavedProjectRenameAction::KeepBundleFilename;
    }
    if (prompt.clickedButton() == open_copy) {
        return SavedProjectRenameAction::SaveCopyAndOpen;
    }
    if (prompt.clickedButton() == stay) {
        return SavedProjectRenameAction::SaveCopyAndStay;
    }
    return SavedProjectRenameAction::Cancel;
}

QString MainWindow::chooseIndependentCopyPath(
    const std::string& project_name) {
    const QString filename = QString::fromStdString(
        pvt::portable_project_filename(project_name));
    const QString preferred = QFileInfo(current_project_path_).absolutePath();
    const QString initial_path =
        QDir(usableDialogDirectory(preferred)).filePath(filename);
    QString suggested_path = initial_path;
    while (true) {
        QString path;
        if (!independent_copy_test_path_.isEmpty()) {
            path.swap(independent_copy_test_path_);
        } else {
            path = QFileDialog::getSaveFileName(
                this, tr("Save independent project copy"), suggested_path,
                tr("PVT project bundle (*.zip);;All files (*)"));
        }
        if (path.isEmpty()) return {};
        if (QFileInfo(path).suffix().compare(
                QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
            path.append(QStringLiteral(".zip"));
        }
        suggested_path = path;
        const QFileInfo destination(path);
        if (equivalent_local_path(path, current_project_path_)) {
            QMessageBox::warning(
                this, tr("Choose a new bundle filename"),
                tr("An independent project cannot replace the bundle that is currently open. Choose a different filename."));
            continue;
        }
        if (destination.exists() || destination.isSymLink()) {
            QMessageBox::warning(
                this, tr("Destination already exists"),
                tr("Independent project copies never overwrite an existing file or directory. Choose a new destination."));
            continue;
        }
        return path;
    }
}

bool MainWindow::saveIndependentRenamedCopy(
    const std::string& project_name, const QString& path, bool open_copy,
    QString* error) {
    const auto fail_copy = [error](const QString& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    if (!valid_text(QString::fromStdString(project_name), TextRule::ProjectName)) {
        return fail_copy(tr("The copied project name is invalid."));
    }
    if (path.isEmpty()) {
        return fail_copy(tr("The copied project needs a destination path."));
    }
    QString editor_error;
    if (!outputEditorsValid(&editor_error)) {
        return fail_copy(
            tr("Fix the invalid output settings before copying the project.\n\n")
            + editor_error);
    }
    if (open_copy) {
        QString replacement_error;
        if (!documentReplacementAllowed(&replacement_error)) {
            return fail_copy(replacement_error);
        }
    }
    if (equivalent_local_path(path, current_project_path_)) {
        return fail_copy(
            tr("An independent project cannot replace the bundle that is currently open."));
    }
    const QFileInfo destination(path);
    if (destination.exists() || destination.isSymLink()) {
        return fail_copy(
            tr("Independent project copies never overwrite an existing file or directory."));
    }

    std::unique_ptr<pvt::ProjectDocument> copy;
    pvt::ProjectConfig opened_project;
    pvt::RenderConfig opened_config;
    std::string opened_active_layer_uuid;
    std::optional<std::string> opened_solo_layer_uuid;
    QString opened_path;
    bool opened_integer_dither_preference = integer_dither_preference_;
    try {
        // Build the complete working snapshot locally. In particular, do not
        // synchronize into project_: Save Copy and Stay must leave the open
        // project byte-for-byte unchanged if allocation or persistence fails.
        pvt::ProjectConfig snapshot = project_;
        const auto active = std::find_if(
            snapshot.layers.begin(), snapshot.layers.end(),
            [this](const pvt::LayerConfig& layer) {
                return layer.uuid == active_layer_uuid_;
            });
        if (active == snapshot.layers.end()) {
            return fail_copy(tr("The active layer is missing from the project."));
        }
        active->render = static_cast<const pvt::RenderData&>(config_);
        snapshot.canvas.width = config_.width;
        snapshot.canvas.height = config_.height;
        snapshot.canvas.block_size = config_.block_size;
        snapshot.canvas.total_frames = config_.total_frames;
        snapshot.canvas.fps = config_.fps;
        snapshot.canvas.clock = config_.clock;
        snapshot.canvas.motion_paths = config_.motion_paths;
        snapshot.canvas.output_compatibility = config_.output_compatibility;
        snapshot.output = config_.output;
        snapshot.name = project_name;

        copy = std::make_unique<pvt::ProjectDocument>();
        std::string copy_error;
        pvt::ProjectDocument copy_source = document_ != nullptr
                                               ? *document_
                                               : pvt::default_project_document();
        copy_source.project = snapshot;
        if (!pvt::make_independent_project_copy(
                copy_source, *copy, &copy_error)) {
            return fail_copy(QString::fromStdString(copy_error));
        }

        const std::size_t active_index = static_cast<std::size_t>(
            std::distance(snapshot.layers.begin(), active));
        std::optional<std::size_t> solo_index;
        if (solo_layer_uuid_) {
            const auto found = std::find_if(
                snapshot.layers.begin(), snapshot.layers.end(),
                [this](const pvt::LayerConfig& layer) {
                    return layer.uuid == *solo_layer_uuid_;
                });
            if (found != snapshot.layers.end()) {
                solo_index = static_cast<std::size_t>(
                    std::distance(snapshot.layers.begin(), found));
            }
        }

        if (open_copy) {
            // Prepare every allocating piece of replacement state before the
            // save. Once persistence succeeds, switching documents is a
            // sequence of non-throwing moves/swaps.
            opened_project = copy->project;
            if (active_index >= opened_project.layers.size()) {
                return fail_copy(
                    tr("The copied project did not preserve its active layer."));
            }
            opened_active_layer_uuid =
                opened_project.layers[active_index].uuid;
            if (solo_index && *solo_index < opened_project.layers.size()) {
                opened_solo_layer_uuid =
                    opened_project.layers[*solo_index].uuid;
            }
            opened_config = pvt::apply_global_config(
                opened_project.canvas, opened_project.output,
                opened_project.layers[active_index].render);
            opened_integer_dither_preference =
                opened_project.output.bit_depth == 32
                    ? integer_dither_preference_
                    : opened_project.output.dither_enabled;
            opened_path = path;
        }

        rememberDialogLocation(path);
        pvt::BundleSaveReport report;
        if (!pvt::save_project_document(
                *copy, path.toStdString(), &report, &copy_error)) {
            return fail_copy(QString::fromStdString(copy_error));
        }
    } catch (const std::bad_alloc&) {
        return fail_copy(
            tr("There was not enough memory to create the independent project copy."));
    } catch (const std::exception& exception) {
        return fail_copy(
            tr("Unexpected independent-copy error: %1")
                .arg(QString::fromUtf8(exception.what())));
    }

    if (!open_copy) {
        status_->setText(
            tr("Created independent version-0 project copy at %1; continuing in the original project.")
                .arg(path));
        return true;
    }

    static_assert(std::is_nothrow_move_assignable_v<pvt::ProjectConfig>);
    static_assert(std::is_nothrow_move_assignable_v<pvt::RenderConfig>);
    static_assert(std::is_nothrow_move_assignable_v<std::string>);
    static_assert(
        std::is_nothrow_move_assignable_v<std::optional<std::string>>);
    cancelMusicAnalysis();
    document_ = std::move(copy);
    project_ = std::move(opened_project);
    config_ = std::move(opened_config);
    active_layer_uuid_ = std::move(opened_active_layer_uuid);
    solo_layer_uuid_ = std::move(opened_solo_layer_uuid);
    solo_group_uuid_.reset();
    selected_group_uuid_.reset();
    current_project_path_.swap(opened_path);
    QString empty_import_path;
    imported_legacy_path_.swap(empty_import_path);
    integer_dither_preference_ = opened_integer_dither_preference;
    baseline_dirty_ = false;
    clearUndoHistory(false);
    if (undo_stack_ != nullptr) undo_stack_->setClean();
    ++document_revision_;
    updateCompatibilityWarning();
    refreshLayerList();
    refreshAll();
    if (live_workspace_ != nullptr) live_workspace_->resetRealtimeFrame();
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
    status_->setText(
        tr("Created and opened independent version-0 project at %1.").arg(path));
    return true;
}

void MainWindow::saveSetup() {
    QString editor_error;
    if (!outputEditorsValid(&editor_error)) {
        QMessageBox::warning(this, tr("Invalid output text"), editor_error);
        return;
    }
    if (current_project_path_.isEmpty()) {
        saveSetupAs();
        return;
    }
    startProjectSave(current_project_path_);
}

void MainWindow::saveSetupAs() {
    const QString filename = QString::fromStdString(
        pvt::portable_project_filename(project_.name));
    QMessageBox choice(this);
    choice.setWindowTitle(tr("Save project as"));
    choice.setIcon(QMessageBox::Question);
    choice.setText(tr("Choose the project bundle form."));
    choice.setInformativeText(
        tr("An unpacked folder is recommended for large projects because each save updates files directly without rebuilding a ZIP archive."));
    QPushButton* const folder = choice.addButton(
        tr("Unpacked Folder…"), QMessageBox::AcceptRole);
    QPushButton* const zip = choice.addButton(
        tr("ZIP File…"), QMessageBox::ActionRole);
    QPushButton* const cancel = choice.addButton(QMessageBox::Cancel);
    folder->setObjectName(QStringLiteral("saveAsUnpackedFolderButton"));
    zip->setObjectName(QStringLiteral("saveAsZipButton"));
    choice.setDefaultButton(folder);
    choice.setEscapeButton(cancel);
    choice.exec();
    if (choice.clickedButton() == cancel) return;

    QString path;
    if (choice.clickedButton() == folder) {
        const QString parent = QFileDialog::getExistingDirectory(
            this, tr("Choose parent folder for unpacked project"),
            usableDialogDirectory());
        if (parent.isEmpty()) return;
        QString directory_name = filename;
        if (directory_name.endsWith(
                QStringLiteral(".zip"), Qt::CaseInsensitive)) {
            directory_name.chop(4);
        }
        path = QDir(parent).filePath(directory_name);
    } else {
        const QString initial_path =
            QDir(usableDialogDirectory()).filePath(filename);
        path = QFileDialog::getSaveFileName(
            this, tr("Save ZIP project bundle"), initial_path,
            tr("PVT project bundle (*.zip);;All files (*)"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) {
            path.append(QStringLiteral(".zip"));
        }
    }
    startProjectSave(path);
}

void MainWindow::startProjectSave(const QString& path) {
    if (path.isEmpty() || project_io_watcher_ == nullptr
        || project_io_watcher_->isRunning() || music_analysis_active_) {
        return;
    }
    rememberDialogLocation(path);
    project_io_operation_ = ProjectIoOperation::Save;
    project_io_path_ = path;
    setProjectIoActive(true, tr("Saving %1 in the background…").arg(path));
    std::shared_ptr<pvt::ProjectDocument> staged;
    try {
        if (document_ == nullptr) {
            document_ = std::make_unique<pvt::ProjectDocument>(
                pvt::default_project_document());
        }
        staged = std::make_shared<pvt::ProjectDocument>(*document_);
        staged->project = project_;
        staged->dirty = baseline_dirty_
                        || (undo_stack_ != nullptr && !undo_stack_->isClean());
    } catch (const std::bad_alloc&) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Save failed"),
            tr("There was not enough memory to prepare the project for saving."));
        return;
    } catch (const std::exception& exception) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Save failed"),
            tr("The project could not be prepared for saving: %1")
                .arg(QString::fromUtf8(exception.what())));
        return;
    } catch (...) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Save failed"),
            tr("The project could not be prepared for background saving."));
        return;
    }
    try {
        project_io_watcher_->setFuture(QtConcurrent::run(
            [staged = std::move(staged), path] {
                ProjectIoResult result;
                result.operation = ProjectIoOperation::Save;
                result.path = path;
                result.document = staged;
                try {
                    std::string error;
                    result.ok = pvt::save_project_document(
                        *staged, path.toStdString(), &result.save_report, &error);
                    result.error = QString::fromStdString(error);
                } catch (const std::exception& exception) {
                    result.error = tr("Unexpected project-save error: %1")
                                       .arg(QString::fromUtf8(exception.what()));
                } catch (...) {
                    result.error = tr(
                        "Project saving failed because of an unexpected error.");
                }
                return result;
            }));
    } catch (const std::exception& exception) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Save failed"),
            tr("The background project-save task could not start: %1")
                .arg(QString::fromUtf8(exception.what())));
    } catch (...) {
        setProjectIoActive(false);
        QMessageBox::critical(
            this, tr("Save failed"),
            tr("The background project-save task could not be created."));
    }
}

void MainWindow::finishProjectSave(pvt::ProjectDocument saved,
                                   const pvt::BundleSaveReport& report,
                                   const QString& path) {
    document_ = std::make_unique<pvt::ProjectDocument>(std::move(saved));
    project_ = document_->project;
    current_project_path_ = QString::fromStdString(document_->source_path);
    imported_legacy_path_.clear();
    baseline_dirty_ = false;
    if (undo_stack_ != nullptr) undo_stack_->setClean();
    updateCompatibilityWarning();
    refreshVersionsPage();
    updateWindowTitle();
    if (report.validated_only) {
        if (report.compacted_storage) {
            status_->setText(
                tr("No project changes; verified current state and compacted shared music analysis at %1")
                    .arg(path));
        } else {
            status_->setText(
                tr("No changes; verified the bundle state at %1").arg(path));
        }
    } else if (report.promoted_external_change) {
        status_->setText(tr("Saved external changes/integrity mismatch as version %1 in %2")
                             .arg(report.version).arg(path));
    } else {
        status_->setText(tr("Saved version %1 to %2").arg(report.version).arg(path));
    }
}

bool MainWindow::saveProjectPath(const QString& path) {
    rememberDialogLocation(path);
    if (document_ == nullptr) {
        document_ = std::make_unique<pvt::ProjectDocument>(pvt::default_project_document());
    }
    document_->project = project_;
    document_->dirty = baseline_dirty_
                       || (undo_stack_ != nullptr && !undo_stack_->isClean());
    pvt::BundleSaveReport report;
    std::string error;
    if (!pvt::save_project_document(*document_, path.toStdString(), &report, &error)) {
        QMessageBox::critical(this, tr("Save failed"), QString::fromStdString(error));
        return false;
    }
    project_ = document_->project;
    current_project_path_ = QString::fromStdString(document_->source_path);
    imported_legacy_path_.clear();
    baseline_dirty_ = false;
    if (undo_stack_ != nullptr) undo_stack_->setClean();
    updateCompatibilityWarning();
    refreshVersionsPage();
    updateWindowTitle();
    if (report.validated_only) {
        if (report.compacted_storage) {
            status_->setText(
                tr("No project changes; verified state and compacted shared storage at %1")
                    .arg(path));
        } else {
            status_->setText(
                tr("No changes; verified the bundle state at %1").arg(path));
        }
    } else if (report.promoted_external_change) {
        status_->setText(tr("Saved external changes/integrity mismatch as version %1 in %2")
                             .arg(report.version).arg(path));
    } else {
        status_->setText(tr("Saved version %1 to %2").arg(report.version).arg(path));
    }
    return true;
}

void MainWindow::loadSetup() {
    if (!documentReplacementAllowed()) return;
    QMessageBox choice(this);
    choice.setWindowTitle(tr("Open / Import"));
    choice.setIcon(QMessageBox::Question);
    choice.setText(tr("Choose a project file or an unpacked project folder."));
    choice.setInformativeText(
        tr("Project files include ZIP bundles and legacy .pvt setups. Unpacked folders are the recommended form for large projects."));
    QPushButton* const file = choice.addButton(
        tr("Project File…"), QMessageBox::AcceptRole);
    QPushButton* const folder = choice.addButton(
        tr("Project Folder…"), QMessageBox::ActionRole);
    QPushButton* const cancel = choice.addButton(QMessageBox::Cancel);
    file->setObjectName(QStringLiteral("openProjectFileButton"));
    folder->setObjectName(QStringLiteral("openProjectFolderButton"));
    choice.setDefaultButton(file);
    choice.setEscapeButton(cancel);
    choice.exec();
    if (choice.clickedButton() == cancel) return;

    QString path;
    if (choice.clickedButton() == folder) {
        path = QFileDialog::getExistingDirectory(
            this, tr("Open unpacked project bundle"),
            usableDialogDirectory());
    } else {
        path = QFileDialog::getOpenFileName(
            this, tr("Open project or import legacy setup"),
            usableDialogDirectory(),
            tr("PVT projects (*.zip *.pvt);;Project bundles (*.zip);;Legacy setups (*.pvt);;All files (*)"));
    }
    if (path.isEmpty()) return;
    if (!confirmDiscardChanges(
            [this, path] { startProjectLoad(path); })) return;
    rememberDialogLocation(path);
    startProjectLoad(path);
}
