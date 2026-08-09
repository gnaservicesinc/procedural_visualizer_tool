#ifndef PVT_PREVIEW_WIDGET_H
#define PVT_PREVIEW_WIDGET_H

#include "procedural_visualizer_tool.h"

#include <QImage>
#include <QWidget>

#include <optional>

class PreviewWidget final : public QWidget {
    Q_OBJECT

public:
    enum class OverlayMode {
        Waves,
        Swings,
        Effects
    };

    explicit PreviewWidget(QWidget* parent = nullptr);

    void setPreview(const QImage& image);
    void setConfiguration(const pvt::RenderConfig& config);
    void setOverlayMode(OverlayMode mode);
    void setSelectedWave(std::optional<std::size_t> index);
    void setSelectedSwing(std::optional<std::size_t> index);
    void setSelectedEffect(std::optional<std::size_t> index);

signals:
    void waveDragStarted(std::size_t index);
    void waveMoved(std::size_t index, double xPercent, double yPercent);
    void waveDragFinished(std::size_t index);
    void waveSelected(std::size_t index);
    void swingDragStarted(std::size_t index);
    void swingMoved(std::size_t index, double centerX, double centerY);
    void swingDragFinished(std::size_t index);
    void swingSelected(std::size_t index);
    void effectDragStarted(std::size_t index);
    void effectMoved(std::size_t index, double centerX, double centerY);
    void effectDragFinished(std::size_t index);
    void effectSelected(std::size_t index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF imageRectangle() const;
    QPointF handlePosition(std::size_t index) const;
    std::optional<std::size_t> hitHandle(const QPointF& position) const;
    std::size_t handleCount() const;
    bool handleVisible(std::size_t index) const;
    bool handleEnabled(std::size_t index) const;
    double handleRadiusFraction(std::size_t index) const;
    std::optional<std::size_t> selectedHandle() const;
    void moveHandle(const QPointF& position);
    void emitDragStarted(std::size_t index);
    void emitDragFinished(std::size_t index);
    void emitSelected(std::size_t index);

    QImage preview_;
    pvt::RenderConfig config_;
    OverlayMode overlay_mode_ = OverlayMode::Waves;
    std::optional<std::size_t> selected_wave_;
    std::optional<std::size_t> selected_swing_;
    std::optional<std::size_t> selected_effect_;
    std::optional<std::size_t> dragged_handle_;
    OverlayMode dragged_mode_ = OverlayMode::Waves;
};

#endif
