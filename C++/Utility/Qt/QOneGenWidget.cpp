#include "QOneGenWidget.h"

#include "OneGenCoordinator.h"
#include "OneGenSourceController.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
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

QOneGenWidget::QOneGenWidget(
    std::shared_ptr<onegen::worker::Coordinator> coordinator,
    OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices,
    DiscoverFn discover, QWidget* parent)
    : QWidget(parent), coordinator_(std::move(coordinator)), controller_(controller),
      devices_(std::move(devices)), discover_(std::move(discover))
{
  setMinimumSize(300, 350);
  setStyleSheet(QStringLiteral(
      "QOneGenWidget QToolButton { min-width: 20px; min-height: 20px; "
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

  features_ = new QTreeWidget;
  features_->setObjectName(QStringLiteral("CameraFeaturesTree"));
  features_->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
  features_->setColumnCount(2);
  features_->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
  features_->setUniformRowHeights(true);
  features_->setRootIsDecorated(true);
  feature_drain_timer_ = new QTimer(this);
  feature_drain_timer_->setSingleShot(true);
  feature_drain_timer_->setInterval(50);
  connect(feature_drain_timer_, &QTimer::timeout, this, &QOneGenWidget::drainFeatureInvalidates);
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
      return result;
    });
  });
  connect(grab_live_button_, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) {
      run(Operation::Start, [this] {
        AsyncResult result;
        controller_->start();
        return result;
      });
    } else {
      run(Operation::Stop, [this] {
        AsyncResult result;
        controller_->stop();
        return result;
      });
    }
  });
  connect(&operation_watcher_, &QFutureWatcher<AsyncResult>::finished,
          this, &QOneGenWidget::finishOperation);
  connect(&discovery_watcher_, &QFutureWatcher<AsyncResult>::finished, this, [this] {
    if (shutting_down_) return;
    const auto result = discovery_watcher_.result();
    if (result.error.isEmpty()) updateDiscoveryDevices(result.devices);
    if (!result.error.isEmpty())
      qWarning().noquote() << QStringLiteral("OneGen discovery failed: %1").arg(result.error);
    setOperationActive(operation_watcher_.isRunning());
  });
  for (const auto& device : devices_) {
    const auto label = device.display_name.empty() ? device.id : device.display_name;
    devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
  }
  setOperationActive(false);
  if (discover_) QTimer::singleShot(0, this, &QOneGenWidget::refresh);
}

void QOneGenWidget::setTitleCallback(std::function<void(const QString&)> callback)
{
  title_callback_ = std::move(callback);
}

void QOneGenWidget::prepareForShutdown()
{
  title_callback_ = {};
  shutting_down_ = true;
  discovery_watcher_.waitForFinished();
  operation_watcher_.waitForFinished();
  const QSignalBlocker blocker(connect_button_);
  connect_button_->setChecked(false);
  if (controller_) {
    try {
      controller_->stop();
    } catch (...) {
    }
    controller_->setLease(nullptr);
  }
  setFeatureModules({}, {}, {}, {});
  lease_.reset();
}

void QOneGenWidget::refresh()
{
  if (shutting_down_ || discovery_watcher_.isRunning() || lease_
      || operation_watcher_.isRunning() || !discover_) return;
  qInfo() << "OneGen discovery started";
  QPointer<QOneGenWidget> self(this);
  discovery_watcher_.setFuture(QtConcurrent::run([discover = discover_, self] {
    AsyncResult result;
    try {
      result.devices = discover([self](const onegen::worker::DiscoveryResult& update) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, update] {
          if (!self || self->shutting_down_) return;
          self->updateDiscoveryDevices(update.devices);
          qInfo().noquote() << QStringLiteral("OneGen discovery: %1 camera(s), producers %2/%3")
              .arg(update.devices.size()).arg(update.finished_producers).arg(update.total_producers);
          self->setOperationActive(self->operation_watcher_.isRunning());
        }, Qt::QueuedConnection);
      });
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
    }
    return result;
  }));
  setOperationActive(false);
}

void QOneGenWidget::updateDiscoveryDevices(std::vector<onegen::DeviceInfo> devices)
{
  devices_ = std::move(devices);
  if (lease_) return;
  const auto selected = devices_box_->currentData();
  const QSignalBlocker blocker(devices_box_);
  devices_box_->clear();
  for (const auto& device : devices_) {
    const auto label = device.display_name.empty() ? device.id : device.display_name;
    devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
  }
  const auto index = devices_box_->findData(selected);
  if (index >= 0) devices_box_->setCurrentIndex(index);
}

void QOneGenWidget::connectCamera()
{
  const auto revert = [this] {
    const QSignalBlocker blocker(connect_button_);
    connect_button_->setChecked(false);
  };
  if (!coordinator_ || devices_box_->currentIndex() < 0) {
    qWarning().noquote() << QStringLiteral("OneGen connect skipped: no camera selected");
    revert();
    return;
  }
  const auto selected = devices_.at(static_cast<std::size_t>(devices_box_->currentIndex()));
  qInfo().noquote() << QStringLiteral("OneGen connect: id=%1 cti=%2")
                          .arg(QString::fromStdString(selected.id),
                               QString::fromStdString(selected.cti_path));
  connection_attempted_ = true;
  updateStatus();
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
  }
}

void QOneGenWidget::disconnectCamera()
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
  }
}

bool QOneGenWidget::run(Operation operation, std::function<AsyncResult()> work)
{
  if (shutting_down_ || operation_watcher_.isRunning()) {
    qWarning().noquote() << QStringLiteral("OneGen ignored operation while busy");
    return false;
  }
  operation_ = operation;
  setOperationActive(true);
  operation_watcher_.setFuture(QtConcurrent::run([this, operation, work = std::move(work)] {
    AsyncResult result;
    try {
      result = work();
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
    }
    if (lease_ && (operation == Operation::GrabOne || operation == Operation::Start
                   || operation == Operation::Stop)) {
      try {
        result.features = lease_->describe_features();
      } catch (const std::exception& error) {
        qWarning().noquote() << QStringLiteral("OneGen feature tree refresh failed: %1")
                                    .arg(QString::fromLocal8Bit(error.what()));
      }
    }
    return result;
  }));
  return true;
}

void QOneGenWidget::finishOperation()
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
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    setOperationActive(false);
    return;
  }

  if (operation == Operation::Connect) {
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
    setFeatureModules(
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
      invalidateFeature(module, name);
    });
    qInfo().noquote() << QStringLiteral("OneGen connected: %1").arg(result.title);
    if (title_callback_) title_callback_(result.title);
  } else if (operation == Operation::Disconnect) {
    if (lease_) lease_->set_invalidate_callback({});
    setFeatureModules({}, {}, {}, {});
    streams_box_->clear();
    lease_.reset();
    updateDiscoveryDevices(devices_);
    qInfo() << "OneGen disconnected";
  } else if (operation == Operation::GrabOne) {
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo() << "OneGen single acquisition completed";
  } else if (operation == Operation::Start || operation == Operation::Stop) {
    setFeatureGrabbing(operation == Operation::Start);
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo() << (operation == Operation::Start ? "OneGen live started" : "OneGen live stopped");
  }
  setOperationActive(false);
}

void QOneGenWidget::setOperationActive(bool active)
{
  const bool connected = static_cast<bool>(lease_);
  devices_box_->setEnabled(!active && !connected);
  streams_box_->setEnabled(!active && connected && !grab_live_button_->isChecked()
                           && streams_box_->count() > 1);
  refresh_button_->setEnabled(!active && !connected && !discovery_watcher_.isRunning());
  connect_button_->setEnabled(!active && (connected || devices_box_->count() > 0));
  grab_one_button_->setEnabled(!active && connected);
  grab_live_button_->setEnabled(!active && connected);
  features_->setEnabled(!active && connected);
  updateStatus();
}

void QOneGenWidget::updateStatus()
{
  const bool connected = static_cast<bool>(lease_);
  const bool live = connected && controller_ && controller_->isGrabbing();
  status_->setText(live ? QStringLiteral("Live") : connected ? QStringLiteral("Connected")
      : connection_attempted_ ? QStringLiteral("Disconnected") : QStringLiteral("Idle"));
  status_->setProperty("status", live ? "grabbing" : connected ? "connected"
      : connection_attempted_ ? "disconnected" : "idle");
  status_->style()->unpolish(status_);
  status_->style()->polish(status_);
}

void QOneGenWidget::setFeatureModules(
    std::vector<std::pair<std::string, onegen::FeatureNode>> modules, FeatureRead read,
    FeatureWrite write, std::string_view camera_name)
{
  feature_modules_ = std::move(modules);
  feature_read_ = std::move(read);
  feature_write_ = std::move(write);
  feature_camera_name_ = QString::fromUtf8(camera_name.data(), static_cast<int>(camera_name.size()));
  if (feature_camera_name_.isEmpty()) feature_camera_name_ = QStringLiteral("Camera");
  rebuildFeatures();
}

void QOneGenWidget::setFeatureSnapshot(
    std::vector<std::pair<std::string, onegen::FeatureNode>> modules)
{
  feature_modules_ = std::move(modules);
  pending_features_.clear();
  rebuildFeatures();
}

void QOneGenWidget::setFeatureGrabbing(bool grabbing)
{
  feature_grabbing_ = grabbing;
  if (!grabbing && !pending_features_.empty() && !feature_drain_timer_->isActive())
    feature_drain_timer_->start();
}

void QOneGenWidget::invalidateFeature(const int module, const std::string_view name)
{
  const QString q = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
  QMetaObject::invokeMethod(
      this,
      [this, q, module] {
        if (rebuilding_features_) return;
        pending_features_.insert(QString::number(module) + QChar('\n') + q);
        if (!feature_grabbing_ && !feature_drain_timer_->isActive()) feature_drain_timer_->start();
      },
      Qt::QueuedConnection);
}

void QOneGenWidget::rebuildFeatures()
{
  rebuilding_features_ = true;
  pending_features_.clear();
  QSet<QString> expanded;
  for (int i = 0; i < features_->topLevelItemCount(); ++i)
    collectExpandedFeatures(features_->topLevelItem(i), expanded);
  QString selected;
  int selected_map = 0;
  if (auto* cur = features_->currentItem()) selected = cur->data(0, Qt::UserRole).toString();
  if (auto* cur = features_->currentItem()) selected_map = cur->data(0, Qt::UserRole + 1).toInt();
  const int scroll = features_->verticalScrollBar()->value();

  features_->clear();
  if (feature_modules_.empty()) {
    rebuilding_features_ = false;
    return;
  }

  auto* cam = new QTreeWidgetItem(features_, QStringList(feature_camera_name_));
  cam->setData(0, Qt::UserRole, QStringLiteral("__camera_root__"));
  try {
    for (std::size_t index = 0; index < feature_modules_.size(); ++index) {
      const auto& entry = feature_modules_[index];
      auto* parent = cam;
      if (!entry.first.empty()) {
        parent = new QTreeWidgetItem(cam, QStringList(QString::fromStdString(entry.first)));
        parent->setData(0, Qt::UserRole, QStringLiteral("__module_%1__").arg(index));
        parent->setData(0, Qt::UserRole + 1, static_cast<int>(index));
      }
      addFeatureCategories(parent, entry.second, static_cast<int>(index));
    }
  } catch (...) {
    features_->clear();
    rebuilding_features_ = false;
    return;
  }

  if (expanded.isEmpty())
    expand_camera_only(cam, 0);
  else
    restoreExpandedFeatures(cam, expanded);

  if (selected.isEmpty()) {
    features_->setCurrentItem(cam);
  } else {
    const auto hits = featureItemsNamed(selected, selected_map);
    if (!hits.isEmpty()) features_->setCurrentItem(hits.front());
  }
  features_->verticalScrollBar()->setValue(scroll);
  rebuilding_features_ = false;
}

void QOneGenWidget::addFeatureCategories(QTreeWidgetItem* cam, const onegen::FeatureNode& cat,
                                         const int map_index)
{
  if (cat.kind != onegen::NodeKind::Category && !cat.name.empty()) return;

  QTreeWidgetItem* item = cam;
  if (cat.name != "Root" && !cat.name.empty()) {
    item = new QTreeWidgetItem(cam, QStringList(QString::fromStdString(cat.display_name)));
    item->setData(0, Qt::UserRole, QString::fromStdString(cat.name));
    item->setData(0, Qt::UserRole + 1, map_index);
    for (const auto& c : cat.children) {
      if (c.kind == onegen::NodeKind::Category) continue;
      addFeatureLeaf(item, c, map_index);
    }
  }
  for (const auto& c : cat.children) {
    if (c.kind == onegen::NodeKind::Category)
      addFeatureCategories(cam, c, map_index);
    else if (cat.name == "Root" || cat.name.empty())
      addFeatureLeaf(item, c, map_index);
  }
}

void QOneGenWidget::addFeatureLeaf(QTreeWidgetItem* parent, const onegen::FeatureNode& n,
                                   const int map_index)
{
  if (!shouldShowFeature(n)) return;
  QWidget* w = createFeatureEditor(n, map_index);
  if (!w) return;
  auto* item = new QTreeWidgetItem(parent, QStringList(QString::fromStdString(n.display_name)));
  item->setData(0, Qt::UserRole, QString::fromStdString(n.name));
  item->setData(0, Qt::UserRole + 1, map_index);
  const auto help = n.tooltip.empty() ? n.description : n.tooltip;
  item->setToolTip(0, QString::fromStdString(help));
  features_->setItemWidget(item, 1, w);
}

QWidget* QOneGenWidget::createFeatureEditor(const onegen::FeatureNode& n, const int map_index)
{
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
          writeFeature(map_index, nodeName, "int", std::to_string(v));
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
          writeFeature(map_index, nodeName, "float", std::to_string(v));
        });
      } break;
      case onegen::NodeKind::Boolean: {
        auto* box = new QCheckBox;
        w = box;
        QSignalBlocker block(box);
        if (n.readable) box->setChecked(n.bool_value);
        connect(box, &QCheckBox::checkStateChanged, this,
                [this, nodeName, map_index](Qt::CheckState s) {
          writeFeature(map_index, nodeName, "bool", s == Qt::Checked ? "1" : "0");
        });
      } break;
      case onegen::NodeKind::String: {
        auto* edit = new QLineEdit;
        w = edit;
        QSignalBlocker block(edit);
        if (n.readable) edit->setText(QString::fromStdString(n.string_value));
        connect(edit, &QLineEdit::editingFinished, this, [this, nodeName, map_index, edit] {
          writeFeature(map_index, nodeName, "string", edit->text().toStdString());
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
          writeFeature(map_index, nodeName, "enum", combo->currentData().toString().toStdString());
        });
      } break;
      case onegen::NodeKind::Command: {
        auto* btn = new QPushButton(QStringLiteral("Execute"));
        w = btn;
        connect(btn, &QPushButton::clicked, this, [this, nodeName, map_index] {
          writeFeature(map_index, nodeName, "execute", {});
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

void QOneGenWidget::writeFeature(const int map_index, const QString& nodeName,
                                 const std::string_view op, const std::string_view value)
{
  if (!feature_write_) return;
  try {
    feature_write_(map_index, nodeName.toStdString(), op, value);
    qInfo().noquote() << QStringLiteral("OneGen feature write: module=%1 node=%2 operation=%3")
        .arg(map_index).arg(nodeName, QString::fromUtf8(op.data(), static_cast<int>(op.size())));
  } catch (const std::exception& error) {
    qWarning().noquote() << QStringLiteral("OneGen feature write failed: %1: %2")
                                .arg(nodeName, QString::fromLocal8Bit(error.what()));
    refreshFeatureEditor(nodeName, map_index);
    return;
  } catch (...) {
    qWarning().noquote() << QStringLiteral("OneGen feature write failed: %1").arg(nodeName);
    refreshFeatureEditor(nodeName, map_index);
    return;
  }
  try {
    if (lease_) setFeatureSnapshot(lease_->describe_features());
  } catch (const std::exception& error) {
    qWarning().noquote() << QStringLiteral("OneGen feature tree refresh failed: %1")
                                .arg(QString::fromLocal8Bit(error.what()));
    refreshFeatureEditor(nodeName, map_index);
  }
}

bool QOneGenWidget::refreshFeatureEditor(const QString& nodeName, const int map_index)
{
  const auto n = resolveFeature(nodeName, map_index);
  if (n.name.empty() && n.kind == onegen::NodeKind::Other) return false;
  const auto items = featureItemsNamed(nodeName, map_index);
  if (items.isEmpty()) return false;
  bool ok = false;
  for (auto* item : items) {
    auto* w = features_->itemWidget(item, 1);
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

QList<QTreeWidgetItem*> QOneGenWidget::featureItemsNamed(
    const QString& nodeName, const int map_index) const
{
  QList<QTreeWidgetItem*> out;
  QList<QTreeWidgetItem*> stack;
  for (int i = 0; i < features_->topLevelItemCount(); ++i)
    stack.append(features_->topLevelItem(i));
  while (!stack.isEmpty()) {
    auto* cur = stack.takeLast();
    if (cur->data(0, Qt::UserRole).toString() == nodeName
        && cur->data(0, Qt::UserRole + 1).toInt() == map_index) out.append(cur);
    for (int i = 0; i < cur->childCount(); ++i) stack.append(cur->child(i));
  }
  return out;
}

void QOneGenWidget::collectExpandedFeatures(QTreeWidgetItem* item, QSet<QString>& out) const
{
  if (!item) return;
  if (item->isExpanded()) out.insert(item->data(0, Qt::UserRole).toString());
  for (int i = 0; i < item->childCount(); ++i) collectExpandedFeatures(item->child(i), out);
}

void QOneGenWidget::restoreExpandedFeatures(QTreeWidgetItem* item, const QSet<QString>& names)
{
  if (!item) return;
  item->setExpanded(names.contains(item->data(0, Qt::UserRole).toString()));
  for (int i = 0; i < item->childCount(); ++i) restoreExpandedFeatures(item->child(i), names);
}

bool QOneGenWidget::shouldShowFeature(const onegen::FeatureNode& n) const
{
  if (n.kind == onegen::NodeKind::Category) return false;
  if (!n.visible) return false;
  return n.readable || n.kind == onegen::NodeKind::Command;
}

void QOneGenWidget::drainFeatureInvalidates()
{
  const auto batch = pending_features_;
  pending_features_.clear();
  if (rebuilding_features_ || feature_modules_.empty()) return;

  bool need_rebuild = false;
  for (const auto& key : batch) {
    const auto split = key.indexOf(QChar('\n'));
    const int map_index = key.left(split).toInt();
    const auto name = key.mid(split + 1);
    auto n = resolveFeature(name, map_index);
    const bool shown = !featureItemsNamed(name, map_index).isEmpty();
    const bool show = shouldShowFeature(n);
    if (n.kind == onegen::NodeKind::Category) continue;
    if (shown && show) {
      if (!refreshFeatureEditor(name, map_index)) need_rebuild = true;
    } else if (shown != show) {
      need_rebuild = true;
    }
  }
  if (need_rebuild && !feature_grabbing_) rebuildFeatures();
}

onegen::FeatureNode QOneGenWidget::resolveFeature(const QString& nodeName,
                                                  const int map_index) const
{
  if (!feature_read_) return {};
  try {
    return feature_read_(map_index, nodeName.toStdString());
  } catch (...) {
    return {};
  }
}
