#ifndef PVT_APPLICATION_SETTINGS_DIALOG_H
#define PVT_APPLICATION_SETTINGS_DIALOG_H

#include "procedural_visualizer_tool.h"

#include <QDialog>

class QComboBox;
class QSpinBox;

class ApplicationSettingsDialog final : public QDialog {
public:
    ApplicationSettingsDialog(int undoLimit, pvt::RenderBackend renderBackend,
                              QWidget* parent = nullptr);

    int undoLimit() const;
    pvt::RenderBackend renderBackend() const;

private:
    QSpinBox* undo_limit_ = nullptr;
    QComboBox* render_backend_ = nullptr;
};

#endif
