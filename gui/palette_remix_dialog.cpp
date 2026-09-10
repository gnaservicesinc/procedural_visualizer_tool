#include "palette_remix_dialog.h"
#include "flexible_spin_box.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
QColor displayColor(const pvt::PaletteColor& color) {
    const auto channel = [&](double value) {
        return static_cast<float>(std::clamp(
            color.encoding == pvt::PaletteColorEncoding::Linear
                ? pvt::palette_remix::encode(value) : value, 0.0, 1.0));
    };
    return QColor::fromRgbF(channel(color.red), channel(color.green),
                            channel(color.blue), static_cast<float>(color.alpha));
}

void checkerboard(QPainter& painter, const QRect& rect) {
    painter.fillRect(rect, QColor(80, 80, 80));
    for (int y = rect.top(); y <= rect.bottom(); y += 10) {
        for (int x = rect.left(); x <= rect.right(); x += 10) {
            if (((x / 10) + (y / 10)) % 2 == 0)
                painter.fillRect(QRect(x, y, 10, 10).intersected(rect), QColor(120, 120, 120));
        }
    }
}
} // namespace

class PaletteRemixSwatches final : public QWidget {
public:
    explicit PaletteRemixSwatches(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(38);
        setMaximumHeight(38);
    }
    void setColors(const pvt::PaletteConfig& palette) { colors_ = palette.colors; update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        checkerboard(painter, rect());
        // At most one sample per display column, even for huge imported palettes.
        if (colors_.empty()) return;
        const auto count = std::min(colors_.size(), static_cast<std::size_t>(width()));
        for (std::size_t i = 0; i < count; ++i) {
            const int left = static_cast<int>(i * static_cast<std::size_t>(width()) / count);
            const int right = static_cast<int>((i + 1) * static_cast<std::size_t>(width()) / count);
            painter.fillRect(QRect(left, 0, right - left, height()),
                              displayColor(colors_[i * colors_.size() / count]));
        }
    }
private:
    std::vector<pvt::PaletteColor> colors_;
};

class PaletteRemixPreview final : public QWidget {
public:
    explicit PaletteRemixPreview(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    QImage image;
    QString message;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        checkerboard(painter, rect());
        if (!image.isNull()) {
            QSize size = image.size();
            size.scale(this->size(), Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - size.width()) / 2,
                                      (height() - size.height()) / 2), size);
            painter.drawImage(target, image);
        }
        if (!message.isEmpty()) {
            painter.fillRect(rect(), palette().color(QPalette::Window));
            painter.setPen(palette().color(QPalette::WindowText));
            painter.drawText(rect().adjusted(12, 12, -12, -12),
                              Qt::AlignCenter | Qt::TextWordWrap, message);
        }
    }
};

PaletteRemixDialog::PaletteRemixDialog(const pvt::PaletteConfig& original,
                                     Preview preview, QWidget* parent)
    : QDialog(parent), original_(original), remixed_(original), preview_(std::move(preview)) {
    setObjectName(QStringLiteral("paletteRemixDialog"));
    setWindowTitle(tr("Palette Remix"));
    auto* layout = new QVBoxLayout(this);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* body = new QVBoxLayout(content);
    auto* help = new QLabel(tr("Explore a new mood for this layer. Every adjustment starts from your original palette. Apply keeps the result as one undoable edit."));
    help->setWordWrap(true);
    body->addWidget(help);
    artwork_ = new PaletteRemixPreview;
    artwork_->message = tr("Rendering preview…");
    body->addWidget(artwork_, 1);
    body->addWidget(new QLabel(tr("Original palette")));
    auto* original_swatches = new PaletteRemixSwatches;
    original_swatches->setColors(original_);
    body->addWidget(original_swatches);
    body->addWidget(new QLabel(tr("Remixed palette")));
    swatches_ = new PaletteRemixSwatches;
    body->addWidget(swatches_);
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto control = [&](const QString& label, const char* name, int min, int max,
                             int initial, const QString& suffix) {
        auto* row = new QWidget;
        auto* horizontal = new QHBoxLayout(row);
        horizontal->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(min, max);
        slider->setValue(initial);
        slider->setAccessibleName(label);
        auto* value = new QSpinBox;
        value->setObjectName(QString::fromLatin1(name));
        value->setRange(min, max);
        value->setValue(initial);
        value->setSuffix(suffix);
        value->setKeyboardTracking(false);
        value->setAccessibleName(label);
        horizontal->addWidget(slider, 1);
        horizontal->addWidget(value);
        form->addRow(label, row);
        connect(slider, &QSlider::valueChanged, value, &QSpinBox::setValue);
        connect(value, &QSpinBox::valueChanged, slider, &QSlider::setValue);
        connect(value, &QSpinBox::valueChanged, this, &PaletteRemixDialog::refresh);
        return value;
    };
    hue_ = control(tr("Hue rotation"), "paletteRemixHue", -180, 180, 0, tr("°"));
    saturation_ = control(tr("Saturation"), "paletteRemixSaturation", 0, 200, 100, tr("%"));
    auto* exposure_row = new QWidget;
    auto* exposure_layout = new QHBoxLayout(exposure_row);
    exposure_layout->setContentsMargins(0, 0, 0, 0);
    auto* exposure_slider = new QSlider(Qt::Horizontal);
    exposure_slider->setRange(-200, 200);
    exposure_slider->setAccessibleName(tr("Exposure"));
    exposure_ = new FlexibleDoubleSpinBox;
    exposure_->setObjectName(QStringLiteral("paletteRemixExposure"));
    exposure_->setRange(-2.0, 2.0);
    exposure_->setDecimals(2);
    exposure_->setSingleStep(0.05);
    exposure_->setKeyboardTracking(false);
    exposure_->setAccessibleName(tr("Exposure"));
    exposure_->setSuffix(tr(" stops"));
    exposure_->setToolTip(tr("+1 stop doubles linear-light brightness; −1 stop halves it. sRGB colors stay within their display range. Linear/HDR colors keep their encoding and extended range."));
    exposure_layout->addWidget(exposure_slider, 1);
    exposure_layout->addWidget(exposure_);
    form->addRow(tr("Exposure"), exposure_row);
    connect(exposure_slider, &QSlider::valueChanged, this, [this](int value) {
        exposure_->setValue(value / 100.0);
    });
    connect(exposure_, &QDoubleSpinBox::valueChanged, this, [this, exposure_slider](double value) {
        exposure_slider->setValue(qRound(value * 100.0));
        refresh();
    });
    offset_ = control(tr("Rotate color order"), "paletteRemixOffset", 0,
        static_cast<int>(original_.colors.empty() ? 0 : original_.colors.size() - 1), 0, {});
    body->addLayout(form);
    reverse_ = new QCheckBox(tr("Reverse color order"));
    reverse_->setObjectName(QStringLiteral("paletteRemixReverse"));
    enabled_ = new QCheckBox(tr("Use this palette for starting colors"));
    enabled_->setObjectName(QStringLiteral("paletteRemixEnabled"));
    enabled_->setChecked(original_.enabled);
    body->addWidget(reverse_);
    body->addWidget(enabled_);
    auto* note = new QLabel(tr("Alpha, entry names, and color encodings are retained. Display swatches clip HDR colors. Starting-image layers use the palette only when palette dithering is enabled."));
    note->setWordWrap(true);
    body->addWidget(note);
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);
    auto* actions = new QHBoxLayout;
    auto* surprise = new QPushButton(tr("Surprise me"));
    surprise->setObjectName(QStringLiteral("paletteRemixSurprise"));
    auto* reset_button = new QPushButton(tr("Reset"));
    reset_button->setObjectName(QStringLiteral("paletteRemixReset"));
    compare_ = new QCheckBox(tr("Show original"));
    compare_->setObjectName(QStringLiteral("paletteRemixCompare"));
    actions->addWidget(surprise);
    actions->addWidget(reset_button);
    actions->addStretch();
    actions->addWidget(compare_);
    layout->addLayout(actions);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply remix"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(reverse_, &QCheckBox::toggled, this, &PaletteRemixDialog::refresh);
    connect(enabled_, &QCheckBox::toggled, this, &PaletteRemixDialog::refresh);
    connect(compare_, &QCheckBox::toggled, this, &PaletteRemixDialog::refresh);
    connect(reset_button, &QPushButton::clicked, this, &PaletteRemixDialog::reset);
    connect(surprise, &QPushButton::clicked, this, [this] {
        // Batch changes into one preview request, then synchronize the sliders.
        const QSignalBlocker hue_block(hue_), sat_block(saturation_),
            exposure_block(exposure_), offset_block(offset_), reverse_block(reverse_),
            compare_block(compare_);
        auto* random = QRandomGenerator::global();
        hue_->setValue(random->bounded(-180, 181));
        saturation_->setValue(random->bounded(55, 161));
        exposure_->setValue(random->bounded(-65, 66) / 100.0);
        offset_->setValue(random->bounded(offset_->maximum() + 1));
        reverse_->setChecked(random->bounded(2) != 0);
        compare_->setChecked(false);
        // Refresh linked sliders explicitly after this batched change.
        for (auto* value : {hue_, saturation_, offset_})
            value->parentWidget()->findChild<QSlider*>()->setValue(value->value());
        exposure_->parentWidget()->findChild<QSlider*>()->setValue(qRound(exposure_->value() * 100.0));
        refresh();
    });
    refresh();
    const QSize available = screen()->availableGeometry().size() - QSize(40, 80);
    resize(QSize(720, 780).boundedTo(available));
}

void PaletteRemixDialog::refresh() {
    if (!compare_) return; // Controls are initialized in order.
    pvt::palette_remix::Settings settings;
    settings.hue_degrees = hue_->value();
    settings.saturation = saturation_->value() / 100.0;
    settings.exposure_stops = exposure_->value();
    settings.offset = static_cast<std::size_t>(offset_->value());
    settings.reverse = reverse_->isChecked();
    remixed_ = pvt::palette_remix::apply(original_, settings);
    remixed_.enabled = enabled_->isChecked();
    swatches_->setColors(remixed_);
    if (preview_) preview_(compare_->isChecked() ? original_ : remixed_);
}

void PaletteRemixDialog::reset() {
    const QSignalBlocker hue_block(hue_), sat_block(saturation_),
        exposure_block(exposure_), offset_block(offset_), reverse_block(reverse_),
        enabled_block(enabled_), compare_block(compare_);
    hue_->setValue(0);
    saturation_->setValue(100);
    exposure_->setValue(0);
    offset_->setValue(0);
    reverse_->setChecked(false);
    enabled_->setChecked(original_.enabled);
    compare_->setChecked(false);
    for (auto* value : {hue_, saturation_, offset_})
        value->parentWidget()->findChild<QSlider*>()->setValue(value->value());
    exposure_->parentWidget()->findChild<QSlider*>()->setValue(0);
    refresh();
}

void PaletteRemixDialog::setArtwork(const QImage& image) {
    artwork_->image = image;
    artwork_->message.clear();
    artwork_->update();
}

void PaletteRemixDialog::setPreviewError(const QString& error) {
    artwork_->message = error;
    artwork_->update();
}
