#pragma once

#include "OneGenCamera.h"
#include "OneGenTypes.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace onegen {

namespace defaults {
/** Return canonical CTIs recursively found below one directory. */
std::vector<std::filesystem::path> producerCtis(const std::filesystem::path& root);
/** Return canonical CTIs from the architecture-specific GenTL environment path. */
std::vector<std::filesystem::path> producerCtis();
}  // namespace defaults

class OneGen {
public:
  OneGen();
  ~OneGen();
  OneGen(const OneGen&) = delete;
  OneGen& operator=(const OneGen&) = delete;

  /** Load a producer once; repeated calls for the same canonical path are harmless. */
  void loadCti(const std::filesystem::path& cti);
  using InterfaceClaimFn = std::function<std::shared_ptr<void>(const InterfaceInfo&)>;
  /** Installs a consumer-owned resource claim before any interface is opened. */
  void set_interface_claim_callback(InterfaceClaimFn claim);
  /** Lists producer interfaces without opening their hardware. */
  std::vector<InterfaceInfo> interfaces() const;
  /** Loads an interface node map without a device; the reference expires on interface release. */
  NodeMap& interface_features(std::string_view id);
  /** Reads the node map of an explicitly producer-scoped interface. */
  NodeMap& interface_features(const InterfaceInfo& info);
  /** Pins a selected interface until explicit release. */
  void open_interface(std::string_view id);
  /** Pins the interface belonging to the selected producer. */
  void open_interface(const InterfaceInfo& info);
  /** Closes one interface after its cameras are closed. */
  void release_interface(std::string_view id);
  /** Releases only the interface belonging to the selected producer. */
  void release_interface(const InterfaceInfo& info);
  /** Releases temporary discovery handles, preserving pinned and active interfaces. */
  void release_idle_interfaces();
  /** Rescans one interface without opening unrelated interfaces. */
  std::vector<DeviceInfo> enumerate_interface(std::string_view id,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});
  /** Rescans only the selected producer/interface pair. */
  std::vector<DeviceInfo> enumerate_interface(const InterfaceInfo& info,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});
  /** Closes interfaces for explicit reconfiguration; rejects open cameras. */
  void release_interfaces();
  /** Enumerates devices while logging individual Interface failures. */
  std::vector<DeviceInfo> enumerate(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});
  /** Returns surviving devices and reports every failed producer-scoped Interface. */
  std::vector<DeviceInfo> enumerate(std::chrono::milliseconds timeout,
      std::vector<std::string>& failures);

  Camera& addCamera();

private:
  friend class Camera;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
