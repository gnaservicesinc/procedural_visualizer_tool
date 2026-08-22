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
    if (selected_swing_ && *selected_swing_ >= config_.swings.size()) {
        selected_swing_.reset();
    }
    if (selected_effect_ && *selected_effect_ >= config_.effects.size()) {
        selected_effect_.reset();
    }
    update();
}

void PreviewWidget::setOverlayMode(OverlayMode mode) {
    // Selecting a preview handle can navigate to the handle's workflow page.
    // That page refreshes the overlay even when it is already showing the
    // requested kind. Treat that refresh as idempotent so it cannot cancel the
    // drag between mousePressEvent()'s selection and drag-start signals.
    if (overlay_mode_ == mode) {
        update();
        return;
    }
    if (dragged_handle_) {
        emitDragFinished(*dragged_handle_);
        dragged_handle_.reset();
    }
    overlay_mode_ = mode;
    update();
}

void PreviewWidget::setSelectedWave(std::optional<std::size_t> index) {
    selected_wave_ = index;
    update();
}

void PreviewWidget::setSelectedSwing(std::optional<std::size_t> index) {
    selected_swing_ = index;
    update();
}

void PreviewWidget::setSelectedEffect(std::optional<std::size_t> index) {
    selected_effect_ = index;
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

bool PreviewWidget::sourceOverlayActive() const {
    if (!config_.surface.enabled
        || (config_.surface.mapping == pvt::SurfaceMapping::Plane
            && !config_.surface.plane_displacement.enabled)) {
        return false;
    }
    if (overlay_mode_ == OverlayMode::Swings) {
        return true;
    }
    return overlay_mode_ == OverlayMode::Effects
           && selected_effect_
           && *selected_effect_ < config_.effects.size()
           && config_.effects[*selected_effect_].space
                  == pvt::EffectSpace::Texture;
}

QRectF PreviewWidget::sourceOverlayRectangle() const {
    const QRectF output = imageRectangle();
    const double inset_width = std::max(140.0, output.width() * 0.34);
    const double aspect = static_cast<double>(std::max(1, config_.height))
                          / static_cast<double>(std::max(1, config_.width));
    const double inset_height = std::max(
        90.0, std::min(output.height() * 0.46, inset_width * aspect));
    return {output.right() - inset_width - 12.0,
            output.top() + 12.0, inset_width, inset_height};
}

QRectF PreviewWidget::handleRectangle(std::size_t index) const {
    const bool source_space = overlay_mode_ == OverlayMode::Swings
        || (overlay_mode_ == OverlayMode::Effects
            && index < config_.effects.size()
            && config_.effects[index].space == pvt::EffectSpace::Texture);
    return sourceOverlayActive() && source_space
               ? sourceOverlayRectangle() : imageRectangle();
}

QPointF PreviewWidget::handlePosition(std::size_t index) const {
    const QRectF target = handleRectangle(index);
    double x = 0.5;
    double y = 0.5;
    switch (overlay_mode_) {
        case OverlayMode::Waves:
            x = config_.waves[index].x_percent / 100.0;
            y = config_.waves[index].y_percent / 100.0;
            break;
        case OverlayMode::Swings:
            x = config_.swings[index].center_x;
            y = config_.swings[index].center_y;
            break;
        case OverlayMode::Effects:
            x = config_.effects[index].center_x;
            y = config_.effects[index].center_y;
            break;
    }
    return {target.left() + target.width() * x,
            target.top() + target.height() * y};
}

std::size_t PreviewWidget::handleCount() const {
    switch (overlay_mode_) {
        case OverlayMode::Waves: return config_.waves.size();
        case OverlayMode::Swings: return config_.swings.size();
        case OverlayMode::Effects: return config_.effects.size();
    }
    return 0U;
}

bool PreviewWidget::handleVisible(std::size_t index) const {
    if (index >= handleCount()) return false;
    return overlay_mode_ != OverlayMode::Effects
           || config_.effects[index].type != pvt::EffectType::BlockScale;
}

bool PreviewWidget::handleEnabled(std::size_t index) const {
    switch (overlay_mode_) {
        case OverlayMode::Waves: return config_.waves[index].enabled;
        case OverlayMode::Swings: return config_.swings[index].enabled;
        case OverlayMode::Effects: return config_.effects[index].enabled;
    }
    return false;
}

double PreviewWidget::handleRadiusFraction(std::size_t index) const {
    switch (overlay_mode_) {
        case OverlayMode::Waves: return 0.0;
        case OverlayMode::Swings: return config_.swings[index].radius;
        case OverlayMode::Effects: return config_.effects[index].area_radius;
    }
    return 0.0;
}

std::optional<std::size_t> PreviewWidget::selectedHandle() const {
    switch (overlay_mode_) {
        case OverlayMode::Waves: return selected_wave_;
        case OverlayMode::Swings: return selected_swing_;
        case OverlayMode::Effects: return selected_effect_;
    }
    return std::nullopt;
}

std::optional<std::size_t> PreviewWidget::hitHandle(const QPointF& position) const {
    std::optional<std::size_t> closest;
    double closest_distance = kHandleRadius * 2.0;
    for (std::size_t index = 0; index < handleCount(); ++index) {
        if (!handleVisible(index)) continue;
        const QPointF delta = position - handlePosition(index);
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

    if (sourceOverlayActive()) {
        const QRectF uv = sourceOverlayRectangle();
        painter.fillRect(uv, QColor(18, 20, 25, 232));
        painter.setPen(QPen(QColor(92, 98, 112), 1.0, Qt::DashLine));
        for (int division = 1; division < 4; ++division) {
            const double fraction = static_cast<double>(division) / 4.0;
            painter.drawLine(QPointF(uv.left() + uv.width() * fraction, uv.top()),
                             QPointF(uv.left() + uv.width() * fraction, uv.bottom()));
            painter.drawLine(QPointF(uv.left(), uv.top() + uv.height() * fraction),
                             QPointF(uv.right(), uv.top() + uv.height() * fraction));
        }
        painter.setPen(QPen(QColor(110, 215, 235), 2.0));
        painter.drawRect(uv);
        const QRectF label(uv.left() + 5.0, uv.top() + 3.0,
                           uv.width() - 10.0, 20.0);
        painter.fillRect(label, QColor(10, 12, 15, 190));
        painter.setPen(QColor(225, 247, 252));
        painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                         tr("UNWRAPPED SOURCE / UV"));
    }

    QString overlay_hint;
    switch (overlay_mode_) {
        case OverlayMode::Waves:
            overlay_hint = tr("Source coordinate (before surface and layer transform)");
            break;
        case OverlayMode::Swings:
            overlay_hint = sourceOverlayActive()
                ? tr("Edit source/UV position in the inset; output remains projected")
                : tr("Source/UV coordinate");
            break;
        case OverlayMode::Effects:
            if (selected_effect_ && *selected_effect_ < config_.effects.size()) {
                const auto& effect = config_.effects[*selected_effect_];
                if (effect.type == pvt::EffectType::BlockScale) {
                    overlay_hint = tr("Block Scale affects the whole image; it has no center");
                } else if (effect.space == pvt::EffectSpace::Texture) {
                    overlay_hint = sourceOverlayActive()
                        ? tr("Edit Texture UV position in the inset; output remains projected")
                        : tr("UV = Texture source coordinate");
                } else {
                    overlay_hint = tr("M = Mapped-object coordinate; dashed radius is final canvas space");
                }
            } else {
                overlay_hint = tr("UV = unprojected Texture source; M = Mapped-object canvas space");
            }
            break;
    }
    if (!overlay_hint.isEmpty() && target.width() > 40.0
        && target.height() > 40.0) {
        const int maximum_width = std::max(
            1, static_cast<int>(target.width()) - 24);
        const QString shown = painter.fontMetrics().elidedText(
            overlay_hint, Qt::ElideRight, maximum_width);
        const QRectF legend(target.left() + 6.0, target.bottom() - 28.0,
                            target.width() - 12.0, 22.0);
        painter.fillRect(legend, QColor(15, 16, 19, 205));
        painter.setPen(QColor(235, 237, 242));
        painter.drawText(legend.adjusted(6.0, 0.0, -6.0, 0.0),
                         Qt::AlignLeft | Qt::AlignVCenter, shown);
    }

    const auto selected = selectedHandle();
    for (std::size_t index = 0; index < handleCount(); ++index) {
        if (!handleVisible(index)) continue;
        const QPointF point = handlePosition(index);
        QColor color = kWaveColors[index % kWaveColors.size()];
        if (!handleEnabled(index)) {
            color.setAlpha(100);
        }
        const double radius_fraction = handleRadiusFraction(index);
        if (radius_fraction > 0.0) {
            const QRectF handle_target = handleRectangle(index);
            const double radius = radius_fraction
                                  * std::min(handle_target.width(),
                                             handle_target.height());
            QColor ring = color;
            ring.setAlpha(handleEnabled(index) ? 185 : 80);
            const bool source_space =
                overlay_mode_ == OverlayMode::Swings
                || (overlay_mode_ == OverlayMode::Effects
                    && config_.effects[index].space
                           == pvt::EffectSpace::Texture);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(ring, selected && *selected == index ? 2.5 : 1.5,
                                source_space ? Qt::DotLine : Qt::DashLine));
            painter.drawEllipse(point, radius, radius);
        }
        painter.setBrush(color);
        painter.setPen(QPen(selected && *selected == index ? Qt::white
                                                           : QColor(15, 16, 19),
                            selected && *selected == index ? 3.0 : 1.5));
        painter.drawEllipse(point, kHandleRadius, kHandleRadius);
        painter.setPen(Qt::black);
        painter.drawText(QRectF(point.x() - kHandleRadius, point.y() - kHandleRadius,
                                kHandleRadius * 2.0, kHandleRadius * 2.0),
                         Qt::AlignCenter, QString::number(index + 1));
        if (overlay_mode_ == OverlayMode::Effects) {
            painter.setPen(color);
            painter.drawText(
                QRectF(point.x() + kHandleRadius + 2.0,
                       point.y() - kHandleRadius, 24.0,
                       kHandleRadius * 2.0),
                Qt::AlignLeft | Qt::AlignVCenter,
                config_.effects[index].space == pvt::EffectSpace::Texture
                    ? tr("UV") : tr("M"));
        }
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
    dragged_handle_ = hitHandle(event->position());
    if (dragged_handle_) {
        dragged_mode_ = overlay_mode_;
        switch (overlay_mode_) {
            case OverlayMode::Waves: selected_wave_ = dragged_handle_; break;
            case OverlayMode::Swings: selected_swing_ = dragged_handle_; break;
            case OverlayMode::Effects: selected_effect_ = dragged_handle_; break;
        }
        setCursor(Qt::ClosedHandCursor);
        emitSelected(*dragged_handle_);
        emitDragStarted(*dragged_handle_);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* event) {
    if (dragged_handle_) {
        moveHandle(event->position());
        event->accept();
        return;
    }
    setCursor(hitHandle(event->position()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && dragged_handle_) {
        const std::size_t finished_index = *dragged_handle_;
        dragged_handle_.reset();
        setCursor(Qt::OpenHandCursor);
        emitDragFinished(finished_index);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void PreviewWidget::moveHandle(const QPointF& position) {
    if (!dragged_handle_ || dragged_mode_ != overlay_mode_
        || *dragged_handle_ >= handleCount()) {
        return;
    }
    const QRectF target = handleRectangle(*dragged_handle_);
    if (target.width() <= 0.0 || target.height() <= 0.0) {
        return;
    }
    const double maximum = pvt::maximum_render_parameter_magnitude();
    const double normalized_x = std::clamp(
        (position.x() - target.left()) / target.width(), -maximum, maximum);
    const double normalized_y = std::clamp(
        (position.y() - target.top()) / target.height(), -maximum, maximum);
    switch (overlay_mode_) {
        case OverlayMode::Waves: {
            const double x = std::clamp(
                normalized_x * 100.0, -maximum, maximum);
            const double y = std::clamp(
                normalized_y * 100.0, -maximum, maximum);
            config_.waves[*dragged_handle_].x_percent = x;
            config_.waves[*dragged_handle_].y_percent = y;
            emit waveMoved(*dragged_handle_, x, y);
            break;
        }
        case OverlayMode::Swings:
            config_.swings[*dragged_handle_].center_x = normalized_x;
            config_.swings[*dragged_handle_].center_y = normalized_y;
            emit swingMoved(*dragged_handle_, normalized_x, normalized_y);
            break;
        case OverlayMode::Effects:
            config_.effects[*dragged_handle_].center_x = normalized_x;
            config_.effects[*dragged_handle_].center_y = normalized_y;
            emit effectMoved(*dragged_handle_, normalized_x, normalized_y);
            break;
    }
    update();
}

void PreviewWidget::emitDragStarted(std::size_t index) {
    switch (overlay_mode_) {
        case OverlayMode::Waves: emit waveDragStarted(index); break;
        case OverlayMode::Swings: emit swingDragStarted(index); break;
        case OverlayMode::Effects: emit effectDragStarted(index); break;
    }
}

void PreviewWidget::emitDragFinished(std::size_t index) {
    switch (dragged_mode_) {
        case OverlayMode::Waves: emit waveDragFinished(index); break;
        case OverlayMode::Swings: emit swingDragFinished(index); break;
        case OverlayMode::Effects: emit effectDragFinished(index); break;
    }
}

void PreviewWidget::emitSelected(std::size_t index) {
    switch (overlay_mode_) {
        case OverlayMode::Waves: emit waveSelected(index); break;
        case OverlayMode::Swings: emit swingSelected(index); break;
        case OverlayMode::Effects: emit effectSelected(index); break;
    }
}
