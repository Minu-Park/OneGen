#include "Utility/Qt/NumericFeatureEditor.h"
#include <QApplication>
#include <QtGlobal>
#include <cassert>
#include <limits>

int main(int argc, char** argv)
{
  QApplication application(argc, argv);
  IntegerFeatureSpinBox integer;
  onegen::FeatureNode node;
  node.kind = onegen::NodeKind::Integer;
  node.readable = node.writable = true;
  node.int_value = node.int_max = INT64_MAX;
  node.int_min = INT64_MIN;
  updateNumericFeatureEditor(&integer, node);
  assert(integer.value() == INT64_MAX);
  node.int_value = INT64_MIN;
  updateNumericFeatureEditor(&integer, node);
  assert(integer.value() == INT64_MIN);
  node.kind = onegen::NodeKind::Float;
  node.float_value = 1.234567890123;
  node.float_min = 0;
  node.float_max = 10;
  node.float_inc = 0.0000001;
  node.unit = "mm";
  QDoubleSpinBox floating;
  updateNumericFeatureEditor(&floating, node);
  assert(qAbs(floating.value() - node.float_value) < 1e-12);
  assert(floating.decimals() >= 7);
  assert(floating.singleStep() == node.float_inc);
  assert(floating.toolTip().contains(QStringLiteral("mm")));
  node.kind = onegen::NodeKind::Integer;
  node.int_value = 0;
  node.readable = false;
  updateNumericFeatureEditor(&integer, node);
  assert(integer.isEnabled());
  assert(integer.placeholderText() == QStringLiteral("Write only"));
  node.writable = false;
  updateNumericFeatureEditor(&integer, node);
  assert(!integer.isEnabled());
}
