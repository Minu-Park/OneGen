#include "OneGenSourceController.h"

#include "OneGenCoordinator.h"
#include "OneGenGraphicsFrameAdapter.h"
#include "SessionFrame.h"

#include <QDebug>
#include <exception>

OneGenSourceController::OneGenSourceController(QObject* parent)
    : AbstractSourceController(parent) {}

OneGenSourceController::~OneGenSourceController()
{
  stop();
  setLease(nullptr);
}

void OneGenSourceController::setLease(onegen::worker::Lease* lease)
{
  if (lease_ == lease) return;
  if (lease_) {
    lease_->set_frame_callback({});
    lease_->set_error_callback({});
  }
  lease_ = lease;
  rejected_frames_.store(0, std::memory_order_relaxed);
  incomplete_frames_.store(0, std::memory_order_relaxed);
  bindLease();
}

void OneGenSourceController::bindLease()
{
  if (!lease_) return;
  const auto& info = lease_->info();
  const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
  lease_->set_frame_callback(
      [this, sourceName](const onegen::Frame& source) { deliver(source, sourceName); });
  lease_->set_error_callback([this](const std::string_view error) {
    qWarning().noquote()
        << "OneGen acquisition failed:"
        << QString::fromLocal8Bit(error.data(), static_cast<qsizetype>(error.size()));
  });
}

void OneGenSourceController::start()
{
  if (!lease_) return;
  lease_->live();
  grabbing_ = true;
}

void OneGenSourceController::stop()
{
  if (lease_) lease_->stop();
  grabbing_ = false;
}

bool OneGenSourceController::isGrabbing() const { return grabbing_; }

void OneGenSourceController::grabOne()
{
  if (!lease_) return;
  const auto& info = lease_->info();
  const auto sourceName = info.display_name.empty() ? info.id : info.display_name;
  lease_->grab_one(
      [this, sourceName](const onegen::Frame& source) { deliver(source, sourceName); });
}

void OneGenSourceController::trigger()
{
  if (lease_) lease_->trigger();
}

void OneGenSourceController::deliver(const onegen::Frame& source, const std::string& sourceName)
{
  try {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    if (source.buffer_info.incomplete && incomplete_frames_.fetch_add(1, std::memory_order_relaxed) == 0) {
      const auto diagnostic = source.buffer_info.diagnostic.empty()
          ? QStringLiteral("incomplete=%1 filled=%2 capacity=%3")
                .arg(source.buffer_info.incomplete)
                .arg(source.buffer_info.size_filled)
                .arg(source.buffer_info.capacity)
          : QString::fromStdString(source.buffer_info.diagnostic);
      qWarning().noquote() << "OneGen incomplete frame delivered:" << diagnostic;
    }
    std::string rejectionReason;
    auto payload = oneGenGraphicsFrame(source, sequence, &rejectionReason);
    if (!payload) {
      if (rejected_frames_.fetch_add(1, std::memory_order_relaxed) == 0) {
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
        qWarning().noquote() << "OneGen" << message;
      }
      return;
    }
    if (!consumer_) return;
    payload->metadata.sourceName = sourceName;
    SessionFrame frame;
    frame.payload = std::move(*payload);
    frame.frameSeq = sequence;
    consumer_(std::move(frame), source.stream_index);
  } catch (const std::exception& error) {
    qWarning() << "OneGen frame delivery failed:" << error.what();
  } catch (...) {
    qWarning() << "OneGen frame delivery failed with an unknown exception.";
  }
}

void OneGenSourceController::setFrameConsumer(FrameConsumer consumer)
{
  consumer_ = std::move(consumer);
}
