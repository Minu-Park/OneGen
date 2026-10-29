#pragma once
#include "OneGenControlUi.h"
#include "OneGenDialogs.h"

#include "OneGenNode.h"
#include "OneGenTypes.h"
#include "OneGenCoordinator.h"
#include "engine/GraphicsFrameAdapter.h"

#include <QFutureWatcher>
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class OneGenSourceController;
class QComboBox;
class QLabel;
class QTimer;
class QToolButton;
class OneGenFeatureTree;
class QTreeWidgetItem;
namespace onegen::worker {
class Coordinator;
class Lease;
}

/** Playground control surface for one OneGen camera session. */
class QOneGenWidget final : public QWidget, protected OneGenControlUi {
  Q_OBJECT
public:
  using DiscoverFn = std::function<std::vector<onegen::DeviceInfo>(
      onegen::worker::Coordinator::ProgressFn)>;
  /** Creates controls with optional consumer-selected favorite storage. */
  QOneGenWidget(std::shared_ptr<onegen::worker::Coordinator> coordinator,
                OneGenSourceController* controller,
                std::vector<onegen::DeviceInfo> devices,
                DiscoverFn discover,
                QWidget* parent = nullptr,
                const QString& settingsFile = {}, const QString& favoritesKey = {});
  ~QOneGenWidget() override;
  /** Restricts the reusable camera panel to its assigned device. */
  void setFixedCamera();
  /** Uses the consumer theme's existing tabbed-tree spacing for a board child panel. */
  void setTabbedCamera();
  /** Reuses the original single-row controls with a consumer-owned device selector. */
  void setExternalSelector(QComboBox* selector, QToolButton* refresh);
  /** Keeps the feature tree and status bar while hiding camera-specific controls. */
  void setFeaturePanelOnly();
  /** Installs Interface settings or updates the same Interface's existing feature editors. */
  void setInterface(const onegen::InterfaceInfo& info, onegen::FeatureNode snapshot);
  /** Reports a camera connection or operation that blocks board reconfiguration. */
  bool cameraInUse() const;
  /** Reports the completed camera connection independently of pending operations. */
  bool cameraConnected() const { return static_cast<bool>(lease_); }
  void setTitleCallback(std::function<void(const QString&)> callback);
  /** @brief Selects the consumer shell for modal dialogs; empty uses plain Qt dialogs. */
  void setDialogShellFactory(OneGenDialogShellFactory factory);
  void prepareForShutdown();

signals:
  /** Announces connection or operation changes without changing tab selection. */
  void stateChanged();

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
  enum class Operation { None, Connect, Disconnect, GrabOne, Start, Stop, Write, Restore, SelectStream };

  void refresh();
  void updateDiscoveryDevices(std::vector<onegen::DeviceInfo> devices);
  void connectCamera();
  void disconnectCamera();
  bool run(Operation operation, std::function<AsyncResult()> work);
  void finishOperation();
  void syncController();
  void adoptConnection(const AsyncResult& result);
  void startGrabOne();
  void finishGrabOne();
  void dropLease();
  /** Drains copied producer notifications before the feature panel is destroyed. */
  void clearFeatureCallback();
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
  std::optional<onegen::InterfaceInfo> interface_;
  std::unique_ptr<GraphicsFrameCallbackGate> invalidation_gate_;
  OneGenSourceController* controller_ = nullptr;
  std::vector<onegen::DeviceInfo> devices_;
  DiscoverFn discover_;
  std::function<void(const QString&)> title_callback_;
  OneGenDialogShellFactory dialog_shell_factory_;
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
