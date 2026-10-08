#pragma once

#include "OneGenCoordinator.h"
#include <atomic>
#include <cstdint>
#include <string>

#include <condition_variable>
#include <future>
#include <utility>

namespace onegen::worker {

/** Owns one scan across the startup wait and subsequent control-panel updates. */
class StartupDiscovery {
public:
  /** @brief Module-owned scan outcomes; observing them never starts or cancels work. */
  enum class Phase { Waiting, Running, Succeeded, Partial, Failed, Stopping, Stopped };
  /** @brief Value-only completion diagnostics, independent of a consumer SDK. */
  struct Status { Phase phase = Phase::Waiting; std::uint64_t scan = 0; std::string detail; };
  /** @brief Serialized diagnostic callback; must not throw or reenter this scan. */
  using StateFn = std::function<void(const Status&, const Status&)>;
  /** @brief Starts one background scan and optionally reports lifecycle transitions. */
  template<class Scan>
  explicit StartupDiscovery(Scan scan, StateFn state_changed = {})
      : state_changed_(std::move(state_changed))
  {
    transition(Phase::Running, "scan started");
    task_ = std::async(std::launch::async, [this, scan = std::move(scan)] {
          try {
            auto devices = scan([this](const DiscoveryResult& update) {
              { std::lock_guard lock(mutex_); latest_ = update; ++revision_; }
              changed_.notify_all();
            });
            { std::lock_guard lock(mutex_);
              latest_.devices = devices;
              latest_.completed = true;
              std::string failures;
              for (const auto& failure : latest_.failures) { if (!failures.empty()) failures += "\n"; failures += failure; }
              const bool all_failed = devices.empty() && latest_.total_producers > 0
                  && latest_.failures.size() == latest_.total_producers;
              transition(all_failed ? Phase::Failed : latest_.failures.empty()
                  ? Phase::Succeeded : Phase::Partial, std::move(failures));
              ++revision_;
            }
            changed_.notify_all();
            return devices;
          } catch (...) {
            std::string detail = "Unknown discovery exception.";
            try { throw; } catch (const std::exception& exception) {
              detail = exception.what();
            } catch (...) {}
            { std::lock_guard lock(mutex_); transition(Phase::Failed, std::move(detail)); ++revision_; }
            changed_.notify_all();
            throw;
          }
        }).share();
  }

  ~StartupDiscovery() {
    { std::lock_guard lock(mutex_); transition(Phase::Stopping, "scan released"); }
    if (task_.valid()) task_.wait();
    { std::lock_guard lock(mutex_); transition(Phase::Stopped); }
  }

  /** @brief Copies the scan phase and failure detail without waiting for completion. */
  Status status() const {
    std::lock_guard lock(mutex_);
    return status_;
  }

  /** Returns the first available snapshot, waiting at most ten seconds for a camera. */
  std::vector<DeviceInfo> waitForCamera(
      std::chrono::milliseconds timeout = std::chrono::seconds{10}) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [this] { return !latest_.devices.empty() || finishedLocked(); });
    if (finishedLocked()) { lock.unlock(); return task_.get(); }
    return latest_.devices;
  }

  /** @brief Reports unfinished startup work so reconfiguration can reject it without blocking. */
  bool running() const {
    std::lock_guard lock(mutex_);
    return !finishedLocked();
  }

  /** Delivers remaining scan updates on the caller thread without starting another scan. */
  std::vector<DeviceInfo> finish(Coordinator::ProgressFn progress) {
    std::size_t revision = 0;
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [&] { return revision_ != revision || finishedLocked(); });
      revision = revision_;
      const auto update = latest_;
      const bool finished = finishedLocked();
      lock.unlock();
      if (progress) progress(update);
      if (finished) return task_.get();
      lock.lock();
    }
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  DiscoveryResult latest_;
  std::size_t revision_ = 0;
  inline static std::atomic<std::uint64_t> next_scan_{0};
  Status status_{Phase::Waiting, ++next_scan_, {}};
  StateFn state_changed_;

  bool finishedLocked() const { return status_.phase != Phase::Running; }
  void transition(Phase phase, std::string detail = {}) {
    // This is the scan transition table. Shutdown owns the join, so a late
    // result is rejected once stopping begins.
    const auto current = status_.phase;
    bool valid = false;
    switch (phase) {
    case Phase::Waiting: break;
    case Phase::Running: valid = current == Phase::Waiting; break;
    case Phase::Succeeded:
    case Phase::Partial:
    case Phase::Failed: valid = current == Phase::Running; break;
    case Phase::Stopping: valid = current != Phase::Stopping && current != Phase::Stopped; break;
    case Phase::Stopped: valid = current == Phase::Stopping; break;
    }
    if (!valid) return;
    const auto before = status_;
    status_.phase = phase;
    status_.detail = std::move(detail);
    if (state_changed_) state_changed_(before, status_);
  }
  std::shared_future<std::vector<DeviceInfo>> task_;
};

} // namespace onegen::worker
