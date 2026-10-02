#include "QOneGenWidget.h"
#include "SingleCaptureDiagnostics.h"
#include <QElapsedTimer>
#include "NumericFeatureEditor.h"
#include "OneGenFeatureTree.h"

#include "OneGenCoordinator.h"
#include "OneGenSourceController.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QMetaObject>
#include <QPushButton>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <exception>

namespace {

void expand_camera_only(QTreeWidgetItem* item, int depth) {
  if (!item) return;
  item->setExpanded(depth == 0);
  for (int i = 0; i < item->childCount(); ++i) expand_camera_only(item->child(i), depth + 1);
}

bool workerProcessDead(const QString& error)
{
  return error.contains(QLatin1String("Worker process exited"))
      || error.contains(QLatin1String("Worker process crashed"));
}

}  // namespace

QOneGenWidget::QOneGenWidget(
    std::shared_ptr<onegen::worker::Coordinator> coordinator,
    OneGenSourceController* controller, std::vector<onegen::DeviceInfo> devices,
    DiscoverFn discover, QWidget* parent,
    const QString& settingsFile, const QString& favoritesKey)
    : QWidget(parent), coordinator_(std::move(coordinator)), controller_(controller),
      devices_(std::move(devices)), discover_(std::move(discover))
{
  setMinimumSize(300, 350);
  auto* layout = new QVBoxLayout(this);
  layout->setObjectName(QStringLiteral("DeviceRootLayout"));

  devices_box_ = new QComboBox;
  devices_box_->setMinimumWidth(120);
  devices_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  devices_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  streams_box_ = new QComboBox;
  streams_box_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  streams_box_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
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

  features_ = new OneGenFeatureTree(nullptr, settingsFile, favoritesKey);
  features_->setObjectName(QStringLiteral("CameraFeaturesTree"));
  features_->setProperty("treeRole", QStringLiteral("DeviceFeatureTree"));
  features_->setColumnCount(2);
  features_->setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Value")});
  features_->setUniformRowHeights(true);
  features_->setRootIsDecorated(true);
  features_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(features_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
    auto* item = features_->itemAt(position);
    if (!item) return;
    const int module = item->data(0, Qt::UserRole + 1).toInt();
    const auto nodeName = item->data(0, Qt::UserRole).toString();
    QMenu menu(this);
    QAction* favorite = nullptr;
    if (item->data(0, OneGenFeatureTree::LeafRole).toBool()) {
      favorite = menu.addAction(features_->isFavorite(item)
          ? tr("Remove from favorites") : tr("Add to favorites"));
      favorite->setShortcut(QKeySequence(QStringLiteral("Ctrl+D")));
    }
    const bool canPersist = lease_ && !operation_watcher_.isRunning()
        && !grab_watcher_.isRunning() && !grab_live_button_->isChecked();
    QAction* copy = nullptr;
    QAction* restore = nullptr;
    if (canPersist) {
      if (favorite) menu.addSeparator();
      copy = menu.addAction(QStringLiteral("Copy module settings"));
      restore = menu.addAction(QStringLiteral("Restore module settings from clipboard"));
      restore->setEnabled(!QApplication::clipboard()->text().isEmpty());
    }
    if (menu.isEmpty()) return;
    const auto* chosen = menu.exec(features_->viewport()->mapToGlobal(position));
    if (!chosen) return;
    if (chosen == favorite) {
      const auto hits = featureItemsNamed(nodeName, module);
      if (!hits.isEmpty()) features_->toggleFavorite(hits.front());
      return;
    }
    try {
      if (chosen == copy) {
        QApplication::clipboard()->setText(QString::fromStdString(lease_->save_features(module)));
        qInfo() << "[GenICam] module settings copied";
      } else if (chosen == restore) {
        const auto bag = QApplication::clipboard()->text().toStdString();
        run(Operation::Restore, [this, module, bag] {
          AsyncResult result;
          lease_->load_features(module, bag);
          result.features = lease_->describe_features();
          return result;
        });
      }
    } catch (const std::exception& error) {
      qWarning().noquote() << "[GenICam] module settings failed:" << error.what();
    }
  });
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
    if (index < 0 || !lease_ || grab_watcher_.isRunning() || grab_live_button_->isChecked()) return;
    const auto stream = streams_box_->currentData().toUInt();
    run(Operation::SelectStream, [this, stream] {
      lease_->select_data_stream(stream);
      AsyncResult result;
      result.features = lease_->describe_features();
      return result;
    });
  });
  connect(grab_one_button_, &QToolButton::clicked, this, [this] { startGrabOne(); });
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
  connect(&grab_watcher_, &QFutureWatcher<AsyncResult>::finished,
          this, &QOneGenWidget::finishGrabOne);
  connect(&discovery_watcher_, &QFutureWatcher<AsyncResult>::finished, this, [this] {
    if (shutting_down_) return;
    const auto result = discovery_watcher_.result();
    if (result.error.isEmpty()) updateDiscoveryDevices(result.devices);
    if (!result.error.isEmpty())
      qWarning().noquote() << QStringLiteral("[GenICam] discovery failed: %1").arg(result.error);
    setOperationActive(operation_watcher_.isRunning());
  });
  for (const auto& device : devices_) {
    const auto label = device.display_name.empty() ? device.id : device.display_name;
    devices_box_->addItem(QString::fromStdString(label), QString::fromStdString(device.id));
  }
  setOperationActive(false);
  if (discover_) QTimer::singleShot(0, this, &QOneGenWidget::refresh);
}

void QOneGenWidget::setFixedCamera()
{
  devices_box_->hide();
  refresh_button_->hide();
}

void QOneGenWidget::setExternalSelector(QComboBox* selector, QToolButton* refresh)
{
  setFixedCamera();
  auto* row = findChild<QHBoxLayout*>(QStringLiteral("DeviceSelectorLayout"));
  row->insertWidget(0, selector);
  row->addWidget(refresh);
  selector->show();
  refresh->show();
}

void QOneGenWidget::setTabbedCamera()
{
  setFixedCamera();
  // The containing device panel owns the minimum size for embedded pages.
  setMinimumSize(0, 0);
  if (auto* panel = findChild<QVBoxLayout*>(QStringLiteral("DeviceTreePanelLayout")))
    panel->setObjectName(QStringLiteral("DeviceTabbedTreePanelLayout"));
}

bool QOneGenWidget::cameraInUse() const
{
  return lease_ || operation_ != Operation::None || operation_watcher_.isRunning() || grab_watcher_.isRunning();
}

void QOneGenWidget::setFeaturePanelOnly()
{
  setFixedCamera();
  setMinimumSize(0, 0);
  connect_button_->hide();
  grab_one_button_->hide();
  grab_live_button_->hide();
  streams_box_->hide();
  if (auto* top = findChild<QHBoxLayout*>(QStringLiteral("DeviceTopBarLayout"))) {
    layout()->removeItem(top);
    delete top;
  }
}

void QOneGenWidget::setInterface(const onegen::InterfaceInfo& info, onegen::FeatureNode snapshot)
{
  const bool sameInterface = interface_ && interface_->cti_path == info.cti_path && interface_->id == info.id;
  interface_ = info;
  setFeaturePanelOnly();
  setTabbedCamera();
  std::vector<std::pair<std::string, onegen::FeatureNode>> modules(4);
  modules[3] = {"Interface", std::move(snapshot)};
  if (sameInterface) {
    feature_camera_name_ = QString::fromStdString(info.display_name.empty() ? info.id : info.display_name);
    setFeatureSnapshot(std::move(modules));
  } else setFeatureModules(std::move(modules),
      [this, info](int, std::string_view name) { return coordinator_->read_interface(info, name); },
      [this, info](int, std::string_view name, std::string_view op, std::string_view value) {
        return coordinator_->write_interface(info, name, op, value);
      }, info.display_name.empty() ? info.id : info.display_name);
  setOperationActive(false);
}

void QOneGenWidget::setTitleCallback(std::function<void(const QString&)> callback)
{
  title_callback_ = std::move(callback);
}

QOneGenWidget::~QOneGenWidget()
{
  prepareForShutdown();
}

void QOneGenWidget::prepareForShutdown()
{
  if (shutting_down_) return;
  title_callback_ = {};
  shutting_down_ = true;
  if (feature_drain_timer_) feature_drain_timer_->stop();
  clearFeatureCallback();
  discovery_watcher_.waitForFinished();
  operation_watcher_.waitForFinished();
  if (controller_) controller_->stop();
  grab_watcher_.waitForFinished();
  const QSignalBlocker blocker(connect_button_);
  connect_button_->setChecked(false);
  if (controller_) {
    try {
      controller_->stop();
    } catch (...) {
    }
    controller_->setLease(nullptr);
    controller_ = nullptr;
  }
  setFeatureModules({}, {}, {}, {});
  lease_.reset();
}

void QOneGenWidget::refresh()
{
  if (shutting_down_ || discovery_watcher_.isRunning() || lease_
      || operation_watcher_.isRunning() || !discover_) return;
  QPointer<QOneGenWidget> self(this);
  discovery_watcher_.setFuture(QtConcurrent::run([discover = discover_, self] {
    AsyncResult result;
    try {
      result.devices = discover([self](const onegen::worker::DiscoveryResult& update) {
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, update] {
          if (!self || self->shutting_down_) return;
          self->updateDiscoveryDevices(update.devices);
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
    qInfo().noquote() << QStringLiteral("[GenICam] connect skipped: no camera selected; connect a camera and refresh the list");
    revert();
    return;
  }
  const auto selected = devices_.at(static_cast<std::size_t>(devices_box_->currentIndex()));
  qInfo().noquote() << QStringLiteral("[GenICam] camera connection requested: model=%1 serial=%2 id=%3 cti=%4")
                          .arg(QString::fromStdString(selected.model), QString::fromStdString(selected.serial), QString::fromStdString(selected.id),
                               QString::fromStdString(selected.cti_path));

  updateStatus();
  if (lease_ && !lease_->alive()) dropLease();
  if (!run(Operation::Connect, [this, selected] {
        AsyncResult result;
        QElapsedTimer timing;
        timing.start();
        const auto mark = [&](const char* stage) {
          const auto elapsed = timing.restart();
          if (qEnvironmentVariableIntValue("ONEGEN_TRACE_CONNECT") == 1)
            qInfo().noquote() << QStringLiteral("[GenICam] connect timing: scope=host stage=%1 duration=%2 ms camera=%3")
                .arg(QLatin1String(stage)).arg(elapsed).arg(QString::fromStdString(selected.id));
        };
        result.lease = std::shared_ptr<onegen::worker::Lease>(coordinator_->open(selected).release());
        mark("open-worker-roundtrip");
        const auto& info = result.lease->info();
        const auto name = info.display_name.empty() ? info.id : info.display_name;
        result.title = QString::fromStdString(name);
        result.streams = result.lease->data_streams();
        mark("list-streams-roundtrip");
        try {
          result.features = result.lease->describe_features();
          mark("feature-snapshot-roundtrip");
        } catch (const std::exception& error) {
          qWarning().noquote() << QStringLiteral("[GenICam] feature tree: %1")
                                     .arg(QString::fromLocal8Bit(error.what()));
        }
        return result;
      })) {
    revert();
  }
}

void QOneGenWidget::disconnectCamera()
{
  auto pendingGrab = grab_watcher_.future();
  if (!run(Operation::Disconnect, [this, pendingGrab]() mutable {
        if (controller_) {
          controller_->stop();
          pendingGrab.waitForFinished();
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
    qDebug().noquote() << QStringLiteral("[GenICam] operation skipped: another control operation is running or shutdown is in progress");
    return false;
  }
  operation_ = operation;
  setOperationActive(true);
  operation_watcher_.setFuture(QtConcurrent::run([this, operation, work = std::move(work)] {
    QElapsedTimer timing;
    timing.start();
    AsyncResult result;
    try {
      result = work();
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
    }
    if (lease_ && result.error.isEmpty()
        && (operation == Operation::Start || operation == Operation::Stop)) {
      try {
        result.features = lease_->describe_features();
      } catch (const std::exception& error) {
        const auto text = QString::fromLocal8Bit(error.what());
        qWarning().noquote() << QStringLiteral("[GenICam] feature tree refresh failed: %1")
                                   .arg(text);
      }
    }
    result.elapsedMs = timing.elapsed();
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
        : operation == Operation::Write ? QStringLiteral("feature write")
        : operation == Operation::Restore ? QStringLiteral("module restore")
        : operation == Operation::SelectStream ? QStringLiteral("stream selection")
        : QStringLiteral("control");
    if (operation == Operation::Write) {
      qWarning().noquote() << QStringLiteral("[GenICam] feature write failed: module=%1 node=%2 operation=%3 value=%4; cause=%5")
          .arg(result.writeMap).arg(result.writeNode, result.writeOp, result.writeValue, result.error);
    } else {
      qWarning().noquote() << QStringLiteral("[GenICam] %1 failed: camera=%2 stream=%3; cause=%4")
          .arg(action, devices_box_->currentText(), streams_box_->currentText(), result.error);
    }
    if (operation == Operation::Connect) {
      const QSignalBlocker blocker(connect_button_);
      connect_button_->setChecked(false);
    }
    if (operation == Operation::Start) {
      const QSignalBlocker blocker(grab_live_button_);
      grab_live_button_->setChecked(false);
    }
    if (operation == Operation::Disconnect || workerProcessDead(result.error)
        || (lease_ && !lease_->alive())) dropLease();
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    else if (operation == Operation::Write && !result.writeNode.isEmpty())
      refreshFeatureEditor(result.writeNode, result.writeMap);
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
    QElapsedTimer tree_timing;
    tree_timing.start();
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
    if (qEnvironmentVariableIntValue("ONEGEN_TRACE_CONNECT") == 1)
      qInfo().noquote() << QStringLiteral("[GenICam] connect timing: scope=host stage=feature-tree-build duration=%1 ms camera=%2")
          .arg(tree_timing.elapsed()).arg(result.title);
    invalidation_gate_ = std::make_unique<GraphicsFrameCallbackGate>();
    lease_->set_invalidate_callback([this, token = invalidation_gate_->token()](const int module, const std::string_view name) {
      GraphicsFrameCallbackGate::Lease callback(token);
      if (callback) invalidateFeature(module, name);
    });
    qInfo().noquote() << QStringLiteral("[GenICam] connected: camera=%1 elapsed_ms=%2")
        .arg(result.title).arg(result.elapsedMs + tree_timing.elapsed());
    if (title_callback_) title_callback_(result.title);
  } else if (operation == Operation::Disconnect) {
    dropLease();
    qInfo() << "[GenICam] disconnected";
  } else if (operation == Operation::Start || operation == Operation::Stop) {
    setFeatureGrabbing(operation == Operation::Start);
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo().noquote() << QStringLiteral("[GenICam] live %1: camera=%2 serial=%3 stream=%4 elapsed_ms=%5")
        .arg(operation == Operation::Start ? QStringLiteral("started") : QStringLiteral("stopped"),
             QString::fromStdString(lease_->info().model), QString::fromStdString(lease_->info().serial), streams_box_->currentText())
        .arg(result.elapsedMs);
  } else if (operation == Operation::Write) {
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    if (!result.writeNode.isEmpty())
      qInfo().noquote() << QStringLiteral("[GenICam] feature write completed: module=%1 node=%2 operation=%3 value=%4")
          .arg(result.writeMap).arg(result.writeNode, result.writeOp, result.writeValue);
  } else if (operation == Operation::Restore) {
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo() << "[GenICam] module settings restored and verified";
  } else if (operation == Operation::SelectStream) {
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo().noquote() << "[GenICam] stream selected:" << streams_box_->currentText();
  }
  setOperationActive(false);
}

void QOneGenWidget::dropLease()
{
  clearFeatureCallback();
  if (controller_) controller_->setLease(nullptr);
  setFeatureModules({}, {}, {}, {});
  streams_box_->clear();
  lease_.reset();
  setFeatureGrabbing(false);
  {
    const QSignalBlocker live(grab_live_button_);
    grab_live_button_->setChecked(false);
  }
  {
    const QSignalBlocker connect(connect_button_);
    connect_button_->setChecked(false);
  }
  updateDiscoveryDevices(devices_);
}

void QOneGenWidget::clearFeatureCallback()
{
  if (invalidation_gate_) invalidation_gate_->beginShutdown();
  if (lease_) lease_->set_invalidate_callback({});
  if (invalidation_gate_) invalidation_gate_->waitForDrain();
  invalidation_gate_.reset();
}

void QOneGenWidget::startGrabOne()
{
  if (!controller_ || !lease_ || shutting_down_ || grab_watcher_.isRunning()) return;
  // Single uses a dedicated watcher instead of operation_watcher_. Keep feature
  // invalidations queued for its complete lifetime so the GUI thread never
  // performs synchronous NodeRead RPCs while acquisition owns the producer.
  feature_drain_timer_->stop();
  setFeatureGrabbing(true);
  grab_one_button_->setEnabled(false);
  grab_watcher_.setFuture(QtConcurrent::run([this] {
    QElapsedTimer timing;
    timing.start();
    AsyncResult result;
    try {
      const auto directory = onegen::diagnostics::beforeSingle(*lease_);
      controller_->grabOne(directory);
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
    }
    if (lease_ && result.error.isEmpty()) {
      try {
        result.features = lease_->describe_features();
      } catch (const std::exception& error) {
        qWarning().noquote() << QStringLiteral("[GenICam] feature tree refresh failed: %1")
                                   .arg(QString::fromLocal8Bit(error.what()));
      }
    }
    result.elapsedMs = timing.elapsed();
    return result;
  }));
  setOperationActive(operation_watcher_.isRunning());
}

void QOneGenWidget::finishGrabOne()
{
  if (shutting_down_) return;
  const auto result = grab_watcher_.result();
  if (!result.error.isEmpty()) {
    qWarning().noquote() << QStringLiteral("[GenICam] single acquisition failed: camera=%1 stream=%2; cause=%3")
        .arg(devices_box_->currentText(), streams_box_->currentText(), result.error);
    if (lease_ && (workerProcessDead(result.error) || !lease_->alive())) dropLease();
  } else if (lease_) {
    // The complete snapshot supersedes every invalidation queued during Single.
    if (!result.features.empty()) setFeatureSnapshot(result.features);
    qInfo().noquote() << QStringLiteral("[GenICam] single acquisition completed: camera=%1 serial=%2 elapsed_ms=%3")
        .arg(QString::fromStdString(lease_->info().model), QString::fromStdString(lease_->info().serial))
        .arg(result.elapsedMs);
  }
  setFeatureGrabbing(false);
  setOperationActive(operation_watcher_.isRunning());
}

void QOneGenWidget::setOperationActive(bool active)
{
  if (active) feature_drain_timer_->stop();
  else if (operation_ == Operation::None && !feature_grabbing_ && !pending_features_.empty()
           && !feature_drain_timer_->isActive()) feature_drain_timer_->start();
  const bool connected = static_cast<bool>(lease_);
  const bool grabbing = grab_watcher_.isRunning();
  const bool blocking = active
      && (operation_ == Operation::Connect || operation_ == Operation::Disconnect
          || operation_ == Operation::Restore || operation_ == Operation::SelectStream);
  devices_box_->setEnabled(!active && !connected);
  streams_box_->setVisible(connected && streams_box_->count() > 1);
  streams_box_->setEnabled(!active && connected && !grabbing && !grab_live_button_->isChecked()
                           && streams_box_->count() > 1);
  refresh_button_->setEnabled(!active && !connected && !discovery_watcher_.isRunning());
  connect_button_->setEnabled(!blocking && (connected || devices_box_->count() > 0));
  grab_one_button_->setEnabled(!active && connected && !grabbing && !grab_live_button_->isChecked());
  grab_live_button_->setEnabled(!blocking && connected && !grabbing);
  features_->setEnabled(connected || interface_.has_value());
  updateStatus();
  emit stateChanged();
}

void QOneGenWidget::updateStatus()
{
  const bool connected = lease_ || interface_.has_value();
  const bool live = connected && controller_ && controller_->isGrabbing();
  status_->setText(live ? QStringLiteral("Live") : connected ? QStringLiteral("Connected")
      : QStringLiteral("Idle"));
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
  if (!applyFeatureSnapshotInPlace()) rebuildFeatures();
  features_->refreshFilter();
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
        if (operation_ == Operation::None && !feature_grabbing_ && !feature_drain_timer_->isActive()) feature_drain_timer_->start();
      },
      Qt::QueuedConnection);
}

QOneGenWidget::FeatureViewport QOneGenWidget::captureFeatureViewport() const
{
  FeatureViewport viewport;
  if (auto* current = features_->currentItem()) {
    viewport.currentName = current->data(0, Qt::UserRole).toString();
    viewport.currentMap = current->data(0, Qt::UserRole + 1).toInt();
  }
  viewport.verticalScroll = features_->verticalScrollBar()->value();
  viewport.horizontalScroll = features_->horizontalScrollBar()->value();
  if (auto* top = features_->itemAt(features_->viewport()->rect().topLeft())) {
    viewport.topName = top->data(0, Qt::UserRole).toString();
    viewport.topMap = top->data(0, Qt::UserRole + 1).toInt();
    viewport.topOffset = features_->visualItemRect(top).top();
  }
  return viewport;
}

void QOneGenWidget::restoreFeatureViewport(const FeatureViewport& viewport)
{
  const bool autoScroll = features_->hasAutoScroll();
  features_->setAutoScroll(false);
  if (!viewport.currentName.isEmpty()) {
    const auto hits = featureItemsNamed(viewport.currentName, viewport.currentMap);
    if (!hits.isEmpty() && features_->currentItem() != hits.front())
      features_->setCurrentItem(hits.front());
  }
  if (!viewport.topName.isEmpty()) {
    const auto hits = featureItemsNamed(viewport.topName, viewport.topMap);
    if (!hits.isEmpty()) {
      features_->scrollToItem(hits.front(), QAbstractItemView::PositionAtTop);
      const int delta = features_->visualItemRect(hits.front()).top() - viewport.topOffset;
      features_->verticalScrollBar()->setValue(features_->verticalScrollBar()->value() + delta);
    } else {
      features_->verticalScrollBar()->setValue(viewport.verticalScroll);
    }
  } else {
    features_->verticalScrollBar()->setValue(viewport.verticalScroll);
  }
  features_->horizontalScrollBar()->setValue(viewport.horizontalScroll);
  features_->setAutoScroll(autoScroll);
}

void QOneGenWidget::rebuildFeatures()
{
  // Block editor signals before clear(): destroying a focused editor emits
  // editingFinished and would rewrite siblings (e.g. UserOutputValueAll=0).
  rebuilding_features_ = true;
  pending_features_.clear();
  QSet<QString> expanded;
  for (int i = 0; i < features_->topLevelItemCount(); ++i)
    collectExpandedFeatures(features_->topLevelItem(i), expanded);
  const auto viewport = captureFeatureViewport();

  features_->clear();
  if (feature_modules_.empty()) {
    rebuilding_features_ = false;
    features_->refreshFilter();
    return;
  }

  auto* cam = new QTreeWidgetItem(features_, QStringList(feature_camera_name_));
  cam->setData(0, Qt::UserRole, QStringLiteral("__camera_root__"));
  try {
    for (std::size_t index = 0; index < feature_modules_.size(); ++index) {
      const auto& entry = feature_modules_[index];
      auto* parent = cam;
      if (!interface_ && !entry.first.empty() && entry.first != "Remote Device") {
        parent = new QTreeWidgetItem(cam, QStringList(QString::fromStdString(entry.first)));
        parent->setData(0, Qt::UserRole, QStringLiteral("__module_%1__").arg(index));
        parent->setData(0, Qt::UserRole + 1, static_cast<int>(index));
      }
      addFeatureCategories(parent, entry.second, static_cast<int>(index));
      if (parent != cam && parent->childCount() == 0) delete parent;
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

  features_->doItemsLayout();
  if (viewport.currentName.isEmpty() && viewport.topName.isEmpty())
    features_->setCurrentItem(cam);
  else
    restoreFeatureViewport(viewport);
  features_->refreshFilter();
  rebuilding_features_ = false;
}

bool QOneGenWidget::applyFeatureSnapshotInPlace()
{
  if (feature_modules_.empty()) return features_->topLevelItemCount() == 0;
  if (features_->topLevelItemCount() == 0) return false;
  auto* cam = features_->topLevelItem(0);
  if (!cam || cam->data(0, Qt::UserRole).toString() != QStringLiteral("__camera_root__"))
    return false;

  const auto viewport = captureFeatureViewport();
  QPointer<QWidget> focused = QApplication::focusWidget();
  if (focused && !features_->isAncestorOf(focused) && focused != features_) focused = nullptr;

  if (cam->text(0) != feature_camera_name_) cam->setText(0, feature_camera_name_);
  rebuilding_features_ = true;
  bool mutated = false;
  bool ok = true;
  try {
    QSet<QString> camKeep;
    for (std::size_t index = 0; index < feature_modules_.size(); ++index) {
      const auto& entry = feature_modules_[index];
      const int map_index = static_cast<int>(index);
      if (!interface_ && !entry.first.empty() && entry.first != "Remote Device") {
        const QString moduleName = QStringLiteral("__module_%1__").arg(index);
        auto* parent = ensureFeatureGroup(
            cam, moduleName, QString::fromStdString(entry.first), map_index, mutated);
        QSet<QString> moduleKeep;
        if (!syncFeatureCategory(parent, entry.second, map_index, moduleKeep, mutated)) {
          ok = false;
          break;
        }
        pruneFeatureChildren(parent, moduleKeep, mutated);
        if (parent->childCount() == 0) {
          delete parent;
          mutated = true;
        } else {
          camKeep.insert(moduleName);
        }
      } else if (!syncFeatureCategory(cam, entry.second, map_index, camKeep, mutated)) {
        ok = false;
        break;
      }
    }
    if (ok) pruneFeatureChildren(cam, camKeep, mutated);
  } catch (...) {
    ok = false;
  }
  rebuilding_features_ = false;
  if (!ok) return false;
  if (mutated) {
    features_->doItemsLayout();
    restoreFeatureViewport(viewport);
  }
  if (focused && focused->isVisible() && !focused->hasFocus())
    focused->setFocus(Qt::OtherFocusReason);
  return true;
}

QTreeWidgetItem* QOneGenWidget::findFeatureChild(
    QTreeWidgetItem* parent, const QString& name, const int map_index) const
{
  if (!parent) return nullptr;
  for (int i = 0; i < parent->childCount(); ++i) {
    auto* child = parent->child(i);
    if (child->data(0, Qt::UserRole).toString() == name
        && child->data(0, Qt::UserRole + 1).toInt() == map_index)
      return child;
  }
  return nullptr;
}

QTreeWidgetItem* QOneGenWidget::ensureFeatureGroup(
    QTreeWidgetItem* parent, const QString& name, const QString& display, const int map_index,
    bool& mutated)
{
  if (auto* existing = findFeatureChild(parent, name, map_index)) {
    if (existing->text(0) != display) existing->setText(0, display);
    return existing;
  }
  auto* item = new QTreeWidgetItem(parent, QStringList(display));
  item->setData(0, Qt::UserRole, name);
  item->setData(0, Qt::UserRole + 1, map_index);
  mutated = true;
  return item;
}

void QOneGenWidget::pruneFeatureChildren(QTreeWidgetItem* parent, const QSet<QString>& keep,
                                         bool& mutated)
{
  if (!parent) return;
  for (int i = parent->childCount() - 1; i >= 0; --i) {
    auto* child = parent->child(i);
    const auto name = child->data(0, Qt::UserRole).toString();
    if (name == QLatin1String("__camera_root__") || keep.contains(name)) continue;
    delete child;
    mutated = true;
  }
}

bool QOneGenWidget::syncFeatureCategory(QTreeWidgetItem* parent, const onegen::FeatureNode& cat,
                                        const int map_index, QSet<QString>& keep, bool& mutated)
{
  if (cat.kind != onegen::NodeKind::Category && !cat.name.empty()) return true;

  QTreeWidgetItem* item = parent;
  if (cat.name != "Root" && !cat.name.empty()) {
    const QString name = QString::fromStdString(cat.name);
    item = ensureFeatureGroup(parent, name, QString::fromStdString(cat.display_name), map_index,
                              mutated);
    QSet<QString> leafKeep;
    for (const auto& child : cat.children) {
      if (child.kind == onegen::NodeKind::Category || !shouldShowFeature(child)) continue;
      const QString leaf = QString::fromStdString(child.name);
      leafKeep.insert(leaf);
      if (auto* existing = findFeatureChild(item, leaf, map_index)) {
        if (!applyFeatureEditor(existing, child, map_index, &mutated)) return false;
      } else {
        addFeatureLeaf(item, child, map_index);
        mutated = true;
      }
    }
    pruneFeatureChildren(item, leafKeep, mutated);
    if (item->childCount() == 0) {
      delete item;
      mutated = true;
    } else {
      keep.insert(name);
    }
  }
  for (const auto& child : cat.children) {
    if (child.kind == onegen::NodeKind::Category) {
      if (!syncFeatureCategory(parent, child, map_index, keep, mutated)) return false;
    } else if (cat.name == "Root" || cat.name.empty()) {
      if (!shouldShowFeature(child)) continue;
      const QString leaf = QString::fromStdString(child.name);
      keep.insert(leaf);
      if (auto* existing = findFeatureChild(item, leaf, map_index)) {
        if (!applyFeatureEditor(existing, child, map_index, &mutated)) return false;
      } else {
        addFeatureLeaf(item, child, map_index);
        mutated = true;
      }
    }
  }
  return true;
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
  if (item != cam && item->childCount() == 0) delete item;
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
  features_->registerFeature(item, QString::fromStdString(feature_modules_.at(map_index).first));
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
        auto* spin = new IntegerFeatureSpinBox;
        w = spin;
        updateNumericFeatureEditor(spin, n);
        connect(spin, &QAbstractSpinBox::editingFinished, this,
                [this, nodeName, map_index, spin] {
          spin->commitDisplayedText();
          writeFeature(map_index, nodeName, "int",
                       QString::number(spin->value()).toStdString());
        });
      } break;
      case onegen::NodeKind::Float: {
        auto* spin = new QDoubleSpinBox;
        w = spin;
        updateNumericFeatureEditor(spin, n);
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this, nodeName, map_index](double value) {
          writeFeature(map_index, nodeName, "float",
                       QString::number(value, 'g', 17).toStdString());
        });
      } break;
      case onegen::NodeKind::Boolean: {
        auto* box = new QCheckBox;
        w = box;
        QSignalBlocker block(box);
        box->setTristate(!n.readable);
        box->setCheckState(n.readable ? (n.bool_value ? Qt::Checked : Qt::Unchecked) : Qt::PartiallyChecked);
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
        else combo->setCurrentIndex(-1);
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
      case onegen::NodeKind::Register: {
        auto* button = new QPushButton(QStringLiteral("Inspect register…"));
        w = button;
        connect(button, &QPushButton::clicked, this, [this, nodeName, map_index] {
          if (!lease_ || operation_watcher_.isRunning()) return;
          const auto n = resolveFeature(nodeName, map_index);
          try {
            if (!n.register_length || n.register_length > 65536)
              throw std::runtime_error("Raw register length must be between 1 and 65536 bytes.");
            QDialog dialog(this);
            dialog.setWindowTitle(nodeName);
            auto* layout = new QVBoxLayout(&dialog);
            auto* bytes = new QPlainTextEdit;
            bytes->setReadOnly(true);
            if (n.readable) {
              const auto value = lease_->read_register(map_index, nodeName.toStdString());
              bytes->setPlainText(QString::fromLatin1(QByteArray(
                  reinterpret_cast<const char*>(value.data()), static_cast<qsizetype>(value.size())).toHex(' ')));
            }
            auto* allow = new QCheckBox(QStringLiteral("Enable raw register writes"));
            allow->setEnabled(n.writable && !grab_watcher_.isRunning() && !grab_live_button_->isChecked());
            auto* actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
            actions->button(QDialogButtonBox::Save)->setEnabled(false);
            connect(allow, &QCheckBox::toggled, &dialog, [bytes, actions](bool checked) {
              bytes->setReadOnly(!checked);
              actions->button(QDialogButtonBox::Save)->setEnabled(checked);
            });
            layout->addWidget(new QLabel(QStringLiteral("%1 bytes. Raw writes bypass named feature controls.").arg(n.register_length)));
            layout->addWidget(bytes);
            layout->addWidget(allow);
            layout->addWidget(actions);
            connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            connect(actions, &QDialogButtonBox::accepted, &dialog, [&] {
              auto hex = bytes->toPlainText();
              hex.remove(QRegularExpression(QStringLiteral("\\s+")));
              if (hex.size() != static_cast<qsizetype>(n.register_length * 2)
                  || !QRegularExpression(QStringLiteral("\\A[0-9a-fA-F]+\\z")).match(hex).hasMatch()) {
                qWarning() << "[GenICam] register write requires the exact number of hexadecimal bytes";
                return;
              }
              const auto raw = QByteArray::fromHex(hex.toLatin1());
              try {
                lease_->write_register(map_index, nodeName.toStdString(),
                    std::vector<std::uint8_t>(raw.begin(), raw.end()));
                qInfo().noquote() << "[GenICam] raw register written:" << nodeName;
                dialog.accept();
              } catch (const std::exception& error) {
                qWarning().noquote() << "[GenICam] register write failed:" << error.what();
              }
            });
            dialog.exec();
            if (lease_) {
              run(Operation::Write, [this] {
                AsyncResult result;
                result.features = lease_->describe_features();
                return result;
              });
            }
          } catch (const std::exception& error) {
            qWarning().noquote() << "[GenICam] register inspection failed:" << error.what();
          }
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
  if (w) {
    w->setProperty("onegenKind", static_cast<int>(n.kind));
    w->setEnabled(n.kind == onegen::NodeKind::Register ? n.readable || n.writable : n.writable);
  }
  return w;
}

bool QOneGenWidget::applyFeatureEditor(QTreeWidgetItem* item, const onegen::FeatureNode& n,
                                       const int map_index, bool* mutated)
{
  if (!item) return false;
  const auto display = QString::fromStdString(n.display_name);
  if (item->text(0) != display) item->setText(0, display);
  const auto help = n.tooltip.empty() ? n.description : n.tooltip;
  const auto tooltip = QString::fromStdString(help);
  if (item->toolTip(0) != tooltip) item->setToolTip(0, tooltip);
  if (applyFeatureNodeToEditor(features_->itemWidget(item, 1), n, mutated)) return true;
  const bool restore = rebuilding_features_;
  rebuilding_features_ = true;
  QWidget* w = createFeatureEditor(n, map_index);
  if (w) features_->setItemWidget(item, 1, w);
  rebuilding_features_ = restore;
  if (mutated && w) *mutated = true;
  return w != nullptr;
}

bool QOneGenWidget::applyFeatureNodeToEditor(QWidget* editor, const onegen::FeatureNode& n,
                                             bool* mutated)
{
  if (!editor) return false;
  const QVariant kind = editor->property("onegenKind");
  if (kind.isValid() && kind.toInt() != static_cast<int>(n.kind)) return false;
  try {
    switch (n.kind) {
      case onegen::NodeKind::Integer:
        if (auto* spin = qobject_cast<IntegerFeatureSpinBox*>(editor)) {
          updateNumericFeatureEditor(spin, n);
          return true;
        }
        return false;
      case onegen::NodeKind::Float:
        if (auto* spin = qobject_cast<QDoubleSpinBox*>(editor)) {
          updateNumericFeatureEditor(spin, n);
          return true;
        }
        return false;
      case onegen::NodeKind::Boolean:
        if (auto* box = qobject_cast<QCheckBox*>(editor)) {
          QSignalBlocker b(box);
          box->setEnabled(n.writable);
          box->setTristate(!n.readable);
          const auto state = n.readable ? (n.bool_value ? Qt::Checked : Qt::Unchecked)
                                        : Qt::PartiallyChecked;
          if (box->checkState() != state) box->setCheckState(state);
          return true;
        }
        return false;
      case onegen::NodeKind::String:
        if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
          QSignalBlocker b(edit);
          edit->setEnabled(n.writable);
          if (n.readable && !edit->hasFocus()) {
            const auto text = QString::fromStdString(n.string_value);
            if (edit->text() != text) edit->setText(text);
          }
          return true;
        }
        return false;
      case onegen::NodeKind::Enumeration: {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
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
            if (mutated) *mutated = true;
          }
          if (n.readable) {
            const auto text = QString::fromStdString(n.enum_value);
            if (combo->currentText() != text) combo->setCurrentText(text);
          } else if (combo->currentIndex() != -1) {
            combo->setCurrentIndex(-1);
          }
          return true;
        }
        return false;
      }
      case onegen::NodeKind::Register:
        if (auto* button = qobject_cast<QPushButton*>(editor)) {
          button->setEnabled(n.readable || n.writable);
          return true;
        }
        return false;
      case onegen::NodeKind::Command:
        if (auto* btn = qobject_cast<QPushButton*>(editor)) {
          btn->setEnabled(n.writable);
          return true;
        }
        return false;
      default:
        if (auto* lab = qobject_cast<QLabel*>(editor)) {
          if (n.readable) {
            const auto text = QString::fromStdString(n.string_value);
            if (lab->text() != text) lab->setText(text);
          }
          return true;
        }
        return false;
    }
  } catch (...) {
    return false;
  }
}

void QOneGenWidget::writeFeature(const int map_index, const QString& nodeName,
                                 const std::string_view op, const std::string_view value)
{
  if ((!lease_ && !interface_) || !feature_write_ || rebuilding_features_ || operation_watcher_.isRunning()) return;
  const auto opText = QString::fromUtf8(op.data(), static_cast<int>(op.size()));
  const auto valueText = QString::fromUtf8(value.data(), static_cast<int>(value.size()));
  const bool explicitSoftwareTrigger = map_index == 0
      && nodeName == QLatin1String("TriggerSoftware") && op == "execute"
      && (grab_watcher_.isRunning() || grab_live_button_->isChecked());
  run(Operation::Write, [this, map_index, nodeName, opText, valueText,
                         explicitSoftwareTrigger] {
    AsyncResult result;
    result.writeMap = map_index;
    result.writeNode = nodeName;
    result.writeOp = opText;
    result.writeValue = valueText;
    try {
      if (explicitSoftwareTrigger) controller_->trigger();
      else feature_write_(map_index, nodeName.toStdString(), opText.toStdString(),
                          valueText.toStdString());
    } catch (const std::exception& error) {
      result.error = QString::fromLocal8Bit(error.what());
    }
    // TriggerSoftware is an acquisition command. Do not insert feature reads
    // between that command and the worker's pending buffer receive.
    if (!explicitSoftwareTrigger) {
      try {
        if (interface_) {
          result.features.resize(4);
          result.features[3] = {"Interface", coordinator_->interface_features(*interface_)};
        } else result.features = lease_->describe_features();
      } catch (const std::exception& error) {
        if (result.error.isEmpty())
          result.error = QStringLiteral("feature tree refresh failed: %1")
                             .arg(QString::fromLocal8Bit(error.what()));
      }
    }
    return result;
  });
}

bool QOneGenWidget::refreshFeatureEditor(const QString& nodeName, const int map_index)
{
  const auto n = resolveFeature(nodeName, map_index);
  if (n.name.empty() && n.kind == onegen::NodeKind::Other) return false;
  const auto items = featureItemsNamed(nodeName, map_index);
  if (items.isEmpty()) return false;
  bool ok = false;
  for (auto* item : items) {
    if (applyFeatureEditor(item, n, map_index)) ok = true;
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
  // The interface panel exposes typed settings; its camera-backed raw inspector is unavailable.
  if (interface_ && n.kind == onegen::NodeKind::Register) return false;
  if (n.kind == onegen::NodeKind::Category) return false;
  if (!n.visible) return false;
  return n.readable || n.writable;
}

void QOneGenWidget::drainFeatureInvalidates()
{
  // Defer synchronous node reads so acquisition and queued frame painting remain responsive.
  if (operation_ != Operation::None || feature_grabbing_ || shutting_down_) return;
  QElapsedTimer timing;
  timing.start();
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
  features_->refreshFilter();
  if (qEnvironmentVariableIntValue("ONEGEN_TRACE_CONNECT") == 1)
    qInfo().noquote() << QStringLiteral("[GenICam] connect timing: scope=host stage=feature-invalidation-drain nodes=%1 duration=%2 ms")
        .arg(batch.size()).arg(timing.elapsed());
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
