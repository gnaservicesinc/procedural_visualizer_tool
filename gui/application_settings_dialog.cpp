#include "application_settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {

constexpr int kMinimumUndoLimit = 0;
constexpr int kMaximumUndoLimit = (std::numeric_limits<int>::max)();
constexpr int kMaximumRecentProjectLimit = (std::numeric_limits<int>::max)();
constexpr std::size_t kMebibyte = std::size_t{1024U} * 1024U;

int spin_box_maximum(std::size_t value) {
    return static_cast<int>((std::min)(
        value,
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
}

int spin_box_value(std::size_t value, int maximum) {
    return static_cast<int>((std::min)(
        value, static_cast<std::size_t>(maximum)));
}

QLabel* explanatory_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

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
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("applicationSettingsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    auto* content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(content);
    content_layout->addWidget(explanatory_label(
        tr("These settings apply to every project and persist after the "
           "application is closed."),
        content));

    auto* tabs = new QTabWidget(content);
    tabs->setObjectName(QStringLiteral("applicationSettingsTabs"));

    auto* general_page = new QWidget(tabs);
    auto* general_layout = new QVBoxLayout(general_page);
    auto* history_group = new QGroupBox(tr("Editing History"), general_page);
    auto* history_form = new QFormLayout(history_group);
    history_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    history_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
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
            tr("Changing this limit clears the current session's undo and redo "
               "history. The maximum follows Qt's signed-int command index; "
               "available memory is the practical limit."),
            history_group));
    general_layout->addWidget(history_group);

    auto* recent_group = new QGroupBox(tr("Recent Projects"), general_page);
    auto* recent_form = new QFormLayout(recent_group);
    recent_project_limit_ = new QSpinBox(recent_group);
    recent_project_limit_->setObjectName(QStringLiteral("recentProjectLimitPreference"));
    recent_project_limit_->setRange(0, kMaximumRecentProjectLimit);
    recent_project_limit_->setValue(std::clamp(
        recentProjectLimit, 0, kMaximumRecentProjectLimit));
    recent_form->addRow(tr("Projects shown (0 = disabled)"),
                        recent_project_limit_);
    recent_form->addRow(explanatory_label(
        tr("The File menu shows each project's name and full path. This is a local "
           "application preference and is not stored inside projects; its only "
           "upper bound is Qt's signed-int menu index."),
        recent_group));
    general_layout->addWidget(recent_group);

    auto* defaults_group = new QGroupBox(tr("New Projects"), general_page);
    auto* defaults_layout = new QVBoxLayout(defaults_group);
    defaults_layout->addWidget(explanatory_label(
        tr("Save the complete project currently open behind this dialog as a "
           "new-project template. New projects receive fresh project and layer "
           "identities; the template may include layers and embedded assets."),
        defaults_group));
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
    tabs->addTab(general_page, tr("General"));

    auto* rendering_page = new QWidget(tabs);
    auto* rendering_layout = new QVBoxLayout(rendering_page);
    auto* backend_group = new QGroupBox(tr("Acceleration"), rendering_page);
    auto* backend_form = new QFormLayout(backend_group);
    backend_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    backend_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
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
            tr("This machine-local backend is used for preview, LIVE, and export. "
               "Automatic is recommended for maximum throughput. On Windows and Linux, "
               "Qt OpenGL admits every valid layer and accelerates supported "
               "generated and surface stages. Ordered dependencies continue on "
               "bounded CPU lanes."),
            backend_group));
    rendering_layout->addWidget(backend_group);

    auto* threading_group = new QGroupBox(tr("Concurrency"), rendering_page);
    auto* threading_form = new QFormLayout(threading_group);
    threading_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    threading_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
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

    const std::size_t maximum_memory_mib =
        (std::numeric_limits<std::size_t>::max)() / kMebibyte;
    const int maximum_memory = spin_box_maximum(maximum_memory_mib);
    render_memory_budget_mib_ = new QSpinBox(threading_group);
    render_memory_budget_mib_->setObjectName(
        QStringLiteral("renderMemoryBudgetPreference"));
    render_memory_budget_mib_->setRange(0, maximum_memory);
    render_memory_budget_mib_->setSpecialValueText(tr("Auto (2 GiB)"));
    render_memory_budget_mib_->setSuffix(tr(" MiB"));
    render_memory_budget_mib_->setValue(spin_box_value(
        performanceSettings.render_memory_budget_mib, maximum_memory));
    render_memory_budget_mib_->setToolTip(tr(
        "Aggregate scheduling budget for project-layer working sets and parallel "
        "export frames. This is a concurrency cap, not a memory preallocation; "
        "a valid render can still run one worker when one frame exceeds it."));
    threading_form->addRow(tr("Render memory budget"),
                           render_memory_budget_mib_);

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
        render_memory_budget_mib_->setValue(0);
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
    rendering_layout->addWidget(threading_group);

    auto* precision_group = new QGroupBox(
        tr("Working Color Precision"), rendering_page);
    auto* precision_form = new QFormLayout(precision_group);
    auto* precision_value = explanatory_label(
        tr("Automatic — native linear RGBA float32"), precision_group);
    precision_value->setObjectName(
        QStringLiteral("workingPrecisionStatus"));
    precision_form->addRow(tr("Internal precision"), precision_value);
    precision_form->addRow(explanatory_label(
        tr("The current CPU and GPU pipelines store and process colors as native "
           "32-bit floating-point RGBA. Simulating 1/2/4-bit precision by "
           "quantizing those buffers would add work and reduce quality, so it "
           "is not presented as a performance option. Export bit depth remains "
           "an authored, project-local choice for reproducible output."),
        precision_group));
    rendering_layout->addWidget(precision_group);
    rendering_layout->addStretch(1);
    tabs->addTab(rendering_page, tr("Performance"));

    content_layout->addWidget(tabs, 1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
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
    setMinimumWidth((std::min)(520, maximum_width));
    const QSize desired = sizeHint().expandedTo(QSize(520, 560));
    resize((std::min)(desired.width(), maximum_width),
           (std::min)(desired.height(), maximum_height));
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
    settings.render_memory_budget_mib =
        static_cast<std::size_t>(render_memory_budget_mib_->value());
    return settings;
}

int ApplicationSettingsDialog::recentProjectLimit() const {
    return recent_project_limit_->value();
}

ApplicationSettingsDialog::NewProjectDefaultsAction
ApplicationSettingsDialog::newProjectDefaultsAction() const {
    return defaults_action_;
}
