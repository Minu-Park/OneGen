#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
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

struct DataStreamInfo {
  std::uint32_t index = 0;
  std::string id;
};

struct Frame {
  /** Raw GenTL buffer facts retained for transport diagnostics. */
  struct BufferInfo {
    std::size_t capacity = 0;
    std::size_t size_filled = 0;
    std::size_t data_size = 0;
    std::size_t image_offset = 0;
    std::size_t y_padding = 0;
    std::uint32_t delivered_image_height = 0;
    bool incomplete = false;
    bool image_present = false;
    bool contains_chunk_data = false;
  };

  struct Scan3dGeometry {
    std::array<double, 3> scale{1.0, 1.0, 1.0};
    std::array<double, 3> offset{0.0, 0.0, 0.0};
    std::array<double, 3> invalid_value{};
    std::array<bool, 3> invalid_enabled{};
    std::string distance_unit;
  };
  struct Part {
    std::vector<std::uint8_t> payload;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
    std::uint64_t data_type = 0;
    std::uint64_t data_format = 0;
    std::uint64_t format_namespace = 0;
    std::uint64_t source_id = 0;
    std::uint64_t region_id = 0;
    std::uint64_t purpose_id = 0;
  };

  std::vector<std::uint8_t> payload;
  std::vector<Part> parts;
  BufferInfo buffer_info;
  std::optional<Scan3dGeometry> scan3d_geometry;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t stride = 0;
  std::uint64_t payload_type = 0;
  std::uint64_t pixel_format = 0;
  std::uint64_t format_namespace = 0;
  std::uint32_t pixel_endianness = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t timestamp = 0;
  std::uint64_t timestamp_ns = 0;
  std::uint32_t stream_index = 0;
  std::string stream_id;
};

}  // namespace onegen
