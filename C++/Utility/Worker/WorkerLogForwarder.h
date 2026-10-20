#pragma once

#include <QByteArray>
#include <QDebug>
#include <QRegularExpression>
#include <QString>
#include <utility>

namespace onegen::worker {

/** @brief Forwards diagnostic stderr records while leaving binary stdout untouched. */
class WorkerLogForwarder {
public:
  /** @brief Associates forwarded diagnostics with their producer. */
  explicit WorkerLogForwarder(QString cti) : cti_(std::move(cti)) {}
  /** @brief Records scan diagnostics as informational text, retaining the original producer severity in the body. */
  void setDiscoveryMode(bool enabled) { discovery_ = enabled; }

  /** @brief Accepts fragmented UTF-8 diagnostics and forwards complete lines. */
  void append(const QByteArray& bytes) {
    pending_.append(bytes);
    for (auto newline = pending_.indexOf('\n'); newline >= 0; newline = pending_.indexOf('\n')) {
      forward(pending_.left(newline));
      pending_.remove(0, newline + 1);
    }
    // Preserve the existing incomplete-line limit without silently losing its diagnostic.
    if (pending_.size() > 65536) finish();
  }

  /** @brief Forwards the final unterminated diagnostic after the process stops. */
  void finish() {
    if (!pending_.isEmpty()) forward(pending_);
    pending_.clear();
  }

private:
  void forward(QByteArray line) const {
    if (line.endsWith('\r')) line.chop(1);
    if (line.isEmpty()) return;
    if (discovery_) {
      qInfo().noquote() << QStringLiteral("[GenICam] discovery diagnostic: %1 cti=%2")
          .arg(QString::fromUtf8(line), cti_);
      return;
    }
    const bool known = line.startsWith("[GenICam]");
    const auto header = known ? line.mid(9).trimmed() : QByteArray{};
    const QString message = (known ? QString() : QStringLiteral("[GenICam/Producer] "))
        + QString::fromUtf8(line) + QStringLiteral(" cti=") + cti_;
    // Match only a leading vendor severity field, never severity words in the message body.
    static const QRegularExpression vendor_level(QStringLiteral(
        R"(^\s*(?:\[\d{4}-\d{2}-\d{2}[ T][^\]\r\n]*\]\s*)?(?:\[(TRACE|DEBUG|INFO|NOTICE|WARN|WARNING|ERROR|CRIT|CRITICAL|FATAL)\]|(TRACE|DEBUG|INFO|NOTICE|WARN|WARNING|ERROR|CRIT|CRITICAL|FATAL)\s*:)(?:\s|$))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto vendor = known ? QRegularExpressionMatch{} : vendor_level.match(QString::fromUtf8(line));
    const auto level = (vendor.captured(1).isEmpty() ? vendor.captured(2) : vendor.captured(1)).toUpper();
    // Explicit tags retain severity; unclassified vendor output keeps its warning fallback.
    if (known && header.startsWith("[DEBUG]")) qDebug().noquote() << message;
    else if (known && (header.startsWith("[CRIT]") || header.startsWith("[ERROR]")
                      || header.startsWith("[FATAL]"))) qCritical().noquote() << message;
    else if (!known && (level == QLatin1String("DEBUG") || level == QLatin1String("TRACE")))
      qDebug().noquote() << message;
    else if (!known && (level == QLatin1String("ERROR") || level == QLatin1String("CRIT")
                        || level == QLatin1String("CRITICAL") || level == QLatin1String("FATAL")))
      qCritical().noquote() << message;
    else if (!known && (level == QLatin1String("INFO") || level == QLatin1String("NOTICE")))
      qInfo().noquote() << message;
    else if (!known || header.startsWith("[WARN]") || header.startsWith("[WARNING]"))
      qWarning().noquote() << message;
    else qInfo().noquote() << message;
  }

  QString cti_;
  QByteArray pending_;
  bool discovery_ = false;
};

} // namespace onegen::worker
