#include "application_settings_dialog.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QGuiApplication>
#include <QLayout>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <memory>

namespace {

constexpr int kMinimumUndoLimit = 0;
constexpr int kMaximumUndoLimit = (std::numeric_limits<int>::max)();
constexpr int kMaximumRecentProjectLimit = (std::numeric_limits<int>::max)();
constexpr std::size_t kMebibyte = std::size_t{1024U} * 1024U;
constexpr std::size_t kGibibyte = kMebibyte * 1024U;
constexpr int kPreferredDialogWidth = 860;
constexpr int kPreferredDialogHeight = 640;

int spin_box_maximum(std::size_t value) {
    return static_cast<int>((std::min)(
        value,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

int spin_box_value(std::size_t value, int maximum) {
    return static_cast<int>((std::min)(
        value, static_cast<std::size_t>(maximum)));
}

QString memory_size_text(std::size_t bytes) {
    const double gibibytes = static_cast<double>(bytes)
                             / static_cast<double>(kGibibyte);
    if (gibibytes >= 1.0) {
        return QObject::tr("%1 GiB").arg(gibibytes, 0, 'f',
                                          gibibytes < 10.0 ? 2 : 1);
    }
    const double mebibytes = static_cast<double>(bytes)
                             / static_cast<double>(kMebibyte);
    return QObject::tr("%1 MiB").arg(mebibytes, 0, 'f', 0);
}

QLabel* explanatory_label(const QString& text, QWidget* parent,
                          const QString& details = {}) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QSizePolicy policy = label->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Preferred);
    policy.setVerticalPolicy(QSizePolicy::Minimum);
    policy.setHeightForWidth(true);
    label->setSizePolicy(policy);
    if (!details.isEmpty()) {
        label->setToolTip(details);
        label->setAccessibleDescription(details);
    }
    return label;
}

void configure_form(QFormLayout* form) {
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFormAlignment(Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(7);
}

void configure_section(QGroupBox* group) {
    group->setFlat(true);
    QSizePolicy policy = group->sizePolicy();
    policy.setHorizontalPolicy(QSizePolicy::Preferred);
    policy.setVerticalPolicy(QSizePolicy::Maximum);
    group->setSizePolicy(policy);
}

QScrollArea* scrollable_settings_page(QWidget* content, QWidget* parent,
                                      const QString& object_name) {
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(object_name);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setWidget(content);
    return scroll;
}

class CompactDoubleSpinBox final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

protected:
    QString textFromValue(double value) const override {
        QString text = locale().toString(value, 'f', decimals());
        if (decimals() <= 0) return text;
        const QString decimal = locale().decimalPoint();
        if (!text.contains(decimal)) return text;
        const QString zero = locale().zeroDigit();
        while (text.endsWith(zero)) text.chop(zero.size());
        if (text.endsWith(decimal)) text.chop(decimal.size());
        return text;
    }
};

} // namespace

ApplicationSettingsDialog::ApplicationSettingsDialog(
    int undoLimit, const PerformanceSettings& performanceSettings,
    int recentProjectLimit,
    bool hasCustomNewProjectDefaults, QWidget* parent,
    const pvt::RendererCapabilities* capabilitiesOverride)
    : QDialog(parent) {
    setObjectName(QStringLiteral("applicationSettingsDialog"));
    setWindowTitle(tr("Application Settings"));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(12);
    auto* scope_note = explanatory_label(
        tr("These settings apply to every project and persist after the "
           "application is closed."),
        this);
    scope_note->setObjectName(QStringLiteral("applicationSettingsScopeNote"));
    root->addWidget(scope_note);

    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("applicationSettingsTabs"));
    tabs->setUsesScrollButtons(true);

    auto* general_page = new QWidget;
    auto* general_layout = new QVBoxLayout(general_page);
    general_layout->setContentsMargins(12, 12, 12, 12);
    general_layout->setSpacing(12);
    auto* general_preferences = new QHBoxLayout;
    general_preferences->setSpacing(12);
    auto* history_group = new QGroupBox(tr("Editing History"), general_page);
    configure_section(history_group);
    auto* history_form = new QFormLayout(history_group);
    configure_form(history_form);
    undo_limit_ = new QSpinBox(history_group);
    undo_limit_->setObjectName(QStringLiteral("undoLimitPreference"));
    undo_limit_->setRange(kMinimumUndoLimit, kMaximumUndoLimit);
    undo_limit_->setSingleStep(10);
    undo_limit_->setSuffix(tr(" steps"));
    undo_limit_->setValue(std::clamp(undoLimit, kMinimumUndoLimit,
                                     kMaximumUndoLimit));
    history_form->addRow(tr("Maximum undo steps (0 = unlimited)"), undo_limit_);
    history_form->addRow(
        explanatory_label(
            tr("Changing this clears the current session's undo and redo "
               "history. A value of 0 keeps commands until memory is exhausted."),
            history_group,
            tr("Changing this limit clears the current session's undo and redo "
               "history. The maximum follows Qt's signed-int command index; "
               "available memory is the practical limit.")));
    general_preferences->addWidget(history_group, 1, Qt::AlignTop);

    auto* recent_group = new QGroupBox(tr("Recent Projects"), general_page);
    configure_section(recent_group);
    auto* recent_form = new QFormLayout(recent_group);
    configure_form(recent_form);
    recent_project_limit_ = new QSpinBox(recent_group);
    recent_project_limit_->setObjectName(QStringLiteral("recentProjectLimitPreference"));
    recent_project_limit_->setRange(0, kMaximumRecentProjectLimit);
    recent_project_limit_->setValue(std::clamp(
        recentProjectLimit, 0, kMaximumRecentProjectLimit));
    recent_form->addRow(tr("Projects shown (0 = disabled)"),
                        recent_project_limit_);
    recent_form->addRow(explanatory_label(
        tr("Controls File > Recent Projects. This machine-local preference is "
           "never stored inside a project."),
        recent_group,
        tr("The File menu shows each project's name and full path. This is a local "
           "application preference and is not stored inside projects; its only "
           "upper bound is Qt's signed-int menu index.")));
    general_preferences->addWidget(recent_group, 1, Qt::AlignTop);
    general_layout->addLayout(general_preferences);

    auto* defaults_group = new QGroupBox(tr("New Projects"), general_page);
    configure_section(defaults_group);
    auto* defaults_layout = new QVBoxLayout(defaults_group);
    defaults_layout->addWidget(explanatory_label(
        tr("Use the open project as the starting point for New Project and its "
           "selected layer as the Add Layer template. New identities are created."),
        defaults_group,
        tr("Save the complete project currently open behind this dialog as a "
           "new-project template. New projects receive fresh project and layer "
           "identities; the template may include layers and embedded assets. "
           "The currently selected layer also becomes the template used by "
           "Add Layer.")));
    auto* defaults_buttons = new QHBoxLayout;
    auto* save_defaults = new QPushButton(
        tr("Use Current Project as Default"), defaults_group);
    save_defaults->setObjectName(QStringLiteral("saveCurrentProjectDefaults"));
    auto* restore_defaults = new QPushButton(
        tr("Restore Built-in Default"), defaults_group);
    restore_defaults->setObjectName(QStringLiteral("restoreBuiltInDefaults"));
    restore_defaults->setEnabled(hasCustomNewProjectDefaults);
    defaults_buttons->addWidget(save_defaults);
    defaults_buttons->addWidget(restore_defaults);
    defaults_buttons->addStretch(1);
    defaults_layout->addLayout(defaults_buttons);
    defaults_status_ = explanatory_label(
        hasCustomNewProjectDefaults
            ? tr("A custom new-project template is active.")
            : tr("The built-in new-project template is active."),
        defaults_group);
    defaults_status_->setObjectName(QStringLiteral("newProjectDefaultsStatus"));
    defaults_layout->addWidget(defaults_status_);
    connect(save_defaults, &QPushButton::clicked, this, [this] {
        defaults_action_ = NewProjectDefaultsAction::SaveCurrentProject;
        defaults_status_->setText(
            tr("Pending: save the current project as the default when OK is clicked."));
    });
    connect(restore_defaults, &QPushButton::clicked, this, [this] {
        defaults_action_ = NewProjectDefaultsAction::RestoreBuiltIn;
        defaults_status_->setText(
            tr("Pending: restore the built-in default when OK is clicked."));
    });
    general_layout->addWidget(defaults_group);
    general_layout->addStretch(1);
    auto* general_scroll = scrollable_settings_page(
        general_page, tabs, QStringLiteral("applicationSettingsScroll"));
    tabs->addTab(general_scroll, tr("General"));

    auto* rendering_page = new QWidget;
    auto* rendering_layout = new QVBoxLayout(rendering_page);
    rendering_layout->setContentsMargins(12, 12, 12, 12);
    rendering_layout->setSpacing(12);
    auto* rendering_columns = new QHBoxLayout;
    rendering_columns->setSpacing(12);
    auto* rendering_summary = new QVBoxLayout;
    rendering_summary->setSpacing(12);
    auto* backend_group = new QGroupBox(tr("Acceleration"), rendering_page);
    configure_section(backend_group);
    auto* backend_form = new QFormLayout(backend_group);
    configure_form(backend_form);
    const pvt::RendererCapabilities capabilities =
        capabilitiesOverride == nullptr ? pvt::renderer_capabilities()
                                        : *capabilitiesOverride;
    render_backend_ = new QComboBox(backend_group);
    render_backend_->setObjectName(QStringLiteral("renderBackendPreference"));
    render_backend_->addItem(
        tr("Automatic (GPU-first, Recommended)"),
        static_cast<int>(RenderBackendPreference::Automatic));
    render_backend_->addItem(tr("CPU"),
                             static_cast<int>(RenderBackendPreference::Cpu));
    render_backend_->addItem(
        tr("CPU + GPU"),
        static_cast<int>(RenderBackendPreference::CpuAndGpu));
    render_backend_->addItem(tr("GPU"),
                             static_cast<int>(RenderBackendPreference::Gpu));
    const int backend_index = render_backend_->findData(
        static_cast<int>(performanceSettings.backend));
    render_backend_->setCurrentIndex(backend_index >= 0 ? backend_index : 0);
    render_backend_->setToolTip(
        tr("Automatic uses the GPU-primary hybrid scheduler, accelerating each "
           "supported stage and assigning only remaining independent work to "
           "bounded CPU lanes. CPU is the deterministic reference renderer. "
           "GPU is strict: runtime acceleration failures are reported instead "
           "of silently restarting the frame on CPU."));
    backend_form->addRow(tr("Backend"), render_backend_);
    QString accelerator_status;
    if (capabilities.metal_available) {
        accelerator_status = tr("Metal ready: %1")
            .arg(QString::fromStdString(capabilities.metal_device_name));
    } else if (capabilities.opengl_surface_available) {
        accelerator_status = tr("OpenGL generated sources and surfaces ready: %1")
            .arg(QString::fromStdString(
                capabilities.opengl_surface_device_name));
    } else if (capabilities.opengl_surface_compiled) {
        accelerator_status = QString::fromStdString(
            capabilities.opengl_surface_status);
    } else {
        accelerator_status = QString::fromStdString(capabilities.metal_status);
    }
    auto* capability_label = explanatory_label(accelerator_status,
                                                backend_group);
    capability_label->setObjectName(QStringLiteral("rendererCapabilityStatus"));
    backend_form->addRow(tr("Acceleration"), capability_label);
    backend_form->addRow(
        explanatory_label(
            tr("Used for preview, LIVE, and export. Automatic keeps supported "
               "work on the GPU and uses bounded CPU lanes for independent work."),
            backend_group,
            tr("This machine-local backend is used for preview, LIVE, and export. "
               "Automatic is recommended for maximum throughput. On Windows and Linux, "
               "Qt OpenGL admits every valid layer and accelerates supported "
               "generated and surface stages. Ordered dependencies continue on "
               "bounded CPU lanes.")));
    rendering_summary->addWidget(backend_group);

    auto* threading_group = new QGroupBox(tr("Concurrency"), rendering_page);
    configure_section(threading_group);
    auto* threading_form = new QFormLayout(threading_group);
    configure_form(threading_form);
    const int maximum_cpu_workers =
        spin_box_maximum(pvt::kMaximumSequenceWorkers);
    preview_live_cpu_workers_ = new QSpinBox(threading_group);
    preview_live_cpu_workers_->setObjectName(
        QStringLiteral("previewLiveCpuWorkersPreference"));
    preview_live_cpu_workers_->setRange(0, maximum_cpu_workers);
    preview_live_cpu_workers_->setSpecialValueText(tr("Auto"));
    preview_live_cpu_workers_->setValue(spin_box_value(
        performanceSettings.preview_live_cpu_workers, maximum_cpu_workers));
    preview_live_cpu_workers_->setToolTip(tr(
        "Maximum CPU layer workers inside one editor preview, LIVE frame, or "
        "full-resolution current-frame export. Auto uses host concurrency and "
        "the render memory budget."));
    threading_form->addRow(tr("Preview / LIVE / still CPU workers"),
                           preview_live_cpu_workers_);

    export_frame_workers_ = new QSpinBox(threading_group);
    export_frame_workers_->setObjectName(
        QStringLiteral("exportFrameWorkersPreference"));
    export_frame_workers_->setRange(0, maximum_cpu_workers);
    export_frame_workers_->setSpecialValueText(tr("Auto"));
    export_frame_workers_->setValue(spin_box_value(
        performanceSettings.export_frame_workers, maximum_cpu_workers));
    export_frame_workers_->setToolTip(tr(
        "Maximum image or movie frames prepared concurrently. One serializes "
        "frames without disabling CPU layer parallelism inside that frame. "
        "Auto uses host capacity, frame count, and the memory budget."));
    threading_form->addRow(tr("Concurrent export frames"),
                           export_frame_workers_);

    export_cpu_workers_ = new QSpinBox(threading_group);
    export_cpu_workers_->setObjectName(
        QStringLiteral("exportCpuWorkersPreference"));
    export_cpu_workers_->setRange(0, maximum_cpu_workers);
    export_cpu_workers_->setSpecialValueText(tr("Auto"));
    export_cpu_workers_->setValue(spin_box_value(
        performanceSettings.export_cpu_workers, maximum_cpu_workers));
    export_cpu_workers_->setToolTip(tr(
        "Maximum CPU layer workers inside each active export frame. Auto "
        "partitions host capacity across the outer frames actually admitted. "
        "An explicit value applies per frame and can increase total CPU use."));
    threading_form->addRow(tr("CPU layer workers per export frame"),
                           export_cpu_workers_);

    const int maximum_gpu_frames =
        spin_box_maximum(pvt::kMaximumGpuFramesInFlight);
    gpu_frames_in_flight_ = new QSpinBox(threading_group);
    gpu_frames_in_flight_->setObjectName(
        QStringLiteral("gpuFramesInFlightPreference"));
    gpu_frames_in_flight_->setRange(0, maximum_gpu_frames);
    gpu_frames_in_flight_->setSpecialValueText(tr("Auto"));
    gpu_frames_in_flight_->setValue(spin_box_value(
        performanceSettings.gpu_frames_in_flight, maximum_gpu_frames));
    gpu_frames_in_flight_->setToolTip(
        capabilities.metal_available
            ? tr("Maximum admitted Metal frames and their GPU-visible working "
                 "sets. Auto uses the renderer's conservative device-safe limit.")
            : (capabilities.opengl_surface_available
                   ? tr("This control applies to Metal. The available Qt OpenGL "
                        "backend uses one serialized context, so its effective "
                        "value is one.")
                   : tr("No active Metal admission controller is available on "
                        "this system, so this setting is currently inactive.")));
    gpu_frames_in_flight_->setEnabled(capabilities.metal_available);
    threading_form->addRow(tr("Metal frames in flight"),
                           gpu_frames_in_flight_);

    const std::size_t physical_memory = total_physical_memory_bytes();
    render_memory_budget_mode_ = new QComboBox(threading_group);
    render_memory_budget_mode_->setObjectName(
        QStringLiteral("renderMemoryBudgetModePreference"));
    render_memory_budget_mode_->addItem(
        tr("Automatic (recommended)"),
        static_cast<int>(RenderMemoryBudgetMode::Automatic));
    render_memory_budget_mode_->addItem(
        tr("MiB"), static_cast<int>(RenderMemoryBudgetMode::Mebibytes));
    render_memory_budget_mode_->addItem(
        tr("GiB"), static_cast<int>(RenderMemoryBudgetMode::Gibibytes));
    render_memory_budget_mode_->addItem(
        physical_memory > 0U
            ? tr("% of physical RAM")
            : tr("% of physical RAM (currently unavailable)"),
        static_cast<int>(RenderMemoryBudgetMode::PercentOfPhysicalMemory));
    int memory_mode_index = render_memory_budget_mode_->findData(
        static_cast<int>(performanceSettings.render_memory_budget_mode));
    if (memory_mode_index < 0) memory_mode_index = 0;
    render_memory_budget_mode_->setCurrentIndex(memory_mode_index);
    if (physical_memory == 0U) {
        const int percentage_index = render_memory_budget_mode_->findData(
            static_cast<int>(
                RenderMemoryBudgetMode::PercentOfPhysicalMemory));
        if (auto* model = qobject_cast<QStandardItemModel*>(
                render_memory_budget_mode_->model());
            model != nullptr && percentage_index >= 0
            && model->item(percentage_index) != nullptr) {
            // Keep an existing portable percentage preference selected and
            // round-trippable, but do not offer a conversion into a unit the
            // current host cannot resolve.
            model->item(percentage_index)->setEnabled(false);
        }
    }

    render_memory_budget_value_ = new CompactDoubleSpinBox(threading_group);
    render_memory_budget_value_->setObjectName(
        QStringLiteral("renderMemoryBudgetValuePreference"));
    render_memory_budget_value_->setAccelerated(true);
    render_memory_budget_value_->setKeyboardTracking(false);
    render_memory_budget_value_->setToolTip(tr(
        "Aggregate scheduling budget for project-layer working sets and parallel "
        "export frames. This is a concurrency cap, not a memory preallocation; "
        "a valid render can still run one worker when one frame exceeds it."));
    auto* memory_row = new QWidget(threading_group);
    auto* memory_row_layout = new QHBoxLayout(memory_row);
    memory_row_layout->setContentsMargins(0, 0, 0, 0);
    memory_row_layout->addWidget(render_memory_budget_value_, 1);
    memory_row_layout->addWidget(render_memory_budget_mode_);
    threading_form->addRow(tr("Render memory budget"), memory_row);

    render_memory_budget_status_ = explanatory_label(QString{}, threading_group);
    render_memory_budget_status_->setObjectName(
        QStringLiteral("renderMemoryBudgetStatus"));
    threading_form->addRow(render_memory_budget_status_);

    auto previous_memory_mode = std::make_shared<RenderMemoryBudgetMode>(
        static_cast<RenderMemoryBudgetMode>(
            render_memory_budget_mode_->currentData().toInt()));
    const auto editor_bytes = [](RenderMemoryBudgetMode mode, double value) {
        PerformanceSettings settings;
        settings.render_memory_budget_mode = mode;
        settings.render_memory_budget_value = value;
        return resolved_render_memory_budget_bytes(settings);
    };
    const auto value_for_bytes = [physical_memory](RenderMemoryBudgetMode mode,
                                                    std::size_t bytes) {
        switch (mode) {
            case RenderMemoryBudgetMode::Automatic: return 0.0;
            case RenderMemoryBudgetMode::Mebibytes:
                return static_cast<double>(bytes)
                       / static_cast<double>(kMebibyte);
            case RenderMemoryBudgetMode::Gibibytes:
                return static_cast<double>(bytes)
                       / static_cast<double>(kGibibyte);
            case RenderMemoryBudgetMode::PercentOfPhysicalMemory:
                return physical_memory == 0U ? 0.0
                    : 100.0 * static_cast<double>(bytes)
                        / static_cast<double>(physical_memory);
        }
        return 0.0;
    };
    const auto configure_memory_editor = [this, physical_memory,
                                           previous_memory_mode,
                                           editor_bytes, value_for_bytes](
                                              bool convert_value) {
        const auto mode = static_cast<RenderMemoryBudgetMode>(
            render_memory_budget_mode_->currentData().toInt());
        const std::size_t prior_bytes = convert_value
            ? editor_bytes(*previous_memory_mode,
                           render_memory_budget_value_->value()) : 0U;
        const QSignalBlocker blocker(render_memory_budget_value_);
        render_memory_budget_value_->setEnabled(
            mode != RenderMemoryBudgetMode::Automatic);
        render_memory_budget_value_->setSpecialValueText(QString{});
        switch (mode) {
            case RenderMemoryBudgetMode::Automatic:
                render_memory_budget_value_->setDecimals(0);
                render_memory_budget_value_->setRange(0.0, 0.0);
                render_memory_budget_value_->setSuffix(QString{});
                render_memory_budget_value_->setValue(0.0);
                break;
            case RenderMemoryBudgetMode::Mebibytes:
                render_memory_budget_value_->setDecimals(0);
                render_memory_budget_value_->setSingleStep(256.0);
                render_memory_budget_value_->setRange(
                    1.0, static_cast<double>(
                        (std::numeric_limits<std::size_t>::max)() / kMebibyte));
                render_memory_budget_value_->setSuffix(tr(" MiB"));
                if (convert_value) {
                    render_memory_budget_value_->setValue(
                        value_for_bytes(mode, prior_bytes));
                }
                break;
            case RenderMemoryBudgetMode::Gibibytes:
                // Preserve the full legacy 1 MiB domain while switching units
                // (1 MiB is exactly 0.0009765625 GiB).
                render_memory_budget_value_->setDecimals(10);
                render_memory_budget_value_->setSingleStep(0.25);
                render_memory_budget_value_->setRange(
                    1.0 / 1024.0, static_cast<double>(
                        (std::numeric_limits<std::size_t>::max)() / kGibibyte));
                render_memory_budget_value_->setSuffix(tr(" GiB"));
                if (convert_value) {
                    render_memory_budget_value_->setValue(
                        value_for_bytes(mode, prior_bytes));
                }
                break;
            case RenderMemoryBudgetMode::PercentOfPhysicalMemory: {
                // Small authored byte limits must not jump merely because an
                // artist changes their display unit on a high-memory host.
                render_memory_budget_value_->setDecimals(12);
                render_memory_budget_value_->setSingleStep(0.1);
                const double maximum = physical_memory == 0U
                    ? 100.0 * static_cast<double>(
                          (std::numeric_limits<std::size_t>::max)())
                    : 100.0 * static_cast<double>(
                          (std::numeric_limits<std::size_t>::max)())
                          / static_cast<double>(physical_memory);
                const double minimum = physical_memory == 0U ? 0.0
                    : 100.0 * static_cast<double>(kMebibyte)
                          / static_cast<double>(physical_memory);
                render_memory_budget_value_->setRange(minimum, maximum);
                render_memory_budget_value_->setSuffix(tr(" %"));
                if (convert_value) {
                    render_memory_budget_value_->setValue(
                        value_for_bytes(mode, prior_bytes));
                }
                break;
            }
        }
        *previous_memory_mode = mode;
    };
    const auto update_memory_status = [this, physical_memory, editor_bytes] {
        const auto mode = static_cast<RenderMemoryBudgetMode>(
            render_memory_budget_mode_->currentData().toInt());
        const std::size_t resolved = editor_bytes(
            mode, render_memory_budget_value_->value());
        QString text = mode == RenderMemoryBudgetMode::Automatic
            ? tr("Automatic currently allows %1 for concurrent render working sets.")
                  .arg(memory_size_text(resolved))
            : tr("The current limit resolves to %1.")
                  .arg(memory_size_text(resolved));
        if (physical_memory > 0U) {
            text += tr(" This system reports %1 of physical RAM.")
                        .arg(memory_size_text(physical_memory));
            if (resolved > physical_memory) {
                text += tr(" Warning: the limit exceeds physical RAM and may cause heavy paging.");
                render_memory_budget_status_->setStyleSheet(
                    QStringLiteral("color: #b35a00;"));
            } else {
                render_memory_budget_status_->setStyleSheet(QString{});
            }
        } else {
            text += tr(" Physical RAM could not be detected. A saved percentage is preserved, but resolves through Automatic until detection succeeds.");
            render_memory_budget_status_->setStyleSheet(QString{});
        }
        render_memory_budget_status_->setText(text);
    };
    configure_memory_editor(false);
    if (performanceSettings.render_memory_budget_mode
        != RenderMemoryBudgetMode::Automatic) {
        render_memory_budget_value_->setValue(
            performanceSettings.render_memory_budget_value);
    }
    update_memory_status();
    connect(render_memory_budget_mode_, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [configure_memory_editor, update_memory_status](int) {
                configure_memory_editor(true);
                update_memory_status();
            });
    connect(render_memory_budget_value_,
            qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [update_memory_status](double) { update_memory_status(); });

    pause_editor_preview_during_export_ = new QCheckBox(
        tr("Pause editor preview while exporting"), threading_group);
    pause_editor_preview_during_export_->setObjectName(
        QStringLiteral("pauseEditorPreviewDuringExportPreference"));
    pause_editor_preview_during_export_->setChecked(
        performanceSettings.pause_editor_preview_during_export);
    pause_editor_preview_during_export_->setToolTip(tr(
        "Stops duplicate editor rendering and playback while a still, image "
        "sequence, or movie export owns the CPU and GPU. The preview resumes "
        "after export. Disable this only when monitoring edits is more important "
        "than export throughput."));
    threading_form->addRow(pause_editor_preview_during_export_);

    auto* reset_performance = new QPushButton(
        tr("Reset Performance Controls to Auto"), threading_group);
    reset_performance->setObjectName(
        QStringLiteral("resetPerformancePreferences"));
    connect(reset_performance, &QPushButton::clicked, this, [this] {
        const int automatic = render_backend_->findData(
            static_cast<int>(RenderBackendPreference::Automatic));
        if (automatic >= 0) render_backend_->setCurrentIndex(automatic);
        preview_live_cpu_workers_->setValue(0);
        export_frame_workers_->setValue(0);
        export_cpu_workers_->setValue(0);
        gpu_frames_in_flight_->setValue(0);
        const int automatic_memory = render_memory_budget_mode_->findData(
            static_cast<int>(RenderMemoryBudgetMode::Automatic));
        if (automatic_memory >= 0) {
            render_memory_budget_mode_->setCurrentIndex(automatic_memory);
        }
        pause_editor_preview_during_export_->setChecked(true);
    });
    threading_form->addRow(reset_performance);

    const int detected_workers = QThread::idealThreadCount();
    auto* host_status = explanatory_label(
        detected_workers > 0
            ? tr("Auto currently sees %1 logical CPU workers. Worker and memory "
                 "limits are upper bounds; the scheduler may use fewer for a "
                 "small render or to stay inside the memory budget.")
                  .arg(detected_workers)
            : tr("The host did not report a logical CPU count. Auto will use "
                 "the renderer's safe fallback."),
        threading_group);
    host_status->setObjectName(QStringLiteral("performanceHostStatus"));
    threading_form->addRow(host_status);

    auto* precision_group = new QGroupBox(
        tr("Working Color Precision"), rendering_page);
    configure_section(precision_group);
    auto* precision_form = new QFormLayout(precision_group);
    configure_form(precision_form);
    auto* precision_value = explanatory_label(
        tr("Automatic — native linear RGBA float32"), precision_group);
    precision_value->setObjectName(
        QStringLiteral("workingPrecisionStatus"));
    precision_form->addRow(tr("Internal precision"), precision_value);
    precision_form->addRow(explanatory_label(
        tr("CPU and GPU processing stays linear float32. Export bit depth remains "
           "a reproducible, project-local output choice."),
        precision_group,
        tr("The current CPU and GPU pipelines store and process colors as native "
           "32-bit floating-point RGBA. Simulating 1/2/4-bit precision by "
           "quantizing those buffers would add work and reduce quality, so it "
           "is not presented as a performance option. Export bit depth remains "
           "an authored, project-local choice for reproducible output.")));
    rendering_summary->addWidget(precision_group);
    rendering_summary->addStretch(1);
    rendering_columns->addLayout(rendering_summary, 2);
    rendering_columns->addWidget(threading_group, 3, Qt::AlignTop);
    rendering_layout->addLayout(rendering_columns);
    rendering_layout->addStretch(1);
    auto* rendering_scroll = scrollable_settings_page(
        rendering_page, tabs,
        QStringLiteral("applicationSettingsPerformanceScroll"));
    tabs->addTab(rendering_scroll, tr("Performance"));

    connect(tabs, &QTabWidget::currentChanged, this,
            [this](int) { scheduleResponsiveLabelLayout(); });
    root->addWidget(tabs, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void ApplicationSettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    QScreen* target = screen();
    if (target == nullptr) target = QGuiApplication::primaryScreen();
    if (target == nullptr) return;
    const QSize available = target->availableGeometry().size();
    const int maximum_width = (std::max)(360, available.width() - 32);
    const int maximum_height = (std::max)(320, available.height() - 32);
    setMaximumSize(maximum_width, maximum_height);
    setMinimumWidth((std::min)(680, maximum_width));
    resize((std::min)(kPreferredDialogWidth, maximum_width),
           (std::min)(kPreferredDialogHeight, maximum_height));
    scheduleResponsiveLabelLayout();
}

void ApplicationSettingsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    scheduleResponsiveLabelLayout();
}

void ApplicationSettingsDialog::scheduleResponsiveLabelLayout() {
    if (responsive_label_layout_pending_) return;
    responsive_label_layout_pending_ = true;
    QTimer::singleShot(0, this, [this] {
        responsive_label_layout_pending_ = false;
        const QList<QLabel*> labels = findChildren<QLabel*>();
        for (QLabel* label : labels) {
            if (!label->wordWrap()) continue;
            label->setMinimumHeight(0);
            QSizePolicy policy = label->sizePolicy();
            policy.setHorizontalPolicy(QSizePolicy::Preferred);
            policy.setVerticalPolicy(QSizePolicy::Minimum);
            policy.setHeightForWidth(true);
            label->setSizePolicy(policy);
            label->updateGeometry();
        }
        if (layout() != nullptr) {
            layout()->invalidate();
            layout()->activate();
        }
        for (QLabel* label : labels) {
            if (!label->wordWrap() || !label->hasHeightForWidth()) continue;
            const int width = label->contentsRect().width();
            if (width <= 0) continue;
            const int required = label->heightForWidth(width);
            if (required > 0) label->setMinimumHeight(required);
        }
    });
}

int ApplicationSettingsDialog::undoLimit() const {
    return undo_limit_->value();
}

PerformanceSettings ApplicationSettingsDialog::performanceSettings() const {
    PerformanceSettings settings;
    const int value = render_backend_->currentData().toInt();
    if (value >= static_cast<int>(RenderBackendPreference::Automatic)
        && value <= static_cast<int>(RenderBackendPreference::Gpu)) {
        settings.backend = static_cast<RenderBackendPreference>(value);
    }
    settings.preview_live_cpu_workers =
        static_cast<std::size_t>(preview_live_cpu_workers_->value());
    settings.export_frame_workers =
        static_cast<std::size_t>(export_frame_workers_->value());
    settings.export_cpu_workers =
        static_cast<std::size_t>(export_cpu_workers_->value());
    settings.gpu_frames_in_flight =
        static_cast<std::size_t>(gpu_frames_in_flight_->value());
    settings.render_memory_budget_mode =
        static_cast<RenderMemoryBudgetMode>(
            render_memory_budget_mode_->currentData().toInt());
    settings.render_memory_budget_value =
        render_memory_budget_value_->value();
    settings.pause_editor_preview_during_export =
        pause_editor_preview_during_export_->isChecked();
    return settings;
}

int ApplicationSettingsDialog::recentProjectLimit() const {
    return recent_project_limit_->value();
}

ApplicationSettingsDialog::NewProjectDefaultsAction
ApplicationSettingsDialog::newProjectDefaultsAction() const {
    return defaults_action_;
}
