#include "stage_output_window.h"

#include <QCloseEvent>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>

#include <algorithm>
#include <cmath>

StageOutputWindow::StageOutputWindow(QWidget* parent)
    : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("stageOutputWindow"));
    setAccessibleName(tr("Live stage output"));
    setWindowTitle(tr("PVT — Video Output"));
    setMinimumSize(320, 200);
    menu_bar_ = new QMenuBar(this);
    menu_bar_->setNativeMenuBar(false);
    auto* output_menu = menu_bar_->addMenu(tr("Output"));
    auto* fullscreen = output_menu->addAction(tr("Enter / Leave Full Screen"));
    fullscreen->setShortcut(QKeySequence(Qt::Key_F11));
    addAction(fullscreen);
    connect(fullscreen, &QAction::triggered, this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });
    auto* playback = output_menu->addAction(tr("Play / Pause"));
    connect(playback, &QAction::triggered, this, &StageOutputWindow::playbackRequested);
    auto* close = output_menu->addAction(tr("Stop Video Output"));
    connect(close, &QAction::triggered, this, &StageOutputWindow::dismissRequested);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setCursor(Qt::ArrowCursor);
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

void StageOutputWindow::showWindowedOnScreen(QScreen* requested,
                                             const QRect& geometry) {
    QScreen* screen = requested;
    if (screen == nullptr) screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return;
    (void)winId();
    if (windowHandle() != nullptr) windowHandle()->setScreen(screen);
    showNormal();
    setGeometry(geometry);
    raise();
    activateWindow();
    setFocus(Qt::OtherFocusReason);
}

void StageOutputWindow::dismiss() {
    // On macOS, showFullScreen() gives the application a dedicated desktop.
    // Leave that native full-screen desktop while the window is still visible;
    // changing only the latent state after hide() can strand a black Space on
    // the output display even though Qt reports this widget as hidden.
    if (isFullScreen()) showNormal();
    hide();
}

void StageOutputWindow::setFrame(const QImage& frame) {
    if (frozen_ || frame.isNull()) return;
    last_good_frame_ = frame;
    emitPresentedImage();
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
    emitPresentedImage();
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

QSize StageOutputWindow::outputPixelSize() const {
    const double ratio = std::max(1.0, devicePixelRatioF());
    return QSize(std::max(1, static_cast<int>(std::lround(width() * ratio))),
                 std::max(1, static_cast<int>(std::lround(height() * ratio))));
}

void StageOutputWindow::clearFrame() {
    last_good_frame_ = {};
    emitPresentedImage();
    update();
}

bool StageOutputWindow::event(QEvent* event) {
    const bool metrics_changed = event != nullptr
        && (event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
            || event->type() == QEvent::DevicePixelRatioChange
#endif
        );
    const bool handled = QWidget::event(event);
    if (menu_bar_ && event && event->type() == QEvent::WindowStateChange) {
        menu_bar_->setVisible(!isFullScreen());
    }
    if (metrics_changed) emit outputMetricsChanged();
    return handled;
}

void StageOutputWindow::closeEvent(QCloseEvent* event) {
    if (event != nullptr) event->ignore();
    emit dismissRequested();
}

void StageOutputWindow::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    if (blackout_ || last_good_frame_.isNull()) return;
    painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth_scaling_);
    const QSize source = last_good_frame_.size();
    if (source.isEmpty()) return;
    const int top = menu_bar_->isVisible() ? menu_bar_->height() : 0;
    const int available_height = height() - top;
    const double scale = std::min(
        static_cast<double>(width()) / source.width(),
        static_cast<double>(available_height) / source.height());
    const QSize target_size(
        std::max(1, static_cast<int>(std::lround(source.width() * scale))),
        std::max(1, static_cast<int>(std::lround(source.height() * scale))));
    const QRect target(QPoint((width() - target_size.width()) / 2,
                              top + (available_height - target_size.height()) / 2),
                       target_size);
    painter.drawImage(target, last_good_frame_);
}

void StageOutputWindow::keyPressEvent(QKeyEvent* event) {
    if (event != nullptr && event->key() == Qt::Key_Escape) {
        if (isFullScreen()) showNormal(); else emit dismissRequested();
        event->accept();
        return;
    }
    if (event && event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        if (!event->isAutoRepeat()) emit playbackRequested();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void StageOutputWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (menu_bar_) menu_bar_->setGeometry(0, 0, width(), menu_bar_->sizeHint().height());
    emit outputMetricsChanged();
}

void StageOutputWindow::emitPresentedImage() {
    if (blackout_ || last_good_frame_.isNull()) {
        QImage black(last_good_frame_.isNull() ? QSize(640, 360) : last_good_frame_.size(), QImage::Format_RGB32);
        black.fill(Qt::black);
        emit imagePresented(black);
    } else emit imagePresented(last_good_frame_);
}
