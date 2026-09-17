#include "OneGenCoordinator.h"
#include "OneGenWorkerClient.h"
#include "OneGenWorkerMessages.h"
#include "Utility/PlaygroundAdapter/Plugin/StartupDiscovery.h"

#include <QCoreApplication>
#include <QDebug>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <set>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// One executable also acts as a hardware-free protocol peer for completion-order checks.
std::atomic<int> startup_diagnostics{0};

int fakeWorker(const std::filesystem::path& cti)
{
  std::ofstream(cti.string() + ".starts", std::ios::app) << "start\n";
  using namespace onegen::worker;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::array<std::byte, 4> header;
  std::string opened_device;
  while (std::cin.read(reinterpret_cast<char*>(header.data()), header.size())) {
    const auto size = frameSize(header);
    if (size > maximum_message_size) return 1;
    std::vector<std::byte> bytes(size);
    if (!std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return 1;
    const auto request = decode(bytes);
    Writer payload;
    if (request.operation == Operation::Open) {
      Reader input(request.payload);
      opened_device = input.string();
    } else if (request.operation == Operation::Close) opened_device.clear();
    else if (request.operation == Operation::ListStreams) {
      payload.scalar(std::uint32_t{1});
      payload.scalar(std::uint32_t{0});
      payload.string(opened_device);
    }
    if (request.operation == Operation::Probe) {
      const auto diagnostic = encode({Operation::Error, 0, false, "Interface XML test failure"});
      const auto diagnostic_header = frameSize(static_cast<std::uint32_t>(diagnostic.size()));
      std::cout.write(reinterpret_cast<const char*>(diagnostic_header.data()), diagnostic_header.size());
      std::cout.write(reinterpret_cast<const char*>(diagnostic.data()), diagnostic.size());
      std::cout.flush();
      const bool slow = cti.parent_path().filename() == "vendor-a";
      if (cti.stem() == "Heartbeat") std::this_thread::sleep_for(std::chrono::seconds{6});
      if (slow) std::this_thread::sleep_for(std::chrono::seconds{1});
      onegen::DeviceInfo device;
      device.id = slow ? "slow" : "fast";
      device.serial = device.id;
      device.tl_type = "TEST";
      device.cti_path = cti.string();
      payload.scalar(std::uint32_t{1});
      write(payload, device);
    }
    const auto body = encode({request.operation, request.id, true, {}, payload.take()});
    const auto response_header = frameSize(static_cast<std::uint32_t>(body.size()));
    std::cout.write(reinterpret_cast<const char*>(response_header.data()), response_header.size());
    std::cout.write(reinterpret_cast<const char*>(body.data()), body.size());
    std::cout.flush();
    if (request.operation == Operation::Shutdown) return 0;
  }
  return 0;
}

int main(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  if (argc == 3 && std::string_view(argv[1]) == "--cti") return fakeWorker(argv[2]);
  if (argc == 5 && (std::string_view(argv[1]) == "--profile-connect" || std::string_view(argv[1]) == "--profile-single")) {
    try {
      onegen::worker::Coordinator coordinator(argv[2], argv[3], std::chrono::seconds{1});
      const auto discovery = coordinator.discover();
      const auto selected = std::find_if(discovery.devices.begin(), discovery.devices.end(),
          [&](const auto& device) { return device.id == argv[4]; });
      if (selected == discovery.devices.end()) throw std::runtime_error("Profile camera not found");
      for (int repeat = 0; repeat < 3; ++repeat) {
        const auto start = std::chrono::steady_clock::now();
        auto lease = coordinator.open(*selected);
        const auto opened = std::chrono::steady_clock::now();
        lease->data_streams();
        lease->describe_features();
        const auto ready = std::chrono::steady_clock::now();
        if (std::string_view(argv[1]) == "--profile-single") {
          std::set<std::pair<int, std::string>> invalidated;
          lease->set_invalidate_callback([&](int module, std::string_view name) {
            invalidated.emplace(module, std::string(name));
          });
          const auto pressed = std::chrono::steady_clock::now();
          bool delivered = false;
          lease->grab_one([&](const auto& frame) {
            delivered = true;
            std::cout << "Single sample=" << repeat + 1 << " receive="
                << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pressed).count()
                << " ms frame=" << frame.width << "x" << frame.height << "\n";
          });
          assert(delivered);
          lease->set_invalidate_callback({});
          const auto reads_started = std::chrono::steady_clock::now();
          for (const auto& [module, name] : invalidated) lease->read_node(module, name);
          std::cout << "Invalidation replay: unique-nodes=" << invalidated.size() << " one-read-per-node="
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reads_started).count()
              << " ms (no GUI rendering)\n";
        }
        lease.reset();
        std::cout << "Coordinator sample=" << repeat + 1
            << " open=" << std::chrono::duration<double, std::milli>(opened - start).count()
            << " ms ready=" << std::chrono::duration<double, std::milli>(ready - start).count()
            << " ms\n";
      }
      return 0;
    } catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  assert(argc == 2);

  // Startup returns on the first camera, while the same scan continues in the controls.
  using namespace std::chrono_literals;
  std::promise<void> continue_scan;
  auto continuation = continue_scan.get_future();
  onegen::worker::StartupDiscovery startup([&](auto progress) {
    onegen::worker::DiscoveryResult update;
    onegen::DeviceInfo camera;
    camera.id = "early";
    update.devices.push_back(camera);
    progress(update);
    continuation.wait();
    camera.id = "late";
    update.devices.push_back(camera);
    progress(update);
    return update.devices;
  });
  assert(startup.waitForCamera(1s).size() == 1);
  continue_scan.set_value();
  assert(startup.finish({}).size() == 2);

  // Even a completed empty scan holds the overlay until the no-camera deadline.
  onegen::worker::StartupDiscovery empty([](auto) {
    return std::vector<onegen::DeviceInfo>{};
  });
  const auto empty_start = std::chrono::steady_clock::now();
  assert(empty.waitForCamera().empty());
  assert(std::chrono::steady_clock::now() - empty_start >= 10s);
  assert(empty.finish({}).empty());

  // A camera arriving after the deadline is still delivered without a second scan.
  std::promise<void> late_camera;
  auto late_signal = late_camera.get_future();
  onegen::worker::StartupDiscovery late([&](auto progress) {
    late_signal.wait();
    onegen::worker::DiscoveryResult update;
    onegen::DeviceInfo camera;
    camera.id = "after-timeout";
    update.devices.push_back(camera);
    progress(update);
    return update.devices;
  });
  assert(late.waitForCamera(20ms).empty());
  late_camera.set_value();
  bool late_delivered = false;
  assert(late.finish([&](const auto& update) {
    late_delivered = late_delivered || !update.devices.empty();
  }).size() == 1);
  assert(late_delivered);

  onegen::DeviceInfo first;
  first.id = "first";
  first.serial = "123";
  first.tl_type = "GEV";
  first.cti_path = "b/Producer.cti";
  auto preferred = first;
  preferred.id = "preferred";
  preferred.cti_path = "a/Producer.cti";
  auto second = first;
  second.id = "second";
  second.serial = "456";
  const auto merged = onegen::worker::Coordinator::merge({first, preferred, second});
  assert(merged.size() == 2);
  assert(merged.front().id == "preferred");
  assert(merged.front().cti_candidates.size() == 2);

  const auto root = std::filesystem::temp_directory_path() /
      ("onegen-worker-ctis-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "vendor-a");
  std::filesystem::create_directories(root / "vendor-b");
  std::ofstream(root / "vendor-a" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti.starts", std::ios::trunc);
  onegen::worker::Coordinator coordinator(argv[1], root, std::chrono::seconds{2});
  std::size_t updates = 0;
  const auto caller = std::this_thread::get_id();
  const auto discovery = coordinator.discover([&](const auto& update) {
    ++updates;
    assert(std::this_thread::get_id() == caller);
    assert(update.finished_producers == updates);
    assert(update.total_producers == 2);
    assert(update.failures.size() == updates);
  });
  assert(updates == 2);
  assert(discovery.completed);
  assert(discovery.devices.empty());
  assert(discovery.failures.size() == 2);

  onegen::worker::Coordinator progressive(argv[0], root, std::chrono::seconds{2});
  const auto previous_handler = qInstallMessageHandler(
      [](QtMsgType type, const QMessageLogContext&, const QString& message) {
        if (type == QtWarningMsg && message == QStringLiteral("[GenICam] Interface XML test failure"))
          ++startup_diagnostics;
      });
  std::size_t progressive_updates = 0;
  auto complete = progressive.discover([&](const auto& update) {
    ++progressive_updates;
    if (progressive_updates == 1) {
      assert(update.finished_producers == 1);
      assert(update.devices.size() == 1);
      assert(update.devices.front().id == "fast");
      auto early_connection = progressive.open(update.devices.front());
      assert(early_connection->info().id == "fast");
    }
  });
  assert(progressive_updates == 2);
  assert(complete.devices.size() == 2);
  const auto fast = std::find_if(complete.devices.begin(), complete.devices.end(),
      [](const auto& device) { return device.id == "fast"; });
  assert(fast != complete.devices.end());
  for (int repeat = 0; repeat < 2; ++repeat) {
    auto reconnected = progressive.open(*fast);
    assert(reconnected->info().id == "fast");
  }
  std::ifstream starts(root / "vendor-b" / "Producer.cti.starts");
  std::string start_line;
  int worker_starts = 0;
  while (std::getline(starts, start_line)) ++worker_starts;
  starts.close();
  assert(worker_starts == 1);
  qInstallMessageHandler(previous_handler);
  assert(startup_diagnostics == 2);
  {
    auto first_camera = progressive.open(*fast);
    auto other = *fast;
    other.id = "fast-secondary";
    other.serial = other.id;
    auto second_camera = progressive.open(other);
    assert(first_camera->data_streams().front().id == "fast");
    assert(second_camera->data_streams().front().id == "fast-secondary");
    bool duplicate_rejected = false;
    try { progressive.open(*fast); } catch (const std::exception&) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    second_camera.reset();
    assert(first_camera->data_streams().front().id == "fast");
    second_camera = progressive.open(other);
    assert(second_camera->data_streams().front().id == "fast-secondary");
    assert(first_camera->data_streams().front().id == "fast");
  }
  progressive.shutdown();

  // Long scans still publish progress before any producer has completed.
  const auto heartbeat_root = root / "heartbeat";
  std::filesystem::create_directories(heartbeat_root);
  std::ofstream(heartbeat_root / "Heartbeat.cti").put('\0');
  onegen::worker::Coordinator heartbeat(argv[0], heartbeat_root, 10s);
  bool waiting_report = false;
  const auto heartbeat_result = heartbeat.discover([&](const auto& update) {
    if (update.finished_producers == 0) {
      assert(update.total_producers == 1 && update.devices.empty());
      waiting_report = true;
    }
  });
  assert(waiting_report && heartbeat_result.finished_producers == 1);
  heartbeat.shutdown();

  // Once attached, the existing error callback receives the diagnostic instead.
  {
    onegen::worker::Connection attached(argv[0], root / "vendor-b" / "Producer.cti");
    std::atomic<int> callback_diagnostics{0};
    attached.set_error_callback([&](std::string_view error) {
      if (error == "Interface XML test failure") ++callback_diagnostics;
    });
    attached.request(onegen::worker::Operation::Probe);
    assert(callback_diagnostics == 1);
    attached.set_error_callback({});
  }

  std::filesystem::remove_all(root);
  std::cout << "test_worker ok\n";
}
