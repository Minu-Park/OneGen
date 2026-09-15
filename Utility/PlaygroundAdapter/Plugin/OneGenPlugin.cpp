#include "DevicePluginTemplate.h"

#include "OneGenControlWidget.h"
#include "OneGenSourceController.h"
#include "onegen/onegen.hpp"

#include <QPointer>
#include <QStringList>
#include <memory>
#include <mutex>

namespace {

QVariantList toDiscoveryData(const std::vector<onegen::DeviceInfo>& devices)
{
  QVariantList values;
  for (const auto& device : devices) {
    values.append(QVariantMap{{QStringLiteral("id"), QString::fromStdString(device.id)},
                              {QStringLiteral("displayName"), QString::fromStdString(device.display_name)},
                              {QStringLiteral("serial"), QString::fromStdString(device.serial)},
                              {QStringLiteral("model"), QString::fromStdString(device.model)},
                              {QStringLiteral("ip"), QString::fromStdString(device.ip)}});
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
    devices.push_back(std::move(device));
  }
  return devices;
}

class OneGenPluginSession final : public IDevicePluginSession {
public:
  OneGenPluginSession(std::shared_ptr<onegen::OneGen> system,
                      std::vector<onegen::DeviceInfo> devices)
      : system_(std::move(system)), camera_(&system_->addCamera()), devices_(std::move(devices)),
        controller_(std::make_unique<OneGenSourceController>(camera_)) {}

  ~OneGenPluginSession() override
  {
    if (widget_) widget_->prepareForShutdown();
    controller_.reset();
    if (system_ && camera_) system_->removeCamera(*camera_);
  }

  QString title() const override { return title_; }
  std::vector<DevicePluginDock> createDockWidgets(QWidget* parent) override
  {
    if (!widget_) {
      widget_ = new OneGenControlWidget(system_, camera_, controller_.get(), devices_, parent);
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
  std::shared_ptr<onegen::OneGen> system_;
  onegen::Camera* camera_ = nullptr;
  std::vector<onegen::DeviceInfo> devices_;
  std::unique_ptr<OneGenSourceController> controller_;
  QPointer<OneGenControlWidget> widget_;
  QString title_ = QStringLiteral("OneGen Camera Session");
  std::function<void(const QString&)> title_changed_;
};

} // namespace

class OneGenPlugin final : public DevicePluginTemplate {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID PlaygroundDevicePlugin_iid)
  Q_INTERFACES(IDevicePlugin)
public:
  DevicePluginDescriptor descriptor() const override
  { return {QStringLiteral("onegen"), QStringLiteral("OneGen Camera"), playgroundDevicePluginVersion(), false}; }

  bool discoverDevices(QVariantMap* data, QString* error) override
  {
    try {
      const auto system = oneGen();
      if (!system) return false;
      if (!loaded_) {
        QStringList failures;
        for (const auto& cti : onegen::defaults::producerCtis()) {
          try {
            system->loadCti(cti);
          } catch (const std::exception& exception) {
            failures.append(QString::fromLocal8Bit(exception.what()));
          }
        }
        loaded_ = system->hasCti();
        if (!loaded_ && !failures.isEmpty()) {
          if (error) *error = failures.join(QLatin1Char('\n'));
          return false;
        }
      }
      const auto devices = system->hasCti()
          ? system->enumerate() : std::vector<onegen::DeviceInfo>{};
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
      const auto system = oneGen();
      if (!system) return {};
      return std::make_unique<OneGenPluginSession>(
          system, fromDiscoveryData(data.value(QStringLiteral("devices")).toList()));
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return {};
    }
  }

  void shutdown() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    system_.reset();
  }

private:
  std::shared_ptr<onegen::OneGen> oneGen()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) return {};
    if (!system_) system_ = std::make_shared<onegen::OneGen>();
    return system_;
  }

  std::mutex mutex_;
  std::shared_ptr<onegen::OneGen> system_;
  bool loaded_ = false;
  bool shutting_down_ = false;
};

#include "OneGenPlugin.moc"
