#pragma once

#include "OneGenWorkerProtocol.h"
#include "OneGenNode.h"
#include "OneGenTypes.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace onegen::worker {

/** @brief Decode bounds applied before any FrameReady field allocation. */
struct MetadataLimits {
  std::uint32_t components = maximum_metadata_components;
  std::uint32_t string_bytes = maximum_metadata_string_bytes;
  std::uint32_t diagnostic_bytes = maximum_metadata_diagnostic_bytes;
  std::uint32_t body_bytes = maximum_frame_ready_size;
};

/** @brief Write view of one v16 FrameReady body; metadata never carries pixels. */
struct FrameReadyRecord {
  SlotIdentity identity;
  const Frame* metadata = nullptr;
};

/** @brief Decoded v16 FrameReady body with an owned metadata copy. */
struct DecodedFrameReady {
  SlotIdentity identity;
  Frame metadata;
};

inline void writeStrings(Writer& out, const std::vector<std::string>& values)
{
  out.scalar(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) out.string(value);
}

inline std::vector<std::string> readStrings(Reader& in)
{
  const auto count = in.scalar<std::uint32_t>();
  std::vector<std::string> values;
  values.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) values.push_back(in.string());
  return values;
}

inline void write(Writer& out, const DeviceInfo& value)
{
  out.string(value.id);
  out.string(value.vendor);
  out.string(value.model);
  out.string(value.serial);
  out.string(value.user_defined_name);
  out.string(value.display_name);
  out.string(value.ip);
  out.string(value.tl_type);
  out.string(value.cti_path);
  writeStrings(out, value.cti_candidates);
  writeStrings(out, value.cti_candidate_ids);
  writeStrings(out, value.cti_candidate_interfaces);
  out.string(value.interface_id);
}

inline DeviceInfo readDeviceInfo(Reader& in)
{
  DeviceInfo value;
  value.id = in.string();
  value.vendor = in.string();
  value.model = in.string();
  value.serial = in.string();
  value.user_defined_name = in.string();
  value.display_name = in.string();
  value.ip = in.string();
  value.tl_type = in.string();
  value.cti_path = in.string();
  value.cti_candidates = readStrings(in);
  value.cti_candidate_ids = readStrings(in);
  value.cti_candidate_interfaces = readStrings(in);
  value.interface_id = in.string();
  return value;
}

inline void write(Writer& out, const InterfaceInfo& value) {
  out.string(value.id);
  out.string(value.cti_path);
  out.string(value.display_name);
  out.string(value.vendor);
  out.string(value.tl_type);
}

inline InterfaceInfo readInterfaceInfo(Reader& in) {
  return {in.string(), in.string(), in.string(), in.string(), in.string()};
}

inline void write(Writer& out, const DataStreamInfo& value)
{
  out.scalar(value.index);
  out.string(value.id);
}

inline DataStreamInfo readDataStreamInfo(Reader& in)
{
  return {in.scalar<std::uint32_t>(), in.string()};
}

inline void write(Writer& out, const FeatureNode& value)
{
  out.string(value.name);
  out.string(value.display_name);
  out.string(value.description);
  out.string(value.tooltip);
  out.scalar(value.kind);
  out.scalar(value.readable);
  out.scalar(value.writable);
  out.scalar(value.visible);
  out.scalar(value.int_value);
  out.scalar(value.int_min);
  out.scalar(value.int_max);
  out.scalar(value.int_inc);
  out.scalar(value.float_value);
  out.scalar(value.float_min);
  out.scalar(value.float_max);
  out.scalar(value.bool_value);
  out.string(value.string_value);
  out.string(value.enum_value);
  writeStrings(out, value.enum_entries);
  out.scalar(static_cast<std::uint32_t>(value.children.size()));
  for (const auto& child : value.children) write(out, child);
  out.string(value.unit);
  out.string(value.representation);
  out.scalar(value.float_inc);
  out.scalar(value.register_length);
  out.scalar(static_cast<std::uint32_t>(value.selecting_features.size()));
  for (const auto& selector : value.selecting_features) write(out, selector);
  out.string(value.observation_error);
}

inline FeatureNode readFeatureNode(Reader& in)
{
  FeatureNode value;
  value.name = in.string();
  value.display_name = in.string();
  value.description = in.string();
  value.tooltip = in.string();
  value.kind = in.scalar<NodeKind>();
  value.readable = in.scalar<bool>();
  value.writable = in.scalar<bool>();
  value.visible = in.scalar<bool>();
  value.int_value = in.scalar<std::int64_t>();
  value.int_min = in.scalar<std::int64_t>();
  value.int_max = in.scalar<std::int64_t>();
  value.int_inc = in.scalar<std::int64_t>();
  value.float_value = in.scalar<double>();
  value.float_min = in.scalar<double>();
  value.float_max = in.scalar<double>();
  value.bool_value = in.scalar<bool>();
  value.string_value = in.string();
  value.enum_value = in.string();
  value.enum_entries = readStrings(in);
  const auto count = in.scalar<std::uint32_t>();
  value.children.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index)
    value.children.push_back(readFeatureNode(in));
  value.unit = in.string();
  value.representation = in.string();
  value.float_inc = in.scalar<double>();
  value.register_length = in.scalar<std::uint64_t>();
  const auto selectors = in.scalar<std::uint32_t>();
  for (std::uint32_t index = 0; index < selectors; ++index)
    value.selecting_features.push_back(readFeatureNode(in));
  value.observation_error = in.string();
  return value;
}

inline void writeBytes(Writer& out, const std::vector<std::uint8_t>& bytes)
{
  if (bytes.size() > (std::numeric_limits<std::uint32_t>::max)())
    throw std::length_error("OneGen frame payload is too large.");
  out.scalar(static_cast<std::uint32_t>(bytes.size()));
  out.bytes(bytes.data(), bytes.size());
}

inline std::vector<std::uint8_t> readBytes(Reader& in)
{
  const auto size = in.scalar<std::uint32_t>();
  std::vector<std::uint8_t> bytes(size);
  in.read(bytes.data(), bytes.size());
  return bytes;
}

inline void write(Writer& out, const Frame::BufferInfo& value)
{
  out.scalar(static_cast<std::uint64_t>(value.capacity));
  out.scalar(static_cast<std::uint64_t>(value.size_filled));
  out.scalar(static_cast<std::uint64_t>(value.data_size));
  out.scalar(static_cast<std::uint64_t>(value.image_offset));
  out.scalar(static_cast<std::uint64_t>(value.y_padding));
  out.scalar(value.delivered_image_height);
  out.scalar(value.incomplete);
  out.scalar(value.image_present);
  out.scalar(value.contains_chunk_data);
  out.string(value.diagnostic);
}

inline Frame::BufferInfo readBufferInfo(Reader& in)
{
  Frame::BufferInfo value;
  value.capacity = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.size_filled = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.data_size = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.image_offset = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.y_padding = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.delivered_image_height = in.scalar<std::uint32_t>();
  value.incomplete = in.scalar<bool>();
  value.image_present = in.scalar<bool>();
  value.contains_chunk_data = in.scalar<bool>();
  value.diagnostic = in.string();
  return value;
}

inline void write(Writer& out, const Frame::Part& value)
{
  writeBytes(out, value.payload);
  out.scalar(value.width);
  out.scalar(value.height);
  out.scalar(static_cast<std::uint64_t>(value.stride));
  out.scalar(value.data_type);
  out.scalar(value.data_format);
  out.scalar(value.format_namespace);
  out.scalar(value.source_id);
  out.scalar(value.region_id);
  out.scalar(value.purpose_id);
}

inline Frame::Part readFramePart(Reader& in)
{
  Frame::Part value;
  value.payload = readBytes(in);
  value.width = in.scalar<std::uint32_t>();
  value.height = in.scalar<std::uint32_t>();
  value.stride = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.data_type = in.scalar<std::uint64_t>();
  value.data_format = in.scalar<std::uint64_t>();
  value.format_namespace = in.scalar<std::uint64_t>();
  value.source_id = in.scalar<std::uint64_t>();
  value.region_id = in.scalar<std::uint64_t>();
  value.purpose_id = in.scalar<std::uint64_t>();
  return value;
}

inline void writeFrameMetadataTail(Writer& out, const Frame& value)
{
  out.scalar(static_cast<std::uint32_t>(value.components.size()));
  for (const auto& component : value.components) {
    out.scalar(component.id);
    out.string(component.name);
  }
  write(out, value.buffer_info);
  out.scalar(static_cast<std::uint8_t>(value.scan3d_geometry.has_value()));
  if (value.scan3d_geometry) {
    for (const auto item : value.scan3d_geometry->scale) out.scalar(item);
    for (const auto item : value.scan3d_geometry->offset) out.scalar(item);
    for (const auto item : value.scan3d_geometry->invalid_value) out.scalar(item);
    for (const auto item : value.scan3d_geometry->invalid_enabled)
      out.scalar(static_cast<std::uint8_t>(item));
    out.string(value.scan3d_geometry->distance_unit);
    out.string(value.scan3d_geometry->output_mode);
    out.string(value.scan3d_geometry->coordinate_system);
    for (const auto item : value.scan3d_geometry->calibrated_axes)
      out.scalar(static_cast<std::uint8_t>(item));
    out.scalar(value.scan3d_geometry->focal_length);
    out.scalar(value.scan3d_geometry->baseline);
    out.scalar(value.scan3d_geometry->principal_u);
    out.scalar(value.scan3d_geometry->principal_v);
  }
  out.scalar(value.width);
  out.scalar(value.height);
  out.scalar(static_cast<std::uint64_t>(value.stride));
  out.scalar(value.payload_type);
  out.scalar(value.pixel_format);
  out.scalar(value.format_namespace);
  out.scalar(value.pixel_endianness);
  out.scalar(value.frame_id);
  out.scalar(value.timestamp);
  out.scalar(value.timestamp_ns);
  out.scalar(value.stream_index);
  out.string(value.stream_id);
}

inline void write(Writer& out, const Frame& value)
{
  writeBytes(out, value.payload);
  out.scalar(static_cast<std::uint32_t>(value.parts.size()));
  for (const auto& part : value.parts) write(out, part);
  out.scalar(static_cast<std::uint32_t>(value.chunks.size()));
  for (const auto& chunk : value.chunks) {
    out.scalar(chunk.id);
    out.scalar(chunk.offset);
    writeBytes(out, chunk.payload);
  }
  writeFrameMetadataTail(out, value);
}

inline Frame readFrame(Reader& in)
{
  Frame value;
  value.payload = readBytes(in);
  const auto part_count = in.scalar<std::uint32_t>();
  value.parts.reserve(part_count);
  for (std::uint32_t index = 0; index < part_count; ++index)
    value.parts.push_back(readFramePart(in));
  const auto chunk_count = in.scalar<std::uint32_t>();
  for (std::uint32_t index = 0; index < chunk_count; ++index) {
    Frame::Chunk chunk;
    chunk.id = in.scalar<std::uint64_t>();
    chunk.offset = in.scalar<std::uint64_t>();
    chunk.payload = readBytes(in);
    value.chunks.push_back(std::move(chunk));
  }
  const auto componentCount = in.scalar<std::uint32_t>();
  for (std::uint32_t index = 0; index < componentCount; ++index) {
    Frame::ComponentIdentity component;
    component.id = in.scalar<std::uint64_t>();
    component.name = in.string();
    value.components.push_back(std::move(component));
  }
  value.buffer_info = readBufferInfo(in);
  if (in.scalar<bool>()) {
    Frame::Scan3dGeometry geometry;
    for (auto& item : geometry.scale) item = in.scalar<double>();
    for (auto& item : geometry.offset) item = in.scalar<double>();
    for (auto& item : geometry.invalid_value) item = in.scalar<double>();
    for (auto& item : geometry.invalid_enabled) item = in.scalar<bool>();
    geometry.distance_unit = in.string();
    geometry.output_mode = in.string();
    geometry.coordinate_system = in.string();
    for (auto& item : geometry.calibrated_axes) item = in.scalar<bool>();
    geometry.focal_length = in.scalar<double>();
    geometry.baseline = in.scalar<double>();
    geometry.principal_u = in.scalar<double>();
    geometry.principal_v = in.scalar<double>();
    value.scan3d_geometry = std::move(geometry);
  }
  value.width = in.scalar<std::uint32_t>();
  value.height = in.scalar<std::uint32_t>();
  value.stride = static_cast<std::size_t>(in.scalar<std::uint64_t>());
  value.payload_type = in.scalar<std::uint64_t>();
  value.pixel_format = in.scalar<std::uint64_t>();
  value.format_namespace = in.scalar<std::uint64_t>();
  value.pixel_endianness = in.scalar<std::uint32_t>();
  value.frame_id = in.scalar<std::uint64_t>();
  value.timestamp = in.scalar<std::uint64_t>();
  value.timestamp_ns = in.scalar<std::uint64_t>();
  value.stream_index = in.scalar<std::uint32_t>();
  value.stream_id = in.string();
  return value;
}

/** @brief Exact v16 FrameReady body size; saturates on overflow for a noexcept gate. */
inline std::size_t encodedFrameReadySize(const Frame& value) noexcept
{
  constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
  std::size_t total = 48;
  const auto add = [&total, maximum](std::size_t bytes) noexcept {
    if (total == maximum) return;
    if (bytes > maximum - total) total = maximum;
    else total += bytes;
  };
  add(4);
  for (const auto& component : value.components) {
    add(8);
    add(4);
    add(component.name.size());
  }
  add(40 + 4 + 3 + 4);
  add(value.buffer_info.diagnostic.size());
  add(1);
  if (value.scan3d_geometry) {
    add(72 + 3 + 12 + 3 + 32);
    add(value.scan3d_geometry->distance_unit.size());
    add(value.scan3d_geometry->output_mode.size());
    add(value.scan3d_geometry->coordinate_system.size());
  }
  add(72);
  add(4);
  add(value.stream_id.size());
  return total;
}

/** @brief Field and body bounds checked by the worker gate and the bounded decoder. */
inline bool frameMetadataWithinLimits(const Frame& value,
                                      const MetadataLimits& limits = {}) noexcept
{
  if (value.components.size() > limits.components) return false;
  for (const auto& component : value.components)
    if (component.name.size() > limits.string_bytes) return false;
  if (value.buffer_info.diagnostic.size() > limits.diagnostic_bytes) return false;
  if (value.stream_id.size() > limits.string_bytes) return false;
  if (value.scan3d_geometry) {
    if (value.scan3d_geometry->distance_unit.size() > limits.string_bytes) return false;
    if (value.scan3d_geometry->output_mode.size() > limits.string_bytes) return false;
    if (value.scan3d_geometry->coordinate_system.size() > limits.string_bytes) return false;
  }
  return encodedFrameReadySize(value) <= limits.body_bytes;
}

namespace detail {

inline std::size_t readSizeValue(Reader& in)
{
  const auto value = in.scalar<std::uint64_t>();
  if (value > (std::numeric_limits<std::size_t>::max)())
    throw std::runtime_error("OneGen frame size exceeds the host size range.");
  return static_cast<std::size_t>(value);
}

} // namespace detail

/** @brief Bounded metadata-tail reader shared by FrameReady; owned readFrame stays unbounded. */
inline Frame readFrameMetadataTail(Reader& in, const MetadataLimits& limits = {})
{
  Frame value;
  const auto component_count = in.scalar<std::uint32_t>();
  if (component_count > limits.components)
    throw std::runtime_error("OneGen FrameReady component count exceeds its limit.");
  value.components.reserve(component_count);
  for (std::uint32_t index = 0; index < component_count; ++index) {
    Frame::ComponentIdentity component;
    component.id = in.scalar<std::uint64_t>();
    component.name = in.string(limits.string_bytes);
    value.components.push_back(std::move(component));
  }
  value.buffer_info.capacity = detail::readSizeValue(in);
  value.buffer_info.size_filled = detail::readSizeValue(in);
  value.buffer_info.data_size = detail::readSizeValue(in);
  value.buffer_info.image_offset = detail::readSizeValue(in);
  value.buffer_info.y_padding = detail::readSizeValue(in);
  value.buffer_info.delivered_image_height = in.scalar<std::uint32_t>();
  value.buffer_info.incomplete = in.boolean();
  value.buffer_info.image_present = in.boolean();
  value.buffer_info.contains_chunk_data = in.boolean();
  value.buffer_info.diagnostic = in.string(limits.diagnostic_bytes);
  if (in.boolean()) {
    Frame::Scan3dGeometry geometry;
    for (auto& item : geometry.scale) item = in.scalar<double>();
    for (auto& item : geometry.offset) item = in.scalar<double>();
    for (auto& item : geometry.invalid_value) item = in.scalar<double>();
    for (auto& item : geometry.invalid_enabled) item = in.boolean();
    geometry.distance_unit = in.string(limits.string_bytes);
    geometry.output_mode = in.string(limits.string_bytes);
    geometry.coordinate_system = in.string(limits.string_bytes);
    for (auto& item : geometry.calibrated_axes) item = in.boolean();
    geometry.focal_length = in.scalar<double>();
    geometry.baseline = in.scalar<double>();
    geometry.principal_u = in.scalar<double>();
    geometry.principal_v = in.scalar<double>();
    value.scan3d_geometry = std::move(geometry);
  }
  value.width = in.scalar<std::uint32_t>();
  value.height = in.scalar<std::uint32_t>();
  value.stride = detail::readSizeValue(in);
  value.payload_type = in.scalar<std::uint64_t>();
  value.pixel_format = in.scalar<std::uint64_t>();
  value.format_namespace = in.scalar<std::uint64_t>();
  value.pixel_endianness = in.scalar<std::uint32_t>();
  value.frame_id = in.scalar<std::uint64_t>();
  value.timestamp = in.scalar<std::uint64_t>();
  value.timestamp_ns = in.scalar<std::uint64_t>();
  value.stream_index = in.scalar<std::uint32_t>();
  value.stream_id = in.string(limits.string_bytes);
  return value;
}

/** @brief Encodes a complete v16 FrameReady body through the owned metadata tail writer. */
inline void write(Writer& out, const FrameReadyRecord& value)
{
  if (!value.metadata) throw std::invalid_argument("FrameReady metadata is missing.");
  if (!frameMetadataWithinLimits(*value.metadata))
    throw std::length_error("OneGen FrameReady metadata exceeds its limits.");
  write(out, value.identity);
  writeFrameMetadataTail(out, *value.metadata);
}

/** @brief Decodes a v16 FrameReady body after bounds checks and rejects trailing bytes. */
inline DecodedFrameReady readFrameReady(Reader& in, const MetadataLimits& limits = {})
{
  if (in.remainingSize() > limits.body_bytes)
    throw std::runtime_error("OneGen FrameReady body exceeds its limit.");
  DecodedFrameReady value;
  value.identity = readSlotIdentity(in);
  value.metadata = readFrameMetadataTail(in, limits);
  if (!in.empty()) throw std::runtime_error("OneGen FrameReady body has trailing bytes.");
  return value;
}

/** @brief Derives the legacy host descriptor from the v16 identity and metadata. */
inline FrameDescriptor describeFrame(const SlotIdentity& identity, const Frame& metadata)
{
  FrameDescriptor value;
  value.session = identity.session;
  value.epoch = identity.epoch;
  value.token = identity.token;
  value.slot = identity.slot;
  value.generation = identity.generation;
  value.frame_id = metadata.frame_id;
  value.sequence = identity.sequence;
  value.used_bytes = identity.used_bytes;
  value.width = metadata.width;
  value.height = metadata.height;
  value.stride = metadata.stride;
  value.pixel_format = metadata.pixel_format;
  value.format_namespace = metadata.format_namespace;
  value.pixel_endianness = metadata.pixel_endianness;
  value.payload_type = metadata.payload_type;
  value.stream_index = metadata.stream_index;
  value.stream_id = metadata.stream_id;
  return value;
}

} // namespace onegen::worker
