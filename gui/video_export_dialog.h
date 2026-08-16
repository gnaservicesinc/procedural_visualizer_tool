#ifndef PVT_VIDEO_EXPORT_DIALOG_H
#define PVT_VIDEO_EXPORT_DIALOG_H

#include "video_export.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QDoubleSpinBox;
class QSpinBox;

class VideoExportDialog final : public QDialog {
public:
    VideoExportDialog(const pvt::video::Capabilities& capabilities,
                      bool projectHasAlpha, bool projectHasMusic,
                      QWidget* parent = nullptr);

    pvt::video::Options options() const;

private:
    void updateChoiceState();

    pvt::video::Capabilities capabilities_;
    bool project_has_alpha_ = false;
    QComboBox* codec_ = nullptr;
    QComboBox* hardware_ = nullptr;
    QComboBox* hevc_quality_ = nullptr;
    QCheckBox* preserve_alpha_ = nullptr;
    QCheckBox* include_music_ = nullptr;
    QComboBox* chunk_mode_ = nullptr;
    QSpinBox* chunk_frames_ = nullptr;
    QDoubleSpinBox* chunk_seconds_ = nullptr;
    QLabel* explanation_ = nullptr;
};

#endif
