#include "audio_processing_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

QString frequencyLabel(double hz) {
    return hz >= 1000.0 ? QStringLiteral("%1k").arg(hz / 1000.0, 0, 'g', 3)
                        : QString::number(hz, 'g', 4);
}

QString newUuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
}

} // namespace

AudioProcessingDialog::AudioProcessingDialog(
    const pvt::AudioInputProcessingConfig& initial,
    const QString& source_name, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Audio Input Processing — %1").arg(source_name));
    resize(820, 720);
    auto* root = new QVBoxLayout(this);
    auto* intro = new QLabel(tr(
        "This chain runs before beat detection, envelopes, spectrum, chroma, or "
        "named frequency splits. Everything is bypassed by default. Music "
        "changes are committed only after a successful reanalysis."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* filters = new QGroupBox(tr("Input filters"));
    auto* filter_form = new QFormLayout(filters);
    high_pass_enabled_ = new QCheckBox(tr("Enable high-pass"));
    high_pass_enabled_->setChecked(initial.high_pass_enabled);
    high_pass_hz_ = new QDoubleSpinBox;
    high_pass_hz_->setRange(0.001, 192000.0);
    high_pass_hz_->setDecimals(3);
    high_pass_hz_->setSuffix(tr(" Hz"));
    high_pass_hz_->setValue(initial.high_pass_hz);
    low_pass_enabled_ = new QCheckBox(tr("Enable low-pass"));
    low_pass_enabled_->setChecked(initial.low_pass_enabled);
    low_pass_hz_ = new QDoubleSpinBox;
    low_pass_hz_->setRange(0.001, 192000.0);
    low_pass_hz_->setDecimals(3);
    low_pass_hz_->setSuffix(tr(" Hz"));
    low_pass_hz_->setValue(initial.low_pass_hz);
    auto* high_row = new QWidget;
    auto* high_layout = new QHBoxLayout(high_row);
    high_layout->setContentsMargins(0, 0, 0, 0);
    high_layout->addWidget(high_pass_enabled_);
    high_layout->addWidget(high_pass_hz_, 1);
    auto* low_row = new QWidget;
    auto* low_layout = new QHBoxLayout(low_row);
    low_layout->setContentsMargins(0, 0, 0, 0);
    low_layout->addWidget(low_pass_enabled_);
    low_layout->addWidget(low_pass_hz_, 1);
    filter_form->addRow(tr("Remove lows"), high_row);
    filter_form->addRow(tr("Remove highs"), low_row);
    root->addWidget(filters);

    auto* equalizer = new QGroupBox(tr("Graphical equalizer"));
    auto* equalizer_layout = new QVBoxLayout(equalizer);
    equalizer_enabled_ = new QCheckBox(tr("Enable multi-band EQ"));
    equalizer_enabled_->setChecked(initial.equalizer_enabled);
    equalizer_layout->addWidget(equalizer_enabled_);
    auto* eq_scroll = new QScrollArea;
    eq_scroll->setWidgetResizable(true);
    eq_scroll->setFrameShape(QFrame::NoFrame);
    auto* eq_body = new QWidget;
    auto* eq_layout = new QHBoxLayout(eq_body);
    eq_layout->setContentsMargins(4, 4, 4, 4);
    for (const auto& band : initial.equalizer_bands) {
        auto* column = new QWidget;
        auto* column_layout = new QVBoxLayout(column);
        column_layout->setContentsMargins(2, 0, 2, 0);
        auto* value = new QDoubleSpinBox;
        value->setRange(-24.0, 24.0);
        value->setDecimals(1);
        value->setSuffix(tr(" dB"));
        value->setValue(band.gain_db);
        value->setAlignment(Qt::AlignCenter);
        value->setMaximumWidth(78);
        auto* slider = new QSlider(Qt::Vertical);
        slider->setRange(-240, 240);
        slider->setValue(static_cast<int>(std::lround(band.gain_db * 10.0)));
        slider->setTickPosition(QSlider::TicksBothSides);
        slider->setTickInterval(60);
        auto* frequency = new QLabel(frequencyLabel(band.frequency_hz));
        frequency->setAlignment(Qt::AlignCenter);
        column_layout->addWidget(value);
        column_layout->addWidget(slider, 1, Qt::AlignHCenter);
        column_layout->addWidget(frequency);
        eq_layout->addWidget(column);
        equalizer_gains_.push_back(value);
        equalizer_frequencies_.push_back(band.frequency_hz);
        connect(slider, &QSlider::valueChanged, this,
                [value](int amount) { value->setValue(amount / 10.0); });
        connect(value, &QDoubleSpinBox::valueChanged, this,
                [slider](double amount) {
                    slider->setValue(static_cast<int>(std::lround(amount * 10.0)));
                });
    }
    eq_layout->addStretch(1);
    eq_scroll->setWidget(eq_body);
    eq_scroll->setMinimumHeight(250);
    equalizer_layout->addWidget(eq_scroll);
    root->addWidget(equalizer, 1);

    auto* stream_group = new QGroupBox(tr("Named post-filter frequency streams"));
    auto* stream_layout = new QVBoxLayout(stream_group);
    auto* stream_help = new QLabel(tr(
        "Each row becomes a selectable clock source. Ranges may overlap; names "
        "are artist-facing while stable IDs preserve routes when rows move."));
    stream_help->setWordWrap(true);
    stream_layout->addWidget(stream_help);
    streams_ = new QTableWidget(0, 3);
    streams_->setHorizontalHeaderLabels({tr("Label"), tr("Low Hz"), tr("High Hz")});
    streams_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    streams_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    streams_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    streams_->verticalHeader()->hide();
    streams_->setSelectionBehavior(QAbstractItemView::SelectRows);
    streams_->setSelectionMode(QAbstractItemView::SingleSelection);
    stream_layout->addWidget(streams_);
    auto* stream_buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add Range"));
    auto* remove = new QPushButton(tr("Remove Selected"));
    stream_buttons->addWidget(add);
    stream_buttons->addWidget(remove);
    stream_buttons->addStretch(1);
    stream_layout->addLayout(stream_buttons);
    root->addWidget(stream_group, 1);
    for (const auto& stream : initial.frequency_streams) addFrequencyStream(&stream);
    connect(add, &QPushButton::clicked, this,
            [this] { addFrequencyStream(); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = streams_->currentRow();
        if (row >= 0) streams_->removeRow(row);
    });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted,
            this, &AudioProcessingDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);
}

void AudioProcessingDialog::addFrequencyStream(
    const pvt::AudioFrequencyStreamConfig* stream) {
    if (streams_->rowCount()
        >= static_cast<int>(pvt::kMaximumAudioFrequencyStreams)) return;
    const int row = streams_->rowCount();
    streams_->insertRow(row);
    auto* name = new QTableWidgetItem(
        stream != nullptr ? QString::fromStdString(stream->name)
                          : tr("Range %1").arg(row + 1));
    name->setData(Qt::UserRole,
                  stream != nullptr ? QString::fromStdString(stream->uuid)
                                    : newUuid());
    streams_->setItem(row, 0, name);
    streams_->setItem(row, 1, new QTableWidgetItem(QString::number(
        stream != nullptr ? stream->low_hz : 20.0, 'g', 12)));
    streams_->setItem(row, 2, new QTableWidgetItem(QString::number(
        stream != nullptr ? stream->high_hz : 200.0, 'g', 12)));
    streams_->setCurrentCell(row, 0);
}

pvt::AudioInputProcessingConfig AudioProcessingDialog::processing() const {
    pvt::AudioInputProcessingConfig result;
    result.high_pass_enabled = high_pass_enabled_->isChecked();
    result.high_pass_hz = high_pass_hz_->value();
    result.low_pass_enabled = low_pass_enabled_->isChecked();
    result.low_pass_hz = low_pass_hz_->value();
    result.equalizer_enabled = equalizer_enabled_->isChecked();
    result.equalizer_bands.clear();
    result.equalizer_bands.reserve(equalizer_gains_.size());
    for (std::size_t index = 0U; index < equalizer_gains_.size(); ++index) {
        result.equalizer_bands.push_back(
            {equalizer_frequencies_[index], equalizer_gains_[index]->value()});
    }
    result.frequency_streams.reserve(
        static_cast<std::size_t>(streams_->rowCount()));
    for (int row = 0; row < streams_->rowCount(); ++row) {
        pvt::AudioFrequencyStreamConfig stream;
        stream.uuid = streams_->item(row, 0)->data(Qt::UserRole)
                          .toString().toStdString();
        stream.name = streams_->item(row, 0)->text().trimmed().toStdString();
        stream.low_hz = streams_->item(row, 1)->text().toDouble();
        stream.high_hz = streams_->item(row, 2)->text().toDouble();
        result.frequency_streams.push_back(std::move(stream));
    }
    return result;
}

void AudioProcessingDialog::accept() {
    const pvt::ValidationResult validation = pvt::validate(processing());
    if (!validation.ok) {
        QMessageBox::warning(this, tr("Audio input processing"),
                             QString::fromStdString(validation.message));
        return;
    }
    QDialog::accept();
}
