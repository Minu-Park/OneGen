#pragma once

#include <QByteArray>
#include <QDebug>
#include <QString>
#include <utility>

namespace onegen::worker {

/** @brief Forwards diagnostic stderr records while leaving binary stdout untouched. */
class WorkerLogForwarder {
public:
  /** @brief Associates forwarded diagnostics with their producer. */
  explicit WorkerLogForwarder(QString cti) : cti_(std::move(cti)) {}

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
    const bool known = line.startsWith("[GenICam]");
    const auto header = known ? line.mid(9).trimmed() : QByteArray{};
    const QString message = (known ? QString() : QStringLiteral("[GenICam/Producer] "))
        + QString::fromUtf8(line) + QStringLiteral(" cti=") + cti_;
    // Explicit tags retain severity for any consumer; legacy stage lines remain informational.
    if (known && header.startsWith("[DEBUG]")) qDebug().noquote() << message;
    else if (known && (header.startsWith("[CRIT]") || header.startsWith("[ERROR]")
                      || header.startsWith("[FATAL]"))) qCritical().noquote() << message;
    else if (!known || header.startsWith("[WARN]") || header.startsWith("[WARNING]"))
      qWarning().noquote() << message;
    else qInfo().noquote() << message;
  }

  QString cti_;
  QByteArray pending_;
};

} // namespace onegen::worker
