#include "OneGenSourceController.h"

#include "OneGenCoordinator.h"
#include "OneGenGraphicsFrameAdapter.h"
#include "SessionFrame.h"

#include <QDebug>
#include <QString>
#include <exception>

OneGenSourceController::OneGenSourceController(QObject* parent)
    : AbstractSourceController(parent) {}

OneGenSourceController::~OneGenSourceController()
{
  stop();
  setLease(nullptr);
  for (auto& [id, child] : children_) child.controller->setLease(nullptr);
}

OneGenSourceController* OneGenSourceController::cameraController(const std::string& id, const std::string& label)
{
  OneGenSourceController* result;
  {
    std::lock_guard lock(mutex_);
    if (const auto found = children_.find(id); found != children_.end()) return found->second.controller.get();
    const auto index = next_source_index_++;
    auto child = std::make_unique<OneGenSourceController>();
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
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) child->start();
  onegen::worker::Lease* lease = nullptr;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
  }
  if (!lease) return;
  lease->live();
  grabbing_.store(true, std::memory_order_relaxed);
}

void OneGenSourceController::stop()
{
  std::vector<OneGenSourceController*> children;
  { std::lock_guard lock(mutex_); for (const auto& [id, child] : children_) children.push_back(child.controller.get()); }
  for (auto* child : children) child->stop();
  onegen::worker::Lease* lease = nullptr;
  {
    std::lock_guard lock(mutex_);
    lease = lease_;
    grabbing_.store(false, std::memory_order_relaxed);
  }
  if (!lease) return;
  try {
    lease->stop();
  } catch (const std::exception& error) {
    qWarning().noquote() << QStringLiteral("[GenICam] live stop failed: %1")
                               .arg(QString::fromLocal8Bit(error.what()));
  } catch (...) {
    qWarning() << "[GenICam] live stop failed with an unknown exception.";
  }
}

bool OneGenSourceController::isGrabbing() const {
  std::lock_guard lock(mutex_);
  for (const auto& [id, child] : children_) if (child.controller->isGrabbing()) return true;
  return grabbing_;
}

void OneGenSourceController::grabOne()
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
  lease->grab_one(
      [this, sourceName](const onegen::Frame& source) { deliver(source, sourceName); });
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
      for (const auto& part : source.parts)
        qInfo().noquote() << QStringLiteral("[GenICam] first frame part: type=%1 PFNC=0x%2 %3x%4 stride=%5 bytes=%6 source=%7 region=%8")
            .arg(part.data_type).arg(QString::number(part.data_format, 16)).arg(part.width).arg(part.height)
            .arg(part.stride).arg(part.payload.size()).arg(part.source_id).arg(part.region_id);
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
