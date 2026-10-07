#include "DevicePluginTemplate.h"

#include "Utility/Qt/QOneGenDeviceWidget.h"
#include "OneGenCoordinator.h"
#include "OneGenSourceController.h"
#include "OneGen.h"
#include "StartupDiscovery.h"

#include <QDebug>
#include <QPointer>
#include <QStringList>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

QVariantList toDiscoveryData(const std::vector<onegen::DeviceInfo>& devices)
{
  QVariantList values;
  for (const auto& device : devices) {
    QStringList candidates;
    QStringList candidate_ids;
    QStringList candidate_interfaces;
    for (const auto& cti : device.cti_candidates)
      candidates.append(QString::fromStdString(cti));
    for (const auto& id : device.cti_candidate_ids) candidate_ids.append(QString::fromStdString(id));
    for (const auto& id : device.cti_candidate_interfaces) candidate_interfaces.append(QString::fromStdString(id));
    values.append(QVariantMap{
        {QStringLiteral("id"), QString::fromStdString(device.id)},
        {QStringLiteral("displayName"), QString::fromStdString(device.display_name)},
        {QStringLiteral("serial"), QString::fromStdString(device.serial)},
        {QStringLiteral("model"), QString::fromStdString(device.model)},
        {QStringLiteral("ip"), QString::fromStdString(device.ip)},
        {QStringLiteral("tlType"), QString::fromStdString(device.tl_type)},
        {QStringLiteral("ctiPath"), QString::fromStdString(device.cti_path)},
        {QStringLiteral("ctiCandidates"), candidates},
        {QStringLiteral("ctiCandidateIds"), candidate_ids},
        {QStringLiteral("ctiCandidateInterfaces"), candidate_interfaces},
        {QStringLiteral("interfaceId"), QString::fromStdString(device.interface_id)}});
  }
  return values;
}

std::vector<onegen::DeviceInfo> fromDiscoveryData(const QVariantList& values)
{
  std::vector<onegen::DeviceInfo> devices;
  for (const auto& value : values) {
    const auto item = value.toMap();
    onegen::DeviceInfo device;
    device.id = item.value(QStringLiteral("id")).toString().toStdString();
    device.display_name = item.value(QStringLiteral("displayName")).toString().toStdString();
    device.serial = item.value(QStringLiteral("serial")).toString().toStdString();
    device.model = item.value(QStringLiteral("model")).toString().toStdString();
    device.ip = item.value(QStringLiteral("ip")).toString().toStdString();
    device.tl_type = item.value(QStringLiteral("tlType")).toString().toStdString();
    device.cti_path = item.value(QStringLiteral("ctiPath")).toString().toStdString();
    for (const auto& candidate : item.value(QStringLiteral("ctiCandidates")).toStringList())
      device.cti_candidates.push_back(candidate.toStdString());
    for (const auto& id : item.value(QStringLiteral("ctiCandidateIds")).toStringList())
      device.cti_candidate_ids.push_back(id.toStdString());
    for (const auto& id : item.value(QStringLiteral("ctiCandidateInterfaces")).toStringList())
      device.cti_candidate_interfaces.push_back(id.toStdString());
    device.interface_id = item.value(QStringLiteral("interfaceId")).toString().toStdString();
    devices.push_back(std::move(device));
  }
  return devices;
}

std::vector<onegen::DeviceInfo> discover(
    const std::shared_ptr<onegen::worker::Coordinator>& coordinator,
    onegen::worker::Coordinator::ProgressFn progress)
{
  if (!coordinator) throw std::runtime_error("GenICam discovery is shutting down.");
  const auto started = std::chrono::steady_clock::now();
  auto last_report = started;
  qInfo().noquote() << QStringLiteral("[GenICam] camera search started with consumer-provided producers");
  const auto result = coordinator->discover([&](const auto& update) {
    if (progress) progress(update);
    const auto now = std::chrono::steady_clock::now();
    if (update.finished_producers < update.total_producers && now - last_report >= std::chrono::seconds{5}) {
      last_report = now;
      qInfo().noquote() << QStringLiteral("[GenICam] camera search in progress: %1/%2 producers checked, %3 camera(s) found, elapsed=%4 s")
          .arg(update.finished_producers).arg(update.total_producers).arg(update.devices.size())
          .arg(std::chrono::duration_cast<std::chrono::seconds>(now - started).count());
    }
  });
  qInfo().noquote() << QStringLiteral("[GenICam] camera search completed: %1 camera(s), %2/%3 producers checked, %4 producer(s) with search failures, elapsed=%5 ms%6")
      .arg(result.devices.size()).arg(result.finished_producers).arg(result.total_producers)
      .arg(result.failures.size())
      .arg(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count())
      .arg(result.devices.empty() ? QStringLiteral("; no connected camera found") : QString{});
  if (!result.failures.empty()) {
    QStringList details;
    for (const auto& failure : result.failures) details.append(QString::fromStdString(failure));
    qWarning().noquote() << QStringLiteral("[GenICam] producer search failures; available results remain usable. Details:\n%1")
        .arg(details.join(QLatin1Char('\n')));
  }
  for (const auto& device : result.devices)
    qDebug().noquote() << QStringLiteral("[GenICam] camera found: model=%1 serial=%2 id=%3 transport=%4 ip=%5 cti=%6")
        .arg(QString::fromStdString(device.model), QString::fromStdString(device.serial),
             QString::fromStdString(device.id), QString::fromStdString(device.tl_type),
             QString::fromStdString(device.ip), QString::fromStdString(device.cti_path));
  if (!result.completed && !result.failures.empty()) {
    QStringList failures;
    for (const auto& failure : result.failures)
      failures.append(QString::fromStdString(failure));
    throw std::runtime_error(failures.join(QLatin1Char('\n')).toStdString());
  }
  return result.devices;
}

using RetainScan = std::function<void(std::shared_ptr<onegen::worker::StartupDiscovery>)>;
/** @brief Replays a shared initial scan once, then permits explicit control refreshes. */
QOneGenWidget::DiscoverFn cachedDiscovery(
    std::shared_ptr<onegen::worker::Coordinator> coordinator,
    std::shared_ptr<onegen::worker::StartupDiscovery> startup, RetainScan retain) {
  return [coordinator = std::move(coordinator), initial =
      std::make_shared<std::shared_ptr<onegen::worker::StartupDiscovery>>(std::move(startup)), retain = std::move(retain)]
      (onegen::worker::Coordinator::ProgressFn progress) mutable {
    if (auto scan = std::exchange(*initial, {})) return scan->finish(std::move(progress));
    auto scan = std::make_shared<onegen::worker::StartupDiscovery>(
        [coordinator](auto update) { return discover(coordinator, std::move(update)); });
    retain(scan);
    return scan->finish(std::move(progress));
  };
}

class OneGenPluginSession final : public IDevicePluginSession {
public:
  OneGenPluginSession(std::shared_ptr<onegen::worker::Coordinator> coordinator,
                      std::vector<onegen::DeviceInfo> devices,
                      std::shared_ptr<onegen::worker::StartupDiscovery> startup, QString settingsFile,
                      std::function<void(QOneGenDeviceWidget*)> registerWidget, RetainScan retain)
      : coordinator_(std::move(coordinator)), devices_(std::move(devices)),
        startup_(std::move(startup)), settings_file_(std::move(settingsFile)),
        controller_(std::make_unique<OneGenSourceController>()), register_widget_(std::move(registerWidget)),
        retain_scan_(std::move(retain)) { controller_->configureControl(coordinator_, devices_); }

  ~OneGenPluginSession() override
  {
    if (widget_) widget_->prepareForShutdown();
    controller_.reset();
  }

  QString title() const override { return title_; }
  std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
  {
    if (!widget_) {
      widget_ = new QOneGenDeviceWidget(
          coordinator_, controller_.get(), devices_,
          cachedDiscovery(coordinator_, std::move(startup_), retain_scan_), parent, settings_file_, settings_file_.isEmpty() ? QString{}
              : QStringLiteral("Plugins/OneGen/Favorites/Nodes"));
      widget_->setTitleCallback([this](const QString& title) {
        title_ = title;
        if (title_changed_) title_changed_(title_);
      });
      register_widget_(widget_);
    }
    return {{QStringLiteral("device-controls"), QStringLiteral("Device Controls"),
             Qt::LeftDockWidgetArea, widget_, true}};
  }
  AbstractSourceController* sourceController() const override { return controller_.get(); }
  unsigned int capabilities() const noexcept override
  {
    return DevicePluginSessionCapability::GraphicsEngine
        | DevicePluginSessionCapability::ScriptEditor;
  }
  void setTitleChangedCallback(std::function<void(const QString&)> callback) override
  { title_changed_ = std::move(callback); }

private:
  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  std::vector<onegen::DeviceInfo> devices_;
  std::shared_ptr<onegen::worker::StartupDiscovery> startup_;
  QString settings_file_;
  std::unique_ptr<OneGenSourceController> controller_;
  QPointer<QOneGenDeviceWidget> widget_;
  QString title_ = QStringLiteral("GenICam Session");
  std::function<void(const QString&)> title_changed_;
  std::function<void(QOneGenDeviceWidget*)> register_widget_;
  RetainScan retain_scan_;
};

} // namespace

class OneGenPlugin final : public DevicePluginTemplate, public IDevicePluginSettings,
    public IDevicePluginGenTLPaths, public IDevicePluginGenTLProducers {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
  Q_INTERFACES(IDevicePlugin IDevicePluginSettings IDevicePluginGenTLPaths IDevicePluginGenTLProducers)
public:
  /** Selects consumer settings without coupling the reusable UI to host paths. */
  void setSettingsFile(const QString& fileName) override { settings_file_ = fileName; }

  /** @brief Accepts exact CTI files without consulting process search variables. */
  bool setGenTLProducerFiles(const QStringList& files, QString* error) override {
    try {
      const auto reason = genTLPathChangeBlockReason();
      if (!reason.isEmpty()) throw std::runtime_error(reason.toStdString());
      const auto worker_coordinator = coordinator();
      std::vector<std::filesystem::path> producers;
      for (const auto& file : files) {
#ifdef _WIN32
        producers.emplace_back(file.toStdWString());
#else
        producers.emplace_back(file.toStdString());
#endif
      }
      if (!worker_coordinator || !worker_coordinator->set_producer_files(std::move(producers)))
        throw std::runtime_error("Release idle GenICam workers before changing producer files.");
      { std::lock_guard lock(mutex_); producers_configured_ = true; }
      qDebug().noquote() << QStringLiteral("[GenICam] consumer producer inventory: %1")
          .arg(files.isEmpty() ? QStringLiteral("(none)") : files.join(QLatin1Char('\n')));
      return true;
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return false;
    }
  }

  /** @brief Starts one retained scan shared by startup and later control panels. */
  bool startGenTLDiscovery(QString* error) override {
    try {
      const auto worker_coordinator = coordinator();
      std::lock_guard lock(mutex_);
      if (!worker_coordinator || !producers_configured_)
        throw std::runtime_error("The consumer must provide GenICam producer files before discovery.");
      if (!startup_) startup_ = std::make_shared<onegen::worker::StartupDiscovery>(
          [worker_coordinator](auto progress) { return discover(worker_coordinator, std::move(progress)); });
      return true;
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return false;
    }
  }

  DevicePluginDescriptor descriptor() const override
  { return {QStringLiteral("onegen"), QStringLiteral("GenICam"), playgroundDevicePluginVersion(), false}; }

  bool discoverDevices(QVariantMap* data, QString* error) override
  {
    try {
      QString startup_error;
      if (!startGenTLDiscovery(&startup_error)) throw std::runtime_error(startup_error.toStdString());
      std::shared_ptr<onegen::worker::StartupDiscovery> startup;
      { std::lock_guard lock(mutex_); startup = startup_; }
      const auto devices = startup->waitForCamera();
      qInfo().noquote() << (devices.empty()
          ? QStringLiteral("[GenICam] opening session without a camera; available startup results are reused")
          : QStringLiteral("[GenICam] camera available: opening session with %1 camera(s); remaining producer search continues").arg(devices.size()));
      if (data) data->insert(QStringLiteral("devices"), toDiscoveryData(devices));
      return true;
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return false;
    }
  }

  std::unique_ptr<IDevicePluginSession> createSession(
      const QVariantMap& data, QString* error) override
  {
    try {
      const auto worker_coordinator = coordinator();
      if (!worker_coordinator) return {};
      std::shared_ptr<onegen::worker::StartupDiscovery> startup;
      { std::lock_guard lock(mutex_); startup = startup_; }
      return std::make_unique<OneGenPluginSession>(
          worker_coordinator, fromDiscoveryData(data.value(QStringLiteral("devices")).toList()),
          std::move(startup), settings_file_, [this](auto* widget) { widgets_.push_back(widget); },
          [this](auto scan) { std::lock_guard lock(mutex_); startup_ = std::move(scan); });
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return {};
    }
  }

  /** @brief Keeps connected cameras, Interfaces and ongoing searches intact. */
  QString genTLPathChangeBlockReason() const override {
    { std::lock_guard lock(mutex_);
      if (shutting_down_) return QStringLiteral("GenICam is shutting down.");
      if (startup_ && startup_->running()) return QStringLiteral("Wait for the GenICam startup search to finish before applying paths.");
    }
    for (const auto& widget : widgets_)
      if (widget && !widget->canReconfigureSearch())
        return widget->genTLPathChangeBlockReason();
    return {};
  }

  /** @brief Retires disconnected producer workers without unloading the plugin. */
  bool resetGenTLSearch(QString* error) override {
    const auto reason = genTLPathChangeBlockReason();
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    std::lock_guard lock(mutex_);
    if (coordinator_ && !coordinator_->reset_idle_search()) {
      if (error) *error = QStringLiteral("GenICam is still searching or connected. Wait for it to become idle.");
      return false;
    }
    startup_.reset();
    return true;
  }

  /** @brief Refreshes surviving controls after the host applies new search paths. */
  void rescanGenTLPaths() override {
    QString error;
    if (!startGenTLDiscovery(&error)) { qWarning().noquote() << "[GenICam]" << error; return; }
    std::erase_if(widgets_, [](const auto& widget) { return widget.isNull(); });
    for (const auto& widget : widgets_) if (widget)
      widget->rescanGenTLPaths(cachedDiscovery(coordinator_, startup_,
          [this](auto scan) { std::lock_guard lock(mutex_); startup_ = std::move(scan); }));
  }

  void shutdown() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    startup_.reset();
    if (coordinator_) coordinator_->shutdown();
    coordinator_.reset();
  }

private:
  QString settings_file_;
  std::vector<QPointer<QOneGenDeviceWidget>> widgets_;
  std::shared_ptr<onegen::worker::Coordinator> coordinator()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) return {};
    if (!coordinator_) {
      const char* worker_value = std::getenv("ONEGEN_WORKER_PATH");
      if (!worker_value || !*worker_value)
        throw std::runtime_error("ONEGEN_WORKER_PATH is not configured for the GenICam package.");
      const std::filesystem::path worker(worker_value);
      if (!std::filesystem::is_regular_file(worker))
        throw std::runtime_error("OneGenWorker is missing from the GenICam package.");
      coordinator_ = std::make_shared<onegen::worker::Coordinator>(worker);

    }
    return coordinator_;
  }

  mutable std::mutex mutex_;
  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  std::shared_ptr<onegen::worker::StartupDiscovery> startup_;
  bool shutting_down_ = false;
  bool producers_configured_ = false;
};

#include "OneGenPlugin.moc"
