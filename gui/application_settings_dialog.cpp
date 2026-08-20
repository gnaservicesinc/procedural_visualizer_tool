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
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {

constexpr int kMinimumUndoLimit = 0;
constexpr int kMaximumUndoLimit = (std::numeric_limits<int>::max)();
constexpr int kMaximumRecentProjectLimit = (std::numeric_limits<int>::max)();

QLabel* explanatory_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

ApplicationSettingsDialog::ApplicationSettingsDialog(
    int undoLimit, pvt::RenderBackend renderBackend,
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
    auto* backend_group = new QGroupBox(tr("Rendering"), rendering_page);
    auto* backend_form = new QFormLayout(backend_group);
    backend_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    backend_form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    render_backend_ = new QComboBox(backend_group);
    render_backend_->setObjectName(QStringLiteral("renderBackendPreference"));
    render_backend_->addItem(tr("CPU"), static_cast<int>(pvt::RenderBackend::Cpu));
    render_backend_->addItem(tr("CPU + GPU (Recommended)"),
                             static_cast<int>(pvt::RenderBackend::CpuAndGpu));
    render_backend_->addItem(tr("GPU"),
                             static_cast<int>(pvt::RenderBackend::Gpu));
    const int backend_index = render_backend_->findData(static_cast<int>(renderBackend));
    render_backend_->setCurrentIndex(backend_index >= 0 ? backend_index : 1);
    render_backend_->setToolTip(
        tr("CPU is the deterministic reference renderer. CPU + GPU uses Metal "
           "on macOS, accelerates supported analytic 3D surfaces with OpenGL "
           "on Windows and Linux, and renders independent layers on two CPU "
           "lanes. GPU is strict and reports unsupported work or runtime "
           "acceleration failures instead of silently retrying on CPU."));
    backend_form->addRow(tr("Backend"), render_backend_);
    const pvt::RendererCapabilities capabilities =
        capabilitiesOverride == nullptr ? pvt::renderer_capabilities()
                                        : *capabilitiesOverride;
    QString accelerator_status;
    if (capabilities.metal_available) {
        accelerator_status = tr("Metal ready: %1")
            .arg(QString::fromStdString(capabilities.metal_device_name));
    } else if (capabilities.opengl_surface_available) {
        accelerator_status = tr("OpenGL 3D surfaces ready: %1")
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
            tr("This backend is used for both live preview and export. CPU + GPU "
               "is recommended for maximum throughput. On Windows and Linux, "
               "Qt OpenGL accelerates built-in Cylinder, Sphere, and Cube "
               "surface mapping; Windows also accelerates flat Plane rotation. "
               "Independent layers use two bounded CPU lanes even if the GPU "
               "does not support a frame. Displacement planes and imported OBJ "
               "meshes remain ordered CPU stages."),
            backend_group));
    rendering_layout->addWidget(backend_group);
    rendering_layout->addStretch(1);
    tabs->addTab(rendering_page, tr("Rendering"));

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

pvt::RenderBackend ApplicationSettingsDialog::renderBackend() const {
    const int value = render_backend_->currentData().toInt();
    if (value < static_cast<int>(pvt::RenderBackend::Cpu)
        || value > static_cast<int>(pvt::RenderBackend::Gpu)) {
        return pvt::RenderBackend::CpuAndGpu;
    }
    return static_cast<pvt::RenderBackend>(value);
}

int ApplicationSettingsDialog::recentProjectLimit() const {
    return recent_project_limit_->value();
}

ApplicationSettingsDialog::NewProjectDefaultsAction
ApplicationSettingsDialog::newProjectDefaultsAction() const {
    return defaults_action_;
}
