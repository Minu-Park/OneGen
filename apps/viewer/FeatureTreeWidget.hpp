#pragma once

#include "onegen/node.hpp"

#include <QList>
#include <QSet>
#include <QString>
#include <QTreeWidget>
#include <string>
#include <string_view>

class QTreeWidgetItem;
class QTimer;

class FeatureTreeWidget : public QTreeWidget {
  Q_OBJECT
public:
  explicit FeatureTreeWidget(QWidget* parent = nullptr);
  void setNodeMap(onegen::NodeMap* map, std::string_view camera_name = {});
  void setGrabbing(bool grabbing);

private:
  void rebuild();
  void addCategories(QTreeWidgetItem* cam, const onegen::Node& cat,
                     const std::string& parent_display);
  void addLeaf(QTreeWidgetItem* parent, const onegen::Node& n);
  QWidget* createEditor(const onegen::Node& n);
  bool refreshEditor(const QString& nodeName);
  void refreshAllEditors();
  bool shouldShow(const onegen::Node& n) const;
  QList<QTreeWidgetItem*> itemsNamed(const QString& nodeName) const;
  void collectExpanded(QTreeWidgetItem* item, QSet<QString>& out) const;
  void restoreExpanded(QTreeWidgetItem* item, const QSet<QString>& names);
  void onInvalidate(std::string_view name);
  void drainInvalidates();
  onegen::Node resolve(const QString& nodeName) const;

  onegen::NodeMap* map_ = nullptr;
  QString camera_name_;
  bool grabbing_ = false;
  bool rebuilding_ = false;
  QTimer* drain_timer_ = nullptr;
  QSet<QString> pending_;
};
