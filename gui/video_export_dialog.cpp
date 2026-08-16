#include "video_export_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QVBoxLayout>

#include <limits>

namespace {

template <typename Enum>
void add_choice(QComboBox* combo, const QString& text, Enum value) {
    combo->addItem(text, static_cast<int>(value));
}

QLabel* wrapped_label(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

} // namespace

VideoExportDialog::VideoExportDialog(
    const pvt::video::Capabilities& available, bool projectHasAlpha,
    bool projectHasMusic, QWidget* parent)
    : QDialog(parent), capabilities_(available),
      project_has_alpha_(projectHasAlpha) {
    setWindowTitle(tr("Export Video"));
    setModal(true);
    setMinimumWidth(620);

    auto* root = new QVBoxLayout(this);
    root->addWidget(wrapped_label(
        tr("Native macOS export writes a QuickTime movie directly; it does not "
           "launch or bundle FFmpeg. VideoToolbox hardware encoding is selected "
           "for compressed formats when the chosen policy permits it."),
        this));
    if (!available.prores_4444 && !available.prores_4444_xq
        && !available.hevc && !available.status.empty()) {
        root->addWidget(wrapped_label(
            QString::fromStdString(available.status), this));
    }

    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    codec_ = new QComboBox(this);
    codec_->setObjectName(QStringLiteral("videoCodec"));
    if (available.png_lossless) {
        add_choice(codec_, tr("Lossless 8-bit RGB/RGBA — PNG in MOV"),
                   pvt::video::Codec::PngLossless);
    }
    if (available.prores_4444_xq) {
        add_choice(codec_, tr("Perceptually lossless+ — ProRes 4444 XQ"),
                   pvt::video::Codec::ProRes4444Xq);
    }
    if (available.prores_4444) {
        add_choice(codec_, tr("Perceptually lossless — ProRes 4444 (Recommended)"),
                   pvt::video::Codec::ProRes4444);
    }
    if (available.hevc) {
        add_choice(codec_, tr("High-fidelity compact — HEVC"),
                   pvt::video::Codec::Hevc);
    }
    const int recommended = codec_->findData(
        static_cast<int>(pvt::video::Codec::ProRes4444));
    if (recommended >= 0) codec_->setCurrentIndex(recommended);
    form->addRow(tr("Format"), codec_);

    hardware_ = new QComboBox(this);
    hardware_->setObjectName(QStringLiteral("videoHardwarePolicy"));
    add_choice(hardware_, tr("Prefer hardware; allow fallback (Recommended)"),
               pvt::video::HardwarePolicy::Prefer);
    add_choice(hardware_, tr("Require hardware"),
               pvt::video::HardwarePolicy::Require);
    add_choice(hardware_, tr("Software only"),
               pvt::video::HardwarePolicy::Software);
    form->addRow(tr("VideoToolbox encoder"), hardware_);

    hevc_quality_ = new QComboBox(this);
    hevc_quality_->setObjectName(QStringLiteral("hevcQuality"));
    add_choice(hevc_quality_, tr("Maximum fidelity — about 1.5 bits/pixel/frame"),
               pvt::video::HevcQuality::MaximumFidelity);
    add_choice(hevc_quality_, tr("Very light compression — about 0.75 bits/pixel/frame"),
               pvt::video::HevcQuality::VeryLightCompression);
    add_choice(hevc_quality_, tr("High quality — about 0.35 bits/pixel/frame"),
               pvt::video::HevcQuality::HighQuality);
    form->addRow(tr("HEVC data rate"), hevc_quality_);

    preserve_alpha_ = new QCheckBox(tr("Preserve transparency"), this);
    preserve_alpha_->setObjectName(QStringLiteral("videoPreserveAlpha"));
    preserve_alpha_->setChecked(projectHasAlpha);
    preserve_alpha_->setEnabled(projectHasAlpha);
    form->addRow(preserve_alpha_);

    include_music_ = new QCheckBox(
        tr("Include audible project and layer-clock music"), this);
    include_music_->setObjectName(QStringLiteral("videoIncludeMusic"));
    include_music_->setChecked(projectHasMusic);
    include_music_->setEnabled(projectHasMusic);
    include_music_->setToolTip(
        tr("Data-only sources remain silent. Audible layer clips use the same fit, loop, and one-shot mapping as their visual clocks."));
    form->addRow(include_music_);

    chunk_mode_ = new QComboBox(this);
    chunk_mode_->setObjectName(QStringLiteral("videoChunkMode"));
    add_choice(chunk_mode_, tr("One movie"),
               pvt::video::ChunkMode::SingleMovie);
    add_choice(chunk_mode_, tr("Split by frame count"),
               pvt::video::ChunkMode::FrameCount);
    add_choice(chunk_mode_, tr("Split by maximum duration"),
               pvt::video::ChunkMode::MaximumSeconds);
    form->addRow(tr("Output chunks"), chunk_mode_);

    chunk_frames_ = new QSpinBox(this);
    chunk_frames_->setObjectName(QStringLiteral("videoChunkFrames"));
    chunk_frames_->setRange(1, (std::numeric_limits<int>::max)());
    chunk_frames_->setValue(240);
    chunk_frames_->setSuffix(tr(" frames"));
    form->addRow(tr("Frames per chunk"), chunk_frames_);

    chunk_seconds_ = new QDoubleSpinBox(this);
    chunk_seconds_->setObjectName(QStringLiteral("videoChunkSeconds"));
    chunk_seconds_->setRange(0.001,
                             static_cast<double>((std::numeric_limits<int>::max)()));
    chunk_seconds_->setDecimals(3);
    chunk_seconds_->setValue(10.0);
    chunk_seconds_->setSuffix(tr(" seconds"));
    form->addRow(tr("Maximum chunk duration"), chunk_seconds_);
    root->addLayout(form);

    explanation_ = wrapped_label(QString{}, this);
    explanation_->setObjectName(QStringLiteral("videoChoiceExplanation"));
    root->addWidget(explanation_);
    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(codec_, &QComboBox::currentIndexChanged,
            this, [this] { updateChoiceState(); });
    connect(preserve_alpha_, &QCheckBox::toggled,
            this, [this] { updateChoiceState(); });
    connect(chunk_mode_, &QComboBox::currentIndexChanged,
            this, [this] { updateChoiceState(); });
    updateChoiceState();
}

void VideoExportDialog::updateChoiceState() {
    if (codec_->currentIndex() < 0) return;
    const auto selected = static_cast<pvt::video::Codec>(
        codec_->currentData().toInt());
    const bool lossless = selected == pvt::video::Codec::PngLossless;
    const bool hevc = selected == pvt::video::Codec::Hevc;
    hardware_->setEnabled(!lossless);
    hevc_quality_->setEnabled(hevc);
    const bool alpha_supported = !hevc || capabilities_.hevc_alpha;
    preserve_alpha_->setEnabled(project_has_alpha_ && alpha_supported);
    if (!alpha_supported) preserve_alpha_->setChecked(false);
    const auto chunk_mode = static_cast<pvt::video::ChunkMode>(
        chunk_mode_->currentData().toInt());
    chunk_frames_->setEnabled(
        chunk_mode == pvt::video::ChunkMode::FrameCount);
    chunk_seconds_->setEnabled(
        chunk_mode == pvt::video::ChunkMode::MaximumSeconds);

    if (lossless) {
        explanation_->setText(
            tr("Each frame is stored as a lossless PNG sample. This preserves "
               "the exact exported 8-bit sRGB pixels and alpha, but files can be enormous."));
    } else if (selected == pvt::video::Codec::ProRes4444Xq) {
        explanation_->setText(
            tr("ProRes 4444 XQ is the highest-fidelity editing codec offered here. "
               "It preserves alpha and is perceptually, not mathematically, lossless."));
    } else if (selected == pvt::video::Codec::ProRes4444) {
        explanation_->setText(
            tr("ProRes 4444 is the balanced choice for compression-sensitive, "
               "high-motion procedural imagery and preserves alpha."));
    } else {
        explanation_->setText(
            preserve_alpha_->isChecked()
                ? tr("HEVC with alpha is compact and hardware-accelerated where available. "
                     "The maximum-fidelity rate is intentionally far above ordinary video presets.")
                : tr("HEVC is compact and hardware-accelerated where available. The supplied "
                     "rates are intentionally generous for imagery where nearly every pixel changes."));
    }
}

pvt::video::Options VideoExportDialog::options() const {
    pvt::video::Options result;
    result.codec = static_cast<pvt::video::Codec>(codec_->currentData().toInt());
    result.hardware = static_cast<pvt::video::HardwarePolicy>(
        hardware_->currentData().toInt());
    result.hevc_quality = static_cast<pvt::video::HevcQuality>(
        hevc_quality_->currentData().toInt());
    result.preserve_alpha = preserve_alpha_->isChecked();
    result.include_project_music = include_music_->isChecked();
    result.chunk_mode = static_cast<pvt::video::ChunkMode>(
        chunk_mode_->currentData().toInt());
    result.chunk_frames = chunk_frames_->value();
    result.chunk_maximum_seconds = chunk_seconds_->value();
    return result;
}
