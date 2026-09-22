#include "OneGenCoordinator.h"

#include "OneGenWorkerMessages.h"
#include "OneGen.h"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <QDebug>

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
  if (index == 3) return "Interface";
  return "System/TL";
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
  bool reusable = false;
  try {
    if (connection_ && connection_->alive()) {
      connection_->request(Operation::Close, {}, std::chrono::seconds{2});
      reusable = true;
    }
  } catch (...) {}
  auto* owner = owner_;
  const auto key = identity_;
  const auto cti = device_.cti_path;
  const auto dying = connection_;
  connection_.reset();
  if (owner) owner->release(key, cti, reusable, dying.get());
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
void Lease::poll_features(std::uint32_t elapsed_ms) {
  Writer out;
  out.scalar(elapsed_ms);
  connection_->request(Operation::PollFeatures, out.take(), std::chrono::seconds{2});
}
std::vector<std::uint8_t> Lease::read_register(int module, std::string_view name) {
  Writer out;
  out.scalar(module);
  out.string(name);
  const auto payload = connection_->request(Operation::RegisterRead, out.take());
  Reader input(payload);
  return readBytes(input);
}
void Lease::write_register(int module, std::string_view name, const std::vector<std::uint8_t>& bytes) {
  Writer out;
  out.scalar(module);
  out.string(name);
  writeBytes(out, bytes);
  connection_->request(Operation::RegisterWrite, out.take());
}
std::string Lease::save_features(int module) {
  Writer out;
  out.scalar(module);
  const auto payload = connection_->request(Operation::SaveFeatures, out.take());
  Reader input(payload);
  return input.string();
}
void Lease::load_features(int module, std::string_view contents) {
  Writer out;
  out.scalar(module);
  out.string(contents);
  connection_->request(Operation::LoadFeatures, out.take());
}
void Lease::trigger() { connection_->request(Operation::Trigger); }
void Lease::stop() { connection_->request(Operation::StopLive); }
void Lease::set_frame_callback(FrameFn callback) { connection_->set_frame_callback(std::move(callback)); }
void Lease::set_error_callback(ErrorFn callback) { connection_->set_error_callback(std::move(callback)); }
void Lease::set_invalidate_callback(InvalidateFn callback)
{ connection_->set_invalidate_callback(std::move(callback)); }
bool Lease::alive() const { return connection_ && connection_->alive(); }

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
        auto worker = connection(cti.string());
        const auto deadline = std::chrono::steady_clock::now() + probe_timeout_;
        do {
          const auto attempt_started = std::chrono::steady_clock::now();
          const auto remaining = std::max(std::chrono::milliseconds{1},
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - attempt_started));
          const auto slice = std::min(std::chrono::milliseconds{1000}, remaining);
          Writer out;
          out.scalar(static_cast<std::uint32_t>(slice.count()));
          item.devices = readVector<DeviceInfo>(
              worker->request(Operation::Probe, out.take(), slice * 4 + std::chrono::seconds{5}),
              readDeviceInfo);
          if (!item.devices.empty() || std::chrono::steady_clock::now() >= deadline) break;
          std::this_thread::sleep_until(std::min(deadline, attempt_started + slice));
        } while (std::chrono::steady_clock::now() < deadline);
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
      while (!completed_cv.wait_for(lock, std::chrono::seconds{5}, [&] { return !completed.empty(); })) {
        lock.unlock();
        if (progress) {
          auto update = result;
          update.devices = merge(std::move(update.devices));
          progress(update);
        }
        lock.lock();
      }
      item = std::move(completed.front());
      completed.pop();
    }
    result.completed = true;
    ++result.finished_producers;
    qDebug().noquote() << QStringLiteral("[GenICam] producer search result: cti=%1 cameras=%2 status=%3 detail=%4")
        .arg(QString::fromStdString(item.cti.string())).arg(item.devices.size())
        .arg(item.error.empty() ? (item.devices.empty() ? QStringLiteral("no connected camera") : QStringLiteral("available"))
                              : QStringLiteral("unavailable"))
        .arg(QString::fromStdString(item.error));
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
    std::shared_ptr<Connection> worker;
    bool claimed = false;
    try {
      {
        std::lock_guard lock(mutex_);
        if (leased_.count(key)) {
          bool live_holder = false;
          for (const auto& held_cti : candidates) {
            const auto found = device_connections_.find(held_cti + '\0' + key);
            if (found != device_connections_.end() && found->second && found->second->alive()) {
              live_holder = true;
              break;
            }
          }
          if (live_holder) throw std::runtime_error("The camera is already in use.");
          leased_.erase(key);
          for (const auto& held_cti : candidates) {
            const auto found = device_connections_.find(held_cti + '\0' + key);
            if (found == device_connections_.end()) continue;
            if (found->second && found->second->alive()) continue;
            if (const auto probe = connections_.find(held_cti);
                probe != connections_.end() && probe->second == found->second)
              connections_.erase(probe);
            device_connections_.erase(found);
          }
        }
        leased_.insert(key);
        claimed = true;
      }
      worker = connection(cti, key);
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
      if (claimed) {
        const auto connection_key = cti + '\0' + key;
        if (const auto found = device_connections_.find(connection_key); found != device_connections_.end()) {
          if (const auto probe = connections_.find(cti); probe != connections_.end() && probe->second == found->second)
            connections_.erase(probe);
          device_connections_.erase(found);
        }
      }
      if (!claimed) break;
    }
  }
  throw std::runtime_error(error.empty() ? "No producer candidate is available." : error);
}

void Coordinator::shutdown()
{
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
  std::unordered_map<std::string, std::shared_ptr<Connection>> devices;
  { std::lock_guard lock(mutex_); connections.swap(connections_); devices.swap(device_connections_); leased_.clear(); }
}

std::shared_ptr<Connection> Coordinator::connection(const std::string& cti, const std::string& device)
{
  if (!device.empty()) {
    const auto key = cti + '\0' + device;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = device_connections_.find(key); found != device_connections_.end()) return found->second;
      if (const auto probe = connections_.find(cti); probe != connections_.end()
          && std::none_of(device_connections_.begin(), device_connections_.end(),
                          [&](const auto& entry) { return entry.second == probe->second; })) {
        device_connections_[key] = probe->second;
        return probe->second;
      }
    }
    auto worker = std::make_shared<Connection>(worker_executable_, cti);
    Writer probe;
    probe.scalar(std::uint32_t{1000});
    worker->request(Operation::Probe, probe.take(), std::chrono::seconds{21});
    std::lock_guard lock(mutex_);
    device_connections_[key] = worker;
    return worker;
  }
  { std::lock_guard lock(mutex_); if (auto found = connections_.find(cti); found != connections_.end()) return found->second; }
  auto worker = std::make_shared<Connection>(worker_executable_, cti);
  std::lock_guard lock(mutex_);
  auto& stored = connections_[cti];
  if (!stored) stored = worker;
  return stored;
}

void Coordinator::release(const std::string& identity, const std::string& cti, bool reusable,
                          Connection* dying)
{
  std::lock_guard lock(mutex_);
  leased_.erase(identity);
  if (!reusable) {
    if (const auto found = device_connections_.find(cti + '\0' + identity); found != device_connections_.end()) {
      if (dying && found->second.get() != dying) return;
      if (const auto probe = connections_.find(cti); probe != connections_.end() && probe->second == found->second)
        connections_.erase(probe);
      device_connections_.erase(found);
    }
  }
}

} // namespace onegen::worker
