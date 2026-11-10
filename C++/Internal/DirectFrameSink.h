#pragma once

#include "OneGenTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace onegen::detail {

enum class FallbackReason : std::uint8_t {
  None,
  UnknownLayout,
  HostUnsupported,
  LayoutConstraint,
  PackedRowAlignment,
  GeometryUnrepresentable,
  Planar,
  MultiPart,
  ChunkData,
  NonImagePayload,
  Jpeg,
  Incomplete,
  MetadataTooLarge,
};

struct DirectReservation {
  std::span<std::uint8_t> destination;
  std::uint64_t ticket = 0;
  /** Keeps the owning pool and its mapping alive until publish/cancel completes. */
  std::shared_ptr<void> owner;
};

enum class ReserveStatus : std::uint8_t { Reserved, NoPool, Stopped, Busy };

enum class CancelCause : std::uint8_t {
  Timeout,
  Fallback,
  Capacity,
  Malformed,
  Exception,
  Stop,
  Detached,
};

/** Worker-owned destination/pool operations; all calls are thread-affine to the live loop. */
class DirectFrameSink {
public:
  virtual ~DirectFrameSink() = default;

  virtual ReserveStatus reserve(DirectReservation& reservation) = 0;
  virtual void publish(const DirectReservation& reservation, const Frame& metadata,
                       std::size_t used_bytes) = 0;
  virtual void cancel(const DirectReservation& reservation, CancelCause cause) noexcept = 0;
  /** Records one frame that was dequeued and discarded because no slot was free. */
  virtual void note_slot_exhausted() noexcept {}
  virtual void note_fallback(FallbackReason reason, const Frame& frame) noexcept = 0;
  virtual FallbackReason metadata_gate(const Frame& metadata) const noexcept = 0;
  virtual void report_capacity_failure(const Frame& metadata, std::size_t required,
                                       std::size_t available) noexcept = 0;
  virtual void wake() noexcept = 0;
};

}  // namespace onegen::detail
