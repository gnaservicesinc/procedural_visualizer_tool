#ifndef PVT_AUDIO_PROCESSING_DIALOG_H
#define PVT_AUDIO_PROCESSING_DIALOG_H

#include "procedural_visualizer_tool.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QTableWidget;

class AudioProcessingDialog final : public QDialog {
public:
    explicit AudioProcessingDialog(
        const pvt::AudioInputProcessingConfig& initial,
        const QString& source_name, QWidget* parent = nullptr);

    pvt::AudioInputProcessingConfig processing() const;

protected:
    void accept() override;

private:
    void addFrequencyStream(const pvt::AudioFrequencyStreamConfig* stream = nullptr);
    void applyEqualizerPreset(int preset);

    QCheckBox* high_pass_enabled_ = nullptr;
    QDoubleSpinBox* high_pass_hz_ = nullptr;
    QCheckBox* low_pass_enabled_ = nullptr;
    QDoubleSpinBox* low_pass_hz_ = nullptr;
    QCheckBox* equalizer_enabled_ = nullptr;
    QTableWidget* streams_ = nullptr;
    std::vector<QDoubleSpinBox*> equalizer_gains_;
    std::vector<double> equalizer_frequencies_;
};

#endif
