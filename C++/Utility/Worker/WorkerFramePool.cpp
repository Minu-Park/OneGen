#include "WorkerFramePool.h"

#include "Internal/StageLog.h"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace onegen::worker {

namespace {

const char* fallbackReasonName(onegen::detail::FallbackReason reason) noexcept
{
  using onegen::detail::FallbackReason;
  switch (reason) {
    case FallbackReason::None: return "none";
    case FallbackReason::UnknownLayout: return "unknown-layout";
    case FallbackReason::HostUnsupported: return "host-unsupported";
    case FallbackReason::LayoutConstraint: return "layout-constraint";
    case FallbackReason::PackedRowAlignment: return "packed-row-alignment";
    case FallbackReason::GeometryUnrepresentable: return "geometry-unrepresentable";
    case FallbackReason::Planar: return "planar";
    case FallbackReason::MultiPart: return "multipart";
    case FallbackReason::ChunkData: return "chunk-data";
    case FallbackReason::NonImagePayload: return "non-image-payload";
    case FallbackReason::Jpeg: return "jpeg";
    case FallbackReason::Incomplete: return "incomplete";
    case FallbackReason::MetadataTooLarge: return "metadata-too-large";
  }
  return "unknown";
}

}  // namespace

WorkerFramePool::WorkerFramePool(std::uint32_t session,
                                 std::shared_ptr<SharedMemoryRegion> region,
                                 std::uint32_t slot_count, std::size_t slot_bytes,
                                 std::uint64_t epoch, std::uint64_t token)
    : region_(std::move(region)),
      slots_(slot_count),
      session_(session),
      slot_count_(slot_count),
      slot_bytes_(slot_bytes),
      epoch_(epoch),
      token_(token)
{
}

std::shared_ptr<WorkerFramePool> WorkerFramePool::attach(std::uint32_t session,
                                                          const FramePoolAttach& value,
                                                          std::string& error)
{
  if (value.region.empty() || value.region.size() > maximum_region_name_bytes) {
    error = "frame pool region name is invalid";
    return {};
  }
  if (value.slot_count == 0 || value.slot_count > maximum_slots) {
    error = "frame pool slot count is outside 1..16";
    return {};
  }
  if (value.slot_bytes == 0) {
    error = "frame pool slot size is zero";
    return {};
  }
  if (value.epoch == 0 || value.token == 0) {
    error = "frame pool epoch or token is zero";
    return {};
  }
  constexpr auto maximum_size = (std::numeric_limits<std::size_t>::max)();
  if (value.slot_bytes > maximum_size
      || value.slot_bytes > (std::numeric_limits<std::uint64_t>::max)() / value.slot_count) {
    error = "frame pool slot geometry overflows";
    return {};
  }
  const auto expected_bytes = value.slot_bytes * value.slot_count;
  if (value.region_bytes != expected_bytes) {
    error = "frame pool region size does not match its slots";
    return {};
  }
  auto region = SharedMemoryRegion::open(value.region, static_cast<std::size_t>(value.region_bytes),
                                         error);
  if (!region) return {};
  if (!region->writable()) {
    error = "frame pool region is not writable";
    return {};
  }
  return std::shared_ptr<WorkerFramePool>(new WorkerFramePool(
      session, std::move(region), value.slot_count, static_cast<std::size_t>(value.slot_bytes),
      value.epoch, value.token));
}

bool WorkerFramePool::valid_reservation(
    const onegen::detail::DirectReservation& reservation) const
{
  return reservation.ticket < slots_.size();
}

void WorkerFramePool::free_reserved_locked(Slot& slot) noexcept
{
  if (slot.state == SlotState::Reserved) slot.state = SlotState::Free;
}

ReserveStatus WorkerFramePool::reserve(onegen::detail::DirectReservation& reservation)
{
  std::lock_guard lock(mutex_);
  if (!active_) return ReserveStatus::NoPool;
  if (stopping_) return ReserveStatus::Stopped;

  auto* writable = region_->writable();
  if (!writable) return ReserveStatus::NoPool;
  for (std::uint32_t index = 0; index < slots_.size(); ++index) {
    auto& slot = slots_[index];
    if (slot.state != SlotState::Free) continue;
    slot.state = SlotState::Reserved;
    slot.used_bytes = 0;
    slot.frame_id = 0;
    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    reservation.destination = std::span<std::uint8_t>(
        writable + static_cast<std::size_t>(index) * slot_bytes_, slot_bytes_);
    reservation.ticket = index;
    return ReserveStatus::Reserved;
  }

  return ReserveStatus::Busy;
}

void WorkerFramePool::note_slot_exhausted() noexcept
{
  try {
    std::uint64_t dropped = 0;
    {
      std::lock_guard lock(mutex_);
      dropped = ++slot_exhausted_;
    }
    if (dropped == 1 || dropped % 100 == 0) {
      onegen::stage_log_warning(
          "direct-shm slot exhausted: session=%u dropped=%llu slots=%u",
          session_, static_cast<unsigned long long>(dropped), slot_count_);
    }
  } catch (...) {
  }
}

bool WorkerFramePool::prepare_publish(const onegen::detail::DirectReservation& reservation,
                                      const Frame& metadata, std::size_t used_bytes,
                                      SlotIdentity& identity, bool& stopped)
{
  std::lock_guard lock(mutex_);
  stopped = false;
  if (!valid_reservation(reservation)) return false;
  auto& slot = slots_[reservation.ticket];
  if (!active_ || stopping_) {
    stopped = true;
    free_reserved_locked(slot);
    return false;
  }
  if (slot.state != SlotState::Reserved) return false;

  ++sequence_;
  if (sequence_ == 0) ++sequence_;
  identity.session = session_;
  identity.epoch = epoch_;
  identity.token = token_;
  identity.slot = static_cast<std::uint32_t>(reservation.ticket);
  identity.generation = slot.generation;
  identity.sequence = sequence_;
  identity.used_bytes = used_bytes;
  slot.state = SlotState::Ready;
  slot.sequence = identity.sequence;
  slot.frame_id = metadata.frame_id;
  slot.used_bytes = used_bytes;
  return true;
}

void WorkerFramePool::fail_publish_encode(
    const onegen::detail::DirectReservation& reservation) noexcept
{
  try {
    std::lock_guard lock(mutex_);
    if (!valid_reservation(reservation)) return;
    auto& slot = slots_[reservation.ticket];
    if (slot.state == SlotState::Reserved || slot.state == SlotState::Ready) {
      slot.state = SlotState::Free;
    }
  } catch (...) {
  }
}

void WorkerFramePool::fail_publish_send(
    const onegen::detail::DirectReservation& reservation) noexcept
{
  try {
    std::lock_guard lock(mutex_);
    if (!valid_reservation(reservation)) return;
    auto& slot = slots_[reservation.ticket];
    if (slot.state == SlotState::Ready) slot.state = SlotState::Retired;
  } catch (...) {
  }
}

void WorkerFramePool::cancel(const onegen::detail::DirectReservation& reservation,
                             onegen::detail::CancelCause) noexcept
{
  try {
    std::lock_guard lock(mutex_);
    if (!valid_reservation(reservation)) return;
    auto& slot = slots_[reservation.ticket];
    if (slot.state == SlotState::Reserved) {
      slot.state = SlotState::Free;
    }
  } catch (...) {
  }
}

ReleaseResult WorkerFramePool::release(const SlotRelease& value) noexcept
{
  try {
    std::lock_guard lock(mutex_);
    if (!active_ || value.epoch != epoch_ || value.token != token_) return ReleaseResult::NoPool;
    if (value.slot >= slots_.size()) return ReleaseResult::Unknown;
    auto& slot = slots_[value.slot];
    if (value.generation != slot.generation) return ReleaseResult::Stale;
    if (slot.state == SlotState::Free) return ReleaseResult::Duplicate;
    if (slot.state == SlotState::Reserved) return ReleaseResult::Unknown;
    slot.state = SlotState::Free;
    return ReleaseResult::Released;
  } catch (...) {
    return ReleaseResult::NoPool;
  }
}

void WorkerFramePool::detach() noexcept
{
  try {
    std::lock_guard lock(mutex_);
    active_ = false;
  } catch (...) {
  }
}

void WorkerFramePool::request_stop() noexcept
{
  try {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  } catch (...) {
  }
}

void WorkerFramePool::resume() noexcept
{
  try {
    std::lock_guard lock(mutex_);
    stopping_ = false;
  } catch (...) {
  }
}

void WorkerFramePool::wake() noexcept
{
  request_stop();
}

bool WorkerFramePool::active() const noexcept
{
  try {
    std::lock_guard lock(mutex_);
    return active_;
  } catch (...) {
    return false;
  }
}

SlotSnapshot WorkerFramePool::slot(std::uint32_t index) const
{
  std::lock_guard lock(mutex_);
  if (index >= slots_.size()) return {};
  const auto& slot = slots_[index];
  return SlotSnapshot{slot.state, slot.generation, slot.sequence, slot.frame_id, slot.used_bytes};
}

std::uint64_t WorkerFramePool::sequence() const
{
  std::lock_guard lock(mutex_);
  return sequence_;
}

std::uint64_t WorkerFramePool::slot_exhausted() const noexcept
{
  try {
    std::lock_guard lock(mutex_);
    return slot_exhausted_;
  } catch (...) {
    return 0;
  }
}

WorkerDirectSink::WorkerDirectSink(PublishFn publish, void* publish_user, ErrorFn error,
                                   void* error_user) noexcept
    : publish_(publish),
      publish_user_(publish_user),
      error_(error),
      error_user_(error_user)
{
}

bool WorkerDirectSink::attach(std::shared_ptr<WorkerFramePool> pool, std::string& error)
{
  std::lock_guard lock(mutex_);
  if (pool_) {
    error = "direct frame pool is already attached";
    return false;
  }
  if (!pool) {
    error = "direct frame pool is missing";
    return false;
  }
  pool_ = std::move(pool);
  return true;
}

void WorkerDirectSink::detach() noexcept
{
  std::shared_ptr<WorkerFramePool> pool;
  try {
    std::lock_guard lock(mutex_);
    pool.swap(pool_);
  } catch (...) {
    return;
  }
  if (pool) pool->detach();
}

ReleaseResult WorkerDirectSink::release(const SlotRelease& value) noexcept
{
  try {
    auto pool = pool_snapshot();
    if (!pool) return ReleaseResult::NoPool;
    return pool->release(value);
  } catch (...) {
    return ReleaseResult::NoPool;
  }
}

void WorkerDirectSink::request_stop() noexcept
{
  try {
    auto pool = pool_snapshot();
    if (pool) pool->request_stop();
  } catch (...) {
  }
}

void WorkerDirectSink::resume() noexcept
{
  try {
    auto pool = pool_snapshot();
    if (pool) pool->resume();
  } catch (...) {
  }
}

ReserveStatus WorkerDirectSink::reserve(onegen::detail::DirectReservation& reservation)
{
  auto pool = pool_snapshot();
  if (!pool) return ReserveStatus::NoPool;
  const auto status = pool->reserve(reservation);
  if (status == ReserveStatus::Reserved) reservation.owner = pool;
  return status;
}

void WorkerDirectSink::publish(const onegen::detail::DirectReservation& reservation,
                               const Frame& metadata, std::size_t used_bytes)
{
  auto pool = reservation.owner
      ? std::static_pointer_cast<WorkerFramePool>(reservation.owner)
      : pool_snapshot();
  if (!pool) throw std::runtime_error("direct frame pool is not attached");

  SlotIdentity identity;
  bool stopped = false;
  if (!pool->prepare_publish(reservation, metadata, used_bytes, identity, stopped)) {
    if (stopped) return;
    throw std::runtime_error("direct frame publish was rejected");
  }

  std::vector<std::byte> body;
  try {
    Writer out;
    write(out, FrameReadyRecord{identity, &metadata});
    body = out.take();
  } catch (...) {
    pool->fail_publish_encode(reservation);
    throw;
  }

  try {
    if (publish_) publish_(publish_user_, identity, body);
  } catch (...) {
    pool->fail_publish_send(reservation);
    throw;
  }
  consecutive_capacity_failures_.store(0, std::memory_order_relaxed);
}

void WorkerDirectSink::cancel(const onegen::detail::DirectReservation& reservation,
                              onegen::detail::CancelCause cause) noexcept
{
  try {
    auto pool = reservation.owner
        ? std::static_pointer_cast<WorkerFramePool>(reservation.owner)
        : pool_snapshot();
    if (pool) pool->cancel(reservation, cause);
  } catch (...) {
  }
}

void WorkerDirectSink::set_drop_counter(
    std::shared_ptr<std::atomic<std::uint64_t>> counter) noexcept
{
  drop_counter_ = std::move(counter);
}

void WorkerDirectSink::note_slot_exhausted() noexcept
{
  if (drop_counter_) drop_counter_->fetch_add(1, std::memory_order_relaxed);
  try {
    if (auto pool = pool_snapshot()) pool->note_slot_exhausted();
  } catch (...) {
  }
}

void WorkerDirectSink::note_fallback(onegen::detail::FallbackReason reason,
                                     const Frame& frame) noexcept
{
  try {
    auto pool = pool_snapshot();
    onegen::stage_log_debug("direct-shm fallback: session=%u frame=%llu reason=%s",
                            pool ? pool->session() : 0U,
                            static_cast<unsigned long long>(frame.frame_id),
                            fallbackReasonName(reason));
  } catch (...) {
  }
}

onegen::detail::FallbackReason WorkerDirectSink::metadata_gate(
    const Frame& metadata) const noexcept
{
  return frameMetadataWithinLimits(metadata) ? onegen::detail::FallbackReason::None
                                             : onegen::detail::FallbackReason::MetadataTooLarge;
}

void WorkerDirectSink::report_capacity_failure(const Frame& metadata, std::size_t required,
                                               std::size_t available) noexcept
{
  try {
    auto pool = pool_snapshot();
    const auto session = pool ? pool->session() : 0U;
    const auto consecutive =
        consecutive_capacity_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto total = total_capacity_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::string message = "direct-shm capacity failure: session=" + std::to_string(session)
        + " frame=" + std::to_string(metadata.frame_id)
        + " required=" + std::to_string(required)
        + " slot_bytes=" + std::to_string(available)
        + " consecutive=" + std::to_string(consecutive)
        + " total=" + std::to_string(total);
    if (error_) {
      error_(error_user_, session, message);
    } else {
      onegen::stage_log_warning("%s", message.c_str());
    }
  } catch (...) {
  }
}

void WorkerDirectSink::wake() noexcept
{
  try {
    auto pool = pool_snapshot();
    if (pool) pool->wake();
  } catch (...) {
  }
}

std::shared_ptr<WorkerFramePool> WorkerDirectSink::pool() const
{
  return pool_snapshot();
}

std::shared_ptr<WorkerFramePool> WorkerDirectSink::pool_snapshot() const
{
  std::lock_guard lock(mutex_);
  return pool_;
}

}  // namespace onegen::worker
