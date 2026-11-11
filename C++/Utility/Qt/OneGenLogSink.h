#pragma once

/** @file OneGenLogSink.h @brief Routes in-process OneGen diagnostics to Qt logging. */

#include "OneGenLog.h"

#include <QDebug>
#include <QString>

namespace onegen::qt {

/** @brief Maps OneGen severities to Qt messages with the shared [GenICam] prefix. */
inline void forwardLog(LogLevel level, std::string_view message) noexcept {
  try {
    const auto text = QStringLiteral("[GenICam] ")
        + QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()));
    switch (level) {
      case LogLevel::Debug: qDebug().noquote() << text; break;
      case LogLevel::Info: qInfo().noquote() << text; break;
      case LogLevel::Warning: qWarning().noquote() << text; break;
    }
  } catch (...) {
  }
}

/** @brief Installs the Qt sink for this process. */
inline void installLogSink() noexcept { set_log_sink(&forwardLog); }

}  // namespace onegen::qt
