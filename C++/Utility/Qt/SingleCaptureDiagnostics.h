#pragma once

#include "OneGenCoordinator.h"
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace onegen::diagnostics {
/** @brief Serializes copied feature metadata without losing integer precision. */
inline QJsonObject nodeRecord(const FeatureNode& n) {
  QJsonArray entries, children, selectors;
  for (const auto& e : n.enum_entries) entries.append(QString::fromStdString(e));
  for (const auto& c : n.children) children.append(nodeRecord(c));
  for (const auto& selector : n.selecting_features) selectors.append(nodeRecord(selector));
  return {{"name", QString::fromStdString(n.name)}, {"label", QString::fromStdString(n.display_name)},
    {"description", QString::fromStdString(n.description)}, {"tooltip", QString::fromStdString(n.tooltip)},
    {"kind", static_cast<int>(n.kind)}, {"readable", n.readable}, {"writable", n.writable},
    {"unit", QString::fromStdString(n.unit)}, {"representation", QString::fromStdString(n.representation)},
    {"integer", QString::number(n.int_value)}, {"integerMin", QString::number(n.int_min)},
    {"integerMax", QString::number(n.int_max)}, {"float", n.float_value}, {"floatMin", n.float_min},
    {"floatMax", n.float_max}, {"boolean", n.bool_value}, {"string", QString::fromStdString(n.string_value)},
    {"enum", QString::fromStdString(n.enum_value)}, {"entries", entries}, {"children", children},
    {"selectingFeatures", selectors}, {"observationError", QString::fromStdString(n.observation_error)}};
}
/** @brief Writes a complete artifact or fails before capture rather than silently truncating it. */
inline void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
    throw std::runtime_error(("Diagnostic artifact write failed: " + path).toStdString());
}
/** @brief Captures copied feature trees and selected axes/components, restoring every selector. */
inline QString beforeSingle(worker::Lease& lease) {
  const auto root = qEnvironmentVariable("ONEGEN_SINGLE_DIAGNOSTICS_DIR");
  if (root.isEmpty()) return {};
  const auto directory = QDir(root).filePath(QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz")
      + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(directory)) throw std::runtime_error("Cannot create Single diagnostic directory");
  const auto& device = lease.info();
  write(QDir(directory).filePath("camera.json"), QJsonDocument(QJsonObject{
    {"id", QString::fromStdString(device.id)}, {"model", QString::fromStdString(device.model)},
    {"serial", QString::fromStdString(device.serial)}, {"cti", QString::fromStdString(device.cti_path)},
    {"snapshotUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}).toJson());
  QJsonArray modules;
  for (const auto& [name, tree] : lease.describe_features())
    modules.append(QJsonObject{{"module", QString::fromStdString(name)}, {"tree", nodeRecord(tree)}});
  write(QDir(directory).filePath("features-before.json"), QJsonDocument(modules).toJson());
  qInfo().noquote() << "[SingleDiagnostic] begin directory=" << directory;
  QJsonArray selected;
  const std::vector<std::pair<std::string, std::vector<std::string>>> groups{
    {"ComponentSelector", {"ComponentIDValue", "ComponentEnable", "PixelFormat"}},
    {"Scan3dCoordinateSelector", {"Scan3dCoordinateScale", "Scan3dCoordinateOffset", "Scan3dInvalidDataFlag", "Scan3dInvalidDataValue"}},
    {"TriggerSelector", {"TriggerMode", "TriggerSource", "TriggerActivation"}},
    {"ChunkSelector", {"ChunkEnable"}}};
  for (const auto& [selectorName, fields] : groups) {
    FeatureNode selector;
    try { selector = lease.read_node(0, selectorName); }
    catch (const std::exception& e) {
      selected.append(QJsonObject{{"selector", QString::fromStdString(selectorName)}, {"error", e.what()}}); continue;
    }
    if (!selector.readable || !selector.writable || selector.kind != NodeKind::Enumeration) {
      selected.append(QJsonObject{{"selector", nodeRecord(selector)}, {"skipped", "Selector is not readable/writable enum"}}); continue;
    }
    try {
      for (const auto& entry : selector.enum_entries) {
        QJsonObject record{{"selector", QString::fromStdString(selectorName)}, {"entry", QString::fromStdString(entry)}};
        try {
          lease.write_node(0, selectorName, "enum", entry);
          for (const auto& field : fields) {
            try { record.insert(QString::fromStdString(field), nodeRecord(lease.read_node(0, field))); }
            catch (const std::exception& e) { record.insert(QString::fromStdString(field), QJsonObject{{"error", e.what()}}); }
          }
        } catch (const std::exception& e) { record.insert("error", e.what()); }
        selected.append(record);
      }
    } catch (...) {
      lease.write_node(0, selectorName, "enum", selector.enum_value);
      throw;
    }
    lease.write_node(0, selectorName, "enum", selector.enum_value);
    if (lease.read_node(0, selectorName).enum_value != selector.enum_value)
      throw std::runtime_error("Diagnostic selector restoration failed: " + selectorName);
  }
  write(QDir(directory).filePath("selectors.json"), QJsonDocument(selected).toJson());
  qInfo().noquote() << "[SingleDiagnostic] selectors restored; acquisition next; directory=" << directory;
  return directory;
}
/** @brief Persists the exact delivered bytes and their layout for one explicit Single request. */
inline void delivered(const Frame& frame, const QString& directory) {
  if (directory.isEmpty()) return;
  QJsonArray parts, chunks;
  const auto saveBytes = [&](const QString& name, const std::vector<std::uint8_t>& bytes) {
    write(QDir(directory).filePath(name), QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())));
  };
  saveBytes("payload.bin", frame.payload);
  for (std::size_t i = 0; i < frame.parts.size(); ++i) {
    const auto& p = frame.parts[i]; const auto file = QString("part-%1.bin").arg(i);
    saveBytes(file, p.payload);
    parts.append(QJsonObject{{"index", static_cast<int>(i)}, {"file", file}, {"type", QString::number(p.data_type)},
      {"PFNC", QString::number(p.data_format, 16)}, {"width", static_cast<int>(p.width)}, {"height", static_cast<int>(p.height)},
      {"stride", QString::number(p.stride)}, {"source", QString::number(p.source_id)}, {"region", QString::number(p.region_id)}, {"purpose", QString::number(p.purpose_id)}});
  }
  for (std::size_t i = 0; i < frame.chunks.size(); ++i) {
    const auto& c = frame.chunks[i]; const auto file = QString("chunk-%1.bin").arg(i); saveBytes(file, c.payload);
    chunks.append(QJsonObject{{"file", file}, {"id", QString::number(c.id)}, {"offset", QString::number(c.offset)}});
  }
  QJsonObject geometry;
  if (frame.scan3d_geometry) {
    const auto& g = *frame.scan3d_geometry;
    QJsonArray axes;
    for (std::size_t i = 0; i < 3; ++i) axes.append(QJsonObject{{"axis", static_cast<int>(i)},
      {"calibrated", g.calibrated_axes[i]}, {"scale", g.scale[i]}, {"offset", g.offset[i]},
      {"invalidEnabled", g.invalid_enabled[i]}, {"invalidValue", g.invalid_value[i]}});
    geometry = {{"mode", QString::fromStdString(g.output_mode)}, {"unit", QString::fromStdString(g.distance_unit)},
      {"system", QString::fromStdString(g.coordinate_system)}, {"axes", axes}};
  }
  write(QDir(directory).filePath("frame.json"), QJsonDocument(QJsonObject{{"frameId", QString::number(frame.frame_id)},
    {"timestamp", QString::number(frame.timestamp)}, {"timestampNs", QString::number(frame.timestamp_ns)},
    {"width", static_cast<int>(frame.width)}, {"height", static_cast<int>(frame.height)},
    {"PFNC", QString::number(frame.pixel_format, 16)}, {"payloadType", QString::number(frame.payload_type)},
    {"endianness", static_cast<int>(frame.pixel_endianness)}, {"incomplete", frame.buffer_info.incomplete},
    {"stream", QString::fromStdString(frame.stream_id)}, {"geometry", geometry},
    {"parts", parts}, {"chunks", chunks}}).toJson());
  qInfo().noquote() << "[SingleDiagnostic] delivered raw artifacts directory=" << directory;
}
} // namespace onegen::diagnostics
