#pragma once

#include "OneGenLog.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace onegen {

namespace detail {
/** Process-wide consumer sink; null keeps the standard diagnostic stream. */
inline std::atomic<LogSink> log_sink{nullptr};
/** True while this thread probes producers; its records are scan detail, not outcomes. */
inline thread_local bool discovery_scope = false;
}  // namespace detail

/** @brief Reports this thread's records at Debug; the scan owner reports the outcome. */
class StageLogDiscoveryScope {
public:
  StageLogDiscoveryScope() noexcept : previous_(detail::discovery_scope) {
    detail::discovery_scope = true;
  }
  ~StageLogDiscoveryScope() { detail::discovery_scope = previous_; }
  StageLogDiscoveryScope(const StageLogDiscoveryScope&) = delete;
  StageLogDiscoveryScope& operator=(const StageLogDiscoveryScope&) = delete;

private:
  bool previous_;
};

/** @brief Serializes one stage record to the consumer sink, or to the standard diagnostic stream. */
inline void stage_log_v(const char* level, const char* fmt, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  const int length = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (length < 0) return;
  try {
    std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
    std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
    if (const auto sink = detail::log_sink.load()) {
      const LogLevel value = level[0] == 'D' || detail::discovery_scope ? LogLevel::Debug
          : level[0] == 'W' ? LogLevel::Warning : LogLevel::Info;
      sink(value, buffer.data());
      return;
    }
    const std::string line = std::string("[GenICam] [") + level + "] " + buffer.data() + '\n';
    static std::recursive_mutex mutex;
    std::lock_guard lock(mutex);
    std::clog << line << std::flush;
  } catch (...) {
    // Diagnostics never fail acquisition or lifecycle transitions.
  }
}

/** @brief Writes one flushed informational stage record for the consumer log. */
inline void stage_log(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  stage_log_v("INFO", fmt, args);
  va_end(args);
}

/** @brief Writes an opt-in debug record that the consumer drops while Debug capture is off. */
inline void stage_log_debug(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  stage_log_v("DEBUG", fmt, args);
  va_end(args);
}

/** @brief Writes a failure diagnostic with an explicit warning severity. */
inline void stage_log_warning(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  stage_log_v("WARN", fmt, args);
  va_end(args);
}

}  // namespace onegen
