#pragma once

/**
 * @file ProducerCoordinator.h
 * @brief Loads GenTL producers in the consumer process and leases their devices.
 *
 * Each canonical CTI gets one OneGen system. Control and feature calls are serialized by the producer lock.
 * Blocking Single and Live acquisition run outside that lock so stop can interrupt them. Producer libraries stay mapped until process
 * exit (see Internal/Gentl.cpp).
 */

#include "FramePool.h"
#include "OneGen.h"
#include "OneGenNode.h"
#include "OneGenTypes.h"
#include "ParameterTypes.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace onegen::producer {

struct DiscoveryResult {
  std::vector<DeviceInfo> devices;
  std::vector<InterfaceInfo> interfaces;
  std::vector<std::string> failures;
  /** @brief Completed producer outcomes retained for the terminal scan report. */
  std::vector<std::string> producer_results;
  bool completed = false;
  std::size_t finished_producers = 0;
  std::size_t total_producers = 0;
};

struct Producer;
struct Registry;
struct LeaseCallbacks;

/** @brief One opened device. Closing it returns the camera to its producer. */
class Lease {
public:
  using FrameFn = std::function<void(const Frame&)>;
  using SharedFrameFn = FramePool::SharedFrameFn;
  using ErrorFn = std::function<void(std::string_view)>;
  using InvalidateFn = std::function<void(int, std::string_view)>;

  Lease(std::shared_ptr<Registry> registry, std::shared_ptr<Producer> producer, Camera* camera,
        DeviceInfo device, std::string identity, bool shared_interface);
  ~Lease();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  /** @brief Stops acquisition and closes the device; copied references stay safe. */
  void close();

  const DeviceInfo& info() const { return device_; }
  std::vector<DataStreamInfo> data_streams() const;
  void select_data_stream(std::uint32_t index);
  std::vector<std::pair<std::string, FeatureNode>> describe_features() const;
  FeatureNode read_node(int module, std::string_view name) const;
  /** @brief Observes a scalar feature and complete selectors. */
  NodeObservation observe_node(int module, std::string_view name) const;
  /** @brief Revalidates and mutates under one producer lock, with post-preparation readback. */
  NodeWriteResult write_node_checked(int module, std::string_view name, std::string_view operation,
      std::string_view value, const NodeObservation& expected, bool compareValue);
  FeatureNode write_node(int module, std::string_view name, std::string_view op,
                         std::string_view value);
  /** @brief Acquires one frame; stop() from another thread aborts it. */
  void grab_one(const FrameFn& deliver);
  void live();
  void trigger();
  void stop();
  /** @brief Owned frames: layouts the direct path cannot represent, or no attached pool. */
  void set_frame_callback(FrameFn cb);
  /** @brief Leased frames from the attached pool, delivered on the acquisition thread. */
  void set_shared_frame_callback(SharedFrameFn cb);
  /** @brief Bytes one frame slot must hold for the current stream configuration. */
  std::uint64_t payload_size();
  /** @brief Attaches a slot pool that the next live() uses. */
  bool create_frame_pool(std::uint32_t slot_count, std::uint64_t slot_bytes, std::string& error);
  /** @brief Detaches the pool; published leases stay valid until released. */
  void destroy_frame_pool();
  /** @brief The attached pool, if any; diagnostics read its counters without the producer lock. */
  std::shared_ptr<const FramePool> frame_pool() const;
  void set_error_callback(ErrorFn cb);
  void set_invalidate_callback(InvalidateFn cb);
  /** True until close(). */
  bool alive() const;
  /** Advances GenApi polling and drains supported producer events. */
  void poll_features(std::uint32_t elapsed_ms);
  /** Reads an explicitly requested register from its owning module. */
  std::vector<std::uint8_t> read_register(int module, std::string_view name);
  /** Writes a register with access and length checks while acquisition is stopped. */
  void write_register(int module, std::string_view name, const std::vector<std::uint8_t>& bytes);
  /** Exports a selector-aware GenApi feature bag for one module. */
  std::string save_features(int module);
  /** Restores and verifies one module's GenApi feature bag while Live is stopped. */
  void load_features(int module, std::string_view contents);

private:
  Camera& camera() const;
  std::shared_ptr<Registry> registry_;
  std::shared_ptr<Producer> producer_;
  Camera* camera_ = nullptr;
  DeviceInfo device_;
  std::string identity_;
  bool shared_interface_ = false;
  std::uint64_t stream_generation_ = 1;
  std::shared_ptr<LeaseCallbacks> callbacks_;
  std::shared_ptr<FramePool> pool_;
  std::atomic<bool> closed_{false};
};

class Coordinator {
public:
  /** Receives cumulative snapshots on completion or after five seconds without a completion. */
  using ProgressFn = std::function<void(const DiscoveryResult&)>;

  /**
   * @param cti_root Producer search root; empty uses the GenTL environment path.
   * @param probe_timeout Initial-discovery budget per producer, scanned in one-second slices.
   * @param claim Consumer-owned resource claim taken before any interface is opened.
   */
  explicit Coordinator(std::filesystem::path cti_root = {},
                       std::chrono::milliseconds probe_timeout = std::chrono::milliseconds{10000},
                       OneGen::InterfaceClaimFn claim = {});
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  /** Probes producers in parallel and merges only identical scoped Device identities. */
  DiscoveryResult discover(ProgressFn progress = {});
  /** Pins one producer-scoped Interface without requiring a device. */
  void open_interface(const InterfaceInfo& info);
  /** Releases one producer-scoped Interface after its Device leases are closed. */
  void close_interface(const InterfaceInfo& info);
  /** Rescans the selected Interface while preserving existing Device sessions. */
  std::vector<DeviceInfo> rescan_interface(const InterfaceInfo& info);
  /** Lists interfaces seen by the last discovery. */
  std::vector<InterfaceInfo> interfaces();
  /** Reads an interface's complete feature tree with no device open. */
  FeatureNode interface_features(const InterfaceInfo& info);
  /** Reads a selected interface feature. */
  FeatureNode read_interface(const InterfaceInfo& info, std::string_view name);
  /** Writes while the selected Interface has no active Device leases. */
  FeatureNode write_interface(const InterfaceInfo& info, std::string_view name,
                              std::string_view operation, std::string_view value);
  /** Deduplicates only identical producer/interface/device identities. */
  static std::vector<DeviceInfo> merge(std::vector<DeviceInfo> devices);
  std::unique_ptr<Lease> open(const DeviceInfo& device);
  /** Drops idle producers; active leases keep theirs until they close. */
  void shutdown();
  /** @brief Closes every producer when no discovery, Interface or Device is active. */
  bool reset_idle_search();
  /** @brief Sets exact consumer-owned CTI files while no producer is loaded; empty disables discovery. */
  bool set_producer_files(std::vector<std::filesystem::path> files);

private:
  std::shared_ptr<Producer> producer(const std::string& cti, bool reload_inactive);
  std::shared_ptr<Producer> loaded(const InterfaceInfo& info);

  std::filesystem::path cti_root_;
  std::chrono::milliseconds probe_timeout_;
  OneGen::InterfaceClaimFn claim_;
  std::optional<std::vector<std::filesystem::path>> producer_files_;
  std::shared_ptr<Registry> registry_;
  std::mutex discovery_mutex_;
};

}  // namespace onegen::producer
