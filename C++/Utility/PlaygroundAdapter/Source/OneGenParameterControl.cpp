#include "OneGenSourceController.h"
#include "ProducerCoordinator.h"
#include <QDebug>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

namespace {
using Error = DeviceParameterError;
using Kind = DeviceParameterKind;
using NodeKind = onegen::NodeKind;
constexpr std::array<const char*, 5> scopes{
    "RemoteDevice", "DataStream", "Device", "Interface", "System/TL"};
std::string parameterScope(int module, const onegen::producer::NodeObservation& observed) {
  return module == 1 ? "DataStream:" + std::to_string(observed.stream_index) + ":" + std::to_string(observed.stream_generation) : scopes.at(module);
}
int parameterModule(const std::string& scope) {
  if (scope == "RemoteDevice") return 0;
  if (scope == "Device") return 2;
  if (scope == "Interface") return 3;
  if (scope == "System/TL") return 4;
  if (scope.starts_with("DataStream:")) {
    const auto split = scope.find(':', 11);
    if (split == std::string::npos) return -1;
    std::uint32_t index; std::uint64_t generation;
    const auto first = std::from_chars(scope.data()+11, scope.data()+split, index);
    const auto second = std::from_chars(scope.data()+split+1, scope.data()+scope.size(), generation);
    if (first.ec == std::errc{} && first.ptr == scope.data()+split && second.ec == std::errc{}
        && second.ptr == scope.data()+scope.size() && generation
        && scope == "DataStream:"+std::to_string(index)+":"+std::to_string(generation)) return 1;
  }
  return -1;
}

DeviceParameterResult failure(Error error, std::string message) {
  DeviceParameterResult result; result.error = error; result.message = std::move(message); return result;
}
std::optional<Kind> kind(NodeKind value) {
  switch (value) {
    case NodeKind::Integer: return Kind::Integer;
    case NodeKind::Float: return Kind::Float;
    case NodeKind::Boolean: return Kind::Boolean;
    case NodeKind::String: return Kind::String;
    case NodeKind::Enumeration: return Kind::Enumeration;
    case NodeKind::Command: return Kind::Command;
    default: return {};
  }
}
std::optional<DeviceParameterValue> scalar(const onegen::FeatureNode& node) {
  if (!node.readable || !node.observation_error.empty()) return {};
  switch (node.kind) {
    case NodeKind::Integer: return node.int_value;
    case NodeKind::Float: return std::isfinite(node.float_value) ? std::optional<DeviceParameterValue>{node.float_value} : std::nullopt;
    case NodeKind::Boolean: return node.bool_value;
    case NodeKind::String: return node.string_value;
    case NodeKind::Enumeration: return node.enum_value;
    default: return {};
  }
}
bool sameObservation(const DeviceParameterDescriptor& a, const DeviceParameterDescriptor& b) {
  const auto fields = [](const auto& d) {
    return std::tie(d.kind, d.displayName, d.description, d.unit, d.available, d.readable,
        d.writable, d.unavailableReason, d.observationError, d.value, d.integerMinimum,
        d.integerMaximum, d.integerIncrement, d.floatMinimum, d.floatMaximum,
        d.floatIncrement, d.enumEntries, d.target.selectors, d.categoryPath, d.visibility,
        d.representation, d.selectorRelations, d.producerShared);
  };
  return fields(a) == fields(b);
}
std::pair<std::string, std::string> setter(Kind type, const DeviceParameterValue& value) {
  switch (type) {
    case Kind::Integer: return {"int", std::to_string(std::get<std::int64_t>(value))};
    case Kind::Float: {
      std::array<char, 64> buffer;
      const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::get<double>(value));
      if (converted.ec != std::errc{}) throw std::runtime_error("Float encoding failed.");
      return {"float", std::string(buffer.data(), converted.ptr)};
    }
    case Kind::Boolean: return {"bool", std::get<bool>(value) ? "1" : "0"};
    case Kind::String: return {"string", std::get<std::string>(value)};
    case Kind::Enumeration: return {"enum", std::get<std::string>(value)};
    default: throw std::runtime_error("No scalar setter for this node kind.");
  }
}
template<class Callback, class Value> void complete(const Callback& callback, Value value) noexcept {
  try { callback(std::move(value)); }
  catch (...) { qWarning() << "[GenICam] parameter consumer callback threw."; }
}
const onegen::FeatureNode* findNode(const onegen::FeatureNode& root, std::string_view name) {
  if (root.name == name) return &root;
  for (const auto& child : root.children)
    if (const auto* found = findNode(child, name)) return found;
  return nullptr;
}
std::string categorySegment(const onegen::FeatureNode& node) {
  auto segment = node.display_name.empty() ? node.name : node.display_name;
  if (segment.size() > deviceParameterCategorySegmentLimit)
    segment.resize(deviceParameterCategorySegmentLimit);
  return segment;
}
std::vector<std::string> categoryPath(const onegen::FeatureNode& root, std::string_view name) {
  std::vector<std::string> path;
  std::function<bool(const onegen::FeatureNode&)> visit = [&](const auto& node) {
    const bool category = node.kind == NodeKind::Category;
    if (category) path.push_back(categorySegment(node));
    if (node.name == name) return true;
    for (const auto& child : node.children)
      if (visit(child)) return true;
    if (category) path.pop_back();
    return false;
  };
  return visit(root) ? path : std::vector<std::string>{};
}
DeviceParameterRepresentation representationOf(const onegen::FeatureNode& node) {
  switch (node.kind) {
    case NodeKind::Enumeration: return DeviceParameterRepresentation::Choice;
    case NodeKind::Boolean: return DeviceParameterRepresentation::Toggle;
    case NodeKind::String: return DeviceParameterRepresentation::Text;
    case NodeKind::Integer:
    case NodeKind::Float:
      if (node.representation == "HexNumber" || node.representation == "IPV4Address"
          || node.representation == "MACAddress")
        return DeviceParameterRepresentation::Text;
      return DeviceParameterRepresentation::Number;
    case NodeKind::Command: return DeviceParameterRepresentation::Automatic;
    default: return DeviceParameterRepresentation::Automatic;
  }
}
void addRelation(DeviceParameterDescriptor& descriptor, const std::string& name,
                 DeviceParameterSelectorRole role) {
  if (name.empty() || name.size() > deviceParameterSelectorRelationNameLimit) return;
  for (const auto& relation : descriptor.selectorRelations)
    if (relation.name == name) return;
  if (descriptor.selectorRelations.size() >= deviceParameterSelectorRelationLimit) return;
  descriptor.selectorRelations.push_back({name, role});
}
void applyTreeStructure(DeviceParameterDescriptor& descriptor, const onegen::FeatureNode& node,
                        const onegen::FeatureNode* root, int module) {
  descriptor.visibility = node.visible ? DeviceParameterVisibility::Primary
                                       : DeviceParameterVisibility::Hidden;
  descriptor.representation = representationOf(node);
  descriptor.categoryPath = root ? categoryPath(*root, node.name) : std::vector<std::string>{};
  if (descriptor.categoryPath.size() > deviceParameterCategoryDepthLimit)
    descriptor.categoryPath.resize(deviceParameterCategoryDepthLimit);
  descriptor.selectorRelations.clear();
  for (const auto& selector : node.selecting_features)
    addRelation(descriptor, selector.name, DeviceParameterSelectorRole::SelectedBy);
  if (root) {
    std::function<void(const onegen::FeatureNode&)> scan = [&](const auto& candidate) {
      for (const auto& selector : candidate.selecting_features)
        if (selector.name == node.name)
          addRelation(descriptor, candidate.name, DeviceParameterSelectorRole::Selects);
      for (const auto& child : candidate.children) scan(child);
    };
    scan(*root);
  }
  descriptor.producerShared = module == 4;
}
void fillDescriptor(DeviceParameterDescriptor& result, const onegen::FeatureNode& node) {
  result.kind = kind(node.kind).value();
  result.displayName = node.display_name; result.description = node.description; result.unit = node.unit;
  result.available = node.readable || node.writable;
  result.readable = node.readable && result.kind != Kind::Command;
  result.writable = node.writable;
  result.observationError = node.observation_error;
  result.value = scalar(node);
  if (!result.available) result.unavailableReason = "Node is not currently accessible.";
  if (result.kind == Kind::Integer) {
    result.integerMinimum = node.int_min; result.integerMaximum = node.int_max; result.integerIncrement = node.int_inc;
  } else if (result.kind == Kind::Float) {
    if (std::isfinite(node.float_min)) result.floatMinimum = node.float_min;
    if (std::isfinite(node.float_max)) result.floatMaximum = node.float_max;
    if (std::isfinite(node.float_inc) && node.float_inc > 0) result.floatIncrement = node.float_inc;
    if ((node.readable && !std::isfinite(node.float_value)) || !std::isfinite(node.float_min)
        || !std::isfinite(node.float_max) || !std::isfinite(node.float_inc)) {
      if (!result.observationError.empty()) result.observationError += "; ";
      result.observationError += "Non-finite numeric observation.";
      result.value.reset();
    }
  } else if (result.kind == Kind::Enumeration) result.enumEntries = node.enum_entries;
}
std::size_t parameterQueueLimit() {
  const auto configured = qEnvironmentVariableIntValue("ONEGEN_PARAMETER_CHANGE_QUEUE_LIMIT");
  if (configured <= 0) return deviceParameterChangeQueueLimit;
  return std::min<std::size_t>(static_cast<std::size_t>(configured), deviceParameterChangeQueueLimit);
}
std::string descriptorKey(const DeviceParameterDescriptor& descriptor) {
  return descriptor.target.scope + "/" + descriptor.target.name;
}
}

/** @brief One admitted change subscription with a module-owned delivery thread. */
struct OneGenSourceController::ParameterWatch {
  ParameterWatch(std::string connection, std::string subscription, ChangeListener listener)
      : connectionId(std::move(connection)), subscriptionId(std::move(subscription)),
        delivery(std::move(listener)), queue(parameterQueueLimit()) {}
  ~ParameterWatch() { stopThread(); }

  void startThread() {
    thread = std::thread([this] { run(); });
  }
  void stopThread() {
    {
      std::lock_guard lock(mutex);
      stop = true;
    }
    condition.notify_all();
    if (thread.joinable()) thread.join();
  }
  void run() {
    std::unique_lock lock(mutex);
    while (!stop) {
      condition.wait_for(lock, std::chrono::milliseconds{100}, [&] { return stop || !queue.empty(); });
      if (stop) break;
      while (!queue.empty()) {
        auto change = queue.pop();
        if (!change) break;
        lock.unlock();
        bool failed = false;
        try { delivery(std::move(*change)); }
        catch (...) { failed = true; }
        lock.lock();
        if (failed) { stop = true; break; }
      }
    }
  }
  void resetDescriptors(std::vector<DeviceParameterDescriptor> values, std::uint64_t nextRevision) {
    std::lock_guard lock(mutex);
    descriptors.clear();
    for (auto& descriptor : values) descriptors[descriptorKey(descriptor)] = std::move(descriptor);
    revision = nextRevision;
  }
  void enqueueSnapshot(std::vector<DeviceParameterDescriptor> values, bool resync, std::string reason) {
    std::lock_guard lock(mutex);
    if (stop || terminal) return;
    descriptors.clear();
    std::uint64_t nextRevision = revision;
    for (auto& descriptor : values) {
      nextRevision = std::max(nextRevision, descriptor.revision);
      descriptors[descriptorKey(descriptor)] = descriptor;
    }
    revision = nextRevision;
    DeviceParameterChange change;
    change.kind = DeviceParameterChangeKind::Snapshot;
    change.resync = resync;
    change.connectionId = connectionId;
    change.subscriptionId = subscriptionId;
    change.sequence = ++sequence;
    change.revision = revision;
    change.descriptors = std::move(values);
    change.overflowReason = std::move(reason);
    if (resync) {
      auto overflow = change.overflowReason;
      queue.replaceWithSnapshot(std::move(change), std::move(overflow));
    } else if (!queue.push(std::move(change))) {
      DeviceParameterChange replacement;
      replacement.kind = DeviceParameterChangeKind::Snapshot;
      replacement.connectionId = connectionId;
      replacement.subscriptionId = subscriptionId;
      replacement.sequence = ++sequence;
      replacement.revision = revision;
      for (const auto& [key, descriptor] : descriptors) {
        static_cast<void>(key);
        replacement.descriptors.push_back(descriptor);
      }
      queue.replaceWithSnapshot(std::move(replacement), "Parameter change queue overflow.");
    }
    condition.notify_one();
  }
  void enqueueTerminal(Error error, std::string message) {
    std::lock_guard lock(mutex);
    if (stop || terminal) return;
    terminal = true;
    const auto terminalMessage = message.empty()
        ? std::string("The parameter subscription ended.") : message;
    DeviceParameterChange change;
    change.kind = DeviceParameterChangeKind::Delta;
    change.connectionId = connectionId;
    change.subscriptionId = subscriptionId;
    change.sequence = ++sequence;
    change.revision = revision;
    change.error = error;
    change.message = terminalMessage;
    if (!queue.push(std::move(change))) {
      DeviceParameterChange replacement;
      replacement.kind = DeviceParameterChangeKind::Snapshot;
      replacement.connectionId = connectionId;
      replacement.subscriptionId = subscriptionId;
      replacement.sequence = ++sequence;
      replacement.revision = revision;
      replacement.error = error;
      replacement.message = terminalMessage;
      queue.replaceWithSnapshot(std::move(replacement), "Parameter change queue overflow.");
    }
    condition.notify_one();
  }
  void enqueueDescriptor(const DeviceParameterDescriptor& descriptor) {
    std::lock_guard lock(mutex);
    if (stop || terminal) return;
    descriptors[descriptorKey(descriptor)] = descriptor;
    revision = std::max(revision, descriptor.revision);
    DeviceParameterChange change;
    change.kind = DeviceParameterChangeKind::Delta;
    change.connectionId = connectionId;
    change.subscriptionId = subscriptionId;
    change.sequence = ++sequence;
    change.revision = revision;
    change.descriptors.push_back(descriptor);
    if (!queue.push(std::move(change))) {
      DeviceParameterChange replacement;
      replacement.kind = DeviceParameterChangeKind::Snapshot;
      replacement.connectionId = connectionId;
      replacement.subscriptionId = subscriptionId;
      replacement.sequence = ++sequence;
      replacement.revision = revision;
      for (const auto& [key, cached] : descriptors) {
        static_cast<void>(key);
        replacement.descriptors.push_back(cached);
      }
      queue.replaceWithSnapshot(std::move(replacement), "Parameter change queue overflow.");
    }
    condition.notify_one();
  }

  std::string connectionId;
  std::string subscriptionId;
  ChangeListener delivery;
  DeviceParameterChangeQueue queue;
  std::mutex mutex;
  std::condition_variable condition;
  std::thread thread;
  bool stop = false;
  bool terminal = false;
  std::uint64_t sequence = 0;
  std::uint64_t revision = 0;
  std::map<std::string, DeviceParameterDescriptor> descriptors;
  QFuture<void> initialWork;
};

std::vector<OneGenSourceController*> OneGenSourceController::parameterControllers() const {
  std::vector<OneGenSourceController*> result{const_cast<OneGenSourceController*>(this)};
  std::lock_guard lock(mutex_);
  for (const auto& [id, child] : children_) result.push_back(child.controller.get());
  return result;
}
DeviceParameterResult OneGenSourceController::parameterAdmission() {
  Q_ASSERT(QThread::currentThread() == thread());
  if (control_blocked_)
    return failure(Error::Busy, "Discovery, Interface or control work is active.");
  std::set<std::string> connections;
  for (auto* controller : parameterControllers()) {
    if (controller->gui_busy_.load() || controller->external_busy_.load())
      return failure(Error::Busy, "A GUI or API control operation is active.");
    std::lock_guard lock(controller->mutex_);
    if (controller->owned_lease_) connections.insert(controller->connection_id_.toStdString());
  }
  if (interface_) connections.insert(interface_->connectionId.toStdString());
  // Revisions retain observations only for active connection generations.
  std::lock_guard observationLock(parameter_mutex_);
  for (auto it = parameter_observations_.begin(); it != parameter_observations_.end();) {
    if (!connections.count(it->second.target.connectionId)) it = parameter_observations_.erase(it);
    else ++it;
  }
  return {};
}
DeviceParameterDescriptor OneGenSourceController::recordParameterObservation(
    DeviceParameterDescriptor descriptor) {
  const auto key = descriptor.target.connectionId + "/" + descriptor.target.scope + "/" + descriptor.target.name;
  std::lock_guard lock(parameter_mutex_);
  const auto previous = parameter_observations_.find(key);
  descriptor.revision = previous != parameter_observations_.end() && sameObservation(previous->second, descriptor)
      ? previous->second.revision : ++parameter_revision_;
  parameter_observations_[key] = descriptor;
  return descriptor;
}
DeviceParameterDescriptor OneGenSourceController::observeParameter(
    const std::shared_ptr<onegen::producer::Lease>& lease, const std::string& connection,
    int module, const onegen::FeatureNode& node, const onegen::FeatureNode* moduleRoot,
    bool completeContext, const std::string& scopeOverride) {
  DeviceParameterDescriptor result;
  result.target = {connection, scopeOverride.empty() ? scopes.at(module) : scopeOverride, node.name, {}};
  fillDescriptor(result, node);
  // Resolve transitive selecting features using existing reads. Never set a selector.
  std::map<std::string, DeviceParameterValue> selectors;
  std::set<std::string> active{node.name};
  std::function<void(const onegen::FeatureNode&)> collect = [&](const auto& selected) {
    for (const auto& relation : selected.selecting_features) {
      if (completeContext) {
        const auto value = scalar(relation);
        if (!value) throw std::runtime_error("Selecting feature cannot be observed: " + relation.name);
        selectors.emplace(relation.name, *value); continue;
      }
      if (active.count(relation.name)) throw std::runtime_error("Cyclic selector context.");
      if (selectors.count(relation.name)) continue;
      if (selectors.size() + active.size() > 128) throw std::runtime_error("Selector context exceeds the observation bound.");
      const auto observed = lease->read_node(module, relation.name);
      const auto value = scalar(observed);
      if (observed.name != relation.name || !value) throw std::runtime_error("Selecting feature cannot be observed: " + relation.name);
      active.insert(relation.name); collect(observed); active.erase(relation.name);
      selectors.emplace(relation.name, *value);
    }
  };
  try { collect(node); }
  catch (const std::exception& error) {
    result.available = false; result.writable = false;
    result.value.reset();
    result.unavailableReason = "Complete selector context is unavailable.";
    if (!result.observationError.empty()) result.observationError += "; ";
    result.observationError += error.what();
  }
  for (auto& [name, value] : selectors) result.target.selectors.push_back({name, value});
  applyTreeStructure(result, node, moduleRoot, module);
  if (module == 1) {
    std::lock_guard lock(parameter_mutex_);
    for (auto it = parameter_observations_.begin(); it != parameter_observations_.end();) {
      if (it->second.target.connectionId == connection && it->second.target.scope.starts_with("DataStream:")
          && it->second.target.scope != result.target.scope) it = parameter_observations_.erase(it);
      else ++it;
    }
  }
  return recordParameterObservation(std::move(result));
}
DeviceParameterDescriptor OneGenSourceController::observeCoordinatorParameter(
    const ParameterEndpoint& endpoint, int module, const onegen::FeatureNode& node,
    const onegen::FeatureNode* moduleRoot, const std::string& scopeOverride) {
  DeviceParameterDescriptor result;
  result.target = {endpoint.connectionId, scopeOverride.empty() ? scopes.at(module) : scopeOverride,
                   node.name, {}};
  fillDescriptor(result, node);
  for (const auto& selector : node.selecting_features) {
    const auto value = scalar(selector);
    if (!value) {
      result.available = false; result.writable = false;
      result.value.reset();
      result.unavailableReason = "Complete selector context is unavailable.";
      if (!result.observationError.empty()) result.observationError += "; ";
      result.observationError += "Selecting feature cannot be observed: " + selector.name;
      result.target.selectors.clear();
      break;
    }
    result.target.selectors.push_back({selector.name, *value});
  }
  applyTreeStructure(result, node, moduleRoot, module);
  return recordParameterObservation(std::move(result));
}
std::optional<OneGenSourceController::ParameterEndpoint>
OneGenSourceController::resolveParameterEndpoint(const std::string& connectionId) const {
  if (connectionId.empty()) return std::nullopt;
  std::lock_guard lock(mutex_);
  if (interface_ && interface_->connectionId.toStdString() == connectionId) {
    ParameterEndpoint endpoint;
    endpoint.isInterface = true;
    endpoint.connectionId = connectionId;
    endpoint.info = interface_->info;
    endpoint.owner = const_cast<OneGenSourceController*>(this);
    return endpoint;
  }
  for (const auto& [key, child] : children_) {
    std::lock_guard cameraLock(child.controller->mutex_);
    if (child.controller->connection_id_.toStdString() != connectionId
        || !child.controller->owned_lease_) continue;
    ParameterEndpoint endpoint;
    endpoint.connectionId = connectionId;
    endpoint.lease = child.controller->owned_lease_;
    endpoint.owner = child.controller.get();
    return endpoint;
  }
  if (owned_lease_ && connection_id_.toStdString() == connectionId) {
    ParameterEndpoint endpoint;
    endpoint.connectionId = connectionId;
    endpoint.lease = owned_lease_;
    endpoint.owner = const_cast<OneGenSourceController*>(this);
    return endpoint;
  }
  return std::nullopt;
}
DeviceParameterInventory OneGenSourceController::connectionInventory(
    const std::string& connectionId, std::string* error) {
  DeviceParameterInventory result;
  const auto endpoint = resolveParameterEndpoint(connectionId);
  if (!endpoint) {
    if (error) *error = "Connection generation is not active.";
    result.result = failure(Error::Disconnected, "Connection generation is not active.");
    return result;
  }
  try {
    const auto flatten = [&](int module, const onegen::FeatureNode& root,
                             const onegen::FeatureNode* moduleRoot, auto& self) -> void {
      if (kind(root.kind)) {
        DeviceParameterDescriptor descriptor;
        if (endpoint->isInterface) {
          descriptor = observeCoordinatorParameter(*endpoint, module, root, moduleRoot);
        } else if (module == 1) {
          const auto observed = endpoint->lease->observe_node(module, root.name);
          descriptor = observeParameter(endpoint->lease, connectionId, module, observed.node,
              moduleRoot, true, parameterScope(module, observed));
        } else {
          descriptor = observeParameter(endpoint->lease, connectionId, module, root, moduleRoot);
        }
        result.parameters.push_back(std::move(descriptor));
      }
      for (const auto& child : root.children) self(module, child, moduleRoot, self);
    };
    if (endpoint->isInterface) {
      if (!coordinator_) throw std::runtime_error("Device control has no coordinator.");
      std::string partialError;
      try {
        const auto interfaceRoot = coordinator_->interface_features(endpoint->info);
        flatten(3, interfaceRoot, &interfaceRoot, flatten);
      } catch (const std::exception& exception) {
        partialError = exception.what();
      }
      try {
        const auto systemRoot = coordinator_->system_features(endpoint->info);
        flatten(4, systemRoot, &systemRoot, flatten);
      } catch (const std::exception& exception) {
        if (!partialError.empty()) partialError += "; ";
        partialError += exception.what();
      }
      if (result.parameters.empty() && !partialError.empty())
        throw std::runtime_error(partialError);
    } else {
      if (!endpoint->lease || !endpoint->lease->alive())
        throw std::runtime_error("Device lease is closed.");
      const auto modules = endpoint->lease->describe_features();
      for (int module = 0; module < 3 && module < static_cast<int>(modules.size()); ++module) {
        const auto& root = modules[module].second;
        flatten(module, root, &root, flatten);
      }
    }
  } catch (const std::exception& exception) {
    result.result = failure(Error::BackendFailure, exception.what());
    result.parameters.clear();
    if (error) *error = exception.what();
  } catch (...) {
    result.result = failure(Error::BackendFailure, "Unexpected feature inventory failure.");
    result.parameters.clear();
    if (error) *error = "Unexpected feature inventory failure.";
  }
  return result;
}

DeviceParameterResult OneGenSourceController::requestParameters(InventoryCompletion completion) {
  if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
  const auto admission = parameterAdmission();
  if (admission.error != Error::None) return admission;
  std::vector<std::string> connections;
  {
    std::lock_guard lock(mutex_);
    if (interface_) connections.push_back(interface_->connectionId.toStdString());
    std::vector<const Child*> cameras;
    for (const auto& [id, child] : children_) cameras.push_back(&child);
    std::sort(cameras.begin(), cameras.end(),
        [](const Child* left, const Child* right) { return left->index < right->index; });
    for (const auto* child : cameras) {
      std::lock_guard cameraLock(child->controller->mutex_);
      if (child->controller->owned_lease_ && child->controller->owned_lease_->alive())
        connections.push_back(child->controller->connection_id_.toStdString());
    }
    if (owned_lease_ && owned_lease_->alive())
      connections.push_back(connection_id_.toStdString());
  }
  if (connections.empty()) return failure(Error::Disconnected, "Device is disconnected.");
  std::sort(connections.begin(), connections.end());
  connections.erase(std::unique(connections.begin(), connections.end()), connections.end());
  external_busy_.store(true);
  const auto controllers = parameterControllers();
  for (auto* controller : controllers) { controller->external_busy_.store(true); emit controller->controlChanged(); }
  tool_work_ = QtConcurrent::run([this, controllers, connections, completion = std::move(completion)] {
    DeviceParameterInventory result;
    try {
      for (const auto& connection : connections) {
        std::string error;
        auto part = connectionInventory(connection, &error);
        if (part.result.error != Error::None) { result.result = part.result; result.parameters.clear(); break; }
        result.parameters.insert(result.parameters.end(),
            std::make_move_iterator(part.parameters.begin()),
            std::make_move_iterator(part.parameters.end()));
      }
    } catch (const std::exception& error) { result.result = failure(Error::BackendFailure, error.what()); result.parameters.clear(); }
    catch (...) { result.result = failure(Error::BackendFailure, "Unexpected feature inventory failure."); result.parameters.clear(); }
    for (auto* controller : controllers) { controller->external_busy_.store(false); emit controller->controlChanged(); }
    complete(completion, std::move(result));
  });
  return {};
}
DeviceParameterResult OneGenSourceController::requestReadParameter(DeviceParameterTarget target, IDeviceParameterControl::Completion completion) {
  return requestParameter(std::move(target), {}, {}, false, std::move(completion));
}
DeviceParameterResult OneGenSourceController::requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
    std::optional<std::uint64_t> revision, IDeviceParameterControl::Completion completion) {
  return requestParameter(std::move(target), std::move(value), revision, true, std::move(completion));
}
DeviceParameterResult OneGenSourceController::requestExecuteParameter(DeviceParameterTarget target,
    std::optional<std::uint64_t> revision, IDeviceParameterControl::Completion completion) {
  return requestParameter(std::move(target), {}, revision, true, std::move(completion));
}
DeviceParameterResult OneGenSourceController::requestParameter(DeviceParameterTarget target,
    std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
    bool mutate, IDeviceParameterControl::Completion completion) {
  if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
  const auto admission = parameterAdmission();
  if (admission.error != Error::None) return admission;
  for (const auto* text : {&target.connectionId, &target.scope, &target.name})
    if (text->empty() || text->find('\0') != std::string::npos) return failure(Error::InvalidArgument, "Exact target fields are required.");
  const int module = parameterModule(target.scope);
  if (module < 0) return failure(Error::Unsupported,
      "Exact RemoteDevice, Device, Interface, System/TL or qualified DataStream scope required.");
  const auto endpoint = resolveParameterEndpoint(target.connectionId);
  if (!endpoint) {
    bool connected = false;
    {
      std::lock_guard lock(mutex_);
      connected = interface_ || owned_lease_ || !children_.empty();
    }
    return failure(connected ? Error::Conflict : Error::Disconnected, "Connection generation is not active.");
  }
  if (endpoint->isInterface) {
    if (module != 3 && module != 4)
      return failure(Error::Unsupported, "The Interface connection exposes only Interface and System/TL scopes.");
    return requestCoordinatorParameter(*endpoint, std::move(target), std::move(value), revision,
        mutate, std::move(completion));
  }
  if (module == 3 || module == 4)
    return failure(Error::Conflict,
        "Camera leases cannot access Interface or System/TL features; use the Interface connection.");
  OneGenSourceController* selected = endpoint->owner;
  const auto lease = endpoint->lease;
  external_busy_.store(true); selected->external_busy_.store(true); emit selected->controlChanged();
  tool_work_ = QtConcurrent::run([this, selected, lease, module, target = std::move(target), value = std::move(value), revision, mutate, completion = std::move(completion)] {
    DeviceParameterResult result;
    bool dispatched = false;
    try {
      const auto root = lease->module_features(module);
      const auto* current = findNode(root, target.name);
      if (!current || current->name != target.name) result = failure(Error::NotFound, "Parameter was not found.");
      else if (!kind(current->kind)) result = failure(Error::Unsupported, "Node is not a typed parameter.");
      else {
        auto observed = lease->observe_node(module, target.name);
        auto descriptor = observeParameter(lease, target.connectionId, module, observed.node, &root,
            true, parameterScope(module, observed));
        result.descriptor = descriptor;
        if (target.scope != descriptor.target.scope || target.selectors != descriptor.target.selectors) result.error = Error::Conflict;
        else if (mutate) {
          result.error = validateDeviceParameterWrite(descriptor, target, value, revision);
          if (result.error == Error::None) {
            const auto operation = value ? setter(descriptor.kind, *value) : std::pair<std::string, std::string>{"execute", ""};
            dispatched = true;
            const auto written = lease->write_node_checked(module, target.name, operation.first, operation.second, observed, revision.has_value());
            using Status = onegen::producer::NodeWriteStatus;
            using Effect = onegen::producer::NodeWriteEffect;
            result.effect = written.effect == Effect::Applied ? DeviceParameterEffect::Applied
                : written.effect == Effect::Unknown ? DeviceParameterEffect::Unknown : DeviceParameterEffect::None;
            result.error = written.status == Status::Success ? Error::None : written.status == Status::Conflict ? Error::Conflict
                : written.status == Status::Unavailable ? Error::Unavailable : Error::BackendFailure;
            result.message = written.message;
            result.descriptor.reset();
            if (written.observation) result.descriptor = observeParameter(lease, target.connectionId, module,
                written.observation->node, &root, true, parameterScope(module, *written.observation));
            // The producer retains trigger/receive ordering; acquisition commands have no extra feature reads.
            if (result.effect == DeviceParameterEffect::Applied && !(module == 0 && target.name == "TriggerSoftware" && !value)) selected->refreshControlSnapshot();
          }
        } else if (!descriptor.available) result.error = Error::Unavailable;
        else if (!descriptor.observationError.empty()) result.error = Error::BackendFailure;
        else if (!descriptor.readable) result.error = Error::Unreadable;
        if (result.error != Error::None && result.message.empty()) result.message = "Feature access, value, selector context or revision validation failed.";
      }
    } catch (const std::exception& error) {
      result.error = Error::BackendFailure; result.message = error.what();
      if (dispatched && result.effect != DeviceParameterEffect::Applied) {
        result.effect = DeviceParameterEffect::Unknown; result.descriptor.reset();
      }
    } catch (...) {
      result.error = Error::BackendFailure; result.message = "Unexpected parameter backend failure.";
      if (dispatched && result.effect != DeviceParameterEffect::Applied) { result.effect = DeviceParameterEffect::Unknown; result.descriptor.reset(); }
    }
    selected->external_busy_.store(false); external_busy_.store(false);
    emit selected->controlChanged(); emit controlChanged();
    if (result.effect == DeviceParameterEffect::Applied && result.descriptor)
      notifyParameterChanged(target.connectionId, module, *result.descriptor);
    complete(completion, std::move(result));
  });
  return {};
}
DeviceParameterResult OneGenSourceController::requestCoordinatorParameter(
    const ParameterEndpoint& endpoint, DeviceParameterTarget target,
    std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
    bool mutate, IDeviceParameterControl::Completion completion) {
  const int module = parameterModule(target.scope);
  const bool system = module == 4;
  auto* selected = endpoint.owner ? endpoint.owner : this;
  if (mutate && !system) {
    bool camerasLeased = false;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [key, child] : children_) {
        std::lock_guard cameraLock(child.controller->mutex_);
        camerasLeased = camerasLeased || bool(child.controller->owned_lease_);
      }
    }
    if (camerasLeased)
      return failure(Error::Unavailable,
          "Disconnect all cameras on this Interface before changing Interface settings.");
  }
  if (!coordinator_) return failure(Error::Unavailable, "Device control has no coordinator.");
  external_busy_.store(true); selected->external_busy_.store(true); emit selected->controlChanged();
  tool_work_ = QtConcurrent::run([this, selected, endpoint, module, system,
                                  target = std::move(target), value = std::move(value), revision,
                                  mutate, completion = std::move(completion)] {
    DeviceParameterResult result;
    bool dispatched = false;
    try {
      std::lock_guard writeLock(parameter_write_mutex_);
      auto root = system ? coordinator_->system_features(endpoint.info)
                         : coordinator_->interface_features(endpoint.info);
      const auto* node = findNode(root, target.name);
      if (!node || node->name != target.name) result = failure(Error::NotFound, "Parameter was not found.");
      else if (!kind(node->kind)) result = failure(Error::Unsupported, "Node is not a typed parameter.");
      else {
        auto descriptor = observeCoordinatorParameter(endpoint, module, *node, &root);
        result.descriptor = descriptor;
        if (target.scope != descriptor.target.scope || target.selectors != descriptor.target.selectors) result.error = Error::Conflict;
        else if (mutate) {
          result.error = validateDeviceParameterWrite(descriptor, target, value, revision);
          if (result.error == Error::None) {
            const auto operation = value ? setter(descriptor.kind, *value) : std::pair<std::string, std::string>{"execute", ""};
            dispatched = true;
            auto writtenRoot = system
                ? coordinator_->write_system(endpoint.info, target.name, operation.first, operation.second)
                : coordinator_->write_interface(endpoint.info, target.name, operation.first, operation.second);
            const auto* written = findNode(writtenRoot, target.name);
            result.effect = DeviceParameterEffect::Applied;
            result.error = Error::None;
            result.message.clear();
            result.descriptor.reset();
            if (written)
              result.descriptor = observeCoordinatorParameter(endpoint, module, *written, &writtenRoot);
          }
        } else if (!descriptor.available) result.error = Error::Unavailable;
        else if (!descriptor.observationError.empty()) result.error = Error::BackendFailure;
        else if (!descriptor.readable) result.error = Error::Unreadable;
        if (result.error != Error::None && result.message.empty())
          result.message = "Feature access, value, selector context or revision validation failed.";
      }
    } catch (const std::exception& error) {
      result.error = Error::BackendFailure; result.message = error.what();
      if (dispatched && result.effect != DeviceParameterEffect::Applied) {
        result.effect = DeviceParameterEffect::Unknown; result.descriptor.reset();
      }
    } catch (...) {
      result.error = Error::BackendFailure; result.message = "Unexpected parameter backend failure.";
      if (dispatched && result.effect != DeviceParameterEffect::Applied) {
        result.effect = DeviceParameterEffect::Unknown; result.descriptor.reset();
      }
    }
    selected->external_busy_.store(false); external_busy_.store(false);
    emit selected->controlChanged(); emit controlChanged();
    if (result.effect == DeviceParameterEffect::Applied && result.descriptor)
      notifyParameterChanged(endpoint.connectionId, module, *result.descriptor);
    complete(completion, std::move(result));
  });
  return {};
}

DeviceParameterResult OneGenSourceController::requestParameterChanges(
    std::string connectionId, std::string subscriptionId, ChangeListener delivery) {
  if (!delivery) return failure(Error::InvalidArgument, "A change listener is required.");
  const auto identity = validateDeviceParameterSubscription(connectionId, subscriptionId);
  if (identity != Error::None) return failure(identity, "The change subscription identity is invalid.");
  const auto admission = parameterAdmission();
  if (admission.error != Error::None) return admission;
  const auto endpoint = resolveParameterEndpoint(connectionId);
  if (!endpoint) return failure(Error::Disconnected, "Connection generation is not active.");
  {
    std::lock_guard lock(parameter_watch_mutex_);
    if (parameter_watches_.size() >= deviceParameterSubscriptionLimit)
      return failure(Error::Busy, "The connection already has the maximum number of change subscriptions.");
    const auto key = connectionId + "/" + subscriptionId;
    if (parameter_watches_.count(key))
      return failure(Error::Conflict, "The change subscription is already active.");
  }
  auto* selected = endpoint->owner ? endpoint->owner : this;
  auto watch = std::make_shared<ParameterWatch>(connectionId, subscriptionId, std::move(delivery));
  {
    std::lock_guard lock(parameter_watch_mutex_);
    parameter_watches_[connectionId + "/" + subscriptionId] = watch;
  }
  external_busy_.store(true);
  selected->external_busy_.store(true);
  emit selected->controlChanged();
  watch->startThread();
  watch->initialWork = QtConcurrent::run([this, watch, selected, connectionId] {
    std::string error;
    const auto inventory = connectionInventory(connectionId, &error);
    if (inventory.result.error != Error::None) {
      watch->enqueueTerminal(inventory.result.error, inventory.result.message);
    } else {
      watch->enqueueSnapshot(inventory.parameters, false, {});
    }
    selected->external_busy_.store(false);
    external_busy_.store(false);
    emit selected->controlChanged();
  });
  {
    std::lock_guard lock(work_mutex_);
    std::erase_if(admitted_work_, [](const QFuture<void>& work) { return work.isFinished(); });
    admitted_work_.push_back(watch->initialWork);
  }
  return {};
}
DeviceParameterResult OneGenSourceController::releaseParameterChanges(
    std::string connectionId, std::string subscriptionId,
    IDeviceParameterControl::Completion completion) {
  std::shared_ptr<ParameterWatch> watch;
  {
    std::lock_guard lock(parameter_watch_mutex_);
    const auto key = connectionId + "/" + subscriptionId;
    const auto found = parameter_watches_.find(key);
    if (found == parameter_watches_.end())
      return failure(Error::NotFound, "The change subscription is not active.");
    watch = found->second;
    parameter_watches_.erase(found);
  }
  if (watch->initialWork.isStarted()) watch->initialWork.waitForFinished();
  watch->stopThread();
  complete(completion, DeviceParameterResult{});
  return {};
}
void OneGenSourceController::notifyParameterChanged(const std::string& connectionId, int module,
    const DeviceParameterDescriptor& descriptor) {
  std::vector<std::shared_ptr<ParameterWatch>> watches;
  {
    std::lock_guard lock(parameter_watch_mutex_);
    for (const auto& [key, watch] : parameter_watches_) {
      static_cast<void>(key);
      if (module == 4 || watch->connectionId == connectionId) watches.push_back(watch);
    }
  }
  for (const auto& watch : watches) watch->enqueueDescriptor(descriptor);
}
void OneGenSourceController::deliverParameterSnapshot(
    const std::shared_ptr<ParameterWatch>& watch,
    std::vector<DeviceParameterDescriptor> descriptors, bool resync, std::string reason) {
  watch->enqueueSnapshot(std::move(descriptors), resync, std::move(reason));
}
void OneGenSourceController::stopParameterWatches() {
  std::vector<std::shared_ptr<ParameterWatch>> watches;
  {
    std::lock_guard lock(parameter_watch_mutex_);
    for (auto& [key, watch] : parameter_watches_) {
      static_cast<void>(key);
      watches.push_back(std::move(watch));
    }
    parameter_watches_.clear();
  }
  for (const auto& watch : watches) {
    if (watch->initialWork.isStarted()) watch->initialWork.waitForFinished();
    watch->stopThread();
  }
}
