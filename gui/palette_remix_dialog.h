#pragma once

#include "palette_remix.h"

#include <QDialog>
#include <QImage>
#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class PaletteRemixPreview;
class PaletteRemixSwatches;

class PaletteRemixDialog final : public QDialog {
    Q_OBJECT
public:
    using Preview = std::function<void(const pvt::PaletteConfig&)>;
    PaletteRemixDialog(const pvt::PaletteConfig& original, Preview preview,
                       QWidget* parent = nullptr);
    const pvt::PaletteConfig& remixedPalette() const { return remixed_; }
    void setArtwork(const QImage& image);
    void setPreviewError(const QString& error);

private:
    void refresh();
    void reset();
    pvt::PaletteConfig original_;
    pvt::PaletteConfig remixed_;
    Preview preview_;
    QSpinBox* hue_ = nullptr;
    QSpinBox* saturation_ = nullptr;
    QDoubleSpinBox* exposure_ = nullptr;
    QSpinBox* offset_ = nullptr;
    QCheckBox* reverse_ = nullptr;
    QCheckBox* enabled_ = nullptr;
    QCheckBox* compare_ = nullptr;
    PaletteRemixPreview* artwork_ = nullptr;
    PaletteRemixSwatches* swatches_ = nullptr;
};
