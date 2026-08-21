#ifndef PVT_STAGE_OUTPUT_WINDOW_H
#define PVT_STAGE_OUTPUT_WINDOW_H

#include <QImage>
#include <QRect>
#include <QSize>
#include <QWidget>

class QKeyEvent;
class QCloseEvent;
class QEvent;
class QPaintEvent;
class QResizeEvent;
class QScreen;

class StageOutputWindow final : public QWidget {
    Q_OBJECT

public:
    explicit StageOutputWindow(QWidget* parent = nullptr);

    void showOnScreen(QScreen* screen);
    void showWindowedOnScreen(QScreen* screen, const QRect& geometry);
    void dismiss();
    void setFrame(const QImage& frame);
    void setFrozen(bool frozen);
    void setBlackout(bool blackout);
    void setSmoothScaling(bool smooth);
    bool isFrozen() const noexcept;
    bool isBlackout() const noexcept;
    bool hasGoodFrame() const noexcept;
    QSize outputPixelSize() const;
    void clearFrame();

signals:
    void dismissRequested();
    void outputMetricsChanged();

protected:
    bool event(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QImage last_good_frame_;
    bool frozen_ = false;
    bool blackout_ = false;
    bool smooth_scaling_ = true;
};

#endif
