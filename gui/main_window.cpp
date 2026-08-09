#include "main_window.h"

#include "preview_widget.h"
#include "../src/config_codec.h"
#include "../src/project_bundle.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFuture>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>
#include <QValidator>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::size_t kMaximumNameBytes = 256;
constexpr std::size_t kMaximumPathBytes = 4095;
constexpr std::size_t kMaximumPrefixBytes = 127;
constexpr int kDefaultUndoLimit = 500;
constexpr int kMinimumUndoLimit = 10;
constexpr int kMaximumUndoLimit = 5000;
constexpr std::size_t kMaximumUndoHistoryBytes = 128U * 1024U * 1024U;

#ifndef PVT_PROGRAM_VERSION
#  define PVT_PROGRAM_VERSION "4.0.0"
#endif

std::optional<std::vector<std::uint64_t>> numeric_version(std::string_view value) {
    const std::size_t suffix = value.find_first_of("-+");
    if (suffix != std::string_view::npos) value = value.substr(0U, suffix);
    if (value.empty()) return std::nullopt;
    std::vector<std::uint64_t> parts;
    for (std::size_t begin = 0U; begin < value.size();) {
        const std::size_t end = value.find('.', begin);
        const std::string_view part = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (part.empty()) return std::nullopt;
        std::uint64_t number = 0U;
        for (const char character : part) {
            if (character < '0' || character > '9') return std::nullopt;
            const auto digit = static_cast<std::uint64_t>(character - '0');
            if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
                return std::nullopt;
            }
            number = number * 10U + digit;
        }
        parts.push_back(number);
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return parts;
}

bool program_version_is_newer(std::string_view candidate) {
    const auto left = numeric_version(candidate);
    const auto right = numeric_version(PVT_PROGRAM_VERSION);
    if (!left || !right) return false;
    const std::size_t count = std::max(left->size(), right->size());
    for (std::size_t index = 0U; index < count; ++index) {
        const auto left_part = index < left->size() ? (*left)[index] : 0U;
        const auto right_part = index < right->size() ? (*right)[index] : 0U;
        if (left_part != right_part) return left_part > right_part;
    }
    return false;
}

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
    return QString::number(index + 1U) + QStringLiteral(". ")
           + QString::fromStdString(wave.name) + QStringLiteral("  [")
           + (wave.enabled ? QStringLiteral("on") : QStringLiteral("off"))
           + QStringLiteral(", ")
           + (wave.synchronized ? QStringLiteral("sync") : QStringLiteral("free"))
           + QLatin1Char(']');
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
    return QString::number(index + 1U) + QStringLiteral(". ")
           + QString::fromStdString(effect.name) + QStringLiteral("  [")
           + QString::fromUtf8(pvt::effect_type_name(effect.type))
           + QStringLiteral(", ")
           + QString::fromUtf8(pvt::effect_space_name(effect.space))
           + QStringLiteral(", ")
           + (effect.enabled ? QStringLiteral("on") : QStringLiteral("off"))
           + QStringLiteral(", ")
           + (effect.synchronized ? QStringLiteral("sync") : QStringLiteral("free"))
           + QLatin1Char(']');
}

std::size_t saturating_add(std::size_t left, std::size_t right) {
    return right > std::numeric_limits<std::size_t>::max() - left
               ? std::numeric_limits<std::size_t>::max()
               : left + right;
}

std::size_t estimated_string_bytes(const std::string& value) {
    return saturating_add(sizeof(std::string), value.capacity() + 1U);
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
    bytes = saturating_add(bytes, estimated_string_bytes(render.palette.name));
    bytes = saturating_add(
        bytes, render.palette.colors.capacity() * sizeof(pvt::PaletteColor));
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
    bytes = saturating_add(bytes, estimated_output_bytes(project.output));
    bytes = saturating_add(bytes,
                           project.layers.capacity() * sizeof(pvt::LayerConfig));
    for (const auto& layer : project.layers) {
        bytes = saturating_add(bytes, estimated_string_bytes(layer.uuid));
        bytes = saturating_add(bytes, estimated_string_bytes(layer.name));
        bytes = saturating_add(bytes, estimated_render_data_bytes(layer.render));
    }
    return bytes;
}

bool render_data_equal(const pvt::RenderData& left,
                       const pvt::RenderData& right) {
    std::string left_bytes;
    std::string right_bytes;
    return pvt::detail::serialize_layer_config(left, left_bytes, nullptr)
           && pvt::detail::serialize_layer_config(right, right_bytes, nullptr)
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
        || !output_data_equal(left.canvas, left.output,
                              right.canvas, right.output)) {
        return false;
    }
    for (std::size_t index = 0U; index < left.layers.size(); ++index) {
        const auto& a = left.layers[index];
        const auto& b = right.layers[index];
        if (a.uuid != b.uuid || a.file_id != b.file_id || a.name != b.name
            || a.enabled != b.enabled || a.blend_mode != b.blend_mode
            || a.opacity != b.opacity || !render_data_equal(a.render, b.render)) {
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

bool configuration_requires_alpha(const pvt::RenderConfig& config) {
    if (config.surface.enabled && config.surface.mapping != pvt::SurfaceMapping::Plane
        && config.surface.curvature > 0.0) {
        return true;
    }
    return std::any_of(config.effects.begin(), config.effects.end(), [](const auto& effect) {
        return effect.enabled && effect.intensity > 0.0 && effect.magnitude > 0.0
               && effect.type != pvt::EffectType::Glow
               && effect.type != pvt::EffectType::BlockScale
               && effect.edge_mode == pvt::EdgeMode::Alpha;
    });
}

void set_form_label(QFormLayout* form, QWidget* field, const QString& text) {
    if (auto* label = qobject_cast<QLabel*>(form->labelForField(field))) {
        label->setText(text);
    }
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
    effect.space = random_chance(random, 0.25)
                       ? pvt::EffectSpace::Surface
                       : pvt::EffectSpace::Texture;
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
    }
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
    if (project_.layers.empty()) {
        project_ = pvt::default_project();
    }
    document_ = std::make_unique<pvt::ProjectDocument>(pvt::default_project_document());
    document_->project = project_;
    document_->dirty = false;
    active_layer_uuid_ = project_.layers.front().uuid;
    loadActiveConfiguration();

    undo_stack_ = new QUndoStack(this);
    const int undo_limit = std::clamp(
        QSettings().value(QStringLiteral("preferences/undoLimit"), kDefaultUndoLimit).toInt(),
        kMinimumUndoLimit, kMaximumUndoLimit);
    undo_stack_->setUndoLimit(undo_limit);

    resize(1420, 860);

    preview_timer_ = new QTimer(this);
    preview_timer_->setSingleShot(true);
    preview_timer_->setInterval(70);
    playback_timer_ = new QTimer(this);
    playback_timer_->setTimerType(Qt::PreciseTimer);
    preview_watcher_ = new QFutureWatcher<PreviewResult>(this);
    export_watcher_ = new QFutureWatcher<ExportResult>(this);
    preview_cancel_ = std::make_shared<std::atomic_bool>(false);

    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    auto* splitter = new QSplitter(Qt::Horizontal);
    preview_ = new PreviewWidget;
    tabs_ = new QTabWidget;
    tabs_->addTab(createWavePage(), tr("Waves"));
    tabs_->addTab(createSwingPage(), tr("Swings"));
    tabs_->addTab(createEffectPage(), tr("Effects"));
    tabs_->addTab(createLayerSettingsPage(), tr("Layer Render"));
    tabs_->addTab(createOutputPage(), tr("Output"));
    tabs_->addTab(createVersionsPage(), tr("Versions"));
    tabs_->setMinimumWidth(440);
    splitter->addWidget(preview_);
    splitter->addWidget(tabs_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);
    outer->addWidget(createTimeline());
    setCentralWidget(central);
    createLayerDock();

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
        if (result.document_revision == document_revision_
            && (result.generation == preview_generation_ || playback_timer_->isActive())) {
            if (result.error.isEmpty()) {
                if (playback_timer_->isActive() && last_previewed_frame_ >= 0
                    && result.frame != last_previewed_frame_) {
                    playback_preview_advanced_ = true;
                }
                last_previewed_frame_ = result.frame;
                preview_->setPreview(result.image);
                status_->setText(tr("Preview frame %1/%2")
                                     .arg(result.frame + 1)
                                     .arg(project_.canvas.total_frames));
            } else {
                status_->setText(result.error);
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
    connect(undo_stack_, &QUndoStack::cleanChanged, this,
            [this] { updateWindowTitle(); });
    refreshLayerList();
    refreshAll();
    undo_stack_->setClean();
    updateWindowTitle();
    updateCompatibilityWarning();
    restoreUserSettings();
    schedulePreview();
}

MainWindow::~MainWindow() {
    if (preview_cancel_ != nullptr) {
        preview_cancel_->store(true, std::memory_order_relaxed);
    }
    cancel_export_.store(true);
    preview_watcher_->waitForFinished();
    export_watcher_->waitForFinished();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (export_watcher_ != nullptr && export_watcher_->isRunning()) {
        close_after_export_ = true;
        cancel_export_.store(true);
        status_->setText(tr("Cancelling export before closing…"));
        event->ignore();
        return;
    }
    if (!confirmDiscardChanges()) {
        event->ignore();
        return;
    }
    saveUserSettings();
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

QWidget* MainWindow::createSwingPage() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Swing modulators reshape the shared synchronized clock. Add, remove, "
           "duplicate, and reorder them to layer loop-safe rhythm variations. "
           "Set a local radius above zero to place the numbered source/UV circle in the preview. "
           "Localized Swings drive source waves and Texture effects; Mapped-object effects use "
           "the global synchronized clock because projected screen points do not map to one UV."));
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
    swing_center_x_ = real_editor(-10.0, 10.0, 4, 0.01);
    swing_center_y_ = real_editor(-10.0, 10.0, 4, 0.01);
    swing_radius_ = real_editor(0.0, 10.0, 4, 0.01);
    swing_radius_->setSpecialValueText(tr("Whole layer (0)"));
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
    form->addRow(tr("Local radius"), swing_radius_);
    layout->addWidget(properties);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.swings.size() >= pvt::kMaximumSwings) {
            QMessageBox::warning(this, tr("Swing limit"),
                                 tr("The safety limit is 64 swing modulators."));
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
    auto* layout = new QVBoxLayout(page);
    auto* explanation = new QLabel(
        tr("Effects keep their list order within two stages: Texture effects run before "
           "surface wrapping; Mapped object effects run after the surface and layer transform "
           "and can move or deform the complete primitive silhouette. Centers and local radii "
           "are draggable in the preview."));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    effect_list_ = new QListWidget;
    effect_list_->setAlternatingRowColors(true);
    layout->addWidget(effect_list_, 1);

    auto* add_row = new QHBoxLayout;
    add_effect_type_ = new QComboBox;
    for (const auto type : {pvt::EffectType::EndlessZoom, pvt::EffectType::Ripple,
                            pvt::EffectType::Shake, pvt::EffectType::FlagWave,
                            pvt::EffectType::Glow, pvt::EffectType::BlockScale}) {
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
                            pvt::EffectType::Glow, pvt::EffectType::BlockScale}) {
        add_enum_item(effect_type_, QString::fromUtf8(pvt::effect_type_name(type)), type);
    }
    effect_space_ = new QComboBox;
    add_enum_item(effect_space_, tr("Texture (before surface)"),
                  pvt::EffectSpace::Texture);
    add_enum_item(effect_space_, tr("Mapped object (after surface)"),
                  pvt::EffectSpace::Surface);
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
    effect_area_radius_ = real_editor(0.0, 10.0, 4, 0.01);
    effect_area_radius_->setSpecialValueText(tr("Whole layer (0)"));
    effect_form_->addRow(tr("Name"), effect_name_);
    effect_form_->addRow(effect_enabled_);
    effect_form_->addRow(effect_sync_);
    effect_form_->addRow(tr("Type"), effect_type_);
    effect_form_->addRow(tr("Apply to"), effect_space_);
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
    effect_form_->addRow(tr("Local area radius"), effect_area_radius_);
    scroll->setWidget(properties);
    layout->addWidget(scroll, 2);

    connect(add, &QPushButton::clicked, this, [this] {
        if (config_.effects.size() >= pvt::kMaximumEffects) {
            QMessageBox::warning(this, tr("Effect limit"),
                                 tr("The safety limit is 256 effects."));
            return;
        }
        auto before = captureActiveState();
        const auto type = static_cast<pvt::EffectType>(add_effect_type_->currentData().toInt());
        auto effect = pvt::default_effect(type);
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
    return page;
}

QWidget* MainWindow::createLayerSettingsPage() {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* contents = new QWidget;
    auto* layout = new QVBoxLayout(contents);

    auto* rhythm_group = new QGroupBox(tr("Rhythm and color timing"));
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
    layout->addWidget(transform_group);

    auto* palette_group = new QGroupBox(tr("Layer palette"));
    auto* palette_layout = new QVBoxLayout(palette_group);
    auto* palette_form = new QFormLayout;
    palette_enabled_ = new QCheckBox(tr("Restrict this layer to palette colors"));
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
    palette_buttons->addWidget(add_color);
    palette_buttons->addWidget(edit_color);
    palette_buttons->addWidget(remove_color);
    palette_layout->addLayout(palette_buttons);
    auto* palette_help = new QLabel(
        tr("Colors are authored in sRGB and embedded in this layer. The renderer "
           "chooses the nearest palette color after effects, surface lighting, and quantization."));
    palette_help->setWordWrap(true);
    palette_layout->addWidget(palette_help);
    layout->addWidget(palette_group);

    auto* surface_group = new QGroupBox(tr("3D surface wrapping"));
    auto* surface = new QFormLayout(surface_group);
    surface_enabled_ = new QCheckBox(tr("Surface mapping enabled"));
    surface_mapping_ = new QComboBox;
    add_enum_item(surface_mapping_, tr("Plane"), pvt::SurfaceMapping::Plane);
    add_enum_item(surface_mapping_, tr("Cylinder"), pvt::SurfaceMapping::Cylinder);
    add_enum_item(surface_mapping_, tr("Sphere"), pvt::SurfaceMapping::Sphere);
    add_enum_item(surface_mapping_, tr("Cube"), pvt::SurfaceMapping::Cube);
    add_enum_item(surface_mapping_, tr("Custom OBJ"), pvt::SurfaceMapping::CustomObj);
    auto* obj_path_row = new QWidget;
    auto* obj_path_layout = new QHBoxLayout(obj_path_row);
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
    auto* obj_browse = new QPushButton(tr("Browse…"));
    obj_path_layout->addWidget(surface_obj_path_, 1);
    obj_path_layout->addWidget(obj_browse);
    surface_rotations_ = integer_editor(-1000, 1000);
    surface_phase_ = real_editor(-36000.0, 36000.0, 3, 1.0);
    surface_curvature_ = real_editor(0.0, 1.0);
    surface_lighting_ = real_editor(0.0, 10.0);
    surface->addRow(surface_enabled_);
    surface->addRow(tr("Surface"), surface_mapping_);
    surface->addRow(tr("OBJ file"), obj_path_row);
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
    alpha_enabled_ = new QCheckBox(tr("Procedural alpha modulation"));
    alpha_enabled_->setToolTip(
        tr("Controls opacity generated by this layer. The project output channel "
           "selection is configured separately on the Output tab."));
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

    layout->addStretch();
    scroll->setWidget(contents);

    connect(obj_browse, &QPushButton::clicked, this, [this] {
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
            surface_obj_path_->setText(selected);
            applyGlobalEditor(surface_obj_path_);
        }
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
    connect(palette_colors_, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { editSelectedPaletteColor(); });

    return scroll;
}

QWidget* MainWindow::createOutputPage() {
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

    auto* output_group = new QGroupBox(tr("Export"));
    auto* output = new QFormLayout(output_group);
    bit_depth_ = new QComboBox;
    bit_depth_->addItem(tr("8-bit PNG"), 8);
    bit_depth_->addItem(tr("16-bit PNG"), 16);
    bit_depth_->addItem(tr("32-bit float EXR"), 32);
    png_compression_ = integer_editor(0, 9);
    png_compression_->setSpecialValueText(tr("Off (0)"));
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
    first_frame_ = integer_editor(0, 1000000000);
    filename_digits_ = integer_editor(1, 12);
    overwrite_ = new QCheckBox(tr("Overwrite matching frames"));
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
    layout->addWidget(output_group);
    layout->addStretch();
    scroll->setWidget(contents);

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
    return scroll;
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
    compare_layout->addLayout(selectors);
    version_diff_ = new QPlainTextEdit;
    version_diff_->setReadOnly(true);
    version_diff_->setPlaceholderText(
        tr("Save at least two versions to compare project, output, layer, and render fields."));
    compare_layout->addWidget(version_diff_, 1);
    layout->addWidget(compare_group, 1);

    connect(version_before_, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshVersionDiff);
    connect(version_after_, &QComboBox::currentIndexChanged,
            this, &MainWindow::refreshVersionDiff);
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
    refreshVersionDiff();
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

void MainWindow::makeSelectedVersionCurrent() {
    if (document_ == nullptr || version_list_ == nullptr
        || version_list_->currentItem() == nullptr) return;
    if (!documentReplacementAllowed()) return;
    // Saving from the dirty confirmation refreshes the list, so capture the
    // user's target before that refresh can change the selection.
    const auto version = version_list_->currentItem()->data(Qt::UserRole).toULongLong();
    if (!confirmDiscardChanges()) return;
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
    baseline_dirty_ = document_->dirty;
    current_project_path_ = QString::fromStdString(document_->source_path);
    updateCompatibilityWarning();
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    refreshVersionsPage();
    schedulePreview();
}

void MainWindow::revertSelectedVersion() {
    if (document_ == nullptr || version_list_ == nullptr
        || version_list_->currentItem() == nullptr) return;
    if (!documentReplacementAllowed()) return;
    const auto version = version_list_->currentItem()->data(Qt::UserRole).toULongLong();
    if (!confirmDiscardChanges()) return;
    const auto choice = QMessageBox::question(
        this, tr("Revert as a new version?"),
        tr("Create a new version copied from version %1 and make it current?").arg(version),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (choice != QMessageBox::Yes) return;
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
    baseline_dirty_ = false;
    current_project_path_ = QString::fromStdString(document_->source_path);
    updateCompatibilityWarning();
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    ++document_revision_;
    refreshLayerList();
    refreshAll();
    refreshVersionsPage();
    schedulePreview();
}

void MainWindow::createLayerDock() {
    layers_dock_ = new QDockWidget(tr("Project & Layers"), this);
    layers_dock_->setObjectName(QStringLiteral("projectLayersDock"));
    layers_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    layers_dock_->setMinimumWidth(250);

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

    compatibility_warning_label_ = new QLabel;
    compatibility_warning_label_->setWordWrap(true);
    compatibility_warning_label_->setStyleSheet(
        QStringLiteral("QLabel { background: #5b4815; color: #fff2b2; "
                       "border: 1px solid #c89b24; border-radius: 4px; padding: 6px; }"));
    compatibility_warning_label_->hide();
    layout->addWidget(compatibility_warning_label_);

    auto* explanation = new QLabel(
        tr("Top rows paint over the layers below them. Blend and opacity apply to the "
           "selected layer; Solo is a preview-only aid."));
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
                            pvt::BlendMode::Add}) {
        QString label = QString::fromUtf8(pvt::blend_mode_name(mode));
        if (mode == pvt::BlendMode::Normal) {
            label = tr("Normal (none)");
        }
        add_enum_item(layer_blend_, label, mode);
    }
    layer_opacity_ = real_editor(0.0, 100.0, 1, 5.0);
    layer_opacity_->setSuffix(tr("%"));
    selected_form->addRow(tr("Name"), layer_name_);
    selected_form->addRow(layer_enabled_);
    selected_form->addRow(layer_solo_);
    selected_form->addRow(tr("Blend"), layer_blend_);
    selected_form->addRow(tr("Opacity"), layer_opacity_);
    layout->addWidget(selected);

    layers_dock_->setWidget(contents);
    addDockWidget(Qt::RightDockWidgetArea, layers_dock_);

    connect(layer_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (populating_ || row < 0) {
            return;
        }
        if (auto* item = layer_list_->item(row)) {
            selectLayer(item->data(Qt::UserRole).toString().toStdString());
        }
    });
    connect(project_name_, &QLineEdit::editingFinished,
            this, &MainWindow::finishProjectNameEdit);
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
        if (layer == nullptr || edited.isEmpty() || !valid_text(edited, TextRule::Name)) {
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
        if (layer == nullptr || layer->enabled == checked) return;
        const std::string uuid = layer->uuid;
        const bool before = layer->enabled;
        const auto before_output = project_.output;
        layer->enabled = checked;
        ensureAlphaForTransparency();
        syncProjectGlobals();
        const auto after_output = project_.output;
        recordUndo(checked ? tr("Show layer") : tr("Hide layer"),
                   [this, uuid, before, before_output] { if (auto* value = findLayer(uuid)) value->enabled = before; project_.output = before_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); },
                   [this, uuid, checked, after_output] { if (auto* value = findLayer(uuid)) value->enabled = checked; project_.output = after_output; loadActiveConfiguration(); refreshLayerList(); refreshAll(); noteDocumentChange(); schedulePreview(); });
        noteDocumentChange();
        refreshLayerList();
        schedulePreview();
    });
    connect(layer_solo_, &QCheckBox::toggled, this, [this](bool checked) {
        if (populating_) return;
        solo_layer_uuid_ = checked ? std::optional<std::string>(active_layer_uuid_) : std::nullopt;
        ++document_revision_;
        refreshLayerList();
        schedulePreview();
    });
    connect(layer_blend_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_) return;
        auto* layer = activeLayer();
        if (layer == nullptr) return;
        const auto after = static_cast<pvt::BlendMode>(layer_blend_->currentData().toInt());
        const auto before = layer->blend_mode;
        if (before == after) return;
        const std::string uuid = layer->uuid;
        layer->blend_mode = after;
        recordUndo(tr("Change layer blend"),
                   [this, uuid, before] { if (auto* value = findLayer(uuid)) value->blend_mode = before; refreshLayerList(); noteDocumentChange(); schedulePreview(); },
                   [this, uuid, after] { if (auto* value = findLayer(uuid)) value->blend_mode = after; refreshLayerList(); noteDocumentChange(); schedulePreview(); });
        noteDocumentChange();
        refreshLayerList();
        schedulePreview();
    });
    connect(layer_opacity_, &QDoubleSpinBox::valueChanged, this, [this](double percent) {
        if (populating_) return;
        auto* layer = activeLayer();
        if (layer == nullptr) return;
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
    connect(up, &QPushButton::clicked, this, [this] { moveActiveLayer(1); });
    connect(down, &QPushButton::clicked, this, [this] { moveActiveLayer(-1); });
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
            playback_preview_advanced_ = false;
            playback_timer_->start(std::max(1, static_cast<int>(std::lround(1000.0 / config_.fps))));
            play_button_->setText(tr("Pause"));
        }
        schedulePreview();
    });
    connect(timeline_, &QSlider::valueChanged, this, [this](int frame) {
        frame_label_->setText(tr("%1 / %2").arg(frame + 1).arg(config_.total_frames));
        schedulePreview();
    });
    frame_label_->setText(tr("1 / %1").arg(config_.total_frames));
    return widget;
}

void MainWindow::createToolbar() {
    auto* file_menu = menuBar()->addMenu(tr("&File"));
    auto* edit_menu = menuBar()->addMenu(tr("&Edit"));
    auto* view_menu = menuBar()->addMenu(tr("&View"));
    auto* toolbar = addToolBar(tr("Project"));
    toolbar->setObjectName(QStringLiteral("projectToolbar"));
    toolbar->setMovable(false);
    auto* new_action = new QAction(tr("New Project"), this);
    auto* open_action = new QAction(tr("Open / Import…"), this);
    auto* open_folder_action = new QAction(tr("Open Bundle Folder…"), this);
    auto* save_action = new QAction(tr("Save…"), this);
    auto* save_as_action = new QAction(tr("Save As…"), this);
    new_action->setShortcut(QKeySequence::New);
    open_action->setShortcut(QKeySequence::Open);
    save_action->setShortcut(QKeySequence::Save);
    save_as_action->setShortcut(QKeySequence::SaveAs);
    file_menu->addActions({new_action, open_action, open_folder_action,
                           save_action, save_as_action});
    file_menu->addSeparator();
    toolbar->addAction(new_action);
    toolbar->addAction(open_action);
    toolbar->addAction(save_action);
    toolbar->addSeparator();
    auto* randomize_values_action = toolbar->addAction(tr("Randomize values"));
    randomize_values_action->setToolTip(
        tr("Randomize bounded, loop-safe parameters while preserving each item's "
           "name, type, enabled state, and position in its stack."));
    auto* randomize_mix_action = toolbar->addAction(tr("Randomize mix"));
    randomize_mix_action->setToolTip(
        tr("Create a new bounded mix of waves, swing waveforms, effect types, and "
           "enabled items."));
    toolbar->addSeparator();
    auto* export_action = toolbar->addAction(tr("Export sequence"));
    auto* cancel_action = toolbar->addAction(tr("Cancel export"));
    cancel_action->setEnabled(false);
    file_menu->addSeparator();
    file_menu->addAction(export_action);

    auto* undo_action = undo_stack_->createUndoAction(this, tr("Undo"));
    auto* redo_action = undo_stack_->createRedoAction(this, tr("Redo"));
    undo_action->setShortcut(QKeySequence::Undo);
    redo_action->setShortcut(QKeySequence::Redo);
    edit_menu->addAction(undo_action);
    edit_menu->addAction(redo_action);
    edit_menu->addSeparator();
    auto* undo_limit_action = edit_menu->addAction(tr("Undo History Limit…"));
    connect(undo_limit_action, &QAction::triggered, this, &MainWindow::editUndoLimit);
    view_menu->addAction(layers_dock_->toggleViewAction());

    connect(new_action, &QAction::triggered, this, [this] {
        if (!documentReplacementAllowed()) return;
        if (!confirmDiscardChanges()) return;
        project_ = pvt::default_project();
        active_layer_uuid_ = project_.layers.front().uuid;
        solo_layer_uuid_.reset();
        current_project_path_.clear();
        imported_legacy_path_.clear();
        document_ = std::make_unique<pvt::ProjectDocument>(pvt::default_project_document());
        document_->project = project_;
        document_->dirty = false;
        baseline_dirty_ = false;
        updateCompatibilityWarning();
        loadActiveConfiguration();
        clearUndoHistory(false);
        undo_stack_->setClean();
        refreshLayerList();
        refreshAll();
        ++document_revision_;
        updateWindowTitle();
        schedulePreview();
    });
    connect(open_action, &QAction::triggered, this, &MainWindow::loadSetup);
    connect(open_folder_action, &QAction::triggered, this, [this] {
        if (!documentReplacementAllowed()) return;
        if (!confirmDiscardChanges()) return;
        const QString path = QFileDialog::getExistingDirectory(
            this, tr("Open unpacked project bundle"), usableDialogDirectory());
        if (path.isEmpty()) return;
        rememberDialogLocation(path);
        QString error;
        if (!loadProjectPath(path, &error)) {
            QMessageBox::critical(this, tr("Load failed"),
                                  tr("The active project was not changed.\n\n%1").arg(error));
            return;
        }
        if (compatibility_warning_.isEmpty()) {
            status_->setText(tr("Loaded %1").arg(path));
        }
    });
    connect(save_action, &QAction::triggered, this, &MainWindow::saveSetup);
    connect(save_as_action, &QAction::triggered, this, &MainWindow::saveSetupAs);
    connect(randomize_values_action, &QAction::triggered, this,
            &MainWindow::randomizeExistingStackSettings);
    connect(randomize_mix_action, &QAction::triggered, this,
            &MainWindow::randomizeStackComposition);
    connect(export_action, &QAction::triggered, this, [this, export_action, cancel_action] {
        if (export_watcher_->isRunning()) {
            return;
        }
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
        status_->setText(tr("Cancelling export…"));
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
    for (auto* editor : {wave_x_, wave_y_, wave_amplitude_, wave_frequency_, wave_phase_,
                         wave_direction_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyWaveEditor(editor); });
    }
    connect(wave_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyWaveEditor(wave_cycles_); });

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
    connect(effect_type_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_type_); });
    connect(effect_space_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_space_); });
    connect(effect_edge_, &QComboBox::currentIndexChanged, this,
            [this] { applyEffectEditor(effect_edge_); });
    connect(effect_cycles_, &QSpinBox::valueChanged, this,
            [this] { applyEffectEditor(effect_cycles_); });
    for (auto* editor : {effect_phase_, effect_intensity_, effect_magnitude_,
                         effect_frequency_, effect_secondary_, effect_center_x_,
                         effect_center_y_, effect_angle_, effect_radius_, effect_threshold_,
                         effect_knee_, effect_area_radius_}) {
        connect(editor, &QDoubleSpinBox::valueChanged, this,
                [this, editor] { applyEffectEditor(editor); });
    }

    for (auto* editor : {width_, height_, block_size_, frames_, spiral_arms_, hue_cycles_,
                         surface_rotations_, quantization_levels_, alpha_cycles_, first_frame_,
                         filename_digits_, png_compression_}) {
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
                         alpha_enabled_, dither_enabled_, write_alpha_, overwrite_,
                         transform_flip_horizontal_, transform_flip_vertical_,
                         palette_enabled_}) {
        connect(editor, &QCheckBox::toggled, this,
                [this, editor] { applyGlobalEditor(editor); });
    }
    for (auto* editor : {surface_mapping_, quantization_mode_, bit_depth_, dither_method_,
                         transform_mirror_}) {
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
    connect(surface_obj_path_, &QLineEdit::textEdited, this, [this] {
        if (surface_obj_path_->hasAcceptableInput()) {
            applyGlobalEditor(surface_obj_path_);
        }
    });
    connect(palette_name_, &QLineEdit::editingFinished, this, [this] {
        if (palette_name_->hasAcceptableInput()) {
            applyGlobalEditor(palette_name_);
        }
    });
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 1) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Swings);
        } else if (index == 2) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Effects);
        } else if (index == 0) {
            preview_->setOverlayMode(PreviewWidget::OverlayMode::Waves);
        }
    });

    connect(preview_, &PreviewWidget::waveSelected, this, [this](std::size_t index) {
        if (index < config_.waves.size()) {
            wave_list_->setCurrentRow(static_cast<int>(index));
            tabs_->setCurrentIndex(0);
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
                    tabs_->setCurrentIndex(1);
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
                    effect_list_->setCurrentRow(static_cast<int>(index));
                    tabs_->setCurrentIndex(2);
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
    project_.output = config_.output;
}

void MainWindow::selectLayer(const std::string& uuid) {
    if (uuid == active_layer_uuid_ || findLayer(uuid) == nullptr) {
        loadLayerEditors();
        return;
    }
    active_layer_uuid_ = uuid;
    loadActiveConfiguration();
    refreshAll();
    refreshLayerList();
    schedulePreview();
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
    for (std::size_t display = 0; display < project_.layers.size(); ++display) {
        const std::size_t index = project_.layers.size() - 1U - display;
        const auto& layer = project_.layers[index];
        QString detail = QString::fromStdString(layer.name) + QStringLiteral("  [")
                         + (layer.enabled ? tr("on") : tr("off"))
                         + QStringLiteral(", ")
                         + (index == 0U
                                ? tr("base")
                                : QString::fromUtf8(
                                      pvt::blend_mode_name(layer.blend_mode)))
                         + QStringLiteral(", ")
                         + QString::number(layer.opacity * 100.0, 'f', 0)
                         + QStringLiteral("%]");
        if (solo_layer_uuid_ && *solo_layer_uuid_ == layer.uuid) {
            detail.append(tr("  [SOLO]"));
        }
        auto* item = new QListWidgetItem(detail, layer_list_);
        item->setData(Qt::UserRole, QString::fromStdString(layer.uuid));
        item->setToolTip(index == 0U
                             ? tr("Bottom/base layer. Its blend mode has no lower layer to affect.")
                             : tr("This layer is composited over every enabled layer below it."));
        if (layer.uuid == active_layer_uuid_) {
            selected_row = static_cast<int>(display);
        }
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
    const bool available = layer != nullptr;
    for (auto* widget : std::initializer_list<QWidget*>{
             layer_name_, layer_enabled_, layer_solo_, layer_blend_, layer_opacity_}) {
        widget->setEnabled(available);
    }
    if (layer != nullptr) {
        layer_name_->setText(QString::fromStdString(layer->name));
        layer_enabled_->setChecked(layer->enabled);
        layer_solo_->setChecked(solo_layer_uuid_ && *solo_layer_uuid_ == layer->uuid);
        select_enum(layer_blend_, layer->blend_mode);
        layer_opacity_->setValue(layer->opacity * 100.0);
        const auto index = static_cast<std::size_t>(
            std::distance(static_cast<const pvt::LayerConfig*>(project_.layers.data()),
                          layer));
        layer_blend_->setEnabled(index != 0U);
        layer_blend_->setToolTip(index == 0U
                                     ? tr("The bottom layer has nothing beneath it to blend with.")
                                     : tr("Blend applies this layer over the enabled layers below."));
    }
    populating_ = was_populating;
}

void MainWindow::addLayer() {
    if (project_.layers.size() >= pvt::kMaximumLayers) {
        QMessageBox::warning(this, tr("Layer limit"),
                             tr("The safety limit is %1 layers.").arg(pvt::kMaximumLayers));
        return;
    }
    auto before = project_;
    const std::string before_active = active_layer_uuid_;
    auto layer = pvt::default_layer(project_.layers.size());
    layer.uuid = pvt::generate_uuid();
    layer.file_id = pvt::allocate_layer_file_id(project_);
    layer.name = tr("Layer %1").arg(project_.layers.size() + 1U).toStdString();
    active_layer_uuid_ = layer.uuid;
    project_.layers.push_back(std::move(layer));
    project_.output.write_alpha = true;
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    recordProjectStateChange(tr("Add layer"), std::move(before), before_active);
    schedulePreview();
}

void MainWindow::duplicateLayer() {
    const auto* source = activeLayer();
    if (source == nullptr || project_.layers.size() >= pvt::kMaximumLayers) {
        return;
    }
    auto before = project_;
    const std::string before_active = active_layer_uuid_;
    const auto source_index = static_cast<std::size_t>(
        std::distance(static_cast<const pvt::LayerConfig*>(project_.layers.data()),
                      source));
    auto layer = *source;
    layer.uuid = pvt::generate_uuid();
    layer.file_id = pvt::allocate_layer_file_id(project_);
    append_copy_suffix(layer.name);
    active_layer_uuid_ = layer.uuid;
    project_.layers.insert(project_.layers.begin()
                               + static_cast<std::ptrdiff_t>(source_index + 1U),
                           std::move(layer));
    project_.output.write_alpha = true;
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    recordProjectStateChange(tr("Duplicate layer"), std::move(before), before_active);
    schedulePreview();
}

void MainWindow::removeLayer() {
    if (project_.layers.size() <= 1U) {
        QMessageBox::information(this, tr("Keep one layer"),
                                 tr("A project must always contain at least one layer."));
        return;
    }
    auto* layer = activeLayer();
    if (layer == nullptr) return;
    auto before = project_;
    const std::string before_active = active_layer_uuid_;
    const auto index = static_cast<std::size_t>(
        std::distance(project_.layers.data(), layer));
    project_.layers.erase(project_.layers.begin() + static_cast<std::ptrdiff_t>(index));
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
}

void MainWindow::moveActiveLayer(int direction) {
    auto* layer = activeLayer();
    if (layer == nullptr || direction == 0) return;
    const auto index = static_cast<std::ptrdiff_t>(
        std::distance(project_.layers.data(), layer));
    const auto target = index + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(project_.layers.size())) return;
    auto before = project_;
    const std::string before_active = active_layer_uuid_;
    std::swap(project_.layers[static_cast<std::size_t>(index)],
              project_.layers[static_cast<std::size_t>(target)]);
    refreshLayerList();
    recordProjectStateChange(direction > 0 ? tr("Move layer up") : tr("Move layer down"),
                             std::move(before), before_active);
    schedulePreview();
}

MainWindow::ActiveDocumentState MainWindow::captureActiveState() const {
    ActiveDocumentState state;
    if (const auto* layer = activeLayer()) {
        state.render = layer->render;
    }
    state.canvas = project_.canvas;
    state.output = project_.output;
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
    if (findLayer(active_layer_uuid_) == nullptr) {
        active_layer_uuid_ = layer_uuid;
    }
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    restoring_undo_ = false;
    noteDocumentChange();
    schedulePreview();
}

void MainWindow::recordActiveStateChange(const QString& text,
                                         ActiveDocumentState before,
                                         const QString& merge_key) {
    if (restoring_undo_) return;
    const std::string uuid = active_layer_uuid_;
    ActiveDocumentState after = captureActiveState();
    if (render_data_equal(before.render, after.render)
        && output_data_equal(before.canvas, before.output,
                             after.canvas, after.output)) {
        return;
    }
    const std::size_t before_bytes = saturating_add(
        estimated_render_data_bytes(before.render),
        estimated_output_bytes(before.output));
    const std::size_t after_bytes = saturating_add(
        estimated_render_data_bytes(after.render),
        estimated_output_bytes(after.output));
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

void MainWindow::restoreProjectState(const pvt::ProjectConfig& state,
                                     const std::string& active_uuid) {
    restoring_undo_ = true;
    project_ = state;
    if (project_.layers.empty()) {
        project_.layers.push_back(pvt::default_layer(0));
    }
    active_layer_uuid_ = findLayer(active_uuid) != nullptr
                             ? active_uuid : project_.layers.back().uuid;
    if (solo_layer_uuid_ && findLayer(*solo_layer_uuid_) == nullptr) {
        solo_layer_uuid_.reset();
    }
    loadActiveConfiguration();
    refreshLayerList();
    refreshAll();
    restoring_undo_ = false;
    noteDocumentChange();
    schedulePreview();
}

void MainWindow::recordProjectStateChange(const QString& text,
                                          pvt::ProjectConfig before,
                                          const std::string& before_active_uuid) {
    if (restoring_undo_) return;
    pvt::ProjectConfig after = project_;
    if (project_config_equal(before, after)
        && before_active_uuid == active_layer_uuid_) {
        return;
    }
    const std::string after_active = active_layer_uuid_;
    const std::size_t payload_bytes = saturating_add(
        saturating_add(estimated_project_bytes(before),
                       estimated_project_bytes(after)),
        saturating_add(estimated_string_bytes(before_active_uuid),
                       estimated_string_bytes(after_active)));
    auto before_state = std::make_shared<pvt::ProjectConfig>(std::move(before));
    auto after_state = std::make_shared<pvt::ProjectConfig>(std::move(after));
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
    if (estimated_payload_bytes > kMaximumUndoHistoryBytes) {
        clearUndoHistory(true);
        baseline_dirty_ = true;
        if (status_ != nullptr) {
            status_->setText(
                tr("Change kept, but its snapshot is too large for the 128 MiB undo budget."));
        }
        updateWindowTitle();
        return;
    }
    const std::size_t added_bytes = merges_with_top ? 0U : estimated_payload_bytes;
    if (added_bytes > kMaximumUndoHistoryBytes -
                          std::min(undo_history_estimated_bytes_,
                                   kMaximumUndoHistoryBytes)) {
        clearUndoHistory(true);
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
    setWindowTitle(tr("%1[*] — Procedural Visualizer Tool").arg(name));
}

void MainWindow::updateCompatibilityWarning() {
    compatibility_warning_.clear();
    if (document_ != nullptr) {
        QStringList newer_versions;
        if (program_version_is_newer(document_->created_with_version)) {
            newer_versions.push_back(
                tr("created with %1")
                    .arg(QString::fromStdString(document_->created_with_version)));
        }
        if (program_version_is_newer(document_->last_changed_with_version)) {
            newer_versions.push_back(
                tr("last changed with %1")
                    .arg(QString::fromStdString(document_->last_changed_with_version)));
        }
        if (document_->newer_program_version && newer_versions.isEmpty()) {
            newer_versions.push_back(tr("saved by an unknown newer version"));
        }
        if (!newer_versions.isEmpty()) {
            compatibility_warning_ =
                tr("Newer-version bundle (%1). It can be opened, but saving with %2 "
                   "may discard fields this version does not understand.")
                    .arg(newer_versions.join(tr(", ")),
                         QString::fromUtf8(PVT_PROGRAM_VERSION));
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

bool MainWindow::confirmDiscardChanges() {
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
    return !hasUnsavedChanges();
}

bool MainWindow::documentReplacementAllowed(QString* error) {
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

void MainWindow::restoreUserSettings() {
    QSettings settings;
    const QString saved_directory = settings.value(QStringLiteral("paths/lastDialogDirectory"))
                                        .toString();
    if (!existing_writable_directory(saved_directory, true).isEmpty()) {
        last_dialog_directory_ = saved_directory;
    }
    restoreGeometry(settings.value(QStringLiteral("ui/mainWindow/geometry")).toByteArray());
    restoreState(settings.value(QStringLiteral("ui/mainWindow/state")).toByteArray());
}

void MainWindow::saveUserSettings() {
    QSettings settings;
    settings.setValue(QStringLiteral("paths/lastDialogDirectory"), last_dialog_directory_);
    settings.setValue(QStringLiteral("ui/mainWindow/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/mainWindow/state"), saveState());
}

void MainWindow::editUndoLimit() {
    const int current = undo_stack_ != nullptr ? undo_stack_->undoLimit() : kDefaultUndoLimit;
    bool accepted = false;
    const int requested = QInputDialog::getInt(
        this, tr("Undo history limit"), tr("Maximum undo steps"), current,
        kMinimumUndoLimit, kMaximumUndoLimit, 10, &accepted);
    if (!accepted || requested == current || undo_stack_ == nullptr) return;
    if (undo_stack_->count() > 0) {
        const bool had_unsaved_changes = !undo_stack_->isClean();
        const auto choice = QMessageBox::question(
            this, tr("Clear undo history?"),
            tr("Changing the limit clears this session’s undo and redo history. Continue?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        baseline_dirty_ = baseline_dirty_ || had_unsaved_changes;
        clearUndoHistory(false);
    }
    undo_stack_->setUndoLimit(requested);
    QSettings().setValue(QStringLiteral("preferences/undoLimit"), requested);
    undo_stack_->setClean();
    updateWindowTitle();
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
    preview_->setConfiguration(config_);
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
    const bool coordinate_effect = !is_glow && !is_block_scale;
    const bool has_center = !is_block_scale;

    effect_form_->setRowVisible(effect_edge_, coordinate_effect);
    effect_form_->setRowVisible(effect_magnitude_, coordinate_effect || is_block_scale);
    effect_form_->setRowVisible(effect_frequency_, coordinate_effect || is_block_scale);
    effect_form_->setRowVisible(effect_secondary_, !is_zoom);
    effect_form_->setRowVisible(effect_center_x_, has_center);
    effect_form_->setRowVisible(effect_center_y_, has_center);
    effect_form_->setRowVisible(effect_angle_, is_shake || is_flag);
    effect_form_->setRowVisible(effect_radius_, is_glow);
    effect_form_->setRowVisible(effect_threshold_, is_glow);
    effect_form_->setRowVisible(effect_knee_, is_glow);
    effect_form_->setRowVisible(effect_area_radius_, !is_block_scale);

    effect_edge_->setToolTip(
        tr("Controls samples that move beyond the source image boundary."));
    effect_center_x_->setToolTip(tr("Normalized horizontal center; 0 is left and 1 is right."));
    effect_center_y_->setToolTip(tr("Normalized vertical center; 0 is top and 1 is bottom."));
    effect_radius_->setToolTip(tr("Glow blur radius in full-resolution output pixels."));
    effect_area_radius_->setToolTip(
        tr("Fraction of the shorter canvas edge. Zero affects the whole layer; "
           "positive values create a feathered draggable circle."));
    effect_threshold_->setToolTip(tr("Linear-light brightness where glow begins."));
    effect_knee_->setToolTip(tr("Soft transition width around the glow threshold."));

    if (is_block_scale) {
        effect_intensity_->setRange(0.0, 1.0);
        effect_magnitude_->setRange(0.00001, 10.0);
        effect_frequency_->setRange(effect_magnitude_->value(), 1000.0);
        effect_secondary_->setRange(0.0, 100.0);
        effect_secondary_->setDecimals(0);
        effect_secondary_->setSingleStep(1.0);
    } else {
        effect_intensity_->setRange(0.0, 100.0);
        effect_magnitude_->setRange(0.0, 10.0);
        effect_frequency_->setRange(0.0, 1000.0);
        effect_secondary_->setRange(-100.0, 100.0);
        effect_secondary_->setDecimals(4);
        effect_secondary_->setSingleStep(0.01);
    }

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
    } else if (is_glow) {
        set_form_label(effect_form_, effect_intensity_, tr("Glow intensity"));
        set_form_label(effect_form_, effect_secondary_, tr("Pulse depth"));
        effect_intensity_->setToolTip(tr("Brightness added by the blurred highlight layer."));
        effect_secondary_->setToolTip(tr("How strongly the synchronized clock pulses glow intensity."));
    } else {
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
             effect_name_, effect_enabled_, effect_sync_, effect_type_, effect_space_,
             effect_cycles_,
             effect_phase_, effect_edge_, effect_intensity_, effect_magnitude_,
             effect_frequency_, effect_secondary_, effect_center_x_, effect_center_y_,
             effect_angle_, effect_radius_, effect_threshold_, effect_knee_,
             effect_area_radius_}) {
        widget->setEnabled(enabled);
    }
    if (index) {
        const auto& effect = config_.effects[*index];
        effect_name_->setText(QString::fromStdString(effect.name));
        effect_enabled_->setChecked(effect.enabled);
        effect_sync_->setChecked(effect.synchronized);
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
    surface_obj_path_->setText(QString::fromStdString(config_.surface.obj_path));
    surface_rotations_->setValue(config_.surface.rotations_per_loop);
    surface_phase_->setValue(config_.surface.phase_degrees);
    surface_curvature_->setValue(config_.surface.curvature);
    surface_lighting_->setValue(config_.surface.lighting);
    transform_flip_horizontal_->setChecked(config_.transform.flip_horizontal);
    transform_flip_vertical_->setChecked(config_.transform.flip_vertical);
    select_enum(transform_mirror_, config_.transform.mirror);
    palette_enabled_->setChecked(config_.palette.enabled);
    palette_name_->setText(QString::fromStdString(config_.palette.name));
    refreshPaletteEditor();
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
    updateOutputEditorValidity();
}

void MainWindow::refreshPaletteEditor() {
    if (palette_colors_ == nullptr) return;
    const int previous_row = palette_colors_->currentRow();
    const QSignalBlocker blocker(palette_colors_);
    palette_colors_->clear();
    for (std::size_t index = 0U; index < config_.palette.colors.size(); ++index) {
        const pvt::PaletteColor& value = config_.palette.colors[index];
        const QColor color = QColor::fromRgbF(
            static_cast<float>(value.red), static_cast<float>(value.green),
            static_cast<float>(value.blue));
        auto* item = new QListWidgetItem(
            tr("%1. %2   RGB(%3, %4, %5)")
                .arg(static_cast<qulonglong>(index + 1U))
                .arg(color.name(QColor::HexRgb).toUpper())
                .arg(value.red, 0, 'f', 3)
                .arg(value.green, 0, 'f', 3)
                .arg(value.blue, 0, 'f', 3),
            palette_colors_);
        item->setBackground(color);
        const double luminance = 0.2126 * value.red + 0.7152 * value.green
                                 + 0.0722 * value.blue;
        item->setForeground(luminance > 0.5 ? Qt::black : Qt::white);
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
    config_.palette = pvt::default_palette(index);
    syncActiveRender();
    loadGlobalEditors();
    schedulePreview();
    recordActiveStateChange(tr("Use palette preset"), std::move(before));
}

void MainWindow::addPaletteColor() {
    if (config_.palette.colors.size() >= pvt::kMaximumPaletteColors) {
        QMessageBox::warning(this, tr("Palette limit"),
                             tr("A palette can contain at most 256 colors."));
        return;
    }
    const QColor initial = config_.palette.colors.empty()
                               ? QColor(Qt::white)
                               : QColor::fromRgbF(
                                     static_cast<float>(config_.palette.colors.back().red),
                                     static_cast<float>(config_.palette.colors.back().green),
                                     static_cast<float>(config_.palette.colors.back().blue));
    const QColor chosen = QColorDialog::getColor(
        initial, this, tr("Add palette color"));
    if (!chosen.isValid()) return;
    auto before = captureActiveState();
    config_.palette.colors.push_back(
        {chosen.redF(), chosen.greenF(), chosen.blueF()});
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
    const QColor chosen = QColorDialog::getColor(
        QColor::fromRgbF(static_cast<float>(current.red),
                         static_cast<float>(current.green),
                         static_cast<float>(current.blue)),
        this, tr("Edit palette color"));
    if (!chosen.isValid()) return;
    auto before = captureActiveState();
    config_.palette.colors[static_cast<std::size_t>(row)] =
        {chosen.redF(), chosen.greenF(), chosen.blueF()};
    syncActiveRender();
    refreshPaletteEditor();
    palette_colors_->setCurrentRow(row);
    schedulePreview();
    recordActiveStateChange(tr("Edit palette color"), std::move(before));
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
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = QStringLiteral("wave:%1:%2:%3")
                            .arg(QString::fromStdString(active_layer_uuid_))
                            .arg(wave.id)
                            .arg(reinterpret_cast<quintptr>(changed_editor));
    recordActiveStateChange(tr("Edit wave"), std::move(before), key);
}

void MainWindow::applySwingEditor(const QObject* changed_editor) {
    if (populating_) {
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
    const QString key = QStringLiteral("swing:%1:%2:%3")
                            .arg(QString::fromStdString(active_layer_uuid_))
                            .arg(swing.id)
                            .arg(reinterpret_cast<quintptr>(changed_editor));
    recordActiveStateChange(tr("Edit swing"), std::move(before), key);
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
        if (effect.type == pvt::EffectType::BlockScale
            && effect.frequency < effect.magnitude) {
            effect.frequency = effect.magnitude;
            const QSignalBlocker blocker(effect_frequency_);
            effect_frequency_->setValue(effect.frequency);
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
    } else if (changed_editor == effect_threshold_) {
        effect.threshold = effect_threshold_->value();
    } else if (changed_editor == effect_knee_) {
        effect.soft_knee = effect_knee_->value();
    } else if (changed_editor == effect_area_radius_) {
        effect.area_radius = effect_area_radius_->value();
    } else {
        return;
    }
    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    updateEffectListItem(*index);
    updateEffectEditorVisibility();
    preview_->setConfiguration(config_);
    schedulePreview();
    const QString key = QStringLiteral("effect:%1:%2:%3")
                            .arg(QString::fromStdString(active_layer_uuid_))
                            .arg(effect.id)
                            .arg(reinterpret_cast<quintptr>(changed_editor));
    recordActiveStateChange(tr("Edit effect"), std::move(before), key);
}

void MainWindow::applyGlobalEditor(const QObject* changed_editor) {
    if (populating_) {
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
    } else if (changed_editor == transform_flip_horizontal_) {
        config_.transform.flip_horizontal =
            transform_flip_horizontal_->isChecked();
    } else if (changed_editor == transform_flip_vertical_) {
        config_.transform.flip_vertical = transform_flip_vertical_->isChecked();
    } else if (changed_editor == transform_mirror_) {
        config_.transform.mirror = static_cast<pvt::MirrorMode>(
            transform_mirror_->currentData().toInt());
    } else if (changed_editor == palette_enabled_) {
        if (palette_enabled_->isChecked() && config_.palette.colors.empty()) {
            config_.palette = pvt::default_palette(0U);
            const QSignalBlocker name_blocker(palette_name_);
            palette_name_->setText(QString::fromStdString(config_.palette.name));
            refreshPaletteEditor();
            status_->setText(tr("Loaded the Ember preset because an enabled palette needs at least one color."));
        } else {
            config_.palette.enabled = palette_enabled_->isChecked();
        }
    } else if (changed_editor == palette_name_) {
        if (!palette_name_->hasAcceptableInput()) return;
        config_.palette.name = palette_name_->text().toStdString();
    } else if (changed_editor == surface_enabled_) {
        config_.surface.enabled = surface_enabled_->isChecked();
    } else if (changed_editor == surface_mapping_) {
        config_.surface.mapping =
            static_cast<pvt::SurfaceMapping>(surface_mapping_->currentData().toInt());
    } else if (changed_editor == surface_obj_path_) {
        if (!surface_obj_path_->hasAcceptableInput()) {
            return;
        }
        config_.surface.obj_path = surface_obj_path_->text().toStdString();
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
    png_compression_->setEnabled(config_.output.bit_depth != 32);
    if (changed_editor == frames_ || changed_editor == fps_) {
        updateTimelineState();
    }
    if (affects_preview) {
        preview_->setConfiguration(config_);
        schedulePreview();
    }
    const QString key = QStringLiteral("setting:%1:%2")
                            .arg(QString::fromStdString(active_layer_uuid_))
                            .arg(reinterpret_cast<quintptr>(changed_editor));
    recordActiveStateChange(tr("Edit project setting"), std::move(before), key);
}

void MainWindow::ensureAlphaForTransparency() {
    bool guaranteed_opaque = false;
    for (const auto& layer : project_.layers) {
        if (!layer.enabled || layer.opacity <= 0.0) continue;
        const pvt::RenderConfig materialized = layer.uuid == active_layer_uuid_
            ? config_
            : pvt::apply_global_config(project_.canvas, project_.output, layer.render);
        const bool procedural_transparency = materialized.alpha.enabled
            && (materialized.alpha.minimum < 1.0 || materialized.alpha.maximum < 1.0);
        const bool source_guaranteed_opaque = layer.opacity >= 1.0
            && !procedural_transparency
            && !configuration_requires_alpha(materialized);
        guaranteed_opaque = guaranteed_opaque || source_guaranteed_opaque;
    }
    const bool project_requests_alpha = !guaranteed_opaque;
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
    if (!index) {
        return;
    }
    const auto target = static_cast<std::ptrdiff_t>(*index) + direction;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(config_.effects.size())) {
        return;
    }
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
    auto before = captureActiveState();
    auto& random = *QRandomGenerator::global();
    for (auto& wave : config_.waves) {
        randomize_wave_settings(wave, random);
    }
    for (auto& swing : config_.swings) {
        randomize_swing_settings(swing, random);
    }
    for (auto& effect : config_.effects) {
        randomize_effect_settings(effect, random);
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
    auto before = captureActiveState();
    auto& random = *QRandomGenerator::global();
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
        swing.waveform = static_cast<pvt::Waveform>(random_integer(random, 0, 3));
        has_enabled_swing = has_enabled_swing || swing.enabled;
        randomize_swing_settings(swing, random);
        config_.swings.push_back(std::move(swing));
    }
    if (!has_enabled_swing && !config_.swings.empty()) {
        config_.swings.front().enabled = true;
    }

    std::array<pvt::EffectType, 6> effect_types = {
        pvt::EffectType::EndlessZoom, pvt::EffectType::Ripple,
        pvt::EffectType::Shake, pvt::EffectType::FlagWave, pvt::EffectType::Glow,
        pvt::EffectType::BlockScale};
    const int effect_count = random_integer(random, 1, static_cast<int>(effect_types.size()));
    bool has_enabled_effect = false;
    for (int index = 0; index < effect_count; ++index) {
        const int selected = random_integer(
            random, index, static_cast<int>(effect_types.size()) - 1);
        std::swap(effect_types[static_cast<std::size_t>(index)],
                  effect_types[static_cast<std::size_t>(selected)]);
        auto effect = pvt::default_effect(effect_types[static_cast<std::size_t>(index)]);
        effect.id = pvt::allocate_id(config_);
        effect.enabled = random_chance(random, 0.65);
        has_enabled_effect = has_enabled_effect || effect.enabled;
        randomize_effect_settings(effect, random);
        config_.effects.push_back(std::move(effect));
    }
    if (!has_enabled_effect) {
        config_.effects.front().enabled = true;
    }

    ensureAlphaForTransparency();
    syncActiveRender();
    syncProjectGlobals();
    refreshAll();
    schedulePreview();
    status_->setText(
        tr("Created a new mix with %1 waves, %2 swings, and %3 effects.")
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

void MainWindow::schedulePreview() {
    ++preview_generation_;
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
        auto cancel = std::make_shared<std::atomic_bool>(false);
        preview_cancel_ = cancel;
        preview_watcher_->setFuture(QtConcurrent::run(
            [project = std::move(project), frame, generation, revision,
             test_delay_ms = preview_test_delay_ms_, cancel]() mutable {
                return generatePreview(std::move(project), frame, generation, revision,
                                       test_delay_ms, cancel);
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
    if (solo_layer_uuid_) {
        for (auto& layer : project.layers) {
            layer.enabled = layer.uuid == *solo_layer_uuid_;
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
            static_cast<double>(std::min(preview_width, preview_height)) /
            static_cast<double>(source_short_edge);

        // These controls are defined in output pixels. Scale them with the preview so
        // the low-resolution preview preserves their full-resolution proportions.
        for (auto& layer : project.layers) {
            layer.render.displacement *= pixel_scale;
            for (auto& effect : layer.render.effects) {
                if (effect.type == pvt::EffectType::Glow) {
                    effect.radius_pixels *= pixel_scale;
                }
            }
        }
        project.canvas.width = preview_width;
        project.canvas.height = preview_height;
        project.canvas.block_size = std::max(
            1, static_cast<int>(std::lround(project.canvas.block_size * scale)));
        pvt::Image image;
        std::string error;
        if (!pvt::render_project_frame(project, frame, image,
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

bool MainWindow::startExport() {
    cancel_export_.store(false);
    status_->setText(tr("Exporting sequence…"));
    try {
        auto project = project_;
        project.output.output_directory =
            resolvedOutputDirectory(QString::fromStdString(project.output.output_directory))
                .toStdString();
        export_active_ = true;
        export_watcher_->setFuture(QtConcurrent::run(
            [this, project = std::move(project)] {
                ExportResult result;
                std::string error;
                try {
                    result.ok = pvt::render_project_sequence(
                        project,
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
    return loadProjectPath(path, error);
}

bool MainWindow::loadProjectPath(const QString& path, QString* error) {
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
    if (loaded.project.layers.empty()) {
        if (error != nullptr) *error = tr("The loaded project contains no layers.");
        return false;
    }
    document_ = std::make_unique<pvt::ProjectDocument>(std::move(loaded));
    project_ = document_->project;
    active_layer_uuid_ = project_.layers.front().uuid;
    solo_layer_uuid_.reset();
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
    refreshVersionsPage();
    updateWindowTitle();
    schedulePreview();
    return true;
}

bool MainWindow::runSmokeChecks(QString* error) {
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

    const auto original = config_;
    auto expected = original;
    expected.surface.rotations_per_loop = 900;
    expected.surface.lighting = 9.0;
    expected.surface.obj_path = "meshes/smoke test.obj";
    expected.ghost_lag_degrees = 5.729612345678;
    expected.output.png_compression_level = 9;
    expected.output.write_alpha = true;
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
    const auto original_project = project_;
    const std::optional<pvt::ProjectDocument> original_document =
        document_ != nullptr
            ? std::optional<pvt::ProjectDocument>(*document_)
            : std::nullopt;
    const std::string original_active_uuid = active_layer_uuid_;
    const auto original_solo_uuid = solo_layer_uuid_;
    const bool original_baseline_dirty = baseline_dirty_;
    const bool original_undo_dirty = undo_stack_ != nullptr && !undo_stack_->isClean();
    const bool original_dither_preference = integer_dither_preference_;
    const QString original_project_path = current_project_path_;
    const QString original_legacy_path = imported_legacy_path_;
    const QString original_dialog_directory = last_dialog_directory_;
    const QString original_status = status_ != nullptr ? status_->text() : QString();
    ScopeExit restore_state([this, original_project, original_document,
                             original_active_uuid, original_solo_uuid,
                             original_baseline_dirty, original_undo_dirty,
                             original_dither_preference, original_project_path,
                             original_legacy_path, original_dialog_directory,
                             original_status] {
        preview_test_delay_ms_ = 0;
        independent_copy_test_path_.clear();
        if (playback_timer_ != nullptr) playback_timer_->stop();
        project_ = original_project;
        document_ = original_document
                        ? std::make_unique<pvt::ProjectDocument>(*original_document)
                        : nullptr;
        active_layer_uuid_ = original_active_uuid;
        solo_layer_uuid_ = original_solo_uuid;
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
        if (status_ != nullptr) status_->setText(original_status);
        schedulePreview();
    });
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
        || !windowTitle().contains(tr("Procedural Visualizer Tool"))) {
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
    const auto validator_accepts = [](const QLineEdit* editor, QString value) {
        int position = static_cast<int>(value.size());
        return editor->validator() != nullptr
               && editor->validator()->validate(value, position) == QValidator::Acceptable;
    };
    if (!validator_rejects(prefix_, QStringLiteral("bad/name"))
        || !validator_rejects(prefix_, QString(50, QChar(0x20ac)))
        || !validator_rejects(output_directory_, QString(1400, QChar(0x20ac)))
        || !validator_rejects(wave_name_, QString(100, QChar(0x20ac)))
        || !validator_rejects(project_name_, QString())
        || !validator_rejects(project_name_, QStringLiteral("bad/name"))
        || !validator_rejects(project_name_, QString(QChar(0x0085)))
        || !validator_rejects(project_name_, QString(100, QChar(0x20ac)))
        || !validator_accepts(project_name_, QStringLiteral("CON: Fire. "))) {
        if (error != nullptr) {
            *error = tr("GUI text validators did not enforce portable UTF-8 byte limits.");
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
    if (config_.total_frames != 12 || !frame_label_->text().endsWith(QStringLiteral("/ 12"))) {
        if (error != nullptr) {
            *error = tr("The GUI timeline did not follow a frame-count edit.");
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
        cancelled_preview_token);
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
    if (config_.waves.size() < 2U || config_.waves.size() > 6U
        || config_.swings.size() > 3U
        || config_.effects.empty() || config_.effects.size() > 6U
        || !enabled_wave || !enabled_effect || !unique_randomized_items
        || !pvt::validate(config_).ok) {
        if (error != nullptr) {
            *error = tr("Randomize mix produced an invalid stack composition.");
        }
        return false;
    }

    // Exercise the project document UI on a deterministic clean project.
    project_ = pvt::default_project();
    document_ = std::make_unique<pvt::ProjectDocument>(pvt::default_project_document());
    document_->project = project_;
    active_layer_uuid_ = project_.layers.front().uuid;
    solo_layer_uuid_.reset();
    current_project_path_.clear();
    imported_legacy_path_.clear();
    baseline_dirty_ = false;
    loadActiveConfiguration();
    clearUndoHistory(false);
    undo_stack_->setClean();
    updateCompatibilityWarning();
    refreshLayerList();
    refreshAll();

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
    phrase_warp_->setValue(0.25);
    const std::string base_uuid = active_layer_uuid_;
    const double base_phrase_warp = config_.phrase_warp;
    addLayer();
    if (project_.layers.size() != 2U || active_layer_uuid_ != project_.layers.back().uuid
        || !project_.output.write_alpha || !write_alpha_->isChecked()
        || layer_list_->count() != 2
        || layer_list_->item(0)->data(Qt::UserRole).toString().toStdString()
               != project_.layers.back().uuid
        || config_.phrase_warp == base_phrase_warp) {
        if (error != nullptr) {
            *error = tr("Adding a layer did not preserve paint order, isolation, or the one-way alpha default.");
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
    recordUndo(tr("Oversized smoke command"), [] {}, [] {}, {},
               kMaximumUndoHistoryBytes + 1U);
    if (undo_stack_->count() != 0 || !baseline_dirty_ || !hasUnsavedChanges()) {
        if (error != nullptr) {
            *error = tr("The hard undo-memory budget did not retain dirty state safely.");
        }
        return false;
    }
    clearUndoHistory(false);
    undo_stack_->setClean();
    baseline_dirty_ = false;
    const QString bundle_path = directory.filePath(QStringLiteral("smoke-project.zip"));
    if (!saveProjectPath(bundle_path) || document_ == nullptr
        || document_->versions.size() != 1U || hasUnsavedChanges()) {
        if (error != nullptr) *error = tr("The GUI could not create the first bundle version.");
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
    makeSelectedVersionCurrent();
    if (document_->current_version != first_version || hasUnsavedChanges()) {
        if (error != nullptr) {
            *error = tr("Saving during Make Current lost the selected version or left dirty state.");
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
        || undo_stack_->count() != 0) {
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
    if (compatibility_warning_.isEmpty() || compatibility_warning_label_->isHidden()
        || !compatibility_warning_.contains(tr("saving"), Qt::CaseInsensitive)) {
        if (error != nullptr) {
            *error = tr("A newer bundle version did not produce a persistent save-risk warning.");
        }
        return false;
    }
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
        snapshot.output = config_.output;
        snapshot.name = project_name;

        copy = std::make_unique<pvt::ProjectDocument>();
        std::string copy_error;
        if (!pvt::make_independent_project_copy(snapshot, *copy, &copy_error)) {
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
    document_ = std::move(copy);
    project_ = std::move(opened_project);
    config_ = std::move(opened_config);
    active_layer_uuid_ = std::move(opened_active_layer_uuid);
    solo_layer_uuid_ = std::move(opened_solo_layer_uuid);
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
    (void)saveProjectPath(current_project_path_);
}

void MainWindow::saveSetupAs() {
    const QString filename = QString::fromStdString(
        pvt::portable_project_filename(project_.name));
    const QString initial_path = QDir(usableDialogDirectory()).filePath(filename);
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save project bundle"), initial_path,
        tr("PVT project bundle (*.zip);;All files (*)"));
    if (path.isEmpty()) return;
    if (QFileInfo(path).suffix().isEmpty()) path.append(QStringLiteral(".zip"));
    (void)saveProjectPath(path);
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
        status_->setText(tr("No changes; validated the complete bundle at %1").arg(path));
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
    if (!confirmDiscardChanges()) return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open project or import legacy setup"), usableDialogDirectory(),
        tr("PVT projects (*.zip *.pvt);;Project bundles (*.zip);;Legacy setups (*.pvt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    rememberDialogLocation(path);
    QString error;
    if (!loadSetupFile(path, &error)) {
        QMessageBox::critical(this, tr("Load failed"),
                              tr("The active setup was not changed.\n\n%1").arg(error));
        return;
    }
    if (compatibility_warning_.isEmpty()) {
        status_->setText(tr("Loaded %1").arg(path));
    }
}
