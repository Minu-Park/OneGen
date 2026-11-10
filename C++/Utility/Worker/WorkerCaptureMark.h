#pragma once

#include "OneGenTypes.h"
#include "OneGenWorkerProtocol.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace onegen::worker {

/**
 * @brief Worker-to-client capture evidence for one dequeued GenTL buffer.
 *
 * The worker sends one mark for every buffer it dequeues, before it copies,
 * publishes or drops that buffer, so the client mirrors the producer counter
 * without a synchronous round trip. `sequence` is derived only from the GenTL
 * frame id. An invalid mark (a frame id that is not capture evidence) keeps the
 * newest valid `sequence`, so the mirrored counter never moves backwards.
 * `dropped` is the session total of transport and slot drops.
 */
struct CaptureMark {
  bool valid = false;
  std::uint64_t generation = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t trigger_id = 0;
  std::uint64_t dropped = 0;
};

inline void write(Writer& out, const CaptureMark& mark)
{
  out.scalar(static_cast<std::uint8_t>(mark.valid ? 1 : 0));
  out.scalar(mark.generation);
  out.scalar(mark.frame_id);
  out.scalar(mark.sequence);
  out.scalar(mark.trigger_id);
  out.scalar(mark.dropped);
}

inline CaptureMark readCaptureMark(Reader& in)
{
  CaptureMark mark;
  mark.valid = in.scalar<std::uint8_t>() != 0;
  mark.generation = in.scalar<std::uint64_t>();
  mark.frame_id = in.scalar<std::uint64_t>();
  mark.sequence = in.scalar<std::uint64_t>();
  mark.trigger_id = in.scalar<std::uint64_t>();
  mark.dropped = in.scalar<std::uint64_t>();
  return mark;
}

/**
 * @brief Derives a monotonic capture sequence from GenTL frame ids.
 *
 * Used only on the acquisition thread. A frame id that moves backwards (a
 * 16-bit GigE Vision 1.x id wraps to 1) continues the sequence; a repeated id
 * (a producer without frame-id support) is not capture evidence.
 */
class CaptureSequence {
public:
  void reset() noexcept
  {
    last_ = 0;
    offset_ = 0;
    seen_ = false;
  }

  /** @brief Returns the sequence for `frame_id`, or `nullopt` when it is not evidence. */
  std::optional<std::uint64_t> observe(const std::uint64_t frame_id) noexcept
  {
    if (seen_ && frame_id == last_) return std::nullopt;
    if (seen_ && frame_id < last_) offset_ += last_;
    last_ = frame_id;
    seen_ = true;
    return offset_ + frame_id;
  }

  /** @brief Returns the newest valid sequence, or zero before the first frame. */
  [[nodiscard]] std::uint64_t current() const noexcept { return seen_ ? offset_ + last_ : 0; }

private:
  std::uint64_t last_ = 0;
  std::uint64_t offset_ = 0;
  bool seen_ = false;
};

/**
 * @brief Client-side mirror of the newest capture mark per session.
 *
 * The client reader thread updates it when a mark arrives; consumers read it
 * from any thread. The lock guards only this map and is never held while a
 * frame is read, copied or delivered.
 */
class CaptureBoard {
public:
  void update(const std::uint32_t session, const CaptureMark& mark)
  {
    std::lock_guard lock(mutex_);
    marks_[session] = mark;
  }

  void clear(const std::uint32_t session)
  {
    std::lock_guard lock(mutex_);
    marks_.erase(session);
  }

  [[nodiscard]] std::optional<CaptureMark> latest(const std::uint32_t session) const
  {
    std::lock_guard lock(mutex_);
    const auto found = marks_.find(session);
    if (found == marks_.end()) return std::nullopt;
    return found->second;
  }

  /**
   * @brief Stamps a delivered frame with the mark of its own buffer.
   * @note The worker writes a buffer's mark before that buffer's frame on the
   *       same ordered channel, so the newest mark matches when ids agree.
   */
  [[nodiscard]] Frame::Capture stamp(const std::uint32_t session, const std::uint64_t frame_id) const
  {
    Frame::Capture capture;
    const auto mark = latest(session);
    if (!mark || !mark->valid || mark->frame_id != frame_id) return capture;
    capture.valid = true;
    capture.generation = mark->generation;
    capture.sequence = mark->sequence;
    capture.trigger_id = mark->trigger_id;
    return capture;
  }

private:
  mutable std::mutex mutex_;
  std::map<std::uint32_t, CaptureMark> marks_;
};

}  // namespace onegen::worker
