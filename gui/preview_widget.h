#ifndef PVT_PREVIEW_WIDGET_H
#define PVT_PREVIEW_WIDGET_H

#include "procedural_visualizer_tool.h"

#include <QImage>
#include <QWidget>

#include <optional>

class PreviewWidget final : public QWidget {
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setPreview(const QImage& image);
    void setConfiguration(const pvt::RenderConfig& config);
    void setSelectedWave(std::optional<std::size_t> index);

signals:
    void waveMoved(std::size_t index, double xPercent, double yPercent);
    void waveSelected(std::size_t index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    QRectF imageRectangle() const;
    QPointF wavePosition(std::size_t index) const;
    std::optional<std::size_t> hitWave(const QPointF& position) const;
    void moveWave(const QPointF& position);

    QImage preview_;
    pvt::RenderConfig config_;
    std::optional<std::size_t> selected_wave_;
    std::optional<std::size_t> dragged_wave_;
};

#endif

