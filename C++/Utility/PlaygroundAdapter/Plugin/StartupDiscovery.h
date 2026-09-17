#pragma once

#include "OneGenCoordinator.h"

#include <condition_variable>
#include <future>
#include <utility>

namespace onegen::worker {

/** Owns one scan across the startup wait and subsequent control-panel updates. */
class StartupDiscovery {
public:
  template<class Scan>
  explicit StartupDiscovery(Scan scan)
      : task_(std::async(std::launch::async, [this, scan = std::move(scan)] {
          try {
            auto devices = scan([this](const DiscoveryResult& update) {
              { std::lock_guard lock(mutex_); latest_ = update; ++revision_; }
              changed_.notify_all();
            });
            { std::lock_guard lock(mutex_); latest_.devices = devices; finished_ = true; ++revision_; }
            changed_.notify_all();
            return devices;
          } catch (...) {
            { std::lock_guard lock(mutex_); finished_ = true; ++revision_; }
            changed_.notify_all();
            throw;
          }
        })) {}

  ~StartupDiscovery() { if (task_.valid()) task_.wait(); }

  /** Returns the first available snapshot, waiting at most ten seconds for a camera. */
  std::vector<DeviceInfo> waitForCamera(
      std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return !latest_.devices.empty(); });
    return latest_.devices;
  }

  /** Delivers remaining scan updates on the caller thread without starting another scan. */
  std::vector<DeviceInfo> finish(Coordinator::ProgressFn progress) {
    std::size_t revision = 0;
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [&] { return revision_ != revision || finished_; });
      revision = revision_;
      const auto update = latest_;
      const bool finished = finished_;
      lock.unlock();
      if (progress) progress(update);
      if (finished) return task_.get();
      lock.lock();
    }
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  DiscoveryResult latest_;
  std::size_t revision_ = 0;
  bool finished_ = false;
  std::future<std::vector<DeviceInfo>> task_;
};

} // namespace onegen::worker
