#pragma once

#include "gentl.hpp"

#include "onegen/onegen.hpp"

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
};

void close_interfaces(Producer& producer);

struct FoundDevice {
  Producer* producer = nullptr;
  std::string if_id;
  DeviceInfo info;
};

std::vector<FoundDevice> scan_devices(std::vector<std::unique_ptr<Producer>>& producers,
                                      uint64_t timeout_ms);

std::string gentl_string(Gentl& g, GenTL::IF_HANDLE hif, const char* devid,
                         GenTL::DEVICE_INFO_CMD cmd);

struct OneGen::Impl {
  std::recursive_mutex mu;
  std::vector<std::unique_ptr<Producer>> producers;
  std::vector<std::unique_ptr<Camera>> cameras;
  std::vector<Camera*> camera_view;
  std::set<std::string> open_keys;
};

}  // namespace onegen
