#pragma once

/** @file DirectLeaseSink.h @brief Consumer-process in-flight lease set behind the pinned direct acquisition path. */

#include "Internal/DirectFrameSink.h"
#include "OneGenTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace onegen::producer {

class DirectLeaseSink;

/**
 * @brief One published slot. The bytes stay valid and immutable while any reference to this
 *        lease is alive; releasing the last reference returns the slot to the sink.
 */
class FrameLease {
public:
  FrameLease(const FrameLease&) = delete;
  FrameLease& operator=(const FrameLease&) = delete;
  ~FrameLease();

  /** @brief Frame metadata; its payload vectors are empty, the image bytes are bytes(). */
  [[nodiscard]] const Frame& metadata() const noexcept { return metadata_; }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }

private:
  friend class DirectLeaseSink;
  FrameLease(std::shared_ptr<DirectLeaseSink> pool, std::uint32_t slot,
             std::span<const std::uint8_t> bytes, Frame metadata,
             std::shared_ptr<std::uint8_t> storage) noexcept;
  std::shared_ptr<DirectLeaseSink> pool_;
  // Consumer-provided bytes behind bytes(); the lease keeps this storage alive.
  std::shared_ptr<std::uint8_t> storage_;
  std::uint32_t slot_ = 0;
  std::span<const std::uint8_t> bytes_;
  Frame metadata_;
};

/** @brief Delivered on the acquisition thread; the callee must not block. */
struct SharedFrame {
  std::shared_ptr<const FrameLease> lease;
};

/** @brief Counters for diagnostics; all values are cumulative for the sink. */
struct LeaseSinkCounters {
  std::uint64_t published = 0;
  std::uint64_t exhausted = 0;
  std::uint64_t capacity_failures = 0;
  std::uint64_t fallbacks = 0;
  std::uint32_t held = 0;
  std::uint32_t max_held = 0;
};

/**
 * @brief Bounded in-flight lease set that is also the pinned direct acquisition sink.
 *
 * This is not a plugin-owned frame pool: it owns no frame memory. Every reservation takes its
 * bytes from the consumer's provided storage and an empty result is a full set. The GenTL
 * announced buffers are owned by Camera::Impl independently of this sink.
 *
 * reserve/publish/cancel run on the acquisition thread. A published slot is handed to the
 * frame callback as a FrameLease and returns to the free list when the last reference drops,
 * from any thread. A full set makes the acquisition loop requeue the GenTL buffer and count
 * one exhausted frame; it never waits for a consumer.
 */
class DirectLeaseSink final : public detail::DirectFrameSink,
                              public std::enable_shared_from_this<DirectLeaseSink> {
public:
  using SharedFrameFn = std::function<void(const SharedFrame&)>;
  using ErrorFn = std::function<void(std::string_view)>;

  /**
   * @brief Storage for one frame: returns at least `bytes` writable bytes, or empty.
   *
   * Called on the acquisition thread. A provider may wait for its Host pool on the first
   * acquire of a size; an empty result counts as a full set: the GenTL buffer is requeued and
   * the frame is dropped.
   */
  using FrameStorage = std::function<std::shared_ptr<std::uint8_t>(std::size_t bytes)>;

  /**
   * @brief Creates a bounded lease set over consumer storage; returns null with error when the
   *        request is empty. The sink allocates no frame memory itself: the slot count only
   *        bounds how many frames are held at once.
   */
  static std::shared_ptr<DirectLeaseSink> create(std::uint32_t slots, std::uint64_t slot_bytes,
                                                 std::string& error, FrameStorage storage);
  ~DirectLeaseSink() override;
  DirectLeaseSink(const DirectLeaseSink&) = delete;
  DirectLeaseSink& operator=(const DirectLeaseSink&) = delete;

  void set_frame_callback(SharedFrameFn callback);
  void set_error_callback(ErrorFn callback);
  /** @brief Refuses new reservations; published leases stay valid. */
  void request_stop() noexcept;
  void resume() noexcept;

  [[nodiscard]] std::uint32_t slot_count() const noexcept { return slot_count_; }
  [[nodiscard]] std::uint64_t slot_bytes() const noexcept { return slot_bytes_; }
  [[nodiscard]] LeaseSinkCounters counters() const;
  /** @brief Frames dropped because every slot was in use; lock-free for diagnostics. */
  [[nodiscard]] std::uint64_t exhausted() const noexcept {
    return exhausted_.load(std::memory_order_relaxed);
  }

  detail::ReserveStatus reserve(detail::DirectReservation& reservation) override;
  void publish(const detail::DirectReservation& reservation, const Frame& metadata,
               std::size_t used_bytes) override;
  void cancel(const detail::DirectReservation& reservation,
              detail::CancelCause cause) noexcept override;
  void note_slot_exhausted() noexcept override;
  void note_fallback(detail::FallbackReason reason, const Frame& frame) noexcept override;
  [[nodiscard]] detail::FallbackReason metadata_gate(const Frame& metadata) const noexcept override;
  void report_capacity_failure(const Frame& metadata, std::size_t required,
                               std::size_t available) noexcept override;
  void wake() noexcept override {}

private:
  friend class FrameLease;
  DirectLeaseSink(std::uint32_t slots, std::uint64_t slot_bytes) noexcept;
  void release(std::uint32_t slot) noexcept;
  void report(std::string_view message) noexcept;

  enum class SlotState : std::uint8_t { Free, Reserved, Held };

  const std::uint32_t slot_count_;
  const std::uint64_t slot_bytes_;
  FrameStorage storage_;
  // Bytes of a reserved slot while the sink takes its storage from the consumer.
  std::vector<std::shared_ptr<std::uint8_t>> reserved_;
  mutable std::mutex mutex_;
  std::vector<SlotState> states_;
  std::vector<std::uint32_t> free_;
  std::uint32_t held_ = 0;
  std::uint32_t max_held_ = 0;
  bool stopped_ = false;
  SharedFrameFn on_frame_;
  ErrorFn on_error_;
  std::atomic<std::uint64_t> published_{0};
  std::atomic<std::uint64_t> exhausted_{0};
  std::atomic<std::uint64_t> capacity_failures_{0};
  std::atomic<std::uint64_t> fallbacks_{0};
};

}  // namespace onegen::producer
