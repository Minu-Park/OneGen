#pragma once

#include "Internal/Gentl.h"

#include "OneGen.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace onegen {

struct Producer {
  std::filesystem::path path;
  Gentl api;
  std::map<std::string, GenTL::IF_HANDLE> interfaces;
  std::set<std::string> pinned;
  std::map<std::string, unsigned int> active_devices;
  OneGen::InterfaceClaimFn claim;
  std::map<std::string, std::shared_ptr<void>> claims;
  std::map<std::string, std::unique_ptr<NodeMap>> interface_maps;
};

void close_interfaces(Producer& producer);
/** Closes one producer-scoped interface after its devices have released it. */
void close_interface(Producer& producer, const std::string& id);
/** Reuses the producer's existing handle before acquiring a consumer resource claim. */
GenTL::IF_HANDLE ensure_interface(Producer& producer, const std::string& id);
/** Reads standard producer and interface metadata without opening hardware. */
InterfaceInfo interface_info(Producer& producer, const std::string& id);

struct FoundDevice {
  Producer* producer = nullptr;
  std::string if_id;
  DeviceInfo info;
};

/** Scans scoped Interfaces, retaining surviving Devices and optional failure details. */
std::vector<FoundDevice> scan_devices(std::vector<std::unique_ptr<Producer>>& producers,
    uint64_t timeout_ms, std::string_view selected_interface = {}, std::string_view selected_producer = {},
    std::vector<std::string>* failures = nullptr);

std::string gentl_string(Gentl& g, GenTL::IF_HANDLE hif, const char* devid,
                         GenTL::DEVICE_INFO_CMD cmd);

struct OneGen::Impl {
  OneGen::InterfaceClaimFn claim;
  std::recursive_mutex mu;
  std::vector<std::unique_ptr<Producer>> producers;
  std::vector<std::unique_ptr<Camera>> cameras;
  // Discovery retains the device/interface association for direct opening.
  std::vector<FoundDevice> discovered;
  std::set<std::string> open_keys;
};

}  // namespace onegen
