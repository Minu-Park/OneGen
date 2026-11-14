#include "OneGenSourceController.h"
#include "../../Qt/SingleCaptureDiagnostics.h"

#include "ProducerCoordinator.h"
#include "OneGenGraphicsFrameAdapter.h"
#include "../DirectPoolPolicy.h"
#include "SessionFrame.h"
#include "Internal/FrameTrace.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
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
// Host Debug enables diagnostics.* debug output; per-run detail dumps stay behind it.
Q_LOGGING_CATEGORY(onegenSourceDiagnosticLog, "diagnostics.OneGen", QtInfoMsg)

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

} // namespace

OneGenSourceController::OneGenSourceController(QObject* parent)
    : AbstractSourceController(parent) {}

OneGenSourceController::~OneGenSourceController()
{
  tool_work_.waitForFinished();
  try { closeDevice(); } catch (...) {}
  try { stop(); } catch (...) {}
  setLease(nullptr);
  for (auto& [id, child] : children_) child.controller->setLease(nullptr);
  children_.clear();
  owned_lease_.reset();
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
  { std::lock_guard lock(mutex_); owned_lease_.reset(); connection_id_.clear(); control_features_.clear(); control_streams_.clear(); }
  emit controlChanged();
}

OneGenSourceController* OneGenSourceController::cameraController(const std::string& id, const std::string& label)
{
  OneGenSourceController* result;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = children_.find(id); found != children_.end()) return found->second.controller.get();
    const auto index = next_source_index_++;
    auto child = std::make_unique<OneGenSourceController>();
    connect(child.get(), &AbstractSourceController::diagnosticAcquisitionChanged, this,
        [this, id](bool active, const QString&) { emit diagnosticAcquisitionChanged(active, QString::fromStdString(id)); },
        Qt::DirectConnection);
    child->forwarded_by_parent_ = true;
    child->setFrameStorageProvider(frame_storage_);
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
  std::unique_lock poolLock(direct_pool_mutex_);
  std::unique_lock lock(mutex_);
  if (lease_ == lease) return;
  auto old_gate = std::move(callback_gate_);
  if (old_gate) old_gate->beginShutdown();
  if (lease_) {
    lease_->set_frame_callback({});
    lease_->set_shared_frame_callback({});
    lease_->set_error_callback({});
    lease_->destroy_frame_pool();
  }
  frame_pool_.reset();
  pool_slots_.store(0, std::memory_order_relaxed);
  lock.unlock();
  // Frame callbacks never acquire direct_pool_mutex_; holding it across the gate
  // drain prevents start from attaching a pool to the lease being replaced.
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

void OneGenSourceController::bindLease()
{
  if (!lease_) return;
  callback_gate_ = std::make_unique<GraphicsFrameCallbackGate>();
  const auto token = callback_gate_->token();
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

  lease_->set_frame_callback(
      [this, sourceName, token](const onegen::Frame& source) {
        GraphicsFrameCallbackGate::Lease callback(token);
        if (callback) deliver(source, sourceName);
      });
  lease_->set_error_callback([model = info.model, serial = info.serial](const std::string_view error) {
    qWarning().noquote()
        << QStringLiteral("[GenICam] camera=%1 serial=%2; cause=%3")
               .arg(QString::fromStdString(model), QString::fromStdString(serial),
                    QString::fromLocal8Bit(error.data(), static_cast<qsizetype>(error.size())));
  });
}

void OneGenSourceController::setFrameStorageProvider(std::shared_ptr<IFrameStorageProvider> provider)
{
  std::vector<OneGenSourceController*> children;
  {
    std::lock_guard lock(mutex_);
    frame_storage_ = provider;
    for (const auto& child : children_) children.push_back(child.second.controller.get());
  }
  for (auto* child : children) child->setFrameStorageProvider(provider);
}

void OneGenSourceController::configureFramePool(
    onegen::producer::Lease& lease, const std::string& sourceName)
{
  bool attached = false;
  std::optional<std::uint64_t> attachedSlotBytes;
  GraphicsFrameCallbackGate::Token token;
  {
    std::lock_guard lock(mutex_);
    attached = static_cast<bool>(frame_pool_);
    if (frame_pool_) attachedSlotBytes = frame_pool_->slot_bytes();
    if (callback_gate_) token = callback_gate_->token();
  }
  if (!token) throw std::runtime_error("Frame delivery is not bound to a consumer.");

  std::string error;
  const auto payload = directSlotBytes(lease, error);
  const auto decision = onegen::direct_pool::decide(attached, attachedSlotBytes, payload);
  if (decision.action == onegen::direct_pool::Action::Reuse) return;
  if (decision.action == onegen::direct_pool::Action::Disabled)
    throw std::runtime_error(error.empty() ? decision.reason : error);

  lease.destroy_frame_pool();
  {
    std::lock_guard lock(mutex_);
    frame_pool_.reset();
    pool_slots_.store(0, std::memory_order_relaxed);
  }
  const auto plan = onegen::producer::plan_frame_pool(decision.slot_bytes,
      onegen::direct_pool::minimum_slots, onegen::direct_pool::preferred_slots,
      onegen::direct_pool::budget_bytes);
  if (!plan.refusal.empty()) throw std::runtime_error(plan.refusal);
  onegen::producer::FramePool::FrameStorage storage;
  {
    std::lock_guard lock(mutex_);
    if (frame_storage_) storage = frameStorageFunction(frame_storage_);
  }
  if (!lease.create_frame_pool(plan.slot_count, plan.slot_bytes, error, std::move(storage))) {
    throw std::runtime_error(error);
  }
  lease.set_shared_frame_callback(
      [this, sourceName, token](const onegen::producer::SharedFrame& source) {
        GraphicsFrameCallbackGate::Lease callback(token);
        if (callback) deliverShared(source, sourceName);
      });
  {
    std::lock_guard lock(mutex_);
    frame_pool_ = lease.frame_pool();
    pool_slots_.store(plan.slot_count, std::memory_order_relaxed);
  }
  qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral(
      "[GenICam] frame pool attached: camera=%1 slots=%2 slotBytes=%3")
      .arg(QString::fromStdString(sourceName)).arg(plan.slot_count).arg(plan.slot_bytes);
}

void OneGenSourceController::start()
{
  if (QThread::currentThread() == thread() && external_busy_.load()) throw std::runtime_error("Device control is busy.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) child->start();
  onegen::producer::Lease* lease = nullptr;
  {
    std::unique_lock poolLock(direct_pool_mutex_);
    {
      std::lock_guard lock(mutex_);
      lease = lease_;
    }
    if (!lease) return;
    const auto& info = lease->info();
    const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
    configureFramePool(*lease, sourceName);
  }
  received_frames_.store(0, std::memory_order_relaxed);
  converted_frames_.store(0, std::memory_order_relaxed);
  delivered_frames_.store(0, std::memory_order_relaxed);
  rejected_frames_.store(0, std::memory_order_relaxed);
  incomplete_frames_.store(0, std::memory_order_relaxed);
  partial_frames_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    exhausted_baseline_.store(frame_pool_ ? frame_pool_->exhausted() : 0, std::memory_order_relaxed);
  }
  emit diagnosticAcquisitionChanged(true, QStringLiteral("camera"));
  try { lease->live(); }
  catch (...) { emit diagnosticAcquisitionChanged(false, QStringLiteral("camera")); throw; }
  grabbing_.store(true, std::memory_order_relaxed);
  // Direct API start bypasses the GUI operation watcher, so publish the state
  // change for camera panels to refresh their live controls and telemetry.
  emit controlChanged();
}

void OneGenSourceController::stop()
{
  try { stopLive(); }
  catch (const std::exception& error) { qWarning() << "[GenICam] stop unavailable:" << error.what(); }
  catch (...) { qWarning() << "[GenICam] stop failed."; }
}

void OneGenSourceController::stopLive()
{
  if (QThread::currentThread() == thread() && external_busy_.load()) throw std::runtime_error("Device control is busy.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) child->stopLive();
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
    // Match start(): a direct API stop must clear the camera panel's live controls
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
  std::shared_ptr<const onegen::producer::FramePool> pool;
  {
    std::lock_guard lock(mutex_);
    pool = frame_pool_;
  }
  if (!pool) return 0;
  const auto exhausted = pool->exhausted();
  const auto baseline = exhausted_baseline_.load(std::memory_order_relaxed);
  return exhausted > baseline ? exhausted - baseline : 0;
}

SourceProducerCapabilities OneGenSourceController::producerCapabilities(unsigned sourceIndex) const
{
  {
    std::lock_guard lock(mutex_);
    if (!children_.empty()) {
      for (const auto& [id, child] : children_)
        if (child.index == sourceIndex) return child.controller->producerCapabilities(0U);
      return {};
    }
  }
  if (sourceIndex != 0U) return {};
  return {pool_slots_.load(std::memory_order_relaxed)};
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
  delivered_frames_.store(0, std::memory_order_relaxed);
  rejected_frames_.store(0, std::memory_order_relaxed);
  emit diagnosticAcquisitionChanged(true, QStringLiteral("camera"));
  struct DiagnosticEnd {
    OneGenSourceController* controller;
    ~DiagnosticEnd() { emit controller->diagnosticAcquisitionChanged(false, QStringLiteral("camera")); }
  } diagnosticEnd{this};
  lease->grab_one(
      [this, sourceName, diagnosticDirectory](const onegen::Frame& source) {
        deliver(source, sourceName);
        onegen::diagnostics::delivered(source, diagnosticDirectory);
      });
}

void OneGenSourceController::trigger()
{
  onegen::producer::Lease* lease = nullptr;
  { std::lock_guard lock(mutex_); lease = lease_; }
  if (lease) lease->trigger();
}

void OneGenSourceController::deliver(const onegen::Frame& source, const std::string& sourceName)
{
  try {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    const bool firstFrame = received_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (firstFrame) {
      qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral("[GenICam] first frame received: camera=%1 frame=%2 payloadType=%3 PFNC=0x%4 %5x%6 bytes=%7 parts=%8 scan3d=%9")
          .arg(QString::fromStdString(sourceName)).arg(source.frame_id).arg(source.payload_type)
          .arg(QString::number(source.pixel_format, 16)).arg(source.width).arg(source.height)
          .arg(source.payload.size()).arg(source.parts.size()).arg(source.scan3d_geometry.has_value());
      qCDebug(onegenSourceDiagnosticLog) << "[GenICam] first frame chunk count:" << source.chunks.size()
              << "(presence does not establish Scan3d chunk overrides)";
      if (source.scan3d_geometry) {
        const auto& g = *source.scan3d_geometry;
        qCDebug(onegenSourceDiagnosticLog).noquote() << "[GenICam] effective Scan3d: mode=" << QString::fromStdString(g.output_mode)
            << "system=" << QString::fromStdString(g.coordinate_system)
            << "unit=" << QString::fromStdString(g.distance_unit);
        for (std::size_t axis = 0; axis < 3; ++axis)
          qCDebug(onegenSourceDiagnosticLog) << "[GenICam] effective Scan3d axis:" << axis
              << "calibrated=" << g.calibrated_axes[axis] << "scale=" << g.scale[axis]
              << "offset=" << g.offset[axis] << "invalidEnabled=" << g.invalid_enabled[axis]
              << "invalidValue=" << g.invalid_value[axis];
      }
      std::size_t partIndex = 0;
      for (const auto& part : source.parts)
        qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral("[GenICam] first frame part: type=%1 PFNC=0x%2 %3x%4 stride=%5 bytes=%6 source=%7 region=%8 purpose=%9")
            .arg(part.data_type).arg(QString::number(part.data_format, 16)).arg(part.width).arg(part.height)
            .arg(part.stride).arg(part.payload.size()).arg(part.source_id).arg(part.region_id).arg(part.purpose_id)
            << "partIndex=" << partIndex++;
    }
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
    GraphicsImageStorage imageStorage;
    {
      std::lock_guard lock(mutex_);
      imageStorage = frameStorageFunction(frame_storage_);
    }
    std::string rejectionReason;
    auto payload = oneGenGraphicsFrame(source, sequence, &rejectionReason, {}, {},
        std::move(imageStorage));
    const auto converted = frameTrace ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
    if (!payload) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
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
    if (!rejectionReason.empty()) {
      partial_frames_.fetch_add(1, std::memory_order_relaxed);
      qWarning().noquote() << "[GenICam] frame retained with rejected parts: camera="
          << QString::fromStdString(sourceName) << "sequence=" << sequence
          << "frameId=" << source.frame_id << QString::fromStdString(rejectionReason);
    }
    payload->metadata.sourceName = sourceName;
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
    if (firstConverted) qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral("[GenICam] first frame converted: images=%1 ranges=%2 pointClouds=%3; forwarding to presentation")
        .arg(frame.payload.images.size()).arg(frame.payload.ranges.size()).arg(frame.payload.pointClouds.size());
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

void OneGenSourceController::deliverShared(
    const onegen::producer::SharedFrame& shared, const std::string& sourceName)
{
  try {
    if (!shared.lease) return;
    const auto& source = shared.lease->metadata();
    const auto bytes = shared.lease->bytes();
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    const bool firstFrame = received_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (firstFrame) {
      qCDebug(onegenSourceDiagnosticLog).noquote() << QStringLiteral(
          "[GenICam] first direct frame received: camera=%1 frame=%2 bytes=%3")
          .arg(QString::fromStdString(sourceName))
          .arg(static_cast<qulonglong>(source.frame_id))
          .arg(static_cast<qulonglong>(bytes.size()));
    }
    GraphicsImageStorage imageStorage;
    {
      std::lock_guard lock(mutex_);
      imageStorage = frameStorageFunction(frame_storage_);
    }
    std::string rejectionReason;
    auto payload = oneGenGraphicsFrame(
        source, sequence, &rejectionReason, bytes, shared.lease, std::move(imageStorage));
    if (!payload) {
      rejected_frames_.fetch_add(1, std::memory_order_relaxed);
      qWarning().noquote() << QStringLiteral(
          "[GenICam] direct frame rejected: camera=%1 sequence=%2 frame=%3 reason=%4")
          .arg(QString::fromStdString(sourceName))
          .arg(static_cast<qulonglong>(sequence))
          .arg(static_cast<qulonglong>(source.frame_id))
          .arg(QString::fromStdString(rejectionReason));
      return;
    }
    converted_frames_.fetch_add(1, std::memory_order_relaxed);
    payload->metadata.sourceName = sourceName;
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
QString targetId(const std::string& key) {
  return QString::fromLatin1(QByteArray(key.data(), qsizetype(key.size())).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
DeviceConnectionTargets copiedTargets(const std::vector<onegen::DeviceInfo>& inventory) {
  DeviceConnectionTargets result;
  for (const auto& d : inventory) result.targets.push_back({targetId(deviceKey(d)).toStdString(),
      d.display_name.empty() ? d.id : d.display_name,
      {{"deviceId", d.id}, {"producer", d.cti_path}, {"interfaceId", d.interface_id}}});
  return result;
}
DeviceConnectionResult refused(DeviceConnectionError error, const char* message) { return {error, message}; }
}

DeviceConnectionState OneGenSourceController::connectionState() const {
  std::lock_guard lock(mutex_);
  if (owned_lease_) {
    const auto& info = owned_lease_->info();
    const bool connected = owned_lease_->alive();
    return {connected, connected && grabbing_.load(), connected ? connection_id_.toStdString() : std::string(),
        connected ? targetId(deviceKey(info)).toStdString() : std::string(),
        connected ? (info.display_name.empty() ? info.id : info.display_name) : std::string(), delivered_frames_.load()};
  }
  for (const auto& [id, child] : children_) {
    const auto state = child.controller->connectionState();
    if (state.connected) return state;
  }
  return {false, false, {}, {}, {}, delivered_frames_.load()};
}

DeviceConnectionCapabilities OneGenSourceController::connectionCapabilities() const {
  const DeviceConnectionAvailability availability = coordinator_ ? DeviceConnectionAvailability{true, {}}
      : DeviceConnectionAvailability{false, "Device control has no coordinator."};
  return {availability, availability, availability, availability, availability, availability};
}

DeviceConnectionResult OneGenSourceController::requestConnectionTargets(TargetsCompletion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!completion) return refused(DeviceConnectionError::InvalidArgument, "Completion is required.");
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  DeviceConnectionTargets value;
  { std::lock_guard lock(mutex_); value = copiedTargets(devices_); }
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

DeviceConnectionResult OneGenSourceController::admitLifecycle(Lifecycle operation, std::string argument,
    IDeviceConnectionControl::Completion completion, TargetsCompletion targetsCompletion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!coordinator_) return refused(DeviceConnectionError::Unavailable, "Device control has no coordinator.");
  if (external_busy_.load() || (tool_work_.isStarted() && !tool_work_.isFinished()) || control_blocked_ || gui_busy_.load())
    return refused(DeviceConnectionError::Busy, "Discovery, Interface or control work is active.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) if (child->gui_busy_.load() || child->external_busy_.load())
    return refused(DeviceConnectionError::Busy, "A GUI control operation is active.");
  const auto state = connectionState();
  OneGenSourceController* target = nullptr;
  onegen::DeviceInfo selected;
  if (operation == Lifecycle::Discover) {
    if (state.connected) return refused(DeviceConnectionError::Busy, "Disconnect before discovery.");
    target = this;
  } else if (operation == Lifecycle::Connect) {
    if (state.connected) return refused(DeviceConnectionError::Conflict, "Disconnect before connecting another device.");
    { std::lock_guard lock(mutex_);
      for (const auto& device : devices_) if (targetId(deviceKey(device)).toStdString() == argument) selected = device; }
    if (selected.id.empty()) return refused(DeviceConnectionError::NotFound, "Exact declared target was not found.");
    // Presentation observers may select/create their view; they do not execute the command.
    emit presentDevice(QString::fromStdString("Device|" + deviceKey(selected)));
    target = cameraController(deviceKey(selected), selected.display_name.empty() ? selected.id : selected.display_name);
  } else {
    if (!state.connected) return refused(DeviceConnectionError::Disconnected, "Device is disconnected.");
    if (state.connectionId != argument) return refused(DeviceConnectionError::Conflict, "The connection changed; observe it again.");
    for (auto* child : children) if (child->connectionState().connectionId == state.connectionId) target = child;
    if (!target) return refused(DeviceConnectionError::NotFound, "Connection controller ended.");
    if (operation == Lifecycle::Single && state.live)
      return refused(DeviceConnectionError::Busy, "Stop Live explicitly before single acquisition.");
  }
  external_busy_.store(true);
  target->external_busy_.store(true);
  emit target->controlChanged();
  tool_work_ = QtConcurrent::run([this, target, selected, operation, completion, targetsCompletion] {
    DeviceConnectionResult result{DeviceConnectionError::BackendFailure, "Device operation did not complete.", DeviceConnectionEffect::Unknown};
    DeviceConnectionTargets discovered;
    try {
      if (operation == Lifecycle::Discover) {
        const auto inventory = coordinator_->discover();
        if (!inventory.completed) throw std::runtime_error("Discovery did not complete; previous inventory is preserved.");
        { std::lock_guard lock(mutex_); devices_ = inventory.devices; }
        discovered = copiedTargets(inventory.devices);
        emit inventoryChanged();
      } else if (operation == Lifecycle::Connect) target->openDevice(coordinator_, selected);
      else if (operation == Lifecycle::Disconnect) target->closeDevice();
      else if (operation == Lifecycle::Single) {
        const auto before = target->converted_frames_.load();
        target->grabOne();
        if (target->converted_frames_.load() != before + 1) throw std::runtime_error("Acquired frame was not converted to a supported resource.");
      } else if (operation == Lifecycle::LiveStart) target->start();
      else target->stopLive();
      if (operation != Lifecycle::Disconnect && operation != Lifecycle::Discover && operation != Lifecycle::Connect)
        try { target->refreshControlSnapshot(); } catch (const std::exception& error) { qWarning() << "[GenICam] feature refresh failed:" << error.what(); }
      result = {{}, {}, DeviceConnectionEffect::Applied, connectionState()};
    } catch (const std::exception& error) {
      // Discovery keeps the previous inventory, so only device commands leave an uncertain effect.
      result = {DeviceConnectionError::BackendFailure, error.what(),
          operation == Lifecycle::Discover ? DeviceConnectionEffect::None : DeviceConnectionEffect::Unknown, connectionState()};
      discovered.result = {DeviceConnectionError::BackendFailure, error.what()};
    } catch (...) {
      result = {DeviceConnectionError::BackendFailure, "Unexpected device backend failure.", DeviceConnectionEffect::Unknown, connectionState()};
      discovered.result = {DeviceConnectionError::BackendFailure, "Unexpected device backend failure."};
    }
    target->external_busy_.store(false);
    external_busy_.store(false);
    emit target->controlChanged();
    emit controlChanged();
    try {
      if (operation == Lifecycle::Discover) targetsCompletion(std::move(discovered));
      else completion(std::move(result));
    } catch (...) { qWarning() << "[GenICam] lifecycle consumer callback threw."; }
  });
  return {};
}
