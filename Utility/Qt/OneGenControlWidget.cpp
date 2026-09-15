#include "OneGenControlWidget.h"

#include "FeatureTreeWidget.hpp"
#include "OneGenCoordinator.h"
#include "OneGenSourceController.h"

#include <QComboBox>
#include <QDebug>
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
    std::shared_ptr<onegen::worker::Coordinator> coordinator,
    OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices,
    std::function<std::vector<onegen::DeviceInfo>()> discover, QWidget* parent)
    : QWidget(parent), coordinator_(std::move(coordinator)), controller_(controller),
      devices_(std::move(devices)), discover_(std::move(discover))
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

  connect(refresh_button_, &QToolButton::clicked, this, [this] { refresh(); });
  connect(connect_button_, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) connectCamera();
    else disconnectCamera();
  });
  connect(streams_box_, &QComboBox::currentIndexChanged, this, [this](const int index) {
    if (index < 0 || !lease_) return;
    try {
      lease_->select_data_stream(streams_box_->currentData().toUInt());
    } catch (const std::exception& error) {
      qWarning().noquote() << QStringLiteral("OneGen stream selection failed: %1")
                                  .arg(QString::fromLocal8Bit(error.what()));
    }
  });
  connect(grab_one_button_, &QToolButton::clicked, this, [this] {
    if (!controller_) return;
    run(Operation::GrabOne, [this] {
      AsyncResult result;
      try {
        controller_->grabOne();
      } catch (const std::exception& error) {
        result.error = QString::fromLocal8Bit(error.what());
      }
      try {
        if (lease_) result.features = lease_->describe_features();
      } catch (const std::exception& error) {
        qWarning().noquote() << QStringLiteral("OneGen feature snapshot failed: %1")
                                    .arg(QString::fromLocal8Bit(error.what()));
      }
      return result;
    });
  });
  connect(grab_live_button_, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) {
      run(Operation::Start, [this] {
        AsyncResult result;
        controller_->start();
        try {
          result.features = lease_->describe_features();
        } catch (const std::exception& error) {
          qWarning().noquote() << QStringLiteral("OneGen feature snapshot failed: %1")
                                      .arg(QString::fromLocal8Bit(error.what()));
        }
        return result;
      });
    } else {
      run(Operation::Stop, [this] {
        AsyncResult result;
        controller_->stop();
        try {
          result.features = lease_->describe_features();
        } catch (const std::exception& error) {
          qWarning().noquote() << QStringLiteral("OneGen feature snapshot failed: %1")
                                      .arg(QString::fromLocal8Bit(error.what()));
        }
        return result;
      });
    }
  });
  connect(&operation_watcher_, &QFutureWatcher<AsyncResult>::finished,
          this, &OneGenControlWidget::finishOperation);
  for (const auto& device : devices_) {
    const auto label = device.display_name.empty() ? device.id : device.display_name;
    devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
  }
  setOperationActive(false);
  if (devices_.empty()) report(QStringLiteral("No cameras. Refresh to search."));
  else report(QStringLiteral("%1 camera(s)").arg(devices_.size()));
}

void OneGenControlWidget::setTitleCallback(std::function<void(const QString&)> callback)
{
  title_callback_ = std::move(callback);
}

void OneGenControlWidget::prepareForShutdown()
{
  title_callback_ = {};
  operation_watcher_.waitForFinished();
  shutting_down_ = true;
  const QSignalBlocker blocker(connect_button_);
  connect_button_->setChecked(false);
  if (controller_) {
    try {
      controller_->stop();
    } catch (...) {
    }
    controller_->setLease(nullptr);
  }
  if (features_) features_->setFeatureModules({}, {}, {}, {});
  lease_.reset();
}

void OneGenControlWidget::refresh()
{
  report(QStringLiteral("Searching..."));
  const auto selected = devices_box_->currentData().toString();
  if (!run(Operation::Refresh, [this, selected] {
        AsyncResult result;
        result.title = selected;
        result.devices = discover_ ? discover_() : devices_;
        return result;
      })) {
    report(QStringLiteral("Busy"));
  }
}

void OneGenControlWidget::connectCamera()
{
  const auto revert = [this] {
    const QSignalBlocker blocker(connect_button_);
    connect_button_->setChecked(false);
  };
  if (!coordinator_ || devices_box_->currentIndex() < 0) {
    qWarning().noquote() << QStringLiteral("OneGen connect skipped: no camera selected");
    report(QStringLiteral("No camera selected"));
    revert();
    return;
  }
  const auto selected = devices_.at(static_cast<std::size_t>(devices_box_->currentIndex()));
  qInfo().noquote() << QStringLiteral("OneGen connect: id=%1 cti=%2")
                          .arg(QString::fromStdString(selected.id),
                               QString::fromStdString(selected.cti_path));
  report(QStringLiteral("Connecting..."));
  if (!run(Operation::Connect, [this, selected] {
        AsyncResult result;
        result.lease = std::shared_ptr<onegen::worker::Lease>(coordinator_->open(selected).release());
        const auto& info = result.lease->info();
        const auto name = info.display_name.empty() ? info.id : info.display_name;
        result.title = QString::fromStdString(name);
        result.streams = result.lease->data_streams();
        try {
          result.features = result.lease->describe_features();
        } catch (const std::exception& error) {
          qWarning().noquote() << QStringLiteral("OneGen feature tree: %1")
                                     .arg(QString::fromLocal8Bit(error.what()));
        }
        return result;
      })) {
    revert();
    report(QStringLiteral("Busy"));
  }
}

void OneGenControlWidget::disconnectCamera()
{
  if (!run(Operation::Disconnect, [this] {
        if (controller_) {
          controller_->stop();
          controller_->setLease(nullptr);
        }
        return AsyncResult{};
      })) {
    const QSignalBlocker blocker(connect_button_);
    connect_button_->setChecked(true);
    report(QStringLiteral("Busy"));
  }
}

bool OneGenControlWidget::run(Operation operation, std::function<AsyncResult()> work)
{
  if (shutting_down_ || operation_watcher_.isRunning()) {
    qWarning().noquote() << QStringLiteral("OneGen ignored operation while busy");
    return false;
  }
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
  return true;
}

void OneGenControlWidget::finishOperation()
{
  if (shutting_down_) return;
  const auto result = operation_watcher_.result();
  const auto operation = operation_;
  operation_ = Operation::None;
  if (!result.error.isEmpty()) {
    const auto action = operation == Operation::GrabOne ? QStringLiteral("single acquisition")
        : operation == Operation::Start ? QStringLiteral("live start")
        : operation == Operation::Stop ? QStringLiteral("live stop")
        : operation == Operation::Connect ? QStringLiteral("connect")
        : QStringLiteral("control");
    qWarning().noquote() << QStringLiteral("OneGen %1 failed: %2").arg(action, result.error);
    if (operation == Operation::Connect) {
      const QSignalBlocker blocker(connect_button_);
      connect_button_->setChecked(false);
    }
    if (operation == Operation::Start) {
      const QSignalBlocker blocker(grab_live_button_);
      grab_live_button_->setChecked(false);
    }
    if (operation == Operation::GrabOne && !result.features.empty())
      features_->setFeatureSnapshot(result.features);
    report(lease_ ? (controller_ && controller_->isGrabbing() ? QStringLiteral("Live")
                                                               : QStringLiteral("Connected"))
                  : QStringLiteral("Disconnected"));
    setOperationActive(false);
    return;
  }

  if (operation == Operation::Refresh) {
    devices_ = result.devices;
    devices_box_->clear();
    for (const auto& device : devices_) {
      const auto label = device.display_name.empty() ? device.id : device.display_name;
      devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
    }
    const auto index = devices_box_->findData(result.title);
    if (index >= 0) devices_box_->setCurrentIndex(index);
    report(devices_.empty() ? QStringLiteral("No cameras")
                            : QStringLiteral("%1 camera(s)").arg(devices_.size()));
  } else if (operation == Operation::Connect) {
    {
      const QSignalBlocker blocker(streams_box_);
      streams_box_->clear();
      for (const auto& stream : result.streams)
        streams_box_->addItem(QString::fromStdString(stream.id), stream.index);
      if (streams_box_->count() > 0) streams_box_->setCurrentIndex(0);
    }
    lease_ = result.lease;
    if (controller_) controller_->setLease(lease_.get());
    const auto& info = lease_->info();
    auto camera_name = info.model.empty() ? result.title.toStdString() : info.model;
    if (!info.serial.empty()) camera_name += " (" + info.serial + ")";
    features_->setFeatureModules(
        result.features,
        [this](const int module, const std::string_view name) {
          return lease_->read_node(module, name);
        },
        [this](const int module, const std::string_view name, const std::string_view op,
               const std::string_view value) {
          return lease_->write_node(module, name, op, value);
        },
        camera_name);
    lease_->set_invalidate_callback([this](const int module, const std::string_view name) {
      if (features_) features_->invalidate(module, name);
    });
    report(QStringLiteral("Connected: %1").arg(result.title));
    if (title_callback_) title_callback_(result.title);
  } else if (operation == Operation::Disconnect) {
    if (lease_) lease_->set_invalidate_callback({});
    features_->setFeatureModules({}, {}, {}, {});
    streams_box_->clear();
    lease_.reset();
    report(QStringLiteral("Disconnected"));
  } else if (operation == Operation::GrabOne) {
    if (!result.features.empty()) features_->setFeatureSnapshot(result.features);
    report(QStringLiteral("Single acquisition completed"));
  } else if (operation == Operation::Start || operation == Operation::Stop) {
    features_->setGrabbing(operation == Operation::Start);
    if (!result.features.empty()) features_->setFeatureSnapshot(result.features);
    report(operation == Operation::Start ? QStringLiteral("Live") : QStringLiteral("Connected"));
  }
  setOperationActive(false);
}

void OneGenControlWidget::setOperationActive(bool active)
{
  const bool connected = static_cast<bool>(lease_);
  devices_box_->setEnabled(!active && !connected);
  streams_box_->setEnabled(!active && connected && !grab_live_button_->isChecked()
                           && streams_box_->count() > 1);
  refresh_button_->setEnabled(!active && !connected);
  connect_button_->setEnabled(!active && (connected || devices_box_->count() > 0));
  grab_one_button_->setEnabled(!active && connected);
  grab_live_button_->setEnabled(!active && connected);
  features_->setEnabled(!active && connected);
}

void OneGenControlWidget::report(const QString& message) { status_->setText(message); }
