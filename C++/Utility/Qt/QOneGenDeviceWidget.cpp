#include "QOneGenDeviceWidget.h"
#include "OneGenSourceController.h"

#include <QComboBox>
#include <QDebug>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <set>

namespace {
std::string cameraKey(const onegen::DeviceInfo& device) {
  return device.cti_path + '\0' + device.interface_id + '\0' + device.id;
}
QString cameraLabel(const onegen::DeviceInfo& device) {
  return QString::fromStdString(device.display_name.empty() ? device.id : device.display_name);
}
}

QOneGenDeviceWidget::QOneGenDeviceWidget(std::shared_ptr<onegen::worker::Coordinator> coordinator,
    OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices,
    QOneGenWidget::DiscoverFn discover, QWidget* parent)
    : QWidget(parent), coordinator_(std::move(coordinator)), controller_(controller),
      discover_(std::move(discover)), devices_(std::move(devices))
{
  setMinimumSize(300, 350);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  selector_bar_ = new QFrame(this);
  selector_bar_->setObjectName(QStringLiteral("OneGenSelectorBar"));
  auto* top = new QHBoxLayout(selector_bar_);
  top->setObjectName(QStringLiteral("DeviceTopBarLayout"));
  devices_box_ = new QComboBox;
  devices_box_->setObjectName(QStringLiteral("OneGenDeviceSelector"));
  devices_box_->setMinimumWidth(120);
  devices_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  devices_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect_button_ = new QToolButton;
  QIcon connectionIcon;
  connectionIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-connect-48.png"), {}, QIcon::Normal, QIcon::Off);
  connectionIcon.addFile(QStringLiteral(":/Resources/Icons/icons8-disconnected-48.png"), {}, QIcon::Normal, QIcon::On);
  connect_button_->setIcon(connectionIcon);
  connect_button_->setCheckable(true);
  connect_button_->setToolTip(QStringLiteral("Connect / Disconnect Interface"));
  refresh_button_ = new QToolButton;
  refresh_button_->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
  refresh_button_->setToolTip(QStringLiteral("Refresh"));
  for (auto* button : {connect_button_, refresh_button_}) {
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
  }
  auto* selector = new QHBoxLayout;
  selector->setObjectName(QStringLiteral("DeviceSelectorLayout"));
  selector->addWidget(devices_box_);
  selector->addWidget(refresh_button_);
  auto* tools = new QHBoxLayout;
  tools->setObjectName(QStringLiteral("DeviceToolLayout"));
  tools->addWidget(connect_button_);
  top->addLayout(selector, 1);
  top->addLayout(tools);
  layout->addWidget(selector_bar_);
  content_ = new QStackedWidget;
  tabs_ = new QTabWidget;
  tabs_->setObjectName(QStringLiteral("OneGenDeviceTabs"));
  tabs_->setProperty("tabRole", QStringLiteral("DeviceControls"));
  content_->addWidget(tabs_);
  layout->addWidget(content_, 1);
  connect(devices_box_, &QComboBox::currentIndexChanged, this, [this] { selectTarget(); });
  connect(refresh_button_, &QToolButton::clicked, this, [this] { refresh(); });
  connect(connect_button_, &QToolButton::toggled, this, [this](bool connected) {
    if (connected) connectInterface(); else disconnectInterface();
  });
  connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] { finish(); });
  connect(&discovery_watcher_, &QFutureWatcher<Result>::finished, this, [this] { finishDiscovery(); });
  interfaces_ = coordinator_->interfaces();
  updateTargets();
  if (discover_) QTimer::singleShot(0, this, [this] { refresh(); });
}

QOneGenDeviceWidget::~QOneGenDeviceWidget() { prepareForShutdown(); }

void QOneGenDeviceWidget::setTitleCallback(std::function<void(const QString&)> callback) {
  title_callback_ = std::move(callback);
}

void QOneGenDeviceWidget::prepareForShutdown() {
  if (shutting_down_) return;
  shutting_down_ = true;
  title_callback_ = {};
  watcher_.waitForFinished();
  discovery_watcher_.waitForFinished();
  // A completed open may not yet have delivered its GUI completion callback.
  if (operation_ == Operation::Connect && !interface_ && watcher_.future().isValid())
    interface_ = watcher_.result().interface;
  clearTabs();
  if (interface_) {
    try { coordinator_->close_interface(*interface_); }
    catch (const std::exception& error) { qWarning() << "[GenICam] interface release failed:" << error.what(); }
    interface_.reset();
  }
}

void QOneGenDeviceWidget::clearTabs() {
  displayed_target_.clear();
  if (direct_camera_) {
    // Restore consumer controls before their temporary camera parent is deleted.
    auto* row = selector_bar_->findChild<QHBoxLayout*>(QStringLiteral("DeviceSelectorLayout"));
    devices_box_->setParent(selector_bar_);
    refresh_button_->setParent(selector_bar_);
    row->insertWidget(0, devices_box_, 1);
    row->insertWidget(1, refresh_button_);
    devices_box_->show();
    refresh_button_->show();
    content_->removeWidget(direct_camera_);
  }
  for (const auto& [id, widget] : cameras_) widget->prepareForShutdown();
  cameras_.clear();
  if (interface_settings_) {
    interface_settings_->prepareForShutdown();
    if (content_->indexOf(interface_settings_) >= 0) {
      content_->removeWidget(interface_settings_);
      delete interface_settings_;
    }
  }
  interface_settings_ = nullptr;
  delete direct_camera_;
  direct_camera_ = nullptr;
  while (tabs_->count()) {
    auto* widget = tabs_->widget(0);
    tabs_->removeTab(0);
    delete widget;
  }
  controller_->clearCameraControllers();
  content_->setCurrentWidget(tabs_);
  selector_bar_->show();
  tabs_->hide();
}

void QOneGenDeviceWidget::refresh() {
  if (shutting_down_ || watcher_.isRunning()) return;
  if (!interface_ && discovery_watcher_.isRunning()) return;
  if (!interface_ && std::any_of(cameras_.begin(), cameras_.end(),
      [](const auto& item) { return item.second->cameraInUse(); })) return;
  operation_ = interface_ ? Operation::Rescan : Operation::Discover;
  const auto selectedInterface = interface_;
  const QPointer<QOneGenDeviceWidget> self(this);
  auto future = QtConcurrent::run([this, selectedInterface, self] {
    Result result;
    try {
      if (selectedInterface) {
        result.devices = coordinator_->rescan_interface(*selectedInterface);
        result.features = coordinator_->interface_features(*selectedInterface);
      } else {
        result.devices = discover_ ? discover_([self](const auto& update) {
          if (!self) return;
          QMetaObject::invokeMethod(self, [self, update] {
            if (!self || self->shutting_down_) return;
            self->devices_ = update.devices;
            self->interfaces_ = self->coordinator_->interfaces();
            if (!self->interface_ && !self->watcher_.isRunning()
                && std::none_of(self->cameras_.begin(), self->cameras_.end(),
                    [](const auto& item) { return item.second->cameraInUse(); })) self->updateTargets();
          }, Qt::QueuedConnection);
        }) : devices_;
        result.interfaces = coordinator_->interfaces();
      }
    } catch (const std::exception& error) { result.error = QString::fromLocal8Bit(error.what()); }
    return result;
  });
  if (selectedInterface) watcher_.setFuture(future);
  else discovery_watcher_.setFuture(future);
  updateControls();
}

void QOneGenDeviceWidget::finishDiscovery() {
  if (shutting_down_) return;
  const auto result = discovery_watcher_.result();
  if (!result.error.isEmpty()) qWarning().noquote() << "[GenICam] discovery failed:" << result.error;
  else {
    devices_ = result.devices;
    interfaces_ = result.interfaces;
    if (!interface_ && !watcher_.isRunning() && std::none_of(cameras_.begin(), cameras_.end(),
        [](const auto& item) { return item.second->cameraInUse(); })) updateTargets();
  }
  updateControls();
}

void QOneGenDeviceWidget::updateTargets() {
  const auto selected = devices_box_->currentData().toString();
  targets_.clear();
  for (const auto& device : devices_) {
    targets_.push_back({"Device|" + cameraKey(device), cameraLabel(device), device, {}});
  }
  for (const auto& info : interfaces_) {
    targets_.push_back({"Interface|" + info.cti_path + '\0' + info.id,
        QString::fromStdString(info.display_name.empty() ? info.id : info.display_name),
        {}, info});
  }
  {
    const QSignalBlocker blocker(devices_box_);
    devices_box_->clear();
    for (const auto& target : targets_) {
      const auto role = target.interface ? QStringLiteral("Interface") : QStringLiteral("Device");
      devices_box_->addItem(target.label, QString::fromStdString(target.key));
      devices_box_->setItemData(devices_box_->count() - 1, role + ": " + target.label, Qt::AccessibleTextRole);
      const auto producer = target.interface ? target.interface->cti_path : target.camera.cti_path;
      const auto interfaceId = target.interface ? target.interface->id : target.camera.interface_id;
      devices_box_->setItemData(devices_box_->count() - 1,
          QStringLiteral("%1: %2\nProducer: %3\nInterface: %4").arg(role, target.label,
              QString::fromStdString(producer), QString::fromStdString(interfaceId)), Qt::ToolTipRole);
    }
    const auto index = devices_box_->findData(selected);
    if (index >= 0) devices_box_->setCurrentIndex(index);
  }
  selectTarget();
}

void QOneGenDeviceWidget::selectTarget() {
  if (interface_ || watcher_.isRunning()) return;
  const auto index = devices_box_->currentIndex();
  const auto key = index >= 0 ? targets_[index].key : std::string{};
  if ((direct_camera_ || interface_settings_) && displayed_target_ == key) {
    updateControls();
    return;
  }
  clearTabs();
  displayed_target_ = key;
  if (index >= 0 && !targets_[index].interface) {
    const auto& device = targets_[index].camera;
    auto* source = controller_->cameraController(cameraKey(device), cameraLabel(device).toStdString());
    direct_camera_ = new QOneGenWidget(coordinator_, source, {device}, {}, content_);
    direct_camera_->setExternalSelector(devices_box_, refresh_button_);
    direct_camera_->setTitleCallback([this](const QString& title) {
      if (title_callback_) title_callback_(title);
    });
    cameras_[cameraKey(device)] = direct_camera_;
    connect(direct_camera_, &QOneGenWidget::stateChanged, this, [this] { updateControls(); });
    content_->addWidget(direct_camera_);
    content_->setCurrentWidget(direct_camera_);
    selector_bar_->hide();
  } else {
    interface_settings_ = new QOneGenWidget(coordinator_, nullptr, {}, {}, content_);
    connect(interface_settings_, &QOneGenWidget::stateChanged, this, [this] { updateControls(); });
    interface_settings_->setFeaturePanelOnly();
    content_->addWidget(interface_settings_);
    content_->setCurrentWidget(interface_settings_);
  }
  updateControls();
}

void QOneGenDeviceWidget::connectInterface() {
  const auto index = devices_box_->currentIndex();
  if (index < 0 || watcher_.isRunning() || interface_ || !targets_[index].interface) { updateControls(); return; }
  const auto info = *targets_[index].interface;
  operation_ = Operation::Connect;
  watcher_.setFuture(QtConcurrent::run([this, info] {
    Result result;
    bool opened = false;
    try {
      coordinator_->open_interface(info);
      opened = true;
      result.features = coordinator_->interface_features(info);
      result.devices = coordinator_->rescan_interface(info);
      result.interface = info;
      result.error.clear();
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
      qWarning().noquote() << "[GenICam] Interface connection failed:" << QString::fromStdString(info.cti_path) << result.error;
      if (opened) { try { coordinator_->close_interface(info); } catch (...) {} }
    }
    return result;
  }));
  updateControls();
}

void QOneGenDeviceWidget::disconnectInterface() {
  if (!interface_ || watcher_.isRunning()) { updateControls(); return; }
  const auto info = *interface_;
  // Each child drains only its own acquisition and device before the shared Interface closes.
  clearTabs();
  operation_ = Operation::Disconnect;
  watcher_.setFuture(QtConcurrent::run([this, info] {
    Result result;
    try { coordinator_->close_interface(info); }
    catch (const std::exception& error) { result.error = QString::fromLocal8Bit(error.what()); }
    return result;
  }));
  updateControls();
}

void QOneGenDeviceWidget::updateCameraTabs(const std::vector<onegen::DeviceInfo>& devices) {
  std::set<std::string> present;
  for (auto device : devices) {
    if (interface_ && (device.cti_path != interface_->cti_path || device.interface_id != interface_->id)) continue;
    if (interface_) {
      device.cti_candidates = {interface_->cti_path}; device.cti_candidate_ids = {device.id};
      device.cti_candidate_interfaces = {interface_->id};
    }
    const auto key = cameraKey(device);
    const auto label = cameraLabel(device);
    present.insert(key);
    if (const auto found = cameras_.find(key); found != cameras_.end()) {
      tabs_->setTabText(tabs_->indexOf(found->second), label);
      continue;
    }
    auto* source = controller_->cameraController(key, label.toStdString());
    auto* camera = new QOneGenWidget(coordinator_, source, {device}, {}, tabs_);
    camera->setTabbedCamera();
    camera->setTitleCallback([this](const QString& title) { if (!interface_ && title_callback_) title_callback_(title); });
    cameras_[key] = camera;
    connect(camera, &QOneGenWidget::stateChanged, this, [this] { updateControls(); });
    tabs_->addTab(camera, label);
  }
  for (auto it = cameras_.begin(); it != cameras_.end();) {
    if (present.count(it->first)) { ++it; continue; }
    auto* camera = it->second;
    const auto index = tabs_->indexOf(camera);
    if (camera->cameraInUse()) {
      if (!tabs_->tabText(index).endsWith(QStringLiteral(" (not discovered)")))
        tabs_->setTabText(index, tabs_->tabText(index) + QStringLiteral(" (not discovered)"));
      ++it;
    } else {
      const auto id = it->first;
      it = cameras_.erase(it);
      tabs_->removeTab(index);
      delete camera;
      controller_->removeCameraController(id);
    }
  }
}

void QOneGenDeviceWidget::finish() {
  if (shutting_down_) return;
  const auto result = watcher_.result();
  if (!result.error.isEmpty()) qWarning().noquote() << "[GenICam] device operation failed:" << result.error;
  if (!result.error.isEmpty() && operation_ == Operation::Disconnect && interface_) showInterface(result);
  if (result.error.isEmpty()) {
    if (operation_ == Operation::Discover) {
      devices_ = result.devices;
      interfaces_ = result.interfaces;
      updateTargets();
    } else if (operation_ == Operation::Connect && result.interface) {
      interface_ = result.interface;
      showInterface(result);
      if (title_callback_) title_callback_(QString::fromStdString(interface_->display_name.empty() ? interface_->id : interface_->display_name));
      qInfo() << "[GenICam] interface connected; cameras=" << result.devices.size();
    } else if (operation_ == Operation::Rescan) {
      showInterface(result);
      qInfo() << "[GenICam] interface rescan completed; cameras=" << result.devices.size();
    } else if (operation_ == Operation::Disconnect) {
      interface_.reset();
      qInfo() << "[GenICam] interface disconnected";
      selectTarget();
    }
  }
  updateControls();
}

void QOneGenDeviceWidget::showInterface(const Result& result) {
  if (!interface_settings_) {
    interface_settings_ = new QOneGenWidget(coordinator_, nullptr, {}, {}, tabs_);
    connect(interface_settings_, &QOneGenWidget::stateChanged, this, [this] { updateControls(); });
  }
  interface_settings_->setInterface(*interface_, result.features);
  if (tabs_->indexOf(interface_settings_) < 0) {
    content_->removeWidget(interface_settings_);
    tabs_->addTab(interface_settings_, QStringLiteral("Interface"));
  }
  content_->setCurrentWidget(tabs_);
  tabs_->show();
  updateCameraTabs(result.devices);
}

void QOneGenDeviceWidget::updateControls() {
  if (shutting_down_) return;
  const bool busy = watcher_.isRunning() || (interface_settings_ && interface_settings_->cameraInUse());
  const bool cameraBusy = std::any_of(cameras_.begin(), cameras_.end(),
      [](const auto& item) { return item.second->cameraInUse(); });
  devices_box_->setEnabled(!busy && !interface_ && !cameraBusy);
  refresh_button_->setEnabled(!busy && (interface_ || (!cameraBusy && !discovery_watcher_.isRunning())));
  refresh_button_->setToolTip(interface_ ? QStringLiteral("Rescan Devices") : QStringLiteral("Refresh devices"));
  const auto index = devices_box_->currentIndex();
  devices_box_->setToolTip(devices_box_->currentData(Qt::ToolTipRole).toString());
  const bool selectedBoard = index >= 0 && targets_[index].interface.has_value();
  connect_button_->setVisible(selectedBoard || interface_.has_value());
  connect_button_->setEnabled(!busy && (selectedBoard || interface_.has_value()));
  { const QSignalBlocker blocker(connect_button_); connect_button_->setChecked(interface_.has_value()); }
}
