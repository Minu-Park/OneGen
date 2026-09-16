#pragma once

#include "OneGenWorkerClient.h"
#include "OneGenNode.h"
#include "OneGenTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace onegen::worker {

struct DiscoveryResult {
  std::vector<DeviceInfo> devices;
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
        Coordinator* owner);
  ~Lease();
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

  const DeviceInfo& info() const { return device_; }
  std::vector<DataStreamInfo> data_streams() const;
  void select_data_stream(std::uint32_t index);
  std::vector<std::pair<std::string, FeatureNode>> describe_features() const;
  FeatureNode read_node(int module, std::string_view name) const;
  FeatureNode write_node(int module, std::string_view name, std::string_view op,
                         std::string_view value);
  void grab_one(const FrameFn& deliver);
  void live();
  void trigger();
  void stop();
  void set_frame_callback(FrameFn cb);
  void set_error_callback(ErrorFn cb);
  void set_invalidate_callback(InvalidateFn cb);

private:
  std::shared_ptr<Connection> connection_;
  DeviceInfo device_;
  std::string identity_;
  Coordinator* owner_ = nullptr;
};

class Coordinator {
public:
  using ProgressFn = std::function<void(const DiscoveryResult&)>;
  /**
   * @param probe_timeout Wait passed to GenTL TLUpdateInterfaceList and
   *        IFUpdateDeviceList. The host does not kill the worker on a second deadline.
   */
  Coordinator(std::filesystem::path worker_executable,
              std::filesystem::path cti_root = {},
              std::chrono::milliseconds probe_timeout = std::chrono::milliseconds{10000});
  ~Coordinator();

  /** Probe GenTL producers concurrently in isolation and merge duplicate physical devices. */
  /** Report cumulative results on the calling thread as each producer finishes. */
  DiscoveryResult discover(ProgressFn progress = {});
  /** Merge records by transport plus serial, IP, or stable GenTL device ID. */
  static std::vector<DeviceInfo> merge(std::vector<DeviceInfo> devices);
  std::unique_ptr<Lease> open(const DeviceInfo& device);
  void shutdown();

private:
  friend class Lease;
  std::shared_ptr<Connection> connection(const std::string& cti);
  void release(const std::string& identity, const std::string& cti);

  std::filesystem::path worker_executable_;
  std::filesystem::path cti_root_;
  std::chrono::milliseconds probe_timeout_;
  std::mutex mutex_;
  std::mutex discovery_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Connection>> connections_;
  std::unordered_set<std::string> leased_;
};

} // namespace onegen::worker
