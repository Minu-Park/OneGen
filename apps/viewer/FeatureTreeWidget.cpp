#include "FeatureTreeWidget.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>

#include <climits>
#include <exception>

namespace {

int clamp_int(std::int64_t v) {
  if (v < INT_MIN) return INT_MIN;
  if (v > INT_MAX) return INT_MAX;
  return static_cast<int>(v);
}

void expand_camera_only(QTreeWidgetItem* item, int depth) {
  if (!item) return;
  item->setExpanded(depth == 0);
  for (int i = 0; i < item->childCount(); ++i) expand_camera_only(item->child(i), depth + 1);
}

}  // namespace

FeatureTreeWidget::FeatureTreeWidget(QWidget* parent) : QTreeWidget(parent) {
  setObjectName(QStringLiteral("CameraFeaturesTree"));
  setColumnCount(2);
  setHeaderLabels({"Feature", "Value"});
  setUniformRowHeights(true);
  setRootIsDecorated(true);
  drain_timer_ = new QTimer(this);
  drain_timer_->setSingleShot(true);
  drain_timer_->setInterval(50);
  connect(drain_timer_, &QTimer::timeout, this, &FeatureTreeWidget::drainInvalidates);
}

void FeatureTreeWidget::setNodeMap(onegen::NodeMap* map, std::string_view camera_name) {
  setNodeMaps(map ? std::vector<std::pair<std::string, onegen::NodeMap*>>{{{}, map}}
                  : std::vector<std::pair<std::string, onegen::NodeMap*>>{}, camera_name);
}

void FeatureTreeWidget::setNodeMaps(
    std::vector<std::pair<std::string, onegen::NodeMap*>> maps,
    std::string_view camera_name) {
  for (const auto& entry : maps_)
    if (entry.second) entry.second->set_invalidate_callback({});
  maps_ = std::move(maps);
  modules_.clear();
  read_ = [this](int index, std::string_view name) {
    if (index < 0 || index >= static_cast<int>(maps_.size()) || !maps_[static_cast<std::size_t>(index)].second)
      return onegen::FeatureNode{};
    return onegen::snapshot(maps_[static_cast<std::size_t>(index)].second->find(std::string(name)));
  };
  write_ = [this](int index, std::string_view name, std::string_view op, std::string_view value) {
    auto node = maps_[static_cast<std::size_t>(index)].second->find(std::string(name));
    onegen::apply_feature(node, op, value);
    return onegen::snapshot(node);
  };
  for (const auto& entry : maps_) {
    onegen::FeatureNode root;
    if (entry.second) root = onegen::snapshot(entry.second->root());
    modules_.emplace_back(entry.first, std::move(root));
  }
  camera_name_ = QString::fromUtf8(camera_name.data(), static_cast<int>(camera_name.size()));
  if (camera_name_.isEmpty()) camera_name_ = QStringLiteral("Camera");
  rebuild();
  for (std::size_t index = 0; index < maps_.size(); ++index) {
    if (maps_[index].second) {
      maps_[index].second->set_invalidate_callback(
          [this, index](std::string_view name) { invalidate(static_cast<int>(index), name); });
    }
  }
}

void FeatureTreeWidget::setFeatureModules(
    std::vector<std::pair<std::string, onegen::FeatureNode>> modules, FeatureRead read,
    FeatureWrite write, std::string_view camera_name)
{
  for (const auto& entry : maps_)
    if (entry.second) entry.second->set_invalidate_callback({});
  maps_.clear();
  modules_ = std::move(modules);
  read_ = std::move(read);
  write_ = std::move(write);
  camera_name_ = QString::fromUtf8(camera_name.data(), static_cast<int>(camera_name.size()));
  if (camera_name_.isEmpty()) camera_name_ = QStringLiteral("Camera");
  rebuild();
}

void FeatureTreeWidget::setGrabbing(bool grabbing) {
  grabbing_ = grabbing;
}

void FeatureTreeWidget::setFeatureSnapshot(
    std::vector<std::pair<std::string, onegen::FeatureNode>> modules) {
  modules_ = std::move(modules);
  pending_.clear();
  rebuild();
}

void FeatureTreeWidget::invalidate(const int module, const std::string_view name)
{
  const QString q = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
  QMetaObject::invokeMethod(
      this,
      [this, q, module] {
        if (rebuilding_ || grabbing_) return;
        pending_.insert(QString::number(module) + QChar('\n') + q);
        if (!drain_timer_->isActive()) drain_timer_->start();
      },
      Qt::QueuedConnection);
}

void FeatureTreeWidget::rebuild() {
  rebuilding_ = true;
  pending_.clear();
  QSet<QString> expanded;
  for (int i = 0; i < topLevelItemCount(); ++i) collectExpanded(topLevelItem(i), expanded);
  QString selected;
  int selected_map = 0;
  if (auto* cur = currentItem()) selected = cur->data(0, Qt::UserRole).toString();
  if (auto* cur = currentItem()) selected_map = cur->data(0, Qt::UserRole + 1).toInt();
  const int scroll = verticalScrollBar()->value();

  clear();
  if (modules_.empty()) {
    rebuilding_ = false;
    return;
  }

  auto* cam = new QTreeWidgetItem(this, QStringList(camera_name_));
  cam->setData(0, Qt::UserRole, QStringLiteral("__camera_root__"));
  try {
    for (std::size_t index = 0; index < modules_.size(); ++index) {
      const auto& entry = modules_[index];
      auto* parent = cam;
      if (!entry.first.empty()) {
        parent = new QTreeWidgetItem(cam, QStringList(QString::fromStdString(entry.first)));
        parent->setData(0, Qt::UserRole, QStringLiteral("__module_%1__").arg(index));
        parent->setData(0, Qt::UserRole + 1, static_cast<int>(index));
      }
      addCategories(parent, entry.second, static_cast<int>(index));
    }
  } catch (...) {
    clear();
    rebuilding_ = false;
    return;
  }

  if (expanded.isEmpty())
    expand_camera_only(cam, 0);
  else
    restoreExpanded(cam, expanded);

  if (selected.isEmpty()) {
    setCurrentItem(cam);
  } else {
    const auto hits = itemsNamed(selected, selected_map);
    if (!hits.isEmpty()) setCurrentItem(hits.front());
  }
  verticalScrollBar()->setValue(scroll);
  rebuilding_ = false;
}

void FeatureTreeWidget::addCategories(QTreeWidgetItem* cam, const onegen::FeatureNode& cat,
                                      const int map_index) {
  if (cat.kind != onegen::NodeKind::Category && !cat.name.empty()) return;

  QTreeWidgetItem* item = cam;
  if (cat.name != "Root" && !cat.name.empty()) {
    item = new QTreeWidgetItem(cam, QStringList(QString::fromStdString(cat.display_name)));
    item->setData(0, Qt::UserRole, QString::fromStdString(cat.name));
    item->setData(0, Qt::UserRole + 1, map_index);
    for (const auto& c : cat.children) {
      if (c.kind == onegen::NodeKind::Category) continue;
      addLeaf(item, c, map_index);
    }
  }
  for (const auto& c : cat.children) {
    if (c.kind == onegen::NodeKind::Category)
      addCategories(cam, c, map_index);
    else if (cat.name == "Root" || cat.name.empty())
      addLeaf(item, c, map_index);
  }
}

void FeatureTreeWidget::addLeaf(QTreeWidgetItem* parent, const onegen::FeatureNode& n,
                                const int map_index) {
  if (!shouldShow(n)) return;
  QWidget* w = createEditor(n, map_index);
  if (!w) return;
  auto* item = new QTreeWidgetItem(parent, QStringList(QString::fromStdString(n.display_name)));
  item->setData(0, Qt::UserRole, QString::fromStdString(n.name));
  item->setData(0, Qt::UserRole + 1, map_index);
  const auto help = n.tooltip.empty() ? n.description : n.tooltip;
  item->setToolTip(0, QString::fromStdString(help));
  setItemWidget(item, 1, w);
}

QWidget* FeatureTreeWidget::createEditor(const onegen::FeatureNode& n, const int map_index) {
  const QString nodeName = QString::fromStdString(n.name);
  QWidget* w = nullptr;
  try {
    switch (n.kind) {
      case onegen::NodeKind::Integer: {
        auto* box = new QSpinBox;
        w = box;
        QSignalBlocker block(box);
        box->setRange(clamp_int(n.int_min), clamp_int(n.int_max));
        box->setSingleStep(n.int_inc > 0 && n.int_inc <= INT_MAX ? static_cast<int>(n.int_inc) : 1);
        if (n.readable) box->setValue(clamp_int(n.int_value));
        connect(box, &QSpinBox::valueChanged, this, [this, nodeName, map_index](int v) {
          write(map_index, nodeName, "int", std::to_string(v));
        });
      } break;
      case onegen::NodeKind::Float: {
        auto* box = new QDoubleSpinBox;
        w = box;
        QSignalBlocker block(box);
        box->setRange(n.float_min, n.float_max);
        box->setSingleStep(0.1);
        box->setDecimals(6);
        if (n.readable) box->setValue(n.float_value);
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, nodeName, map_index](double v) {
          write(map_index, nodeName, "float", std::to_string(v));
        });
      } break;
      case onegen::NodeKind::Boolean: {
        auto* box = new QCheckBox;
        w = box;
        QSignalBlocker block(box);
        if (n.readable) box->setChecked(n.bool_value);
        connect(box, &QCheckBox::checkStateChanged, this,
                [this, nodeName, map_index](Qt::CheckState s) {
          write(map_index, nodeName, "bool", s == Qt::Checked ? "1" : "0");
        });
      } break;
      case onegen::NodeKind::String: {
        auto* edit = new QLineEdit;
        w = edit;
        QSignalBlocker block(edit);
        if (n.readable) edit->setText(QString::fromStdString(n.string_value));
        connect(edit, &QLineEdit::editingFinished, this, [this, nodeName, map_index, edit] {
          write(map_index, nodeName, "string", edit->text().toStdString());
        });
      } break;
      case onegen::NodeKind::Enumeration: {
        auto* combo = new QComboBox;
        w = combo;
        QSignalBlocker block(combo);
        for (const auto& e : n.enum_entries)
          combo->addItem(QString::fromStdString(e), QString::fromStdString(e));
        if (n.readable) combo->setCurrentText(QString::fromStdString(n.enum_value));
        connect(combo, &QComboBox::currentIndexChanged, this, [this, nodeName, map_index](int) {
          auto* combo = qobject_cast<QComboBox*>(sender());
          if (!combo) return;
          write(map_index, nodeName, "enum", combo->currentData().toString().toStdString());
        });
      } break;
      case onegen::NodeKind::Command: {
        auto* btn = new QPushButton(QStringLiteral("Execute"));
        w = btn;
        connect(btn, &QPushButton::clicked, this, [this, nodeName, map_index] {
          write(map_index, nodeName, "execute", {});
        });
      } break;
      case onegen::NodeKind::Category:
        return nullptr;
      default: {
        auto* lab = new QLabel;
        w = lab;
        lab->setText(QString::fromStdString(n.string_value));
      } break;
    }
  } catch (...) {
    delete w;
    return nullptr;
  }
  if (w) w->setEnabled(n.writable || n.kind == onegen::NodeKind::Command);
  return w;
}

void FeatureTreeWidget::write(const int map_index, const QString& nodeName, const std::string_view op,
                              const std::string_view value)
{
  if (!write_) return;
  try {
    write_(map_index, nodeName.toStdString(), op, value);
    refreshEditor(nodeName, map_index);
  } catch (const std::exception& error) {
    qWarning().noquote() << QStringLiteral("OneGen feature write failed: %1: %2")
                                .arg(nodeName, QString::fromLocal8Bit(error.what()));
    refreshEditor(nodeName, map_index);
  } catch (...) {
    qWarning().noquote() << QStringLiteral("OneGen feature write failed: %1").arg(nodeName);
    refreshEditor(nodeName, map_index);
  }
}

bool FeatureTreeWidget::refreshEditor(const QString& nodeName, const int map_index) {
  const auto n = resolve(nodeName, map_index);
  if (n.name.empty() && n.kind == onegen::NodeKind::Other) return false;
  const auto items = itemsNamed(nodeName, map_index);
  if (items.isEmpty()) return false;
  bool ok = false;
  for (auto* item : items) {
    auto* w = itemWidget(item, 1);
    try {
      switch (n.kind) {
        case onegen::NodeKind::Integer:
          if (auto* box = qobject_cast<QSpinBox*>(w)) {
            QSignalBlocker b(box);
            box->setEnabled(n.writable);
            box->setRange(clamp_int(n.int_min), clamp_int(n.int_max));
            box->setSingleStep(n.int_inc > 0 && n.int_inc <= INT_MAX ? static_cast<int>(n.int_inc) : 1);
            if (n.readable) box->setValue(clamp_int(n.int_value));
            ok = true;
          }
          break;
        case onegen::NodeKind::Float:
          if (auto* box = qobject_cast<QDoubleSpinBox*>(w)) {
            QSignalBlocker b(box);
            box->setEnabled(n.writable);
            box->setRange(n.float_min, n.float_max);
            if (n.readable) box->setValue(n.float_value);
            ok = true;
          }
          break;
        case onegen::NodeKind::Boolean:
          if (auto* box = qobject_cast<QCheckBox*>(w)) {
            QSignalBlocker b(box);
            box->setEnabled(n.writable);
            if (n.readable) box->setChecked(n.bool_value);
            ok = true;
          }
          break;
        case onegen::NodeKind::String:
          if (auto* edit = qobject_cast<QLineEdit*>(w)) {
            QSignalBlocker b(edit);
            edit->setEnabled(n.writable);
            if (n.readable) edit->setText(QString::fromStdString(n.string_value));
            ok = true;
          }
          break;
        case onegen::NodeKind::Enumeration: {
          if (auto* combo = qobject_cast<QComboBox*>(w)) {
            QSignalBlocker b(combo);
            combo->setEnabled(n.writable);
            const auto& entries = n.enum_entries;
            bool same = combo->count() == static_cast<int>(entries.size());
            if (same) {
              for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemData(i).toString().toStdString() != entries[static_cast<size_t>(i)]) {
                  same = false;
                  break;
                }
              }
            }
            if (!same) {
              combo->clear();
              for (const auto& e : entries)
                combo->addItem(QString::fromStdString(e), QString::fromStdString(e));
            }
            if (n.readable) combo->setCurrentText(QString::fromStdString(n.enum_value));
            ok = true;
          }
        } break;
        case onegen::NodeKind::Command:
          if (auto* btn = qobject_cast<QPushButton*>(w)) {
            btn->setEnabled(n.writable);
            ok = true;
          }
          break;
        default:
          if (auto* lab = qobject_cast<QLabel*>(w)) {
            if (n.readable) lab->setText(QString::fromStdString(n.string_value));
            ok = true;
          }
          break;
      }
    } catch (...) {
    }
  }
  return ok;
}

QList<QTreeWidgetItem*> FeatureTreeWidget::itemsNamed(
    const QString& nodeName, const int map_index) const {
  QList<QTreeWidgetItem*> out;
  QList<QTreeWidgetItem*> stack;
  for (int i = 0; i < topLevelItemCount(); ++i) stack.append(topLevelItem(i));
  while (!stack.isEmpty()) {
    auto* cur = stack.takeLast();
    if (cur->data(0, Qt::UserRole).toString() == nodeName
        && cur->data(0, Qt::UserRole + 1).toInt() == map_index) out.append(cur);
    for (int i = 0; i < cur->childCount(); ++i) stack.append(cur->child(i));
  }
  return out;
}

void FeatureTreeWidget::collectExpanded(QTreeWidgetItem* item, QSet<QString>& out) const {
  if (!item) return;
  if (item->isExpanded()) out.insert(item->data(0, Qt::UserRole).toString());
  for (int i = 0; i < item->childCount(); ++i) collectExpanded(item->child(i), out);
}

void FeatureTreeWidget::restoreExpanded(QTreeWidgetItem* item, const QSet<QString>& names) {
  if (!item) return;
  item->setExpanded(names.contains(item->data(0, Qt::UserRole).toString()));
  for (int i = 0; i < item->childCount(); ++i) restoreExpanded(item->child(i), names);
}

bool FeatureTreeWidget::shouldShow(const onegen::FeatureNode& n) const {
  if (n.kind == onegen::NodeKind::Category) return false;
  if (!n.visible) return false;
  return n.readable || n.kind == onegen::NodeKind::Command;
}

void FeatureTreeWidget::refreshAllEditors() {
  if (modules_.empty() || rebuilding_) return;
  QList<QTreeWidgetItem*> stack;
  for (int i = 0; i < topLevelItemCount(); ++i) stack.append(topLevelItem(i));
  while (!stack.isEmpty()) {
    auto* cur = stack.takeLast();
    const auto name = cur->data(0, Qt::UserRole).toString();
    if (!name.isEmpty() && !name.startsWith(QLatin1String("__")))
      refreshEditor(name, cur->data(0, Qt::UserRole + 1).toInt());
    for (int i = 0; i < cur->childCount(); ++i) stack.append(cur->child(i));
  }
}

void FeatureTreeWidget::drainInvalidates() {
  const auto batch = pending_;
  pending_.clear();
  if (rebuilding_ || modules_.empty()) return;

  bool need_rebuild = false;
  for (const auto& key : batch) {
    const auto split = key.indexOf(QChar('\n'));
    const int map_index = key.left(split).toInt();
    const auto name = key.mid(split + 1);
    auto n = resolve(name, map_index);
    const bool shown = !itemsNamed(name, map_index).isEmpty();
    const bool show = shouldShow(n);
    if (n.kind == onegen::NodeKind::Category) continue;
    if (shown && show) {
      if (!refreshEditor(name, map_index)) need_rebuild = true;
    } else if (shown != show) {
      need_rebuild = true;
    }
  }
  if (need_rebuild && !grabbing_) rebuild();
}

onegen::FeatureNode FeatureTreeWidget::resolve(const QString& nodeName, const int map_index) const {
  if (!read_) return {};
  try {
    return read_(map_index, nodeName.toStdString());
  } catch (...) {
    return {};
  }
}
