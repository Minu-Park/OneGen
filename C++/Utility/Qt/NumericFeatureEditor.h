#pragma once

#include "OneGenNode.h"

#include <QAbstractSpinBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QString>
#include <algorithm>
#include <cmath>
#include <limits>

/** 64-bit integer spin box; QSpinBox cannot hold GenApi IInteger values. */
class IntegerFeatureSpinBox final : public QAbstractSpinBox {
  Q_OBJECT
public:
  explicit IntegerFeatureSpinBox(QWidget* parent = nullptr) : QAbstractSpinBox(parent)
  {
    setKeyboardTracking(false);
    setAlignment(Qt::AlignRight);
    setCorrectionMode(CorrectToNearestValue);
  }

  void setRange(qlonglong minimum, qlonglong maximum)
  {
    min_ = (std::min)(minimum, maximum);
    max_ = (std::max)(minimum, maximum);
    setValue(value_);
  }

  void setSingleStep(qlonglong step) { inc_ = step > 0 ? step : 1; }

  void setValue(qlonglong value)
  {
    const auto next = clampValue(value);
    if (next == value_ && lineEdit()->text() == QString::number(next)) return;
    value_ = next;
    const QSignalBlocker blocker(lineEdit());
    lineEdit()->setText(QString::number(value_));
  }

  qlonglong value() const { return value_; }

  void setPlaceholderText(const QString& text) { lineEdit()->setPlaceholderText(text); }
  QString placeholderText() const { return lineEdit()->placeholderText(); }

  void commitDisplayedText()
  {
    bool ok = false;
    const qlonglong parsed = lineEdit()->text().toLongLong(&ok);
    setValue(ok ? parsed : value_);
  }

  void stepBy(int steps) override
  {
    if (steps == 0) return;
    qlonglong next = value_;
    if (steps > 0) {
      const qlonglong room = max_ - value_;
      const qlonglong add = static_cast<qlonglong>(steps) * inc_;
      next = add > room ? max_ : value_ + add;
    } else {
      const qlonglong room = value_ - min_;
      const qlonglong sub = static_cast<qlonglong>(-steps) * inc_;
      next = sub > room ? min_ : value_ - sub;
    }
    next = clampValue(next);
    if (next == value_) return;
    setValue(next);
    emit editingFinished();
  }

  StepEnabled stepEnabled() const override
  {
    if (!isEnabled() || isReadOnly()) return StepNone;
    StepEnabled enabled = StepNone;
    if (value_ < max_) enabled |= StepUpEnabled;
    if (value_ > min_) enabled |= StepDownEnabled;
    return enabled;
  }

  QValidator::State validate(QString& input, int&) const override
  {
    if (input.isEmpty() || input == QLatin1String("-") || input == QLatin1String("+"))
      return QValidator::Intermediate;
    bool ok = false;
    const qlonglong parsed = input.toLongLong(&ok);
    if (!ok) return QValidator::Invalid;
    if (parsed < min_ || parsed > max_) return QValidator::Intermediate;
    return QValidator::Acceptable;
  }

  void fixup(QString& input) const override
  {
    bool ok = false;
    qlonglong parsed = input.toLongLong(&ok);
    if (!ok) parsed = value_;
    input = QString::number(clampValue(parsed));
  }

private:
  qlonglong clampValue(qlonglong value) const
  {
    if (value < min_) return min_;
    if (value > max_) return max_;
    if (inc_ <= 1) return value;
    const qlonglong snapped = min_ + ((value - min_) / inc_) * inc_;
    return snapped > max_ ? max_ : snapped;
  }

  qlonglong value_ = 0;
  qlonglong min_ = (std::numeric_limits<qlonglong>::min)();
  qlonglong max_ = (std::numeric_limits<qlonglong>::max)();
  qlonglong inc_ = 1;
};

inline int floatFeatureDecimals(double value, double minimum, double maximum, double increment)
{
  auto places = [](double v) {
    if (!std::isfinite(v)) return 0;
    QString text = QString::number(std::abs(v), 'f', 12);
    while (text.endsWith(QLatin1Char('0'))) text.chop(1);
    const int dot = text.indexOf(QLatin1Char('.'));
    if (dot < 0) return 0;
    return (std::min)(12, static_cast<int>(text.size() - dot - 1));
  };
  int decimals = places(value);
  decimals = (std::max)(decimals, places(minimum));
  decimals = (std::max)(decimals, places(maximum));
  if (increment > 0) decimals = (std::max)(decimals, places(increment));
  if (decimals == 0 && !(increment > 0)) decimals = 6;
  return std::clamp(decimals, 0, 12);
}

inline QString numericFeatureToolTip(const onegen::FeatureNode& node)
{
  const bool integer = node.kind == onegen::NodeKind::Integer;
  const auto minimum = integer ? QString::number(static_cast<qlonglong>(node.int_min))
      : QString::number(node.float_min, 'g', 17);
  const auto maximum = integer ? QString::number(static_cast<qlonglong>(node.int_max))
      : QString::number(node.float_max, 'g', 17);
  const auto increment = integer ? QString::number(static_cast<qlonglong>(node.int_inc))
      : node.float_inc > 0 ? QString::number(node.float_inc, 'g', 17) : QStringLiteral("Continuous");
  return QStringLiteral("Range: %1 to %2\nIncrement: %3\nUnit: %4\nRepresentation: %5")
      .arg(minimum, maximum, increment, QString::fromStdString(node.unit),
           QString::fromStdString(node.representation));
}

/** Updates an integer spin box from a GenApi snapshot without narrowing to 32-bit. */
inline void updateNumericFeatureEditor(IntegerFeatureSpinBox* spin, const onegen::FeatureNode& node)
{
  const QSignalBlocker blocker(spin);
  spin->setEnabled(node.writable);
  spin->setRange(node.int_min, node.int_max);
  spin->setSingleStep(node.int_inc > 0 ? node.int_inc : 1);
  spin->setPlaceholderText(node.readable ? QString{} : QStringLiteral("Write only"));
  if (node.readable && !(spin->hasFocus() && spin->value() == static_cast<qlonglong>(node.int_value)))
    spin->setValue(node.int_value);
  spin->setToolTip(numericFeatureToolTip(node));
}

/** Updates a double spin box using increment-aware decimals instead of free-form text. */
inline void updateNumericFeatureEditor(QDoubleSpinBox* spin, const onegen::FeatureNode& node)
{
  const QSignalBlocker blocker(spin);
  spin->setEnabled(node.writable);
  spin->setKeyboardTracking(false);
  spin->setAlignment(Qt::AlignRight);
  const double minimum = std::isfinite(node.float_min) ? node.float_min : -1e12;
  const double maximum = std::isfinite(node.float_max) ? node.float_max : 1e12;
  const int decimals = floatFeatureDecimals(node.float_value, minimum, maximum, node.float_inc);
  spin->setDecimals(decimals);
  spin->setRange((std::min)(minimum, maximum), (std::max)(minimum, maximum));
  spin->setSingleStep(node.float_inc > 0 ? node.float_inc
      : (decimals > 0 ? std::pow(10.0, -decimals) : 1.0));
  if (node.readable && !(spin->hasFocus() && spin->value() == node.float_value))
    spin->setValue(node.float_value);
  spin->setToolTip(numericFeatureToolTip(node));
}
