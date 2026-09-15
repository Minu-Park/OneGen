#pragma once

#include "onegen/node.hpp"

#include <QList>
#include <QSet>
#include <QString>
#include <QTreeWidget>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QTreeWidgetItem;
class QTimer;

class FeatureTreeWidget : public QTreeWidget {
  Q_OBJECT
public:
  using FeatureRead = std::function<onegen::FeatureNode(int, std::string_view)>;
  using FeatureWrite =
      std::function<onegen::FeatureNode(int, std::string_view, std::string_view, std::string_view)>;

  explicit FeatureTreeWidget(QWidget* parent = nullptr);
  void setNodeMap(onegen::NodeMap* map, std::string_view camera_name = {});
  /** Shows multiple GenApi module maps below one camera root. */
  void setNodeMaps(std::vector<std::pair<std::string, onegen::NodeMap*>> maps,
                   std::string_view camera_name = {});
  void setFeatureModules(std::vector<std::pair<std::string, onegen::FeatureNode>> modules,
                         FeatureRead read, FeatureWrite write, std::string_view camera_name = {});
  void setFeatureSnapshot(std::vector<std::pair<std::string, onegen::FeatureNode>> modules);
  void setGrabbing(bool grabbing);
  void invalidate(int module, std::string_view name);

private:
  void rebuild();
  void addCategories(QTreeWidgetItem* cam, const onegen::FeatureNode& cat, int map_index);
  void addLeaf(QTreeWidgetItem* parent, const onegen::FeatureNode& n, int map_index);
  QWidget* createEditor(const onegen::FeatureNode& n, int map_index);
  bool refreshEditor(const QString& nodeName, int map_index);
  void refreshAllEditors();
  bool shouldShow(const onegen::FeatureNode& n) const;
  QList<QTreeWidgetItem*> itemsNamed(const QString& nodeName, int map_index) const;
  void collectExpanded(QTreeWidgetItem* item, QSet<QString>& out) const;
  void restoreExpanded(QTreeWidgetItem* item, const QSet<QString>& names);
  void drainInvalidates();
  onegen::FeatureNode resolve(const QString& nodeName, int map_index) const;
  void write(int map_index, const QString& nodeName, std::string_view op, std::string_view value);

  std::vector<std::pair<std::string, onegen::NodeMap*>> maps_;
  std::vector<std::pair<std::string, onegen::FeatureNode>> modules_;
  FeatureRead read_;
  FeatureWrite write_;
  QString camera_name_;
  bool grabbing_ = false;
  bool rebuilding_ = false;
  QTimer* drain_timer_ = nullptr;
  QSet<QString> pending_;
};
