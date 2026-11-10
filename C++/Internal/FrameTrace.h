#pragma once

#include "StageLog.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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

/** @brief Worker-side stages for one frame; frame_id is the producer FrameID shared with the host. */
struct WorkerFrameTrace {
  std::uint32_t session = 0;
  std::uint64_t frame_id = 0;
  std::size_t bytes = 0;
  double serialize_ms = 0;
  double encode_ms = 0;
  double write_ms = 0;
  std::uint64_t transport_dropped = 0;
};

/** @brief Emits one bounded worker record without letting diagnostics fail acquisition. */
inline void trace_worker_frame(const WorkerFrameTrace& trace)
{
  if (!frame_trace_enabled()) return;
  try {
    stage_log_debug(
        "frame transport: side=worker session=%u frame=%llu bytes=%zu serialize_ms=%.3f "
        "encode_ms=%.3f write_ms=%.3f transport_dropped=%llu",
        trace.session, static_cast<unsigned long long>(trace.frame_id), trace.bytes,
        trace.serialize_ms, trace.encode_ms, trace.write_ms,
        static_cast<unsigned long long>(trace.transport_dropped));
  } catch (...) {
  }
}

} // namespace onegen
