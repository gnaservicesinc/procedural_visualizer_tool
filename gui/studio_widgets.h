#ifndef PVT_STUDIO_WIDGETS_H
#define PVT_STUDIO_WIDGETS_H

#include <QDial>
#include <QColor>
#include <QPointF>
#include <QVector>
#include <QWidget>

class QPaintEvent;
class QSize;

// Compact custom controls inspired by the supplied mixer/dial references.
// They scale from device-independent geometry, so no low-resolution knob
// bitmap is stretched on Retina/UHD displays.
class StudioKnob final : public QDial {
    Q_OBJECT

public:
    explicit StudioKnob(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setUnit(const QString& unit);
    void setDisplayDivisor(double divisor);
    void setAccentColor(const QColor& color);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString unit_;
    double display_divisor_ = 1.0;
    QColor accent_;
};

class LiveLevelMeter final : public QWidget {
    Q_OBJECT

public:
    explicit LiveLevelMeter(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setLevel(double level);
    void setCaption(const QString& caption);
    void setPeakWarning(bool warning);
    void setDecibelScale(bool enabled);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double level_ = 0.0;
    QString caption_;
    bool peak_warning_ = false;
    bool decibel_scale_ = false;
};

class LiveSpectrumMeter final : public QWidget {
    Q_OBJECT

public:
    explicit LiveSpectrumMeter(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    // Each point stores frequency in x and normalized post-processing level in y.
    void setBands(const QVector<QPointF>& bands);
    void setGateOpen(bool open);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<QPointF> bands_;
    bool gate_open_ = true;
};

class StatusLamp final : public QWidget {
    Q_OBJECT

public:
    enum class State { Off, Ready, Warning, Fault };
    Q_ENUM(State)

    explicit StatusLamp(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    void setState(State state);
    void setText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    State state_ = State::Off;
    QString text_;
};

#endif
