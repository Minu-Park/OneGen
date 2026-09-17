#include "Utility/Qt/NumericFeatureEditor.h"
#include <QApplication>
#include <cassert>
#include <limits>

int main(int argc, char** argv)
{
  QApplication application(argc, argv);
  QLineEdit edit;
  onegen::FeatureNode node;
  node.kind = onegen::NodeKind::Integer;
  node.readable = node.writable = true;
  node.int_value = node.int_max = INT64_MAX;
  node.int_min = INT64_MIN;
  updateNumericFeatureEditor(&edit, node);
  assert(edit.text() == QStringLiteral("9223372036854775807"));
  node.int_value = INT64_MIN;
  updateNumericFeatureEditor(&edit, node);
  assert(edit.text() == QStringLiteral("-9223372036854775808"));
  node.kind = onegen::NodeKind::Float;
  node.float_value = 1.2345678901234567;
  node.float_inc = 0.0000001;
  node.unit = "mm";
  updateNumericFeatureEditor(&edit, node);
  assert(edit.text().toDouble() == node.float_value);
  assert(edit.toolTip().contains(QStringLiteral("mm")));
  node.readable = false;
  updateNumericFeatureEditor(&edit, node);
  assert(edit.text().isEmpty() && edit.isEnabled());
  assert(edit.placeholderText() == QStringLiteral("Write only"));
  node.writable = false;
  updateNumericFeatureEditor(&edit, node);
  assert(!edit.isEnabled());
}
