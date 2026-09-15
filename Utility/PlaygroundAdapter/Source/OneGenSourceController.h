#pragma once

#include "AbstractSourceController.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace onegen {
struct Frame;
namespace worker {
class Lease;
}
}

/** Owns OneGen live acquisition and delivers SessionFrame values synchronously. */
class OneGenSourceController final : public AbstractSourceController {
  Q_OBJECT
public:
  explicit OneGenSourceController(QObject* parent = nullptr);
  ~OneGenSourceController() override;

  void setLease(onegen::worker::Lease* lease);
  void start() override;
  void stop() override;
  void grabOne();
  void trigger();
  bool isGrabbing() const override;
  void setFrameConsumer(FrameConsumer consumer) override;
  bool supports3D() const override { return true; }

private:
  void deliver(const onegen::Frame& source, const std::string& sourceName);
  void bindLease();
  onegen::worker::Lease* lease_ = nullptr;
  FrameConsumer consumer_;
  std::atomic<bool> grabbing_{false};
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  std::atomic<std::uint64_t> incomplete_frames_{0};
};
