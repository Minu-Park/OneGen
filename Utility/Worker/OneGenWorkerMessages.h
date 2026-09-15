#pragma once

#include "OneGenWorkerProtocol.h"
#include "onegen/node.hpp"
#include "onegen/types.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <utility>

namespace onegen::worker {

inline QJsonObject deviceJson(const DeviceInfo& device)
{
  QJsonArray candidates;
  for (const auto& path : device.cti_candidates)
    candidates.append(QString::fromStdString(path));
  return {{QStringLiteral("id"), QString::fromStdString(device.id)},
          {QStringLiteral("vendor"), QString::fromStdString(device.vendor)},
          {QStringLiteral("model"), QString::fromStdString(device.model)},
          {QStringLiteral("serial"), QString::fromStdString(device.serial)},
          {QStringLiteral("userDefinedName"), QString::fromStdString(device.user_defined_name)},
          {QStringLiteral("displayName"), QString::fromStdString(device.display_name)},
          {QStringLiteral("ip"), QString::fromStdString(device.ip)},
          {QStringLiteral("tlType"), QString::fromStdString(device.tl_type)},
          {QStringLiteral("ctiPath"), QString::fromStdString(device.cti_path)},
          {QStringLiteral("ctiCandidates"), candidates}};
}

inline DeviceInfo deviceInfo(const QJsonObject& value)
{
  DeviceInfo device;
  device.id = value.value(QStringLiteral("id")).toString().toStdString();
  device.vendor = value.value(QStringLiteral("vendor")).toString().toStdString();
  device.model = value.value(QStringLiteral("model")).toString().toStdString();
  device.serial = value.value(QStringLiteral("serial")).toString().toStdString();
  device.user_defined_name = value.value(QStringLiteral("userDefinedName")).toString().toStdString();
  device.display_name = value.value(QStringLiteral("displayName")).toString().toStdString();
  device.ip = value.value(QStringLiteral("ip")).toString().toStdString();
  device.tl_type = value.value(QStringLiteral("tlType")).toString().toStdString();
  device.cti_path = value.value(QStringLiteral("ctiPath")).toString().toStdString();
  for (const auto candidate : value.value(QStringLiteral("ctiCandidates")).toArray())
    device.cti_candidates.push_back(candidate.toString().toStdString());
  return device;
}

inline QString kindName(NodeKind kind)
{
  switch (kind) {
    case NodeKind::Category: return QStringLiteral("category");
    case NodeKind::Integer: return QStringLiteral("int");
    case NodeKind::Float: return QStringLiteral("float");
    case NodeKind::Boolean: return QStringLiteral("bool");
    case NodeKind::Enumeration: return QStringLiteral("enum");
    case NodeKind::Command: return QStringLiteral("command");
    case NodeKind::String: return QStringLiteral("string");
    default: return QStringLiteral("other");
  }
}

inline NodeKind kindFromName(const QString& name)
{
  if (name == QLatin1String("category")) return NodeKind::Category;
  if (name == QLatin1String("int")) return NodeKind::Integer;
  if (name == QLatin1String("float")) return NodeKind::Float;
  if (name == QLatin1String("bool")) return NodeKind::Boolean;
  if (name == QLatin1String("enum")) return NodeKind::Enumeration;
  if (name == QLatin1String("command")) return NodeKind::Command;
  if (name == QLatin1String("string")) return NodeKind::String;
  return NodeKind::Other;
}

inline QJsonObject featureJson(const FeatureNode& node)
{
  QJsonArray children;
  for (const auto& child : node.children) children.append(featureJson(child));
  QJsonArray entries;
  for (const auto& entry : node.enum_entries) entries.append(QString::fromStdString(entry));
  return {{QStringLiteral("name"), QString::fromStdString(node.name)},
          {QStringLiteral("displayName"), QString::fromStdString(node.display_name)},
          {QStringLiteral("description"), QString::fromStdString(node.description)},
          {QStringLiteral("tooltip"), QString::fromStdString(node.tooltip)},
          {QStringLiteral("kind"), kindName(node.kind)},
          {QStringLiteral("readable"), node.readable},
          {QStringLiteral("writable"), node.writable},
          {QStringLiteral("visible"), node.visible},
          {QStringLiteral("intValue"), static_cast<qint64>(node.int_value)},
          {QStringLiteral("intMin"), static_cast<qint64>(node.int_min)},
          {QStringLiteral("intMax"), static_cast<qint64>(node.int_max)},
          {QStringLiteral("intInc"), static_cast<qint64>(node.int_inc)},
          {QStringLiteral("floatValue"), node.float_value},
          {QStringLiteral("floatMin"), node.float_min},
          {QStringLiteral("floatMax"), node.float_max},
          {QStringLiteral("boolValue"), node.bool_value},
          {QStringLiteral("stringValue"), QString::fromStdString(node.string_value)},
          {QStringLiteral("enumValue"), QString::fromStdString(node.enum_value)},
          {QStringLiteral("enumEntries"), entries},
          {QStringLiteral("children"), children}};
}

inline FeatureNode featureNode(const QJsonObject& value)
{
  FeatureNode node;
  node.name = value.value(QStringLiteral("name")).toString().toStdString();
  node.display_name = value.value(QStringLiteral("displayName")).toString().toStdString();
  node.description = value.value(QStringLiteral("description")).toString().toStdString();
  node.tooltip = value.value(QStringLiteral("tooltip")).toString().toStdString();
  node.kind = kindFromName(value.value(QStringLiteral("kind")).toString());
  node.readable = value.value(QStringLiteral("readable")).toBool();
  node.writable = value.value(QStringLiteral("writable")).toBool();
  node.visible = value.value(QStringLiteral("visible")).toBool();
  node.int_value = value.value(QStringLiteral("intValue")).toInteger();
  node.int_min = value.value(QStringLiteral("intMin")).toInteger();
  node.int_max = value.value(QStringLiteral("intMax")).toInteger();
  node.int_inc = value.value(QStringLiteral("intInc")).toInteger(1);
  node.float_value = value.value(QStringLiteral("floatValue")).toDouble();
  node.float_min = value.value(QStringLiteral("floatMin")).toDouble();
  node.float_max = value.value(QStringLiteral("floatMax")).toDouble();
  node.bool_value = value.value(QStringLiteral("boolValue")).toBool();
  node.string_value = value.value(QStringLiteral("stringValue")).toString().toStdString();
  node.enum_value = value.value(QStringLiteral("enumValue")).toString().toStdString();
  for (const auto entry : value.value(QStringLiteral("enumEntries")).toArray())
    node.enum_entries.push_back(entry.toString().toStdString());
  for (const auto child : value.value(QStringLiteral("children")).toArray())
    node.children.push_back(featureNode(child.toObject()));
  return node;
}

inline QJsonObject bufferJson(const Frame::BufferInfo& info)
{
  return {{QStringLiteral("capacity"), static_cast<qint64>(info.capacity)},
          {QStringLiteral("sizeFilled"), static_cast<qint64>(info.size_filled)},
          {QStringLiteral("dataSize"), static_cast<qint64>(info.data_size)},
          {QStringLiteral("imageOffset"), static_cast<qint64>(info.image_offset)},
          {QStringLiteral("yPadding"), static_cast<qint64>(info.y_padding)},
          {QStringLiteral("deliveredImageHeight"), static_cast<int>(info.delivered_image_height)},
          {QStringLiteral("incomplete"), info.incomplete},
          {QStringLiteral("imagePresent"), info.image_present},
          {QStringLiteral("containsChunkData"), info.contains_chunk_data},
          {QStringLiteral("diagnostic"), QString::fromStdString(info.diagnostic)}};
}

inline Frame::BufferInfo bufferInfo(const QJsonObject& value)
{
  Frame::BufferInfo info;
  info.capacity = static_cast<std::size_t>(value.value(QStringLiteral("capacity")).toInteger());
  info.size_filled = static_cast<std::size_t>(value.value(QStringLiteral("sizeFilled")).toInteger());
  info.data_size = static_cast<std::size_t>(value.value(QStringLiteral("dataSize")).toInteger());
  info.image_offset = static_cast<std::size_t>(value.value(QStringLiteral("imageOffset")).toInteger());
  info.y_padding = static_cast<std::size_t>(value.value(QStringLiteral("yPadding")).toInteger());
  info.delivered_image_height =
      static_cast<std::uint32_t>(value.value(QStringLiteral("deliveredImageHeight")).toInt());
  info.incomplete = value.value(QStringLiteral("incomplete")).toBool();
  info.image_present = value.value(QStringLiteral("imagePresent")).toBool();
  info.contains_chunk_data = value.value(QStringLiteral("containsChunkData")).toBool();
  info.diagnostic = value.value(QStringLiteral("diagnostic")).toString().toStdString();
  return info;
}

inline std::pair<QJsonObject, QByteArray> frameMessage(const Frame& frame)
{
  QByteArray binary;
  QJsonArray parts;
  if (frame.parts.empty()) {
    binary = QByteArray(reinterpret_cast<const char*>(frame.payload.data()),
                        static_cast<qsizetype>(frame.payload.size()));
  } else {
    for (const auto& part : frame.parts) {
      parts.append(QJsonObject{
          {QStringLiteral("size"), static_cast<qint64>(part.payload.size())},
          {QStringLiteral("width"), static_cast<int>(part.width)},
          {QStringLiteral("height"), static_cast<int>(part.height)},
          {QStringLiteral("stride"), static_cast<qint64>(part.stride)},
          {QStringLiteral("dataType"), QString::number(part.data_type)},
          {QStringLiteral("dataFormat"), QString::number(part.data_format)},
          {QStringLiteral("formatNamespace"), QString::number(part.format_namespace)},
          {QStringLiteral("sourceId"), QString::number(part.source_id)},
          {QStringLiteral("regionId"), QString::number(part.region_id)},
          {QStringLiteral("purposeId"), QString::number(part.purpose_id)}});
      binary.append(reinterpret_cast<const char*>(part.payload.data()),
                    static_cast<qsizetype>(part.payload.size()));
    }
  }
  QJsonObject geometry;
  if (frame.scan3d_geometry) {
    const auto& g = *frame.scan3d_geometry;
    geometry = {{QStringLiteral("scaleX"), g.scale[0]},
                {QStringLiteral("scaleY"), g.scale[1]},
                {QStringLiteral("scaleZ"), g.scale[2]},
                {QStringLiteral("offsetX"), g.offset[0]},
                {QStringLiteral("offsetY"), g.offset[1]},
                {QStringLiteral("offsetZ"), g.offset[2]},
                {QStringLiteral("distanceUnit"), QString::fromStdString(g.distance_unit)}};
  }
  QJsonObject meta{
      {QStringLiteral("operation"), QStringLiteral("frame")},
      {QStringLiteral("width"), static_cast<int>(frame.width)},
      {QStringLiteral("height"), static_cast<int>(frame.height)},
      {QStringLiteral("stride"), static_cast<qint64>(frame.stride)},
      {QStringLiteral("payloadType"), QString::number(frame.payload_type)},
      {QStringLiteral("pixelFormat"), QString::number(frame.pixel_format)},
      {QStringLiteral("formatNamespace"), QString::number(frame.format_namespace)},
      {QStringLiteral("pixelEndianness"), static_cast<int>(frame.pixel_endianness)},
      {QStringLiteral("frameId"), QString::number(frame.frame_id)},
      {QStringLiteral("timestamp"), QString::number(frame.timestamp)},
      {QStringLiteral("timestampNs"), QString::number(frame.timestamp_ns)},
      {QStringLiteral("streamIndex"), static_cast<int>(frame.stream_index)},
      {QStringLiteral("streamId"), QString::fromStdString(frame.stream_id)},
      {QStringLiteral("buffer"), bufferJson(frame.buffer_info)},
      {QStringLiteral("parts"), parts},
      {QStringLiteral("geometry"), geometry}};
  return {meta, binary};
}

inline Frame frameFromMessage(const QJsonObject& meta, const QByteArray& binary)
{
  Frame frame;
  frame.width = static_cast<std::uint32_t>(meta.value(QStringLiteral("width")).toInt());
  frame.height = static_cast<std::uint32_t>(meta.value(QStringLiteral("height")).toInt());
  frame.stride = static_cast<std::size_t>(meta.value(QStringLiteral("stride")).toInteger());
  frame.payload_type = meta.value(QStringLiteral("payloadType")).toString().toULongLong();
  frame.pixel_format = meta.value(QStringLiteral("pixelFormat")).toString().toULongLong();
  frame.format_namespace = meta.value(QStringLiteral("formatNamespace")).toString().toULongLong();
  frame.pixel_endianness =
      static_cast<std::uint32_t>(meta.value(QStringLiteral("pixelEndianness")).toInt());
  frame.frame_id = meta.value(QStringLiteral("frameId")).toString().toULongLong();
  frame.timestamp = meta.value(QStringLiteral("timestamp")).toString().toULongLong();
  frame.timestamp_ns = meta.value(QStringLiteral("timestampNs")).toString().toULongLong();
  frame.stream_index = static_cast<std::uint32_t>(meta.value(QStringLiteral("streamIndex")).toInt());
  frame.stream_id = meta.value(QStringLiteral("streamId")).toString().toStdString();
  frame.buffer_info = bufferInfo(meta.value(QStringLiteral("buffer")).toObject());
  const auto parts = meta.value(QStringLiteral("parts")).toArray();
  int offset = 0;
  if (parts.isEmpty()) {
    frame.payload.assign(binary.begin(), binary.end());
  } else {
    for (const auto part_value : parts) {
      const auto object = part_value.toObject();
      Frame::Part part;
      const auto size = static_cast<int>(object.value(QStringLiteral("size")).toInteger());
      part.payload.assign(binary.begin() + offset, binary.begin() + offset + size);
      offset += size;
      part.width = static_cast<std::uint32_t>(object.value(QStringLiteral("width")).toInt());
      part.height = static_cast<std::uint32_t>(object.value(QStringLiteral("height")).toInt());
      part.stride = static_cast<std::size_t>(object.value(QStringLiteral("stride")).toInteger());
      part.data_type = object.value(QStringLiteral("dataType")).toString().toULongLong();
      part.data_format = object.value(QStringLiteral("dataFormat")).toString().toULongLong();
      part.format_namespace = object.value(QStringLiteral("formatNamespace")).toString().toULongLong();
      part.source_id = object.value(QStringLiteral("sourceId")).toString().toULongLong();
      part.region_id = object.value(QStringLiteral("regionId")).toString().toULongLong();
      part.purpose_id = object.value(QStringLiteral("purposeId")).toString().toULongLong();
      frame.parts.push_back(std::move(part));
    }
  }
  const auto geometry = meta.value(QStringLiteral("geometry")).toObject();
  if (!geometry.isEmpty()) {
    Frame::Scan3dGeometry value;
    value.scale = {geometry.value(QStringLiteral("scaleX")).toDouble(),
                   geometry.value(QStringLiteral("scaleY")).toDouble(),
                   geometry.value(QStringLiteral("scaleZ")).toDouble()};
    value.offset = {geometry.value(QStringLiteral("offsetX")).toDouble(),
                    geometry.value(QStringLiteral("offsetY")).toDouble(),
                    geometry.value(QStringLiteral("offsetZ")).toDouble()};
    value.distance_unit = geometry.value(QStringLiteral("distanceUnit")).toString().toStdString();
    frame.scan3d_geometry = value;
  }
  return frame;
}

} // namespace onegen::worker
