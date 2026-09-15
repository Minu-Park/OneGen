#include "OneGenCoordinator.h"

#include "OneGenWorkerMessages.h"
#include "onegen/onegen.hpp"

#include <QJsonArray>
#include <algorithm>
#include <future>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace onegen::worker {

namespace {

std::string identity(const DeviceInfo& device)
{
  const std::string transport = device.tl_type + '\0';
  if (!device.serial.empty()) return transport + "serial\0" + device.serial;
  if (!device.ip.empty()) return transport + "ip\0" + device.ip;
  return transport + "id\0" + device.id;
}

const char* moduleLabel(const int index)
{
  if (index == 0) return "Remote Device";
  if (index == 1) return "Data Stream";
  if (index == 2) return "Device";
  return "Interface";
}

} // namespace

Lease::Lease(std::shared_ptr<Connection> connection, DeviceInfo device, std::string identity,
             Coordinator* owner)
    : connection_(std::move(connection)), device_(std::move(device)),
      identity_(std::move(identity)), owner_(owner) {}

Lease::~Lease()
{
  try {
    if (connection_) connection_->rpc({{QStringLiteral("operation"), QStringLiteral("close")}});
  } catch (...) {
  }
  auto* owner = owner_;
  const auto key = identity_;
  const auto cti = device_.cti_path;
  connection_.reset();
  if (owner) owner->release(key, cti);
}

std::vector<DataStreamInfo> Lease::data_streams() const
{
  const auto response =
      connection_->rpc({{QStringLiteral("operation"), QStringLiteral("listStreams")}});
  std::vector<DataStreamInfo> streams;
  for (const auto value : response.json.value(QStringLiteral("streams")).toArray()) {
    const auto item = value.toObject();
    streams.push_back({static_cast<std::uint32_t>(item.value(QStringLiteral("index")).toInt()),
                       item.value(QStringLiteral("id")).toString().toStdString()});
  }
  return streams;
}

void Lease::select_data_stream(const std::uint32_t index)
{
  connection_->rpc({{QStringLiteral("operation"), QStringLiteral("selectStream")},
                    {QStringLiteral("index"), static_cast<int>(index)}});
}

std::vector<std::pair<std::string, FeatureNode>> Lease::describe_features() const
{
  const auto response =
      connection_->rpc({{QStringLiteral("operation"), QStringLiteral("describeFeatures")}});
  std::vector<std::pair<std::string, FeatureNode>> modules;
  for (const auto value : response.json.value(QStringLiteral("modules")).toArray()) {
    const auto item = value.toObject();
    modules.emplace_back(moduleLabel(item.value(QStringLiteral("index")).toInt()),
                         featureNode(item.value(QStringLiteral("root")).toObject()));
  }
  return modules;
}

FeatureNode Lease::read_node(const int module, const std::string_view name) const
{
  const auto response = connection_->rpc(
      {{QStringLiteral("operation"), QStringLiteral("nodeRead")},
       {QStringLiteral("module"), module},
       {QStringLiteral("name"), QString::fromUtf8(name.data(), static_cast<int>(name.size()))}});
  return featureNode(response.json.value(QStringLiteral("node")).toObject());
}

FeatureNode Lease::write_node(const int module, const std::string_view name, const std::string_view op,
                              const std::string_view value)
{
  const auto response = connection_->rpc(
      {{QStringLiteral("operation"), QStringLiteral("nodeWrite")},
       {QStringLiteral("module"), module},
       {QStringLiteral("name"), QString::fromUtf8(name.data(), static_cast<int>(name.size()))},
       {QStringLiteral("op"), QString::fromUtf8(op.data(), static_cast<int>(op.size()))},
       {QStringLiteral("value"), QString::fromUtf8(value.data(), static_cast<int>(value.size()))}});
  return featureNode(response.json.value(QStringLiteral("node")).toObject());
}

void Lease::grab_one(const FrameFn& deliver)
{
  const auto response = connection_->rpc({{QStringLiteral("operation"), QStringLiteral("grabOne")}});
  if (deliver)
    deliver(frameFromMessage(response.json.value(QStringLiteral("frame")).toObject(),
                             response.binary));
}

void Lease::live()
{
  connection_->rpc({{QStringLiteral("operation"), QStringLiteral("startLive")}});
}

void Lease::trigger()
{
  connection_->rpc({{QStringLiteral("operation"), QStringLiteral("trigger")}});
}

void Lease::stop()
{
  connection_->rpc({{QStringLiteral("operation"), QStringLiteral("stopLive")}});
}

void Lease::set_frame_callback(FrameFn cb) { connection_->set_frame_callback(std::move(cb)); }
void Lease::set_error_callback(ErrorFn cb) { connection_->set_error_callback(std::move(cb)); }
void Lease::set_invalidate_callback(InvalidateFn cb)
{
  connection_->set_invalidate_callback(std::move(cb));
}

Coordinator::Coordinator(std::filesystem::path worker_executable,
                         std::filesystem::path cti_root,
                         const std::chrono::milliseconds probe_timeout)
    : worker_executable_(std::move(worker_executable)), cti_root_(std::move(cti_root)),
      probe_timeout_(probe_timeout) {}

Coordinator::~Coordinator() { shutdown(); }

DiscoveryResult Coordinator::discover()
{
  DiscoveryResult result;
  const auto ctis = cti_root_.empty() ? defaults::producerCtis()
                                      : defaults::producerCtis(cti_root_);
  struct Item {
    std::filesystem::path cti;
    ProbeResult probe;
  };
  std::vector<std::future<Item>> probes;
  probes.reserve(ctis.size());
  for (const auto& cti : ctis) {
    probes.push_back(std::async(std::launch::async, [this, cti] {
      Item item{cti, {}};
      try {
        auto worker = connection(cti.string());
        item.probe.completed = true;
        const auto response = worker->rpc(
            {{QStringLiteral("operation"), QStringLiteral("probe")},
             {QStringLiteral("timeoutMs"), static_cast<int>(probe_timeout_.count())}},
            probe_timeout_ + std::chrono::seconds{20});
        item.probe.success = true;
        for (const auto value : response.json.value(QStringLiteral("devices")).toArray()) {
          auto device = deviceInfo(value.toObject());
          if (device.cti_path.empty()) device.cti_path = cti.string();
          item.probe.devices.push_back(std::move(device));
        }
      } catch (const std::exception& exception) {
        item.probe.error = exception.what();
      }
      return item;
    }));
  }
  for (auto& pending : probes) {
    auto item = pending.get();
    result.completed = result.completed || item.probe.completed;
    if (item.probe.success) {
      for (auto& device : item.probe.devices) result.devices.push_back(std::move(device));
    } else {
      result.failures.push_back(item.cti.string() + ": " + item.probe.error);
    }
  }
  result.devices = merge(std::move(result.devices));
  return result;
}

std::vector<DeviceInfo> Coordinator::merge(std::vector<DeviceInfo> devices)
{
  std::sort(devices.begin(), devices.end(), [](const auto& left, const auto& right) {
    return std::tie(left.cti_path, left.id) < std::tie(right.cti_path, right.id);
  });
  std::map<std::string, std::vector<DeviceInfo>> groups;
  for (auto& device : devices) groups[identity(device)].push_back(std::move(device));
  devices.clear();
  for (auto& [key, group] : groups) {
    DeviceInfo selected = group.front();
    selected.cti_candidates.clear();
    for (const auto& candidate : group) {
      const auto path = candidate.cti_path;
      if (std::find(selected.cti_candidates.begin(), selected.cti_candidates.end(), path)
          == selected.cti_candidates.end())
        selected.cti_candidates.push_back(path);
    }
    devices.push_back(std::move(selected));
  }
  return devices;
}

std::unique_ptr<Lease> Coordinator::open(const DeviceInfo& device)
{
  const auto key = identity(device);
  std::vector<std::string> candidates = device.cti_candidates;
  if (candidates.empty() && !device.cti_path.empty()) candidates.push_back(device.cti_path);
  std::string error;
  for (const auto& cti : candidates) {
    auto worker = connection(cti);
    bool claimed = false;
    try {
      {
        std::lock_guard lock(mutex_);
        if (leased_.count(key)) throw std::runtime_error("The camera is already in use.");
        leased_.insert(key);
        claimed = true;
      }
      worker->rpc({{QStringLiteral("operation"), QStringLiteral("open")},
                   {QStringLiteral("deviceId"), QString::fromStdString(device.id)}},
                  std::chrono::seconds{60});
      DeviceInfo opened = device;
      opened.cti_path = cti;
      return std::make_unique<Lease>(std::move(worker), std::move(opened), key, this);
    } catch (const std::exception& exception) {
      error = exception.what();
      worker.reset();
      std::shared_ptr<Connection> idle;
      {
        std::lock_guard lock(mutex_);
        if (claimed) leased_.erase(key);
        const auto it = connections_.find(cti);
        if (it != connections_.end() && it->second.use_count() == 1) {
          idle = std::move(it->second);
          connections_.erase(it);
        }
      }
      idle.reset();
      if (!claimed) break;
    }
  }
  throw std::runtime_error(error.empty() ? "No producer candidate is available." : error);
}

void Coordinator::shutdown()
{
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
  {
    std::lock_guard lock(mutex_);
    connections.swap(connections_);
    leased_.clear();
  }
  connections.clear();
}

std::shared_ptr<Connection> Coordinator::connection(const std::string& cti)
{
  {
    std::lock_guard lock(mutex_);
    if (const auto it = connections_.find(cti); it != connections_.end() && it->second)
      return it->second;
  }
  auto worker = std::make_shared<Connection>(worker_executable_, cti);
  std::lock_guard lock(mutex_);
  auto& stored = connections_[cti];
  if (!stored) stored = worker;
  return stored;
}

void Coordinator::release(const std::string& identity, const std::string& cti)
{
  std::lock_guard lock(mutex_);
  leased_.erase(identity);
  const auto it = connections_.find(cti);
  if (it != connections_.end() && it->second.use_count() == 1) connections_.erase(it);
}

} // namespace onegen::worker
