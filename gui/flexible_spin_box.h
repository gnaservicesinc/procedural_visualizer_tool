#ifndef PVT_FLEXIBLE_SPIN_BOX_H
#define PVT_FLEXIBLE_SPIN_BOX_H

#include <QDoubleSpinBox>
#include <QSpinBox>

// A decimal editor that also accepts common artist-friendly fractions such as
// 1/2, 2 1/2, 2+1/2, and Unicode vulgar fractions. The stored value remains a
// double; fractions are only an input convenience and never leak into project
// serialization.
class FlexibleDoubleSpinBox : public QDoubleSpinBox {
public:
    explicit FlexibleDoubleSpinBox(QWidget* parent = nullptr);

    static bool parseFlexibleNumber(const QString& text, const QLocale& locale,
                                    double& value);

protected:
    QValidator::State validate(QString& input, int& position) const override;
    double valueFromText(const QString& text) const override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    QString editing_special_text_;
    bool editing_special_value_ = false;
};

class ResolvedAutoSpinBox final : public QSpinBox {
public:
    explicit ResolvedAutoSpinBox(QWidget* parent = nullptr);

    void setResolvedAutoValue(int value,
                              const QString& label = QStringLiteral("Auto"));

protected:
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    int resolved_auto_value_ = 0;
    QString auto_text_;
    bool editing_auto_ = false;
};

#endif
