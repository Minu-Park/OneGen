#pragma once

#include <cstdarg>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace onegen {

/** @brief Serializes a complete typed stage record to the consumer-capturable diagnostic stream. */
inline void stage_log_v(const char* level, const char* fmt, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  const int length = std::vsnprintf(nullptr, 0, fmt, copy);
  va_end(copy);
  if (length < 0) return;
  std::vector<char> buffer(static_cast<std::size_t>(length) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), fmt, args);
  const std::string line = std::string("[GenICam] [") + level + "] " + buffer.data() + '\n';
  static std::recursive_mutex mutex;
  std::lock_guard lock(mutex);
  std::clog << line << std::flush;
}

/** @brief Writes one flushed informational stage record without changing the worker protocol. */
inline void stage_log(const char* fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  stage_log_v("INFO", fmt, args);
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
