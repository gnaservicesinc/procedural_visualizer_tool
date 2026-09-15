#include "main_window.h"
#include "photo_import.h"
#include "../src/project_bundle.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QVBoxLayout>

bool MainWindow::importPhotoSource(const QString& path) {
    ImportedPhoto photo;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool extracted = extractPhoto(path, photo, error);
    if (extracted) derivePhotoImages(photo);
    QApplication::restoreOverrideCursor();
    if (!extracted) { QMessageBox::warning(this, tr("Could not import photo"), error); return false; }
    QTemporaryDir temporary;
    if (!temporary.isValid()) return false;
    const QString stem = QFileInfo(path).completeBaseName();
    const QString primary = temporary.filePath(stem + ".png");
    if (!photo.primary.save(primary, "PNG")) return false;
    std::vector<pvt::DerivedImage> images;
    // Keep the initial color view available after choosing a cutout or mask.
    photo.images.prepend({"color", tr("Original color view"), photo.primary});
    for (int i = 0; i < photo.images.size(); ++i) {
        const auto& image = photo.images[i];
        const QString file = temporary.filePath(stem + QString("-%1.png").arg(i + 1));
        if (!image.image.save(file, "PNG")) { QMessageBox::warning(this, tr("Could not import photo"), tr("A derived image could not be saved.")); return false; }
        images.push_back({image.kind.toStdString(), image.name.toStdString(), file.toStdString(), {}, {}});
    }
    if (!setStartingImageSource(primary, &images)) return false;
    showPhotoInspector(photo.notes);
    return true;
}

void MainWindow::showPhotoInspector(const QString& notes) {
    if (!activeLayer() || config_.starting_image.derived_images.empty()) {
        QMessageBox::information(this, tr("Photo images"), tr("Import a portrait or spatial HEIC/HEIF/JPEG photo to inspect its available images and masks.")); return;
    }
    QDialog dialog(this); dialog.setWindowTitle(tr("Photo images and depth")); dialog.resize(850, 650);
    auto* layout = new QVBoxLayout(&dialog);
    auto* help = new QLabel(notes + tr("\nInspect each image before using it. Depth is relative, near = white; it does not reconstruct hidden surfaces. Export saves the full-resolution PNG. Photo depth adds to an enabled Plane height map; it never replaces it."));
    help->setWordWrap(true); layout->addWidget(help);
    auto* row = new QHBoxLayout;
    auto* list = new QListWidget; list->setMinimumWidth(240); row->addWidget(list);
    auto* preview = new QLabel; preview->setAlignment(Qt::AlignCenter); preview->setMinimumSize(380, 250); row->addWidget(preview, 1); layout->addLayout(row, 1);
    auto* details = new QLabel; layout->addWidget(details);
    const auto assets = config_.starting_image.derived_images;
    for (const auto& asset : assets) list->addItem(QString::fromStdString(asset.name + " · " + asset.kind));
    const auto refresh = [&] {
        if (list->currentRow() < 0) return;
        const auto& asset = assets[static_cast<std::size_t>(list->currentRow())];
        QImage image(QString::fromStdString(asset.path));
        preview->setPixmap(QPixmap::fromImage(image).scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        details->setText(tr("%1 × %2 · %3").arg(image.width()).arg(image.height()).arg(QString::fromStdString(asset.name)));
    };
    connect(list, &QListWidget::currentRowChanged, &dialog, refresh);
    auto* actions = new QHBoxLayout;
    auto* use = new QPushButton(tr("Use selected as starting image"));
    auto* export_image = new QPushButton(tr("Export selected PNG…"));
    auto* remove = new QPushButton(tr("Remove selected derived image"));
    actions->addWidget(use); actions->addWidget(export_image); actions->addWidget(remove); layout->addLayout(actions);
    connect(export_image, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        const auto& asset = assets[static_cast<std::size_t>(list->currentRow())];
        const QString target = QFileDialog::getSaveFileName(&dialog, tr("Export photo image"), QString::fromStdString(asset.basename), tr("PNG images (*.png)"));
        if (target.isEmpty()) return;
        QFile source(QString::fromStdString(asset.path)); QSaveFile output(target);
        if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) { details->setText(tr("Could not open the export file.")); return; }
        while (!source.atEnd()) {
            const auto block = source.read(1024 * 1024);
            if (block.isEmpty() || output.write(block) != block.size()) { details->setText(tr("Could not write the export file.")); return; }
        }
        details->setText(output.commit() ? tr("Exported full-resolution PNG.") : output.errorString());
    });
    connect(use, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        const auto asset = assets[static_cast<std::size_t>(list->currentRow())];
        if (setStartingImageSource(QString::fromStdString(asset.path), &assets)) dialog.accept();
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (list->currentRow() < 0) return;
        auto remaining = assets; remaining.erase(remaining.begin() + list->currentRow());
        if (setStartingImageSource(QString::fromStdString(config_.starting_image.path), &remaining)) dialog.accept();
    });
    auto* form = new QFormLayout;
    auto* enabled = new QCheckBox(tr("Use photo depth")); enabled->setObjectName("photoDepthEnabled"); enabled->setChecked(config_.starting_image.depth_enabled);
    const bool has_depth = std::any_of(assets.begin(), assets.end(), [](const auto& a) { return a.kind == "depth"; });
    enabled->setEnabled(has_depth);
    auto* lighting = new QCheckBox(tr("Depth lighting and self-shadows")); lighting->setChecked(config_.starting_image.depth_lighting);
    lighting->setToolTip(tr("Uses the layer's light direction and environment map. This is a shallow height-field approximation, not a complete 3D scene."));
    auto* amount = new QDoubleSpinBox; amount->setRange(0, 1); amount->setSingleStep(.01); amount->setValue(config_.starting_image.depth_amount);
    auto* tilt_x = new QDoubleSpinBox; auto* tilt_y = new QDoubleSpinBox;
    for (auto* tilt : {tilt_x, tilt_y}) { tilt->setRange(-10, 10); tilt->setSingleStep(.5); tilt->setSuffix(tr("°")); }
    tilt_x->setValue(config_.starting_image.depth_tilt_x); tilt_y->setValue(config_.starting_image.depth_tilt_y);
    form->addRow(enabled); form->addRow(lighting); form->addRow(tr("Depth strength"), amount);
    form->addRow(tr("Tilt up / down"), tilt_x); form->addRow(tr("Tilt left / right"), tilt_y); layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
        auto before = captureActiveState();
        auto& photo = config_.starting_image;
        photo.depth_enabled = enabled->isChecked(); photo.depth_lighting = lighting->isChecked(); photo.depth_amount = amount->value();
        photo.depth_tilt_x = tilt_x->value(); photo.depth_tilt_y = tilt_y->value();
        syncActiveRender(); syncProjectGlobals();
        if (document_) { document_->project = project_; document_->dirty = true; }
        recordActiveStateChange(tr("Photo depth settings"), std::move(before));
        schedulePreview();
    });
    list->setCurrentRow(0); dialog.exec();
}

#include <QTimer>
#include <QUndoStack>
#include <QTabWidget>
#include <QScrollArea>
bool MainWindow::runPhotoSmokeChecks(QString* error) {
    const auto fail = [&](const char* text) { if (error) *error = QString::fromUtf8(text); return false; };
    const auto original = captureProjectState(); const auto active = active_layer_uuid_;
    QTemporaryDir directory;
    QImage image(64,64,QImage::Format_RGBA8888); image.fill(Qt::green);
    const auto path = directory.filePath("photo.png");
    if (!image.save(path)) return fail("Could not create photo smoke fixture.");
    std::vector<pvt::DerivedImage> assets{{"color","Original",path.toStdString(),{},{}},{"depth","Depth",path.toStdString(),{},{}}};
    config_.output.write_alpha = true; project_.output.write_alpha = true;
    if (!setStartingImageSource(path,&assets)) return fail("Photo attachments were not imported.");
    if (!pvt::find_project_attachment(*document_,pvt::derived_image_attachment_id(active,1))) return fail("Photo depth is missing from the attachment registry.");
    undo_stack_->undo();
    if (!config_.starting_image.derived_images.empty()) return fail("Photo import did not undo.");
    undo_stack_->redo();
    bool edited = false;
    QTimer::singleShot(0, this, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* enabled = dialog->findChild<QCheckBox*>("photoDepthEnabled");
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* list = dialog->findChild<QListWidget*>();
        if (!enabled || !buttons || !list || list->count()!=2) { dialog->reject(); return; }
        enabled->setChecked(true); buttons->button(QDialogButtonBox::Apply)->click(); edited = true;
        if (qEnvironmentVariableIsSet("PVT_PHOTO_SMOKE_SCREENSHOTS")) dialog->grab().save("/tmp/pvt-photo-inspector.png");
        dialog->reject();
    });
    showPhotoInspector();
    if (!edited || !config_.starting_image.depth_enabled) return fail("Photo inspector did not edit the shared starting-image model.");
    undo_stack_->undo();
    if (config_.starting_image.depth_enabled) return fail("Photo settings did not undo.");
    undo_stack_->redo();
    duplicateLayer();
    const auto duplicate = active_layer_uuid_;
    if (duplicate==active || !pvt::find_project_attachment(*document_,pvt::derived_image_attachment_id(duplicate,1))) return fail("Duplicate layer lost photo attachments.");
    removeLayer();
    if (pvt::find_project_attachment(*document_,pvt::derived_image_attachment_id(duplicate,1))) return fail("Removed layer retained photo attachments.");
    undo_stack_->undo();
    if (!pvt::find_project_attachment(*document_,pvt::derived_image_attachment_id(duplicate,1))) return fail("Undo did not restore photo attachments.");
    if (!setStartingImageSource({}) || !config_.starting_image.derived_images.empty()) return fail("Clearing a photo retained derived images.");
    undo_stack_->undo();
    if (config_.starting_image.derived_images.size()!=2) return fail("Undo did not restore a cleared photo.");
    restoreProjectState(original, active); undo_stack_->clear();
    if (qEnvironmentVariableIsSet("PVT_PHOTO_SMOKE_SCREENSHOTS")) {
        QTimer::singleShot(250, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            dialog->grab().save("/tmp/pvt-remotes-settings.png"); dialog->reject();
        });
        showApplicationSettings(true);
    }
    return true;
}
