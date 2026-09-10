#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace onegen {

struct DeviceInfo {
  std::string id;
  std::string vendor;
  std::string model;
  std::string serial;
  std::string user_defined_name;
  std::string display_name;
  std::string ip;
  std::string tl_type;
  std::string cti_path;
};

struct Frame {
  std::vector<std::uint8_t> payload;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t pixel_format = 0;
  std::uint64_t timestamp = 0;
};

}  // namespace onegen
