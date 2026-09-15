#pragma once

#include "onegen/types.hpp"

#include <QFutureWatcher>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class FeatureTreeWidget;
class OneGenSourceController;
class QComboBox;
class QLabel;
class QToolButton;
namespace onegen { class Camera; class OneGen; }

/** Minimal Playground control surface for one OneGen camera session. */
class OneGenControlWidget final : public QWidget {
  Q_OBJECT
public:
  OneGenControlWidget(std::shared_ptr<onegen::OneGen> system, onegen::Camera* camera,
                      OneGenSourceController* controller,
                      std::vector<onegen::DeviceInfo> devices, QWidget* parent = nullptr);
  void setTitleCallback(std::function<void(const QString&)> callback);
  void prepareForShutdown();

private:
  struct AsyncResult {
    QString error;
    QString title;
    std::vector<onegen::DeviceInfo> devices;
    std::vector<onegen::DataStreamInfo> streams;
  };
  enum class Operation { None, LoadCti, Refresh, Connect, Disconnect, GrabOne, Start, Trigger, Stop };

  void refresh();
  void connectCamera();
  void disconnectCamera();
  void run(Operation operation, std::function<AsyncResult()> work);
  void finishOperation();
  void setOperationActive(bool active);
  void report(const QString& message);

  std::shared_ptr<onegen::OneGen> system_;
  onegen::Camera* camera_ = nullptr;
  OneGenSourceController* controller_ = nullptr;
  std::vector<onegen::DeviceInfo> devices_;
  std::function<void(const QString&)> title_callback_;
  QComboBox* devices_box_ = nullptr;
  QComboBox* streams_box_ = nullptr;
  QToolButton* load_cti_button_ = nullptr;
  QToolButton* refresh_button_ = nullptr;
  QToolButton* connect_button_ = nullptr;
  QToolButton* grab_one_button_ = nullptr;
  QToolButton* grab_live_button_ = nullptr;
  QToolButton* trigger_button_ = nullptr;
  QLabel* status_ = nullptr;
  FeatureTreeWidget* features_ = nullptr;
  QFutureWatcher<AsyncResult> operation_watcher_;
  Operation operation_ = Operation::None;
  bool shutting_down_ = false;
};
