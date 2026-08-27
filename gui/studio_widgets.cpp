#include "studio_widgets.h"

#include <QColor>
#include <QConicalGradient>
#include <QFontDatabase>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QRadialGradient>
#include <QStyleOptionSlider>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

QColor studio_accent() { return QColor(QStringLiteral("#59d4c8")); }

} // namespace

StudioKnob::StudioKnob(QWidget* parent)
    : QDial(parent), accent_(studio_accent()) {
    setNotchesVisible(false);
    setWrapping(false);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(76, 92);
}

QSize StudioKnob::sizeHint() const { return {96, 112}; }
void StudioKnob::setUnit(const QString& unit) { unit_ = unit; update(); }
void StudioKnob::setDisplayDivisor(double divisor) {
    display_divisor_ = std::isfinite(divisor) && divisor != 0.0 ? divisor : 1.0;
    update();
}
void StudioKnob::setAccentColor(const QColor& color) {
    if (color.isValid()) accent_ = color;
    update();
}

void StudioKnob::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const int readout_height = std::max(22, fontMetrics().height() + 7);
    const QRectF dial_area = QRectF(rect()).adjusted(7, 5, -7, -readout_height - 4);
    const double diameter = std::min(dial_area.width(), dial_area.height());
    const QPointF center(dial_area.center().x(), dial_area.center().y());
    const QRectF knob(center.x() - diameter * 0.36,
                      center.y() - diameter * 0.36,
                      diameter * 0.72, diameter * 0.72);
    const double amount = maximum() == minimum()
        ? 0.0
        : static_cast<double>(value() - minimum())
              / static_cast<double>(maximum() - minimum());
    constexpr double start_degrees = 225.0;
    constexpr double sweep_degrees = 270.0;

    QPen track_pen(QColor(QStringLiteral("#13171a")),
                   std::max(5.0, diameter * 0.075), Qt::SolidLine,
                   Qt::RoundCap);
    painter.setPen(track_pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(dial_area.adjusted(diameter * 0.09, diameter * 0.09,
                                       -diameter * 0.09, -diameter * 0.09),
                    static_cast<int>((90.0 - start_degrees) * 16.0),
                    static_cast<int>(-sweep_degrees * 16.0));
    QPen value_pen(accent_, track_pen.widthF(), Qt::SolidLine, Qt::RoundCap);
    painter.setPen(value_pen);
    painter.drawArc(dial_area.adjusted(diameter * 0.09, diameter * 0.09,
                                       -diameter * 0.09, -diameter * 0.09),
                    static_cast<int>((90.0 - start_degrees) * 16.0),
                    static_cast<int>(-sweep_degrees * amount * 16.0));

    QRadialGradient metal(knob.center(), knob.width() * 0.6,
                          knob.center() - QPointF(knob.width() * 0.18,
                                                  knob.height() * 0.22));
    metal.setColorAt(0.0, QColor(QStringLiteral("#596168")));
    metal.setColorAt(0.46, QColor(QStringLiteral("#343a3f")));
    metal.setColorAt(1.0, QColor(QStringLiteral("#14181b")));
    painter.setPen(QPen(QColor(QStringLiteral("#080a0c")), 2.0));
    painter.setBrush(metal);
    painter.drawEllipse(knob);
    painter.setPen(QPen(QColor(255, 255, 255, 34), 1.0));
    painter.drawEllipse(knob.adjusted(3, 3, -3, -3));

    const double angle = (start_degrees + sweep_degrees * amount) * kPi / 180.0;
    const QPointF indicator_end(
        center.x() + std::cos(angle) * knob.width() * 0.28,
        center.y() + std::sin(angle) * knob.height() * 0.28);
    painter.setPen(QPen(accent_.lighter(125), std::max(2.0, diameter * 0.035),
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center, indicator_end);

    QRect readout(5, height() - readout_height, width() - 10,
                  readout_height - 2);
    painter.setPen(QPen(QColor(QStringLiteral("#0a0d0e")), 1.0));
    painter.setBrush(QColor(QStringLiteral("#101719")));
    painter.drawRoundedRect(readout, 4, 4);
    QFont display_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    display_font.setPixelSize(std::max(10, readout.height() - 8));
    display_font.setWeight(QFont::DemiBold);
    painter.setFont(display_font);
    painter.setPen(accent_.lighter(118));
    const double display = static_cast<double>(value()) / display_divisor_;
    const QString text = std::fabs(display - std::round(display)) < 1.0e-6
        ? QString::number(static_cast<qlonglong>(std::llround(display))) + unit_
        : QString::number(display, 'f', 1) + unit_;
    painter.drawText(readout.adjusted(3, 0, -3, 0), Qt::AlignCenter, text);

    if (hasFocus()) {
        painter.setPen(QPen(palette().highlight().color(), 1.0, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 5, 5);
    }
}

LiveLevelMeter::LiveLevelMeter(QWidget* parent) : QWidget(parent) {
    setMinimumSize(116, 92);
    setAccessibleName(tr("Live audio level"));
}

QSize LiveLevelMeter::sizeHint() const { return {150, 106}; }
void LiveLevelMeter::setLevel(double requested) {
    if (!std::isfinite(requested)) return;
    const double bounded = std::clamp(requested, 0.0, 1.0);
    if (std::fabs(bounded - level_) < 0.002) return;
    level_ = bounded;
    update();
}
void LiveLevelMeter::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}
void LiveLevelMeter::setPeakWarning(bool warning) {
    if (peak_warning_ == warning) return;
    peak_warning_ = warning;
    update();
}

void LiveLevelMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(5, 5, -5, -5);
    QLinearGradient panel(face.topLeft(), face.bottomLeft());
    panel.setColorAt(0.0, QColor(QStringLiteral("#33393e")));
    panel.setColorAt(1.0, QColor(QStringLiteral("#171b1e")));
    painter.setPen(QPen(QColor(QStringLiteral("#080a0b")), 1.5));
    painter.setBrush(panel);
    painter.drawRoundedRect(face, 8, 8);

    const int segments = 18;
    const QRectF channel = face.adjusted(14, 14, -14, -36);
    const double gap = 2.0;
    const double segment_width = (channel.width() - gap * (segments - 1))
                                 / segments;
    const int active = static_cast<int>(std::lround(level_ * segments));
    for (int index = 0; index < segments; ++index) {
        const QRectF segment(channel.left() + index * (segment_width + gap),
                             channel.top(), segment_width, channel.height());
        QColor color;
        if (index < 12) color = QColor(QStringLiteral("#50d6bd"));
        else if (index < 16) color = QColor(QStringLiteral("#e3bc58"));
        else color = QColor(QStringLiteral("#ed6a5f"));
        if (index >= active) color = QColor(38, 45, 47);
        if (peak_warning_ && index >= 16 && index < active) color = color.lighter(135);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(segment, 1.5, 1.5);
    }
    QFont display = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    display.setWeight(QFont::DemiBold);
    display.setPixelSize(std::max(10, fontMetrics().height()));
    painter.setFont(display);
    painter.setPen(QColor(QStringLiteral("#9cece0")));
    const QString value = QStringLiteral("%1  %2")
        .arg(QString::number(level_ * 100.0, 'f', 0) + QLatin1Char('%'),
             caption_);
    painter.drawText(face.adjusted(8, face.height() - 30, -8, -4),
                     Qt::AlignCenter, value);
}

LiveSpectrumMeter::LiveSpectrumMeter(QWidget* parent) : QWidget(parent) {
    setMinimumSize(280, 126);
    setAccessibleName(tr("Live post-EQ spectrum"));
    setToolTip(tr(
        "Real-time frequency energy after the input filters, EQ, gain, and noise gate—the same signal used by Live analysis."));
}

QSize LiveSpectrumMeter::sizeHint() const { return {520, 150}; }

void LiveSpectrumMeter::setBands(const QVector<QPointF>& bands) {
    bands_ = bands;
    update();
}

void LiveSpectrumMeter::setGateOpen(bool open) {
    if (gate_open_ == open) return;
    gate_open_ = open;
    update();
}

void LiveSpectrumMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(5, 5, -5, -5);
    QLinearGradient panel(face.topLeft(), face.bottomLeft());
    panel.setColorAt(0.0, QColor(QStringLiteral("#292f34")));
    panel.setColorAt(1.0, QColor(QStringLiteral("#111518")));
    painter.setPen(QPen(QColor(QStringLiteral("#080a0b")), 1.5));
    painter.setBrush(panel);
    painter.drawRoundedRect(face, 8, 8);

    const QRectF graph = face.adjusted(12, 22, -12, -28);
    painter.setPen(QPen(QColor(255, 255, 255, 25), 1.0));
    for (int line = 1; line < 4; ++line) {
        const double y = graph.bottom() - graph.height() * line / 4.0;
        painter.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
    }
    if (!bands_.isEmpty()) {
        const double gap = std::max(2.0, graph.width() * 0.008);
        const double band_count = static_cast<double>(bands_.size());
        const double width = std::max(
            1.0, (graph.width() - gap * (band_count - 1.0)) / band_count);
        for (int index = 0; index < bands_.size(); ++index) {
            const double level = std::clamp(bands_[index].y(), 0.0, 1.0);
            const QRectF bar(graph.left() + index * (width + gap),
                             graph.bottom() - graph.height() * level,
                             width, graph.height() * level);
            QLinearGradient energy(bar.bottomLeft(), bar.topLeft());
            energy.setColorAt(0.0, QColor(QStringLiteral("#43bfae")));
            energy.setColorAt(0.72, QColor(QStringLiteral("#78d9ce")));
            energy.setColorAt(1.0, QColor(QStringLiteral("#e6bd5c")));
            painter.setPen(Qt::NoPen);
            painter.setBrush(energy);
            painter.drawRoundedRect(bar, 2.0, 2.0);

            const double hz = bands_[index].x();
            const QString label = hz >= 1000.0
                ? QStringLiteral("%1k").arg(hz / 1000.0, 0, 'g', 2)
                : QString::number(hz, 'g', 3);
            painter.setPen(QColor(QStringLiteral("#98a6ae")));
            painter.drawText(QRectF(bar.left() - gap * 0.5, graph.bottom() + 3.0,
                                    width + gap, 18.0),
                             Qt::AlignHCenter | Qt::AlignTop, label);
        }
    }
    painter.setPen(gate_open_ ? QColor(QStringLiteral("#9cece0"))
                              : QColor(QStringLiteral("#e3bc58")));
    painter.drawText(face.adjusted(10, 3, -10, -3),
                     Qt::AlignLeft | Qt::AlignTop,
                     gate_open_ ? tr("POST EQ · GATE OPEN")
                                : tr("POST EQ · GATE CLOSED"));
}

StatusLamp::StatusLamp(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(24);
}
QSize StatusLamp::sizeHint() const {
    return {std::max(86, fontMetrics().horizontalAdvance(text_) + 34), 26};
}
void StatusLamp::setState(State state) {
    if (state_ == state) return;
    state_ = state;
    update();
}
void StatusLamp::setText(const QString& text) {
    if (text_ == text) return;
    text_ = text;
    updateGeometry();
    update();
}
void StatusLamp::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor light;
    switch (state_) {
        case State::Off: light = QColor(QStringLiteral("#364044")); break;
        case State::Ready: light = QColor(QStringLiteral("#50d6bd")); break;
        case State::Warning: light = QColor(QStringLiteral("#e3bc58")); break;
        case State::Fault: light = QColor(QStringLiteral("#ed6a5f")); break;
    }
    const QRectF lamp(3, (height() - 12) / 2.0, 12, 12);
    QRadialGradient glow(lamp.center(), 10);
    glow.setColorAt(0.0, light.lighter(175));
    glow.setColorAt(0.5, light);
    glow.setColorAt(1.0, QColor(light.red(), light.green(), light.blue(), 0));
    painter.setPen(QPen(light.darker(175), 1.0));
    painter.setBrush(glow);
    painter.drawEllipse(lamp);
    painter.setPen(palette().text().color());
    painter.drawText(QRect(22, 0, width() - 22, height()),
                     Qt::AlignLeft | Qt::AlignVCenter, text_);
}
