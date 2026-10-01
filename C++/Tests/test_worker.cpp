#include "OneGenCoordinator.h"
#include "FakeWorker.h"
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
#include <map>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// One executable also acts as a hardware-free protocol peer for completion-order checks.
std::atomic<int> startup_diagnostics{0};


int main(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  if (argc == 3 && std::string_view(argv[1]) == "--cti") return fakeWorker(argv[2]);
  if ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--profile-discovery") {
    try {
      const auto budget = argc == 5 ? std::chrono::milliseconds{std::stoll(argv[4])}
                                    : std::chrono::milliseconds{1000};
      onegen::worker::Coordinator coordinator(argv[2], argv[3], budget);
      const auto result = coordinator.discover();
      std::cout << "Discovery: producers=" << result.finished_producers << '/' << result.total_producers
                << " interfaces=" << result.interfaces.size() << " devices=" << result.devices.size()
                << " failed-producers=" << result.failures.size() << '\n';
      for (const auto& failure : result.failures) std::cout << "Failure: " << failure << '\n';
      coordinator.shutdown();
      return result.completed ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 6 && std::string_view(argv[1]) == "--profile-interface") {
    try {
      const std::string id = argv[5];
      onegen::worker::Coordinator coordinator(argv[2], {}, std::chrono::seconds{1});
      bool connected = false;
      for (int candidate = 3; candidate <= 4; ++candidate) {
        const onegen::InterfaceInfo info{id, std::filesystem::weakly_canonical(argv[candidate]).string()};
        try {
          coordinator.open_interface(info);
          std::cout << "Interface candidate opened: " << info.cti_path << '\n';
          std::cout << "Feature root: " << coordinator.interface_features(info).name
                    << ", cameras: " << coordinator.rescan_interface(info).size() << '\n';
          coordinator.close_interface(info);
          coordinator.open_interface(info); coordinator.close_interface(info);
          connected = true; break;
        } catch (const std::exception& error) {
          std::cout << "Interface candidate rejected: " << error.what() << '\n';
        }
      }
      coordinator.shutdown();
      if (!connected) throw std::runtime_error("No interface candidate connected.");
      std::cout << "Producer fallback, settings/rescan, close and reconnect passed.\n";
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if ((argc == 5 || argc == 6)
      && (std::string_view(argv[1]) == "--profile-connect" || std::string_view(argv[1]) == "--profile-single")) {
    try {
      const auto discovery_timeout = argc == 6
          ? std::chrono::milliseconds{std::stoll(argv[5])} : std::chrono::seconds{1};
      onegen::worker::Coordinator coordinator(argv[2], argv[3], discovery_timeout);
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
  assert(startup.finish({}).size() == 2); // Reusable by another session.
  assert(!startup.running());

  // Multiple consumers wait on one in-flight result; cached exceptions reach each reader.
  {
    std::promise<void> complete_scan;
    auto signal = complete_scan.get_future();
    std::atomic<int> scans{0};
    onegen::worker::StartupDiscovery shared([&](auto) {
      ++scans;
      signal.wait();
      onegen::DeviceInfo device;
      device.id = "shared";
      return std::vector<onegen::DeviceInfo>{device};
    });
    auto first_waiter = std::async(std::launch::async, [&] { return shared.finish({}); });
    auto second_waiter = std::async(std::launch::async, [&] { return shared.finish({}); });
    complete_scan.set_value();
    assert(first_waiter.get().front().id == "shared");
    assert(second_waiter.get().front().id == "shared");
    assert(scans == 1);
    onegen::worker::StartupDiscovery failed([](auto) -> std::vector<onegen::DeviceInfo> {
      throw std::runtime_error("Synthetic startup failure");
    });
    for (int consumer = 0; consumer < 2; ++consumer) {
      bool rejected = false;
      try { failed.finish({}); } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()) == "Synthetic startup failure";
      }
      assert(rejected);
    }
  }

  // A completed empty startup inventory does not impose another session wait.
  onegen::worker::StartupDiscovery empty([](auto) {
    return std::vector<onegen::DeviceInfo>{};
  });
  const auto empty_start = std::chrono::steady_clock::now();
  assert(empty.waitForCamera().empty());
  assert(std::chrono::steady_clock::now() - empty_start < 1s);
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
  assert(merged.size() == 3);
  assert(merged.front().id == "preferred");
  assert(merged.front().cti_candidates.size() == 1);
  auto scoped = first;
  scoped.interface_id = "another-board";
  assert(onegen::worker::Coordinator::merge({first, scoped}).size() == 2);
  assert(onegen::worker::Coordinator::merge({first, first}).size() == 1);

  // Fabricated InterfaceInfo values must use the same canonical paths as discovery.
  const auto root = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
      ("onegen-worker-ctis-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root / "vendor-a");
  std::filesystem::create_directories(root / "vendor-b");
  std::ofstream(root / "vendor-a" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti.starts", std::ios::trunc);
  // Exact consumer files override directory/environment discovery, including empty.
  {
    onegen::worker::Coordinator explicit_files(argv[0], root, std::chrono::milliseconds{1});
    assert(explicit_files.set_producer_files({root / "vendor-b" / "Producer.cti"}));
    const auto selected = explicit_files.discover();
    assert(selected.total_producers == 1 && selected.completed);
    assert(!explicit_files.set_producer_files({})); // Retained workers require reset.
    assert(explicit_files.reset_idle_search());
    assert(explicit_files.set_producer_files({}));
    const auto none = explicit_files.discover();
    assert(none.completed && none.total_producers == 0 && none.devices.empty());
  }
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
    other.cti_candidate_ids = {other.id};
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

  // A stale lease must not release the same camera's replacement worker lease.
  {
    onegen::worker::Coordinator restarted(argv[0], root, 100ms);
    auto stale = restarted.open(*fast);
    try { stale->read_node(0, "ExitWorker"); } catch (const std::exception&) {}
    assert(!stale->alive());
    auto current = restarted.open(*fast);
    stale.reset();
    bool duplicate_rejected = false;
    try { restarted.open(*fast); } catch (const std::exception&) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    assert(current->data_streams().front().id == "fast");
    current.reset();
  }

  // Empty short probes retry within the overall budget and return as soon as a camera appears.
  const auto retry_root = root / "retry";
  std::filesystem::create_directories(retry_root);
  std::ofstream(retry_root / "Retry.cti").put('\0');
  onegen::worker::Coordinator retrying(argv[0], retry_root, 5s);
  const auto retry_started = std::chrono::steady_clock::now();
  const auto retry_result = retrying.discover();
  const auto retry_elapsed = std::chrono::steady_clock::now() - retry_started;
  assert(retry_result.devices.size() == 1);
  assert(retry_elapsed >= 1s && retry_elapsed < 2s);
  retrying.shutdown();

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

  // A shared board worker keeps each camera's requests and callbacks independent.
  {
    const auto boards = root / "shared-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "Board.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator shared(argv[0], boards, 100ms);
    const auto scan = shared.discover();
    assert(scan.interfaces.size() == 1);
    const auto board = scan.interfaces.front();
    shared.open_interface(board);
    bool duplicate_rejected = false;
    try { shared.open_interface(board); } catch (...) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    const auto cameras = shared.rescan_interface(board);
    assert(cameras.size() == 2);
    auto first = shared.open(cameras[0]);
    auto second = shared.open(cameras[1]);
    assert(first->data_streams()[0].id == "channel-a");
    assert(second->data_streams()[0].id == "channel-b");
    std::atomic<int> a{0}, b{0};
    first->set_frame_callback([&](const auto&) { ++a; });
    second->set_frame_callback([&](const auto&) { ++b; });
    first->live(); second->live(); first->trigger(); second->trigger();
    const auto wait_frames = [&](int expected_a, int expected_b) {
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while ((a != expected_a || b != expected_b) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      assert(a == expected_a && b == expected_b);
    };
    wait_frames(1, 1);
    bool write_rejected = false, release_rejected = false;
    try { shared.write_interface(board, "Feature", "set", "1"); } catch (...) { write_rejected = true; }
    try { shared.close_interface(board); } catch (...) { release_rejected = true; }
    assert(write_rejected && release_rejected);
    first.reset();
    assert(second->data_streams()[0].id == "channel-b");
    second->trigger(); wait_frames(1, 2);
    assert(shared.rescan_interface(board).size() == 2);
    second->trigger(); wait_frames(1, 3);
    second.reset();
    shared.write_interface(board, "Feature", "set", "1");
    shared.close_interface(board);
    shared.open_interface(board); shared.close_interface(board);
    std::ifstream starts(cti.string() + ".starts");
    std::size_t count = 0; std::string line;
    while (std::getline(starts, line)) ++count;
    assert(count == 1); // Both cameras used the original board worker.
    shared.shutdown();
  }
  // Empty interfaces remain selectable and configurable without a camera lease.
  {
    const auto boards = root / "empty-board";
    std::filesystem::create_directories(boards);
    std::ofstream(boards / "EmptyBoard.cti").put('\0');
    onegen::worker::Coordinator empty_board(argv[0], boards, 100ms);
    const auto scan = empty_board.discover();
    assert(scan.devices.empty() && scan.failures.empty() && scan.interfaces.size() == 1);
    empty_board.open_interface(scan.interfaces[0]);
    assert(empty_board.rescan_interface(scan.interfaces[0]).empty());
    assert(empty_board.interface_features(scan.interfaces[0]).name == "Root");
    empty_board.write_interface(scan.interfaces[0], "Feature", "set", "1");
    empty_board.close_interface(scan.interfaces[0]);
    std::filesystem::remove(boards / "EmptyBoard.cti");
    assert(empty_board.discover().interfaces.empty());
    assert(empty_board.interfaces().empty());
    empty_board.shutdown();
  }
  // A failed native open must release its idle worker before another producer is tried.
  {
    const auto boards = root / "failed-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "FailBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator failed(argv[0], boards, 100ms);
    const auto board = failed.discover().interfaces.front();
    for (int retry = 0; retry < 2; ++retry) {
      bool rejected = false;
      try { failed.open_interface(board); } catch (const std::exception&) { rejected = true; }
      assert(rejected);
    }
    std::ifstream starts(cti.string() + ".starts");
    std::size_t count = 0; std::string line;
    while (std::getline(starts, line)) ++count;
    assert(count == 2);
    failed.shutdown();
  }
  const auto line_count = [](const std::filesystem::path& path) {
    std::ifstream file(path);
    std::size_t count = 0;
    for (std::string line; std::getline(file, line);) ++count;
    return count;
  };
  // Failed Interfaces are reported once without dropping devices from surviving Interfaces.
  for (const auto name : {"ProbeFail", "PartialProbe"}) {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    const auto cti = directory / (std::string(name) + ".cti");
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator failed(argv[0], directory, 5s);
    const auto result = failed.discover();
    assert(result.completed && result.interfaces.size() == 1 && result.failures.size() == 1);
    assert(result.failures.front().find("interface=busy-board") != std::string::npos);
    assert(result.devices.size() == (std::string_view(name) == "PartialProbe" ? 1U : 0U));
    assert(line_count(cti.string() + ".probes") == 1); // Failure never becomes an empty-inventory retry.
    failed.open_interface(result.interfaces.front());
    assert(line_count(cti.string() + ".starts") == 2); // Failed idle discovery worker was retired.
    failed.close_interface(result.interfaces.front());
    failed.shutdown();
  }
  // Discovery failure cannot retire a pinned Interface or interrupt its live Device.
  {
    const auto directory = root / "active-failure";
    std::filesystem::create_directories(directory);
    const auto cti = directory / "PartialProbe.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator active(argv[0], directory, 100ms);
    const onegen::InterfaceInfo board{"board", cti.string()};
    active.open_interface(board);
    assert(!active.reset_idle_search()); // A camera-less Interface still owns its producer.
    const auto pinned_result = active.discover();
    assert(pinned_result.failures.size() == 1 && line_count(cti.string() + ".starts") == 1);
    assert(active.interface_features(board).name == "Root");
    auto camera = active.open(active.rescan_interface(board).front());
    assert(!active.reset_idle_search());
    std::atomic<int> frames{0};
    camera->set_frame_callback([&](const auto&) { ++frames; });
    camera->live();
    const auto result = active.discover();
    assert(result.failures.size() == 1 && result.devices.size() == 1);
    assert(camera->alive() && camera->data_streams().front().id == "channel-a");
    camera->trigger();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (frames == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    assert(frames == 1 && line_count(cti.string() + ".starts") == 1);
    camera.reset();
    assert(active.interface_features(board).name == "Root");
    active.close_interface(board);
    assert(active.reset_idle_search());
    assert(active.interfaces().empty());
    active.open_interface(board);
    assert(line_count(cti.string() + ".starts") == 2);
    active.close_interface(board);
    active.shutdown();
  }
  // A direct Device lease protects its worker independently of pinned Interface sessions.
  {
    const auto directory = root / "direct-active-failure";
    std::filesystem::create_directories(directory);
    const auto cti = directory / "SessionProbeFailure.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator active(argv[0], directory, 100ms);
    const auto initial = active.discover();
    assert(initial.failures.empty() && initial.devices.size() == 1);
    auto camera = active.open(initial.devices.front());
    assert(!active.reset_idle_search());
    camera->live();
    const auto result = active.discover();
    assert(result.failures.size() == 1 && result.devices.size() == 1);
    assert(camera->alive() && camera->data_streams().front().id == "fast");
    assert(line_count(cti.string() + ".starts") == 1);
    camera.reset();
    assert(active.reset_idle_search());
    active.shutdown();
  }
  std::filesystem::remove_all(root);
  std::cout << "test_worker ok\n";
}
