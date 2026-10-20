#include "OneGenCoordinator.h"

#include "OneGenWorkerMessages.h"
#include "OneGen.h"

#include <algorithm>
#include <condition_variable>
#include <future>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#include <QDebug>

namespace onegen::worker {
namespace {

std::string identity(const DeviceInfo& device)
{
  return device.cti_path + '\0' + device.interface_id + '\0' + device.id;
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
std::vector<T> readVector(Reader& input, T (*read)(Reader&))
{
  const auto count = input.scalar<std::uint32_t>();
  std::vector<T> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) values.push_back(read(input));
  return values;
}

template <class T>
std::vector<T> readVector(const std::vector<std::byte>& payload, T (*read)(Reader&))
{
  Reader input(payload);
  return readVector(input, read);
}

} // namespace

Lease::Lease(std::shared_ptr<Connection> connection, DeviceInfo device, std::string identity,
             Coordinator* owner, std::uint32_t session)
    : connection_(std::move(connection)), device_(std::move(device)),
      identity_(std::move(identity)), owner_(owner), session_(session) {}

Lease::~Lease()
{
  if (closed_) return;
  bool reusable = false;
  try {
    if (connection_ && connection_->alive()) {
      connection_->request(Operation::Close, {}, std::chrono::seconds{2}, session_);
      reusable = true;
    }
  } catch (...) {}
  auto* owner = owner_;
  const auto key = identity_;
  const auto cti = device_.cti_path;
  const auto dying = connection_;
  if (connection_) {
    connection_->set_frame_callback({}, session_);
    connection_->set_error_callback({}, session_);
    connection_->set_invalidate_callback({}, session_);
  }
  connection_.reset();
  if (owner) owner->release(key, cti, reusable, dying.get(), session_);
}

void Lease::close()
{
  if (closed_) return;
  // Refuse to claim disconnection when the owning worker rejects Close.
  if (connection_ && connection_->alive())
    connection_->request(Operation::Close, {}, std::chrono::seconds{2}, session_);
  closed_ = true;
  connection_->set_frame_callback({}, session_);
  connection_->set_error_callback({}, session_);
  connection_->set_invalidate_callback({}, session_);
  auto* owner = std::exchange(owner_, nullptr);
  if (owner) owner->release(identity_, device_.cti_path, connection_->alive(), connection_.get(), session_);
}

Connection& Lease::openConnection() const
{
  if (closed_.load() || !connection_) throw std::runtime_error("Device lease is closed.");
  return *connection_;
}

std::vector<DataStreamInfo> Lease::data_streams() const
{
  return readVector<DataStreamInfo>(openConnection().request(Operation::ListStreams, {}, std::chrono::seconds{30}, session_), readDataStreamInfo);
}

void Lease::select_data_stream(std::uint32_t index)
{
  Writer out;
  out.scalar(index);
  openConnection().request(Operation::SelectStream, out.take(), std::chrono::seconds{30}, session_);
}

std::vector<std::pair<std::string, FeatureNode>> Lease::describe_features() const
{
  const auto payload = openConnection().request(Operation::DescribeFeatures, {}, std::chrono::seconds{30}, session_);
  Reader input(payload);
  const auto count = input.scalar<std::uint32_t>();
  std::vector<std::pair<std::string, FeatureNode>> modules;
  modules.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto module = input.scalar<std::int32_t>();
    auto features = readFeatureNode(input);
    if (session_ && module >= 3) features = {};
    modules.emplace_back(moduleLabel(module), std::move(features));
  }
  return modules;
}

FeatureNode Lease::read_node(int module, std::string_view name) const
{
  Writer out;
  out.scalar(module);
  out.string(name);
  const auto payload = openConnection().request(Operation::NodeRead, out.take(), std::chrono::seconds{30}, session_);
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
  const auto payload = openConnection().request(Operation::NodeWrite, out.take(), std::chrono::seconds{30}, session_);
  Reader input(payload);
  return readFeatureNode(input);
}

NodeObservation Lease::observe_node(int module, std::string_view name) const {
  Writer out; out.scalar(module); out.string(name);
  const auto payload = openConnection().request(Operation::ObserveNode, out.take(), std::chrono::seconds{30}, session_);
  Reader in(payload); return readNodeObservation(in);
}
NodeWriteResult Lease::write_node_checked(int module, std::string_view name, std::string_view operation,
    std::string_view value, const NodeObservation& expected, bool compareValue) {
  Writer out; out.scalar(module); out.string(name); out.string(operation); out.string(value);
  write(out, expected); out.scalar(compareValue);
  const auto payload = openConnection().request(Operation::WriteNodeChecked, out.take(), std::chrono::seconds{30}, session_);
  Reader in(payload); return readNodeWriteResult(in);
}

void Lease::grab_one(const FrameFn& deliver)
{
  const auto payload = openConnection().request(Operation::GrabOne, {}, std::chrono::seconds{60}, session_);
  Reader input(payload);
  if (deliver) deliver(readFrame(input));
}

void Lease::live() { openConnection().request(Operation::StartLive, {}, std::chrono::seconds{30}, session_); }
void Lease::poll_features(std::uint32_t elapsed_ms) {
  Writer out;
  out.scalar(elapsed_ms);
  openConnection().request(Operation::PollFeatures, out.take(), std::chrono::seconds{2}, session_);
}
std::vector<std::uint8_t> Lease::read_register(int module, std::string_view name) {
  Writer out;
  out.scalar(module);
  out.string(name);
  const auto payload = openConnection().request(Operation::RegisterRead, out.take(), std::chrono::seconds{30}, session_);
  Reader input(payload);
  return readBytes(input);
}
void Lease::write_register(int module, std::string_view name, const std::vector<std::uint8_t>& bytes) {
  Writer out;
  out.scalar(module);
  out.string(name);
  writeBytes(out, bytes);
  openConnection().request(Operation::RegisterWrite, out.take(), std::chrono::seconds{30}, session_);
}
std::string Lease::save_features(int module) {
  Writer out;
  out.scalar(module);
  const auto payload = openConnection().request(Operation::SaveFeatures, out.take(), std::chrono::seconds{30}, session_);
  Reader input(payload);
  return input.string();
}
void Lease::load_features(int module, std::string_view contents) {
  Writer out;
  out.scalar(module);
  out.string(contents);
  openConnection().request(Operation::LoadFeatures, out.take(), std::chrono::seconds{30}, session_);
}
void Lease::trigger() { openConnection().request(Operation::Trigger, {}, std::chrono::seconds{30}, session_); }
void Lease::stop() { openConnection().request(Operation::StopLive, {}, std::chrono::seconds{30}, session_); }
void Lease::set_frame_callback(FrameFn callback) { if (!closed_) connection_->set_frame_callback(std::move(callback), session_); }
void Lease::set_error_callback(ErrorFn callback) { if (!closed_) connection_->set_error_callback(std::move(callback), session_); }
void Lease::set_invalidate_callback(InvalidateFn callback)
{ if (!closed_) connection_->set_invalidate_callback(std::move(callback), session_); }
bool Lease::alive() const { return !closed_ && connection_ && connection_->alive(); }

Coordinator::Coordinator(std::filesystem::path worker_executable,
                         std::filesystem::path cti_root,
                         std::chrono::milliseconds probe_timeout, Connection::ClaimFn claim)
    : worker_executable_(std::move(worker_executable)), cti_root_(std::move(cti_root)),
      probe_timeout_(probe_timeout), claim_(std::move(claim)) {}

Coordinator::~Coordinator() { shutdown(); }

void Coordinator::open_interface(const InterfaceInfo& info) {
  const auto resource = info.cti_path + '\0' + info.id;
  {
    std::lock_guard lock(mutex_);
    if (interface_sessions_.count(resource)) throw std::runtime_error("The Interface is already connected.");
    interface_sessions_[resource] = info;
  }
  try {
    Writer out; out.string(info.id);
    connection(info.cti_path)->request(Operation::OpenInterface, out.take());
  } catch (...) {
    std::shared_ptr<Connection> failed;
    {
      std::lock_guard lock(mutex_);
      interface_sessions_.erase(resource);
      const bool in_use = std::any_of(interface_sessions_.begin(), interface_sessions_.end(),
          [&](const auto& item) { return item.second.cti_path == info.cti_path; })
          || std::any_of(leased_interfaces_.begin(), leased_interfaces_.end(),
              [&](const auto& item) { return item.second.starts_with(info.cti_path + '\0'); });
      if (!in_use) {
        if (const auto found = connections_.find(info.cti_path); found != connections_.end()) {
          failed = std::move(found->second);
          connections_.erase(found);
        }
        std::erase_if(device_connections_, [&](const auto& item) { return item.second == failed; });
      }
    }
    // A producer can retain native resources even after a failed TLOpenInterface.
    // Destroy its idle worker before a subsequent explicit connection attempt.
    if (failed) failed->close();
    failed.reset();
    throw;
  }
}

void Coordinator::close_interface(const InterfaceInfo& info) {
  std::lock_guard lock(mutex_);
  const auto key = info.cti_path + '\0' + info.id;
  for (const auto& [device, iface] : leased_interfaces_)
    if (iface == key) throw std::runtime_error("Disconnect this Interface's Devices before releasing it.");
  if (const auto found = connections_.find(info.cti_path); found != connections_.end() && found->second->alive()) {
    Writer out; out.string(info.id);
    found->second->request(Operation::CloseInterface, out.take());
  }
  interface_sessions_.erase(key);
}

std::vector<DeviceInfo> Coordinator::rescan_interface(const InterfaceInfo& info) {
  std::lock_guard discovery_lock(discovery_mutex_);
  Writer out; out.string(info.id);
  return readVector<DeviceInfo>(connection(info.cti_path)->request(Operation::ProbeInterface, out.take()), readDeviceInfo);
}

std::vector<InterfaceInfo> Coordinator::interfaces() {
  std::lock_guard lock(mutex_);
  std::vector<InterfaceInfo> result;
  for (const auto& [cti, inventory] : interface_inventory_)
    result.insert(result.end(), inventory.begin(), inventory.end());
  return result;
}

FeatureNode Coordinator::interface_features(const InterfaceInfo& info) {
  Writer out;
  out.string(info.id);
  const auto bytes = connection(info.cti_path)->request(Operation::InterfaceFeatures, out.take());
  Reader reader(bytes);
  return readFeatureNode(reader);
}

FeatureNode Coordinator::read_interface(const InterfaceInfo& info, std::string_view name) {
  Writer out;
  out.string(info.id);
  out.string(name);
  const auto bytes = connection(info.cti_path)->request(Operation::InterfaceRead, out.take());
  Reader reader(bytes);
  return readFeatureNode(reader);
}

FeatureNode Coordinator::write_interface(const InterfaceInfo& info, std::string_view name,
                                         std::string_view operation, std::string_view value) {
  std::lock_guard lock(mutex_);
  for (const auto& [key, id] : leased_interfaces_)
    if (id == info.cti_path + '\0' + info.id) throw std::runtime_error("Disconnect all cameras on this interface before reconfiguration.");
  const auto found = connections_.find(info.cti_path);
  if (found == connections_.end()) throw std::runtime_error("Interface producer is unavailable.");
  Writer out;
  out.string(info.id);
  out.string(name);
  out.string(operation);
  out.string(value);
  const auto bytes = found->second->request(Operation::InterfaceWrite, out.take());
  Reader reader(bytes);
  return readFeatureNode(reader);
}

DiscoveryResult Coordinator::discover(ProgressFn progress)
{
  std::lock_guard discovery_lock(discovery_mutex_);
  DiscoveryResult result;
  const auto ctis = producer_files_ ? *producer_files_
      : (cti_root_.empty() ? defaults::producerCtis() : defaults::producerCtis(cti_root_));
  {
    std::lock_guard lock(mutex_);
    std::erase_if(interface_inventory_, [&](const auto& item) {
      return std::none_of(ctis.begin(), ctis.end(), [&](const auto& cti) { return cti.string() == item.first; });
    });
  }
  result.total_producers = ctis.size();
  result.completed = ctis.empty();
  if (progress) progress(result);
  struct Item { std::filesystem::path cti; std::vector<DeviceInfo> devices; std::string error; std::vector<InterfaceInfo> interfaces; };
  std::mutex completed_mutex;
  std::condition_variable completed_cv;
  std::queue<Item> completed;
  std::vector<std::future<void>> probes;
  for (const auto& cti : ctis) {
    probes.push_back(std::async(std::launch::async, [&, cti] {
      Item item{cti};
      std::shared_ptr<Connection> worker;
      try {
        // Only a new explicit discovery may replace an inactive or dead worker.
        std::shared_ptr<Connection> previous;
        {
          std::lock_guard lock(mutex_);
          if (const auto found = connections_.find(cti.string()); found != connections_.end())
            previous = found->second;
        }
        if (previous && (!previous->alive() || previous->inactive()))
          retire_failed_discovery(cti.string(), previous.get(), false);
        worker = connection(cti.string());
        item.interfaces = readVector<InterfaceInfo>(worker->request(Operation::ListInterfaces), readInterfaceInfo);
        const auto deadline = std::chrono::steady_clock::now() + probe_timeout_;
        do {
          const auto attempt_started = std::chrono::steady_clock::now();
          const auto remaining = std::max(std::chrono::milliseconds{1},
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - attempt_started));
          const auto slice = std::min(std::chrono::milliseconds{1000}, remaining);
          Writer out;
          out.scalar(static_cast<std::uint32_t>(slice.count()));
          const auto payload = worker->request(Operation::Probe, out.take(), slice * 4 + std::chrono::seconds{5});
          Reader input(payload);
          item.devices = readVector(input, readDeviceInfo);
          for (const auto& failure : readStrings(input)) {
            if (!item.error.empty()) item.error += '\n';
            item.error += failure;
          }
          if (!input.empty()) throw std::runtime_error("Unexpected producer probe response bytes.");
          if (!item.error.empty() || !item.devices.empty() || std::chrono::steady_clock::now() >= deadline) break;
          std::this_thread::sleep_until(std::min(deadline, attempt_started + slice));
        } while (std::chrono::steady_clock::now() < deadline);
        for (auto& device : item.devices)
          if (device.cti_path.empty()) device.cti_path = cti.string();
      } catch (const std::exception& exception) { item.error = exception.what(); }
      catch (...) { item.error = "Unknown producer probe failure."; }
      if (!item.error.empty() && worker) retire_failed_discovery(cti.string(), worker.get());
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
    result.interfaces.insert(result.interfaces.end(), item.interfaces.begin(), item.interfaces.end());
    { std::lock_guard lock(mutex_); interface_inventory_[item.cti.string()] = item.interfaces; }
    result.completed = true;
    ++result.finished_producers;
    result.producer_results.push_back(QStringLiteral("cti=%1 cameras=%2 status=%3 detail=%4")
        .arg(QString::fromStdString(item.cti.string())).arg(item.devices.size())
        .arg(item.error.empty() ? (item.devices.empty() ? QStringLiteral("no connected camera") : QStringLiteral("available"))
                              : (item.devices.empty() ? QStringLiteral("search failed") : QStringLiteral("partial result")))
        .arg(item.error.empty() ? QStringLiteral("scan completed") : QString::fromStdString(item.error)).toStdString());
    for (auto& device : item.devices) result.devices.push_back(std::move(device));
    if (!item.error.empty()) {
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

void Coordinator::retire_failed_discovery(const std::string& cti, Connection* expected, bool preserve_inactive)
{
  std::shared_ptr<Connection> failed;
  {
    std::lock_guard lock(mutex_);
    const auto found = connections_.find(cti);
    if (found == connections_.end() || found->second.get() != expected) return;
    if (preserve_inactive && found->second->alive() && found->second->inactive()) return;
    if (std::any_of(interface_sessions_.begin(), interface_sessions_.end(),
                    [&](const auto& item) { return item.second.cti_path == cti; })
        || std::any_of(leased_interfaces_.begin(), leased_interfaces_.end(),
                       [&](const auto& item) { return item.second.starts_with(cti + '\0'); })) return;
    failed = std::move(found->second);
    connections_.erase(found);
    std::erase_if(device_connections_, [&](const auto& item) { return item.second == failed; });
  }
  failed->close();
  qInfo().noquote() << QStringLiteral("[GenICam] failed discovery worker retired: cti=%1")
                          .arg(QString::fromStdString(cti));
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
    selected.cti_candidate_ids.clear();
    selected.cti_candidate_interfaces.clear();
    for (const auto& candidate : group)
      if (std::find(selected.cti_candidates.begin(), selected.cti_candidates.end(), candidate.cti_path)
          == selected.cti_candidates.end())
      {
        selected.cti_candidates.push_back(candidate.cti_path);
        selected.cti_candidate_ids.push_back(candidate.id);
        selected.cti_candidate_interfaces.push_back(candidate.interface_id);
      }
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
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto& cti = candidates[index];
    const auto& device_id = index < device.cti_candidate_ids.size() ? device.cti_candidate_ids[index] : device.id;
    const auto& interface_id = index < device.cti_candidate_interfaces.size()
        ? device.cti_candidate_interfaces[index] : device.interface_id;
    std::shared_ptr<Connection> worker;
    bool claimed = false;
    std::uint32_t session = 0;
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
        leased_interfaces_[key] = cti + '\0' + interface_id;
        for (const auto& [resource, board] : interface_sessions_)
          if (board.cti_path == cti && board.id == interface_id) {
            auto [channel, inserted] = session_channels_.try_emplace(cti + '\0' + key, next_session_);
            if (inserted) ++next_session_;
            session = channel->second;
            worker = connections_.at(cti);
            device_connections_[cti + '\0' + key] = worker;
            break;
          }
      }
      if (!worker) worker = connection(cti, key);
      Writer out;
      out.string(device_id);
      out.string(interface_id);
      worker->request(Operation::Open, out.take(), std::chrono::seconds{60}, session);
      auto opened = device;
      opened.cti_path = cti;
      opened.id = device_id;
      opened.interface_id = interface_id;
      { std::lock_guard lock(mutex_); leased_interfaces_[key] = cti + '\0' + opened.interface_id; }
      return std::make_unique<Lease>(std::move(worker), std::move(opened), key, this, session);
    } catch (const std::exception& exception) {
      error = exception.what();
      std::lock_guard lock(mutex_);
      if (claimed) { leased_.erase(key); leased_interfaces_.erase(key); }
      if (session && worker && worker->alive()) {
        try { worker->request(Operation::Close, {}, std::chrono::seconds{2}, session); } catch (...) {}
      }
      if (claimed) {
        const auto connection_key = cti + '\0' + key;
        if (const auto found = device_connections_.find(connection_key); found != device_connections_.end()) {
          if (!session) {
            if (const auto probe = connections_.find(cti); probe != connections_.end() && probe->second == found->second)
              connections_.erase(probe);
          }
          device_connections_.erase(found);
        }
      }
      if (!claimed) break;
    }
  }
  throw std::runtime_error(error.empty() ? "No producer candidate is available." : error);
}

bool Coordinator::set_producer_files(std::vector<std::filesystem::path> files)
{
  std::unique_lock discovery_lock(discovery_mutex_, std::try_to_lock);
  if (!discovery_lock.owns_lock()) return false;
  std::lock_guard lock(mutex_);
  if (!connections_.empty() || !device_connections_.empty()
      || !leased_.empty() || !interface_sessions_.empty()) return false;
  producer_files_ = std::move(files);
  return true;
}

bool Coordinator::reset_idle_search()
{
  std::unique_lock discovery_lock(discovery_mutex_, std::try_to_lock);
  if (!discovery_lock.owns_lock()) return false;
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
  std::unordered_map<std::string, std::shared_ptr<Connection>> devices;
  {
    std::lock_guard lock(mutex_);
    if (!leased_.empty() || !interface_sessions_.empty()) return false;
    connections.swap(connections_);
    devices.swap(device_connections_);
    interface_inventory_.clear();
    session_channels_.clear();
    leased_interfaces_.clear();
  }
  // Both maps can reference the same worker. Close each process before returning.
  std::unordered_set<Connection*> closed;
  for (const auto& map : {&connections, &devices})
    for (const auto& [key, connection] : *map) {
      (void)key;
      if (connection && closed.insert(connection.get()).second) connection->close();
    }
  return true;
}

void Coordinator::shutdown()
{
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
  std::unordered_map<std::string, std::shared_ptr<Connection>> devices;
  { std::lock_guard lock(mutex_); connections.swap(connections_); devices.swap(device_connections_); leased_.clear(); leased_interfaces_.clear(); interface_sessions_.clear(); session_channels_.clear(); interface_inventory_.clear(); }
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
    auto worker = std::make_shared<Connection>(worker_executable_, cti, claim_);
    Writer probe;
    probe.scalar(std::uint32_t{1000});
    worker->request(Operation::Probe, probe.take(), std::chrono::seconds{21});
    std::lock_guard lock(mutex_);
    device_connections_[key] = worker;
    return worker;
  }
  { std::lock_guard lock(mutex_); if (auto found = connections_.find(cti); found != connections_.end()) return found->second; }
  auto worker = std::make_shared<Connection>(worker_executable_, cti, claim_);
  std::lock_guard lock(mutex_);
  auto& stored = connections_[cti];
  if (!stored) stored = worker;
  return stored;
}

void Coordinator::release(const std::string& identity, const std::string& cti, bool reusable,
                          Connection* dying, std::uint32_t session)
{
  std::lock_guard lock(mutex_);
  const auto found = device_connections_.find(cti + '\0' + identity);
  // A dead worker's lease may outlive a reconnect with the same scoped identity.
  if (dying && (found == device_connections_.end() || found->second.get() != dying)) return;
  leased_.erase(identity);
  leased_interfaces_.erase(identity);
  if (!reusable && found != device_connections_.end()) {
    if (!session) {
      if (const auto probe = connections_.find(cti); probe != connections_.end() && probe->second == found->second)
        connections_.erase(probe);
    }
    device_connections_.erase(found);
  }
}

} // namespace onegen::worker
