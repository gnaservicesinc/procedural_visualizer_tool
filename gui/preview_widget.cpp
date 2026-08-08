#include "preview_widget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double kHandleRadius = 8.0;

const std::array<QColor, 8> kWaveColors = {
    QColor(77, 208, 225), QColor(255, 183, 77), QColor(186, 104, 200),
    QColor(129, 199, 132), QColor(239, 83, 80), QColor(100, 181, 246),
    QColor(255, 241, 118), QColor(240, 98, 146)};

} // namespace

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(480, 300);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewWidget::setPreview(const QImage& image) {
    preview_ = image;
    update();
}

void PreviewWidget::setConfiguration(const pvt::RenderConfig& config) {
    config_ = config;
    if (selected_wave_ && *selected_wave_ >= config_.waves.size()) {
        selected_wave_.reset();
    }
    update();
}

void PreviewWidget::setSelectedWave(std::optional<std::size_t> index) {
    selected_wave_ = index;
    update();
}

QRectF PreviewWidget::imageRectangle() const {
    // Configuration is authoritative while a replacement preview is rendering.
    // Using the previous image's aspect ratio makes handles jump after a canvas edit.
    const QSize source(std::max(1, config_.width), std::max(1, config_.height));
    QSizeF fitted = source;
    fitted.scale(size(), Qt::KeepAspectRatio);
    return {(width() - fitted.width()) * 0.5, (height() - fitted.height()) * 0.5,
            fitted.width(), fitted.height()};
}

QPointF PreviewWidget::wavePosition(std::size_t index) const {
    const QRectF target = imageRectangle();
    const auto& wave = config_.waves[index];
    return {target.left() + target.width() * wave.x_percent / 100.0,
            target.top() + target.height() * wave.y_percent / 100.0};
}

std::optional<std::size_t> PreviewWidget::hitWave(const QPointF& position) const {
    std::optional<std::size_t> closest;
    double closest_distance = kHandleRadius * 2.0;
    for (std::size_t index = 0; index < config_.waves.size(); ++index) {
        const QPointF delta = position - wavePosition(index);
        const double distance = std::hypot(delta.x(), delta.y());
        if (distance <= closest_distance) {
            closest = index;
            closest_distance = distance;
        }
    }
    return closest;
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(25, 27, 32));

    const QRectF target = imageRectangle();
    painter.save();
    painter.setClipRect(target);
    constexpr int checker_size = 12;
    for (int y = static_cast<int>(target.top()); y < static_cast<int>(target.bottom());
         y += checker_size) {
        for (int x = static_cast<int>(target.left()); x < static_cast<int>(target.right());
             x += checker_size) {
            const bool light = (((x - static_cast<int>(target.left())) / checker_size)
                                + ((y - static_cast<int>(target.top())) / checker_size))
                               % 2
                               == 0;
            painter.fillRect(QRect(x, y, checker_size, checker_size),
                             light ? QColor(82, 84, 89) : QColor(54, 56, 61));
        }
    }
    painter.restore();
    if (!preview_.isNull()) {
        painter.drawImage(target, preview_);
    }
    painter.setPen(QPen(QColor(160, 164, 173), 1.0));
    painter.drawRect(target);

    for (std::size_t index = 0; index < config_.waves.size(); ++index) {
        const auto& wave = config_.waves[index];
        const QPointF point = wavePosition(index);
        QColor color = kWaveColors[index % kWaveColors.size()];
        if (!wave.enabled) {
            color.setAlpha(100);
        }
        painter.setBrush(color);
        painter.setPen(QPen(selected_wave_ && *selected_wave_ == index ? Qt::white
                                                                       : QColor(15, 16, 19),
                            selected_wave_ && *selected_wave_ == index ? 3.0 : 1.5));
        painter.drawEllipse(point, kHandleRadius, kHandleRadius);
        painter.setPen(Qt::black);
        painter.drawText(QRectF(point.x() - kHandleRadius, point.y() - kHandleRadius,
                                kHandleRadius * 2.0, kHandleRadius * 2.0),
                         Qt::AlignCenter, QString::number(index + 1));
    }

    if (preview_.isNull()) {
        painter.setPen(QColor(220, 222, 227));
        painter.drawText(target, Qt::AlignCenter, tr("Rendering preview…"));
    }
}

void PreviewWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    dragged_wave_ = hitWave(event->position());
    if (dragged_wave_) {
        selected_wave_ = dragged_wave_;
        setCursor(Qt::ClosedHandCursor);
        emit waveSelected(*dragged_wave_);
        emit waveDragStarted(*dragged_wave_);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragged_wave_) {
        moveWave(event->position());
        event->accept();
        return;
    }
    setCursor(hitWave(event->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragged_wave_) {
        const std::size_t finished_index = *dragged_wave_;
        dragged_wave_.reset();
        setCursor(Qt::OpenHandCursor);
        emit waveDragFinished(finished_index);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PreviewWidget::moveWave(const QPointF& position) {
    if (!dragged_wave_ || *dragged_wave_ >= config_.waves.size()) {
        return;
    }
    const QRectF target = imageRectangle();
    if (target.width() <= 0.0 || target.height() <= 0.0) {
        return;
    }
    const double x = std::clamp((position.x() - target.left()) / target.width() * 100.0,
                                -100.0, 200.0);
    const double y = std::clamp((position.y() - target.top()) / target.height() * 100.0,
                                -100.0, 200.0);
    config_.waves[*dragged_wave_].x_percent = x;
    config_.waves[*dragged_wave_].y_percent = y;
    emit waveMoved(*dragged_wave_, x, y);
    update();
}
