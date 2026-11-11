#include "FramePool.h"

#include "Internal/StageLog.h"

#include <algorithm>
#include <limits>
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace onegen::producer {
namespace {

std::uint8_t* allocate_slot(std::uint64_t bytes) noexcept {
#ifdef _WIN32
  return static_cast<std::uint8_t*>(
      VirtualAlloc(nullptr, static_cast<SIZE_T>(bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
#else
  void* memory = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return memory == MAP_FAILED ? nullptr : static_cast<std::uint8_t*>(memory);
#endif
}

void free_slot(std::uint8_t* memory, std::uint64_t bytes) noexcept {
  if (!memory) return;
#ifdef _WIN32
  (void)bytes;
  VirtualFree(memory, 0, MEM_RELEASE);
#else
  munmap(memory, static_cast<size_t>(bytes));
#endif
}

// Touch every page so allocation failures surface at pool creation, not mid-acquisition.
void prefault(std::uint8_t* memory, std::uint64_t bytes) noexcept {
  constexpr std::uint64_t page = 4096;
  for (std::uint64_t offset = 0; offset < bytes; offset += page) memory[offset] = 0;
}

}  // namespace

FramePoolPlan plan_frame_pool(std::uint64_t payload_bytes, std::uint32_t minimum_slots,
                              std::uint32_t preferred_slots, std::uint64_t budget_bytes) noexcept {
  FramePoolPlan plan;
  if (payload_bytes == 0) {
    plan.refusal = "PayloadSize is unavailable.";
    return plan;
  }
  plan.slot_bytes = payload_bytes;
  const auto affordable = budget_bytes / payload_bytes;
  const auto slots = std::min<std::uint64_t>(preferred_slots, affordable);
  if (slots < minimum_slots) {
    plan.refusal = "Payload of " + std::to_string(payload_bytes) + " bytes needs "
        + std::to_string(minimum_slots) + " frame slots, which exceeds the "
        + std::to_string(budget_bytes) + "-byte frame buffer budget.";
    return plan;
  }
  plan.slot_count = static_cast<std::uint32_t>(slots);
  return plan;
}

FrameLease::FrameLease(std::shared_ptr<FramePool> pool, std::uint32_t slot,
                       std::span<const std::uint8_t> bytes, Frame metadata) noexcept
    : pool_(std::move(pool)), slot_(slot), bytes_(bytes), metadata_(std::move(metadata)) {}

FrameLease::~FrameLease() {
  if (pool_) pool_->release(slot_);
}

std::shared_ptr<FramePool> FramePool::create(std::uint32_t slots, std::uint64_t slot_bytes,
                                             std::string& error) {
  if (slots == 0 || slot_bytes == 0) {
    error = "Frame pool requires at least one non-empty slot.";
    return {};
  }
  std::shared_ptr<FramePool> pool(new (std::nothrow) FramePool(slots, slot_bytes));
  if (!pool) {
    error = "Frame pool allocation failed.";
    return {};
  }
  for (std::uint32_t index = 0; index < slots; ++index) {
    auto* memory = allocate_slot(slot_bytes);
    if (!memory) {
      error = "Could not allocate " + std::to_string(slots) + " frame slots of "
          + std::to_string(slot_bytes) + " bytes.";
      return {};
    }
    prefault(memory, slot_bytes);
    pool->memory_.push_back(memory);
    pool->free_.push_back(index);
  }
  return pool;
}

FramePool::FramePool(std::uint32_t slots, std::uint64_t slot_bytes) noexcept
    : slot_count_(slots), slot_bytes_(slot_bytes), states_(slots, SlotState::Free) {
  memory_.reserve(slots);
  free_.reserve(slots);
}

FramePool::~FramePool() {
  for (auto* memory : memory_) free_slot(memory, slot_bytes_);
}

void FramePool::set_frame_callback(SharedFrameFn callback) {
  std::lock_guard lock(mutex_);
  on_frame_ = std::move(callback);
}

void FramePool::set_error_callback(ErrorFn callback) {
  std::lock_guard lock(mutex_);
  on_error_ = std::move(callback);
}

void FramePool::request_stop() noexcept {
  std::lock_guard lock(mutex_);
  stopped_ = true;
}

void FramePool::resume() noexcept {
  std::lock_guard lock(mutex_);
  stopped_ = false;
}

FramePoolCounters FramePool::counters() const {
  FramePoolCounters result;
  result.published = published_.load(std::memory_order_relaxed);
  result.exhausted = exhausted_.load(std::memory_order_relaxed);
  result.capacity_failures = capacity_failures_.load(std::memory_order_relaxed);
  result.fallbacks = fallbacks_.load(std::memory_order_relaxed);
  std::lock_guard lock(mutex_);
  result.held = held_;
  result.max_held = max_held_;
  return result;
}

detail::ReserveStatus FramePool::reserve(detail::DirectReservation& reservation) {
  std::lock_guard lock(mutex_);
  if (stopped_) return detail::ReserveStatus::Stopped;
  if (free_.empty()) return detail::ReserveStatus::Busy;
  const auto slot = free_.back();
  free_.pop_back();
  states_[slot] = SlotState::Reserved;
  reservation.destination = {memory_[slot], static_cast<std::size_t>(slot_bytes_)};
  reservation.ticket = slot;
  reservation.owner = shared_from_this();
  return detail::ReserveStatus::Reserved;
}

void FramePool::publish(const detail::DirectReservation& reservation, const Frame& metadata,
                        std::size_t used_bytes) {
  const auto slot = static_cast<std::uint32_t>(reservation.ticket);
  SharedFrameFn callback;
  {
    std::lock_guard lock(mutex_);
    if (slot >= slot_count_ || states_[slot] != SlotState::Reserved) return;
    if (used_bytes > slot_bytes_) {
      // The writer never exceeds its destination; refuse instead of exposing a bad span.
      states_[slot] = SlotState::Free;
      free_.push_back(slot);
      return;
    }
    states_[slot] = SlotState::Held;
    max_held_ = std::max(max_held_, ++held_);
    callback = on_frame_;
  }
  published_.fetch_add(1, std::memory_order_relaxed);
  // The lease owns the slot from here; dropping it without a callback frees the slot.
  auto lease = std::shared_ptr<const FrameLease>(new FrameLease(
      shared_from_this(), slot, {memory_[slot], used_bytes}, metadata));
  if (callback) callback(SharedFrame{std::move(lease)});
}

void FramePool::cancel(const detail::DirectReservation& reservation,
                       detail::CancelCause) noexcept {
  const auto slot = static_cast<std::uint32_t>(reservation.ticket);
  std::lock_guard lock(mutex_);
  if (slot >= slot_count_ || states_[slot] != SlotState::Reserved) return;
  states_[slot] = SlotState::Free;
  free_.push_back(slot);
}

void FramePool::release(std::uint32_t slot) noexcept {
  std::lock_guard lock(mutex_);
  if (slot >= slot_count_ || states_[slot] != SlotState::Held) return;
  states_[slot] = SlotState::Free;
  --held_;
  free_.push_back(slot);
}

void FramePool::note_slot_exhausted() noexcept {
  const auto dropped = exhausted_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (dropped == 1 || dropped % 100 == 0) {
    stage_log_warning("frame slot exhausted: dropped=%llu slots=%u",
                      static_cast<unsigned long long>(dropped), slot_count_);
  }
}

void FramePool::note_fallback(detail::FallbackReason reason, const Frame&) noexcept {
  const auto count = fallbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1)
    stage_log("frame layout uses the owned path: reason=%u", static_cast<unsigned>(reason));
}

detail::FallbackReason FramePool::metadata_gate(const Frame&) const noexcept {
  // In-process metadata is not serialized, so no transport size limit applies.
  return detail::FallbackReason::None;
}

void FramePool::report_capacity_failure(const Frame& metadata, std::size_t required,
                                        std::size_t available) noexcept {
  const auto count = capacity_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1 || count % 100 == 0) {
    report("frame does not fit its slot: frame=" + std::to_string(metadata.frame_id)
           + " required=" + std::to_string(required) + " available=" + std::to_string(available)
           + " count=" + std::to_string(count) + "; restart acquisition after a payload change");
  }
}

void FramePool::report(std::string_view message) noexcept {
  try {
    ErrorFn callback;
    {
      std::lock_guard lock(mutex_);
      callback = on_error_;
    }
    if (callback) callback(message);
    else stage_log_warning("%.*s", static_cast<int>(message.size()), message.data());
  } catch (...) {
  }
}

}  // namespace onegen::producer
