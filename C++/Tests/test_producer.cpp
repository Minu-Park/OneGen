// In-process producer coordination against the synthetic GenTL producer: discovery with a
// failing producer, leases, checked writes, pooled Live with held leases, owned Live, Single,
// close/reopen and idle reset. Usage: test_producer <SyntheticGenTL.cti> <FailingInitProducer.cti>
#include "ProducerCoordinator.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using onegen::producer::Coordinator;
using onegen::producer::NodeWriteEffect;
using onegen::producer::NodeWriteStatus;
using onegen::producer::SharedFrame;

namespace {

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 10s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(5ms);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 3);
  const auto synthetic = std::filesystem::canonical(argv[1]);
  const auto failing = std::filesystem::canonical(argv[2]);

  // Consumer claims remain effective after integration with the in-process coordinator.
  {
    std::atomic<int> claims{0};
    Coordinator denied({}, 2s, [&](const onegen::InterfaceInfo&) -> std::shared_ptr<void> {
      ++claims;
      throw std::runtime_error("Synthetic board is owned by another package.");
    });
    assert(denied.set_producer_files({synthetic}));
    const auto blocked = denied.discover();
    assert(claims > 0 && blocked.devices.empty());
    assert(blocked.failures.size() == 1);
    assert(blocked.failures.front().find("Synthetic board is owned by another package.")
           != std::string::npos);
  }

  Coordinator coordinator({}, 2s);
  assert(coordinator.set_producer_files({synthetic, failing}));

  std::size_t progress_calls = 0;
  auto found = coordinator.discover([&](const auto&) { ++progress_calls; });
  assert(found.completed && found.total_producers == 2 && found.finished_producers == 2);
  assert(progress_calls >= 3);
  assert(found.devices.size() == 1);
  assert(found.devices.front().id == "SyntheticCam0");
  assert(found.failures.size() == 1);
  assert(found.failures.front().find("Producer inactive") != std::string::npos);
  assert(coordinator.interfaces().size() == 1);
  // A loaded producer refuses a new file list until it is reset.
  assert(!coordinator.set_producer_files({synthetic}));

  const auto& device = found.devices.front();
  auto lease = coordinator.open(device);
  assert(lease && lease->alive());
  bool refused = false;
  try { (void)coordinator.open(device); } catch (const std::runtime_error&) { refused = true; }
  assert(refused);

  const auto modules = lease->describe_features();
  assert(modules.size() == 5 && modules.front().first == "Remote Device");
  assert(lease->read_node(0, "Width").int_value > 0);
  assert(lease->payload_size() > 0);

  // Checked writes refuse a stale observation before the SDK setter.
  auto observed = lease->observe_node(0, "Width");
  auto stale = observed;
  stale.node.int_value += 1;
  const auto conflict = lease->write_node_checked(0, "Width", "int",
      std::to_string(observed.node.int_value), stale, true);
  assert(conflict.status == NodeWriteStatus::Conflict && conflict.effect == NodeWriteEffect::None);
  const auto written = lease->write_node_checked(0, "Width", "int",
      std::to_string(observed.node.int_value), observed, true);
  if (written.status != NodeWriteStatus::Success) std::cerr << "checked write: " << written.message << '\n';
  assert(written.status == NodeWriteStatus::Success && written.effect == NodeWriteEffect::Applied);
  assert(lease->write_node_checked(3, "Width", "int", "1", observed, false).status
         == NodeWriteStatus::Unavailable);

  // Pooled Live: the consumer holds up to four leases; the producer never waits for it.
  std::string error;
  const auto payload = lease->payload_size();
  assert(lease->create_frame_pool(6, payload, error) && error.empty());
  assert(!lease->create_frame_pool(6, payload, error));
  std::mutex held_mutex;
  std::deque<std::shared_ptr<const onegen::producer::FrameLease>> held;
  std::atomic<std::uint64_t> shared_frames{0}, owned_frames{0}, last_id{0};
  std::atomic<bool> ordered{true};
  lease->set_shared_frame_callback([&](const SharedFrame& frame) {
    assert(frame.lease && frame.lease->bytes().size() <= payload);
    const auto id = frame.lease->metadata().frame_id;
    if (id <= last_id.exchange(id)) ordered = false;
    std::lock_guard lock(held_mutex);
    held.push_back(frame.lease);
    if (held.size() > 4) held.pop_front();
    ++shared_frames;
  });
  lease->set_frame_callback([&](const onegen::Frame&) { ++owned_frames; });
  lease->live();
  assert(waitFor([&] { return shared_frames.load() >= 50; }));
  lease->stop();
  assert(ordered);
  assert(owned_frames == 0);
  const auto pool = lease->frame_pool();
  assert(pool);
  const auto counters = pool->counters();
  assert(counters.published == shared_frames.load());
  assert(counters.max_held <= 6);
  // Leases still held by the consumer stay valid and readable after stop.
  {
    std::lock_guard lock(held_mutex);
    assert(!held.empty() && !held.back()->bytes().empty());
    held.clear();
  }
  assert(pool->counters().held == 0);

  // Holding every slot makes acquisition drop and count frames instead of blocking.
  std::deque<std::shared_ptr<const onegen::producer::FrameLease>> all;
  lease->set_shared_frame_callback([&](const SharedFrame& frame) {
    std::lock_guard lock(held_mutex);
    all.push_back(frame.lease);
  });
  const auto exhausted_before = pool->exhausted();
  lease->live();
  assert(waitFor([&] { return pool->exhausted() > exhausted_before + 20; }));
  lease->stop();
  {
    std::lock_guard lock(held_mutex);
    assert(all.size() == 6);
    all.clear();
  }

  // Without a pool, Live delivers owned frames.
  lease->destroy_frame_pool();
  assert(!lease->frame_pool());
  lease->live();
  assert(waitFor([&] { return owned_frames.load() >= 10; }));
  lease->stop();

  // Single delivers exactly one owned frame and refuses while Live is active.
  std::size_t singles = 0;
  lease->grab_one([&](const onegen::Frame& frame) {
    assert(!frame.payload.empty());
    ++singles;
  });
  assert(singles == 1);
  lease->live();
  refused = false;
  try { lease->grab_one({}); } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  lease->stop();

  // Idle reset is refused while a lease exists; close frees the identity for reopen.
  assert(!coordinator.reset_idle_search());
  lease->close();
  assert(!lease->alive());
  refused = false;
  try { (void)lease->read_node(0, "Width"); } catch (const std::runtime_error&) { refused = true; }
  assert(refused);
  auto reopened = coordinator.open(device);
  assert(reopened->read_node(0, "Width").int_value > 0);
  reopened.reset();
  lease.reset();

  assert(coordinator.reset_idle_search());
  assert(coordinator.interfaces().empty());
  assert(coordinator.set_producer_files({synthetic}));
  found = coordinator.discover();
  assert(found.devices.size() == 1 && found.failures.empty());

  std::cout << "In-process producer coordination passed.\n";
  return 0;
}
