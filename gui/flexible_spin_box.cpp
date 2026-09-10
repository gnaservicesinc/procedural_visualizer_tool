#include "flexible_spin_box.h"

#include <QLocale>
#include <QRegularExpression>
#include <QFocusEvent>
#include <QLineEdit>
#include <QSignalBlocker>

#include <algorithm>
#include <cmath>

FlexibleDoubleSpinBox::FlexibleDoubleSpinBox(QWidget* parent)
    : QDoubleSpinBox(parent) {}

bool FlexibleDoubleSpinBox::parseFlexibleNumber(
    const QString& text, const QLocale& locale, double& value) {
    QString input = text.trimmed();
    if (input.isEmpty()) return false;

    struct VulgarFraction {
        QChar symbol;
        int numerator;
        int denominator;
    };
    static const VulgarFraction fractions[] = {
        {QChar(0x00BC), 1, 4}, {QChar(0x00BD), 1, 2},
        {QChar(0x00BE), 3, 4}, {QChar(0x2150), 1, 7},
        {QChar(0x2151), 1, 9}, {QChar(0x2152), 1, 10},
        {QChar(0x2153), 1, 3}, {QChar(0x2154), 2, 3},
        {QChar(0x2155), 1, 5}, {QChar(0x2156), 2, 5},
        {QChar(0x2157), 3, 5}, {QChar(0x2158), 4, 5},
        {QChar(0x2159), 1, 6}, {QChar(0x215A), 5, 6},
        {QChar(0x215B), 1, 8}, {QChar(0x215C), 3, 8},
        {QChar(0x215D), 5, 8}, {QChar(0x215E), 7, 8},
    };
    for (const VulgarFraction& fraction : fractions) {
        if (!input.endsWith(fraction.symbol)) continue;
        QString whole_text = input.left(input.size() - 1).trimmed();
        const bool negative = whole_text.startsWith(QLatin1Char('-'));
        if (whole_text == QStringLiteral("-")
            || whole_text == QStringLiteral("+")) whole_text.clear();
        bool whole_ok = whole_text.isEmpty();
        double whole = 0.0;
        if (!whole_text.isEmpty()) {
            whole = locale.toDouble(whole_text, &whole_ok);
            if (!whole_ok) whole = QLocale::c().toDouble(whole_text, &whole_ok);
        }
        if (!whole_ok || std::floor(whole) != whole) return false;
        const double part = static_cast<double>(fraction.numerator)
                            / fraction.denominator;
        value = negative ? whole - part : whole + part;
        return std::isfinite(value);
    }

    static const QRegularExpression mixed(
        QStringLiteral(R"(^([+-]?\d+)\s*(?:\+\s*|\s+)(\d+)\s*[/⁄]\s*(\d+)$)"));
    static const QRegularExpression simple(
        QStringLiteral(R"(^([+-]?)(\d+)\s*[/⁄]\s*(\d+)$)"));
    QRegularExpressionMatch match = mixed.match(input);
    if (match.hasMatch()) {
        bool whole_ok = false;
        bool numerator_ok = false;
        bool denominator_ok = false;
        const qlonglong whole = match.captured(1).toLongLong(&whole_ok);
        const qulonglong numerator =
            match.captured(2).toULongLong(&numerator_ok);
        const qulonglong denominator =
            match.captured(3).toULongLong(&denominator_ok);
        if (!whole_ok || !numerator_ok || !denominator_ok
            || denominator == 0U) return false;
        const double part = static_cast<double>(numerator)
                            / static_cast<double>(denominator);
        value = match.captured(1).startsWith(QLatin1Char('-'))
                          ? static_cast<double>(whole) - part
                          : static_cast<double>(whole) + part;
        return std::isfinite(value);
    }
    match = simple.match(input);
    if (match.hasMatch()) {
        bool numerator_ok = false;
        bool denominator_ok = false;
        const qulonglong numerator =
            match.captured(2).toULongLong(&numerator_ok);
        const qulonglong denominator =
            match.captured(3).toULongLong(&denominator_ok);
        if (!numerator_ok || !denominator_ok || denominator == 0U) {
            return false;
        }
        value = static_cast<double>(numerator)
                / static_cast<double>(denominator);
        if (match.captured(1) == QStringLiteral("-")) value = -value;
        return std::isfinite(value);
    }

    bool ok = false;
    value = locale.toDouble(input, &ok);
    if (!ok) value = QLocale::c().toDouble(input, &ok);
    return ok && std::isfinite(value);
}

QString FlexibleDoubleSpinBox::numericText(const QString& text) const {
    QString result = text.trimmed();
    if (!prefix().isEmpty() && result.startsWith(prefix())) {
        result.remove(0, prefix().size());
    }
    if (!suffix().isEmpty() && result.endsWith(suffix().trimmed())) {
        result.chop(suffix().trimmed().size());
    }
    return result.trimmed();
}

QValidator::State FlexibleDoubleSpinBox::validate(
    QString& input, int& position) const {
    Q_UNUSED(position);
    double parsed = 0.0;
    const QString number = numericText(input);
    if (parseFlexibleNumber(number, locale(), parsed)) {
        return parsed >= minimum() && parsed <= maximum()
                   ? QValidator::Acceptable : QValidator::Intermediate;
    }
    if (number.isEmpty()) return QValidator::Intermediate;
    static const QRegularExpression partial(
        QStringLiteral(R"(^[+\-\d\s.,/⁄¼-¾⅐-⅞]*$)"));
    if (partial.match(number).hasMatch()) {
        return QValidator::Intermediate;
    }
    return QValidator::Invalid;
}

void FlexibleDoubleSpinBox::focusInEvent(QFocusEvent* event) {
    editing_special_text_ = specialValueText();
    editing_special_value_ = value() == minimum()
                             && !editing_special_text_.isEmpty();
    if (editing_special_value_) {
        const QSignalBlocker blocker(this);
        setSpecialValueText(QString{});
        lineEdit()->setModified(false);
    }
    QDoubleSpinBox::focusInEvent(event);
    if (editing_special_value_) selectAll();
}

void FlexibleDoubleSpinBox::focusOutEvent(QFocusEvent* event) {
    const bool restore_value = editing_special_value_
        && ((value() == minimum() && !lineEdit()->isModified())
            || numericText(lineEdit()->text()).isEmpty());
    if (restore_value) setValue(minimum());
    QDoubleSpinBox::focusOutEvent(event);
    if (editing_special_value_) {
        const QSignalBlocker blocker(this);
        setSpecialValueText(editing_special_text_);
    }
    editing_special_value_ = false;
    editing_special_text_.clear();
}

ResolvedAutoSpinBox::ResolvedAutoSpinBox(QWidget* parent)
    : QSpinBox(parent) {}

void ResolvedAutoSpinBox::setResolvedAutoValue(int value,
                                               const QString& label) {
    const int first_explicit = minimum() < maximum() ? minimum() + 1 : minimum();
    resolved_auto_value_ = std::clamp(value, first_explicit, maximum());
    auto_text_ = QStringLiteral("%1 (%2)")
                     .arg(resolved_auto_value_)
                     .arg(label);
    setSpecialValueText(auto_text_);
}

void ResolvedAutoSpinBox::focusInEvent(QFocusEvent* event) {
    QSpinBox::focusInEvent(event);
    editing_auto_ = value() == minimum() && !auto_text_.isEmpty();
    if (editing_auto_) {
        const QSignalBlocker blocker(this);
        setSpecialValueText(QString{});
        // Showing the resolved value must not turn Automatic into an explicit
        // setting when another control reads this widget while it has focus.
        const QSignalBlocker editor_blocker(lineEdit());
        lineEdit()->setText(locale().toString(resolved_auto_value_));
        lineEdit()->setModified(false);
    }
    if (editing_auto_) selectAll();
}

void ResolvedAutoSpinBox::focusOutEvent(QFocusEvent* event) {
    const bool return_to_auto = editing_auto_
        && ((value() == minimum() && !lineEdit()->isModified())
            || lineEdit()->text().trimmed().isEmpty());
    if (return_to_auto) {
        setValue(minimum());
        setSpecialValueText(auto_text_);
    }
    QSpinBox::focusOutEvent(event);
    if (editing_auto_) {
        setSpecialValueText(auto_text_);
    }
    editing_auto_ = false;
}

void ResolvedAutoSpinBox::stepBy(int steps) {
    if (editing_auto_ && value() == minimum() && !lineEdit()->isModified()) {
        const QSignalBlocker blocker(this);
        setValue(resolved_auto_value_);
    }
    QSpinBox::stepBy(steps);
}

double FlexibleDoubleSpinBox::valueFromText(const QString& text) const {
    double parsed = value();
    return parseFlexibleNumber(numericText(text), locale(), parsed)
               ? parsed : QDoubleSpinBox::valueFromText(text);
}
