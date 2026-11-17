// Lease bound: exact reserve/publish/cancel/release accounting, leases outliving the pool
// owner, exhaustion without waiting, and consumer-provided storage with no pool allocation.
#include "DirectLeaseSink.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

namespace d = onegen::detail;
using onegen::producer::DirectLeaseSink;
using onegen::producer::SharedFrame;

namespace {

d::DirectReservation reserve(DirectLeaseSink& pool) {
  d::DirectReservation reservation;
  assert(pool.reserve(reservation) == d::ReserveStatus::Reserved);
  assert(reservation.destination.size() == pool.slot_bytes());
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
  assert(!DirectLeaseSink::create(3, 64, error, {}) && !error.empty());

  error.clear();
  auto pool = DirectLeaseSink::create(3, 64, error, storage);
  assert(pool && error.empty());
  std::vector<std::shared_ptr<const onegen::producer::FrameLease>> held;
  pool->set_frame_callback([&](const SharedFrame& frame) { held.push_back(frame.lease); });

  onegen::Frame metadata;
  metadata.frame_id = 7;
  metadata.width = 8;
  metadata.height = 4;
  for (int index = 0; index < 3; ++index) {
    auto reservation = reserve(*pool);
    std::memset(reservation.destination.data(), 0x40 + index, 32);
    pool->publish(reservation, metadata, 32);
  }
  assert(held.size() == 3);
  assert(held[1]->bytes().size() == 32 && held[1]->bytes()[0] == 0x41);
  assert(held[2]->metadata().frame_id == 7);
  assert(pool->counters().held == 3 && pool->counters().max_held == 3);

  // A full pool refuses instead of waiting; the acquisition loop counts the drop.
  d::DirectReservation full;
  assert(pool->reserve(full) == d::ReserveStatus::Busy);
  pool->note_slot_exhausted();
  assert(pool->exhausted() == 1);

  // Releasing a lease from any owner returns exactly that slot.
  held.erase(held.begin());
  assert(pool->counters().held == 2);
  auto again = reserve(*pool);
  pool->cancel(again, d::CancelCause::Fallback);
  again = reserve(*pool);
  assert(pool->reserve(full) == d::ReserveStatus::Busy);
  pool->cancel(again, d::CancelCause::Timeout);
  // A second cancel or a stale release is harmless.
  pool->cancel(again, d::CancelCause::Timeout);

  // A publish larger than the slot is refused and frees the reservation.
  auto oversized = reserve(*pool);
  pool->publish(oversized, metadata, pool->slot_bytes() + 1);
  assert(held.size() == 2);
  assert(pool->counters().held == 2);
  pool->cancel(reserve(*pool), d::CancelCause::Capacity);

  // Stop refuses new reservations; published leases stay valid.
  pool->request_stop();
  d::DirectReservation stopped;
  assert(pool->reserve(stopped) == d::ReserveStatus::Stopped);
  pool->resume();

  // Reservations own the pool only until publish/cancel completes, as in the acquisition loop.
  for (auto* reservation : {&full, &again, &oversized, &stopped}) reservation->owner.reset();
  // Leases keep slot memory alive after the creator drops the pool.
  std::weak_ptr<DirectLeaseSink> weak = pool;
  const auto bytes = held[0]->bytes();
  pool.reset();
  assert(!weak.expired());
  assert(bytes[0] == 0x41);
  held.clear();
  assert(weak.expired());

  // Consumer-provided storage: the pool allocates nothing, each reservation writes into the
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

    // No storage from the consumer reads as a full pool and takes no slot.
    refuse = true;
    d::DirectReservation none;
    assert(external->reserve(none) == d::ReserveStatus::Busy);
    refuse = false;
    external->cancel(reserve(*external), d::CancelCause::Timeout);

    // The lease, not the pool, keeps the bytes alive.
    external.reset();
    assert(outstanding == 1 && leases[0]->bytes()[0] == 0x5a);
    leases.clear();
    assert(outstanding == 0);
  }

  std::cout << "Frame lease accounting and consumer storage passed.\n";
  return 0;
}
