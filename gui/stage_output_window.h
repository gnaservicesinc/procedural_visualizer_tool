#ifndef PVT_STAGE_OUTPUT_WINDOW_H
#define PVT_STAGE_OUTPUT_WINDOW_H

#include <QImage>
#include <QWidget>

class QKeyEvent;
class QPaintEvent;
class QScreen;

class StageOutputWindow final : public QWidget {
    Q_OBJECT

public:
    explicit StageOutputWindow(QWidget* parent = nullptr);

    void showOnScreen(QScreen* screen);
    void setFrame(const QImage& frame);
    void setFrozen(bool frozen);
    void setBlackout(bool blackout);
    void setSmoothScaling(bool smooth);
    bool isFrozen() const noexcept;
    bool isBlackout() const noexcept;
    bool hasGoodFrame() const noexcept;
    void clearFrame();

signals:
    void escapeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QImage last_good_frame_;
    bool frozen_ = false;
    bool blackout_ = false;
    bool smooth_scaling_ = true;
};

#endif
