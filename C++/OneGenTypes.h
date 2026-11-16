#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace onegen {

/** @brief A pre-effect claim refusal; the caller may retry after the owner releases it. */
class DeviceClaimRefused final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

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

/** @brief Declares how one GenTL Interface is claimed across sessions. */
enum class InterfaceClaimScope {
  /** A frame grabber board: one exclusive claim per Interface, inherited by its cameras. */
  Interface,
  /** A network or USB Interface: one claim per camera; the Interface itself is shared. */
  Camera,
  /** An unlabelled Interface: treated as exclusive and reported with its reason. */
  Unknown,
};

/** @brief Derives the claim scope from the producer's InterfaceType label.
 *  @note Frame grabbers (CoaXPress, CameraLink) are exclusive boards; GigE Vision and
 *        USB3 Vision claim per camera when one is opened. Any other label is unknown and
 *        therefore exclusive.
 */
inline InterfaceClaimScope interfaceClaimScope(std::string_view type) {
  std::string label;
  label.reserve(type.size());
  for (const char value : type) {
    if (value >= 'a' && value <= 'z') label.push_back(static_cast<char>(value - 'a' + 'A'));
    else if ((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9'))
      label.push_back(value);
  }
  const auto contains = [&label](std::string_view token) {
    return label.find(token) != std::string::npos;
  };
  if (contains("COAXPRESS") || contains("CXP") || label == "CL" || contains("CAMERALINK"))
    return InterfaceClaimScope::Interface;
  if (contains("GIGE") || contains("GEV") || contains("USB3") || contains("U3V"))
    return InterfaceClaimScope::Camera;
  return InterfaceClaimScope::Unknown;
}

/** @brief Reports one short reason for the claim scope decision. */
inline std::string interfaceClaimReason(InterfaceClaimScope scope, std::string_view type) {
  switch (scope) {
    case InterfaceClaimScope::Interface:
      return "The frame grabber Interface is one exclusive board; its cameras inherit the claim.";
    case InterfaceClaimScope::Camera:
      return "Network and USB Interfaces claim per camera; the Interface itself is shared.";
    case InterfaceClaimScope::Unknown:
      return "The Interface type is unknown (" + std::string(type) + "); it is treated as exclusive.";
  }
  return {};
}

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
