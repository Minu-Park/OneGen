#include "ProducerCoordinator.h"

#include "ProducerMessages.h"

#include "Internal/ConnectTiming.h"
#include "Internal/PinnedAcquisition.h"
#include "Internal/StageLog.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <future>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace onegen::producer {

/** One loaded GenTL producer. Its lock serializes every SDK call for the producer. */
struct Producer {
  std::string key;
  std::recursive_mutex mutex;
  std::unique_ptr<OneGen> system;
  /** Load or initialization failure; the producer stays inactive until rediscovery. */
  std::string inactive_error;
  bool probed = false;
  std::vector<Camera*> idle_cameras;
};

/** Coordinator state shared with leases so a lease may outlive its coordinator. */
struct Registry {
  std::mutex mutex;
  std::map<std::string, std::shared_ptr<Producer>> producers;
  std::map<std::string, std::vector<InterfaceInfo>> inventory;
  /** Interface sessions keyed by producer key + '\0' + interface id. */
  std::map<std::string, InterfaceInfo> interface_sessions;
  /** Device identity -> producer key + '\0' + interface id. */
  std::map<std::string, std::string> leased;
};

struct LeaseCallbacks {
  std::mutex mutex;
  Lease::FrameFn frame;
  Lease::ErrorFn error;
  Lease::InvalidateFn invalidate;
};

namespace {

std::string producerKey(const std::string& cti) {
  const std::filesystem::path path(cti);
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  return (error ? path.lexically_normal() : canonical).string();
}

std::string identity(const DeviceInfo& device) {
  return producerKey(device.cti_path) + '\0' + device.interface_id + '\0' + device.id;
}

std::string interfaceKey(const std::string& producer, const std::string& interface_id) {
  return producer + '\0' + interface_id;
}

const char* moduleLabel(int index) {
  if (index == 0) return "Remote Device";
  if (index == 1) return "Data Stream";
  if (index == 2) return "Device";
  if (index == 3) return "Interface";
  return "System/TL";
}

Camera::Module moduleFromIndex(int index) {
  if (index == 0) return Camera::Module::RemoteDevice;
  if (index == 1) return Camera::Module::DataStream;
  if (index == 2) return Camera::Module::Device;
  if (index == 3) return Camera::Module::Interface;
  if (index == 4) return Camera::Module::System;
  throw std::runtime_error("Invalid feature module index.");
}

int moduleIndex(Camera::Module module) {
  switch (module) {
    case Camera::Module::RemoteDevice: return 0;
    case Camera::Module::DataStream: return 1;
    case Camera::Module::Device: return 2;
    case Camera::Module::Interface: return 3;
    default: return 4;
  }
}

constexpr std::array all_modules{Camera::Module::RemoteDevice, Camera::Module::DataStream,
                                 Camera::Module::Device, Camera::Module::Interface,
                                 Camera::Module::System};

bool sharedModule(int module) { return module >= 3; }

OneGen& active(Producer& producer) {
  if (!producer.system) throw std::runtime_error(producer.inactive_error);
  return *producer.system;
}

void ensureProbed(Producer& producer) {
  if (producer.probed) return;
  std::vector<std::string> failures;
  active(producer).enumerate(std::chrono::milliseconds{1000}, failures);
  active(producer).release_idle_interfaces();
  producer.probed = true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Lease

Lease::Lease(std::shared_ptr<Registry> registry, std::shared_ptr<Producer> producer,
             Camera* camera, DeviceInfo device, std::string identity, bool shared_interface)
    : registry_(std::move(registry)), producer_(std::move(producer)), camera_(camera),
      device_(std::move(device)), identity_(std::move(identity)),
      shared_interface_(shared_interface), callbacks_(std::make_shared<LeaseCallbacks>()) {
  // Callbacks capture the shared callback state, never this lease, so a delivery racing
  // close() reads cleared callbacks instead of a destroyed object.
  camera_->set_frame_callback([callbacks = callbacks_](const Frame& frame) {
    FrameFn callback;
    {
      std::lock_guard lock(callbacks->mutex);
      callback = callbacks->frame;
    }
    if (callback) callback(frame);
  });
  camera_->set_error_callback([callbacks = callbacks_](std::string_view message) {
    ErrorFn callback;
    {
      std::lock_guard lock(callbacks->mutex);
      callback = callbacks->error;
    }
    if (callback) callback(message);
  });
  for (const auto module : all_modules) {
    camera_->features(module).set_invalidate_callback(
        [callbacks = callbacks_, index = moduleIndex(module)](std::string_view name) {
          // Runs inside a GenApi invalidate dispatch that must not see an exception.
          try {
            InvalidateFn callback;
            {
              std::lock_guard lock(callbacks->mutex);
              callback = callbacks->invalidate;
            }
            if (callback) callback(index, name);
          } catch (...) {
          }
        });
  }
}

Lease::~Lease() {
  try { close(); } catch (...) {
    stage_log_warning("device lease cleanup failed: device=%s", device_.id.c_str());
  }
}

void Lease::close() {
  if (closed_.exchange(true)) return;
  {
    std::lock_guard lock(callbacks_->mutex);
    callbacks_->frame = {};
    callbacks_->error = {};
    callbacks_->invalidate = {};
  }
  std::exception_ptr failure;
  {
    std::lock_guard lock(producer_->mutex);
    try {
      if (pool_) pool_->request_stop();
      camera_->stop();
    } catch (...) { failure = std::current_exception(); }
    detail::set_direct_frame_sink(*camera_, {});
    if (pool_) {
      pool_->set_frame_callback({});
      pool_->set_error_callback({});
      pool_.reset();
    }
    try { camera_->close(); } catch (...) { if (!failure) failure = std::current_exception(); }
    camera_->set_frame_callback({});
    camera_->set_error_callback({});
    for (const auto module : all_modules) {
      try { camera_->features(module).set_invalidate_callback({}); } catch (...) {}
    }
    producer_->idle_cameras.push_back(camera_);
    try { if (producer_->system) producer_->system->release_idle_interfaces(); } catch (...) {}
  }
  {
    std::lock_guard lock(registry_->mutex);
    registry_->leased.erase(identity_);
  }
  if (failure) std::rethrow_exception(failure);
}

Camera& Lease::camera() const {
  if (closed_.load()) throw std::runtime_error("Device lease is closed.");
  return *camera_;
}

std::vector<DataStreamInfo> Lease::data_streams() const {
  std::lock_guard lock(producer_->mutex);
  return camera().data_streams();
}

void Lease::select_data_stream(std::uint32_t index) {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  const auto before = opened.selected_data_stream();
  opened.select_data_stream(index);
  if (before != index) ++stream_generation_;
  opened.prepare();
}

std::vector<std::pair<std::string, FeatureNode>> Lease::describe_features() const {
  ConnectTiming timing("describe-features");
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  std::vector<std::pair<std::string, FeatureNode>> modules;
  modules.reserve(all_modules.size());
  for (const auto module : all_modules) {
    const auto index = moduleIndex(module);
    // Interface and System maps are shared by every device of an interface session.
    auto features = shared_interface_ && sharedModule(index)
        ? FeatureNode{} : snapshot(opened.features(module).root());
    modules.emplace_back(moduleLabel(index), std::move(features));
    timing.mark(moduleLabel(index));
  }
  return modules;
}

FeatureNode Lease::read_node(int module, std::string_view name) const {
  std::lock_guard lock(producer_->mutex);
  return snapshot(camera().features(moduleFromIndex(module)).find(name));
}

FeatureNode Lease::write_node(int module, std::string_view name, std::string_view operation,
                              std::string_view value) {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  if (shared_interface_ && sharedModule(module))
    throw std::runtime_error("Use the interface settings tab for shared configuration.");
  auto node = opened.features(moduleFromIndex(module)).find(name);
  apply_feature(node, operation, value);
  auto changed = snapshot(node);
  if (module == 0 && operation != "execute" && !opened.is_live() && !opened.is_acquiring())
    opened.prepare();
  return changed;
}

NodeObservation Lease::observe_node(int module, std::string_view name) const {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  return {opened.features(moduleFromIndex(module)).find(name).parameter_snapshot(),
          opened.selected_data_stream(), stream_generation_};
}

NodeWriteResult Lease::write_node_checked(int module, std::string_view name,
                                          std::string_view operation, std::string_view value,
                                          const NodeObservation& expected, bool compareValue) {
  NodeWriteResult result;
  try {
    std::lock_guard lock(producer_->mutex);
    auto& opened = camera();
    const auto target = moduleFromIndex(module);
    if (sharedModule(module)) {
      result.status = NodeWriteStatus::Unavailable;
      result.message = "Shared maps are not device parameter targets.";
      return result;
    }
    auto node = opened.features(target).find(name);
    const NodeObservation current{node.parameter_snapshot(), opened.selected_data_stream(),
                                  stream_generation_};
    if (!current.node.writable || !current.node.observation_error.empty()) {
      result.status = NodeWriteStatus::Unavailable;
      result.message = "Parameter is not writable or cannot be observed.";
      return result;
    }
    if ((target == Camera::Module::DataStream
         && (current.stream_index != expected.stream_index
             || current.stream_generation != expected.stream_generation))
        || !same_parameter_context(current.node, expected.node, compareValue)) {
      result.status = NodeWriteStatus::Conflict;
      result.message = "Parameter or selected stream changed before mutation.";
      return result;
    }
    result.effect = NodeWriteEffect::Unknown;
    const bool trigger = target == Camera::Module::RemoteDevice && name == "TriggerSoftware"
        && operation == "execute";
    if (trigger) {
      opened.trigger();
    } else if (!node.apply_checked(operation, value, expected.node, compareValue)) {
      result.effect = NodeWriteEffect::None;
      result.status = NodeWriteStatus::Conflict;
      result.message = "Node-map context changed before SDK setter.";
      return result;
    }
    result.effect = NodeWriteEffect::Applied;
    if (target == Camera::Module::RemoteDevice && operation != "execute" && !opened.is_live()
        && !opened.is_acquiring())
      opened.prepare();
    result.observation = trigger ? current : NodeObservation{
        opened.features(target).find(name).parameter_snapshot(), opened.selected_data_stream(),
        stream_generation_};
    if (!result.observation->node.observation_error.empty()) {
      result.status = NodeWriteStatus::BackendFailure;
      result.message = result.observation->node.observation_error;
    }
  } catch (const std::exception& error) {
    result.status = NodeWriteStatus::BackendFailure;
    result.message = error.what();
  } catch (...) {
    result.status = NodeWriteStatus::BackendFailure;
    result.message = "Unexpected checked-write failure.";
  }
  return result;
}

void Lease::grab_one(const FrameFn& deliver) {
  Camera* target = nullptr;
  {
    std::lock_guard lock(producer_->mutex);
    target = &camera();
    if (target->is_live()) throw std::runtime_error("Stop Live before Single acquisition.");
  }
  // The single acquisition runs outside the producer lock so stop() can abort it, as the
  // acquisition loop does for Live.
  ConnectTiming timing("single");
  const auto frames = target->single(1);
  timing.mark("acquire-and-stop");
  if (frames.empty()) throw std::runtime_error("Single acquisition returned no frame.");
  if (deliver) deliver(frames.front());
}

void Lease::live() {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  if (pool_) {
    detail::set_direct_frame_sink(opened, pool_);
    pool_->resume();
  } else {
    detail::set_direct_frame_sink(opened, {});
  }
  opened.live();
}

void Lease::trigger() {
  std::lock_guard lock(producer_->mutex);
  camera().trigger();
}

void Lease::stop() {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  if (pool_) pool_->request_stop();
  opened.stop();
}

void Lease::set_frame_callback(FrameFn callback) {
  std::lock_guard lock(callbacks_->mutex);
  callbacks_->frame = std::move(callback);
}

void Lease::set_shared_frame_callback(SharedFrameFn callback) {
  std::lock_guard lock(producer_->mutex);
  if (pool_) pool_->set_frame_callback(std::move(callback));
}

std::uint64_t Lease::payload_size() {
  std::lock_guard lock(producer_->mutex);
  return camera().payload_size();
}

bool Lease::create_frame_pool(std::uint32_t slot_count, std::uint64_t slot_bytes,
                              std::string& error) {
  std::lock_guard lock(producer_->mutex);
  if (closed_.load()) {
    error = "Device lease is closed.";
    return false;
  }
  if (pool_) {
    error = "A frame pool is already attached.";
    return false;
  }
  auto pool = FramePool::create(slot_count, slot_bytes, error);
  if (!pool) return false;
  pool->set_error_callback([callbacks = callbacks_](std::string_view message) {
    ErrorFn callback;
    {
      std::lock_guard callback_lock(callbacks->mutex);
      callback = callbacks->error;
    }
    if (callback) callback(message);
  });
  pool_ = std::move(pool);
  return true;
}

void Lease::destroy_frame_pool() {
  std::lock_guard lock(producer_->mutex);
  if (!pool_) return;
  if (!closed_.load()) detail::set_direct_frame_sink(*camera_, {});
  pool_->set_frame_callback({});
  pool_->set_error_callback({});
  pool_.reset();
}

std::shared_ptr<const FramePool> Lease::frame_pool() const {
  std::lock_guard lock(producer_->mutex);
  return pool_;
}

void Lease::set_error_callback(ErrorFn callback) {
  std::lock_guard lock(callbacks_->mutex);
  callbacks_->error = std::move(callback);
}

void Lease::set_invalidate_callback(InvalidateFn callback) {
  std::lock_guard lock(callbacks_->mutex);
  callbacks_->invalidate = std::move(callback);
}

bool Lease::alive() const { return !closed_.load(); }

void Lease::poll_features(std::uint32_t elapsed_ms) {
  std::lock_guard lock(producer_->mutex);
  camera().poll_features((std::min)(elapsed_ms, 60000U));
}

std::vector<std::uint8_t> Lease::read_register(int module, std::string_view name) {
  std::lock_guard lock(producer_->mutex);
  return camera().features(moduleFromIndex(module)).find(name).read_register();
}

void Lease::write_register(int module, std::string_view name,
                           const std::vector<std::uint8_t>& bytes) {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  if (shared_interface_ && sharedModule(module))
    throw std::runtime_error("Use the interface settings tab for shared configuration.");
  if (opened.is_live() || opened.is_acquiring())
    throw std::runtime_error("Stop acquisition before raw register writes.");
  opened.features(moduleFromIndex(module)).find(name).write_register(bytes);
  opened.prepare();
}

std::string Lease::save_features(int module) {
  std::lock_guard lock(producer_->mutex);
  return camera().features(moduleFromIndex(module)).save_features();
}

void Lease::load_features(int module, std::string_view contents) {
  std::lock_guard lock(producer_->mutex);
  auto& opened = camera();
  if (shared_interface_ && sharedModule(module))
    throw std::runtime_error("Use the interface settings tab for shared configuration.");
  if (opened.is_live() || opened.is_acquiring())
    throw std::runtime_error("Stop acquisition before restoring features.");
  opened.features(moduleFromIndex(module)).load_features(contents);
  opened.prepare();
}

// ---------------------------------------------------------------------------------------------
// Coordinator

Coordinator::Coordinator(std::filesystem::path cti_root, std::chrono::milliseconds probe_timeout,
                         OneGen::InterfaceClaimFn claim)
    : cti_root_(std::move(cti_root)), probe_timeout_(probe_timeout), claim_(std::move(claim)),
      registry_(std::make_shared<Registry>()) {}

Coordinator::~Coordinator() { shutdown(); }

std::shared_ptr<Producer> Coordinator::producer(const std::string& cti, bool reload_inactive) {
  const auto key = producerKey(cti);
  std::shared_ptr<Producer> result;
  {
    std::lock_guard lock(registry_->mutex);
    auto& stored = registry_->producers[key];
    if (stored && reload_inactive && !stored->system) {
      // Only an explicit discovery replaces an inactive producer, and only while unused.
      const bool used = std::any_of(registry_->leased.begin(), registry_->leased.end(),
          [&](const auto& item) { return item.second.rfind(key + '\0', 0) == 0; });
      if (!used) stored.reset();
    }
    if (!stored) {
      stored = std::make_shared<Producer>();
      stored->key = key;
    }
    result = stored;
  }
  std::lock_guard producer_lock(result->mutex);
  if (!result->system && result->inactive_error.empty()) {
    try {
      auto system = std::make_unique<OneGen>();
      if (claim_) system->set_interface_claim_callback(claim_);
      system->loadCti(key);
      result->system = std::move(system);
    } catch (const std::exception& error) {
      result->inactive_error = inactiveProducerMessage(error.what());
      stage_log_warning("producer inactive: cti=%s detail=%s", key.c_str(), error.what());
    }
  }
  return result;
}

std::shared_ptr<Producer> Coordinator::loaded(const InterfaceInfo& info) {
  auto found = producer(info.cti_path, false);
  if (!found->system) throw std::runtime_error(found->inactive_error);
  return found;
}

void Coordinator::open_interface(const InterfaceInfo& info) {
  const auto key = producerKey(info.cti_path);
  const auto resource = interfaceKey(key, info.id);
  {
    std::lock_guard lock(registry_->mutex);
    if (registry_->interface_sessions.count(resource))
      throw std::runtime_error("The Interface is already connected.");
    registry_->interface_sessions[resource] = info;
  }
  try {
    auto found = loaded(info);
    std::lock_guard lock(found->mutex);
    found->system->open_interface(InterfaceInfo{info.id, key});
  } catch (...) {
    std::lock_guard lock(registry_->mutex);
    registry_->interface_sessions.erase(resource);
    throw;
  }
}

void Coordinator::close_interface(const InterfaceInfo& info) {
  const auto key = producerKey(info.cti_path);
  const auto resource = interfaceKey(key, info.id);
  {
    std::lock_guard lock(registry_->mutex);
    for (const auto& [device, iface] : registry_->leased)
      if (iface == resource)
        throw std::runtime_error("Disconnect this Interface's Devices before releasing it.");
  }
  std::shared_ptr<Producer> found;
  {
    std::lock_guard lock(registry_->mutex);
    if (const auto item = registry_->producers.find(key); item != registry_->producers.end())
      found = item->second;
  }
  if (found) {
    std::lock_guard lock(found->mutex);
    if (found->system) found->system->release_interface(InterfaceInfo{info.id, key});
  }
  std::lock_guard lock(registry_->mutex);
  registry_->interface_sessions.erase(resource);
}

std::vector<DeviceInfo> Coordinator::rescan_interface(const InterfaceInfo& info) {
  std::lock_guard discovery_lock(discovery_mutex_);
  auto found = loaded(info);
  std::lock_guard lock(found->mutex);
  auto devices = found->system->enumerate_interface(InterfaceInfo{info.id, found->key});
  for (auto& device : devices)
    if (device.cti_path.empty()) device.cti_path = found->key;
  return devices;
}

std::vector<InterfaceInfo> Coordinator::interfaces() {
  std::lock_guard lock(registry_->mutex);
  std::vector<InterfaceInfo> result;
  for (const auto& [cti, inventory] : registry_->inventory)
    result.insert(result.end(), inventory.begin(), inventory.end());
  return result;
}

FeatureNode Coordinator::interface_features(const InterfaceInfo& info) {
  auto found = loaded(info);
  std::lock_guard lock(found->mutex);
  return snapshot(found->system->interface_features(InterfaceInfo{info.id, found->key}).root());
}

FeatureNode Coordinator::read_interface(const InterfaceInfo& info, std::string_view name) {
  auto found = loaded(info);
  std::lock_guard lock(found->mutex);
  return snapshot(
      found->system->interface_features(InterfaceInfo{info.id, found->key}).find(name));
}

FeatureNode Coordinator::write_interface(const InterfaceInfo& info, std::string_view name,
                                         std::string_view operation, std::string_view value) {
  const auto resource = interfaceKey(producerKey(info.cti_path), info.id);
  {
    std::lock_guard lock(registry_->mutex);
    for (const auto& [device, iface] : registry_->leased)
      if (iface == resource)
        throw std::runtime_error("Disconnect all cameras on this interface before reconfiguration.");
  }
  auto found = loaded(info);
  std::lock_guard lock(found->mutex);
  auto& map = found->system->interface_features(InterfaceInfo{info.id, found->key});
  auto node = map.find(name);
  apply_feature(node, operation, value);
  return snapshot(map.root());
}

DiscoveryResult Coordinator::discover(ProgressFn progress) {
  std::lock_guard discovery_lock(discovery_mutex_);
  DiscoveryResult result;
  const auto ctis = producer_files_ ? *producer_files_
      : (cti_root_.empty() ? defaults::producerCtis() : defaults::producerCtis(cti_root_));
  {
    std::lock_guard lock(registry_->mutex);
    std::erase_if(registry_->inventory, [&](const auto& item) {
      return std::none_of(ctis.begin(), ctis.end(),
                          [&](const auto& cti) { return producerKey(cti.string()) == item.first; });
    });
  }
  result.total_producers = ctis.size();
  result.completed = ctis.empty();
  if (progress) progress(result);

  struct Item {
    std::filesystem::path cti;
    std::vector<DeviceInfo> devices;
    std::string error;
    std::vector<InterfaceInfo> interfaces;
  };
  std::mutex completed_mutex;
  std::condition_variable completed_cv;
  std::queue<Item> completed;
  std::vector<std::future<void>> probes;
  for (const auto& cti : ctis) {
    probes.push_back(std::async(std::launch::async, [&, cti] {
      const StageLogDiscoveryScope quiet;
      Item item{cti};
      try {
        auto found = producer(cti.string(), true);
        if (!found->system) throw std::runtime_error(found->inactive_error);
        {
          std::lock_guard lock(found->mutex);
          item.interfaces = found->system->interfaces();
        }
        const auto deadline = std::chrono::steady_clock::now() + probe_timeout_;
        do {
          const auto attempt_started = std::chrono::steady_clock::now();
          const auto remaining = std::max(std::chrono::milliseconds{1},
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - attempt_started));
          const auto slice = std::min(std::chrono::milliseconds{1000}, remaining);
          std::vector<std::string> failures;
          {
            std::lock_guard lock(found->mutex);
            try {
              item.devices = found->system->enumerate(slice, failures);
              found->system->release_idle_interfaces();
            } catch (...) {
              found->system->release_idle_interfaces();
              throw;
            }
            found->probed = true;
          }
          for (const auto& failure : failures) {
            if (!item.error.empty()) item.error += '\n';
            item.error += failure;
          }
          if (!item.error.empty() || !item.devices.empty()
              || std::chrono::steady_clock::now() >= deadline)
            break;
          std::this_thread::sleep_until(std::min(deadline, attempt_started + slice));
        } while (std::chrono::steady_clock::now() < deadline);
        for (auto& device : item.devices)
          if (device.cti_path.empty()) device.cti_path = found->key;
      } catch (const std::exception& exception) {
        item.error = exception.what();
      } catch (...) {
        item.error = "Unknown producer probe failure.";
      }
      {
        std::lock_guard lock(completed_mutex);
        completed.push(std::move(item));
      }
      completed_cv.notify_one();
    }));
  }
  for (std::size_t index = 0; index < probes.size(); ++index) {
    Item item;
    {
      std::unique_lock lock(completed_mutex);
      while (!completed_cv.wait_for(lock, std::chrono::seconds{5},
                                    [&] { return !completed.empty(); })) {
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
    result.interfaces.insert(result.interfaces.end(), item.interfaces.begin(),
                             item.interfaces.end());
    {
      std::lock_guard lock(registry_->mutex);
      registry_->inventory[producerKey(item.cti.string())] = item.interfaces;
    }
    result.completed = true;
    ++result.finished_producers;
    const auto status = item.error.empty()
        ? (item.devices.empty() ? "no connected camera" : "available")
        : (item.devices.empty() ? "search failed" : "partial result");
    result.producer_results.push_back("cti=" + item.cti.string() + " cameras="
        + std::to_string(item.devices.size()) + " status=" + status + " detail="
        + (item.error.empty() ? std::string("scan completed") : item.error));
    for (auto& device : item.devices) result.devices.push_back(std::move(device));
    if (!item.error.empty()) result.failures.push_back(item.cti.string() + ": " + item.error);
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

std::vector<DeviceInfo> Coordinator::merge(std::vector<DeviceInfo> devices) {
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
      if (std::find(selected.cti_candidates.begin(), selected.cti_candidates.end(),
                    candidate.cti_path) == selected.cti_candidates.end()) {
        selected.cti_candidates.push_back(candidate.cti_path);
        selected.cti_candidate_ids.push_back(candidate.id);
        selected.cti_candidate_interfaces.push_back(candidate.interface_id);
      }
    devices.push_back(std::move(selected));
  }
  return devices;
}

std::unique_ptr<Lease> Coordinator::open(const DeviceInfo& device) {
  const auto key = identity(device);
  auto candidates = device.cti_candidates;
  if (candidates.empty() && !device.cti_path.empty()) candidates.push_back(device.cti_path);
  std::string error;
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    const auto cti_key = producerKey(candidates[index]);
    const auto& device_id =
        index < device.cti_candidate_ids.size() ? device.cti_candidate_ids[index] : device.id;
    const auto& interface_id = index < device.cti_candidate_interfaces.size()
        ? device.cti_candidate_interfaces[index] : device.interface_id;
    const auto resource = interfaceKey(cti_key, interface_id);
    bool shared_interface = false;
    {
      std::lock_guard lock(registry_->mutex);
      if (registry_->leased.count(key)) throw std::runtime_error("The camera is already in use.");
      registry_->leased[key] = resource;
      shared_interface = registry_->interface_sessions.count(resource) != 0;
    }
    try {
      ConnectTiming timing("open");
      auto found = producer(cti_key, false);
      std::lock_guard lock(found->mutex);
      auto& system = active(*found);
      ensureProbed(*found);
      Camera* camera = nullptr;
      if (!found->idle_cameras.empty()) {
        camera = found->idle_cameras.back();
        found->idle_cameras.pop_back();
      } else {
        camera = &system.addCamera();
      }
      try {
        DeviceInfo selected;
        selected.id = device_id;
        selected.interface_id = interface_id;
        selected.cti_path = cti_key;
        camera->open(selected);
        timing.mark("camera-open");
        camera->prepare();
        timing.mark("camera-prepare");
      } catch (...) {
        try { camera->close(); } catch (...) {}
        found->idle_cameras.push_back(camera);
        try { system.release_idle_interfaces(); } catch (...) {}
        throw;
      }
      auto opened = device;
      opened.cti_path = cti_key;
      opened.id = device_id;
      opened.interface_id = interface_id;
      auto lease = std::make_unique<Lease>(registry_, found, camera, std::move(opened), key,
                                           shared_interface);
      try { lease->poll_features(0); } catch (const std::exception& failure) {
        stage_log_warning("feature event setup failed: device=%s detail=%s", device_id.c_str(),
                          failure.what());
      }
      return lease;
    } catch (const std::exception& exception) {
      error = exception.what();
      std::lock_guard lock(registry_->mutex);
      registry_->leased.erase(key);
      stage_log_debug("device open failed: cti=%s detail=%s", cti_key.c_str(), exception.what());
    }
  }
  throw std::runtime_error(error.empty() ? "No producer candidate is available." : error);
}

bool Coordinator::set_producer_files(std::vector<std::filesystem::path> files) {
  std::unique_lock discovery_lock(discovery_mutex_, std::try_to_lock);
  if (!discovery_lock.owns_lock()) return false;
  std::lock_guard lock(registry_->mutex);
  if (!registry_->producers.empty() || !registry_->leased.empty()
      || !registry_->interface_sessions.empty())
    return false;
  producer_files_ = std::move(files);
  return true;
}

bool Coordinator::reset_idle_search() {
  std::unique_lock discovery_lock(discovery_mutex_, std::try_to_lock);
  if (!discovery_lock.owns_lock()) return false;
  std::map<std::string, std::shared_ptr<Producer>> released;
  {
    std::lock_guard lock(registry_->mutex);
    if (!registry_->leased.empty() || !registry_->interface_sessions.empty()) return false;
    released.swap(registry_->producers);
    registry_->inventory.clear();
  }
  // Producer teardown (GCCloseLib) stays outside the registry lock.
  released.clear();
  return true;
}

void Coordinator::shutdown() {
  std::map<std::string, std::shared_ptr<Producer>> released;
  {
    std::lock_guard lock(registry_->mutex);
    released.swap(registry_->producers);
    registry_->inventory.clear();
    registry_->interface_sessions.clear();
  }
  released.clear();
}

}  // namespace onegen::producer
