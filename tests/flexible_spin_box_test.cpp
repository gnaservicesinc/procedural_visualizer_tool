#include "flexible_spin_box.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; ++failures; }
}
class DecimalEditor : public FlexibleDoubleSpinBox {
public:
    using FlexibleDoubleSpinBox::validate;
    using FlexibleDoubleSpinBox::valueFromText;
};
void focus(QWidget& widget, QEvent::Type type) {
    QFocusEvent event(type);
    QApplication::sendEvent(&widget, &event);
}
void type(QAbstractSpinBox& widget, const QString& text) {
    widget.selectAll();
    QKeyEvent key(QEvent::KeyPress, 0, Qt::NoModifier, text);
    QApplication::sendEvent(&widget, &key);
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    for (const auto& locale : {QLocale::c(), QLocale("de_DE"), QLocale("fr_FR")}) {
        double value = 0.0;
        for (const auto& text : {QStringLiteral("-0 1/2"), QStringLiteral("-½"),
                                 QStringLiteral("-0½"), QStringLiteral("-1/2")}) {
            check(FlexibleDoubleSpinBox::parseFlexibleNumber(text, locale, value)
                      && value == -0.5, "Negative fractions must keep their sign");
        }
        DecimalEditor decimal;
        decimal.setLocale(locale);
        decimal.setRange(-100.0, 100.0);
        decimal.setPrefix(QStringLiteral("Gain "));
        decimal.setSuffix(QStringLiteral(" dB"));
        QString text = QStringLiteral("Gain -2 1/2 dB");
        int position = static_cast<int>(text.size());
        check(decimal.validate(text, position) == QValidator::Acceptable,
              "Decorated fraction must be accepted");
        check(decimal.valueFromText(text) == -2.5,
              "Decorated fraction must be parsed");
        type(decimal, QStringLiteral("-2 1/2"));
        decimal.interpretText();
        check(decimal.value() == -2.5, "Typing into a control with units must work");
    }

    DecimalEditor named;
    named.setRange(0.0, 64.0);
    named.setSpecialValueText(QStringLiteral("BLACKOUT"));
    focus(named, QEvent::FocusIn);
    named.stepUp();
    focus(named, QEvent::FocusOut);
    check(named.value() == 1.0, "Stepping away from a named zero must survive focus loss");

    ResolvedAutoSpinBox automatic;
    automatic.setRange(0, 64);
    automatic.setResolvedAutoValue(8);
    int edit_count = 0;
    QObject::connect(&automatic, &QSpinBox::valueChanged, [&] { ++edit_count; });
    focus(automatic, QEvent::FocusIn);
    check(automatic.value() == 0 && automatic.cleanText() == QStringLiteral("8"),
          "Focusing Automatic must reveal the number without changing the setting");
    focus(automatic, QEvent::FocusOut);
    check(automatic.value() == 0 && edit_count == 0 && !automatic.specialValueText().isEmpty(),
          "Untouched Automatic must remain automatic without emitting an edit");
    focus(automatic, QEvent::FocusIn);
    type(automatic, QStringLiteral("8"));
    focus(automatic, QEvent::FocusOut);
    check(automatic.value() == 8 && edit_count == 1,
          "Typing the resolved number must commit an explicit value");
    automatic.setValue(0);
    edit_count = 0;
    focus(automatic, QEvent::FocusIn);
    automatic.stepUp();
    focus(automatic, QEvent::FocusOut);
    check(automatic.value() == 9 && edit_count == 1,
          "Stepping Automatic must commit from its resolved number");
    automatic.setResolvedAutoValue(12);
    automatic.setValue(0);
    check(automatic.specialValueText().startsWith(QStringLiteral("12")),
          "Automatic label must refresh even while an explicit value was selected");
    focus(automatic, QEvent::FocusIn);
    type(automatic, QStringLiteral("10"));
    auto* editor = automatic.findChild<QLineEdit*>();
    editor->clear();
    focus(automatic, QEvent::FocusOut);
    check(automatic.value() == 0, "Clearing a focused Automatic edit must restore Automatic");
    return failures == 0 ? 0 : 1;
}
