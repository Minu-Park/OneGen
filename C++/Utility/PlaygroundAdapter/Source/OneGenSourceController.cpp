#include "OneGenSourceController.h"
#include "../../Qt/SingleCaptureDiagnostics.h"

#include "OneGenCoordinator.h"
#include "OneGenGraphicsFrameAdapter.h"
#include "SessionFrame.h"

#include <QDebug>
#include <QString>
#include <QStringList>
#include <functional>
#include <exception>
#include <QtConcurrent>
#include <QJsonArray>
#include <QThread>
#include <QUuid>

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

void OneGenSourceController::configureControl(std::shared_ptr<onegen::worker::Coordinator> coordinator, std::vector<onegen::DeviceInfo> devices) {
  coordinator_ = std::move(coordinator);
  setDevices(std::move(devices));
}
void OneGenSourceController::setDevices(std::vector<onegen::DeviceInfo> devices) {
  Q_ASSERT(QThread::currentThread() == thread());
  std::lock_guard lock(mutex_);
  devices_ = std::move(devices);
}
std::shared_ptr<onegen::worker::Lease> OneGenSourceController::deviceLease() const {
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
std::shared_ptr<onegen::worker::Lease> OneGenSourceController::openDevice(
    std::shared_ptr<onegen::worker::Coordinator> coordinator, const onegen::DeviceInfo& device) {
  if (deviceLease()) throw std::runtime_error("Disconnect the current device before connecting.");
  auto opened = std::shared_ptr<onegen::worker::Lease>(coordinator->open(device).release());
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
    child->setFrameConsumer([this, index](SessionFrame&& frame, unsigned int) {
      FrameConsumer consumer;
      { std::lock_guard lock(mutex_); consumer = consumer_; }
      if (consumer) consumer(std::move(frame), index);
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

void OneGenSourceController::setLease(onegen::worker::Lease* lease)
{
  std::unique_lock lock(mutex_);
  if (lease_ == lease) return;
  auto old_gate = std::move(callback_gate_);
  if (old_gate) old_gate->beginShutdown();
  if (lease_) {
    lease_->set_frame_callback({});
    lease_->set_error_callback({});
  }
  lock.unlock();
  if (old_gate) old_gate->waitForDrain();
  lock.lock();
  lease_ = lease;
  rejected_frames_.store(0, std::memory_order_relaxed);
  incomplete_frames_.store(0, std::memory_order_relaxed);
  received_frames_.store(0, std::memory_order_relaxed);
  converted_frames_.store(0, std::memory_order_relaxed);
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
  try {
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
        qInfo().noquote() << "[GenICam] component node: camera=" << QString::fromStdString(sourceName)
            << "module=" << QString::fromStdString(module) << "node=" << name
            << "label=" << QString::fromStdString(node.display_name)
            << "readable=" << node.readable << "writable=" << node.writable
            << "value=" << value << "entries=" << entries.join(',')
            << "description=" << QString::fromStdString(node.description);
      }
      for (const auto& child : node.children) inspect(child, module);
    };
    for (const auto& [module, root] : lease_->describe_features()) inspect(root, module);
    qInfo() << "[GenICam] component inventory: nodes=" << componentNodes
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

void OneGenSourceController::start()
{
  if (QThread::currentThread() == thread() && external_busy_.load()) throw std::runtime_error("Device control is busy.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) child->start();
  onegen::worker::Lease* lease = nullptr;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
  }
  if (!lease) return;
  emit diagnosticAcquisitionChanged(true, QStringLiteral("camera"));
  try { lease->live(); }
  catch (...) { emit diagnosticAcquisitionChanged(false, QStringLiteral("camera")); throw; }
  grabbing_.store(true, std::memory_order_relaxed);
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
  onegen::worker::Lease* lease = nullptr;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
  }
  if (!lease) return;
  try {
    lease->stop();
    grabbing_.store(false, std::memory_order_relaxed);
    emit diagnosticAcquisitionChanged(false, QStringLiteral("camera"));
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

void OneGenSourceController::grabOne(const QString& diagnosticDirectory)
{
  onegen::worker::Lease* lease = nullptr;
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
  onegen::worker::Lease* lease = nullptr;
  { std::lock_guard lock(mutex_); lease = lease_; }
  if (lease) lease->trigger();
}

void OneGenSourceController::deliver(const onegen::Frame& source, const std::string& sourceName)
{
  try {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    const bool firstFrame = received_frames_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (firstFrame) {
      qInfo().noquote() << QStringLiteral("[GenICam] first frame received: camera=%1 frame=%2 payloadType=%3 PFNC=0x%4 %5x%6 bytes=%7 parts=%8 scan3d=%9")
          .arg(QString::fromStdString(sourceName)).arg(source.frame_id).arg(source.payload_type)
          .arg(QString::number(source.pixel_format, 16)).arg(source.width).arg(source.height)
          .arg(source.payload.size()).arg(source.parts.size()).arg(source.scan3d_geometry.has_value());
      qInfo() << "[GenICam] first frame chunk count:" << source.chunks.size()
              << "(presence does not establish Scan3d chunk overrides)";
      if (source.scan3d_geometry) {
        const auto& g = *source.scan3d_geometry;
        qInfo().noquote() << "[GenICam] effective Scan3d: mode=" << QString::fromStdString(g.output_mode)
            << "system=" << QString::fromStdString(g.coordinate_system)
            << "unit=" << QString::fromStdString(g.distance_unit);
        for (std::size_t axis = 0; axis < 3; ++axis)
          qInfo() << "[GenICam] effective Scan3d axis:" << axis
              << "calibrated=" << g.calibrated_axes[axis] << "scale=" << g.scale[axis]
              << "offset=" << g.offset[axis] << "invalidEnabled=" << g.invalid_enabled[axis]
              << "invalidValue=" << g.invalid_value[axis];
      }
      std::size_t partIndex = 0;
      for (const auto& part : source.parts)
        qInfo().noquote() << QStringLiteral("[GenICam] first frame part: type=%1 PFNC=0x%2 %3x%4 stride=%5 bytes=%6 source=%7 region=%8 purpose=%9")
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
    std::string rejectionReason;
    auto payload = oneGenGraphicsFrame(source, sequence, &rejectionReason);
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
    if (firstConverted) qInfo().noquote() << QStringLiteral("[GenICam] first frame converted: images=%1 ranges=%2 pointClouds=%3; forwarding to presentation")
        .arg(frame.payload.images.size()).arg(frame.payload.ranges.size()).arg(frame.payload.pointClouds.size());
    consumer(std::move(frame), source.stream_index);
  } catch (const std::exception& error) {
    qWarning() << "[GenICam] frame delivery failed:" << error.what();
  } catch (...) {
    qWarning() << "[GenICam] frame delivery failed with an unknown exception.";
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
QJsonObject schema(QJsonObject properties, QJsonArray required) {
  return {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}};
}
QJsonObject stateSchema() {
  const QJsonObject text{{"type", "string"}}, flag{{"type", "boolean"}};
  return schema({{"connected", flag}, {"live", flag}, {"connectionId", text}, {"targetId", text},
      {"convertedFrames", text}}, {"connected", "live", "connectionId", "targetId", "convertedFrames"});
}
QJsonObject targets(const std::vector<onegen::DeviceInfo>& inventory) {
  QJsonArray values;
  for (const auto& d : inventory) values.append(QJsonObject{{"targetId", targetId(deviceKey(d))},
      {"title", QString::fromStdString(d.display_name.empty() ? d.id : d.display_name)},
      {"deviceId", QString::fromStdString(d.id)}, {"producer", QString::fromStdString(d.cti_path)},
      {"interfaceId", QString::fromStdString(d.interface_id)}});
  return {{"targets", values}};
}
}

QJsonObject OneGenSourceController::controlState() const {
  std::lock_guard lock(mutex_);
  if (owned_lease_) return {{"connected", owned_lease_->alive()}, {"live", grabbing_.load()},
      {"connectionId", connection_id_}, {"targetId", targetId(deviceKey(owned_lease_->info()))},
      {"convertedFrames", QString::number(converted_frames_.load())}};
  for (const auto& [id, child] : children_) {
    const auto state = child.controller->controlState();
    if (state.value("connected").toBool()) return state;
  }
  return {{"connected", false}, {"live", false}, {"connectionId", ""}, {"targetId", ""}, {"convertedFrames", "0"}};
}

QList<ToolDescriptor> OneGenSourceController::toolDescriptors() const {
  const QJsonObject text{{"type", "string"}, {"minLength", 1}};
  const auto empty = schema({}, {});
  const auto connection = schema({{"connectionId", text}}, {"connectionId"});
  const auto entry = schema({{"targetId", text}, {"title", QJsonObject{{"type", "string"}}},
      {"deviceId", text}, {"producer", text}, {"interfaceId", QJsonObject{{"type", "string"}}}},
      {"targetId", "title", "deviceId", "producer", "interfaceId"});
  QList<ToolDescriptor> result{
      {"device.targets", 1, "Device targets", "Copy exact producer/interface/device targets.", ToolEffect::Observe, true, {}, empty,
          schema({{"targets", QJsonObject{{"type", "array"}, {"items", entry}}}}, {"targets"})},
      {"device.connection", 1, "Device connection", "Observe the module-owned connection and converted-frame count, not presentation completion.", ToolEffect::Observe, true, {}, empty, stateSchema()},
      {"device.discover", 1, "Discover devices", "Run existing isolated producer discovery while disconnected and idle.", ToolEffect::Device, true, {}, empty,
          schema({{"targets", QJsonObject{{"type", "array"}, {"items", entry}}}}, {"targets"})},
      {"device.connect", 1, "Connect device", "Open an exact declared Device; implicit switching and Interface mode are refused.", ToolEffect::Device, true, {}, schema({{"targetId", text}}, {"targetId"}), stateSchema()},
      {"device.disconnect", 1, "Disconnect device", "Close the exact connection generation.", ToolEffect::Device, true, {}, connection, stateSchema()},
      {"device.single", 1, "Acquire one image", "Wait for native single acquisition and conversion; query the viewer separately for presentation evidence.", ToolEffect::Pipeline, true, {}, connection, stateSchema()},
      {"device.live-start", 1, "Start Live", "Start existing acquisition on the exact connection; completion is worker acknowledgement.", ToolEffect::Pipeline, true, {}, connection, stateSchema()},
      {"device.live-stop", 1, "Stop Live", "Stop existing acquisition on the exact connection.", ToolEffect::Pipeline, true, {}, connection, stateSchema()}};
  if (!coordinator_) for (auto& descriptor : result) {
    descriptor.available = false; descriptor.unavailableReason = "Device control has no coordinator.";
  }
  return result;
}

ApiResult<void> OneGenSourceController::requestTool(const QString& name, int version, QJsonObject arguments, Completion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  using Result = ApiResult<QJsonValue>;
  if (!completion) return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, "Completion is required.");
  bool known = false;
  for (const auto& descriptor : toolDescriptors()) if (descriptor.name == name) known = true;
  if (!known) return ApiResult<void>::failure(ApiErrorCode::NotFound, "Tool is not declared.");
  if (version != 1) return ApiResult<void>::failure(ApiErrorCode::Unsupported, "Unsupported tool version.");
  if (!coordinator_) return ApiResult<void>::failure(ApiErrorCode::Unavailable, "Device control has no coordinator.");
  const bool observe = name == "device.targets" || name == "device.connection";
  const bool discover = name == "device.discover";
  const QString field = name == "device.connect" ? "targetId" : "connectionId";
  if (((observe || discover) && !arguments.isEmpty()) || (!observe && !discover && (arguments.size() != 1 || !arguments.value(field).isString()
      || arguments.value(field).toString().isEmpty() || arguments.value(field).toString().contains(QChar::Null))))
    return ApiResult<void>::failure(ApiErrorCode::InvalidArgument, "Exact typed arguments are required.");
  const auto deliver = [completion = std::move(completion)](Result result) noexcept {
    try { completion(std::move(result)); } catch (...) { qWarning() << "[GenICam] tool consumer callback threw."; }
  };
  if (observe) {
    if (name == "device.targets") { QJsonObject value; { std::lock_guard lock(mutex_); value = targets(devices_); } deliver(Result::success(value)); }
    else deliver(Result::success(controlState()));
    return ApiResult<void>::success();
  }
  if (external_busy_.load() || (tool_work_.isStarted() && !tool_work_.isFinished()) || control_blocked_ || gui_busy_.load())
    return ApiResult<void>::failure(ApiErrorCode::Busy, "Discovery, Interface or control work is active.");
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) if (child->gui_busy_.load() || child->external_busy_.load())
    return ApiResult<void>::failure(ApiErrorCode::Busy, "A GUI control operation is active.");
  auto state = controlState();
  OneGenSourceController* target = nullptr;
  onegen::DeviceInfo selected;
  if (discover) {
    if (state.value("connected").toBool()) return ApiResult<void>::failure(ApiErrorCode::Busy, "Disconnect before discovery.");
    target = this;
  } else if (name == "device.connect") {
    if (state.value("connected").toBool()) return ApiResult<void>::failure(ApiErrorCode::Conflict, "Disconnect before connecting another device.");
    { std::lock_guard lock(mutex_);
      for (const auto& device : devices_) if (targetId(deviceKey(device)) == arguments.value(field).toString()) selected = device; }
    if (selected.id.empty()) return ApiResult<void>::failure(ApiErrorCode::NotFound, "Exact declared target was not found.");
    // Presentation observers may select/create their view; they do not execute the command.
    emit presentDevice(QString::fromStdString("Device|" + deviceKey(selected)));
    target = cameraController(deviceKey(selected), selected.display_name.empty() ? selected.id : selected.display_name);
  } else {
    if (!state.value("connected").toBool()) return ApiResult<void>::failure(ApiErrorCode::Unavailable, "Device is disconnected.");
    if (state.value("connectionId").toString() != arguments.value(field).toString())
      return ApiResult<void>::failure(ApiErrorCode::Conflict, "Connection generation changed.");
    for (auto* child : children) if (child->controlState().value("connectionId") == state.value("connectionId")) target = child;
    if (!target) return ApiResult<void>::failure(ApiErrorCode::NotFound, "Connection controller ended.");
    if (name == "device.single" && state.value("live").toBool())
      return ApiResult<void>::failure(ApiErrorCode::Busy, "Stop Live explicitly before single acquisition.");
  }
  external_busy_.store(true);
  target->external_busy_.store(true);
  emit target->controlChanged();
  tool_work_ = QtConcurrent::run([this, target, selected, name, deliver] {
    Result result = Result::failure(ApiErrorCode::Internal, "Device operation did not complete.");
    try {
      if (name == "device.discover") {
        const auto inventory = coordinator_->discover();
        if (!inventory.completed) throw std::runtime_error("Discovery did not complete; previous inventory is preserved.");
        { std::lock_guard lock(mutex_); devices_ = inventory.devices; }
        result = Result::success(targets(inventory.devices));
        emit inventoryChanged();
      } else if (name == "device.connect") target->openDevice(coordinator_, selected);
      else if (name == "device.disconnect") target->closeDevice();
      else if (name == "device.single") {
        const auto before = target->converted_frames_.load();
        target->grabOne();
        if (target->converted_frames_.load() != before + 1) throw std::runtime_error("Acquired frame was not converted to a supported resource.");
      } else if (name == "device.live-start") target->start();
      else target->stopLive();
      if (name != "device.disconnect" && name != "device.discover" && name != "device.connect") try { target->refreshControlSnapshot(); } catch (const std::exception& error) { qWarning() << "[GenICam] feature refresh failed:" << error.what(); }
      if (name != "device.discover") result = Result::success(controlState());
    } catch (const std::exception& error) { result = Result::failure(ApiErrorCode::Unavailable, QString::fromLocal8Bit(error.what())); }
    catch (...) { result = Result::failure(ApiErrorCode::Internal, "Unexpected device backend failure."); }
    target->external_busy_.store(false);
    external_busy_.store(false);
    emit target->controlChanged();
    emit controlChanged();
    deliver(std::move(result));
  });
  return ApiResult<void>::success();
}
