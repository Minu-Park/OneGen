#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtEndian>
#include <limits>

namespace onegen::worker {

inline constexpr int protocol_version = 1;
inline constexpr quint32 maximum_json_size = 16U * 1024U * 1024U;

inline QByteArray encode(const QJsonObject& message, const QByteArray& binary = {})
{
  QJsonObject body = message;
  if (!binary.isEmpty())
    body.insert(QStringLiteral("binarySize"), static_cast<qint64>(binary.size()));
  const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
  QByteArray frame(4, Qt::Uninitialized);
  qToBigEndian(static_cast<quint32>(payload.size()), frame.data());
  frame.append(payload);
  frame.append(binary);
  return frame;
}

inline bool decode(QByteArray& buffer, QJsonObject* message, QByteArray* binary, QString* error)
{
  if (buffer.size() < 4) return false;
  const auto size = qFromBigEndian<quint32>(buffer.constData());
  if (size > maximum_json_size) {
    if (error) *error = QStringLiteral("Worker message exceeds the size limit.");
    buffer.clear();
    return false;
  }
  if (buffer.size() < 4 + static_cast<qsizetype>(size)) return false;
  QJsonParseError parse_error;
  const auto document = QJsonDocument::fromJson(buffer.mid(4, size), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    if (error) *error = QStringLiteral("Invalid worker JSON message: %1").arg(parse_error.errorString());
    buffer.remove(0, 4 + size);
    return false;
  }
  const auto object = document.object();
  const auto binary_size = object.value(QStringLiteral("binarySize")).toInteger();
  if (binary_size < 0 || binary_size > (std::numeric_limits<qsizetype>::max)() - 4 - size) {
    if (error) *error = QStringLiteral("Invalid worker binary payload size.");
    buffer.clear();
    return false;
  }
  if (buffer.size() < 4 + static_cast<qsizetype>(size) + static_cast<qsizetype>(binary_size))
    return false;
  if (message) *message = object;
  if (binary) *binary = buffer.mid(4 + size, static_cast<qsizetype>(binary_size));
  buffer.remove(0, 4 + size + static_cast<qsizetype>(binary_size));
  return true;
}

inline bool decode(QByteArray& buffer, QJsonObject* message, QString* error)
{
  QByteArray binary;
  return decode(buffer, message, &binary, error);
}

} // namespace onegen::worker
