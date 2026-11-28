#include "OneGenSourceController.h"
#include "SingleCaptureDiagnostics.h"

#include "ProducerCoordinator.h"
#include "OneGenGraphicsFrameAdapter.h"
#include "DeviceRuntime/Contract/SessionFrame.h"
#include "engine/GraphicsFrameBuilder.h"
#include "engine/GraphicsWriteRegion.h"
#include "Internal/FrameTrace.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <exception>
#include <limits>
#include <optional>
#include <QtConcurrent>
#include <QThread>
#include <QUuid>

namespace {
Q_LOGGING_CATEGORY(onegenSourceDiagnosticLog, "diagnostics.OneGen", QtInfoMsg)
Q_LOGGING_CATEGORY(onegenAcquisitionDetailLog, "playground.device.acquisition.details", QtInfoMsg)

void logFirstAcquisitionInput(const onegen::Frame& source, std::size_t bytes, bool direct)
{
  if (!onegenAcquisitionDetailLog().isDebugEnabled()) return;
  qCDebug(onegenAcquisitionDetailLog).noquote() << QStringLiteral(
      "[GenICam] first input: direct=%1 frame=%2 payloadType=%3 PFNC=0x%4 width=%5 height=%6 "
      "stride=%7 bytes=%8 parts=%9 chunks=%10 scan3d=%11")
      .arg(direct).arg(source.frame_id).arg(source.payload_type).arg(QString::number(source.pixel_format, 16))
      .arg(source.width).arg(source.height).arg(source.stride).arg(bytes).arg(source.parts.size())
      .arg(source.chunks.size()).arg(source.scan3d_geometry.has_value());
  constexpr std::size_t maximumLoggedParts = 16;
  const auto count = std::min(source.parts.size(), maximumLoggedParts);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& part = source.parts[i];
    qCDebug(onegenAcquisitionDetailLog).noquote() << QStringLiteral(
        "[GenICam] first input part: ordinal=%1 type=%2 PFNC=0x%3 width=%4 height=%5 stride=%6 bytes=%7")
        .arg(i).arg(part.data_type).arg(QString::number(part.data_format, 16))
        .arg(part.width).arg(part.height).arg(part.stride).arg(part.payload.size());
  }
}

// Upper bound on frames in flight per source. The Host pool is the real slot bound; this only
// sizes the lease bookkeeping and never allocates frame memory.
constexpr std::uint32_t kMaximumInFlightFrames = 64;

std::optional<std::uint64_t> directSlotBytes(onegen::producer::Lease& lease, std::string& error)
{
  try {
    if (const auto payload = lease.payload_size()) return payload;
    error = "Stream payload size is unavailable.";
  } catch (const std::exception& exception) {
    error = exception.what();
  } catch (...) {
    error = "unknown feature read failure";
  }
  return std::nullopt;
}

std::optional<DeviceImageRejection> deviceImageRejection(const OneGenImageRejection& rejection) noexcept
{
  if (!rejection.failure) return DeviceImageRejection::NoImage;
  switch (*rejection.failure) {
  case GraphicsImageFailure::MissingBytes: return DeviceImageRejection::MissingBytes;
  case GraphicsImageFailure::InvalidDimensions: return DeviceImageRejection::InvalidDimensions;
  case GraphicsImageFailure::UnsupportedFormat: return DeviceImageRejection::UnsupportedFormat;
  case GraphicsImageFailure::InvalidStride: return DeviceImageRejection::InvalidStride;
  case GraphicsImageFailure::StrideBelowMinimum: return DeviceImageRejection::StrideBelowMinimum;
  case GraphicsImageFailure::ExtentOverflow: return DeviceImageRejection::ExtentOverflow;
  case GraphicsImageFailure::TruncatedBytes: return DeviceImageRejection::TruncatedBytes;
  case GraphicsImageFailure::OddWidth: return DeviceImageRejection::OddWidth;
  case GraphicsImageFailure::UnsupportedConversion: return DeviceImageRejection::UnsupportedConversion;
  case GraphicsImageFailure::StorageRefused: return DeviceImageRejection::StorageRefused;
  case GraphicsImageFailure::None:
  case GraphicsImageFailure::ExcessBytes: return std::nullopt;
  }
  return std::nullopt;
}

DeviceImageConversion deviceImageConversion(OneGenImageRejection::Conversion conversion) noexcept
{
  using Conversion = OneGenImageRejection::Conversion;
  switch (conversion) {
  case Conversion::None: return DeviceImageConversion::None;
  case Conversion::Borrow: return DeviceImageConversion::Borrow;
  case Conversion::Copy: return DeviceImageConversion::Copy;
  case Conversion::SwapByteOrder: return DeviceImageConversion::SwapByteOrder;
  case Conversion::JpegCopy: return DeviceImageConversion::JpegCopy;
  }
  return DeviceImageConversion::None;
}

std::string contextErrorMessage(GraphicsFrameError error)
{
  switch (error) {
  case GraphicsFrameError::None: return "Frame storage failed.";
  case GraphicsFrameError::InvalidInput: return "Frame storage request is invalid.";
  case GraphicsFrameError::Capacity: return "Frame storage admission failed: frame size exceeds capacity.";
  case GraphicsFrameError::Allocation: return "Frame storage allocation failed.";
  case GraphicsFrameError::Disconnected: return "Frame storage session is disconnected.";
  case GraphicsFrameError::StaleGeneration: return "Frame storage grant expired.";
  case GraphicsFrameError::UnsealedWrite: return "Frame write region remained open.";
  case GraphicsFrameError::ClassNotPrepared: return "Frame storage class is not prepared.";
  case GraphicsFrameError::NoReadyCredit: return "Frame storage has no ready credit.";
  case GraphicsFrameError::ClassRefused: return "Frame storage admission failed: class refused by host.";
  }
  return "Frame storage allocation failed.";
}

} // namespace

struct OneGenSourceController::DirectDestinationPool {
  std::mutex mutex;
  std::size_t capacity = kMaximumInFlightFrames;
  std::size_t blockBytes = 0;
  std::vector<void*> freeBlocks;

  ~DirectDestinationPool()
  {
    for (void* ptr : freeBlocks) ::operator delete(ptr);
  }
};

struct OneGenSourceController::DirectDestinationState {
  std::shared_ptr<const DirectRunRecord> run;
  std::optional<GraphicsFrameContext> context;
  std::optional<GraphicsFrameBuilder> builder;
  GraphicsWriteRegion<std::uint8_t> region;

  DirectDestinationState() noexcept = default;

  DirectDestinationState(std::shared_ptr<const DirectRunRecord> runIn,
                         GraphicsFrameContext contextIn)
      : run(std::move(runIn))
      , context(std::move(contextIn))
      , builder(*context)
  {
  }

  ~DirectDestinationState()
  {
    // Destroy write region before builder, and builder before context.
    if (region.isOpen()) {
      GraphicsWriteRegion<std::uint8_t> abandoned = std::move(region);
    }
    builder.reset();
    context.reset();
  }
};

namespace {

template <typename T>
struct DestinationPoolAllocator {
  using value_type = T;
  std::shared_ptr<OneGenSourceController::DirectDestinationPool> pool;

  explicit DestinationPoolAllocator(
      std::shared_ptr<OneGenSourceController::DirectDestinationPool> poolIn) noexcept
      : pool(std::move(poolIn))
  {
  }
  template <typename U>
  DestinationPoolAllocator(const DestinationPoolAllocator<U>& other) noexcept
      : pool(other.pool)
  {
  }

  [[nodiscard]] T* allocate(std::size_t count)
  {
    const std::size_t bytes = count * sizeof(T);
    if (pool) {
      std::lock_guard lock(pool->mutex);
      if (pool->blockBytes == bytes && !pool->freeBlocks.empty()) {
        void* ptr = pool->freeBlocks.back();
        pool->freeBlocks.pop_back();
        return static_cast<T*>(ptr);
      }
    }
    return static_cast<T*>(::operator new(bytes));
  }

  void deallocate(T* ptr, std::size_t count) noexcept
  {
    if (!ptr) return;
    const std::size_t bytes = count * sizeof(T);
    if (pool) {
      std::lock_guard lock(pool->mutex);
      if (pool->blockBytes == 0U) pool->blockBytes = bytes;
      if (pool->blockBytes == bytes && pool->freeBlocks.size() < pool->capacity) {
        pool->freeBlocks.push_back(ptr);
        return;
      }
    }
    ::operator delete(ptr);
  }

  template <typename U>
  bool operator==(const DestinationPoolAllocator<U>& other) const noexcept
  {
    return pool == other.pool;
  }
};

} // namespace

OneGenSourceController::OneGenSourceController(QObject* parent, DeviceImageRejectionSink imageRejectionSink,
    DevicePlanarRejectionSink planarRejectionSink, std::uint32_t imageSourceIndex)
    : AbstractSourceController(parent), image_rejection_sink_(imageRejectionSink),
      planar_rejection_sink_(planarRejectionSink), image_source_index_(imageSourceIndex) {}

OneGenSourceController::~OneGenSourceController()
{
  stopParameterWatches();
  drainControl();
  std::vector<OneGenSourceController*> children;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [id, child] : children_) children.push_back(child.controller.get());
  }
  for (auto* child : children) child->drainControl();
  try { closeDevice(); } catch (...) {}
  try { stop(); } catch (...) {}
  setLease(nullptr);
  for (auto& [id, child] : children_) child.controller->setLease(nullptr);
  children_.clear();
  owned_lease_.reset();
  // An explicit Interface is pinned in the producer; release it with this session.
  try {
    if (interface_ && coordinator_) coordinator_->close_interface(interface_->info);
  } catch (...) {}
}

void OneGenSourceController::drainControl()
{
  std::vector<QFuture<void>> futures;
  {
    std::lock_guard lock(work_mutex_);
    futures.swap(admitted_work_);
  }
  for (auto& future : futures) future.waitForFinished();
  tool_work_.waitForFinished();
}

void OneGenSourceController::configureControl(std::shared_ptr<onegen::producer::Coordinator> coordinator, std::vector<onegen::DeviceInfo> devices) {
  coordinator_ = std::move(coordinator);
  setDevices(std::move(devices));
}
void OneGenSourceController::setDevices(std::vector<onegen::DeviceInfo> devices) {
  Q_ASSERT(QThread::currentThread() == thread());
  std::lock_guard lock(mutex_);
  devices_ = std::move(devices);
}
std::shared_ptr<onegen::producer::Lease> OneGenSourceController::deviceLease() const {
  std::lock_guard lock(mutex_);
  return owned_lease_;
}
std::vector<onegen::DeviceInfo> OneGenSourceController::devices() const {
  std::lock_guard lock(mutex_); return devices_;
}

QString OneGenSourceController::genTLPathChangeBlockReason() const {
  {
    std::lock_guard lock(mutex_);
    if (interface_)
      return QStringLiteral("Disconnect GenICam cameras and Interfaces before applying paths.");
    for (const auto& [key, child] : children_) {
      std::lock_guard childLock(child.controller->mutex_);
      if (child.controller->owned_lease_ && child.controller->owned_lease_->alive())
        return QStringLiteral("Disconnect GenICam cameras and Interfaces before applying paths.");
    }
  }
  if (controllerBusy(*this))
    return QStringLiteral("Wait for GenICam searches or pending operations to finish before applying paths.");
  return {};
}

bool OneGenSourceController::retryDiscovery(QString* error, DiscoveryFn discovery) {
  const auto reason = genTLPathChangeBlockReason();
  if (!reason.isEmpty()) {
    if (error) *error = reason;
    return false;
  }
  const auto admitted = admitLifecycle(Lifecycle::Discover, {}, {},
      [](DeviceConnectionTargets result) {
        if (result.result.error != DeviceConnectionError::None)
          qWarning().noquote() << QStringLiteral("[GenICam] session discovery retry failed:")
              << QString::fromStdString(result.result.message);
      }, std::move(discovery));
  if (admitted.error != DeviceConnectionError::None) {
    if (error) *error = QString::fromStdString(admitted.message);
    return false;
  }
  return true;
}
std::vector<std::pair<std::string, onegen::FeatureNode>> OneGenSourceController::controlFeatures() const {
  std::lock_guard lock(mutex_); return control_features_;
}
std::vector<onegen::DataStreamInfo> OneGenSourceController::controlStreams() const {
  std::lock_guard lock(mutex_); return control_streams_;
}
void OneGenSourceController::refreshControlSnapshot() {
  const auto lease = deviceLease();
  if (!lease || !lease->alive()) return;
  auto streams = lease->data_streams();
  auto features = lease->describe_features();
  std::lock_guard lock(mutex_);
  control_streams_ = std::move(streams); control_features_ = std::move(features);
}

const onegen::FeatureNode* OneGenSourceController::findFeature(const onegen::FeatureNode& root,
                                                               const std::string_view name) {
  if (root.name == name) return &root;
  for (const auto& child : root.children)
    if (const auto* found = findFeature(child, name)) return found;
  return nullptr;
}

DeviceConnectionAvailability OneGenSourceController::modeAvailability(
    const std::vector<std::pair<std::string, onegen::FeatureNode>>& features,
    const char* mode, const char* label) {
  const onegen::FeatureNode* node = nullptr;
  for (const auto& [module, tree] : features)
    if (module == "Remote Device") node = findFeature(tree, "AcquisitionMode");
  if (!node) return {false, "The device has no AcquisitionMode feature."};
  if (!node->writable)
    return {false, "AcquisitionMode is not writable in the device's current state."};
  if (node->kind != onegen::NodeKind::Enumeration)
    return {false, "AcquisitionMode is not an enumeration."};
  if (std::find(node->enum_entries.begin(), node->enum_entries.end(), mode) == node->enum_entries.end())
    return {false, std::string("AcquisitionMode does not offer ") + label + "."};
  return {true, {}};
}

DeviceConnectionAvailability OneGenSourceController::singleAvailability() const {
  std::vector<const OneGenSourceController*> candidates;
  {
    std::lock_guard lock(mutex_);
    candidates.push_back(this);
    for (const auto& [id, child] : children_) candidates.push_back(child.controller.get());
  }
  for (const auto* candidate : candidates) {
    std::lock_guard lock(candidate->mutex_);
    if (candidate->control_features_.empty()) continue;
    return modeAvailability(candidate->control_features_, "SingleFrame", "SingleFrame");
  }
  return {false, "Connect the device to observe AcquisitionMode."};
}

OneGenSourceController::AcquisitionModeChange OneGenSourceController::applyAcquisitionMode(
    const bool single) {
  const auto lease = deviceLease();
  if (!lease) throw OneGenAcquisitionModeRefused("Device is disconnected.");
  AcquisitionModeChange change;
  {
    std::lock_guard lock(mutex_);
    const auto& info = lease->info();
    change.sourceName = info.display_name.empty() ? info.id : info.display_name;
  }
  const std::string desired = single ? "SingleFrame" : "Continuous";
  try {
    const auto mode = lease->read_node(0, "AcquisitionMode");
    if (!mode.readable)
      throw OneGenAcquisitionModeRefused("AcquisitionMode is not readable on this device.");
    if (mode.kind != onegen::NodeKind::Enumeration)
      throw OneGenAcquisitionModeRefused("AcquisitionMode is not an enumeration on this device.");
    if (!mode.writable)
      throw OneGenAcquisitionModeRefused(
          "AcquisitionMode is not writable in the device's current state.");
    if (std::find(mode.enum_entries.begin(), mode.enum_entries.end(), desired)
        == mode.enum_entries.end())
      throw OneGenAcquisitionModeRefused("AcquisitionMode does not offer " + desired + ".");
    change.previous = mode.enum_value;
    change.current = change.previous;
    if (change.previous != desired) {
      const auto written = lease->write_node(0, "AcquisitionMode", "enum", desired);
      change.current = written.enum_value;
      if (change.current != desired)
        throw OneGenAcquisitionModeRefused("AcquisitionMode readback is " + change.current
                                           + " after requesting " + desired + ".");
      change.changed = true;
    }
  } catch (const OneGenAcquisitionModeRefused&) {
    throw;
  } catch (const std::exception& error) {
    throw OneGenAcquisitionModeRefused(std::string("AcquisitionMode could not be written: ")
                                        + error.what());
  }
  return change;
}

void OneGenSourceController::reportAcquisitionMode(const AcquisitionModeChange& change,
                                                   const char* action) const {
  if (!change.changed) return;
  const auto camera = QString::fromStdString(change.sourceName);
  const auto previous = QString::fromStdString(change.previous);
  const auto current = QString::fromStdString(change.current);
  qCInfo(onegenSourceDiagnosticLog).noquote() << QStringLiteral(
      "[GenICam] AcquisitionMode changed: camera=%1 action=%2 previous=%3 current=%4")
      .arg(camera, QString::fromLatin1(action), previous, current);
  qCDebug(onegenSourceDiagnosticLog).noquote() << "@diagnostic " + QString::fromUtf8(QJsonDocument(QJsonObject{
      {"event", "acquisition_mode"}, {"fields", QJsonObject{
          {"camera", camera}, {"action", QString::fromLatin1(action)},
          {"previous", previous}, {"current", current}, {"changed", true}}}})
      .toJson(QJsonDocument::Compact));
}
std::shared_ptr<onegen::producer::Lease> OneGenSourceController::openDevice(
    std::shared_ptr<onegen::producer::Coordinator> coordinator, const onegen::DeviceInfo& device) {
  if (deviceLease()) throw std::runtime_error("Disconnect the current device before connecting.");
  auto opened = std::shared_ptr<onegen::producer::Lease>(coordinator->open(device).release());
  { std::lock_guard lock(mutex_); owned_lease_ = opened; connection_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces); }
  try { setLease(opened.get()); }
  catch (...) { closeDevice(); throw; }
  try { refreshControlSnapshot(); } catch (const std::exception& error) { qWarning() << "[GenICam] feature observation failed:" << error.what(); }
  emit controlChanged();
  return opened;
}
void OneGenSourceController::closeDevice() {
  const auto lease = deviceLease();
  if (!lease) return;
  stopLive();
  lease->close();
  setLease(nullptr);
  {
    std::lock_guard lock(mutex_);
    owned_lease_.reset();
    connection_id_.clear();
    parent_connection_id_.clear();
    discovery_error_.clear();
    control_features_.clear();
    control_streams_.clear();
    std::lock_guard errors(error_state_->mutex);
    error_state_->message.clear();
  }
  emit controlChanged();
}

OneGenSourceController* OneGenSourceController::cameraController(const std::string& id, const std::string& label)
{
  OneGenSourceController* result;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = children_.find(id); found != children_.end()) return found->second.controller.get();
    const auto index = next_source_index_++;
    auto child = std::make_unique<OneGenSourceController>(nullptr, image_rejection_sink_, planar_rejection_sink_, index);
    connect(child.get(), &AbstractSourceController::diagnosticAcquisitionChanged, this,
        [this, id](bool active, const QString&) { emit diagnosticAcquisitionChanged(active, QString::fromStdString(id)); },
        Qt::DirectConnection);
    child->forwarded_by_parent_ = true;
    child->setFrameContextSource(context_source_ ? context_source_->source : nullptr);
    child->setFrameConsumer([this, index, childPtr = child.get()](SessionFrame&& frame, unsigned int) {
      FrameConsumer consumer;
      { std::lock_guard lock(mutex_); consumer = consumer_; }
      if (consumer) {
        consumer(std::move(frame), index);
        childPtr->delivered_frames_.fetch_add(1, std::memory_order_relaxed);
      }
      else qWarning() << "[GenICam] outcome=rejected reason=no-parent-presentation-consumer sourceIndex="
          << index << "sequence=" << frame.frameSeq;
    });
    result = child.get();
    children_.emplace(id, Child{index, label, std::move(child)});
  }
  publishSourceDescriptors();
  return result;
}

void OneGenSourceController::removeCameraController(const std::string& id)
{
  drainControl();
  std::unique_ptr<OneGenSourceController> removed;
  {
    std::lock_guard lock(mutex_);
    const auto found = children_.find(id);
    if (found == children_.end()) return;
    removed = std::move(found->second.controller);
    children_.erase(found);
    if (children_.empty()) next_source_index_ = 0;
  }
  removed.reset();
  publishSourceDescriptors();
}

void OneGenSourceController::clearCameraControllers()
{
  drainControl();
  std::map<std::string, Child> removed;
  {
    std::lock_guard lock(mutex_);
    removed.swap(children_);
    next_source_index_ = 0;
  }
  removed.clear();
  publishSourceDescriptors();
}

std::vector<GraphicsSourceDescriptor> OneGenSourceController::sourceDescriptors() const
{
  std::lock_guard lock(mutex_);
  std::vector<GraphicsSourceDescriptor> result;
  for (const auto& [id, child] : children_) result.push_back({child.index, id, child.label});
  return result;
}

void OneGenSourceController::setLease(onegen::producer::Lease* lease)
{
  std::unique_lock leaseLock(direct_lease_mutex_);
  std::unique_lock lock(mutex_);
  if (lease_ == lease) return;
  auto old_gate = std::move(callback_gate_);
  if (old_gate) old_gate->beginShutdown();
  if (lease_) {
    lease_->set_frame_callback({});
    lease_->set_shared_frame_callback({});
    lease_->set_error_callback({});
    lease_->detach_lease_sink();
  }
  lease_sink_.reset();
  active_direct_run_.reset();
  lock.unlock();
  // Frame callbacks never acquire direct_lease_mutex_; holding it across the gate
  // drain prevents start from attaching a lease sink to the lease being replaced.
  if (old_gate) old_gate->waitForDrain();
  lock.lock();
  lease_ = lease;
  rejected_frames_.store(0, std::memory_order_relaxed);
  incomplete_frames_.store(0, std::memory_order_relaxed);
  received_frames_.store(0, std::memory_order_relaxed);
  converted_frames_.store(0, std::memory_order_relaxed);
  delivered_frames_.store(0, std::memory_order_relaxed);
  partial_frames_.store(0, std::memory_order_relaxed);
  bindLease();
}

std::function<void(const onegen::Frame&)> OneGenSourceController::makeAcquisitionCallback(
    const std::string& sourceName, GraphicsFrameCallbackGate::Token token)
{
  return [this, sourceName, token = std::move(token)](const onegen::Frame& source) {
    GraphicsFrameCallbackGate::Lease callback(token);
    if (callback) deliver(source, sourceName);
  };
}

GraphicsFrameCallbackGate::Token OneGenSourceController::ensureCallbackGateLocked(
    onegen::producer::Lease& lease, const std::string& sourceName)
{
  if (!callback_gate_) {
    callback_gate_ = std::make_unique<GraphicsFrameCallbackGate>();
    lease.set_frame_callback(makeAcquisitionCallback(sourceName, callback_gate_->token()));
  }
  return callback_gate_->token();
}

void OneGenSourceController::bindLease()
{
  if (!lease_) return;
  const auto& info = lease_->info();
  const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
  // Inspect the copied node tree without changing selectors or acquisition settings.
  // The inventory is a Debug detail dump; skip its feature round trip otherwise.
  if (onegenSourceDiagnosticLog().isDebugEnabled()) try {
    std::size_t componentNodes = 0;
    const std::function<void(const onegen::FeatureNode&, const std::string&)> inspect =
        [&](const onegen::FeatureNode& node, const std::string& module) {
      const auto name = QString::fromStdString(node.name);
      if (name.contains(QStringLiteral("Component"), Qt::CaseInsensitive)) {
        ++componentNodes;
        QString value;
        if (node.readable) switch (node.kind) {
          case onegen::NodeKind::Integer: value = QString::number(node.int_value); break;
          case onegen::NodeKind::Float: value = QString::number(node.float_value, 'g', 17); break;
          case onegen::NodeKind::Boolean: value = node.bool_value ? "true" : "false"; break;
          case onegen::NodeKind::Enumeration: value = QString::fromStdString(node.enum_value); break;
          case onegen::NodeKind::String: value = QString::fromStdString(node.string_value); break;
          default: break;
        }
        QStringList entries;
        for (const auto& entry : node.enum_entries) entries.push_back(QString::fromStdString(entry));
        qCDebug(onegenSourceDiagnosticLog).noquote() << "[GenICam] component node: camera=" << QString::fromStdString(sourceName)
            << "module=" << QString::fromStdString(module) << "node=" << name
            << "label=" << QString::fromStdString(node.display_name)
            << "readable=" << node.readable << "writable=" << node.writable
            << "value=" << value << "entries=" << entries.join(',')
            << "description=" << QString::fromStdString(node.description);
      }
      for (const auto& child : node.children) inspect(child, module);
    };
    for (const auto& [module, root] : lease_->describe_features()) inspect(root, module);
    qCDebug(onegenSourceDiagnosticLog) << "[GenICam] component inventory: nodes=" << componentNodes
            << "selectorUnchanged=true; selected values do not map every multipart sibling";
  } catch (const std::exception& error) {
    qWarning() << "[GenICam] component inventory unavailable:" << error.what();
  }

  callback_gate_.reset();
  (void)ensureCallbackGateLocked(*lease_, sourceName);
  lease_->set_error_callback([this, errors = error_state_, model = info.model, serial = info.serial](
                                 const std::string_view error) {
    const auto message = QString::fromLocal8Bit(error.data(), static_cast<qsizetype>(error.size()));
    qWarning().noquote()
        << QStringLiteral("[GenICam] camera=%1 serial=%2; cause=%3")
               .arg(QString::fromStdString(model), QString::fromStdString(serial), message);
    {
      std::lock_guard lock(errors->mutex);
      errors->message = message.toStdString();
    }
  });
}

void OneGenSourceController::setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source)
{
  std::shared_ptr<ContextSourceState> nextState;
  if (source) {
    nextState = std::make_shared<ContextSourceState>();
    nextState->source = source;
  }
  std::vector<OneGenSourceController*> children;
  {
    std::lock_guard lock(mutex_);
    context_source_ = std::move(nextState);
    last_single_failure_.clear();
    for (const auto& child : children_) children.push_back(child.second.controller.get());
  }
  for (auto* child : children) child->setFrameContextSource(source);
}

std::shared_ptr<OneGenSourceController::ContextSourceState>
OneGenSourceController::activeContextSource(std::string* error) const
{
  std::shared_ptr<ContextSourceState> state;
  {
    std::lock_guard lock(mutex_);
    state = context_source_;
  }
  if (!state || !state->source) {
    if (error) *error = "Frame context source is not configured.";
    return {};
  }
  return state;
}

void OneGenSourceController::configureDirectSink(
    onegen::producer::Lease& lease, const std::string& sourceName)
{
  GraphicsFrameCallbackGate::Token token;
  std::shared_ptr<DirectDestinationPool> pool;
  {
    std::lock_guard lock(mutex_);
    token = ensureCallbackGateLocked(lease, sourceName);
    if (!destination_pool_) {
      destination_pool_ = std::make_shared<DirectDestinationPool>();
      std::vector<std::shared_ptr<DirectDestinationState>> warmup;
      warmup.reserve(kMaximumInFlightFrames);
      for (std::uint32_t i = 0; i < kMaximumInFlightFrames; ++i) {
        warmup.push_back(std::allocate_shared<DirectDestinationState>(
            DestinationPoolAllocator<DirectDestinationState>(destination_pool_)));
      }
    }
    pool = destination_pool_;
  }
  if (!token) throw std::runtime_error("Frame delivery is not bound to a consumer.");

  std::string sourceError;
  auto sourceState = activeContextSource(&sourceError);
  if (!sourceState) {
    lease.detach_lease_sink();
    std::lock_guard lock(mutex_);
    lease_sink_.reset();
    active_direct_run_.reset();
    throw std::runtime_error(sourceError);
  }

  std::string error;
  const auto payload = directSlotBytes(lease, error);
  if (!payload || *payload == 0) {
    // Non-image / dynamic payload layouts without a fixed stream payload size use the owned
    // conversion path, which still calls beginFrame(plannedBytes) for every delivered frame.
    lease.detach_lease_sink();
    std::lock_guard lock(mutex_);
    lease_sink_.reset();
    active_direct_run_.reset();
    qWarning().noquote() << QStringLiteral(
        "[GenICam] direct frame path unavailable: camera=%1 reason=%2")
        .arg(QString::fromStdString(sourceName), QString::fromStdString(error));
    return;
  }

  auto runRecord = std::make_shared<const DirectRunRecord>(
      DirectRunRecord{std::move(sourceState), token, sourceName});
  std::shared_ptr<const onegen::producer::DirectLeaseSink> attached;
  {
    std::lock_guard lock(mutex_);
    active_direct_run_ = runRecord;
    attached = lease_sink_;
  }
  if (attached && attached->slot_bytes() == *payload) return;

  lease.detach_lease_sink();
  {
    std::lock_guard lock(mutex_);
    lease_sink_.reset();
  }

  auto destinationProvider = [this, pool](std::size_t bytes) -> onegen::producer::DirectLeaseSink::DestinationReservation {
    using DestinationReservation = onegen::producer::DirectLeaseSink::DestinationReservation;
    using DestinationStatus = onegen::producer::DirectLeaseSink::DestinationStatus;
    std::shared_ptr<const DirectRunRecord> run;
    {
      std::lock_guard lock(mutex_);
      run = active_direct_run_;
    }
    if (!run || !run->sourceState || !run->sourceState->source) {
      return {DestinationStatus::Refused, {}, {}};
    }
    GraphicsFrameCallbackGate::Lease callback(run->gateToken);
    if (!callback) {
      return {DestinationStatus::Refused, {}, {}};
    }
    auto ctxResult = run->sourceState->source->beginFrame(bytes);
    if (!ctxResult) {
      const auto err = ctxResult.error();
      if (err == GraphicsFrameError::NoReadyCredit) {
        return {DestinationStatus::Busy, {}, {}};
      }
      const auto message = contextErrorMessage(err);
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      bool firstMessage = false;
      {
        std::lock_guard errors(error_state_->mutex);
        if (error_state_->message.empty()) {
          error_state_->message = message;
          firstMessage = true;
        }
      }
      if (firstMessage || (err != GraphicsFrameError::ClassRefused && err != GraphicsFrameError::Capacity))
        qWarning().noquote() << QStringLiteral("[GenICam] camera=%1 direct storage refused: %2")
            .arg(QString::fromStdString(run->sourceName), QString::fromStdString(message));
      // A class or capacity refusal drops only this frame; the run keeps acquiring and the
      // next frame asks again. Other failures still stop the direct sink.
      if (err == GraphicsFrameError::ClassRefused || err == GraphicsFrameError::Capacity)
        return {DestinationStatus::Busy, {}, {}};
      return {DestinationStatus::Refused, {}, {}};
    }
    auto state = std::allocate_shared<DirectDestinationState>(
        DestinationPoolAllocator<DirectDestinationState>(pool),
        run, std::move(ctxResult).value());
    auto reserved = state->builder->reserve<std::uint8_t>(bytes, 1U);
    if (!reserved) {
      const auto err = reserved.error();
      state.reset();
      if (err == GraphicsFrameError::NoReadyCredit) {
        return {DestinationStatus::Busy, {}, {}};
      }
      const auto message = contextErrorMessage(err);
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      bool firstMessage = false;
      {
        std::lock_guard errors(error_state_->mutex);
        if (error_state_->message.empty()) {
          error_state_->message = message;
          firstMessage = true;
        }
      }
      if (firstMessage || (err != GraphicsFrameError::ClassRefused && err != GraphicsFrameError::Capacity))
        qWarning().noquote() << QStringLiteral("[GenICam] camera=%1 direct reserve refused: %2")
            .arg(QString::fromStdString(run->sourceName), QString::fromStdString(message));
      // A class or capacity refusal drops only this frame; the run keeps acquiring and the
      // next frame asks again. Other failures still stop the direct sink.
      if (err == GraphicsFrameError::ClassRefused || err == GraphicsFrameError::Capacity)
        return {DestinationStatus::Busy, {}, {}};
      return {DestinationStatus::Refused, {}, {}};
    }
    state->region = std::move(reserved).value();
    std::span<std::uint8_t> writable(state->region.data(), state->region.size());
    return {DestinationStatus::Reserved, writable, std::move(state)};
  };

  // The sink owns no frame memory: each reservation acquires from the injected context source.
  if (!lease.attach_lease_sink(kMaximumInFlightFrames, *payload, error, std::move(destinationProvider))) {
    throw std::runtime_error(error);
  }
  lease.set_shared_frame_callback(
      [this](const onegen::producer::SharedFrame& source) {
        deliverDirect(source);
      });
  {
    std::lock_guard lock(mutex_);
    lease_sink_ = lease.lease_sink();
  }
  qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral(
      "[GenICam] Frame context direct sink attached: camera=%1 slots=%2 slotBytes=%3")
      .arg(QString::fromStdString(sourceName)).arg(kMaximumInFlightFrames).arg(*payload);
}

void OneGenSourceController::start()
{
  if (QThread::currentThread() == thread() && external_busy_.load()) throw std::runtime_error("Device control is busy.");
  {
    std::lock_guard lock(mutex_);
    operation_failures_.clear();
  }
  std::vector<std::pair<std::string, OneGenSourceController*>> children;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [id, child] : children_) children.emplace_back(id, child.controller.get());
  }
  for (const auto& [id, child] : children) {
    unsigned int index = 0;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = children_.find(id); found != children_.end()) index = found->second.index;
    }
    try {
      child->start();
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      operation_failures_.push_back({id, index, error.what()});
    } catch (...) {
      std::lock_guard lock(mutex_);
      operation_failures_.push_back({id, index, "The camera did not start."});
    }
  }
  onegen::producer::Lease* lease = nullptr;
  {
    std::unique_lock leaseLock(direct_lease_mutex_);
    {
      std::lock_guard lock(mutex_);
      lease = lease_;
    }
    if (!lease) return;
    const auto& info = lease->info();
    const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
    std::string sourceError;
    if (!activeContextSource(&sourceError)) throw std::runtime_error(sourceError);
    const auto modeChange = applyAcquisitionMode(false);
    reportAcquisitionMode(modeChange, "live");
    configureDirectSink(*lease, sourceName);
  }
  received_frames_.store(0, std::memory_order_relaxed);
  converted_frames_.store(0, std::memory_order_relaxed);
  delivered_frames_.store(0, std::memory_order_relaxed);
  rejected_frames_.store(0, std::memory_order_relaxed);
  incomplete_frames_.store(0, std::memory_order_relaxed);
  partial_frames_.store(0, std::memory_order_relaxed);
  for (auto& count : image_rejection_counts_) count.store(0, std::memory_order_relaxed);
  for (auto& count : planar_rejection_counts_) count.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    exhausted_baseline_.store(lease_sink_ ? lease_sink_->exhausted() : 0, std::memory_order_relaxed);
  }
  emit diagnosticAcquisitionChanged(true, QStringLiteral("camera"));
  try { lease->live(); }
  catch (...) { emit diagnosticAcquisitionChanged(false, QStringLiteral("camera")); throw; }
  grabbing_.store(true, std::memory_order_relaxed);
  // Direct API start bypasses the GUI operation watcher, so publish the state
  // change for the Host panel to refresh its live controls and telemetry.
  emit controlChanged();
}

void OneGenSourceController::stop()
{
  {
    std::lock_guard lock(mutex_);
    operation_failures_.clear();
  }
  std::vector<std::pair<std::string, OneGenSourceController*>> children;
  {
    std::lock_guard lock(mutex_);
    for (const auto& [id, child] : children_) children.emplace_back(id, child.controller.get());
  }
  for (const auto& [id, child] : children) {
    unsigned int index = 0;
    {
      std::lock_guard lock(mutex_);
      if (const auto found = children_.find(id); found != children_.end()) index = found->second.index;
    }
    try {
      child->stopLive();
    } catch (const std::exception& error) {
      std::lock_guard lock(mutex_);
      operation_failures_.push_back({id, index, error.what()});
    } catch (...) {
      std::lock_guard lock(mutex_);
      operation_failures_.push_back({id, index, "The camera did not stop."});
    }
  }
  try { stopOwnAcquisition(); }
  catch (const std::exception& error) { qWarning() << "[GenICam] stop unavailable:" << error.what(); }
  catch (...) { qWarning() << "[GenICam] stop failed."; }
}

void OneGenSourceController::stopLive()
{
  if (QThread::currentThread() == thread() && external_busy_.load()) throw std::runtime_error("Device control is busy.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  std::exception_ptr first;
  for (auto* child : children) try {
    child->stopLive();
  } catch (...) {
    if (!first) first = std::current_exception();
  }
  try {
    stopOwnAcquisition();
  } catch (...) {
    if (!first) first = std::current_exception();
  }
  if (first) std::rethrow_exception(first);
}

void OneGenSourceController::stopOwnAcquisition()
{
  onegen::producer::Lease* lease = nullptr;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
  }
  if (!lease) return;
  try {
    const auto& info = lease->info();
    const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
    lease->stop();
    std::unique_ptr<GraphicsFrameCallbackGate> oldGate;
    {
      std::lock_guard lock(mutex_);
      active_direct_run_.reset();
      oldGate = std::move(callback_gate_);
    }
    if (oldGate) {
      oldGate->beginShutdown();
      oldGate->waitForDrain();
    }
    grabbing_.store(false, std::memory_order_relaxed);
    qInfo().noquote() << QStringLiteral(
        "[GenICam] live diagnostics: camera=%1 received=%2 delivered=%3 failed=%4 incomplete=%5 dropped=%6")
        .arg(QString::fromStdString(sourceName))
        .arg(static_cast<qulonglong>(received_frames_.load(std::memory_order_relaxed)))
        .arg(static_cast<qulonglong>(delivered_frames_.load(std::memory_order_relaxed)))
        .arg(static_cast<qulonglong>(rejected_frames_.load(std::memory_order_relaxed)))
        .arg(static_cast<qulonglong>(incomplete_frames_.load(std::memory_order_relaxed)))
        .arg(static_cast<qulonglong>(droppedFrames()));
    emit diagnosticAcquisitionChanged(false, QStringLiteral("camera"));
    // Match start(): a direct API stop must clear the Host panel's live controls
    // and Debug telemetry without a GUI operation completion.
    emit controlChanged();
  } catch (const std::exception& error) {
    qWarning().noquote() << QStringLiteral("[GenICam] live stop failed: %1")
                               .arg(QString::fromLocal8Bit(error.what()));
    throw;
  } catch (...) {
    qWarning() << "[GenICam] live stop failed with an unknown exception.";
    throw;
  }
}

bool OneGenSourceController::isGrabbing() const {
  std::lock_guard lock(mutex_);
  for (const auto& [id, child] : children_) if (child.controller->isGrabbing()) return true;
  return grabbing_;
}

std::vector<SourceOperationFailure> OneGenSourceController::sourceOperationFailures() const
{
  std::lock_guard lock(mutex_);
  return operation_failures_;
}

std::optional<SourceRuntimeDiagnostics> OneGenSourceController::sourceRuntimeDiagnostics(unsigned sourceIndex) const {
  const auto sources = sourceDescriptors();
  if (!(sources.empty() && sourceIndex == 0) && std::none_of(sources.begin(), sources.end(), [sourceIndex](const auto& s) { return s.index == sourceIndex; }))
    return std::nullopt;
  return runtimeDiagnostics(sourceIndex);
}

SourceRuntimeDiagnostics OneGenSourceController::runtimeDiagnostics(
    const unsigned sourceIndex) const {
  {
    // Child queries are atomics only; holding the parent lock keeps the child
    // alive while a camera panel is removed.
    std::lock_guard lock(mutex_);
    if (!children_.empty()) {
      for (const auto& [id, child] : children_)
        if (child.index == sourceIndex) return child.controller->runtimeDiagnostics(0U);
      return {};
    }
  }
  if (sourceIndex != 0U) return {};
  return {grabbing_.load(std::memory_order_acquire),
      delivered_frames_.load(std::memory_order_acquire),
      rejected_frames_.load(std::memory_order_acquire) + droppedFrames()};
}

std::uint64_t OneGenSourceController::droppedFrames() const
{
  std::shared_ptr<const onegen::producer::DirectLeaseSink> sink;
  {
    std::lock_guard lock(mutex_);
    sink = lease_sink_;
  }
  if (!sink) return 0;
  const auto exhausted = sink->exhausted();
  const auto baseline = exhausted_baseline_.load(std::memory_order_relaxed);
  return exhausted > baseline ? exhausted - baseline : 0;
}

void OneGenSourceController::grabOne(const QString& diagnosticDirectory)
{
  onegen::producer::Lease* lease = nullptr;
  std::string sourceName;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
    if (lease) {
      const auto& info = lease->info();
      sourceName = info.display_name.empty() ? info.id : info.display_name;
    }
  }
  if (!lease) return;
  std::string sourceError;
  if (!activeContextSource(&sourceError)) throw std::runtime_error(sourceError);
  {
    std::lock_guard lock(mutex_);
    last_single_failure_.clear();
  }
  const auto modeChange = applyAcquisitionMode(true);
  reportAcquisitionMode(modeChange, "single");
  delivered_frames_.store(0, std::memory_order_relaxed);
  rejected_frames_.store(0, std::memory_order_relaxed);
  emit diagnosticAcquisitionChanged(true, QStringLiteral("camera"));
  struct DiagnosticEnd {
    OneGenSourceController* controller;
    ~DiagnosticEnd() { emit controller->diagnosticAcquisitionChanged(false, QStringLiteral("camera")); }
  } diagnosticEnd{this};
  lease->grab_one(
      [this, sourceName, diagnosticDirectory](const onegen::Frame& source) {
        deliver(source, sourceName, true);
        onegen::diagnostics::delivered(source, diagnosticDirectory);
      });
}

void OneGenSourceController::trigger()
{
  onegen::producer::Lease* lease = nullptr;
  { std::lock_guard lock(mutex_); lease = lease_; }
  if (lease) lease->trigger();
}

namespace {
/** True for the first occurrence and whenever the count crosses a multiple of one hundred. */
bool crossesReportThreshold(const std::uint64_t before, const std::uint64_t after) noexcept
{
  return before == 0 || after / 100 > before / 100;
}
} // namespace

bool OneGenSourceController::reportImageRejection(const OneGenImageRejection& evidence,
    const onegen::Frame& source, std::uint64_t sequence, std::size_t readableBytes,
    bool directInput, bool single, std::size_t repeat) noexcept
{
  if (!image_rejection_sink_) return false;
  const auto reason = deviceImageRejection(evidence);
  if (!reason) return false;
  if (!single) {
    auto& counter = image_rejection_counts_[static_cast<std::size_t>(*reason)];
    const auto before = counter.fetch_add(repeat, std::memory_order_relaxed);
    if (!crossesReportThreshold(before, before + repeat)) return true;
  }
  DeviceImageRejectionObservation observation;
  observation.reason = *reason;
  observation.conversion = deviceImageConversion(evidence.conversion);
  observation.sourceIndex = forwarded_by_parent_ ? image_source_index_ : source.stream_index;
  observation.width = source.width;
  observation.height = source.height;
  observation.directInput = directInput;
  observation.frameSequence = sequence;
  observation.pixelFormat = source.pixel_format;
  observation.reportedStride = source.stride;
  observation.effectiveStride = evidence.effectiveStride;
  observation.minimumRowBytes = evidence.minimumRowBytes;
  observation.readableBytes = readableBytes;
  observation.requiredBytes = evidence.requiredBytes;
  image_rejection_sink_(observation);
  return true;
}

void OneGenSourceController::reportPlanarRejection(const OneGenPlanarRejection& evidence,
    const onegen::Frame& source, std::uint64_t sequence, const std::optional<GraphicsFrame>& output,
    bool single, std::size_t repeat) noexcept
{
  using Reason = OneGenPlanarRejection::Reason;
  if (!planar_rejection_sink_) return;
  DevicePlanarRejectionObservation observation;
  switch (evidence.reason) {
    case Reason::MissingAxis: observation.reason = DevicePlanarRejection::MissingAxis; break;
    case Reason::DuplicateAxis: observation.reason = DevicePlanarRejection::DuplicateAxis; break;
    case Reason::InvalidLayout: observation.reason = DevicePlanarRejection::InvalidLayout; break;
    case Reason::StrideBelowMinimum: observation.reason = DevicePlanarRejection::StrideBelowMinimum; break;
    case Reason::TruncatedPlane: observation.reason = DevicePlanarRejection::TruncatedPlane; break;
    case Reason::JoinedRangeRejected: observation.reason = DevicePlanarRejection::JoinedRangeRejected; break;
    case Reason::MissingCalibration: observation.reason = DevicePlanarRejection::MissingCalibration; break;
    case Reason::InvalidCalibration: observation.reason = DevicePlanarRejection::InvalidCalibration; break;
    case Reason::UnsupportedGeometry: observation.reason = DevicePlanarRejection::UnsupportedGeometry; break;
    case Reason::UnsupportedUnit: observation.reason = DevicePlanarRejection::UnsupportedUnit; break;
    case Reason::AuxiliaryLayoutRejected: observation.reason = DevicePlanarRejection::AuxiliaryLayoutRejected; break;
    case Reason::RangeInvalidDimensions: observation.reason = DevicePlanarRejection::RangeInvalidDimensions; break;
    case Reason::RangeSizeLimitExceeded: observation.reason = DevicePlanarRejection::RangeSizeLimitExceeded; break;
    case Reason::RangeStorageRefused: observation.reason = DevicePlanarRejection::RangeStorageRefused; break;
    // The host contract has no dedicated rule for these; the nearest layout rule carries them.
    case Reason::UnsupportedPartFormat: observation.reason = DevicePlanarRejection::InvalidLayout; break;
    case Reason::InvalidPartDimensions: observation.reason = DevicePlanarRejection::RangeInvalidDimensions; break;
    case Reason::BufferRepaired: observation.reason = DevicePlanarRejection::InvalidLayout; break;
  }
  if (source.parts.size() > DevicePlanarRejectionObservation::noPart) return;
  if (!single) {
    auto& counter = planar_rejection_counts_[static_cast<std::size_t>(observation.reason)];
    const auto before = counter.fetch_add(repeat, std::memory_order_relaxed);
    if (!crossesReportThreshold(before, before + repeat)) return;
  }
  // A single-payload frame has no parts; its own extent and format describe the anomaly.
  const auto partIndex = evidence.affectedPart.value_or(evidence.anchorPart);
  const bool hasPart = partIndex < source.parts.size();
  observation.sourceIndex = forwarded_by_parent_ ? image_source_index_ : source.stream_index;
  observation.partCount = static_cast<std::uint32_t>(source.parts.size());
  observation.anchorPart = static_cast<std::uint32_t>(evidence.anchorPart);
  if (evidence.affectedPart) observation.affectedPart = static_cast<std::uint32_t>(*evidence.affectedPart);
  observation.width = hasPart ? source.parts[partIndex].width : source.width;
  observation.height = hasPart ? source.parts[partIndex].height : source.height;
  observation.matchedAxes = static_cast<std::uint8_t>(evidence.matchedAxes);
  observation.duplicateAxes = static_cast<std::uint8_t>(evidence.duplicateAxes);
  observation.namespaceMismatchAxes = static_cast<std::uint8_t>(evidence.namespaceMismatchAxes);
  observation.shapeMismatchAxes = static_cast<std::uint8_t>(evidence.shapeMismatchAxes);
  observation.frameRetained = output.has_value();
  observation.imageRetained = output && output->hasImage();
  observation.rangeRetained = output && output->hasRangeFrame();
  observation.frameSequence = sequence;
  observation.pixelFormat = hasPart ? source.parts[partIndex].data_format : source.pixel_format;
  observation.reportedStride = evidence.reportedStride;
  observation.effectiveStride = evidence.effectiveStride;
  observation.minimumRowBytes = evidence.minimumRowBytes;
  observation.readableBytes = evidence.readableBytes;
  observation.requiredBytes = evidence.requiredBytes;
  observation.requestedStorageBytes = evidence.requestedStorageBytes;
  planar_rejection_sink_(observation);
}

bool OneGenSourceController::reportAnomalies(const OneGenFrameAnomalies& anomalies,
    const onegen::Frame& source, std::uint64_t sequence, std::size_t readableBytes, bool directInput,
    const std::optional<GraphicsFrame>& output, bool single) noexcept
{
  bool reported = false;
  for (std::size_t i = 0; i < anomalies.imageCount; ++i)
    reported |= reportImageRejection(anomalies.images[i], source, sequence, readableBytes, directInput, single);
  for (std::size_t index = 0; index < anomalies.imageOverflow.size(); ++index) {
    if (!anomalies.imageOverflow[index]) continue;
    OneGenImageRejection evidence{};
    if (index) evidence.failure = static_cast<GraphicsImageFailure>(index - 1U);
    reported |= reportImageRejection(evidence, source, sequence, readableBytes, directInput, single,
        anomalies.imageOverflow[index]);
  }
  if (!planar_rejection_sink_) return reported;
  for (std::size_t i = 0; i < anomalies.partCount; ++i)
    reportPlanarRejection(anomalies.parts[i], source, sequence, output, single);
  for (std::size_t index = 0; index < anomalies.partOverflow.size(); ++index) {
    if (!anomalies.partOverflow[index]) continue;
    OneGenPlanarRejection evidence;
    evidence.reason = static_cast<OneGenPlanarRejection::Reason>(index);
    reportPlanarRejection(evidence, source, sequence, output, single, anomalies.partOverflow[index]);
  }
  return reported || anomalies.partCount != 0
      || std::any_of(anomalies.partOverflow.begin(), anomalies.partOverflow.end(),
                     [](const std::size_t count) { return count != 0; });
}

void OneGenSourceController::deliver(const onegen::Frame& source, const std::string& sourceName, bool single)
{
  try {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    const bool firstFrame = received_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (firstFrame) logFirstAcquisitionInput(source, source.payload.size(), false);
    if (source.buffer_info.incomplete) {
      incomplete_frames_.fetch_add(1, std::memory_order_relaxed);
      const auto diagnostic = source.buffer_info.diagnostic.empty()
          ? QStringLiteral("incomplete=%1 filled=%2 capacity=%3")
                .arg(source.buffer_info.incomplete)
                .arg(source.buffer_info.size_filled)
                .arg(source.buffer_info.capacity)
          : QString::fromStdString(source.buffer_info.diagnostic);
      qWarning().noquote() << "[GenICam] incomplete frame delivered:" << diagnostic;
    }
    const bool frameTrace = onegen::frame_trace_enabled();
    const auto conversionStarted = frameTrace ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    std::string sourceError;
    auto sourceState = activeContextSource(&sourceError);
    if (!sourceState) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      if (single) {
        std::lock_guard lock(mutex_);
        last_single_failure_ = sourceError;
      }
      qWarning().noquote() << "[GenICam] frame rejected: camera=" << QString::fromStdString(sourceName)
          << "sequence=" << sequence << "reason=" << QString::fromStdString(sourceError);
      return;
    }
    std::string rejectionReason;
    OneGenFrameAnomalies anomalies;
    GraphicsFrameError storageError = GraphicsFrameError::None;
    auto payload = oneGenGraphicsFrame(source, sequence, *sourceState->source, &rejectionReason,
        nullptr, nullptr, sourceName, &storageError, &anomalies);
    if (storageError != GraphicsFrameError::None) {
      if (storageError != GraphicsFrameError::NoReadyCredit) {
        std::lock_guard errors(error_state_->mutex);
        if (error_state_->message.empty()) error_state_->message = rejectionReason;
      }
    }
    const bool anomalous = anomalies.total() != 0;
    const bool reported = anomalous
        && reportAnomalies(anomalies, source, sequence, source.payload.size(), false, payload, single);
    const auto converted = frameTrace ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (!payload) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      if (single && !rejectionReason.empty()) {
        std::lock_guard lock(mutex_);
        last_single_failure_ = rejectionReason;
      }
      if (reported) return;
      {
        const auto message = QStringLiteral(
            "Frame rejected: %1; PFNC 0x%2, %3x%4, stride %5, payload %6 bytes, "
            "endianness %7; buffer{capacity=%8, filled=%9, data=%10, deliveredHeight=%11, "
            "imageOffset=%12, yPadding=%13, incomplete=%14, imagePresent=%15, chunk=%16}")
            .arg(QString::fromStdString(rejectionReason))
            .arg(QString::number(source.pixel_format, 16))
            .arg(source.width).arg(source.height).arg(source.stride)
            .arg(source.payload.size()).arg(source.pixel_endianness)
            .arg(source.buffer_info.capacity).arg(source.buffer_info.size_filled)
            .arg(source.buffer_info.data_size).arg(source.buffer_info.delivered_image_height)
            .arg(source.buffer_info.image_offset).arg(source.buffer_info.y_padding)
            .arg(source.buffer_info.incomplete).arg(source.buffer_info.image_present)
            .arg(source.buffer_info.contains_chunk_data);
        qWarning().noquote() << "[GenICam] outcome=rejected camera=" << QString::fromStdString(sourceName)
            << "sequence=" << sequence << "frameId=" << source.frame_id << message;
      }
      return;
    }
    const bool firstConverted = converted_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (anomalous || !rejectionReason.empty()) {
      const auto fitted = partial_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
      // Fitted frames are published; the warning is rate limited like the sink reports.
      if (anomalous && (fitted == 1 || fitted % 100 == 0))
        qWarning().noquote() << "[GenICam] frame fitted and published: camera=" << QString::fromStdString(sourceName)
            << "sequence=" << sequence << "frameId=" << source.frame_id << "anomalies=" << anomalies.total()
            << "fittedFrames=" << fitted;
      if (!rejectionReason.empty()) qWarning().noquote() << "[GenICam] frame retained with rejected parts: camera="
          << QString::fromStdString(sourceName) << "sequence=" << sequence
          << "frameId=" << source.frame_id << QString::fromStdString(rejectionReason);
    }
    SessionFrame frame;
    frame.payload = std::move(*payload);
    frame.frameSeq = sequence;
    FrameConsumer consumer;
    {
      std::lock_guard lock(mutex_);
      consumer = consumer_;
    }
    if (!consumer) {
      qWarning() << "[GenICam] frame converted but no presentation consumer is installed."
          << "sequence=" << sequence << "frameId=" << source.frame_id;
      return;
    }
    if (firstConverted) qCDebug(onegenAcquisitionDetailLog).noquote() << QStringLiteral("[GenICam] first frame converted: images=%1 ranges=%2 pointClouds=%3; forwarding to presentation")
        .arg(frame.payload.images().size()).arg(frame.payload.ranges().size()).arg(frame.payload.pointClouds().size());
    const auto consumerStarted = frameTrace ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
    consumer(std::move(frame), source.stream_index);
    if (!forwarded_by_parent_) delivered_frames_.fetch_add(1, std::memory_order_relaxed);
    if (frameTrace) {
      const auto consumerFinished = std::chrono::steady_clock::now();
      qDebug().noquote() << QStringLiteral(
          "[GenICam] frame transport: side=adapter camera=%1 sequence=%2 frame=%3 bytes=%4 "
          "conversion_ms=%5 consumer_ms=%6")
          .arg(QString::fromStdString(sourceName))
          .arg(static_cast<qulonglong>(sequence))
          .arg(static_cast<qulonglong>(source.frame_id))
          .arg(static_cast<qulonglong>(source.payload.size()))
          .arg(onegen::frame_trace_ms(conversionStarted, converted), 0, 'f', 3)
          .arg(onegen::frame_trace_ms(consumerStarted, consumerFinished), 0, 'f', 3);
    }
  } catch (const std::exception& error) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
    qWarning() << "[GenICam] frame delivery failed:" << error.what();
  } catch (...) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
    qWarning() << "[GenICam] frame delivery failed with an unknown exception.";
  }
}

void OneGenSourceController::deliverDirect(const onegen::producer::SharedFrame& shared)
{
  try {
    if (!shared.lease) return;
    auto* state = static_cast<DirectDestinationState*>(shared.lease->owner().get());
    if (!state || !state->run || !state->builder || !state->region.isOpen()) return;
    GraphicsFrameCallbackGate::Lease callback(state->run->gateToken);
    std::shared_ptr<const DirectRunRecord> activeRun;
    {
      std::lock_guard lock(mutex_);
      activeRun = active_direct_run_;
    }
    if (!callback || activeRun.get() != state->run.get()) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      if (state->region.isOpen()) {
        GraphicsWriteRegion<std::uint8_t> abandoned = std::move(state->region);
      }
      state->builder.reset();
      state->context.reset();
      return;
    }

    const auto& sourceName = state->run->sourceName;
    const auto& source = shared.lease->metadata();
    const auto bytes = shared.lease->bytes();
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    const bool firstFrame = received_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (firstFrame) logFirstAcquisitionInput(source, bytes.size(), true);

    std::string rejectionReason;
    OneGenFrameAnomalies anomalies;
    auto payload = oneGenBuildDirectImageFrame(
        source, sequence, *state->builder, std::move(state->region), bytes.size(),
        &rejectionReason, nullptr, sourceName, &anomalies);
    state->builder.reset();
    state->context.reset();
    const auto readableBytes = bytes.empty() ? source.payload.size() : bytes.size();
    const bool anomalous = anomalies.total() != 0;
    const bool reported = anomalous
        && reportAnomalies(anomalies, source, sequence, readableBytes, true, payload, false);
    if (!payload) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      if (reported) return;
      qWarning().noquote() << QStringLiteral(
          "[GenICam] direct frame rejected: camera=%1 sequence=%2 frame=%3 reason=%4")
          .arg(QString::fromStdString(sourceName))
          .arg(static_cast<qulonglong>(sequence))
          .arg(static_cast<qulonglong>(source.frame_id))
          .arg(QString::fromStdString(rejectionReason));
      return;
    }
    converted_frames_.fetch_add(1, std::memory_order_relaxed);
    if (anomalous) {
      const auto fitted = partial_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (fitted == 1 || fitted % 100 == 0)
        qWarning().noquote() << "[GenICam] direct frame fitted and published: camera=" << QString::fromStdString(sourceName)
            << "sequence=" << sequence << "frameId=" << source.frame_id << "anomalies=" << anomalies.total()
            << "fittedFrames=" << fitted;
    }
    SessionFrame frame;
    frame.payload = std::move(*payload);
    frame.frameSeq = sequence;
    FrameConsumer consumer;
    {
      std::lock_guard lock(mutex_);
      consumer = consumer_;
    }
    if (!consumer) {
      qWarning() << "[GenICam] direct frame converted but no presentation consumer is installed."
                 << "sequence=" << sequence << "frameId=" << source.frame_id;
      return;
    }
    consumer(std::move(frame), source.stream_index);
    if (!forwarded_by_parent_) delivered_frames_.fetch_add(1, std::memory_order_relaxed);
  } catch (const std::exception& error) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
    qWarning() << "[GenICam] direct frame delivery failed:" << error.what();
  } catch (...) {
    rejected_frames_.fetch_add(1, std::memory_order_relaxed);
    qWarning() << "[GenICam] direct frame delivery failed with an unknown exception.";
  }
}

void OneGenSourceController::setFrameConsumer(FrameConsumer consumer)
{
  std::lock_guard lock(mutex_);
  consumer_ = std::move(consumer);
}

namespace {
std::string deviceKey(const onegen::DeviceInfo& device) {
  return device.cti_path + '\0' + device.interface_id + '\0' + device.id;
}
std::string interfaceKey(const onegen::DeviceInfo& device) {
  return device.cti_path + '\0' + device.interface_id;
}
std::string interfaceKey(const onegen::InterfaceInfo& info) {
  return info.cti_path + '\0' + info.id;
}
QString targetId(const std::string& key) {
  return QString::fromLatin1(QByteArray(key.data(), qsizetype(key.size())).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
std::optional<std::vector<std::string>> decodeTargetId(const std::string& value) {
  const auto decoded = QByteArray::fromBase64(QByteArray::fromStdString(value),
      QByteArray::Base64UrlEncoding);
  if (decoded.isEmpty()) return std::nullopt;
  std::vector<std::string> parts;
  const std::string text = decoded.toStdString();
  std::size_t begin = 0;
  while (true) {
    const auto end = text.find('\0', begin);
    parts.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  if (parts.size() != 2 && parts.size() != 3) return std::nullopt;
  if (std::any_of(parts.begin(), parts.end(), [](const auto& part) { return part.empty(); }))
    return std::nullopt;
  return parts;
}
DeviceConnectionInterfaceInfo publishedInterfaceInfo(const onegen::InterfaceInfo& info) {
  const auto scope = onegen::interfaceClaimScope(info.tl_type);
  return {info.tl_type, scope != onegen::InterfaceClaimScope::Camera,
      onegen::interfaceClaimReason(scope, info.tl_type)};
}
onegen::InterfaceInfo fallbackInterfaceInfo(const onegen::DeviceInfo& device) {
  return {device.interface_id, device.cti_path, device.interface_id, {}, device.tl_type};
}
DeviceConnectionTargets copiedTargets(const std::vector<onegen::DeviceInfo>& inventory,
    const std::vector<onegen::InterfaceInfo>& interfaces) {
  std::map<std::string, onegen::InterfaceInfo> infoByKey;
  for (const auto& info : interfaces) infoByKey[interfaceKey(info)] = info;
  std::vector<onegen::DeviceInfo> sorted = inventory;
  std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
    return std::tie(left.cti_path, left.interface_id, left.id)
        < std::tie(right.cti_path, right.interface_id, right.id);
  });
  for (const auto& device : sorted) infoByKey.try_emplace(interfaceKey(device), fallbackInterfaceInfo(device));
  DeviceConnectionTargets result;
  for (const auto& device : sorted) {
    result.targets.push_back({targetId(deviceKey(device)).toStdString(),
        device.display_name.empty() ? device.id : device.display_name,
        {{"deviceId", device.id}, {"producer", device.cti_path}, {"interfaceId", device.interface_id}},
        DeviceConnectionTargetKind::Device, targetId(interfaceKey(device)).toStdString(), std::nullopt});
  }
  for (const auto& [key, info] : infoByKey) {
    result.targets.push_back({targetId(key).toStdString(),
        info.display_name.empty() ? info.id : info.display_name,
        {{"producer", info.cti_path}, {"interfaceId", info.id}},
        DeviceConnectionTargetKind::Interface, {}, publishedInterfaceInfo(info)});
  }
  return result;
}
DeviceConnectionResult refused(DeviceConnectionError error, const char* message) { return {error, message}; }
}

DeviceConnectionState OneGenSourceController::connectionState() const {
  std::lock_guard lock(mutex_);
  if (interface_) return interfaceStateLocked();
  if (owned_lease_) {
    const auto& info = owned_lease_->info();
    const bool connected = owned_lease_->alive();
    DeviceConnectionState state;
    state.connected = connected;
    state.live = connected && grabbing_.load();
    state.connectionId = connected ? connection_id_.toStdString() : std::string();
    state.targetId = connected ? targetId(deviceKey(info)).toStdString() : std::string();
    state.displayName = connected ? (info.display_name.empty() ? info.id : info.display_name) : std::string();
    state.deliveredFrames = delivered_frames_.load();
    DeviceConnectionAvailabilitySet capabilities;
    const DeviceConnectionAvailability availability = coordinator_ ? DeviceConnectionAvailability{true, {}}
        : DeviceConnectionAvailability{false, "Device control has no coordinator."};
    capabilities.targets = capabilities.discover = capabilities.connect = capabilities.disconnect = availability;
    capabilities.single = modeAvailability(control_features_, "SingleFrame", "SingleFrame");
    capabilities.live = modeAvailability(control_features_, "Continuous", "Continuous");
    state.capabilities = capabilities;
    return state;
  }
  std::vector<const Child*> cameras;
  for (const auto& [id, child] : children_) cameras.push_back(&child);
  std::sort(cameras.begin(), cameras.end(),
      [](const Child* left, const Child* right) { return left->index < right->index; });
  for (const auto* child : cameras) {
    const auto iterator = std::find_if(children_.begin(), children_.end(),
        [child](const auto& entry) { return &entry.second == child; });
    if (iterator == children_.end()) continue;
    auto state = cameraStateLocked(iterator->first, *child);
    if (state.connected) return state;
  }
  return {false, false, {}, {}, {}, delivered_frames_.load()};
}

DeviceConnectionState OneGenSourceController::interfaceStateLocked() const {
  DeviceConnectionState state;
  if (!interface_) return state;
  state.connected = true;
  state.connectionId = interface_->connectionId.toStdString();
  state.targetId = targetId(interfaceKey(interface_->info)).toStdString();
  state.displayName = interface_->info.display_name.empty() ? interface_->info.id
                                                           : interface_->info.display_name;
  state.kind = DeviceConnectionTargetKind::Interface;
  state.error = interface_->error;
  state.capabilities = interfaceAvailability();
  state.implicitlyOpened = interface_->implicit;
  state.interfaceInfo = publishedInterfaceInfo(interface_->info);
  return state;
}

DeviceConnectionState OneGenSourceController::cameraStateLocked(
    const std::string& key, const Child& child) const {
  DeviceConnectionState state;
  const auto& controller = *child.controller;
  std::lock_guard lock(controller.mutex_);
  if (!controller.owned_lease_ || !controller.owned_lease_->alive()) return state;
  const auto& info = controller.owned_lease_->info();
  state.connected = true;
  state.live = controller.grabbing_.load();
  state.connectionId = controller.connection_id_.toStdString();
  state.targetId = targetId(deviceKey(info)).toStdString();
  state.displayName = info.display_name.empty() ? info.id : info.display_name;
  state.deliveredFrames = controller.delivered_frames_.load();
  state.kind = DeviceConnectionTargetKind::Device;
  state.parentConnectionId = interface_ ? interface_->connectionId.toStdString() : std::string();
  state.sources.push_back({key, child.index});
  {
    std::lock_guard errors(controller.error_state_->mutex);
    state.error = controller.error_state_->message;
  }
  if (!controller.discovery_error_.empty()) {
    if (!state.error.empty()) state.error += "; ";
    state.error += controller.discovery_error_;
  }
  DeviceConnectionAvailabilitySet capabilities;
  const DeviceConnectionAvailability availability = coordinator_ ? DeviceConnectionAvailability{true, {}}
      : DeviceConnectionAvailability{false, "Device control has no coordinator."};
  capabilities.targets = capabilities.discover = capabilities.connect = capabilities.disconnect = availability;
  capabilities.single = modeAvailability(controller.control_features_, "SingleFrame", "SingleFrame");
  capabilities.live = modeAvailability(controller.control_features_, "Continuous", "Continuous");
  state.capabilities = capabilities;
  return state;
}

DeviceConnectionAvailabilitySet OneGenSourceController::interfaceAvailability() const {
  DeviceConnectionAvailabilitySet capabilities;
  const DeviceConnectionAvailability availability = coordinator_ ? DeviceConnectionAvailability{true, {}}
      : DeviceConnectionAvailability{false, "Device control has no coordinator."};
  capabilities.targets = capabilities.discover = capabilities.connect = capabilities.disconnect = availability;
  capabilities.single = {false, "An Interface has no data stream; acquire on a connected camera."};
  capabilities.live = capabilities.single;
  return capabilities;
}

std::vector<DeviceConnectionState> OneGenSourceController::connectionStates() const {
  std::lock_guard lock(mutex_);
  std::vector<DeviceConnectionState> result;
  if (interface_) result.push_back(interfaceStateLocked());
  std::vector<const Child*> cameras;
  for (const auto& [id, child] : children_) cameras.push_back(&child);
  std::sort(cameras.begin(), cameras.end(),
      [](const Child* left, const Child* right) { return left->index < right->index; });
  for (const auto* child : cameras) {
    const auto iterator = std::find_if(children_.begin(), children_.end(),
        [child](const auto& entry) { return &entry.second == child; });
    if (iterator == children_.end()) continue;
    auto state = cameraStateLocked(iterator->first, *child);
    if (state.connected) result.push_back(std::move(state));
  }
  return result;
}

DeviceConnectionState OneGenSourceController::stateForConnection(const std::string& connectionId) const {
  std::lock_guard lock(mutex_);
  if (interface_ && interface_->connectionId.toStdString() == connectionId)
    return interfaceStateLocked();
  for (const auto& [key, child] : children_) {
    auto state = cameraStateLocked(key, child);
    if (state.connectionId == connectionId) return state;
  }
  return {};
}

onegen::InterfaceInfo OneGenSourceController::interfaceInfoFor(const onegen::DeviceInfo& device) const {
  if (coordinator_) try {
    const auto interfaces = coordinator_->interfaces();
    const auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto& info) {
      return info.cti_path == device.cti_path && info.id == device.interface_id;
    });
    if (found != interfaces.end()) return *found;
  } catch (...) {
  }
  return fallbackInterfaceInfo(device);
}

DeviceConnectionCapabilities OneGenSourceController::connectionCapabilities() const {
  const DeviceConnectionAvailability availability = coordinator_ ? DeviceConnectionAvailability{true, {}}
      : DeviceConnectionAvailability{false, "Device control has no coordinator."};
  DeviceConnectionCapabilities capabilities{availability, availability, availability, availability,
      coordinator_ ? singleAvailability()
                   : DeviceConnectionAvailability{false, "Device control has no coordinator."},
      availability};
  capabilities.connectionModel = DeviceConnectionModel::Hierarchical;
  return capabilities;
}

DeviceConnectionResult OneGenSourceController::requestConnectionTargets(TargetsCompletion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  DeviceConnectionTargets value;
  {
    std::vector<onegen::DeviceInfo> devices;
    { std::lock_guard lock(mutex_); devices = devices_; }
    std::vector<onegen::InterfaceInfo> interfaces;
    try { interfaces = coordinator_->interfaces(); } catch (...) {}
    value = copiedTargets(devices, interfaces);
  }
  try { completion(std::move(value)); } catch (...) { qWarning() << "[GenICam] lifecycle consumer callback threw."; }
  return {};
}

DeviceConnectionResult OneGenSourceController::requestConnectionState(IDeviceConnectionControl::Completion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  try { completion({{}, {}, {}, connectionState()}); } catch (...) { qWarning() << "[GenICam] lifecycle consumer callback threw."; }
  return {};
}

DeviceConnectionResult OneGenSourceController::requestConnectionStates(StatesCompletion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  DeviceConnectionStates value;
  value.connections = connectionStates();
  try { completion(std::move(value)); } catch (...) { qWarning() << "[GenICam] lifecycle consumer callback threw."; }
  return {};
}

std::optional<OneGenSourceController::ResolvedTarget> OneGenSourceController::resolveTarget(
    const std::string& target) const {
  const auto parts = decodeTargetId(target);
  if (!parts) return std::nullopt;
  if (parts->size() == 2) {
    ResolvedTarget result;
    result.isInterface = true;
    result.interfaceInfo = {(*parts)[1], (*parts)[0], (*parts)[1], {}, {}};
    if (coordinator_) try {
      for (const auto& info : coordinator_->interfaces())
        if (info.cti_path == (*parts)[0] && info.id == (*parts)[1]) {
          result.interfaceInfo = info;
          break;
        }
    } catch (...) {
    }
    if (result.interfaceInfo.tl_type.empty()) {
      std::lock_guard lock(mutex_);
      for (const auto& device : devices_)
        if (device.cti_path == (*parts)[0] && device.interface_id == (*parts)[1]) {
          result.interfaceInfo = interfaceInfoFor(device);
          break;
        }
    }
    return result;
  }
  const std::string key = (*parts)[0] + '\0' + (*parts)[1] + '\0' + (*parts)[2];
  std::lock_guard lock(mutex_);
  for (const auto& device : devices_) {
    if (deviceKey(device) != key) continue;
    return ResolvedTarget{false, device, interfaceInfoFor(device)};
  }
  return std::nullopt;
}

onegen::InterfaceInfo OneGenSourceController::ensureCameraInterface(const onegen::DeviceInfo& device) {
  const auto info = interfaceInfoFor(device);
  {
    std::lock_guard lock(mutex_);
    if (interface_) {
      if (interface_->info.cti_path != info.cti_path || interface_->info.id != info.id)
        throw std::runtime_error("The session already owns another Interface.");
      return interface_->info;
    }
  }
  try {
    coordinator_->open_interface(info);
  } catch (const std::exception& error) {
    // A GUI-opened Interface is already pinned; adopt it instead of failing the camera connect.
    if (std::string(error.what()).find("already connected") == std::string::npos) throw;
  }
  std::lock_guard lock(mutex_);
  if (!interface_) interface_ = InterfaceConnection{info, QUuid::createUuid().toString(QUuid::WithoutBraces),
      true, 0, {}};
  return interface_->info;
}

void OneGenSourceController::closeIdleInterface() {
  onegen::InterfaceInfo info;
  QString connectionId;
  {
    std::lock_guard lock(mutex_);
    if (!interface_ || !interface_->implicit || interface_->refCount > 0) return;
    info = interface_->info;
    connectionId = interface_->connectionId;
  }
  try {
    if (coordinator_) coordinator_->close_interface(info);
  } catch (const std::exception& error) {
    std::lock_guard lock(mutex_);
    if (interface_ && interface_->connectionId == connectionId) interface_->error = error.what();
    return;
  }
  std::lock_guard lock(mutex_);
  if (interface_ && interface_->connectionId == connectionId) interface_.reset();
}

void OneGenSourceController::setDiscoveryError(std::string message) {
  std::lock_guard lock(mutex_);
  discovery_error_ = std::move(message);
}

void OneGenSourceController::annotateDiscovery(
    const std::vector<onegen::DeviceInfo>& discovered, bool interfacePresent) {
  const std::string interfaceMarker = "The Interface was not discovered.";
  const std::string cameraMarker = "The camera was not discovered.";
  std::lock_guard lock(mutex_);
  if (interface_) {
    if (!interfacePresent) {
      if (interface_->error.empty()) interface_->error = interfaceMarker;
    } else if (interface_->error == interfaceMarker) {
      interface_->error.clear();
    }
  }
  for (const auto& [key, child] : children_) {
    const bool found = std::any_of(discovered.begin(), discovered.end(),
        [&](const auto& device) { return deviceKey(device) == key; });
    auto& controller = *child.controller;
    std::lock_guard childLock(controller.mutex_);
    if (!found) {
      if (controller.discovery_error_.empty()) controller.discovery_error_ = cameraMarker;
    } else if (controller.discovery_error_ == cameraMarker) {
      controller.discovery_error_.clear();
    }
  }
}

DeviceConnectionResult OneGenSourceController::requestDiscover(TargetsCompletion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::Discover, {}, {}, std::move(completion));
}
DeviceConnectionResult OneGenSourceController::requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::Connect, std::move(targetId), std::move(completion), {});
}
DeviceConnectionResult OneGenSourceController::requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::Disconnect, std::move(connectionId), std::move(completion), {});
}
DeviceConnectionResult OneGenSourceController::requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::Single, std::move(connectionId), std::move(completion), {});
}
DeviceConnectionResult OneGenSourceController::requestLiveStart(std::string connectionId, IDeviceConnectionControl::Completion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::LiveStart, std::move(connectionId), std::move(completion), {});
}
DeviceConnectionResult OneGenSourceController::requestLiveStop(std::string connectionId, IDeviceConnectionControl::Completion completion) {
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  return admitLifecycle(Lifecycle::LiveStop, std::move(connectionId), std::move(completion), {});
}

bool OneGenSourceController::controllerBusy(const OneGenSourceController& controller) {
  return controller.external_busy_.load() || controller.gui_busy_.load()
      || controller.control_blocked_ || controller.active_work_.load() > 0
      || controller.parameter_work_.load() > 0;
}

DeviceConnectionResult OneGenSourceController::admitLifecycle(Lifecycle operation, std::string argument,
    IDeviceConnectionControl::Completion completion, TargetsCompletion targetsCompletion,
    DiscoveryFn discovery) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  const bool structural = operation == Lifecycle::Discover || operation == Lifecycle::Connect
      || operation == Lifecycle::Disconnect;
  if (structural) {
    if (controllerBusy(*this)) return refused(DeviceConnectionError::Busy, "Discovery, Interface or control work is active.");
    std::lock_guard lock(mutex_);
    for (const auto& [id, child] : children_)
      if (controllerBusy(*child.controller))
        return refused(DeviceConnectionError::Busy, "A per-camera control operation is active.");
  }
  // Removed cameras are destroyed only on a later admission, never from their own work future.
  retired_.clear();
  OneGenSourceController* target = nullptr;
  onegen::DeviceInfo selected;
  std::optional<ResolvedTarget> resolved;
  bool interfaceOperation = false;
  if (operation == Lifecycle::Discover) {
    target = this;
  } else if (operation == Lifecycle::Connect) {
    resolved = resolveTarget(argument);
    if (!resolved) return refused(DeviceConnectionError::NotFound, "Exact declared target was not found.");
    if (resolved->isInterface) {
      const auto state = connectionState();
      if (state.connected && state.kind == DeviceConnectionTargetKind::Interface)
        return refused(DeviceConnectionError::Conflict, "The Interface is already connected.");
      if (state.connected) return refused(DeviceConnectionError::Conflict, "The session already owns another Interface.");
      target = this;
      interfaceOperation = true;
    } else {
      selected = resolved->device;
      {
        std::lock_guard lock(mutex_);
        if (children_.count(deviceKey(selected)))
          return refused(DeviceConnectionError::Conflict, "The camera is already connected.");
        if (interface_ && (interface_->info.cti_path != resolved->interfaceInfo.cti_path
                || interface_->info.id != resolved->interfaceInfo.id))
          return refused(DeviceConnectionError::Conflict, "The session already owns another Interface.");
      }
      emit presentDevice(QString::fromStdString("Device|" + deviceKey(selected)));
      target = cameraController(deviceKey(selected), selected.display_name.empty() ? selected.id : selected.display_name);
    }
  } else {
    {
      std::lock_guard lock(mutex_);
      if (interface_ && interface_->connectionId.toStdString() == argument) {
        interfaceOperation = true;
        target = this;
      } else {
        for (const auto& [key, child] : children_) {
          auto state = cameraStateLocked(key, child);
          if (state.connectionId != argument) continue;
          target = child.controller.get();
          break;
        }
      }
      if (operation == Lifecycle::Disconnect && interfaceOperation && interface_->implicit)
        return refused(DeviceConnectionError::Conflict,
            "The Interface was opened implicitly by its cameras and closes with the last camera.");
    }
    if (!target) return refused(DeviceConnectionError::Disconnected, "The connection is not open.");
    if (operation == Lifecycle::Single && target != this && target->isGrabbing())
      return refused(DeviceConnectionError::Busy, "Stop Live explicitly before single acquisition.");
  }
  if (!structural) {
    if (structural_work_.load() > 0 || control_blocked_)
      return refused(DeviceConnectionError::Busy, "Structural device work is active.");
    if (target == this && (gui_busy_.load() || active_work_.load() > 0))
      return refused(DeviceConnectionError::Busy, "A control operation is active on this connection.");
    if (target != this && (target->external_busy_.load() || target->gui_busy_.load()
        || target->active_work_.load() > 0))
      return refused(DeviceConnectionError::Busy, "A control operation is active on this connection.");
  }
  external_busy_.store(true);
  target->external_busy_.store(true);
  active_work_.fetch_add(1);
  target->active_work_.fetch_add(1);
  if (structural) structural_work_.fetch_add(1);
  emit target->controlChanged();
  auto work = QtConcurrent::run([this, target, selected, operation, completion, targetsCompletion,
                                    discovery = std::move(discovery),
                                    reason = std::move(argument), resolved, interfaceOperation, structural] {
    DeviceConnectionResult result{DeviceConnectionError::BackendFailure, "Device operation did not complete.", DeviceConnectionEffect::Unknown};
    DeviceConnectionTargets discovered;
    std::string connectionId = reason;
    const auto finishState = [this, &connectionId] {
      return connectionId.empty() ? DeviceConnectionState{} : stateForConnection(connectionId);
    };
    try {
      if (operation == Lifecycle::Discover) {
        std::vector<onegen::DeviceInfo> inventoryDevices;
        if (discovery) {
          inventoryDevices = discovery();
        } else {
          auto inventory = coordinator_->discover();
          if (!inventory.completed) throw std::runtime_error("Discovery did not complete; previous inventory is preserved.");
          inventoryDevices = std::move(inventory.devices);
        }
        { std::lock_guard lock(mutex_); devices_ = inventoryDevices; }
        std::optional<onegen::InterfaceInfo> tracked;
        {
          std::lock_guard lock(mutex_);
          if (interface_) tracked = interface_->info;
        }
        bool interfacePresent = true;
        if (tracked) {
          interfacePresent = false;
          try {
            for (const auto& info : coordinator_->interfaces()) {
              if (info.cti_path == tracked->cti_path && info.id == tracked->id) {
                interfacePresent = true;
                break;
              }
            }
          } catch (...) {
          }
        }
        annotateDiscovery(inventoryDevices, interfacePresent);
        std::vector<onegen::InterfaceInfo> interfaces;
        try { interfaces = coordinator_->interfaces(); } catch (...) {}
        discovered = copiedTargets(inventoryDevices, interfaces);
        emit inventoryChanged();
      } else if (operation == Lifecycle::Connect && resolved && resolved->isInterface) {
        try {
          coordinator_->open_interface(resolved->interfaceInfo);
        } catch (const std::exception& error) {
          if (std::string(error.what()).find("already connected") == std::string::npos) throw;
        }
        std::lock_guard lock(mutex_);
        if (!interface_) interface_ = InterfaceConnection{resolved->interfaceInfo,
            QUuid::createUuid().toString(QUuid::WithoutBraces), false, 0, {}};
        connectionId = interface_->connectionId.toStdString();
      } else if (operation == Lifecycle::Connect) {
        OneGenSourceController* camera = cameraController(deviceKey(selected),
            selected.display_name.empty() ? selected.id : selected.display_name);
        try {
          ensureCameraInterface(selected);
          camera->openDevice(coordinator_, selected);
        } catch (...) {
          {
            std::lock_guard lock(mutex_);
            const auto found = children_.find(deviceKey(selected));
            if (found != children_.end()) {
              retired_.push_back(std::move(found->second.controller));
              children_.erase(found);
            }
            if (children_.empty()) next_source_index_ = 0;
          }
          publishSourceDescriptors();
          closeIdleInterface();
          throw;
        }
        {
          std::lock_guard lock(mutex_);
          QString parent;
          if (interface_) {
            ++interface_->refCount;
            parent = interface_->connectionId;
          }
          std::lock_guard cameraLock(camera->mutex_);
          camera->parent_connection_id_ = parent;
          connectionId = camera->connection_id_.toStdString();
        }
      } else if (operation == Lifecycle::Disconnect && interfaceOperation) {
        {
          std::lock_guard lock(mutex_);
          if (!interface_ || interface_->connectionId.toStdString() != connectionId)
            throw std::runtime_error("The Interface connection changed; observe it again.");
        }
        std::vector<std::pair<std::string, OneGenSourceController*>> cameras;
        {
          std::lock_guard lock(mutex_);
          for (const auto& [key, child] : children_) cameras.emplace_back(key, child.controller.get());
        }
        std::exception_ptr failure;
        std::vector<std::string> removed;
        for (const auto& [key, camera] : cameras) try {
          camera->closeDevice();
          removed.push_back(key);
        } catch (...) {
          if (!failure) failure = std::current_exception();
        }
        {
          std::lock_guard lock(mutex_);
          for (const auto& key : removed) {
            const auto found = children_.find(key);
            if (found == children_.end()) continue;
            retired_.push_back(std::move(found->second.controller));
            children_.erase(found);
          }
          if (children_.empty()) next_source_index_ = 0;
          if (interface_) {
            if (failure) interface_->error = "A camera did not disconnect; the Interface stays open.";
            else interface_->refCount = 0;
          }
        }
        if (failure) {
          result = {DeviceConnectionError::BackendFailure,
              "A camera did not disconnect; the Interface stays open.", DeviceConnectionEffect::Unknown,
              stateForConnection(connectionId)};
          std::rethrow_exception(failure);
        }
        onegen::InterfaceInfo closed;
        {
          std::lock_guard lock(mutex_);
          if (!interface_) throw std::runtime_error("The Interface connection changed; observe it again.");
          closed = interface_->info;
        }
        coordinator_->close_interface(closed);
        {
          std::lock_guard lock(mutex_);
          if (interface_ && interface_->connectionId.toStdString() == connectionId) interface_.reset();
        }
        publishSourceDescriptors();
        connectionId.clear();
        result = {{}, {}, DeviceConnectionEffect::Applied, DeviceConnectionState{}};
      } else if (operation == Lifecycle::Disconnect) {
        target->closeDevice();
        std::string key;
        {
          std::lock_guard lock(mutex_);
          for (const auto& [candidate, child] : children_)
            if (child.controller.get() == target) { key = candidate; break; }
          if (!key.empty()) {
            const auto found = children_.find(key);
            if (found != children_.end()) {
              retired_.push_back(std::move(found->second.controller));
              children_.erase(found);
            }
            if (children_.empty()) next_source_index_ = 0;
            if (interface_ && interface_->refCount > 0) --interface_->refCount;
          }
        }
        closeIdleInterface();
        publishSourceDescriptors();
        connectionId.clear();
        result = {{}, {}, DeviceConnectionEffect::Applied, DeviceConnectionState{}};
      } else if (operation == Lifecycle::Single) {
        const auto before = target->converted_frames_.load();
        target->grabOne();
        if (target->converted_frames_.load() != before + 1) {
          std::string singleError;
          {
            std::lock_guard lock(target->mutex_);
            singleError = target->last_single_failure_;
          }
          throw std::runtime_error(singleError.empty()
              ? "Acquired frame was not converted to a supported resource." : singleError);
        }
        result = {{}, {}, DeviceConnectionEffect::Applied, finishState()};
      } else if (operation == Lifecycle::LiveStart) {
        target->start();
        result = {{}, {}, DeviceConnectionEffect::Applied, finishState()};
      } else {
        target->stopLive();
        result = {{}, {}, DeviceConnectionEffect::Applied, finishState()};
      }
      if (operation != Lifecycle::Disconnect && operation != Lifecycle::Discover && operation != Lifecycle::Connect)
        try { target->refreshControlSnapshot(); } catch (const std::exception& error) { qWarning() << "[GenICam] feature refresh failed:" << error.what(); }
      if (operation == Lifecycle::Connect)
        result = {{}, {}, DeviceConnectionEffect::Applied, finishState()};
      else if (result.error == DeviceConnectionError::None && operation != Lifecycle::Discover
          && operation != Lifecycle::Disconnect)
        result.state = finishState();
    } catch (const onegen::DeviceClaimRefused& error) {
      // The claim is taken before the Device opens, so the refusal has no effect.
      result = {DeviceConnectionError::Conflict, error.what(), DeviceConnectionEffect::None,
          finishState()};
    } catch (const OneGenAcquisitionModeRefused& error) {
      // The mode write is the action's first effect; refusal leaves acquisition untouched.
      result = {DeviceConnectionError::Unavailable, error.what(), DeviceConnectionEffect::None,
          finishState()};
    } catch (const std::exception& error) {
      // Discovery keeps the previous inventory, so only device commands leave an uncertain effect.
      result = {DeviceConnectionError::BackendFailure, error.what(),
          operation == Lifecycle::Discover ? DeviceConnectionEffect::None : DeviceConnectionEffect::Unknown,
          finishState()};
      discovered.result = {DeviceConnectionError::BackendFailure, error.what()};
    } catch (...) {
      result = {DeviceConnectionError::BackendFailure, "Unexpected device backend failure.", DeviceConnectionEffect::Unknown, finishState()};
      discovered.result = {DeviceConnectionError::BackendFailure, "Unexpected device backend failure."};
    }
    target->external_busy_.store(false);
    external_busy_.store(false);
    target->active_work_.fetch_sub(1);
    active_work_.fetch_sub(1);
    if (structural) structural_work_.fetch_sub(1);
    emit target->controlChanged();
    emit controlChanged();
    try {
      if (operation == Lifecycle::Discover) targetsCompletion(std::move(discovered));
      else completion(std::move(result));
    } catch (...) { qWarning() << "[GenICam] lifecycle consumer callback threw."; }
  });
  {
    std::lock_guard lock(work_mutex_);
    std::erase_if(admitted_work_, [](const QFuture<void>& work) { return work.isFinished(); });
    admitted_work_.push_back(work);
  }
  if (structural || target == this) tool_work_ = work;
  if (target != this) target->tool_work_ = work;
  return {};
}
