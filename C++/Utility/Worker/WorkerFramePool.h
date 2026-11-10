#pragma once

#include "OneGenWorkerMessages.h"
#include "OneGenWorkerProtocol.h"

#include "Internal/DirectFrameSink.h"
#include "Internal/SharedMemoryRegion.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#ifdef reserve
#undef reserve
#endif

namespace onegen::worker {

using onegen::detail::FallbackReason;
using onegen::detail::ReserveStatus;

/** @brief Worker-side slot lifecycle; Retired waits for the exact host release. */
enum class SlotState : std::uint8_t { Free, Reserved, Ready, Retired };

/** @brief Result of a host SlotRelease after exact identity validation. */
enum class ReleaseResult : std::uint8_t {
  Released,
  Stale,
  Duplicate,
  Unknown,
  NoPool,
};

/** @brief Read-only slot view for tests and diagnostics. */
struct SlotSnapshot {
  SlotState state = SlotState::Free;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t frame_id = 0;
  std::size_t used_bytes = 0;
};

/**
 * @brief One attached worker frame region with generation/sequence slot state.
 *
 * The pool is shared by the live loop and the request thread. reserve never
 * waits: when no slot is free it returns Busy. The caller drops one dequeued
 * frame and calls note_slot_exhausted(), so the counter tracks dropped frames
 * rather than reserve retries.
 */
class WorkerFramePool {
public:
  static constexpr std::uint32_t maximum_slots = 16;
  static constexpr std::size_t maximum_region_name_bytes = 128;

  static std::shared_ptr<WorkerFramePool> attach(std::uint32_t session,
                                                 const FramePoolAttach& value,
                                                 std::string& error);

  WorkerFramePool(const WorkerFramePool&) = delete;
  WorkerFramePool& operator=(const WorkerFramePool&) = delete;

  [[nodiscard]] ReserveStatus reserve(onegen::detail::DirectReservation& reservation);
  [[nodiscard]] bool prepare_publish(const onegen::detail::DirectReservation& reservation,
                                     const Frame& metadata, std::size_t used_bytes,
                                     SlotIdentity& identity, bool& stopped);
  void fail_publish_encode(const onegen::detail::DirectReservation& reservation) noexcept;
  void fail_publish_send(const onegen::detail::DirectReservation& reservation) noexcept;
  void cancel(const onegen::detail::DirectReservation& reservation,
              onegen::detail::CancelCause cause) noexcept;
  [[nodiscard]] ReleaseResult release(const SlotRelease& value) noexcept;

  void note_slot_exhausted() noexcept;

  void detach() noexcept;
  void request_stop() noexcept;
  void resume() noexcept;
  /** Marks reservations stopped; the next reserve returns Stopped until resume(). */
  void wake() noexcept;

  [[nodiscard]] std::uint32_t session() const noexcept { return session_; }
  [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::uint64_t token() const noexcept { return token_; }
  [[nodiscard]] std::size_t slot_bytes() const noexcept { return slot_bytes_; }
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] std::uint64_t slot_exhausted() const noexcept;

  [[nodiscard]] SlotSnapshot slot(std::uint32_t index) const;
  [[nodiscard]] std::uint64_t sequence() const;

private:
  struct Slot {
    SlotState state = SlotState::Free;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t frame_id = 0;
    std::size_t used_bytes = 0;
  };

  WorkerFramePool(std::uint32_t session, std::shared_ptr<SharedMemoryRegion> region,
                  std::uint32_t slot_count, std::size_t slot_bytes, std::uint64_t epoch,
                  std::uint64_t token);

  [[nodiscard]] bool valid_reservation(const onegen::detail::DirectReservation& reservation) const;
  void free_reserved_locked(Slot& slot) noexcept;

  mutable std::mutex mutex_;
  std::shared_ptr<SharedMemoryRegion> region_;
  std::vector<Slot> slots_;
  std::uint32_t session_ = 0;
  std::uint32_t slot_count_ = 0;
  std::size_t slot_bytes_ = 0;
  std::uint64_t epoch_ = 0;
  std::uint64_t token_ = 0;
  std::uint64_t sequence_ = 0;
  std::uint64_t slot_exhausted_ = 0;
  bool active_ = true;
  bool stopping_ = false;
};

/**
 * @brief DirectFrameSink backed by a WorkerFramePool and the worker output path.
 *
 * publish marks the slot Ready before encoding, encodes outside the pool mutex,
 * and calls PublishFn with the exact v16 FrameReady body. Encode failures free
 * the slot; send failures retire it until the exact host release.
 */
class WorkerDirectSink final : public onegen::detail::DirectFrameSink {
public:
  using PublishFn = void (*)(void* user, const SlotIdentity& identity,
                             std::span<const std::byte> body);
  using ErrorFn = void (*)(void* user, std::uint32_t session, std::string_view message);

  WorkerDirectSink(PublishFn publish, void* publish_user, ErrorFn error = nullptr,
                   void* error_user = nullptr) noexcept;

  [[nodiscard]] bool attach(std::shared_ptr<WorkerFramePool> pool, std::string& error);
  void detach() noexcept;
  [[nodiscard]] ReleaseResult release(const SlotRelease& value) noexcept;
  void request_stop() noexcept;
  void resume() noexcept;

  [[nodiscard]] ReserveStatus reserve(onegen::detail::DirectReservation& reservation) override;
  void publish(const onegen::detail::DirectReservation& reservation, const Frame& metadata,
               std::size_t used_bytes) override;
  void cancel(const onegen::detail::DirectReservation& reservation,
              onegen::detail::CancelCause cause) noexcept override;
  void note_slot_exhausted() noexcept override;
  void note_fallback(onegen::detail::FallbackReason reason, const Frame& frame) noexcept override;
  [[nodiscard]] onegen::detail::FallbackReason metadata_gate(
      const Frame& metadata) const noexcept override;
  void report_capacity_failure(const Frame& metadata, std::size_t required,
                               std::size_t available) noexcept override;
  void wake() noexcept override;

  [[nodiscard]] std::shared_ptr<WorkerFramePool> pool() const;

private:
  [[nodiscard]] std::shared_ptr<WorkerFramePool> pool_snapshot() const;

  mutable std::mutex mutex_;
  std::shared_ptr<WorkerFramePool> pool_;
  PublishFn publish_ = nullptr;
  void* publish_user_ = nullptr;
  ErrorFn error_ = nullptr;
  void* error_user_ = nullptr;
  std::atomic<std::uint64_t> consecutive_capacity_failures_{0};
  std::atomic<std::uint64_t> total_capacity_failures_{0};
};

}  // namespace onegen::worker
