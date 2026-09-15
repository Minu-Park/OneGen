#include "OneGenSourceController.h"

#include "OneGenGraphicsFrameAdapter.h"
#include "SessionFrame.h"
#include "onegen/camera.hpp"

#include <QDebug>
#include <exception>

OneGenSourceController::OneGenSourceController(onegen::Camera* camera, QObject* parent)
    : AbstractSourceController(parent), camera_(camera)
{
  if (camera_) {
    camera_->set_frame_callback([this](const onegen::Frame& source) { deliver(source); });
    camera_->set_error_callback([this](const std::string_view error) {
      emit acquisitionError(QString::fromLocal8Bit(error.data(), static_cast<qsizetype>(error.size())));
    });
  }
}

OneGenSourceController::~OneGenSourceController()
{
  stop();
  if (camera_) {
    camera_->set_frame_callback({});
    camera_->set_error_callback({});
  }
}

void OneGenSourceController::start()
{
  if (!camera_ || !camera_->is_open()) return;
  camera_->live();
  grabbing_ = true;
}

void OneGenSourceController::stop()
{
  if (camera_) camera_->stop();
  grabbing_ = false;
}

bool OneGenSourceController::isGrabbing() const { return grabbing_; }

void OneGenSourceController::grabOne()
{
  if (!camera_ || !camera_->is_open()) return;
  for (const auto& frame : camera_->single(1)) deliver(frame);
}

void OneGenSourceController::trigger()
{
  if (camera_) camera_->trigger();
}

void OneGenSourceController::deliver(const onegen::Frame& source)
{
  try {
    const auto sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
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
        emit acquisitionError(message);
      }
      return;
    }
    if (!consumer_) return;
    if (camera_) {
      const auto& info = camera_->info();
      payload->metadata.sourceName = info.display_name.empty() ? info.id : info.display_name;
    }
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
