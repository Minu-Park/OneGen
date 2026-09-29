#pragma once

#include "QOneGenWidget.h"
#include <QFutureWatcher>
#include <QPointer>
#include <map>
#include <optional>

class QTabWidget;
class QStackedWidget;

/** Presents producer-scoped Interfaces and Devices without inferring hardware types. */
class QOneGenDeviceWidget final : public QWidget {
  Q_OBJECT
public:
  QOneGenDeviceWidget(std::shared_ptr<onegen::worker::Coordinator> coordinator,
      OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices,
      QOneGenWidget::DiscoverFn discover, QWidget* parent = nullptr);
  ~QOneGenDeviceWidget() override;
  /** Drains camera operations before releasing this widget's Interface session. */
  void prepareForShutdown();
  /** Publishes device titles without changing connection state on tab selection. */
  void setTitleCallback(std::function<void(const QString&)> callback);

private:
  struct Target {
    std::string key;
    QString label;
    onegen::DeviceInfo camera;
    std::optional<onegen::InterfaceInfo> interface;
  };
  struct Result {
    QString error;
    std::vector<onegen::DeviceInfo> devices;
    std::vector<onegen::InterfaceInfo> interfaces;
    std::optional<onegen::InterfaceInfo> interface;
    onegen::FeatureNode features;
  };
  enum class Operation { Discover, Connect, Rescan, Disconnect };
  void refresh();
  void selectTarget();
  void connectInterface();
  void disconnectInterface();
  void finish();
  void finishDiscovery();
  void updateTargets(bool complete = true);
  void updateCameraTabs(const std::vector<onegen::DeviceInfo>& devices);
  void showInterface(const Result& result);
  void clearTabs();
  void updateControls();

  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  OneGenSourceController* controller_;
  QOneGenWidget::DiscoverFn discover_;
  std::vector<onegen::DeviceInfo> devices_;
  std::vector<onegen::InterfaceInfo> interfaces_;
  std::vector<Target> targets_;
  std::string displayed_target_;
  std::optional<onegen::InterfaceInfo> interface_;
  std::map<std::string, QOneGenWidget*> cameras_;
  QOneGenWidget* interface_settings_ = nullptr;
  QOneGenWidget* direct_camera_ = nullptr;
  QWidget* selector_bar_;
  QStackedWidget* content_;
  QComboBox* devices_box_;
  QToolButton* connect_button_;
  QToolButton* refresh_button_;
  QTabWidget* tabs_;
  QFutureWatcher<Result> watcher_;
  QFutureWatcher<Result> discovery_watcher_;
  Operation operation_ = Operation::Discover;
  bool shutting_down_ = false;
  std::function<void(const QString&)> title_callback_;
};
