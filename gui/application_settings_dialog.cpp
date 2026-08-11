#include "application_settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr int kMinimumUndoLimit = 10;
constexpr int kMaximumUndoLimit = 5000;

QLabel* explanatory_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

ApplicationSettingsDialog::ApplicationSettingsDialog(
    int undoLimit, pvt::RenderBackend renderBackend, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("applicationSettingsDialog"));
    setWindowTitle(tr("Application Settings"));
    setModal(true);
    setMinimumWidth(520);

    auto* root = new QVBoxLayout(this);
    root->addWidget(explanatory_label(
        tr("These settings apply to every project and persist after the "
           "application is closed."),
        this));

    auto* tabs = new QTabWidget(this);
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
    history_form->addRow(tr("Maximum undo steps"), undo_limit_);
    history_form->addRow(
        explanatory_label(
            tr("Changing this limit clears the current session's undo and redo "
               "history. A separate 128 MiB safety limit always remains active."),
            history_group));
    general_layout->addWidget(history_group);
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
    render_backend_->addItem(tr("GPU (Strict)"),
                             static_cast<int>(pvt::RenderBackend::Gpu));
    const int backend_index = render_backend_->findData(static_cast<int>(renderBackend));
    render_backend_->setCurrentIndex(backend_index >= 0 ? backend_index : 1);
    render_backend_->setToolTip(
        tr("CPU is the deterministic reference renderer. CPU + GPU uses Metal "
           "where supported and falls back to CPU. GPU (Strict) requires Metal "
           "and reports unsupported work."));
    backend_form->addRow(tr("Backend"), render_backend_);
    backend_form->addRow(
        explanatory_label(
            tr("This backend is used for both live preview and export. CPU + GPU "
               "is recommended for normal use; strict GPU mode is useful for "
               "diagnosing Metal compatibility."),
            backend_group));
    rendering_layout->addWidget(backend_group);
    rendering_layout->addStretch(1);
    tabs->addTab(rendering_page, tr("Rendering"));

    root->addWidget(tabs, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
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
