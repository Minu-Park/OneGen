#include "OneGen.h"
#include "OneGenWorkerClient.h"
#include "OneGenWorkerMessages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
static void setEnvironment(const char* name, const char* value) {
  _putenv_s(name, value ? value : "");
}
#else
static void setEnvironment(const char* name, const char* value) {
  if (value) setenv(name, value, 1);
  else unsetenv(name);
}
#endif

namespace {

using namespace std::chrono_literals;
using onegen::Frame;
using onegen::worker::Connection;
using onegen::worker::Operation;
using onegen::worker::SharedFrame;

constexpr std::uint32_t session = 7;
constexpr std::size_t slot_bytes = 512;

struct ScenarioGuard {
  std::string previous_scenario;
  std::string previous_opt_in;
  bool had_scenario = false;
  bool had_opt_in = false;

  ScenarioGuard(std::string_view scenario, bool opt_in) {
    if (const char* value = std::getenv("ONEGEN_SYNTHETIC_SCENARIO")) {
      previous_scenario = value;
      had_scenario = true;
    }
    if (const char* value = std::getenv("ONEGEN_DIRECT_SHM_TEST")) {
      previous_opt_in = value;
      had_opt_in = true;
    }
    setEnvironment("ONEGEN_SYNTHETIC_SCENARIO", std::string(scenario).c_str());
    setEnvironment("ONEGEN_DIRECT_SHM_TEST", opt_in ? "1" : "0");
  }
  ~ScenarioGuard() {
    setEnvironment("ONEGEN_SYNTHETIC_SCENARIO", had_scenario ? previous_scenario.c_str() : nullptr);
    setEnvironment("ONEGEN_DIRECT_SHM_TEST", had_opt_in ? previous_opt_in.c_str() : nullptr);
  }
};

struct FormatSpec {
  const char* format;
  const char* width;
  const char* height;
  const char* xpad;
  const char* offset;
  const char* endianness;
  const char* trailing = "0";
};

struct FormatGuard {
  std::array<std::string, 7> previous;
  std::array<bool, 7> had{};
  FormatGuard(const FormatSpec& spec) {
    const std::array<const char*, 7> names = {
        "ONEGEN_SYNTHETIC_FORMAT", "ONEGEN_SYNTHETIC_WIDTH", "ONEGEN_SYNTHETIC_HEIGHT",
        "ONEGEN_SYNTHETIC_XPAD", "ONEGEN_SYNTHETIC_OFFSET", "ONEGEN_SYNTHETIC_ENDIAN",
        "ONEGEN_SYNTHETIC_TRAILING"};
    const std::array<const char*, 7> values = {
        spec.format, spec.width, spec.height, spec.xpad, spec.offset, spec.endianness,
        spec.trailing};
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (const char* value = std::getenv(names[index])) {
        previous[index] = value;
        had[index] = true;
      }
      setEnvironment(names[index], values[index]);
    }
  }
  ~FormatGuard() {
    const std::array<const char*, 7> names = {
        "ONEGEN_SYNTHETIC_FORMAT", "ONEGEN_SYNTHETIC_WIDTH", "ONEGEN_SYNTHETIC_HEIGHT",
        "ONEGEN_SYNTHETIC_XPAD", "ONEGEN_SYNTHETIC_OFFSET", "ONEGEN_SYNTHETIC_ENDIAN",
        "ONEGEN_SYNTHETIC_TRAILING"};
    for (std::size_t index = 0; index < names.size(); ++index)
      setEnvironment(names[index], had[index] ? previous[index].c_str() : nullptr);
  }
};

void require(bool condition, const std::string& message);

void checkFormatGuardRestoration() {
  const std::array<const char*, 7> names = {
      "ONEGEN_SYNTHETIC_FORMAT", "ONEGEN_SYNTHETIC_WIDTH", "ONEGEN_SYNTHETIC_HEIGHT",
      "ONEGEN_SYNTHETIC_XPAD", "ONEGEN_SYNTHETIC_OFFSET", "ONEGEN_SYNTHETIC_ENDIAN",
      "ONEGEN_SYNTHETIC_TRAILING"};
  for (const auto* name : names) setEnvironment(name, nullptr);
  {
    FormatGuard guard(FormatSpec{"0x01080001", "4", "2", "0", "0", "0", "8"});
    for (const auto* name : names) require(std::getenv(name) != nullptr, "guard did not set env");
  }
  for (const auto* name : names) require(std::getenv(name) == nullptr, "guard did not unset env");
  for (const auto* name : names) setEnvironment(name, "original");
  {
    FormatGuard guard(FormatSpec{"0x01080001", "4", "2", "0", "0", "0", "8"});
  }
  for (const auto* name : names) {
    const char* value = std::getenv(name);
    require(value && std::string_view(value) == "original", "guard did not restore env");
    setEnvironment(name, nullptr);
  }
}

struct Collected {
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::vector<SharedFrame> shared;
  std::vector<Frame> owned;
  std::vector<std::string> errors;

  bool waitShared(std::size_t count, std::chrono::milliseconds timeout = 10s) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, timeout, [&] { return shared.size() >= count; });
  }
  bool waitOwned(std::size_t count, std::chrono::milliseconds timeout = 10s) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, timeout, [&] { return owned.size() >= count; });
  }
  bool waitErrors(std::size_t count, std::chrono::milliseconds timeout = 10s) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, timeout, [&] { return errors.size() >= count; });
  }
  std::size_t sharedCount() const {
    std::lock_guard lock(mutex);
    return shared.size();
  }
  std::size_t ownedCount() const {
    std::lock_guard lock(mutex);
    return owned.size();
  }
  std::vector<std::string> takeErrors() {
    std::lock_guard lock(mutex);
    return errors;
  }
  void clear() {
    std::lock_guard lock(mutex);
    shared.clear();
    owned.clear();
    errors.clear();
  }
};

void collect(Connection& connection, Collected& events) {
  connection.setSharedFrameCallback([&events](const SharedFrame& frame) {
    {
      std::lock_guard lock(events.mutex);
      events.shared.push_back(frame);
    }
    events.ready.notify_all();
  }, session);
  connection.set_frame_callback([&events](const Frame& frame) {
    {
      std::lock_guard lock(events.mutex);
      events.owned.push_back(frame);
    }
    events.ready.notify_all();
  }, session);
  connection.set_error_callback([&events](std::string_view error) {
    {
      std::lock_guard lock(events.mutex);
      events.errors.emplace_back(error);
    }
    events.ready.notify_all();
  }, session);
}

void openCamera(Connection& connection) {
  onegen::worker::Writer payload;
  payload.string("SyntheticCam0");
  payload.string("SyntheticBoard");
  connection.request(Operation::Open, payload.take(), 30s, session);
}

Frame grabOwned(Connection& connection) {
  auto reply = connection.request(Operation::GrabOne, {}, 30s, session);
  onegen::worker::Reader reader(reply);
  return onegen::worker::readFrame(reader);
}

bool sameOptionalDouble(double left, double right);
bool sameScan3d(const std::optional<Frame::Scan3dGeometry>& left,
                const std::optional<Frame::Scan3dGeometry>& right);
bool sameComponents(const std::vector<Frame::ComponentIdentity>& left,
                    const std::vector<Frame::ComponentIdentity>& right);

bool equalMetadata(const Frame& left, const Frame& right) {
  if (left.width != right.width || left.height != right.height || left.stride != right.stride)
    return false;
  if (left.payload_type != right.payload_type || left.pixel_format != right.pixel_format
      || left.format_namespace != right.format_namespace
      || left.pixel_endianness != right.pixel_endianness)
    return false;
  if (left.frame_id != right.frame_id || left.timestamp != right.timestamp
      || left.timestamp_ns != right.timestamp_ns)
    return false;
  if (left.stream_index != right.stream_index || left.stream_id != right.stream_id)
    return false;
  if (left.buffer_info.capacity != right.buffer_info.capacity
      || left.buffer_info.size_filled != right.buffer_info.size_filled
      || left.buffer_info.data_size != right.buffer_info.data_size
      || left.buffer_info.image_offset != right.buffer_info.image_offset
      || left.buffer_info.y_padding != right.buffer_info.y_padding
      || left.buffer_info.delivered_image_height != right.buffer_info.delivered_image_height
      || left.buffer_info.incomplete != right.buffer_info.incomplete
      || left.buffer_info.image_present != right.buffer_info.image_present
      || left.buffer_info.contains_chunk_data != right.buffer_info.contains_chunk_data
      || left.buffer_info.diagnostic != right.buffer_info.diagnostic)
    return false;
  if (!sameComponents(left.components, right.components)) return false;
  return sameScan3d(left.scan3d_geometry, right.scan3d_geometry);
}

std::string describe(const Frame& frame) {
  return "frame=" + std::to_string(frame.frame_id) + " " + std::to_string(frame.width) + "x"
      + std::to_string(frame.height) + " stride=" + std::to_string(frame.stride)
      + " format=" + std::to_string(frame.pixel_format)
      + " payload=" + std::to_string(frame.payload_type)
      + " used=" + std::to_string(frame.payload.size());
}

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

Frame ownedReference(const std::filesystem::path& worker, const std::filesystem::path& cti,
                     std::string_view scenario) {
  ScenarioGuard guard(scenario, false);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  openCamera(connection);
  Frame frame = grabOwned(connection);
  connection.request(Operation::Close, {}, 10s, session);
  connection.close();
  return frame;
}

void checkOwnedParity(const std::filesystem::path& worker, const std::filesystem::path& cti,
                      std::string_view scenario, const Frame& reference) {
  ScenarioGuard guard(scenario, true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error), "pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(1), std::string(scenario) + ": no shared frame");
  SharedFrame shared;
  {
    std::lock_guard lock(events.mutex);
    shared = events.shared.front();
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  require(shared.lease != nullptr, "shared frame lease missing");
  require(equalMetadata(reference, shared.lease->metadata()),
          std::string(scenario) + ": shared metadata differs from owned: reference="
              + describe(reference) + " shared=" + describe(shared.lease->metadata()));
  require(shared.lease->bytes().size() == reference.payload.size(),
          std::string(scenario) + ": shared byte count differs");
  require(std::equal(shared.lease->bytes().begin(), shared.lease->bytes().end(),
                     reference.payload.begin()),
          std::string(scenario) + ": shared bytes differ");
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

Frame ownedFormatReference(const std::filesystem::path& worker, const std::filesystem::path& cti,
                           const FormatSpec& spec) {
  ScenarioGuard scenario("M0", false);
  FormatGuard format(spec);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  openCamera(connection);
  Frame frame = grabOwned(connection);
  connection.request(Operation::Close, {}, 10s, session);
  connection.close();
  return frame;
}

void checkFormatParity(const std::filesystem::path& worker, const std::filesystem::path& cti,
                       const FormatSpec& spec, const Frame& reference) {
  ScenarioGuard scenario("M0", true);
  FormatGuard format(spec);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  const auto slot_bytes = static_cast<std::uint64_t>(reference.stride * reference.height);
  require(slot_bytes != 0, std::string("format parity: empty geometry for ") + spec.format);
  require(connection.createFramePool(session, 3, slot_bytes, error),
          std::string("format parity pool attach failed for ") + spec.format + ": " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(1), std::string("format parity: no shared frame for ") + spec.format);
  SharedFrame shared;
  {
    std::lock_guard lock(events.mutex);
    shared = events.shared.front();
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  require(shared.lease != nullptr, "format parity: missing lease");
  require(equalMetadata(reference, shared.lease->metadata()),
          std::string("format parity metadata mismatch for ") + spec.format);
  require(shared.lease->bytes().size() == reference.payload.size(),
          std::string("format parity byte count mismatch for ") + spec.format);
  require(std::equal(shared.lease->bytes().begin(), shared.lease->bytes().end(),
                     reference.payload.begin()),
          std::string("format parity byte mismatch for ") + spec.format);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkFormatFallback(const std::filesystem::path& worker, const std::filesystem::path& cti,
                         const FormatSpec& spec, const Frame& reference) {
  ScenarioGuard scenario("M0", true);
  FormatGuard format(spec);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  const auto slot_bytes = static_cast<std::uint64_t>(
      std::max<std::size_t>(1, reference.payload.size()));
  require(connection.createFramePool(session, 3, slot_bytes, error),
          std::string("format fallback pool attach failed for ") + spec.format + ": " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitOwned(1), std::string("format fallback: no owned frame for ") + spec.format);
  require(events.sharedCount() == 0,
          std::string("format fallback: unexpected shared frame for ") + spec.format);
  Frame fallback;
  {
    std::lock_guard lock(events.mutex);
    fallback = events.owned.front();
  }
  require(equalMetadata(reference, fallback),
          std::string("format fallback metadata mismatch for ") + spec.format);
  require(fallback.payload == reference.payload,
          std::string("format fallback byte mismatch for ") + spec.format);
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkFormatCapacity(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  const FormatSpec spec{"0x02180014", "64", "4", "0", "0", "0"};
  ScenarioGuard scenario("M0", true);
  FormatGuard format(spec);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, 300, error),
          "RGB8 capacity pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitErrors(5), "RGB8 capacity: five errors were not reported");
  auto errors = events.takeErrors();
  require(events.sharedCount() == 0 && events.ownedCount() == 0,
          "RGB8 capacity: frame delivered instead of an error");
  std::set<int> consecutive;
  for (std::size_t index = 0; index < 5; ++index) {
    const auto position = errors[index].find(" consecutive=");
    require(position != std::string::npos, "RGB8 capacity: counter missing: " + errors[index]);
    consecutive.insert(std::stoi(errors[index].substr(position + 13)));
  }
  require(consecutive == std::set<int>({1, 2, 3, 4, 5}),
          "RGB8 capacity: consecutive counters were not 1..5");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkOptInOff(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M0", false);
  Connection connection(worker, cti);
  std::string error;
  require(!connection.createFramePool(session, 3, slot_bytes, error),
          "opt-in off unexpectedly accepted a pool");
  require(error.find("direct shared-memory test mode is disabled") != std::string::npos,
          "opt-in off error did not identify the disabled test mode: " + error);
  connection.close();
}

void checkFallbackScenario(const std::filesystem::path& worker, const std::filesystem::path& cti,
                           std::string_view scenario, std::size_t expected_frames,
                           bool expect_incomplete, bool expect_huge_component) {
  ScenarioGuard guard(scenario, true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error), "pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitOwned(expected_frames), std::string(scenario) + ": no owned fallback frame");
  require(events.sharedCount() == 0, std::string(scenario) + ": shared frame received for fallback");
  std::vector<Frame> frames;
  {
    std::lock_guard lock(events.mutex);
    frames = events.owned;
  }
  require(frames.size() >= expected_frames, std::string(scenario) + ": too few fallback frames");
  if (expect_incomplete) {
    require(frames.front().buffer_info.incomplete, std::string(scenario) + ": incomplete flag missing");
    require(!frames.front().buffer_info.diagnostic.empty(),
            std::string(scenario) + ": incomplete diagnostic missing");
  }
  if (expect_huge_component) {
    bool huge = false;
    for (const auto& component : frames.front().components)
      if (component.name.size() > 4096) huge = true;
    require(huge, std::string(scenario) + ": oversized component was not preserved");
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkM9(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M9", true);
  Connection connection(worker, cti);
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<Frame> frames;
  onegen::Frame mono10;
  std::size_t mono10_bytes = 0;
  bool has_mono10 = false;
  connection.setSharedFrameCallback([&](const SharedFrame& frame) {
    {
      std::lock_guard lock(mutex);
      if (!has_mono10 && frame.lease) {
        mono10 = frame.lease->metadata();
        mono10_bytes = frame.lease->bytes().size();
        has_mono10 = true;
      }
    }
    // Do not retain the lease: the M9 fallback sequence exceeds the three-slot
    // ring, and the direct path drops while every slot is still referenced.
    ready.notify_all();
  }, session);
  connection.set_frame_callback([&](const Frame& frame) {
    {
      std::lock_guard lock(mutex);
      frames.push_back(frame);
    }
    ready.notify_all();
  }, session);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error), "pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  {
    std::unique_lock lock(mutex);
    require(ready.wait_for(lock, 10s, [&] { return has_mono10 && frames.size() >= 3; }),
            "M9: three fallback frames or the Mono10 direct frame were not delivered");
  }
  bool chunk = false, multipart = false, jpeg = false;
  {
    std::lock_guard lock(mutex);
    for (const auto& frame : frames) {
      if (frame.buffer_info.contains_chunk_data) chunk = true;
      if (frame.payload_type == 10) multipart = true;
      if (frame.payload_type == 5) jpeg = true;
    }
  }
  require(chunk && multipart && jpeg, "M9: not all fallback variants were observed");
  require(mono10.pixel_format == 0x01100003,
          "M9: Mono10 was not handled by the direct path");
  require(mono10_bytes == 512, "M9: Mono10 byte count differs");
  connection.request(Operation::StopLive, {}, 10s, session);
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkM5(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M5", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error), "pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(2), "M5: two shared frames were not delivered");
  std::vector<SharedFrame> shared;
  {
    std::lock_guard lock(events.mutex);
    shared = events.shared;
  }
  std::set<bool> present;
  for (const auto& frame : shared) present.insert(frame.lease->metadata().buffer_info.image_present);
  require(present.count(false) && present.count(true), "M5: image_present values were not preserved");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkCapacity(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M0", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, 128, error), "small pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitErrors(5), "capacity: five errors were not reported");
  auto errors = events.takeErrors();
  require(errors.size() >= 5, "capacity: too few errors");
  require(events.sharedCount() == 0 && events.ownedCount() == 0,
          "capacity: frame delivered instead of an error");
  std::set<std::uint64_t> frames;
  std::set<int> consecutive;
  for (std::size_t index = 0; index < 5; ++index) {
    const auto& message = errors[index];
    require(message.find("direct-shm capacity failure: session=") == 0,
            "capacity: unexpected error: " + message);
    const auto frame_position = message.find(" frame=");
    const auto required_position = message.find(" required=");
    const auto consecutive_position = message.find(" consecutive=");
    require(frame_position != std::string::npos && required_position != std::string::npos
                && consecutive_position != std::string::npos,
            "capacity: incomplete error: " + message);
    const auto frame = std::stoull(message.substr(frame_position + 7,
        required_position - frame_position - 7));
    const auto value = std::stoi(message.substr(consecutive_position + 13));
    frames.insert(frame);
    consecutive.insert(value);
  }
  require(frames.size() == 5, "capacity: frame ids were not distinct");
  require(consecutive == std::set<int>({1, 2, 3, 4, 5}),
          "capacity: consecutive counters were not 1..5");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.destroyFramePool(session);

  require(connection.createFramePool(session, 3, slot_bytes, error),
          "replacement pool attach failed: " + error);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(1), "capacity: recovery frame was not published");
  require(events.ownedCount() == 0, "capacity: recovery produced an owned frame");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkCapacityThenNonCapacityError(const std::filesystem::path& worker,
                                       const std::filesystem::path& cti) {
  ScenarioGuard guard("M14", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, 128, error),
          "capacity/non-capacity pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitErrors(2), "capacity/non-capacity: two errors were not reported");
  auto errors = events.takeErrors();
  require(errors.size() >= 2, "capacity/non-capacity: too few errors");
  require(errors[0].find("direct-shm capacity failure: session=") == 0,
          "capacity/non-capacity: first error is not capacity: " + errors[0]);
  require(errors[1].find("direct-shm capacity failure") != 0,
          "capacity/non-capacity: later non-capacity error was suppressed: " + errors[1]);
  require(errors[1].find("grab: image offset exceeds delivered data") != std::string::npos,
          "capacity/non-capacity: later error identity changed: " + errors[1]);
  require(events.sharedCount() == 0 && events.ownedCount() == 0,
          "capacity/non-capacity: a frame was delivered");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
  std::cout << "T4 capacity/non-capacity error contrast passed\n";
}

void checkCapacityReset(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M12", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, 300, error), "alternating pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  {
    std::unique_lock lock(events.mutex);
    require(events.ready.wait_for(lock, 10s,
        [&] { return events.errors.size() >= 2 && events.shared.size() >= 2; }),
        "capacity reset: alternating publish/error sequence did not occur");
  }
  auto errors = events.takeErrors();
  require(errors.size() >= 2, "capacity reset: too few errors");
  for (std::size_t index = 0; index < 2; ++index) {
    const auto position = errors[index].find(" consecutive=");
    require(position != std::string::npos, "capacity reset: counter missing: " + errors[index]);
    require(std::stoi(errors[index].substr(position + 13)) == 1,
            "capacity reset: consecutive counter did not reset after publish: " + errors[index]);
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

void checkAllSlotsHeldStop(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M0", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error),
          "all-slots pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(3), "all-slots: three retained shared frames were not delivered");
  {
    std::unique_lock lock(events.mutex);
    require(!events.ready.wait_for(lock, 500ms, [&] { return events.shared.size() > 3; }),
            "all-slots: a fourth frame was delivered while all leases were retained");
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
  std::cout << "T4 all three slots held: reserve backpressure bounded and stop returned\n";
}

void checkOwnedMalformedSuppression(const std::filesystem::path& worker,
                                    const std::filesystem::path& cti) {
  ScenarioGuard guard("M13", false);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitOwned(1), "owned malformed: recovery frame missing");
  std::this_thread::sleep_for(300ms);
  {
    std::lock_guard lock(events.mutex);
    require(events.errors.size() == 1,
            "owned malformed: consecutive failures produced " + std::to_string(events.errors.size())
                + " errors instead of one");
    require(events.shared.empty(), "owned malformed: shared frame was delivered");
  }
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.close();
}

void checkLifecycle(const std::filesystem::path& worker, const std::filesystem::path& cti) {
  ScenarioGuard guard("M0", true);
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  require(connection.createFramePool(session, 3, slot_bytes, error), "pool attach failed: " + error);
  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(1), "lifecycle: first frame missing");
  SharedFrame retained;
  {
    std::lock_guard lock(events.mutex);
    retained = events.shared.front();
  }
  const auto retained_bytes = std::vector<std::uint8_t>(retained.lease->bytes().begin(),
                                                       retained.lease->bytes().end());
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();

  connection.request(Operation::Close, {}, 10s, session);
  std::this_thread::sleep_for(200ms);
  require(events.sharedCount() == 0 && events.ownedCount() == 0,
          "lifecycle: frame delivered after Close");
  require(std::equal(retained.lease->bytes().begin(), retained.lease->bytes().end(),
                     retained_bytes.begin()),
          "lifecycle: retained lease changed after Close");

  openCamera(connection);
  connection.request(Operation::StartLive, {}, 10s, session);
  require(events.waitShared(1), "lifecycle: reopened frame missing");
  SharedFrame reopened;
  {
    std::lock_guard lock(events.mutex);
    reopened = events.shared.front();
  }
  require(reopened.lease->descriptor().generation > retained.lease->descriptor().generation,
          "lifecycle: slot generation did not advance after reopen");
  require(std::equal(retained.lease->bytes().begin(), retained.lease->bytes().end(),
                     retained_bytes.begin()),
          "lifecycle: retained lease changed after reopen");
  connection.request(Operation::StopLive, {}, 10s, session);
  events.clear();
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
}

std::uint64_t checksum(std::span<const std::uint8_t> bytes) {
  std::uint64_t value = 1469598103934665603ULL;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

onegen::FeatureNode readFeature(Connection& connection, std::string_view name) {
  onegen::worker::Writer request;
  request.scalar(std::int32_t{0});
  request.string(name);
  auto payload = connection.request(Operation::NodeRead, request.take(), 10s, session);
  onegen::worker::Reader reader(payload);
  return onegen::worker::readFeatureNode(reader);
}

void writeFeature(Connection& connection, std::string_view name, std::string_view operation,
                  std::string_view value) {
  onegen::worker::Writer request;
  request.scalar(std::int32_t{0});
  request.string(name);
  request.string(operation);
  request.string(value);
  (void)connection.request(Operation::NodeWrite, request.take(), 10s, session);
}

bool sameOptionalDouble(double left, double right) {
  return (std::isnan(left) && std::isnan(right)) || left == right;
}

bool sameScan3d(const std::optional<Frame::Scan3dGeometry>& left,
                const std::optional<Frame::Scan3dGeometry>& right) {
  if (left.has_value() != right.has_value()) return false;
  if (!left) return true;
  return left->scale == right->scale && left->offset == right->offset
      && left->invalid_value == right->invalid_value
      && left->invalid_enabled == right->invalid_enabled
      && left->calibrated_axes == right->calibrated_axes
      && left->distance_unit == right->distance_unit
      && left->output_mode == right->output_mode
      && left->coordinate_system == right->coordinate_system
      && sameOptionalDouble(left->focal_length, right->focal_length)
      && sameOptionalDouble(left->baseline, right->baseline)
      && sameOptionalDouble(left->principal_u, right->principal_u)
      && sameOptionalDouble(left->principal_v, right->principal_v);
}

bool sameComponents(const std::vector<Frame::ComponentIdentity>& left,
                    const std::vector<Frame::ComponentIdentity>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index)
    if (left[index].id != right[index].id || left[index].name != right[index].name) return false;
  return true;
}

bool sameStableMetadata(const Frame& left, const Frame& right) {
  return left.width == right.width && left.height == right.height && left.stride == right.stride
      && left.payload_type == right.payload_type && left.pixel_format == right.pixel_format
      && left.format_namespace == right.format_namespace
      && left.pixel_endianness == right.pixel_endianness
      && left.stream_index == right.stream_index && left.stream_id == right.stream_id
      && left.buffer_info.capacity == right.buffer_info.capacity
      && left.buffer_info.size_filled == right.buffer_info.size_filled
      && left.buffer_info.data_size == right.buffer_info.data_size
      && left.buffer_info.image_offset == right.buffer_info.image_offset
      && left.buffer_info.y_padding == right.buffer_info.y_padding
      && left.buffer_info.delivered_image_height == right.buffer_info.delivered_image_height
      && left.buffer_info.incomplete == right.buffer_info.incomplete
      && left.buffer_info.image_present == right.buffer_info.image_present
      && left.buffer_info.contains_chunk_data == right.buffer_info.contains_chunk_data
      && left.buffer_info.diagnostic == right.buffer_info.diagnostic
      && sameComponents(left.components, right.components) && sameScan3d(left.scan3d_geometry,
                                                                         right.scan3d_geometry);
}

void checkMetadataComparator() {
  Frame left;
  Frame right;
  left.stream_id = right.stream_id = "stream";
  require(sameStableMetadata(left, right), "comparator rejected equal metadata");
  right.buffer_info.diagnostic = "diagnostic";
  require(!sameStableMetadata(left, right), "comparator accepted mutated metadata");
  right.buffer_info.diagnostic.clear();
  right.stream_index = 1;
  require(!sameStableMetadata(left, right), "comparator accepted mutated metadata");
  right.stream_index = 0;
  left.components = {{7, "Intensity"}};
  right.components = {{7, "Intensity"}};
  require(sameStableMetadata(left, right), "comparator rejected equal metadata");
  right.components[0].name = "Depth";
  require(!sameStableMetadata(left, right), "comparator accepted mutated metadata");
  left.components.clear();
  right.components.clear();
  Frame::Scan3dGeometry geometry;
  geometry.scale = {1.0, 2.0, 3.0};
  geometry.distance_unit = "mm";
  left.scan3d_geometry = geometry;
  right.scan3d_geometry = geometry;
  require(sameStableMetadata(left, right), "comparator rejected equal metadata");
  right.scan3d_geometry->scale[1] = 9.0;
  require(!sameStableMetadata(left, right), "comparator accepted mutated metadata");
}

bool lifecycleSucceeded(std::size_t worker_errors) {
  return worker_errors == 0;
}

enum class FormatProbeDisposition { Unavailable, Supported, Failed };
enum class FormatAttemptDisposition { SkippedUnavailable, Passed, Failed };

struct FormatProbeActions {
  std::function<bool()> available;
  std::function<void()> write;
  std::function<std::string()> read;
  std::function<void()> restore;
};

struct FormatProbeObservation {
  bool available = false;
  bool writeAttempted = false;
  bool readbackMatched = false;
  std::string error;
};

FormatProbeDisposition classifyFormatProbe(const FormatProbeObservation& observation,
                                           std::string& error) {
  if (!observation.available) return FormatProbeDisposition::Unavailable;
  if (!observation.error.empty()) {
    error = observation.error;
    return FormatProbeDisposition::Failed;
  }
  if (!observation.writeAttempted) {
    error = "available format write was not attempted";
    return FormatProbeDisposition::Failed;
  }
  if (!observation.readbackMatched) {
    error = "available format readback did not match";
    return FormatProbeDisposition::Failed;
  }
  return FormatProbeDisposition::Supported;
}

FormatAttemptDisposition classifyFormatAttempt(FormatProbeDisposition probe,
                                               bool acquisitionPassed) {
  if (probe == FormatProbeDisposition::Unavailable) return FormatAttemptDisposition::SkippedUnavailable;
  if (probe != FormatProbeDisposition::Supported || !acquisitionPassed)
    return FormatAttemptDisposition::Failed;
  return FormatAttemptDisposition::Passed;
}

FormatProbeDisposition runFormatProbeTransaction(const FormatProbeActions& actions,
                                                 std::string_view target,
                                                 std::string& error) {
  FormatProbeObservation observation;
  observation.available = actions.available();
  if (!observation.available) return FormatProbeDisposition::Unavailable;
  try {
    actions.write();
    observation.writeAttempted = true;
    observation.readbackMatched = actions.read() == target;
  } catch (const std::exception& failure) {
    observation.error = failure.what();
  } catch (...) {
    observation.error = "unknown format write/read failure";
  }
  try {
    actions.restore();
  } catch (const std::exception& failure) {
    if (!observation.error.empty()) observation.error += "; ";
    observation.error += std::string("restore failed: ") + failure.what();
  } catch (...) {
    if (!observation.error.empty()) observation.error += "; ";
    observation.error += "restore failed: unknown exception";
  }
  return classifyFormatProbe(observation, error);
}

void checkFormatProbePolicy() {
  std::string error;
  std::size_t writes = 0;
  std::size_t restores = 0;
  const auto actions = [&](bool available, bool writeThrows, bool readThrows,
                           std::string readback, bool restoreThrows) {
    writes = 0;
    restores = 0;
    return FormatProbeActions{
        [available] { return available; },
        [&, writeThrows] {
          ++writes;
          if (writeThrows) throw std::runtime_error("write transport fault");
        },
        [&, readThrows, readback = std::move(readback)]() -> std::string {
          if (readThrows) throw std::runtime_error("read transport fault");
          return readback;
        },
        [&, restoreThrows] {
          ++restores;
          if (restoreThrows) throw std::runtime_error("restore readback fault");
        }};
  };

  require(runFormatProbeTransaction(actions(false, false, false, "Target", false), "Target", error)
              == FormatProbeDisposition::Unavailable,
          "unavailable format was not skipped");
  require(writes == 0 && restores == 0, "unavailable preflight mutated or restored");
  require(error.empty(), "unavailable preflight produced an error");

  writes = 0;
  restores = 0;
  try {
    (void)runFormatProbeTransaction(
        FormatProbeActions{
            []() -> bool { throw std::runtime_error("observation transport fault"); },
            [&] { ++writes; },
            [] { return std::string("Target"); },
            [&] { ++restores; }},
        "Target", error);
    require(false, "observation fault was converted to an unsupported skip");
  } catch (const std::runtime_error& failure) {
    require(std::string_view(failure.what()) == "observation transport fault",
            "observation fault identity changed");
    require(writes == 0 && restores == 0,
            "observation fault mutated or restored without a write");
  }

  require(runFormatProbeTransaction(actions(true, false, false, "Target", false), "Target", error)
              == FormatProbeDisposition::Supported,
          "available format was not accepted");
  require(writes == 1 && restores == 1, "available format did not write/restore exactly once");

  require(runFormatProbeTransaction(actions(true, true, false, "Target", false), "Target", error)
              == FormatProbeDisposition::Failed,
          "write fault was classified as skip/success");
  require(error.find("write transport fault") != std::string::npos && restores == 1,
          "write fault lost its error or skipped restoration");

  require(runFormatProbeTransaction(actions(true, false, true, "Target", false), "Target", error)
              == FormatProbeDisposition::Failed,
          "read fault was classified as skip/success");
  require(error.find("read transport fault") != std::string::npos && restores == 1,
          "read fault lost its error or skipped restoration");

  require(runFormatProbeTransaction(actions(true, false, false, "Other", false), "Target", error)
              == FormatProbeDisposition::Failed,
          "readback mismatch was classified as skip/success");

  require(runFormatProbeTransaction(actions(true, false, false, "Target", true), "Target", error)
              == FormatProbeDisposition::Failed,
          "restore fault was classified as skip/success");
  require(error.find("restore failed") != std::string::npos,
          "restore fault lost its error");

  require(classifyFormatAttempt(FormatProbeDisposition::Unavailable, false)
              == FormatAttemptDisposition::SkippedUnavailable,
          "unavailable attempt was not skipped");
  require(classifyFormatAttempt(FormatProbeDisposition::Supported, false)
              == FormatAttemptDisposition::Failed,
          "supported format acquisition failure was not failed");
  require(classifyFormatAttempt(FormatProbeDisposition::Supported, true)
              == FormatAttemptDisposition::Passed,
          "supported format acquisition success was not passed");
  std::cout << "Format probe policy: unavailable skip, supported write/read, and unexpected faults passed\n";
}

void checkHardwareDispositionPolicy() {
  require(lifecycleSucceeded(0), "lifecycle policy rejected success");
  require(!lifecycleSucceeded(1), "lifecycle policy accepted errors");
  checkFormatProbePolicy();
}

struct LifecycleEvents {
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::size_t count = 0;
  SharedFrame retained;
  SharedFrame latest;
  std::uint64_t retained_checksum = 0;

  bool waitCount(std::size_t expected, std::chrono::milliseconds timeout = 30s) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, timeout, [&] { return count >= expected; });
  }
  std::size_t current() const {
    std::lock_guard lock(mutex);
    return count;
  }
};

void restoreFeatures(Connection& connection, std::string_view format, std::string_view pattern) {
  writeFeature(connection, "PixelFormat", "enum", format);
  writeFeature(connection, "TestPattern", "enum", pattern);
  const auto observed_format = readFeature(connection, "PixelFormat").enum_value;
  const auto observed_pattern = readFeature(connection, "TestPattern").enum_value;
  require(observed_format == format && observed_pattern == pattern,
          "feature restoration failed: format=" + observed_format + " pattern=" + observed_pattern);
}

void printFrameSummary(std::string_view label, const Frame& frame, std::size_t stored_bytes) {
  std::cout << "Hardware " << label << ": frame=" << frame.frame_id
            << " " << frame.width << "x" << frame.height
            << " stride=" << frame.stride
            << " format=" << frame.pixel_format
            << " namespace=" << frame.format_namespace
            << " payload=" << frame.payload_type
            << " endian=" << frame.pixel_endianness
            << " timestamp=" << frame.timestamp
            << " timestamp_ns=" << frame.timestamp_ns
            << " stored_bytes=" << stored_bytes
            << " capacity=" << frame.buffer_info.capacity
            << " size_filled=" << frame.buffer_info.size_filled
            << " data_size=" << frame.buffer_info.data_size
            << " image_offset=" << frame.buffer_info.image_offset
            << " y_padding=" << frame.buffer_info.y_padding
            << " delivered_height=" << frame.buffer_info.delivered_image_height
            << " incomplete=" << frame.buffer_info.incomplete
            << " image_present=" << frame.buffer_info.image_present
            << " diagnostic_bytes=" << frame.buffer_info.diagnostic.size()
            << " metadata_payload=" << frame.payload.size() << '\n';
}

struct InProcessOwnedActions {
  std::function<void()> select;
  std::function<Frame()> acquire;
  std::function<void()> stop;
  std::function<void()> restore;
  std::function<void()> close;
};

Frame runInProcessOwnedTransaction(const InProcessOwnedActions& actions) {
  try {
    actions.select();
    auto frame = actions.acquire();
    actions.restore();
    actions.close();
    return frame;
  } catch (...) {
    const auto original = std::current_exception();
    std::string cleanup_error;
    try {
      actions.stop();
    } catch (const std::exception& error) {
      cleanup_error = std::string("stop failed: ") + error.what();
    } catch (...) {
      cleanup_error = "stop failed: unknown exception";
    }
    try {
      actions.restore();
    } catch (const std::exception& error) {
      if (!cleanup_error.empty()) cleanup_error += "; ";
      cleanup_error += std::string("restore failed: ") + error.what();
    } catch (...) {
      if (!cleanup_error.empty()) cleanup_error += "; ";
      cleanup_error += "restore failed: unknown exception";
    }
    try { actions.close(); } catch (...) {}
    if (!cleanup_error.empty())
      throw std::runtime_error("in-process owned restoration failed: " + cleanup_error);
    std::rethrow_exception(original);
  }
}

void checkInProcessOwnedCleanupPolicy() {
  bool selected = false;
  bool stopped = false;
  bool restored = false;
  bool closed = false;
  const auto reset = [&] {
    selected = stopped = restored = closed = false;
  };
  const auto actions = [&](bool selectThrows, bool acquireThrows, bool stopThrows,
                           bool restoreThrows) {
    return InProcessOwnedActions{
        [&, selectThrows] {
          selected = true;
          if (selectThrows) throw std::runtime_error("setup fault after mutation");
        },
        [&, acquireThrows] {
          if (acquireThrows) throw std::runtime_error("acquisition fault");
          Frame frame;
          frame.width = 1;
          frame.height = 1;
          frame.stride = 1;
          return frame;
        },
        [&, stopThrows] {
          stopped = true;
          if (stopThrows) throw std::runtime_error("stop fault");
        },
        [&, restoreThrows] {
          restored = true;
          if (restoreThrows) throw std::runtime_error("restore readback fault");
        },
        [&] { closed = true; }};
  };

  reset();
  const auto success = runInProcessOwnedTransaction(actions(false, false, false, false));
  require(success.width == 1 && selected && !stopped && restored && closed,
          "in-process owned success transaction did not restore/close");

  reset();
  try {
    runInProcessOwnedTransaction(actions(false, true, false, false));
    require(false, "in-process owned acquisition fault did not propagate");
  } catch (const std::runtime_error& error) {
    require(std::string_view(error.what()) == "acquisition fault",
            "in-process owned acquisition fault identity changed");
    require(selected && stopped && restored && closed,
            "in-process owned acquisition fault skipped cleanup");
  }

  reset();
  try {
    runInProcessOwnedTransaction(actions(true, false, false, false));
    require(false, "in-process owned setup fault did not propagate");
  } catch (const std::runtime_error& error) {
    require(std::string_view(error.what()) == "setup fault after mutation",
            "in-process owned setup fault identity changed");
    require(selected && stopped && restored && closed,
            "in-process owned setup fault skipped cleanup");
  }

  reset();
  try {
    runInProcessOwnedTransaction(actions(false, true, false, true));
    require(false, "in-process owned restore fault did not surface");
  } catch (const std::runtime_error& error) {
    require(std::string(error.what()).find("in-process owned restoration failed: restore failed: restore readback fault")
                != std::string::npos,
            "in-process owned restore fault was not surfaced");
    require(selected && stopped && restored && closed,
            "in-process owned restore fault skipped cleanup");
  }

  reset();
  try {
    runInProcessOwnedTransaction(actions(false, true, true, false));
    require(false, "in-process owned stop fault did not surface");
  } catch (const std::runtime_error& error) {
    require(std::string(error.what()).find("stop failed: stop fault") != std::string::npos,
            "in-process owned stop fault was not surfaced");
    require(selected && stopped && restored && closed,
            "in-process owned stop fault skipped cleanup");
  }
  std::cout << "In-process owned cleanup: setup/acquisition/stop/restore faults passed\n";
}

Frame ownedInProcess(const std::filesystem::path& cti, std::string_view device,
                     std::string_view interface, std::string_view target,
                     std::string_view original_format, std::string_view original_pattern) {
  onegen::OneGen system;
  system.loadCti(cti);
  const auto devices = system.enumerate(10s);
  const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& item) {
    return item.id == device && item.interface_id == interface;
  });
  require(found != devices.end(), "in-process owned: camera not enumerated");
  auto& camera = system.addCamera();
  camera.open(*found);
  auto& features = camera.features(onegen::Camera::Module::RemoteDevice);
  const auto restore = [&] {
    features.find("PixelFormat").set_enum(original_format);
    features.find("TestPattern").set_enum(original_pattern);
    require(features.find("PixelFormat").get_enum() == original_format
                && features.find("TestPattern").get_enum() == original_pattern,
            "in-process owned restoration failed");
  };
  return runInProcessOwnedTransaction(InProcessOwnedActions{
      [&] {
        features.find("TestPattern").set_enum("Testimage1");
        features.find("PixelFormat").set_enum(target);
        const auto selected = features.find("PixelFormat");
        std::cout << "Hardware in-process selection: target=" << target
                  << " enum=" << selected.get_enum()
                  << " enum_value=0x" << std::hex
                  << static_cast<std::uint64_t>(selected.get_enum_value()) << std::dec << '\n';
      },
      [&] {
        auto frames = camera.single(1);
        require(!frames.empty(), "in-process owned returned no frame");
        std::cout << "Hardware in-process source: target=" << target
                  << " pfnc=0x" << std::hex << frames.front().pixel_format << std::dec << '\n';
        return frames.front();
      },
      [&] { camera.stop(); },
      restore,
      [&] { camera.close(); }});
}

struct BenchEvents {
  mutable std::mutex mutex;
  std::condition_variable ready;
  std::size_t validated = 0;
  std::size_t errors = 0;
  std::size_t correctness = 0;
  std::size_t bytes = 0;
  std::uint64_t expected_checksum = 0;
  Frame expected_metadata;
  std::uint64_t last_frame = 0;
  bool timed = false;
  bool timed_done = false;
  std::chrono::steady_clock::time_point started;
  std::uint64_t timed_elapsed_ms = 0;
  std::size_t timed_validated = 0;
  std::size_t timed_bytes = 0;
  std::size_t timed_errors = 0;
  std::size_t timed_correctness = 0;
  std::uint64_t timed_last_frame = 0;
  std::size_t requested = 0;
  std::size_t snapshot_validated = 0;
  std::set<std::uint64_t> frame_ids;

  bool waitValidated(std::size_t expected, std::chrono::milliseconds timeout = 120s) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, timeout, [&] { return validated >= expected; });
  }
};

int runBenchmark(const std::filesystem::path& worker, const std::filesystem::path& cti,
                 std::string_view device, std::string_view interface, bool direct,
                 std::size_t frames, std::uint64_t slot_bytes) {
  std::string original_pattern;
  std::string original_format;
  {
    setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
    Connection probe(worker, cti);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    probe.request(Operation::Open, open.take(), 30s, session);
    original_pattern = readFeature(probe, "TestPattern").enum_value;
    original_format = readFeature(probe, "PixelFormat").enum_value;
    probe.request(Operation::Close, {}, 10s, session);
    probe.close();
  }

  setEnvironment("ONEGEN_DIRECT_SHM_TEST", direct ? "1" : "0");
  Connection connection(worker, cti);
  BenchEvents events;
  connection.set_frame_callback([&](const Frame& frame) {
    {
      std::lock_guard lock(events.mutex);
      const auto observed = checksum(frame.payload);
      const bool metadata = sameStableMetadata(events.expected_metadata, frame);
      if (!events.expected_checksum) {
        events.expected_checksum = observed;
        events.expected_metadata = frame;
        events.expected_metadata.payload.clear();
      } else if (observed != events.expected_checksum || !metadata) {
        ++events.correctness;
      }
      events.bytes += frame.payload.size();
      ++events.validated;
      events.last_frame = frame.frame_id;
      events.frame_ids.insert(frame.frame_id);
      if (events.timed && !events.timed_done && events.validated >= events.requested) {
        events.timed_done = true;
        events.timed_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - events.started).count());
        events.timed_validated = events.validated;
        events.snapshot_validated = events.validated;
        events.timed_bytes = events.bytes;
        events.timed_errors = events.errors;
        events.timed_correctness = events.correctness;
        events.timed_last_frame = events.last_frame;
      }
    }
    events.ready.notify_all();
  }, session);
  connection.setSharedFrameCallback([&](const SharedFrame& shared) {
    {
      std::lock_guard lock(events.mutex);
      const auto observed = checksum(shared.lease->bytes());
      const bool metadata = sameStableMetadata(events.expected_metadata, shared.lease->metadata());
      if (!events.expected_checksum) {
        events.expected_checksum = observed;
        events.expected_metadata = shared.lease->metadata();
      } else if (observed != events.expected_checksum || !metadata) {
        ++events.correctness;
      }
      events.bytes += shared.lease->bytes().size();
      ++events.validated;
      events.last_frame = shared.lease->metadata().frame_id;
      events.frame_ids.insert(events.last_frame);
      if (events.timed && !events.timed_done && events.validated >= events.requested) {
        events.timed_done = true;
        events.timed_elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - events.started).count());
        events.timed_validated = events.validated;
        events.snapshot_validated = events.validated;
        events.timed_bytes = events.bytes;
        events.timed_errors = events.errors;
        events.timed_correctness = events.correctness;
        events.timed_last_frame = events.last_frame;
      }
    }
    events.ready.notify_all();
  }, session);
  connection.set_error_callback([&](std::string_view error) {
    {
      std::lock_guard lock(events.mutex);
      ++events.errors;
    }
    events.ready.notify_all();
    std::cout << "BENCH error: " << error << '\n';
  }, session);

  std::string error;
  if (direct) {
    require(connection.createFramePool(session, 3, slot_bytes, error),
            "benchmark pool attach failed: " + error);
  }
  onegen::worker::Writer open;
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);
  std::string observed_format;
  std::uint64_t elapsed_ms = 0;
  try {
    writeFeature(connection, "TestPattern", "enum", "Testimage1");
    observed_format = readFeature(connection, "PixelFormat").enum_value;
    require(observed_format == original_format,
            "benchmark PixelFormat changed unexpectedly: " + observed_format);
    {
      std::lock_guard lock(events.mutex);
      events.requested = frames;
      events.timed = true;
      events.timed_done = false;
      events.started = std::chrono::steady_clock::now();
    }
    connection.request(Operation::StartLive, {}, 60s, session);
    require(events.waitValidated(frames), "benchmark timed frames missing");
    connection.request(Operation::StopLive, {}, 30s, session);
    {
      std::lock_guard lock(events.mutex);
      require(events.timed_done, "benchmark timed snapshot was not captured");
      elapsed_ms = events.timed_elapsed_ms;
      events.validated = events.timed_validated;
      events.timed = false;
    }
    writeFeature(connection, "TestPattern", "enum", original_pattern);
    const auto restored = readFeature(connection, "TestPattern").enum_value;
    require(restored == original_pattern, "benchmark TestPattern restoration failed");
  } catch (...) {
    const auto original = std::current_exception();
    try { connection.request(Operation::StopLive, {}, 30s, session); } catch (...) {}
    std::string restore_error;
    try {
      writeFeature(connection, "TestPattern", "enum", original_pattern);
      const auto restored = readFeature(connection, "TestPattern").enum_value;
      if (restored != original_pattern) restore_error = "observed " + restored;
    } catch (const std::exception& error) {
      restore_error = error.what();
    } catch (...) {
      restore_error = "unknown restoration failure";
    }
    try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
    if (direct) connection.destroyFramePool(session);
    connection.close();
    if (!restore_error.empty())
      throw std::runtime_error("benchmark restoration failed: " + restore_error);
    std::rethrow_exception(original);
  }
  connection.request(Operation::Close, {}, 10s, session);
  if (direct) connection.destroyFramePool(session);
  connection.close();

  const auto final_frames = events.timed_validated;
  const auto final_bytes = events.timed_bytes;
  const auto final_errors = events.timed_errors;
  const auto final_correctness = events.timed_correctness;
  const auto final_last_frame = events.timed_last_frame;
  std::cout << "BENCH side=" << (direct ? "direct" : "owned")
            << " frames=" << final_frames
            << " requested=" << frames
            << " elapsed_ms=" << elapsed_ms
            << " bytes=" << final_bytes
            << " checksum=0x" << std::hex << events.expected_checksum << std::dec
            << " errors=" << final_errors
            << " correctness=" << final_correctness
            << " snapshot=" << events.snapshot_validated
            << " last_frame=" << final_last_frame
            << " format=" << observed_format
            << " restored_pattern=" << original_pattern << '\n';
  require(final_frames == frames, "benchmark validated frame count is short");
  require(final_errors == 0, "benchmark observed worker errors");
  require(final_correctness == 0, "benchmark observed output correctness failures");
  return 0;
}

FormatProbeDisposition probeFormatSupport(const std::filesystem::path& worker,
                                          const std::filesystem::path& cti,
                                          std::string_view device, std::string_view interface,
                                          std::string_view target,
                                          std::string_view original_format,
                                          std::string_view original_pattern) {
  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
  Connection connection(worker, cti);
  onegen::worker::Writer open;
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);

  std::string probe_error;
  FormatProbeDisposition disposition = FormatProbeDisposition::Failed;
  try {
    const auto actions = FormatProbeActions{
        [&] {
          const auto format = readFeature(connection, "PixelFormat");
          if (format.kind != onegen::NodeKind::Enumeration || !format.readable
              || !format.observation_error.empty())
            throw std::runtime_error("PixelFormat enumeration observation is unavailable: "
                                     + format.observation_error);
          return std::find(format.enum_entries.begin(), format.enum_entries.end(), target)
              != format.enum_entries.end();
        },
        [&] { writeFeature(connection, "PixelFormat", "enum", target); },
        [&] { return readFeature(connection, "PixelFormat").enum_value; },
        [&] { restoreFeatures(connection, original_format, original_pattern); }};
    disposition = runFormatProbeTransaction(actions, target, probe_error);
  } catch (...) {
    try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
    connection.close();
    throw;
  }
  try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
  connection.close();

  if (disposition == FormatProbeDisposition::Failed)
    throw std::runtime_error("format preflight failed for " + std::string(target) + ": " + probe_error);
  return disposition;
}

int runHardwareFormats(const std::filesystem::path& worker, const std::filesystem::path& cti,
                       std::string_view device, std::string_view interface) {
  std::string original_format;
  std::string original_pattern;
  std::vector<std::string> targets;
  {
    setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
    Connection connection(worker, cti);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    connection.request(Operation::Open, open.take(), 30s, session);
    const auto format = readFeature(connection, "PixelFormat");
    const auto pattern = readFeature(connection, "TestPattern");
    require(format.kind == onegen::NodeKind::Enumeration && format.writable,
            "hardware formats: PixelFormat is not writable");
    require(pattern.kind == onegen::NodeKind::Enumeration && pattern.writable,
            "hardware formats: TestPattern is not writable");
    original_format = format.enum_value;
    original_pattern = pattern.enum_value;
    for (const auto& entry : format.enum_entries) {
      if (entry != original_format
          && (entry == "Mono8" || entry == "RGB8" || entry == "BayerRG10" || entry == "BayerRG12"))
        targets.push_back(entry);
    }
    std::cout << "Hardware formats: original=" << original_format
              << " pattern=" << original_pattern << " entries=";
    for (const auto& entry : format.enum_entries) std::cout << '[' << entry << ']';
    std::cout << " targets=";
    for (const auto& entry : targets) std::cout << '[' << entry << ']';
    std::cout << '\n';
    if (targets.empty()) {
      std::cout << "Hardware formats: no additional supported format; skipped\n";
      connection.request(Operation::Close, {}, 10s, session);
      connection.close();
      return 0;
    }
    connection.request(Operation::Close, {}, 10s, session);
    connection.close();
  }

  for (const auto& target : targets) {
    const auto probe = probeFormatSupport(worker, cti, device, interface, target,
                                           original_format, original_pattern);
    if (probe == FormatProbeDisposition::Unavailable) {
      std::cout << "Hardware format unsupported: " << target << '\n';
      continue;
    }
    require(probe == FormatProbeDisposition::Supported,
            "hardware format probe did not produce a supported disposition: " + target);
    try {
      Frame owned;
      if (target == "RGB8") {
        owned = ownedInProcess(cti, device, interface, target, original_format, original_pattern);
      } else {
        Connection connection(worker, cti);
        Collected events;
        collect(connection, events);
        onegen::worker::Writer open;
        open.string(device);
        open.string(interface);
        connection.request(Operation::Open, open.take(), 30s, session);
        try {
          writeFeature(connection, "TestPattern", "enum", "Testimage1");
          writeFeature(connection, "PixelFormat", "enum", target);
          owned = grabOwned(connection);
          restoreFeatures(connection, original_format, original_pattern);
        } catch (...) {
          const auto original = std::current_exception();
          std::string restore_error;
          try {
            restoreFeatures(connection, original_format, original_pattern);
          } catch (const std::exception& error) {
            restore_error = error.what();
          } catch (...) {
            restore_error = "unknown restoration failure";
          }
          try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
          connection.close();
          if (!restore_error.empty())
            throw std::runtime_error("hardware format owned restoration failed: " + restore_error);
          std::rethrow_exception(original);
        }
        connection.request(Operation::Close, {}, 10s, session);
        connection.close();
      }
      require(owned.timestamp != 0 && owned.timestamp_ns != 0,
              "hardware formats owned timestamps are missing: " + target);
      printFrameSummary("owned/" + target, owned, owned.payload.size());

      setEnvironment("ONEGEN_DIRECT_SHM_TEST", "1");
      Connection connection(worker, cti);
      Collected events;
      collect(connection, events);
      std::string error;
      const auto required = static_cast<std::uint64_t>(owned.stride * owned.height);
      require(connection.createFramePool(session, 3, required, error),
              "hardware formats pool attach failed: " + error);
      onegen::worker::Writer open;
      open.string(device);
      open.string(interface);
      connection.request(Operation::Open, open.take(), 30s, session);
      SharedFrame shared;
      try {
        writeFeature(connection, "TestPattern", "enum", "Testimage1");
        writeFeature(connection, "PixelFormat", "enum", target);
        connection.request(Operation::StartLive, {}, 30s, session);
        require(events.waitShared(1, 30s), "hardware formats direct frame missing: " + target);
        connection.request(Operation::StopLive, {}, 30s, session);
        {
          std::lock_guard lock(events.mutex);
          shared = events.shared.front();
        }
        restoreFeatures(connection, original_format, original_pattern);
      } catch (...) {
        const auto original = std::current_exception();
        try { connection.request(Operation::StopLive, {}, 30s, session); } catch (...) {}
        std::string restore_error;
        try {
          restoreFeatures(connection, original_format, original_pattern);
        } catch (const std::exception& error) {
          restore_error = error.what();
        } catch (...) {
          restore_error = "unknown restoration failure";
        }
        try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
        connection.destroyFramePool(session);
        connection.close();
        if (!restore_error.empty())
          throw std::runtime_error("hardware format direct restoration failed: " + restore_error);
        std::rethrow_exception(original);
      }
      connection.request(Operation::Close, {}, 10s, session);
      connection.destroyFramePool(session);
      connection.close();
      require(shared.lease != nullptr, "hardware formats missing lease");
      require(sameStableMetadata(owned, shared.lease->metadata()),
              "hardware formats stable metadata differs: " + target);
      require(owned.payload.size() == shared.lease->bytes().size(),
              "hardware formats byte count differs: " + target);
      require(std::equal(owned.payload.begin(), owned.payload.end(), shared.lease->bytes().begin()),
              "hardware formats static bytes differ: " + target);
      require(shared.lease->metadata().timestamp != 0
                  && shared.lease->metadata().timestamp_ns != 0,
              "hardware formats direct timestamps are missing: " + target);
      printFrameSummary("direct/" + target, shared.lease->metadata(),
                        shared.lease->descriptor().used_bytes);
      require(classifyFormatAttempt(probe, true) == FormatAttemptDisposition::Passed,
              "supported format acquisition success was classified as failure");
      std::cout << "Hardware format parity: " << target
                << " checksum=0x" << std::hex << checksum(owned.payload) << std::dec << '\n';
    } catch (...) {
      require(classifyFormatAttempt(probe, false) == FormatAttemptDisposition::Failed,
              "supported format acquisition failure was classified as a skip");
      std::cerr << "Hardware format acquisition failure after supported preflight: " << target << '\n';
      throw;
    }
  }

  {
    setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
    Connection restore(worker, cti);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    restore.request(Operation::Open, open.take(), 30s, session);
    const auto format = readFeature(restore, "PixelFormat").enum_value;
    const auto pattern = readFeature(restore, "TestPattern").enum_value;
    require(format == original_format && pattern == original_pattern,
            "hardware formats final restoration failed: format=" + format + " pattern=" + pattern);
    std::cout << "Hardware formats final restoration verified: format=" << format
              << " pattern=" << pattern << '\n';
    restore.request(Operation::Close, {}, 10s, session);
    restore.close();
  }
  return 0;
}

int runHardwareLifecycle(const std::filesystem::path& worker, const std::filesystem::path& cti,
                         std::string_view device, std::string_view interface) {
  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
  Frame reference;
  {
    Connection connection(worker, cti);
    Collected events;
    collect(connection, events);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    connection.request(Operation::Open, open.take(), 30s, session);
    reference = grabOwned(connection);
    connection.request(Operation::Close, {}, 10s, session);
    connection.close();
  }
  const auto required = static_cast<std::uint64_t>(reference.stride * reference.height);
  require(required != 0, "lifecycle: empty reference geometry");

  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "1");
  Connection connection(worker, cti);
  LifecycleEvents events;
  std::atomic<std::size_t> deliveries{0};
  std::atomic<std::size_t> worker_errors{0};
  connection.setSharedFrameCallback([&](const SharedFrame& frame) {
    {
      std::lock_guard lock(events.mutex);
      ++events.count;
      ++deliveries;
      events.latest = frame;
      if (!events.retained.lease) {
        events.retained = frame;
        events.retained_checksum = checksum(frame.lease->bytes());
      }
    }
    events.ready.notify_all();
  }, session);
  connection.set_error_callback([&](std::string_view error) {
    ++worker_errors;
    std::cout << "Lifecycle worker error: " << error << '\n';
  }, session);
  std::string error;
  require(connection.createFramePool(session, 3, required, error),
          "lifecycle pool attach failed: " + error);
  onegen::worker::Writer open;
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);

  connection.request(Operation::StartLive, {}, 30s, session);
  require(events.waitCount(1), "lifecycle: first retained frame missing");
  {
    std::lock_guard lock(events.mutex);
    printFrameSummary("lifecycle-retained", events.retained.lease->metadata(),
                      events.retained.lease->descriptor().used_bytes);
    std::cout << "Lifecycle retained: slot=" << events.retained.lease->descriptor().slot
              << " generation=" << events.retained.lease->descriptor().generation
              << " sequence=" << events.retained.lease->descriptor().sequence
              << " used_bytes=" << events.retained.lease->descriptor().used_bytes
              << " checksum=0x" << std::hex << events.retained_checksum << std::dec << '\n';
  }
  require(events.waitCount(4), "lifecycle: further frames were not delivered");
  const auto baseline_checksum = events.retained_checksum;
  require(checksum(events.retained.lease->bytes()) == baseline_checksum,
          "lifecycle: retained bytes changed while more frames were delivered");
  {
    std::lock_guard lock(events.mutex);
    require(events.latest.lease && sameStableMetadata(events.retained.lease->metadata(),
                                                       events.latest.lease->metadata()),
            "lifecycle: stable metadata changed across further frames");
  }
  connection.request(Operation::StopLive, {}, 30s, session);
  const auto after_stop = deliveries.load();
  std::this_thread::sleep_for(500ms);
  const auto after_stop_wait = deliveries.load();
  std::cout << "Lifecycle StopLive deliveries: after_response=" << after_stop
            << " after_500ms=" << after_stop_wait << '\n';
  require(after_stop_wait == after_stop, "lifecycle: delivery after StopLive response");
  require(checksum(events.retained.lease->bytes()) == baseline_checksum,
          "lifecycle: retained bytes changed after StopLive");

  connection.request(Operation::Close, {}, 10s, session);
  const auto after_close = deliveries.load();
  std::this_thread::sleep_for(500ms);
  const auto after_close_wait = deliveries.load();
  std::cout << "Lifecycle Close deliveries: after_response=" << after_close
            << " after_500ms=" << after_close_wait << '\n';
  require(after_close_wait == after_close, "lifecycle: delivery after Close response");
  require(checksum(events.retained.lease->bytes()) == baseline_checksum,
          "lifecycle: retained bytes changed after Close");

  open = onegen::worker::Writer{};
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);
  connection.request(Operation::StartLive, {}, 30s, session);
  const auto resumed_target = after_close + 1;
  require(events.waitCount(resumed_target), "lifecycle: no frame after reopen");
  SharedFrame resumed;
  {
    std::lock_guard lock(events.mutex);
    resumed = events.latest;
  }
  require(resumed.lease->descriptor().sequence > events.retained.lease->descriptor().sequence,
          "lifecycle: sequence did not advance after reopen");
  require(checksum(events.retained.lease->bytes()) == baseline_checksum,
          "lifecycle: retained bytes changed after reopen");
  connection.request(Operation::StopLive, {}, 30s, session);
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();
  require(lifecycleSucceeded(worker_errors.load()), "lifecycle observed worker errors: "
              + std::to_string(worker_errors.load()));
  require(events.retained.lease->metadata().timestamp != 0
              && events.retained.lease->metadata().timestamp_ns != 0,
          "lifecycle retained timestamps are missing");
  std::cout << "Lifecycle errors=" << worker_errors.load()
            << "; retained lease stayed readable; Stop/Close produced no late delivery; "
               "reopen advanced sequence; releases remained valid\n";
  return 0;
}

int runHardwarePattern(const std::filesystem::path& worker, const std::filesystem::path& cti,
                       std::string_view device, std::string_view interface) {
  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
  Frame owned;
  std::string original_pattern;
  {
    Connection connection(worker, cti);
    Collected events;
    collect(connection, events);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    connection.request(Operation::Open, open.take(), 30s, session);
    const auto pattern = readFeature(connection, "TestPattern");
    if (pattern.kind != onegen::NodeKind::Enumeration || !pattern.writable
        || std::find(pattern.enum_entries.begin(), pattern.enum_entries.end(), "Testimage1")
            == pattern.enum_entries.end()) {
      std::cout << "Hardware pattern: unavailable or not writable; same-frame parity skipped\n";
      connection.request(Operation::Close, {}, 10s, session);
      connection.close();
      return 0;
    }
    original_pattern = pattern.enum_value;
    const auto restore = [&] {
      writeFeature(connection, "TestPattern", "enum", original_pattern);
      const auto observed = readFeature(connection, "TestPattern").enum_value;
      require(observed == original_pattern,
              "hardware pattern restoration failed: expected " + original_pattern + " observed "
                  + observed);
      std::cout << "Hardware pattern restored and verified: " << observed << '\n';
    };
    try {
      writeFeature(connection, "TestPattern", "enum", "Testimage1");
      owned = grabOwned(connection);
      restore();
    } catch (...) {
      const auto original = std::current_exception();
      std::string restore_error;
      try {
        restore();
      } catch (const std::exception& error) {
        restore_error = error.what();
      } catch (...) {
        restore_error = "unknown restoration failure";
      }
      if (!restore_error.empty())
        throw std::runtime_error("hardware pattern owned restoration failed: " + restore_error);
      std::rethrow_exception(original);
    }
    connection.request(Operation::Close, {}, 10s, session);
    connection.close();
  }

  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "1");
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  const auto required = static_cast<std::uint64_t>(owned.stride * owned.height);
  require(connection.createFramePool(session, 3, required, error),
          "hardware pattern pool attach failed: " + error);
  onegen::worker::Writer open;
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);
  const auto restore = [&] {
    writeFeature(connection, "TestPattern", "enum", original_pattern);
    const auto observed = readFeature(connection, "TestPattern").enum_value;
    require(observed == original_pattern,
            "hardware pattern restoration failed: expected " + original_pattern + " observed "
                + observed);
    std::cout << "Hardware pattern restored and verified after direct: " << observed << '\n';
  };
  SharedFrame shared;
  try {
    writeFeature(connection, "TestPattern", "enum", "Testimage1");
    connection.request(Operation::StartLive, {}, 30s, session);
    require(events.waitShared(1, 30s), "hardware pattern direct frame was not delivered");
    connection.request(Operation::StopLive, {}, 30s, session);
    {
      std::lock_guard lock(events.mutex);
      shared = events.shared.front();
    }
    restore();
  } catch (...) {
    const auto original = std::current_exception();
    try { connection.request(Operation::StopLive, {}, 30s, session); } catch (...) {}
    std::string restore_error;
    try {
      restore();
    } catch (const std::exception& error) {
      restore_error = error.what();
    } catch (...) {
      restore_error = "unknown restoration failure";
    }
    try { connection.request(Operation::Close, {}, 10s, session); } catch (...) {}
    connection.destroyFramePool(session);
    connection.close();
    if (!restore_error.empty())
      throw std::runtime_error("hardware pattern direct restoration failed: " + restore_error);
    std::rethrow_exception(original);
  }
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);
  connection.close();

  require(sameStableMetadata(owned, shared.lease->metadata()),
          "hardware pattern stable metadata differs");
  require(owned.payload.size() == shared.lease->bytes().size(),
          "hardware pattern byte count differs");
  require(std::equal(owned.payload.begin(), owned.payload.end(), shared.lease->bytes().begin()),
          "hardware pattern static test image bytes differ");
  std::cout << "Hardware pattern parity: same static Testimage1 bytes and stable metadata matched; "
               "owned_checksum=0x"
            << std::hex << checksum(owned.payload) << " direct_checksum=0x"
            << checksum(shared.lease->bytes()) << std::dec << '\n';
  return 0;
}

int runHardware(const std::filesystem::path& worker, const std::filesystem::path& cti,
                std::string_view device, std::string_view interface) {
  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "0");
  Frame owned;
  {
    Connection connection(worker, cti);
    Collected events;
    collect(connection, events);
    onegen::worker::Writer open;
    open.string(device);
    open.string(interface);
    connection.request(Operation::Open, open.take(), 30s, session);
    for (const auto name : {"TestImageSelector", "TestPattern", "PixelFormat"}) {
      try {
        onegen::worker::Writer request;
        request.scalar(std::int32_t{0});
        request.string(name);
        auto payload = connection.request(Operation::NodeRead, request.take(), 10s, session);
        onegen::worker::Reader reader(payload);
        const auto node = onegen::worker::readFeatureNode(reader);
        std::cout << "Hardware feature: " << name << " kind=" << static_cast<int>(node.kind)
                  << " value=" << node.enum_value << " entries=";
        for (const auto& entry : node.enum_entries) std::cout << '[' << entry << ']';
        std::cout << '\n';
      } catch (const std::exception& error) {
        std::cout << "Hardware feature: " << name << " unavailable: " << error.what() << '\n';
      }
    }
    owned = grabOwned(connection);
    connection.request(Operation::Close, {}, 10s, session);
    connection.close();
  }
  require(owned.width && owned.height && owned.stride, "hardware owned frame has empty geometry");
  require(owned.payload.size() == owned.stride * owned.height,
          "hardware owned payload size does not match stride*height");
  const auto printMetadata = [](std::string_view side, const Frame& frame, std::size_t stored_bytes) {
    std::cout << "Hardware " << side
              << ": frame=" << frame.frame_id
              << " " << frame.width << "x" << frame.height
              << " stride=" << frame.stride
              << " format=" << frame.pixel_format
              << " namespace=" << frame.format_namespace
              << " payload=" << frame.payload_type
              << " endian=" << frame.pixel_endianness
              << " timestamp=" << frame.timestamp
              << " timestamp_ns=" << frame.timestamp_ns
              << " stream=" << frame.stream_id
              << " index=" << frame.stream_index
              << " stored_bytes=" << stored_bytes
              << " capacity=" << frame.buffer_info.capacity
              << " size_filled=" << frame.buffer_info.size_filled
              << " data_size=" << frame.buffer_info.data_size
              << " image_offset=" << frame.buffer_info.image_offset
              << " y_padding=" << frame.buffer_info.y_padding
              << " delivered_height=" << frame.buffer_info.delivered_image_height
              << " incomplete=" << frame.buffer_info.incomplete
              << " image_present=" << frame.buffer_info.image_present
              << " chunk=" << frame.buffer_info.contains_chunk_data
              << " diagnostic_bytes=" << frame.buffer_info.diagnostic.size()
              << " components=" << frame.components.size()
              << " metadata_payload=" << frame.payload.size() << '\n';
  };
  printMetadata("owned", owned, owned.payload.size());
  std::cout << "Hardware owned checksum=0x" << std::hex << checksum(owned.payload) << std::dec << '\n';

  setEnvironment("ONEGEN_DIRECT_SHM_TEST", "1");
  Connection connection(worker, cti);
  Collected events;
  collect(connection, events);
  std::string error;
  const auto required = static_cast<std::uint64_t>(owned.stride * owned.height);
  require(required <= (std::numeric_limits<std::uint64_t>::max)() / 3,
          "hardware payload overflows the pool budget");
  require(connection.createFramePool(session, 3, required, error),
          "hardware pool attach failed: " + error);
  onegen::worker::Writer open;
  open.string(device);
  open.string(interface);
  connection.request(Operation::Open, open.take(), 30s, session);
  connection.request(Operation::StartLive, {}, 30s, session);
  require(events.waitShared(1, 30s), "hardware direct frame was not delivered");
  SharedFrame shared;
  {
    std::lock_guard lock(events.mutex);
    shared = events.shared.front();
  }
  connection.request(Operation::StopLive, {}, 30s, session);
  connection.request(Operation::Close, {}, 10s, session);
  connection.destroyFramePool(session);

  const auto& metadata = shared.lease->metadata();
  require(metadata.width == owned.width && metadata.height == owned.height
              && metadata.stride == owned.stride,
          "hardware direct geometry differs from owned");
  require(metadata.pixel_format == owned.pixel_format
              && metadata.payload_type == owned.payload_type,
          "hardware direct format differs from owned");
  require(metadata.stream_id == owned.stream_id, "hardware direct stream identity differs");
  require(shared.lease->bytes().size() == required,
          "hardware direct byte count differs from stride*height");
  printMetadata("direct", metadata, shared.lease->descriptor().used_bytes);
  std::cout << "Hardware direct descriptor: slot=" << shared.lease->descriptor().slot
            << " generation=" << shared.lease->descriptor().generation
            << " sequence=" << shared.lease->descriptor().sequence
            << " used_bytes=" << shared.lease->descriptor().used_bytes
            << " lease_bytes=" << shared.lease->bytes().size()
            << " checksum=0x" << std::hex << checksum(shared.lease->bytes()) << std::dec
            << " (different acquisition; byte equality is not required)\n";
  connection.close();
  std::cout << "Hardware correctness: owned and direct acquisitions passed; no settings were written.\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc == 6 && std::string_view(argv[1]) == "--hardware")
    return runHardware(std::filesystem::weakly_canonical(argv[2]),
                       std::filesystem::weakly_canonical(argv[3]), argv[4], argv[5]);
  if (argc == 6 && std::string_view(argv[1]) == "--hardware-pattern")
    return runHardwarePattern(std::filesystem::weakly_canonical(argv[2]),
                              std::filesystem::weakly_canonical(argv[3]), argv[4], argv[5]);
  if (argc == 6 && std::string_view(argv[1]) == "--hardware-lifecycle")
    return runHardwareLifecycle(std::filesystem::weakly_canonical(argv[2]),
                                std::filesystem::weakly_canonical(argv[3]), argv[4], argv[5]);
  if (argc == 6 && std::string_view(argv[1]) == "--hardware-formats")
    return runHardwareFormats(std::filesystem::weakly_canonical(argv[2]),
                              std::filesystem::weakly_canonical(argv[3]), argv[4], argv[5]);
  if (argc == 9 && std::string_view(argv[1]) == "--benchmark") {
    const auto side = std::string_view(argv[6]);
    require(side == "owned" || side == "direct", "benchmark side must be owned or direct");
    return runBenchmark(std::filesystem::weakly_canonical(argv[2]),
                        std::filesystem::weakly_canonical(argv[3]), argv[4], argv[5],
                        side == "direct", static_cast<std::size_t>(std::stoull(argv[7])),
                        std::stoull(argv[8]));
  }
  if (argc != 3) {
    std::cerr << "usage: test_worker_direct_shm <OneGenWorker> <SyntheticGenTL.cti>\n"
                 "       test_worker_direct_shm --hardware <OneGenWorker> <cti> <device> <interface>\n";
    return 2;
  }
  const std::filesystem::path worker = std::filesystem::weakly_canonical(argv[1]);
  const std::filesystem::path cti = std::filesystem::weakly_canonical(argv[2]);

  checkFormatGuardRestoration();
  checkMetadataComparator();
  checkHardwareDispositionPolicy();
  checkInProcessOwnedCleanupPolicy();
  const auto m0 = ownedReference(worker, cti, "M0");
  checkOptInOff(worker, cti);
  checkOwnedParity(worker, cti, "M0", m0);
  std::cout << "T1 owned parity: M0 bytes and metadata match\n";

  for (const auto scenario : {"M1", "M2", "M3", "M4", "M6", "M7"}) {
    const auto reference = ownedReference(worker, cti, scenario);
    checkOwnedParity(worker, cti, scenario, reference);
  }
  checkM5(worker, cti);
  std::cout << "T4 shared metadata suite: M1-M7 passed\n";

  checkFallbackScenario(worker, cti, "M8", 1, true, false);
  checkM9(worker, cti);
  checkFallbackScenario(worker, cti, "M10", 1, false, true);
  std::cout << "T4 fallback suite: M8-M10 passed\n";

  const FormatSpec eligible[] = {
      {"0x01100003", "64", "4", "2", "0", "2"},
      {"0x010A0046", "64", "4", "0", "0", "0"},
      {"0x02180014", "5", "4", "1", "16", "0"},
      {"0x02180014", "5", "4", "1", "16", "0", "64"},
      {"0x01100005", "16", "4", "0", "0", "0"},
      {"0x01100011", "8", "4", "0", "0", "0"},
      {"0x02100032", "4", "4", "0", "0", "0"},
  };
  for (const auto& spec : eligible) {
    const auto reference = ownedFormatReference(worker, cti, spec);
    checkFormatParity(worker, cti, spec, reference);
  }
  std::cout << "T4 layout classes: seven direct parity cases passed\n";

  const FormatSpec fallback[] = {
      {"0x02100032", "3", "4", "0", "0", "0"},
      {"0x021000A5", "3", "4", "0", "0", "0"},
      {"0x02180021", "4", "4", "0", "0", "0"},
      {"0x81080001", "4", "4", "0", "0", "0"},
      {"0x0110002A", "3", "4", "0", "0", "0"},
  };
  for (const auto& spec : fallback) {
    const auto reference = ownedFormatReference(worker, cti, spec);
    checkFormatFallback(worker, cti, spec, reference);
  }
  std::cout << "T4 layout fallbacks: YUV/BiColor/planar/unknown/legacy passed\n";
  checkFormatCapacity(worker, cti);
  std::cout << "T4 RGB8 capacity: five distinct errors passed\n";

  checkCapacity(worker, cti);
  checkAllSlotsHeldStop(worker, cti);
  checkCapacityThenNonCapacityError(worker, cti);
  checkCapacityReset(worker, cti);
  checkOwnedMalformedSuppression(worker, cti);
  std::cout << "T4 capacity/error contrast: five distinct capacity errors, reset, and one owned "
               "malformed error passed\n";
  checkLifecycle(worker, cti);
  std::cout << "T4 lifecycle: Close/reopen with retained lease passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "test_worker_direct_shm failed: " << error.what() << '\n';
  return 1;
}

