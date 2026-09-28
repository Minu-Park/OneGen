#pragma once

#include "OneGenWorkerProtocol.h"
#include "OneGenNode.h"
#include "OneGenTypes.h"

#include <limits>
#include <utility>

namespace onegen::worker {

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
  write(out, value.buffer_info);
  out.scalar(value.scan3d_geometry.has_value());
  if (value.scan3d_geometry) {
    for (const auto item : value.scan3d_geometry->scale) out.scalar(item);
    for (const auto item : value.scan3d_geometry->offset) out.scalar(item);
    for (const auto item : value.scan3d_geometry->invalid_value) out.scalar(item);
    for (const auto item : value.scan3d_geometry->invalid_enabled) out.scalar(item);
    out.string(value.scan3d_geometry->distance_unit);
    out.string(value.scan3d_geometry->output_mode);
    out.string(value.scan3d_geometry->coordinate_system);
    for (const auto item : value.scan3d_geometry->calibrated_axes) out.scalar(item);
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

} // namespace onegen::worker
