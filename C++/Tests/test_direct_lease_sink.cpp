// Lease sink: exact reserve/publish/cancel/release accounting, leases outliving the sink
// owner, exhaustion without waiting, and consumer-provided storage with no sink allocation.
#include "DirectLeaseSink.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace d = onegen::detail;
using onegen::producer::DirectLeaseSink;
using onegen::producer::SharedFrame;

namespace {

d::DirectReservation reserve(DirectLeaseSink& sink) {
  d::DirectReservation reservation;
  assert(sink.reserve(reservation) == d::ReserveStatus::Reserved);
  assert(reservation.destination.size() == sink.slot_bytes());
  assert(reservation.owner);
  return reservation;
}

}  // namespace

int main() {
  const auto storage = [](std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
    return std::shared_ptr<std::uint8_t>(new std::uint8_t[bytes],
        std::default_delete<std::uint8_t[]>());
  };
  std::string error;
  assert(!DirectLeaseSink::create(0, 64, error, storage) && !error.empty());
  error.clear();
  assert(!DirectLeaseSink::create(3, 64, error, DirectLeaseSink::FrameStorage{}) && !error.empty());
  error.clear();
  assert(!DirectLeaseSink::create(3, 64, error, DirectLeaseSink::DestinationProvider{})
      && !error.empty());

  error.clear();
  auto sink = DirectLeaseSink::create(3, 64, error, storage);
  assert(sink && error.empty());
  std::vector<std::shared_ptr<const onegen::producer::FrameLease>> held;
  sink->set_frame_callback([&](const SharedFrame& frame) { held.push_back(frame.lease); });

  onegen::Frame metadata;
  metadata.frame_id = 7;
  metadata.width = 8;
  metadata.height = 4;
  for (int index = 0; index < 3; ++index) {
    auto reservation = reserve(*sink);
    std::memset(reservation.destination.data(), 0x40 + index, 32);
    sink->publish(reservation, metadata, 32);
  }
  assert(held.size() == 3);
  assert(held[1]->bytes().size() == 32 && held[1]->bytes()[0] == 0x41);
  assert(held[2]->metadata().frame_id == 7);
  assert(sink->counters().held == 3 && sink->counters().max_held == 3);

  // A full lease set refuses instead of waiting; the acquisition loop counts the drop.
  d::DirectReservation full;
  assert(sink->reserve(full) == d::ReserveStatus::Busy);
  sink->note_slot_exhausted();
  assert(sink->exhausted() == 1);

  // Releasing a lease from any owner returns exactly that slot.
  held.erase(held.begin());
  assert(sink->counters().held == 2);
  auto again = reserve(*sink);
  sink->cancel(again, d::CancelCause::Fallback);
  again = reserve(*sink);
  assert(sink->reserve(full) == d::ReserveStatus::Busy);
  sink->cancel(again, d::CancelCause::Timeout);
  // A second cancel or a stale release is harmless.
  sink->cancel(again, d::CancelCause::Timeout);

  // A publish larger than the slot is refused and frees the reservation.
  auto oversized = reserve(*sink);
  sink->publish(oversized, metadata, sink->slot_bytes() + 1);
  assert(held.size() == 2);
  assert(sink->counters().held == 2);
  sink->cancel(reserve(*sink), d::CancelCause::Capacity);

  // Stop refuses new reservations; published leases stay valid.
  sink->request_stop();
  d::DirectReservation stopped;
  assert(sink->reserve(stopped) == d::ReserveStatus::Stopped);
  sink->resume();

  // Reservations own the sink only until publish/cancel completes, as in the acquisition loop.
  for (auto* reservation : {&full, &again, &oversized, &stopped}) reservation->owner.reset();
  // Leases keep slot storage alive after the creator drops the sink.
  std::weak_ptr<DirectLeaseSink> weak = sink;
  const auto bytes = held[0]->bytes();
  sink.reset();
  assert(!weak.expired());
  assert(bytes[0] == 0x41);
  held.clear();
  assert(weak.expired());

  // Consumer-provided storage: the sink allocates nothing, each reservation writes into the
  // bytes the consumer handed out, and a lease keeps exactly those bytes alive.
  {
    int outstanding = 0;
    bool refuse = false;
    std::vector<std::uint8_t*> handed;
    auto storage = [&](std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
      if (refuse) return {};
      ++outstanding;
      auto* memory = new std::uint8_t[bytes];
      handed.push_back(memory);
      return std::shared_ptr<std::uint8_t>(memory, [&outstanding](std::uint8_t* released) {
        --outstanding;
        delete[] released;
      });
    };
    std::string storage_error;
    auto external = DirectLeaseSink::create(2, 64, storage_error, storage);
    assert(external && storage_error.empty());
    assert(outstanding == 0);
    std::vector<std::shared_ptr<const onegen::producer::FrameLease>> leases;
    external->set_frame_callback([&](const SharedFrame& frame) { leases.push_back(frame.lease); });

    auto first = reserve(*external);
    assert(outstanding == 1 && first.destination.data() == handed.back());
    std::memset(first.destination.data(), 0x5a, 16);
    external->publish(first, metadata, 16);
    assert(leases.size() == 1 && leases[0]->bytes().data() == handed.back());
    assert(leases[0]->bytes()[0] == 0x5a && outstanding == 1);

    // A cancelled reservation gives its bytes back at once.
    auto cancelled = reserve(*external);
    assert(outstanding == 2);
    external->cancel(cancelled, d::CancelCause::Timeout);
    assert(outstanding == 1);

    // No storage from the consumer reads as a full set and takes no slot.
    refuse = true;
    d::DirectReservation none;
    assert(external->reserve(none) == d::ReserveStatus::Busy);
    refuse = false;
    external->cancel(reserve(*external), d::CancelCause::Timeout);

    // The lease, not the sink, keeps the bytes alive.
    external.reset();
    assert(outstanding == 1 && leases[0]->bytes()[0] == 0x5a);
    leases.clear();
    assert(outstanding == 0);
  }

  // DestinationProvider reservation: opaque owner is freed immediately on cancel(), transferred
  // cleanly through FrameLease on publish(), and Refused stops the sink without returning Busy.
  {
    int aliveOwners = 0;
    bool busy = false;
    bool refuse = false;
    auto provider = [&](std::size_t bytes) -> DirectLeaseSink::DestinationReservation {
      if (refuse) return {DirectLeaseSink::DestinationStatus::Refused, {}, {}};
      if (busy) return {DirectLeaseSink::DestinationStatus::Busy, {}, {}};
      auto buf = std::shared_ptr<std::uint8_t>(new std::uint8_t[bytes],
          [&aliveOwners](std::uint8_t* ptr) {
            --aliveOwners;
            delete[] ptr;
          });
      ++aliveOwners;
      auto* raw = buf.get();
      return {DirectLeaseSink::DestinationStatus::Reserved,
              std::span<std::uint8_t>(raw, bytes), std::move(buf)};
    };
    std::string destError;
    auto sink2 = DirectLeaseSink::create(2, 64, destError, provider);
    assert(sink2 && destError.empty());
    std::shared_ptr<const onegen::producer::FrameLease> publishedLease;
    sink2->set_frame_callback([&](const SharedFrame& f) { publishedLease = f.lease; });

    auto cancelled = reserve(*sink2);
    assert(aliveOwners == 1);
    sink2->cancel(cancelled, d::CancelCause::Fallback);
    cancelled.owner.reset();
    assert(aliveOwners == 0 && sink2->counters().held == 0);

    auto res = reserve(*sink2);
    assert(aliveOwners == 1);
    std::memset(res.destination.data(), 0x3c, 24);
    sink2->publish(res, metadata, 24);
    res.owner.reset();
    assert(publishedLease && publishedLease->owner() && publishedLease->bytes().size() == 24);
    assert(publishedLease->bytes()[0] == 0x3c && aliveOwners == 1);
    publishedLease.reset();
    assert(aliveOwners == 0 && sink2->counters().held == 0);

    busy = true;
    d::DirectReservation busyRes;
    assert(sink2->reserve(busyRes) == d::ReserveStatus::Busy);
    busy = false;

    refuse = true;
    d::DirectReservation refusedRes;
    assert(sink2->reserve(refusedRes) == d::ReserveStatus::Refused);
    assert(sink2->reserve(refusedRes) == d::ReserveStatus::Stopped);
  }

  // Reused slot metadata retains heap capacity across steady-state publishes.
  {
    auto slotStorage = [&](std::size_t bytes) -> std::shared_ptr<std::uint8_t> {
      return std::shared_ptr<std::uint8_t>(new std::uint8_t[bytes]);
    };
    std::string slotError;
    auto sink3 = DirectLeaseSink::create(1, 32, slotError, slotStorage);
    assert(sink3 && slotError.empty());
    std::shared_ptr<const onegen::producer::FrameLease> lease;
    sink3->set_frame_callback([&](const SharedFrame& f) { lease = f.lease; });

    onegen::Frame longMeta;
    longMeta.width = 4;
    longMeta.height = 4;
    longMeta.stream_id = "stream-with-long-non-sso-identifier-0123456789abcdef";
    longMeta.components = {{1, "IntensityLongComponentName"}, {2, "RangeLongComponentName"}};

    auto first = reserve(*sink3);
    longMeta.frame_id = 1;
    sink3->publish(first, longMeta, 16);
    first.owner.reset();
    const char* streamPtr = lease->metadata().stream_id.data();
    const auto* compPtr = lease->metadata().components.data();
    assert(lease->metadata().frame_id == 1);
    lease.reset();

    auto second = reserve(*sink3);
    longMeta.frame_id = 2;
    sink3->publish(second, longMeta, 16);
    second.owner.reset();
    assert(lease->metadata().frame_id == 2);
    assert(lease->metadata().stream_id.data() == streamPtr);
    assert(lease->metadata().components.data() == compPtr);
    lease.reset();
  }

  std::cout << "Frame lease accounting and consumer storage passed.\n";
  return 0;
}
