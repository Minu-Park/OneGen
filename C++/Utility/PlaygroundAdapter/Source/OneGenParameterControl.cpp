#include "OneGenSourceController.h"
#include "OneGenCoordinator.h"
#include <QDebug>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <charconv>
#include <set>
#include <tuple>

namespace {
using Error = DeviceParameterError;
using Kind = DeviceParameterKind;
using NodeKind = onegen::NodeKind;
constexpr std::array<const char*, 3> scopes{"RemoteDevice", "DataStream", "Device"};
std::string parameterScope(int module, const onegen::worker::NodeObservation& observed) {
  return module == 1 ? "DataStream:" + std::to_string(observed.stream_index) + ":" + std::to_string(observed.stream_generation) : scopes.at(module);
}
int parameterModule(const std::string& scope) {
  if (scope == "RemoteDevice") return 0;
  if (scope == "Device") return 2;
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
        d.floatIncrement, d.enumEntries, d.target.selectors);
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
}

std::vector<OneGenSourceController*> OneGenSourceController::parameterControllers() const {
  std::vector<OneGenSourceController*> result{const_cast<OneGenSourceController*>(this)};
  std::lock_guard lock(mutex_);
  for (const auto& [id, child] : children_) result.push_back(child.controller.get());
  return result;
}
DeviceParameterResult OneGenSourceController::parameterAdmission() {
  Q_ASSERT(QThread::currentThread() == thread());
  if (control_blocked_ || (tool_work_.isStarted() && !tool_work_.isFinished()))
    return failure(Error::Busy, "Discovery, Interface or control work is active.");
  std::set<std::string> connections;
  for (auto* controller : parameterControllers()) {
    if (controller->gui_busy_.load() || controller->external_busy_.load())
      return failure(Error::Busy, "A GUI or API control operation is active.");
    std::lock_guard lock(controller->mutex_);
    if (controller->owned_lease_) connections.insert(controller->connection_id_.toStdString());
  }
  // Revisions retain observations only for active connection generations.
  for (auto it = parameter_observations_.begin(); it != parameter_observations_.end();) {
    if (!connections.count(it->second.target.connectionId)) it = parameter_observations_.erase(it);
    else ++it;
  }
  return {};
}
DeviceParameterDescriptor OneGenSourceController::observeParameter(
    const std::shared_ptr<onegen::worker::Lease>& lease, const std::string& connection,
    int module, const onegen::FeatureNode& node, bool completeContext, const std::string& scopeOverride) {
  DeviceParameterDescriptor result;
  result.target = {connection, scopeOverride.empty() ? scopes.at(module) : scopeOverride, node.name, {}};
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
  const auto key = connection + "/" + result.target.scope + "/" + node.name;
  if (module == 1) for (auto it = parameter_observations_.begin(); it != parameter_observations_.end();) {
    if (it->second.target.connectionId == connection && it->second.target.scope.starts_with("DataStream:")
        && it->second.target.scope != result.target.scope) it = parameter_observations_.erase(it);
    else ++it;
  }
  const auto previous = parameter_observations_.find(key);
  result.revision = previous != parameter_observations_.end() && sameObservation(previous->second, result)
      ? previous->second.revision : ++parameter_revision_;
  parameter_observations_[key] = result;
  return result;
}

DeviceParameterResult OneGenSourceController::requestParameters(InventoryCompletion completion) {
  if (!completion) return failure(Error::InvalidArgument, "A completion is required.");
  const auto admission = parameterAdmission();
  if (admission.error != Error::None) return admission;
  std::vector<std::pair<std::shared_ptr<onegen::worker::Lease>, std::string>> connections;
  for (auto* controller : parameterControllers()) {
    std::lock_guard lock(controller->mutex_);
    if (controller->owned_lease_) connections.emplace_back(controller->owned_lease_, controller->connection_id_.toStdString());
  }
  if (connections.empty()) return failure(Error::Disconnected, "Device is disconnected.");
  external_busy_.store(true);
  const auto controllers = parameterControllers();
  for (auto* controller : controllers) { controller->external_busy_.store(true); emit controller->controlChanged(); }
  tool_work_ = QtConcurrent::run([this, controllers, connections, completion = std::move(completion)] {
    DeviceParameterInventory result;
    try {
      for (const auto& [lease, connection] : connections) {
        const auto modules = lease->describe_features();
        std::function<void(int, const onegen::FeatureNode&)> flatten = [&](int module, const auto& node) {
          if (kind(node.kind)) {
            if (module == 1) {
              const auto observed = lease->observe_node(module, node.name);
              result.parameters.push_back(observeParameter(lease, connection, module, observed.node, true, parameterScope(module, observed)));
            } else result.parameters.push_back(observeParameter(lease, connection, module, node));
          }
          for (const auto& child : node.children) flatten(module, child);
        };
        for (int module = 0; module < std::min<int>(3, modules.size()); ++module)
          flatten(module, modules[module].second);
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
  if (module < 0) return failure(Error::Unsupported, "Exact RemoteDevice, Device or qualified DataStream scope required.");
  OneGenSourceController* selected = nullptr;
  std::shared_ptr<onegen::worker::Lease> lease;
  bool connected = false;
  for (auto* controller : parameterControllers()) {
    std::lock_guard lock(controller->mutex_);
    connected = connected || bool(controller->owned_lease_);
    if (controller->owned_lease_ && controller->connection_id_.toStdString() == target.connectionId) {
      selected = controller; lease = controller->owned_lease_; break;
    }
  }
  if (!lease) return failure(connected ? Error::Conflict : Error::Disconnected, "Connection generation is not active.");
  external_busy_.store(true); selected->external_busy_.store(true); emit selected->controlChanged();
  tool_work_ = QtConcurrent::run([this, selected, lease, module, target = std::move(target), value = std::move(value), revision, mutate, completion = std::move(completion)] {
    DeviceParameterResult result;
    bool dispatched = false;
    try {
      const auto observed = lease->observe_node(module, target.name);
      const auto& node = observed.node;
      if (node.name != target.name) result = failure(Error::NotFound, "Parameter was not found.");
      else if (!kind(node.kind)) result = failure(Error::Unsupported, "Node is not a typed parameter.");
      else {
        auto current = observeParameter(lease, target.connectionId, module, node, true, parameterScope(module, observed));
        result.descriptor = current;
        if (target.scope != current.target.scope || target.selectors != current.target.selectors) result.error = Error::Conflict;
        else if (mutate) {
          result.error = validateDeviceParameterWrite(current, target, value, revision);
          if (result.error == Error::None) {
            const auto operation = value ? setter(current.kind, *value) : std::pair<std::string, std::string>{"execute", ""};
            dispatched = true;
            const auto written = lease->write_node_checked(module, target.name, operation.first, operation.second, observed, revision.has_value());
            using Status = onegen::worker::NodeWriteStatus;
            using Effect = onegen::worker::NodeWriteEffect;
            result.effect = written.effect == Effect::Applied ? DeviceParameterEffect::Applied
                : written.effect == Effect::Unknown ? DeviceParameterEffect::Unknown : DeviceParameterEffect::None;
            result.error = written.status == Status::Success ? Error::None : written.status == Status::Conflict ? Error::Conflict
                : written.status == Status::Unavailable ? Error::Unavailable : Error::BackendFailure;
            result.message = written.message;
            result.descriptor.reset();
            if (written.observation) result.descriptor = observeParameter(lease, target.connectionId, module,
                written.observation->node, true, parameterScope(module, *written.observation));
            // The worker retains trigger/receive ordering; acquisition commands have no extra feature reads.
            if (result.effect == DeviceParameterEffect::Applied && !(module == 0 && target.name == "TriggerSoftware" && !value)) selected->refreshControlSnapshot();
          }
        } else if (!current.available) result.error = Error::Unavailable;
        else if (!current.observationError.empty()) result.error = Error::BackendFailure;
        else if (!current.readable) result.error = Error::Unreadable;
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
    complete(completion, std::move(result));
  });
  return {};
}
