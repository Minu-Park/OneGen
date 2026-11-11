#pragma once

#include <chrono>
#include <cstdlib>

namespace onegen {

/** @brief Caches the opt-in frame trace flag so Debug-off frames add no environment lookup. */
inline bool frame_trace_enabled()
{
  static const bool enabled = [] {
    const auto* value = std::getenv("ONEGEN_TRACE_FRAMES");
    return value && value[0] == '1';
  }();
  return enabled;
}

/** @brief Converts a steady-clock span to milliseconds for one trace field. */
inline double frame_trace_ms(std::chrono::steady_clock::time_point from,
                             std::chrono::steady_clock::time_point to)
{
  return std::chrono::duration<double, std::milli>(to - from).count();
}

} // namespace onegen
