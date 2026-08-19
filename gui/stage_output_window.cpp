#include "stage_output_window.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QWindow>

#include <algorithm>
#include <cmath>

StageOutputWindow::StageOutputWindow(QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("stageOutputWindow"));
    setAccessibleName(tr("Live stage output"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setCursor(Qt::BlankCursor);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral("background: #000000;"));
}

void StageOutputWindow::showOnScreen(QScreen* requested) {
    QScreen* screen = requested;
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return;
    // Creating the native handle before assigning its screen avoids a visible
    // hop through the editor's display on macOS multi-projector rigs.
    (void)winId();
    if (windowHandle() != nullptr) windowHandle()->setScreen(screen);
    setGeometry(screen->geometry());
    showFullScreen();
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void StageOutputWindow::setFrame(const QImage& frame) {
    if (frozen_ || frame.isNull()) return;
    last_good_frame_ = frame;
    update();
}

void StageOutputWindow::setFrozen(bool frozen) {
    if (frozen_ == frozen) return;
    frozen_ = frozen;
    update();
}

void StageOutputWindow::setBlackout(bool blackout) {
    if (blackout_ == blackout) return;
    blackout_ = blackout;
    update();
}

void StageOutputWindow::setSmoothScaling(bool smooth) {
    if (smooth_scaling_ == smooth) return;
    smooth_scaling_ = smooth;
    update();
}

bool StageOutputWindow::isFrozen() const noexcept { return frozen_; }
bool StageOutputWindow::isBlackout() const noexcept { return blackout_; }
bool StageOutputWindow::hasGoodFrame() const noexcept {
    return !last_good_frame_.isNull();
}

void StageOutputWindow::clearFrame() {
    last_good_frame_ = {};
    update();
}

void StageOutputWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (blackout_ || last_good_frame_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth_scaling_);
    const QSize source = last_good_frame_.size();
    if (source.isEmpty()) return;
    const double scale = std::min(
        static_cast<double>(width()) / source.width(),
        static_cast<double>(height()) / source.height());
    const QSize target_size(
        std::max(1, static_cast<int>(std::lround(source.width() * scale))),
        std::max(1, static_cast<int>(std::lround(source.height() * scale))));
    const QRect target(QPoint((width() - target_size.width()) / 2,
                              (height() - target_size.height()) / 2),
                       target_size);
    painter.drawImage(target, last_good_frame_);
}

void StageOutputWindow::keyPressEvent(QKeyEvent* event) {
    if (event != nullptr && event->key() == Qt::Key_Escape) {
        emit escapeRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
