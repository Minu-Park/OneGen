#pragma once

#include "AbstractSourceController.h"
#include "engine/GraphicsFrameAdapter.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <map>
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

  /** Returns a stable camera controller whose frames share the consumer's source topology. */
  OneGenSourceController* cameraController(const std::string& id, const std::string& label);
  /** Removes a child after its camera panel has drained acquisition and callbacks. */
  void removeCameraController(const std::string& id);
  /** Resets presentation sources after all child panels have drained. */
  void clearCameraControllers();
  /** Reports one stable presentation source per child camera. */
  std::vector<GraphicsSourceDescriptor> sourceDescriptors() const override;
  void setLease(onegen::worker::Lease* lease);
  void start() override;
  void stop() override;
  /** @brief Captures one frame, optionally retaining raw bytes in a consumer diagnostic directory. */
  void grabOne(const QString& diagnosticDirectory = {});
  void trigger();
  bool isGrabbing() const override;
  void setFrameConsumer(FrameConsumer consumer) override;
  bool supports3D() const override { return true; }

private:
  void deliver(const onegen::Frame& source, const std::string& sourceName);
  void bindLease();
  mutable std::mutex mutex_;
  struct Child { unsigned int index; std::string label; std::unique_ptr<OneGenSourceController> controller; };
  std::map<std::string, Child> children_;
  unsigned int next_source_index_ = 0;
  onegen::worker::Lease* lease_ = nullptr;
  std::unique_ptr<GraphicsFrameCallbackGate> callback_gate_;
  FrameConsumer consumer_;
  std::atomic<bool> grabbing_{false};
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  std::atomic<std::uint64_t> incomplete_frames_{0};
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> converted_frames_{0};
  std::atomic<std::uint64_t> partial_frames_{0};
};
