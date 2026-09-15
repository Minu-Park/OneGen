#include "OneGenControlWidget.h"

#include "FeatureTreeWidget.hpp"
#include "OneGenSourceController.h"
#include "Chrome/ThemedFileDialog.h"
#include "onegen/onegen.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <exception>

OneGenControlWidget::OneGenControlWidget(
    std::shared_ptr<onegen::OneGen> system, onegen::Camera* camera,
    OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices, QWidget* parent)
    : QWidget(parent), system_(std::move(system)), camera_(camera), controller_(controller),
      devices_(std::move(devices))
{
  setMinimumSize(300, 350);
  setStyleSheet(QStringLiteral(
      "OneGenControlWidget QToolButton { min-width: 20px; min-height: 20px; "
      "max-width: 20px; max-height: 20px; padding: 0; border-radius: 8px; }"));
  auto* layout = new QVBoxLayout(this);
  layout->setObjectName(QStringLiteral("DeviceRootLayout"));

  devices_box_ = new QComboBox;
  devices_box_->setMinimumWidth(120);
  devices_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  streams_box_ = new QComboBox;
  streams_box_->setToolTip(QStringLiteral("Data Stream"));
  streams_box_->setEnabled(false);

  refresh_button_ = new QToolButton;
  refresh_button_->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-refresh-48.png")));
  refresh_button_->setIconSize(QSize(16, 16));
  refresh_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  refresh_button_->setToolTip(QStringLiteral("Refresh"));

  connect_button_ = new QToolButton;
  QIcon connect_icon;
  connect_icon.addFile(QStringLiteral(":/Resources/Icons/icons8-connect-48.png"), {},
                       QIcon::Normal, QIcon::Off);
  connect_icon.addFile(QStringLiteral(":/Resources/Icons/icons8-disconnected-48.png"), {},
                       QIcon::Normal, QIcon::On);
  connect_button_->setIcon(connect_icon);
  connect_button_->setIconSize(QSize(16, 16));
  connect_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  connect_button_->setCheckable(true);
  connect_button_->setToolTip(QStringLiteral("Connect / Disconnect"));

  load_cti_button_ = new QToolButton;
  load_cti_button_->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-folder-48.png")));
  load_cti_button_->setIconSize(QSize(16, 16));
  load_cti_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  load_cti_button_->setToolTip(QStringLiteral("Load CTI"));

  grab_one_button_ = new QToolButton;
  grab_one_button_->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-camera-48.png")));
  grab_one_button_->setIconSize(QSize(16, 16));
  grab_one_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  grab_one_button_->setToolTip(QStringLiteral("Grab One"));
  grab_one_button_->setEnabled(false);

  grab_live_button_ = new QToolButton;
  QIcon live_icon;
  live_icon.addFile(QStringLiteral(":/Resources/Icons/icons8-cameras-48.png"), {},
                    QIcon::Normal, QIcon::Off);
  live_icon.addFile(QStringLiteral(":/Resources/Icons/icons8-pause-48.png"), {},
                    QIcon::Normal, QIcon::On);
  grab_live_button_->setIcon(live_icon);
  grab_live_button_->setIconSize(QSize(16, 16));
  grab_live_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  grab_live_button_->setToolTip(QStringLiteral("Grab Live / Stop"));
  grab_live_button_->setCheckable(true);
  grab_live_button_->setEnabled(false);

  trigger_button_ = new QToolButton;
  trigger_button_->setIcon(QIcon(QStringLiteral(":/Resources/Icons/icons8-play-48.png")));
  trigger_button_->setIconSize(QSize(16, 16));
  trigger_button_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  trigger_button_->setToolTip(QStringLiteral("Software Trigger"));
  trigger_button_->setEnabled(false);

  auto* selector_layout = new QHBoxLayout;
  selector_layout->setObjectName(QStringLiteral("DeviceSelectorLayout"));
  selector_layout->addWidget(devices_box_);
  selector_layout->addWidget(streams_box_);
  selector_layout->addWidget(refresh_button_);
  auto* tool_layout = new QHBoxLayout;
  tool_layout->setObjectName(QStringLiteral("DeviceToolLayout"));
  tool_layout->addWidget(connect_button_);
  tool_layout->addWidget(grab_one_button_);
  tool_layout->addWidget(grab_live_button_);
  tool_layout->addWidget(trigger_button_);
  tool_layout->addWidget(load_cti_button_);
  auto* top_layout = new QHBoxLayout;
  top_layout->setObjectName(QStringLiteral("DeviceTopBarLayout"));
  top_layout->addLayout(selector_layout, 1);
  top_layout->addLayout(tool_layout, 0);
  layout->addLayout(top_layout);

  features_ = new FeatureTreeWidget;
  features_->setObjectName(QStringLiteral("CameraFeaturesTree"));
  features_->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
  auto* features_layout = new QVBoxLayout;
  features_layout->setObjectName(QStringLiteral("DeviceTreePanelLayout"));
  features_layout->addWidget(features_);
  layout->addLayout(features_layout);

  auto* status_bar = new QStatusBar;
  status_bar->setObjectName(QStringLiteral("CameraStatusBar"));
  status_bar->setSizeGripEnabled(false);
  status_ = new QLabel(QStringLiteral("Disconnected"));
  status_->setObjectName(QStringLiteral("CameraStatusLabel"));
  status_->setAlignment(Qt::AlignCenter);
  status_bar->addWidget(status_);
  layout->addWidget(status_bar);

  connect(load_cti_button_, &QToolButton::clicked, this, [this] {
    const auto path = ThemedFileDialog::getOpenFileName(
        this, QStringLiteral("Load GenTL Producer"), {},
        QStringLiteral("GenTL Producer (*.cti *.CTI);;All Files (*)"));
    if (path.isEmpty()) return;
    run(Operation::LoadCti, [this, path] {
      AsyncResult result;
      system_->loadCti(path.toStdString());
      result.devices = system_->enumerate();
      return result;
    });
  });
  connect(refresh_button_, &QToolButton::clicked, this, [this] { refresh(); });
  connect(connect_button_, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) connectCamera();
    else disconnectCamera();
  });
  connect(streams_box_, &QComboBox::currentIndexChanged, this, [this](const int index) {
    if (index < 0 || !camera_ || !camera_->is_open()) return;
    try {
      camera_->select_data_stream(streams_box_->currentData().toUInt());
    } catch (const std::exception& error) {
      report(QString::fromLocal8Bit(error.what()));
    }
  });
  connect(grab_one_button_, &QToolButton::clicked, this, [this] {
    if (controller_)
      run(Operation::GrabOne, [this] { controller_->grabOne(); return AsyncResult{}; });
  });
  connect(grab_live_button_, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) run(Operation::Start, [this] { controller_->start(); return AsyncResult{}; });
    else run(Operation::Stop, [this] { controller_->stop(); return AsyncResult{}; });
  });
  connect(trigger_button_, &QToolButton::clicked, this, [this] {
    if (controller_)
      run(Operation::Trigger, [this] { controller_->trigger(); return AsyncResult{}; });
  });
  connect(&operation_watcher_, &QFutureWatcher<AsyncResult>::finished,
          this, &OneGenControlWidget::finishOperation);
  if (controller_)
    connect(controller_, &OneGenSourceController::acquisitionError,
            this, &OneGenControlWidget::report);
  refresh();
}

void OneGenControlWidget::setTitleCallback(std::function<void(const QString&)> callback)
{
  title_callback_ = std::move(callback);
}

void OneGenControlWidget::prepareForShutdown()
{
  shutting_down_ = true;
  title_callback_ = {};
  operation_watcher_.waitForFinished();
  const QSignalBlocker blocker(connect_button_);
  connect_button_->setChecked(false);
  disconnectCamera();
}

void OneGenControlWidget::refresh()
{
  const auto selected = devices_box_->currentData().toString();
  run(Operation::Refresh, [this, selected] {
    AsyncResult result;
    result.title = selected;
    result.devices = system_ && system_->hasCti() ? system_->enumerate() : devices_;
    return result;
  });
}

void OneGenControlWidget::connectCamera()
{
  if (!camera_ || devices_box_->currentIndex() < 0) return;
  const auto query = devices_box_->currentData().toString().toStdString();
  run(Operation::Connect, [this, query] {
    AsyncResult result;
    if (camera_->is_open()) camera_->close();
    camera_->open(query);
    const auto& info = camera_->info();
    const auto name = info.display_name.empty() ? info.id : info.display_name;
    result.title = QString::fromStdString(name);
    result.streams = camera_->data_streams();
    return result;
  });
}

void OneGenControlWidget::disconnectCamera()
{
  if (!camera_) return;
  run(Operation::Disconnect, [this] {
    if (controller_) controller_->stop();
    camera_->close();
    return AsyncResult{};
  });
}

void OneGenControlWidget::run(Operation operation, std::function<AsyncResult()> work)
{
  if (shutting_down_ || operation_watcher_.isRunning()) return;
  operation_ = operation;
  setOperationActive(true);
  operation_watcher_.setFuture(QtConcurrent::run([work = std::move(work)] {
    try {
      return work();
    } catch (const std::exception& error) {
      AsyncResult result;
      result.error = QString::fromLocal8Bit(error.what());
      return result;
    }
  }));
}

void OneGenControlWidget::finishOperation()
{
  if (shutting_down_) return;
  const auto result = operation_watcher_.result();
  const auto operation = operation_;
  operation_ = Operation::None;
  if (!result.error.isEmpty()) {
    report(result.error);
    if (operation == Operation::Connect) {
      const QSignalBlocker blocker(connect_button_);
      connect_button_->setChecked(false);
    }
    if (operation == Operation::Start) {
      const QSignalBlocker blocker(grab_live_button_);
      grab_live_button_->setChecked(false);
    }
    setOperationActive(false);
    return;
  }

  if (operation == Operation::Refresh || operation == Operation::LoadCti) {
    devices_ = result.devices;
    devices_box_->clear();
    for (const auto& device : devices_) {
      const auto label = device.display_name.empty() ? device.id : device.display_name;
      devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
    }
    const auto index = devices_box_->findData(result.title);
    if (index >= 0) devices_box_->setCurrentIndex(index);
  } else if (operation == Operation::Connect) {
    {
      const QSignalBlocker blocker(streams_box_);
      streams_box_->clear();
      for (const auto& stream : result.streams)
        streams_box_->addItem(QString::fromStdString(stream.id), stream.index);
      if (streams_box_->count() > 0) streams_box_->setCurrentIndex(0);
    }
    const auto& info = camera_->info();
    auto camera_name = info.model.empty() ? result.title.toStdString() : info.model;
    if (!info.serial.empty()) camera_name += " (" + info.serial + ")";
    features_->setNodeMaps({
        {"Remote Device", &camera_->features(onegen::Camera::Module::RemoteDevice)},
        {"Device", &camera_->features(onegen::Camera::Module::Device)},
        {"Interface", &camera_->features(onegen::Camera::Module::Interface)}}, camera_name);
    report(QStringLiteral("Connected: %1").arg(result.title));
    if (title_callback_) title_callback_(result.title);
  } else if (operation == Operation::Disconnect) {
    features_->setNodeMap(nullptr);
    streams_box_->clear();
    report(QStringLiteral("Disconnected"));
  } else if (operation == Operation::GrabOne) {
    report(QStringLiteral("Single acquisition completed"));
  } else if (operation == Operation::Start || operation == Operation::Stop) {
    features_->setGrabbing(operation == Operation::Start);
    report(operation == Operation::Start ? QStringLiteral("Live") : QStringLiteral("Connected"));
  }
  setOperationActive(false);
}

void OneGenControlWidget::setOperationActive(bool active)
{
  const bool connected = camera_ && camera_->is_open();
  devices_box_->setEnabled(!active && !connected);
  streams_box_->setEnabled(!active && connected && !grab_live_button_->isChecked()
                           && streams_box_->count() > 1);
  refresh_button_->setEnabled(!active && !connected);
  load_cti_button_->setEnabled(!active && !connected);
  connect_button_->setEnabled(!active && (connected || devices_box_->count() > 0));
  grab_one_button_->setEnabled(!active && connected);
  grab_live_button_->setEnabled(!active && connected);
  trigger_button_->setEnabled(!active && connected && grab_live_button_->isChecked());
}

void OneGenControlWidget::report(const QString& message) { status_->setText(message); }
