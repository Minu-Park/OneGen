#pragma once

#include "AbstractSourceController.h"

#include <atomic>
#include <cstdint>

namespace onegen { class Camera; struct Frame; }

/** Owns OneGen live acquisition and delivers SessionFrame values synchronously. */
class OneGenSourceController final : public AbstractSourceController {
  Q_OBJECT
public:
  explicit OneGenSourceController(onegen::Camera* camera, QObject* parent = nullptr);
  ~OneGenSourceController() override;

  void start() override;
  void stop() override;
  void grabOne();
  void trigger();
  bool isGrabbing() const override;
  void setFrameConsumer(FrameConsumer consumer) override;
  bool supports3D() const override { return true; }

signals:
  void acquisitionError(const QString& message);

private:
  void deliver(const onegen::Frame& source);
  onegen::Camera* camera_ = nullptr;
  FrameConsumer consumer_;
  std::atomic<bool> grabbing_{false};
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
};
