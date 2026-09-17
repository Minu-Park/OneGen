#include "DevicePluginTemplate.h"

#include "Utility/Qt/QOneGenWidget.h"
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
    for (const auto& cti : device.cti_candidates)
      candidates.append(QString::fromStdString(cti));
    values.append(QVariantMap{
        {QStringLiteral("id"), QString::fromStdString(device.id)},
        {QStringLiteral("displayName"), QString::fromStdString(device.display_name)},
        {QStringLiteral("serial"), QString::fromStdString(device.serial)},
        {QStringLiteral("model"), QString::fromStdString(device.model)},
        {QStringLiteral("ip"), QString::fromStdString(device.ip)},
        {QStringLiteral("tlType"), QString::fromStdString(device.tl_type)},
        {QStringLiteral("ctiPath"), QString::fromStdString(device.cti_path)},
        {QStringLiteral("ctiCandidates"), candidates}});
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
    devices.push_back(std::move(device));
  }
  return devices;
}

std::vector<onegen::DeviceInfo> discover(
    const std::shared_ptr<onegen::worker::Coordinator>& coordinator,
    onegen::worker::Coordinator::ProgressFn progress)
{
  if (!coordinator) throw std::runtime_error("GenICam discovery is shutting down.");
  const auto* gentl_key = sizeof(void*) == 8 ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH";
  const char* gentl_path = std::getenv(gentl_key);
  qDebug().noquote() << QStringLiteral("[GenICam] discovery search path: %1=%2")
                          .arg(QLatin1String(gentl_key),
                               (gentl_path && *gentl_path) ? QString::fromLocal8Bit(gentl_path)
                                                           : QStringLiteral("(unset)"));
  QStringList producers;
  for (const auto& cti : onegen::defaults::producerCtis())
    producers.append(QString::fromStdString(cti.string()));
  qDebug().noquote() << QStringLiteral("[GenICam] discovery producer inventory: %1")
                          .arg(producers.isEmpty() ? QStringLiteral("(none)")
                                                   : producers.join(QLatin1Char('\n')));
  const auto started = std::chrono::steady_clock::now();
  auto last_report = started;
  qInfo().noquote() << QStringLiteral("[GenICam] camera search started: %1 installed producer(s); camera presence is checked separately")
                          .arg(producers.size());
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
  qInfo().noquote() << QStringLiteral("[GenICam] camera search completed: %1 camera(s), %2/%3 producers checked, %4 unavailable producer(s), elapsed=%5 ms%6")
      .arg(result.devices.size()).arg(result.finished_producers).arg(result.total_producers)
      .arg(result.failures.size())
      .arg(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count())
      .arg(result.devices.empty() ? QStringLiteral("; no connected camera found") : QString{});
  if (!result.failures.empty()) {
    QStringList details;
    for (const auto& failure : result.failures) details.append(QString::fromStdString(failure));
    qInfo().noquote() << QStringLiteral("[GenICam] optional producers unavailable; other search results remain usable. Details:\n%1")
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

class OneGenPluginSession final : public IDevicePluginSession {
public:
  OneGenPluginSession(std::shared_ptr<onegen::worker::Coordinator> coordinator,
                      std::vector<onegen::DeviceInfo> devices,
                      std::shared_ptr<onegen::worker::StartupDiscovery> startup)
      : coordinator_(std::move(coordinator)), devices_(std::move(devices)),
        startup_(std::move(startup)),
        controller_(std::make_unique<OneGenSourceController>()) {}

  ~OneGenPluginSession() override
  {
    if (widget_) widget_->prepareForShutdown();
    controller_.reset();
  }

  QString title() const override { return title_; }
  std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
  {
    if (!widget_) {
      widget_ = new QOneGenWidget(
          coordinator_, controller_.get(), devices_,
          [coordinator = coordinator_, startup =
              std::make_shared<std::shared_ptr<onegen::worker::StartupDiscovery>>(std::move(startup_))]
          (onegen::worker::Coordinator::ProgressFn progress) mutable {
            if (auto initial = std::exchange(*startup, {}))
              return initial->finish(std::move(progress));
            return discover(coordinator, std::move(progress));
          }, parent);
      widget_->setTitleCallback([this](const QString& title) {
        title_ = title;
        if (title_changed_) title_changed_(title_);
      });
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
  std::unique_ptr<OneGenSourceController> controller_;
  QPointer<QOneGenWidget> widget_;
  QString title_ = QStringLiteral("GenICam Session");
  std::function<void(const QString&)> title_changed_;
};

} // namespace

class OneGenPlugin final : public DevicePluginTemplate {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
  Q_INTERFACES(IDevicePlugin)
public:
  DevicePluginDescriptor descriptor() const override
  { return {QStringLiteral("onegen"), QStringLiteral("GenICam"), playgroundDevicePluginVersion(), false}; }

  bool discoverDevices(QVariantMap* data, QString* error) override
  {
    try {
      const auto worker_coordinator = coordinator();
      if (!worker_coordinator) throw std::runtime_error("GenICam discovery is shutting down.");
      auto startup = std::make_shared<onegen::worker::StartupDiscovery>(
          [worker_coordinator](onegen::worker::Coordinator::ProgressFn progress) {
            return discover(worker_coordinator, std::move(progress));
          });
      const auto devices = startup->waitForCamera();
      qInfo().noquote() << (devices.empty()
          ? QStringLiteral("[GenICam] startup search wait reached 10 s without a camera; opening session while remaining producers continue")
          : QStringLiteral("[GenICam] camera available: opening session with %1 camera(s); remaining producer search continues").arg(devices.size()));
      if (data) data->insert(QStringLiteral("devices"), toDiscoveryData(devices));
      { std::lock_guard lock(mutex_); startup_ = std::move(startup); }
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
      { std::lock_guard lock(mutex_); startup = std::exchange(startup_, {}); }
      return std::make_unique<OneGenPluginSession>(
          worker_coordinator, fromDiscoveryData(data.value(QStringLiteral("devices")).toList()),
          std::move(startup));
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return {};
    }
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

  std::mutex mutex_;
  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  std::shared_ptr<onegen::worker::StartupDiscovery> startup_;
  bool shutting_down_ = false;
};

#include "OneGenPlugin.moc"
