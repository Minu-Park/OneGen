#pragma once

#include "OneGenNode.h"
#include "OneGenTypes.h"
#include "OneGenCoordinator.h"

#include <QFutureWatcher>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class OneGenSourceController;
class QComboBox;
class QLabel;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
namespace onegen::worker {
class Coordinator;
class Lease;
}

/** Playground control surface for one OneGen camera session. */
class QOneGenWidget final : public QWidget {
  Q_OBJECT
public:
  using DiscoverFn = std::function<std::vector<onegen::DeviceInfo>(
      onegen::worker::Coordinator::ProgressFn)>;
  QOneGenWidget(std::shared_ptr<onegen::worker::Coordinator> coordinator,
                OneGenSourceController* controller,
                std::vector<onegen::DeviceInfo> devices,
                DiscoverFn discover,
                QWidget* parent = nullptr);
  ~QOneGenWidget() override;
  void setTitleCallback(std::function<void(const QString&)> callback);
  void prepareForShutdown();

private:
  using FeatureRead = std::function<onegen::FeatureNode(int, std::string_view)>;
  using FeatureWrite =
      std::function<onegen::FeatureNode(int, std::string_view, std::string_view, std::string_view)>;

  struct AsyncResult {
    QString error;
    QString title;
    std::vector<onegen::DeviceInfo> devices;
    std::vector<onegen::DataStreamInfo> streams;
    std::vector<std::pair<std::string, onegen::FeatureNode>> features;
    std::shared_ptr<onegen::worker::Lease> lease;
    QString writeNode;
    QString writeOp;
    QString writeValue;
    int writeMap = 0;
    qint64 elapsedMs = 0;
  };
  enum class Operation { None, Connect, Disconnect, GrabOne, Start, Stop, Write, Restore };

  void refresh();
  void updateDiscoveryDevices(std::vector<onegen::DeviceInfo> devices);
  void connectCamera();
  void disconnectCamera();
  bool run(Operation operation, std::function<AsyncResult()> work);
  void finishOperation();
  void startGrabOne();
  void finishGrabOne();
  void dropLease();
  void setOperationActive(bool active);
  /** Refresh only connection/acquisition state; operation details belong in logs. */
  void updateStatus();

  void setFeatureModules(std::vector<std::pair<std::string, onegen::FeatureNode>> modules,
                         FeatureRead read, FeatureWrite write, std::string_view camera_name);
  /**
   * Applies a complete feature snapshot.
   *
   * Same-structure refreshes update existing editors in place and insert newly
   * visible leaves. Selector-style remove, type, or editor-kind changes prune or
   * replace those rows; the tree is rebuilt only when the camera root is missing
   * or an editor cannot be reused.
   */
  void setFeatureSnapshot(std::vector<std::pair<std::string, onegen::FeatureNode>> modules);
  void setFeatureGrabbing(bool grabbing);
  void invalidateFeature(int module, std::string_view name);
  void rebuildFeatures();
  struct FeatureViewport {
    QString currentName;
    int currentMap = 0;
    QString topName;
    int topMap = 0;
    int topOffset = 0;
    int verticalScroll = 0;
    int horizontalScroll = 0;
  };
  FeatureViewport captureFeatureViewport() const;
  void restoreFeatureViewport(const FeatureViewport& viewport);
  bool applyFeatureSnapshotInPlace();
  bool syncFeatureCategory(QTreeWidgetItem* parent, const onegen::FeatureNode& cat, int map_index,
                           QSet<QString>& keep, bool& mutated);
  QTreeWidgetItem* findFeatureChild(QTreeWidgetItem* parent, const QString& name,
                                    int map_index) const;
  QTreeWidgetItem* ensureFeatureGroup(QTreeWidgetItem* parent, const QString& name,
                                      const QString& display, int map_index, bool& mutated);
  void pruneFeatureChildren(QTreeWidgetItem* parent, const QSet<QString>& keep, bool& mutated);
  void addFeatureCategories(QTreeWidgetItem* cam, const onegen::FeatureNode& cat, int map_index);
  void addFeatureLeaf(QTreeWidgetItem* parent, const onegen::FeatureNode& n, int map_index);
  QWidget* createFeatureEditor(const onegen::FeatureNode& n, int map_index);
  bool applyFeatureEditor(QTreeWidgetItem* item, const onegen::FeatureNode& n, int map_index,
                          bool* mutated = nullptr);
  bool applyFeatureNodeToEditor(QWidget* editor, const onegen::FeatureNode& n,
                                bool* mutated = nullptr);
  bool refreshFeatureEditor(const QString& nodeName, int map_index);
  bool shouldShowFeature(const onegen::FeatureNode& n) const;
  QList<QTreeWidgetItem*> featureItemsNamed(const QString& nodeName, int map_index) const;
  void collectExpandedFeatures(QTreeWidgetItem* item, QSet<QString>& out) const;
  void restoreExpandedFeatures(QTreeWidgetItem* item, const QSet<QString>& names);
  void drainFeatureInvalidates();
  onegen::FeatureNode resolveFeature(const QString& nodeName, int map_index) const;
  /** Writes a node and refreshes the snapshot off the GUI thread. */
  void writeFeature(int map_index, const QString& nodeName, std::string_view op,
                    std::string_view value);

  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  std::shared_ptr<onegen::worker::Lease> lease_;
  OneGenSourceController* controller_ = nullptr;
  std::vector<onegen::DeviceInfo> devices_;
  DiscoverFn discover_;
  std::function<void(const QString&)> title_callback_;
  QComboBox* devices_box_ = nullptr;
  QComboBox* streams_box_ = nullptr;
  QToolButton* refresh_button_ = nullptr;
  QToolButton* connect_button_ = nullptr;
  QToolButton* grab_one_button_ = nullptr;
  QToolButton* grab_live_button_ = nullptr;
  QLabel* status_ = nullptr;
  QTreeWidget* features_ = nullptr;
  QTimer* feature_drain_timer_ = nullptr;
  FeatureRead feature_read_;
  FeatureWrite feature_write_;
  std::vector<std::pair<std::string, onegen::FeatureNode>> feature_modules_;
  QString feature_camera_name_;
  QSet<QString> pending_features_;
  QFutureWatcher<AsyncResult> operation_watcher_;
  QFutureWatcher<AsyncResult> grab_watcher_;
  QFutureWatcher<AsyncResult> discovery_watcher_;
  Operation operation_ = Operation::None;
  bool shutting_down_ = false;
  bool feature_grabbing_ = false;
  bool rebuilding_features_ = false;
};
