#include "WorkerFramePool.h"

#include "Internal/SharedMemoryRegion.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using onegen::detail::CancelCause;
using onegen::detail::DirectReservation;
using onegen::detail::FallbackReason;
using onegen::detail::ReserveStatus;
using onegen::worker::FramePoolAttach;
using onegen::worker::ReleaseResult;
using onegen::worker::SlotIdentity;
using onegen::worker::SlotRelease;
using onegen::worker::SlotState;
using onegen::worker::WorkerDirectSink;
using onegen::worker::WorkerFramePool;

struct PoolFixture {
  std::shared_ptr<onegen::SharedMemoryRegion> host;
  FramePoolAttach attach;
  std::shared_ptr<WorkerFramePool> pool;
};

PoolFixture make_pool(std::uint32_t slots = 2, std::uint64_t slot_bytes = 64,
                      std::uint64_t epoch = 11, std::uint64_t token = 22)
{
  PoolFixture fixture;
  std::string error;
  fixture.host = onegen::SharedMemoryRegion::create(
      static_cast<std::size_t>(slots * slot_bytes), error);
  assert(fixture.host);
  fixture.attach.region = fixture.host->name();
  fixture.attach.region_bytes = slots * slot_bytes;
  fixture.attach.slot_count = slots;
  fixture.attach.slot_bytes = slot_bytes;
  fixture.attach.epoch = epoch;
  fixture.attach.token = token;
  fixture.pool = WorkerFramePool::attach(7, fixture.attach, error);
  assert(fixture.pool);
  return fixture;
}

onegen::Frame make_frame(std::uint64_t frame_id)
{
  onegen::Frame frame;
  frame.frame_id = frame_id;
  frame.width = 4;
  frame.height = 3;
  frame.stride = 4;
  frame.payload_type = 1;
  frame.pixel_format = 0x01080001;
  frame.stream_id = "stream";
  return frame;
}

SlotRelease release_of(const WorkerFramePool& pool, std::uint32_t slot, std::uint64_t generation)
{
  return SlotRelease{pool.epoch(), pool.token(), slot, generation};
}

SlotRelease release_of(const WorkerFramePool& pool, const DirectReservation& reservation)
{
  return release_of(pool, static_cast<std::uint32_t>(reservation.ticket),
                    pool.slot(static_cast<std::uint32_t>(reservation.ticket)).generation);
}

SlotRelease release_of(const SlotIdentity& identity)
{
  return SlotRelease{identity.epoch, identity.token, identity.slot, identity.generation};
}

struct PublishState {
  WorkerDirectSink* sink = nullptr;
  int calls = 0;
  bool throw_send = false;
  bool release_inside = false;
  std::vector<SlotIdentity> identities;
  std::vector<std::vector<std::byte>> bodies;
};

void publish_callback(void* user, const SlotIdentity& identity,
                      std::span<const std::byte> body)
{
  auto& state = *static_cast<PublishState*>(user);
  ++state.calls;
  state.identities.push_back(identity);
  state.bodies.emplace_back(body.begin(), body.end());
  if (state.release_inside) {
    assert(state.sink);
    assert(state.sink->release(release_of(identity)) == ReleaseResult::Released);
  }
  if (state.throw_send) throw std::runtime_error("send failed");
}

struct ErrorState {
  int calls = 0;
  std::vector<std::string> messages;
};

void error_callback(void* user, std::uint32_t, std::string_view message)
{
  auto& state = *static_cast<ErrorState*>(user);
  ++state.calls;
  state.messages.emplace_back(message);
}

struct SinkFixture {
  std::unique_ptr<WorkerDirectSink> sink;
  PublishState published;
  ErrorState errors;
};

SinkFixture make_sink(const std::shared_ptr<WorkerFramePool>& pool)
{
  SinkFixture fixture;
  fixture.sink = std::make_unique<WorkerDirectSink>(
      &publish_callback, &fixture.published, &error_callback, &fixture.errors);
  fixture.published.sink = fixture.sink.get();
  std::string error;
  assert(fixture.sink->attach(pool, error));
  return fixture;
}

onegen::Frame frame_with_body_size(std::size_t target)
{
  onegen::Frame frame;
  const auto base = onegen::worker::encodedFrameReadySize(frame);
  assert(target >= base);
  auto remaining = target - base;
  std::uint64_t id = 1;
  while (remaining > 0) {
    if (remaining < 12) {
      assert(!frame.components.empty());
      auto& name = frame.components.back().name;
      const auto grow = std::min<std::size_t>(remaining, 4096 - name.size());
      assert(grow > 0);
      name.append(grow, 'x');
      remaining -= grow;
      continue;
    }
    const auto name_bytes = std::min<std::size_t>(4096, remaining - 12);
    frame.components.push_back(onegen::Frame::ComponentIdentity{id++, std::string(name_bytes, 'x')});
    remaining -= 12 + name_bytes;
  }
  assert(onegen::worker::encodedFrameReadySize(frame) == target);
  return frame;
}

void attach_validation()
{
  auto invalid = [](const FramePoolAttach& attach) {
    std::string error;
    const auto pool = WorkerFramePool::attach(1, attach, error);
    assert(!pool);
    assert(!error.empty());
  };

  FramePoolAttach base;
  base.region = "missing-region";
  base.region_bytes = 64;
  base.slot_count = 1;
  base.slot_bytes = 64;
  base.epoch = 1;
  base.token = 2;

  auto value = base;
  value.region.clear();
  invalid(value);

  value = base;
  value.region.assign(129, 'x');
  invalid(value);

  value = base;
  value.slot_count = 0;
  invalid(value);

  value = base;
  value.slot_count = 17;
  invalid(value);

  value = base;
  value.slot_bytes = 0;
  invalid(value);

  value = base;
  value.epoch = 0;
  invalid(value);

  value = base;
  value.token = 0;
  invalid(value);

  value = base;
  value.region_bytes = 63;
  invalid(value);

  value = base;
  value.slot_count = 2;
  value.slot_bytes = (std::numeric_limits<std::uint64_t>::max)();
  invalid(value);

  invalid(base);
}

void reserve_and_release_states()
{
  auto fixture = make_pool(2);
  auto sink = make_sink(fixture.pool);

  DirectReservation first;
  DirectReservation second;
  assert(sink.sink->reserve(first) == ReserveStatus::Reserved);
  assert(sink.sink->reserve(second) == ReserveStatus::Reserved);
  assert(first.destination.size() == 64);
  assert(second.destination.size() == 64);
  assert(fixture.pool->slot(0).state == SlotState::Reserved);
  assert(fixture.pool->slot(1).state == SlotState::Reserved);
  assert(sink.sink->release(release_of(*fixture.pool, first)) == ReleaseResult::Unknown);
  sink.sink->publish(first, make_frame(10), 12);
  sink.sink->publish(second, make_frame(11), 12);
  assert(fixture.pool->slot(0).state == SlotState::Ready);
  assert(fixture.pool->slot(1).state == SlotState::Ready);

  assert(sink.sink->release(release_of(*fixture.pool, first)) == ReleaseResult::Released);
  assert(sink.sink->release(release_of(*fixture.pool, first)) == ReleaseResult::Duplicate);
  auto stale = release_of(*fixture.pool, first);
  --stale.generation;
  assert(sink.sink->release(stale) == ReleaseResult::Stale);
  assert(sink.sink->release(SlotRelease{fixture.pool->epoch(), fixture.pool->token(), 99, 1})
         == ReleaseResult::Unknown);
  assert(sink.sink->release(SlotRelease{fixture.pool->epoch() + 1, fixture.pool->token(), 0, 1})
         == ReleaseResult::NoPool);
  assert(sink.sink->release(release_of(*fixture.pool, second)) == ReleaseResult::Released);

  DirectReservation third;
  assert(sink.sink->reserve(third) == ReserveStatus::Reserved);
  assert(sink.sink->release(release_of(*fixture.pool, third)) == ReleaseResult::Unknown);
  sink.sink->cancel(third, CancelCause::Fallback);
  assert(fixture.pool->slot(0).state == SlotState::Free);
}

void cancel_generation_and_sequence()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);

  DirectReservation first;
  assert(sink.sink->reserve(first) == ReserveStatus::Reserved);
  const auto generation = fixture.pool->slot(0).generation;
  sink.sink->cancel(first, CancelCause::Timeout);
  assert(fixture.pool->slot(0).state == SlotState::Free);
  assert(fixture.pool->slot(0).generation == generation);

  DirectReservation second;
  assert(sink.sink->reserve(second) == ReserveStatus::Reserved);
  assert(fixture.pool->slot(0).generation == generation + 1);
  sink.sink->publish(second, make_frame(100), 12);
  assert(fixture.pool->slot(0).state == SlotState::Ready);
  assert(sink.published.identities.size() == 1);
  assert(sink.published.identities[0].sequence == 1);
  assert(fixture.pool->release(release_of(sink.published.identities[0])) == ReleaseResult::Released);

  DirectReservation third;
  assert(sink.sink->reserve(third) == ReserveStatus::Reserved);
  sink.sink->publish(third, make_frame(101), 12);
  assert(sink.published.identities.size() == 2);
  assert(sink.published.identities[1].sequence == 2);
  assert(fixture.pool->sequence() == 2);
  assert(fixture.pool->release(release_of(sink.published.identities[1])) == ReleaseResult::Released);

  const auto body = sink.published.bodies[0];
  onegen::worker::Reader reader(body);
  const auto decoded = onegen::worker::readFrameReady(reader);
  assert(decoded.identity.sequence == 1);
  assert(decoded.metadata.frame_id == 100);
}

void publish_failures()
{
  {
    auto fixture = make_pool(1);
    auto sink = make_sink(fixture.pool);
    DirectReservation reservation;
    assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
    sink.published.throw_send = true;
    bool threw = false;
    try {
      sink.sink->publish(reservation, make_frame(200), 12);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
    assert(sink.published.calls == 1);
    assert(fixture.pool->slot(0).state == SlotState::Retired);
    assert(sink.sink->release(release_of(*fixture.pool, reservation)) == ReleaseResult::Released);
    assert(fixture.pool->slot(0).state == SlotState::Free);

    DirectReservation again;
    assert(sink.sink->reserve(again) == ReserveStatus::Reserved);
    sink.published.throw_send = false;
    sink.sink->publish(again, make_frame(201), 12);
    assert(fixture.pool->release(release_of(sink.published.identities.back()))
           == ReleaseResult::Released);
  }
  {
    auto fixture = make_pool(1);
    auto sink = make_sink(fixture.pool);
    DirectReservation reservation;
    assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
    auto oversized = make_frame(300);
    oversized.components.push_back(
        onegen::Frame::ComponentIdentity{1, std::string(70000, 'x')});
    bool threw = false;
    try {
      sink.sink->publish(reservation, oversized, 12);
    } catch (const std::length_error&) {
      threw = true;
    }
    assert(threw);
    assert(sink.published.calls == 0);
    assert(fixture.pool->slot(0).state == SlotState::Free);
    DirectReservation again;
    assert(sink.sink->reserve(again) == ReserveStatus::Reserved);
    sink.sink->cancel(again, CancelCause::Exception);
  }
}

void metadata_gate_boundary()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  const auto accepted = frame_with_body_size(onegen::worker::maximum_frame_ready_size);
  const auto rejected = frame_with_body_size(onegen::worker::maximum_frame_ready_size + 1);
  assert(onegen::worker::frameMetadataWithinLimits(accepted));
  assert(!onegen::worker::frameMetadataWithinLimits(rejected));
  assert(sink.sink->metadata_gate(accepted) == FallbackReason::None);
  assert(sink.sink->metadata_gate(rejected) == FallbackReason::MetadataTooLarge);
}

void release_inside_publish()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  sink.published.release_inside = true;
  DirectReservation reservation;
  assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
  sink.sink->publish(reservation, make_frame(400), 12);
  assert(sink.published.calls == 1);
  assert(fixture.pool->slot(0).state == SlotState::Free);
  DirectReservation again;
  assert(sink.sink->reserve(again) == ReserveStatus::Reserved);
  sink.sink->cancel(again, CancelCause::Fallback);
}

void capacity_failure_counts()
{
  auto fixture = make_pool(1, 64);
  auto sink = make_sink(fixture.pool);
  sink.sink->report_capacity_failure(make_frame(500), 128, 64);
  sink.sink->report_capacity_failure(make_frame(501), 256, 64);
  assert(sink.errors.calls == 2);
  assert(sink.errors.messages[0].find("session=7") != std::string::npos);
  assert(sink.errors.messages[0].find("frame=500") != std::string::npos);
  assert(sink.errors.messages[0].find("required=128") != std::string::npos);
  assert(sink.errors.messages[0].find("slot_bytes=64") != std::string::npos);
  assert(sink.errors.messages[0].find("consecutive=1") != std::string::npos);
  assert(sink.errors.messages[1].find("consecutive=2") != std::string::npos);
  assert(sink.errors.messages[1].find("total=2") != std::string::npos);

  DirectReservation reservation;
  assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
  sink.sink->publish(reservation, make_frame(502), 12);
  assert(fixture.pool->release(release_of(sink.published.identities.back()))
         == ReleaseResult::Released);
  sink.sink->report_capacity_failure(make_frame(503), 128, 64);
  assert(sink.errors.calls == 3);
  assert(sink.errors.messages[2].find("consecutive=1") != std::string::npos);
  assert(sink.errors.messages[2].find("total=3") != std::string::npos);
}

void sink_detach_keeps_reservation_mapping()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  DirectReservation reservation;
  assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
  const auto destination = reservation.destination;
  assert(!destination.empty());
  fixture.pool.reset();
  fixture.host.reset();
  sink.sink->detach();
  destination[0] = 0x5a;
  bool threw = false;
  try {
    sink.sink->publish(reservation, make_frame(602), 12);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(!threw);
  assert(sink.published.calls == 0);
}

void detach_and_reattach()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  DirectReservation reservation;
  assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
  const auto old_release = release_of(*fixture.pool, reservation);
  sink.sink->detach();
  assert(sink.sink->reserve(reservation) == ReserveStatus::NoPool);
  bool threw = false;
  try {
    sink.sink->publish(reservation, make_frame(600), 12);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(!threw);
  assert(sink.published.calls == 0);
  assert(fixture.pool->slot(reservation.ticket).state == SlotState::Free);
  assert(sink.sink->release(old_release) == ReleaseResult::NoPool);
  assert(!fixture.pool->active());

  auto second = make_pool(1, 64, 33, 44);
  std::string error;
  assert(sink.sink->attach(second.pool, error));
  DirectReservation current;
  assert(sink.sink->reserve(current) == ReserveStatus::Reserved);
  sink.sink->publish(current, make_frame(601), 12);
  assert(sink.published.identities.back().epoch == 33);
  assert(sink.published.identities.back().token == 44);
  assert(sink.sink->release(old_release) == ReleaseResult::NoPool);
  assert(second.pool->release(release_of(sink.published.identities.back()))
         == ReleaseResult::Released);

  DirectReservation in_flight;
  assert(sink.sink->reserve(in_flight) == ReserveStatus::Reserved);
  second.pool->detach();
  threw = false;
  try {
    sink.sink->publish(in_flight, make_frame(602), 12);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(!threw);
  assert(sink.published.calls == 1);
  assert(second.pool->slot(in_flight.ticket).state == SlotState::Free);
}

void detach_wakes_reserve()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  DirectReservation held;
  assert(sink.sink->reserve(held) == ReserveStatus::Reserved);

  std::atomic<bool> started{false};
  ReserveStatus status = ReserveStatus::Reserved;
  DirectReservation waiting;
  std::thread thread([&] {
    started.store(true, std::memory_order_release);
    status = sink.sink->reserve(waiting);
  });
  while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sink.sink->detach();
  thread.join();
  assert(status == ReserveStatus::NoPool);
  assert(fixture.pool->slot(0).state == SlotState::Reserved);
}

void reserve_wakes_on_release()
{
  auto fixture = make_pool(1);
  auto sink = make_sink(fixture.pool);
  DirectReservation held;
  assert(sink.sink->reserve(held) == ReserveStatus::Reserved);

  std::atomic<bool> started{false};
  ReserveStatus status = ReserveStatus::NoPool;
  DirectReservation waiting;
  std::thread thread([&] {
    started.store(true, std::memory_order_release);
    status = sink.sink->reserve(waiting);
  });
  while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  sink.sink->cancel(held, CancelCause::Fallback);
  thread.join();
  assert(status == ReserveStatus::Reserved);
  sink.sink->cancel(waiting, CancelCause::Fallback);
}

}  // namespace

void stop_discards_in_flight_publish()
{
  auto fixture = make_pool(2);
  auto sink = make_sink(fixture.pool);
  DirectReservation reservation;
  assert(sink.sink->reserve(reservation) == ReserveStatus::Reserved);
  sink.sink->request_stop();
  bool threw = false;
  try {
    sink.sink->publish(reservation, make_frame(600), 12);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(!threw);
  assert(sink.published.calls == 0);
  assert(fixture.pool->slot(reservation.ticket).state == SlotState::Free);
  DirectReservation stopped;
  assert(sink.sink->reserve(stopped) == ReserveStatus::Stopped);
  sink.sink->resume();
  DirectReservation resumed;
  assert(sink.sink->reserve(resumed) == ReserveStatus::Reserved);
  sink.sink->cancel(resumed, CancelCause::Exception);

  DirectReservation invalid;
  assert(sink.sink->reserve(invalid) == ReserveStatus::Reserved);
  sink.sink->cancel(invalid, CancelCause::Exception);
  bool rejected = false;
  try {
    sink.sink->publish(invalid, make_frame(601), 12);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
}

int main()
{
  attach_validation();
  reserve_and_release_states();
  cancel_generation_and_sequence();
  publish_failures();
  stop_discards_in_flight_publish();
  sink_detach_keeps_reservation_mapping();
  metadata_gate_boundary();
  release_inside_publish();
  capacity_failure_counts();
  detach_and_reattach();
  detach_wakes_reserve();
  reserve_wakes_on_release();
  return 0;
}
