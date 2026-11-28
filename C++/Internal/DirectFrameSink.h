#pragma once

#include "OneGenTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace onegen::detail {

inline void copy_scan3d_geometry(std::optional<Frame::Scan3dGeometry>& dst,
                                 const std::optional<Frame::Scan3dGeometry>& src) {
  if (!src) {
    dst.reset();
    return;
  }
  if (!dst) {
    dst = *src;
    return;
  }
  dst->scale = src->scale;
  dst->offset = src->offset;
  dst->invalid_value = src->invalid_value;
  dst->invalid_enabled = src->invalid_enabled;
  if (dst->distance_unit != src->distance_unit) dst->distance_unit = src->distance_unit;
  if (dst->output_mode != src->output_mode) dst->output_mode = src->output_mode;
  if (dst->coordinate_system != src->coordinate_system) {
    dst->coordinate_system = src->coordinate_system;
  }
  dst->calibrated_axes = src->calibrated_axes;
  dst->focal_length = src->focal_length;
  dst->baseline = src->baseline;
  dst->principal_u = src->principal_u;
  dst->principal_v = src->principal_v;
}

inline void copy_components(std::vector<Frame::ComponentIdentity>& dst,
                            const std::vector<Frame::ComponentIdentity>& src) {
  if (dst.size() == src.size()) {
    bool equal = true;
    for (std::size_t index = 0; index < src.size(); ++index) {
      if (dst[index].id != src[index].id || dst[index].name != src[index].name) {
        equal = false;
        break;
      }
    }
    if (equal) return;
  }
  dst.resize(src.size());
  for (std::size_t index = 0; index < src.size(); ++index) {
    dst[index].id = src[index].id;
    if (dst[index].name != src[index].name) dst[index].name = src[index].name;
  }
}

inline void reset_reusable_frame(Frame& dst) noexcept {
  dst.payload.clear();
  dst.parts.clear();
  dst.chunks.clear();
  dst.buffer_info.diagnostic.clear();
  std::string saved_diagnostic = std::move(dst.buffer_info.diagnostic);
  dst.buffer_info = {};
  dst.buffer_info.diagnostic = std::move(saved_diagnostic);
  dst.width = 0;
  dst.height = 0;
  dst.stride = 0;
  dst.payload_type = 0;
  dst.pixel_format = 0;
  dst.format_namespace = 0;
  dst.pixel_endianness = 0;
  dst.frame_id = 0;
  dst.timestamp = 0;
  dst.timestamp_ns = 0;
  dst.stream_index = 0;
}

inline void copy_frame_metadata_reusing_capacity(Frame& dst, const Frame& src) {
  dst.payload.clear();
  dst.parts.clear();
  dst.chunks.clear();
  dst.buffer_info.capacity = src.buffer_info.capacity;
  dst.buffer_info.size_filled = src.buffer_info.size_filled;
  dst.buffer_info.data_size = src.buffer_info.data_size;
  dst.buffer_info.image_offset = src.buffer_info.image_offset;
  dst.buffer_info.y_padding = src.buffer_info.y_padding;
  dst.buffer_info.delivered_image_height = src.buffer_info.delivered_image_height;
  dst.buffer_info.incomplete = src.buffer_info.incomplete;
  dst.buffer_info.image_present = src.buffer_info.image_present;
  dst.buffer_info.contains_chunk_data = src.buffer_info.contains_chunk_data;
  dst.buffer_info.repaired = src.buffer_info.repaired;
  if (dst.buffer_info.diagnostic != src.buffer_info.diagnostic) {
    dst.buffer_info.diagnostic = src.buffer_info.diagnostic;
  }
  copy_scan3d_geometry(dst.scan3d_geometry, src.scan3d_geometry);
  copy_components(dst.components, src.components);
  dst.width = src.width;
  dst.height = src.height;
  dst.stride = src.stride;
  dst.payload_type = src.payload_type;
  dst.pixel_format = src.pixel_format;
  dst.format_namespace = src.format_namespace;
  dst.pixel_endianness = src.pixel_endianness;
  dst.frame_id = src.frame_id;
  dst.timestamp = src.timestamp;
  dst.timestamp_ns = src.timestamp_ns;
  dst.stream_index = src.stream_index;
  if (dst.stream_id != src.stream_id) dst.stream_id = src.stream_id;
}

enum class FallbackReason : std::uint8_t {
  None,
  UnknownLayout,
  HostUnsupported,
  LayoutConstraint,
  PackedRowAlignment,
  GeometryUnrepresentable,
  Planar,
  MultiPart,
  ChunkData,
  NonImagePayload,
  Jpeg,
  Incomplete,
  MetadataTooLarge,
  /** The transport extents disagree with the declared layout; the owned path fits the bytes. */
  Malformed,
};

struct DirectReservation {
  std::span<std::uint8_t> destination;
  std::uint64_t ticket = 0;
  /** Keeps the owning DirectLeaseSink alive until publish/cancel completes. */
  std::shared_ptr<void> owner;
};

enum class ReserveStatus : std::uint8_t { Reserved, NoPool, Stopped, Busy, Refused };

enum class CancelCause : std::uint8_t {
  Timeout,
  Fallback,
  Capacity,
  Malformed,
  Exception,
  Stop,
  Detached,
};

/** Worker-owned destination/lease-sink operations; all calls are thread-affine to the live loop. */
class DirectFrameSink {
public:
  virtual ~DirectFrameSink() = default;

  virtual ReserveStatus reserve(DirectReservation& reservation) = 0;
  virtual void publish(const DirectReservation& reservation, const Frame& metadata,
                       std::size_t used_bytes) = 0;
  virtual void cancel(const DirectReservation& reservation, CancelCause cause) noexcept = 0;
  /** Records one Busy drop: no free lease slot or no storage from the consumer. */
  virtual void note_slot_exhausted() noexcept {}
  virtual void note_fallback(FallbackReason reason, const Frame& frame) noexcept = 0;
  virtual FallbackReason metadata_gate(const Frame& metadata) const noexcept = 0;
  virtual void report_capacity_failure(const Frame& metadata, std::size_t required,
                                       std::size_t available) noexcept = 0;
  virtual void wake() noexcept = 0;
};

}  // namespace onegen::detail
