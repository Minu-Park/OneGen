#include "OneGenCoordinator.h"

#include "OneGenWorkerMessages.h"
#include "OneGen.h"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <queue>
#include <stdexcept>
#include <tuple>

namespace onegen::worker {
namespace {

std::string identity(const DeviceInfo& device)
{
  const std::string transport = device.tl_type + '\0';
  if (!device.serial.empty()) return transport + "serial\0" + device.serial;
  if (!device.ip.empty()) return transport + "ip\0" + device.ip;
  return transport + "id\0" + device.id;
}

const char* moduleLabel(int index)
{
  if (index == 0) return "Remote Device";
  if (index == 1) return "Data Stream";
  if (index == 2) return "Device";
  return "Interface";
}

template <class T>
std::vector<T> readVector(const std::vector<std::byte>& payload, T (*read)(Reader&))
{
  Reader input(payload);
  const auto count = input.scalar<std::uint32_t>();
  std::vector<T> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) values.push_back(read(input));
  return values;
}

} // namespace

Lease::Lease(std::shared_ptr<Connection> connection, DeviceInfo device, std::string identity,
             Coordinator* owner)
    : connection_(std::move(connection)), device_(std::move(device)),
      identity_(std::move(identity)), owner_(owner) {}

Lease::~Lease()
{
  try {
    if (connection_) connection_->request(Operation::Close, {}, std::chrono::seconds{2});
  } catch (...) {}
  auto* owner = owner_;
  const auto key = identity_;
  const auto cti = device_.cti_path;
  connection_.reset();
  if (owner) owner->release(key, cti);
}

std::vector<DataStreamInfo> Lease::data_streams() const
{
  return readVector<DataStreamInfo>(connection_->request(Operation::ListStreams), readDataStreamInfo);
}

void Lease::select_data_stream(std::uint32_t index)
{
  Writer out;
  out.scalar(index);
  connection_->request(Operation::SelectStream, out.take());
}

std::vector<std::pair<std::string, FeatureNode>> Lease::describe_features() const
{
  const auto payload = connection_->request(Operation::DescribeFeatures);
  Reader input(payload);
  const auto count = input.scalar<std::uint32_t>();
  std::vector<std::pair<std::string, FeatureNode>> modules;
  modules.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto module = input.scalar<std::int32_t>();
    auto features = readFeatureNode(input);
    modules.emplace_back(moduleLabel(module), std::move(features));
  }
  return modules;
}

FeatureNode Lease::read_node(int module, std::string_view name) const
{
  Writer out;
  out.scalar(module);
  out.string(name);
  const auto payload = connection_->request(Operation::NodeRead, out.take());
  Reader input(payload);
  return readFeatureNode(input);
}

FeatureNode Lease::write_node(int module, std::string_view name, std::string_view operation,
                              std::string_view value)
{
  Writer out;
  out.scalar(module);
  out.string(name);
  out.string(operation);
  out.string(value);
  const auto payload = connection_->request(Operation::NodeWrite, out.take());
  Reader input(payload);
  return readFeatureNode(input);
}

void Lease::grab_one(const FrameFn& deliver)
{
  const auto payload = connection_->request(Operation::GrabOne, {}, std::chrono::seconds{60});
  Reader input(payload);
  if (deliver) deliver(readFrame(input));
}

void Lease::live() { connection_->request(Operation::StartLive); }
void Lease::trigger() { connection_->request(Operation::Trigger); }
void Lease::stop() { connection_->request(Operation::StopLive); }
void Lease::set_frame_callback(FrameFn callback) { connection_->set_frame_callback(std::move(callback)); }
void Lease::set_error_callback(ErrorFn callback) { connection_->set_error_callback(std::move(callback)); }
void Lease::set_invalidate_callback(InvalidateFn callback)
{ connection_->set_invalidate_callback(std::move(callback)); }

Coordinator::Coordinator(std::filesystem::path worker_executable,
                         std::filesystem::path cti_root,
                         std::chrono::milliseconds probe_timeout)
    : worker_executable_(std::move(worker_executable)), cti_root_(std::move(cti_root)),
      probe_timeout_(probe_timeout) {}

Coordinator::~Coordinator() { shutdown(); }

DiscoveryResult Coordinator::discover(ProgressFn progress)
{
  std::lock_guard discovery_lock(discovery_mutex_);
  DiscoveryResult result;
  const auto ctis = cti_root_.empty() ? defaults::producerCtis() : defaults::producerCtis(cti_root_);
  result.total_producers = ctis.size();
  struct Item { std::filesystem::path cti; std::vector<DeviceInfo> devices; std::string error; };
  std::mutex completed_mutex;
  std::condition_variable completed_cv;
  std::queue<Item> completed;
  std::vector<std::future<void>> probes;
  for (const auto& cti : ctis) {
    probes.push_back(std::async(std::launch::async, [&, cti] {
      Item item{cti};
      try {
        Writer out;
        out.scalar(static_cast<std::uint32_t>(probe_timeout_.count()));
        auto worker = connection(cti.string());
        item.devices = readVector<DeviceInfo>(
            worker->request(Operation::Probe, out.take(), probe_timeout_ + std::chrono::seconds{20}),
            readDeviceInfo);
        for (auto& device : item.devices)
          if (device.cti_path.empty()) device.cti_path = cti.string();
      } catch (const std::exception& exception) { item.error = exception.what(); }
      catch (...) { item.error = "Unknown producer probe failure."; }
      { std::lock_guard lock(completed_mutex); completed.push(std::move(item)); }
      completed_cv.notify_one();
    }));
  }
  for (std::size_t index = 0; index < probes.size(); ++index) {
    Item item;
    {
      std::unique_lock lock(completed_mutex);
      completed_cv.wait(lock, [&] { return !completed.empty(); });
      item = std::move(completed.front());
      completed.pop();
    }
    result.completed = true;
    ++result.finished_producers;
    if (item.error.empty()) {
      for (auto& device : item.devices) result.devices.push_back(std::move(device));
    } else {
      result.failures.push_back(item.cti.string() + ": " + item.error);
    }
    // Merge a copy so later producer aliases do not lose earlier candidates.
    if (progress) {
      auto update = result;
      update.devices = merge(std::move(update.devices));
      progress(update);
    }
  }
  for (auto& probe : probes) probe.get();
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
    (void)key;
    auto selected = group.front();
    selected.cti_candidates.clear();
    for (const auto& candidate : group)
      if (std::find(selected.cti_candidates.begin(), selected.cti_candidates.end(), candidate.cti_path)
          == selected.cti_candidates.end())
        selected.cti_candidates.push_back(candidate.cti_path);
    devices.push_back(std::move(selected));
  }
  return devices;
}

std::unique_ptr<Lease> Coordinator::open(const DeviceInfo& device)
{
  const auto key = identity(device);
  auto candidates = device.cti_candidates;
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
      Writer out;
      out.string(device.id);
      worker->request(Operation::Open, out.take(), std::chrono::seconds{60});
      auto opened = device;
      opened.cti_path = cti;
      return std::make_unique<Lease>(std::move(worker), std::move(opened), key, this);
    } catch (const std::exception& exception) {
      error = exception.what();
      std::lock_guard lock(mutex_);
      if (claimed) leased_.erase(key);
      if (const auto found = connections_.find(cti);
          found != connections_.end() && found->second.use_count() == 1)
        connections_.erase(found);
      if (!claimed) break;
    }
  }
  throw std::runtime_error(error.empty() ? "No producer candidate is available." : error);
}

void Coordinator::shutdown()
{
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
  { std::lock_guard lock(mutex_); connections.swap(connections_); leased_.clear(); }
}

std::shared_ptr<Connection> Coordinator::connection(const std::string& cti)
{
  { std::lock_guard lock(mutex_); if (auto found = connections_.find(cti); found != connections_.end()) return found->second; }
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
  if (const auto found = connections_.find(cti);
      found != connections_.end() && found->second.use_count() == 1)
    connections_.erase(found);
}

} // namespace onegen::worker
