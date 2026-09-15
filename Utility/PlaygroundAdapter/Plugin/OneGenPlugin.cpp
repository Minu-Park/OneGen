#include "DevicePluginTemplate.h"

#include "OneGenControlWidget.h"
#include "OneGenCoordinator.h"
#include "OneGenSourceController.h"
#include "onegen/onegen.hpp"

#include <QDebug>
#include <QPointer>
#include <QStringList>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

QStringList toStringList(const std::vector<std::string>& values)
{
  QStringList list;
  for (const auto& value : values) list.append(QString::fromStdString(value));
  return list;
}

QVariantList toDiscoveryData(const std::vector<onegen::DeviceInfo>& devices)
{
  QVariantList values;
  for (const auto& device : devices) {
    values.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(device.id)},
                              {QStringLiteral("displayName"), QString::fromStdString(device.display_name)},
                              {QStringLiteral("serial"), QString::fromStdString(device.serial)},
                              {QStringLiteral("model"), QString::fromStdString(device.model)},
                              {QStringLiteral("ip"), QString::fromStdString(device.ip)},
                              {QStringLiteral("tlType"), QString::fromStdString(device.tl_type)},
                              {QStringLiteral("ctiPath"), QString::fromStdString(device.cti_path)},
                              {QStringLiteral("ctiCandidates"), toStringList(device.cti_candidates)}});
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
    const std::shared_ptr<onegen::worker::Coordinator>& coordinator)
{
  if (!coordinator) throw std::runtime_error("GenICam discovery is shutting down.");
  const auto* gentl_key = sizeof(void*) == 8 ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH";
  const char* gentl_path = std::getenv(gentl_key);
  qInfo().noquote() << QStringLiteral("OneGen %1=%2")
                          .arg(QLatin1String(gentl_key),
                               (gentl_path && *gentl_path) ? QString::fromLocal8Bit(gentl_path)
                                                           : QStringLiteral("(unset)"));
  QStringList producers;
  for (const auto& cti : onegen::defaults::producerCtis())
    producers.append(QString::fromStdString(cti.string()));
  qInfo().noquote() << QStringLiteral("OneGen producers: %1")
                          .arg(producers.isEmpty() ? QStringLiteral("(none)")
                                                   : producers.join(QLatin1Char('\n')));
  const auto result = coordinator->discover();
  qInfo().noquote() << QStringLiteral("OneGen discovery: %1 camera(s), %2 producer failure(s)")
                           .arg(result.devices.size())
                           .arg(result.failures.size());
  for (const auto& failure : result.failures)
    qWarning().noquote() << QStringLiteral("OneGen probe failed: %1")
                               .arg(QString::fromStdString(failure));
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
                      std::vector<onegen::DeviceInfo> devices)
      : coordinator_(std::move(coordinator)), devices_(std::move(devices)),
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
      widget_ = new OneGenControlWidget(
          coordinator_, controller_.get(), devices_,
          [coordinator = coordinator_] { return discover(coordinator); }, parent);
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
  std::unique_ptr<OneGenSourceController> controller_;
  QPointer<OneGenControlWidget> widget_;
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
      const auto devices = discover(coordinator());
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
      return std::make_unique<OneGenPluginSession>(
          worker_coordinator, fromDiscoveryData(data.value(QStringLiteral("devices")).toList()));
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return {};
    }
  }

  void shutdown() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    if (coordinator_) coordinator_->shutdown();
    coordinator_.reset();
  }

private:
  std::shared_ptr<onegen::worker::Coordinator> coordinator()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) return {};
    if (!coordinator_) {
      const char* root_value = std::getenv("ONEGEN_CTI_ROOT");
      if (!root_value || !*root_value)
        throw std::runtime_error("ONEGEN_CTI_ROOT is not configured for the GenICam package.");
      const std::filesystem::path root(root_value);
#ifdef _WIN32
      const auto worker = root.parent_path() / "OneGenWorker.exe";
#else
      const auto worker = root.parent_path() / "OneGenWorker";
#endif
      if (!std::filesystem::is_regular_file(worker))
        throw std::runtime_error("OneGenWorker is missing from the GenICam package.");
      coordinator_ = std::make_shared<onegen::worker::Coordinator>(worker);
    }
    return coordinator_;
  }

  std::mutex mutex_;
  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  bool shutting_down_ = false;
};

#include "OneGenPlugin.moc"
