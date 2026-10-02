#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
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
  std::vector<std::string> cti_candidates;
  /** Producer-specific IDs aligned with cti_candidates. */
  std::vector<std::string> cti_candidate_ids;
  /** Producer-specific InterfaceIDs aligned with cti_candidates. */
  std::vector<std::string> cti_candidate_interfaces;
  std::string interface_id;
};

/** Producer-reported interface identity, independent of device presence. */
struct InterfaceInfo {
  std::string id;
  std::string cti_path;
  std::string display_name;
  std::string vendor;
  std::string tl_type;
};

struct DataStreamInfo {
  std::uint32_t index = 0;
  std::string id;
};

struct Frame {
  /** Owned raw chunk bytes; offset refers to the original transport buffer. */
  struct Chunk {
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    std::vector<std::uint8_t> payload;
  };
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
    std::string diagnostic;
  };

  struct Scan3dGeometry {
    std::array<double, 3> scale{1.0, 1.0, 1.0};
    std::array<double, 3> offset{0.0, 0.0, 0.0};
    std::array<double, 3> invalid_value{};
    std::array<bool, 3> invalid_enabled{};
    std::string distance_unit;
    /** Explicit SFNC interpretation; empty strings retain legacy producer behavior. */
    std::string output_mode;
    std::string coordinate_system;
    /** Calibration availability per A/B/C axis, independent of default numeric values. */
    std::array<bool, 3> calibrated_axes{true, true, true};
    /** Disparity reconstruction; NaN when the corresponding node is absent. */
    double focal_length = std::numeric_limits<double>::quiet_NaN();
    double baseline = std::numeric_limits<double>::quiet_NaN();
    double principal_u = std::numeric_limits<double>::quiet_NaN();
    double principal_v = std::numeric_limits<double>::quiet_NaN();
  };
  /** SFNC component identity copied before acquisition; duplicate IDs remain ambiguous. */
  struct ComponentIdentity { std::uint64_t id = 0; std::string name; };
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
  std::vector<ComponentIdentity> components;
  std::vector<Chunk> chunks;
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
