#pragma once

#include "OneGenWorkerClient.h"
#include "OneGenNode.h"
#include "OneGenParameterMessages.h"
#include "OneGenTypes.h"

#include <chrono>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace onegen::worker {

struct DiscoveryResult {
  std::vector<DeviceInfo> devices;
  std::vector<InterfaceInfo> interfaces;
  std::vector<std::string> failures;
  bool completed = false;
  std::size_t finished_producers = 0;
  std::size_t total_producers = 0;
};

class Coordinator;

class Lease {
public:
  using FrameFn = std::function<void(const Frame&)>;
  using ErrorFn = std::function<void(std::string_view)>;
  using InvalidateFn = std::function<void(int, std::string_view)>;

  Lease(std::shared_ptr<Connection> connection, DeviceInfo device, std::string identity,
        Coordinator* owner, std::uint32_t session = 0);
  ~Lease();
  /** @brief Closes this device channel while copied consumer references remain safe. */
  void close();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  const DeviceInfo& info() const { return device_; }
  std::vector<DataStreamInfo> data_streams() const;
  void select_data_stream(std::uint32_t index);
  std::vector<std::pair<std::string, FeatureNode>> describe_features() const;
  FeatureNode read_node(int module, std::string_view name) const;
  /** @brief Observes a scalar feature and complete selectors on the owning worker. */
  NodeObservation observe_node(int module, std::string_view name) const;
  /** @brief Revalidates and mutates within one worker request, with post-preparation readback. */
  NodeWriteResult write_node_checked(int module, std::string_view name, std::string_view operation,
      std::string_view value, const NodeObservation& expected, bool compareValue);
  FeatureNode write_node(int module, std::string_view name, std::string_view op,
                         std::string_view value);
  void grab_one(const FrameFn& deliver);
  void live();
  void trigger();
  void stop();
  void set_frame_callback(FrameFn cb);
  void set_error_callback(ErrorFn cb);
  void set_invalidate_callback(InvalidateFn cb);
  /** True while the isolated worker process is still accepting requests. */
  bool alive() const;
  /** Advances GenApi polling and drains supported producer events. */
  void poll_features(std::uint32_t elapsed_ms);
  /** Reads an explicitly requested register from its owning module. */
  std::vector<std::uint8_t> read_register(int module, std::string_view name);
  /** Writes a register through the worker's access and length checks. */
  void write_register(int module, std::string_view name, const std::vector<std::uint8_t>& bytes);
  /** Exports a selector-aware GenApi feature bag for one module. */
  std::string save_features(int module);
  /** Restores and verifies one module's GenApi feature bag while Live is stopped. */
  void load_features(int module, std::string_view contents);

private:
  Connection& openConnection() const;
  std::shared_ptr<Connection> connection_;
  DeviceInfo device_;
  std::string identity_;
  Coordinator* owner_ = nullptr;
  std::uint32_t session_ = 0;
  std::atomic<bool> closed_{false};
};

class Coordinator {
public:
  /** Receives cumulative snapshots on completion or after five seconds without a completion. */
  using ProgressFn = std::function<void(const DiscoveryResult&)>;
  /**
   * @param probe_timeout Total initial-discovery budget. GenTL updates run in
   *        one-second slices so the first camera can complete startup early.
   */
  Coordinator(std::filesystem::path worker_executable,
              std::filesystem::path cti_root = {},
              std::chrono::milliseconds probe_timeout = std::chrono::milliseconds{10000},
              Connection::ClaimFn claim = {});
  ~Coordinator();

  /** Probes producers in isolation and merges only identical scoped Device identities. */
  /** Report cumulative results on the calling thread as each producer finishes. */
  DiscoveryResult discover(ProgressFn progress = {});
  /** Pins one producer-scoped Interface without requiring a device or vendor adapter. */
  void open_interface(const InterfaceInfo& info);
  /** Releases one producer-scoped Interface after its Device leases are closed. */
  void close_interface(const InterfaceInfo& info);
  /** Rescans the selected Interface while preserving existing Device sessions. */
  std::vector<DeviceInfo> rescan_interface(const InterfaceInfo& info);
  /** Lists interfaces without requiring a camera lease. */
  std::vector<InterfaceInfo> interfaces();
  /** Reads an interface's complete feature tree with no device open. */
  FeatureNode interface_features(const InterfaceInfo& info);
  /** Reads a selected interface feature. */
  FeatureNode read_interface(const InterfaceInfo& info, std::string_view name);
  /** Writes while the selected Interface has no active or opening Device leases. */
  FeatureNode write_interface(const InterfaceInfo& info, std::string_view name,
                              std::string_view operation, std::string_view value);
  /** Deduplicates only identical producer/interface/device identities. */
  static std::vector<DeviceInfo> merge(std::vector<DeviceInfo> devices);
  std::unique_ptr<Lease> open(const DeviceInfo& device);
  void shutdown();
  /** @brief Releases retained workers only when no discovery, Interface or Device is active. */
  bool reset_idle_search();
  /** @brief Sets exact consumer-owned CTI files only after idle workers are released; empty disables discovery. */
  bool set_producer_files(std::vector<std::filesystem::path> files);

private:
  friend class Lease;
  std::shared_ptr<Connection> connection(const std::string& cti, const std::string& device = {});
  /** Retires a failed discovery worker only when no Interface or Device is using or opening it. */
  void retire_failed_discovery(const std::string& cti, Connection* expected, bool preserve_inactive = true);
  /** Retains successfully closed workers for rediscovery-free reconnects. */
  void release(const std::string& identity, const std::string& cti, bool reusable,
               Connection* dying = nullptr, std::uint32_t session = 0);

  std::filesystem::path worker_executable_;
  std::filesystem::path cti_root_;
  std::optional<std::vector<std::filesystem::path>> producer_files_;
  std::chrono::milliseconds probe_timeout_;
  Connection::ClaimFn claim_;
  std::unordered_map<std::string, InterfaceInfo> interface_sessions_;
  std::uint32_t next_session_ = 1;
  std::unordered_map<std::string, std::uint32_t> session_channels_;
  std::unordered_map<std::string, std::vector<InterfaceInfo>> interface_inventory_;
  std::mutex mutex_;
  std::mutex discovery_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections_;
  std::unordered_map<std::string, std::shared_ptr<Connection>> device_connections_;
  std::unordered_set<std::string> leased_;
  std::unordered_map<std::string, std::string> leased_interfaces_;
};

} // namespace onegen::worker
