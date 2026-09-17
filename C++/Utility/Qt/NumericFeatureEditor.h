#pragma once

#include "OneGenNode.h"
#include <QLineEdit>
#include <QSignalBlocker>

/** Updates a native text editor without narrowing integer values or rounding doubles. */
inline void updateNumericFeatureEditor(QLineEdit* edit, const onegen::FeatureNode& node)
{
  const QSignalBlocker blocker(edit);
  const bool integer = node.kind == onegen::NodeKind::Integer;
  edit->setEnabled(node.writable);
  edit->setPlaceholderText(node.readable ? QString{} : QStringLiteral("Write only"));
  edit->setText(!node.readable ? QString{} : integer
      ? QString::number(static_cast<qlonglong>(node.int_value))
      : QString::number(node.float_value, 'g', 17));
  const auto minimum = integer ? QString::number(static_cast<qlonglong>(node.int_min))
      : QString::number(node.float_min, 'g', 17);
  const auto maximum = integer ? QString::number(static_cast<qlonglong>(node.int_max))
      : QString::number(node.float_max, 'g', 17);
  const auto increment = integer ? QString::number(static_cast<qlonglong>(node.int_inc))
      : node.float_inc > 0 ? QString::number(node.float_inc, 'g', 17) : QStringLiteral("Continuous");
  edit->setToolTip(QStringLiteral("Range: %1 to %2\nIncrement: %3\nUnit: %4\nRepresentation: %5")
      .arg(minimum, maximum, increment, QString::fromStdString(node.unit),
           QString::fromStdString(node.representation)));
}
