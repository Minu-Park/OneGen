#include "DirectLeaseSink.h"

#include "Internal/StageLog.h"

#include <algorithm>
#include <limits>
#include <new>

namespace onegen::producer {

FrameLease::FrameLease(std::shared_ptr<DirectLeaseSink> sink, std::uint32_t slot,
                       std::span<const std::uint8_t> bytes, Frame metadata,
                       std::shared_ptr<std::uint8_t> storage) noexcept
    : sink_(std::move(sink)), storage_(std::move(storage)), slot_(slot), bytes_(bytes),
      metadata_(std::move(metadata)) {}

FrameLease::~FrameLease() {
  if (sink_) sink_->release(slot_);
}

std::shared_ptr<DirectLeaseSink> DirectLeaseSink::create(std::uint32_t slots,
                                                         std::uint64_t slot_bytes,
                                                         std::string& error,
                                                         FrameStorage storage) {
  if (slots == 0 || slot_bytes == 0) {
    error = "Lease sink requires at least one non-empty slot.";
    return {};
  }
  if (!storage) {
    error = "Lease sink requires consumer storage.";
    return {};
  }
  std::shared_ptr<DirectLeaseSink> sink(new (std::nothrow) DirectLeaseSink(slots, slot_bytes));
  if (!sink) {
    error = "Lease sink allocation failed.";
    return {};
  }
  sink->storage_ = std::move(storage);
  sink->reserved_.resize(slots);
  for (std::uint32_t index = 0; index < slots; ++index) sink->free_.push_back(index);
  return sink;
}

DirectLeaseSink::DirectLeaseSink(std::uint32_t slots, std::uint64_t slot_bytes) noexcept
    : slot_count_(slots), slot_bytes_(slot_bytes), states_(slots, SlotState::Free) {
  free_.reserve(slots);
}

DirectLeaseSink::~DirectLeaseSink() = default;

void DirectLeaseSink::set_frame_callback(SharedFrameFn callback) {
  std::lock_guard lock(mutex_);
  on_frame_ = std::move(callback);
}

void DirectLeaseSink::set_error_callback(ErrorFn callback) {
  std::lock_guard lock(mutex_);
  on_error_ = std::move(callback);
}

void DirectLeaseSink::request_stop() noexcept {
  std::lock_guard lock(mutex_);
  stopped_ = true;
}

void DirectLeaseSink::resume() noexcept {
  std::lock_guard lock(mutex_);
  stopped_ = false;
}

LeaseSinkCounters DirectLeaseSink::counters() const {
  LeaseSinkCounters result;
  result.published = published_.load(std::memory_order_relaxed);
  result.exhausted = exhausted_.load(std::memory_order_relaxed);
  result.capacity_failures = capacity_failures_.load(std::memory_order_relaxed);
  result.fallbacks = fallbacks_.load(std::memory_order_relaxed);
  std::lock_guard lock(mutex_);
  result.held = held_;
  result.max_held = max_held_;
  return result;
}

detail::ReserveStatus DirectLeaseSink::reserve(detail::DirectReservation& reservation) {
  std::uint32_t slot = 0;
  {
    std::lock_guard lock(mutex_);
    if (stopped_) return detail::ReserveStatus::Stopped;
    if (free_.empty()) return detail::ReserveStatus::Busy;
    slot = free_.back();
    free_.pop_back();
    states_[slot] = SlotState::Reserved;
  }
  // Ask the consumer outside the sink lock: the first acquire of a size may wait for the Host
  // pool, and a frame release must not wait behind it. No storage is a full set.
  auto bytes = storage_(static_cast<std::size_t>(slot_bytes_));
  if (!bytes) {
    std::lock_guard lock(mutex_);
    states_[slot] = SlotState::Free;
    free_.push_back(slot);
    return detail::ReserveStatus::Busy;
  }
  std::uint8_t* memory = bytes.get();
  {
    std::lock_guard lock(mutex_);
    reserved_[slot] = std::move(bytes);
  }
  reservation.destination = {memory, static_cast<std::size_t>(slot_bytes_)};
  reservation.ticket = slot;
  reservation.owner = shared_from_this();
  return detail::ReserveStatus::Reserved;
}

void DirectLeaseSink::publish(const detail::DirectReservation& reservation, const Frame& metadata,
                              std::size_t used_bytes) {
  const auto slot = static_cast<std::uint32_t>(reservation.ticket);
  SharedFrameFn callback;
  std::shared_ptr<std::uint8_t> storage;
  std::uint8_t* memory = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (slot >= slot_count_ || states_[slot] != SlotState::Reserved) return;
    storage = std::move(reserved_[slot]);
    if (used_bytes > slot_bytes_) {
      // The writer never exceeds its destination; refuse instead of exposing a bad span.
      states_[slot] = SlotState::Free;
      free_.push_back(slot);
      return;
    }
    memory = storage.get();
    states_[slot] = SlotState::Held;
    max_held_ = std::max(max_held_, ++held_);
    callback = on_frame_;
  }
  published_.fetch_add(1, std::memory_order_relaxed);
  // The lease owns the slot from here; dropping it without a callback frees the slot.
  auto lease = std::shared_ptr<const FrameLease>(new FrameLease(
      shared_from_this(), slot, {memory, used_bytes}, metadata, std::move(storage)));
  if (callback) callback(SharedFrame{std::move(lease)});
}

void DirectLeaseSink::cancel(const detail::DirectReservation& reservation,
                             detail::CancelCause) noexcept {
  const auto slot = static_cast<std::uint32_t>(reservation.ticket);
  std::shared_ptr<std::uint8_t> storage;
  std::lock_guard lock(mutex_);
  if (slot >= slot_count_ || states_[slot] != SlotState::Reserved) return;
  storage = std::move(reserved_[slot]);
  states_[slot] = SlotState::Free;
  free_.push_back(slot);
}

void DirectLeaseSink::release(std::uint32_t slot) noexcept {
  std::lock_guard lock(mutex_);
  if (slot >= slot_count_ || states_[slot] != SlotState::Held) return;
  states_[slot] = SlotState::Free;
  --held_;
  free_.push_back(slot);
}

void DirectLeaseSink::note_slot_exhausted() noexcept {
  const auto dropped = exhausted_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (dropped == 1 || dropped % 100 == 0) {
    stage_log_warning("frame slot exhausted: dropped=%llu slots=%u",
                      static_cast<unsigned long long>(dropped), slot_count_);
  }
}

void DirectLeaseSink::note_fallback(detail::FallbackReason reason, const Frame&) noexcept {
  const auto count = fallbacks_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1)
    stage_log("frame layout uses the owned path: reason=%u", static_cast<unsigned>(reason));
}

detail::FallbackReason DirectLeaseSink::metadata_gate(const Frame&) const noexcept {
  // In-process metadata is not serialized, so no transport size limit applies.
  return detail::FallbackReason::None;
}

void DirectLeaseSink::report_capacity_failure(const Frame& metadata, std::size_t required,
                                              std::size_t available) noexcept {
  const auto count = capacity_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1 || count % 100 == 0) {
    report("frame does not fit its slot: frame=" + std::to_string(metadata.frame_id)
           + " required=" + std::to_string(required) + " available=" + std::to_string(available)
           + " count=" + std::to_string(count) + "; restart acquisition after a payload change");
  }
}

void DirectLeaseSink::report(std::string_view message) noexcept {
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
