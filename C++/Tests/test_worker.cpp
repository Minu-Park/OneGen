#include "OneGenCoordinator.h"
#include "FakeWorker.h"
#include "OneGenWorkerClient.h"
#include "OneGenWorkerMessages.h"
#include "Utility/PlaygroundAdapter/Plugin/StartupDiscovery.h"

#include <QCoreApplication>
#include <QDebug>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <set>
#include <map>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// One executable also acts as a hardware-free protocol peer for completion-order checks.
std::atomic<int> startup_diagnostics{0};


int runTests(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  if (argc == 3 && std::string_view(argv[1]) == "--cti") return fakeWorker(argv[2]);
  if (argc == 4 && std::string_view(argv[1]) == "--inactive-producer") {
    onegen::worker::Connection connection(argv[2], argv[3]);
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::string observed = "request unexpectedly succeeded";
      try { connection.request(onegen::worker::Operation::ListInterfaces); }
      catch (const std::exception& error) {
        observed = error.what();
      }
      const bool alive = connection.alive(), inactive = connection.inactive();
      if (observed.find("Producer inactive:") == std::string::npos
          || observed.find("GCInitLib failed (-1001)") == std::string::npos
          || !alive || !inactive) {
        throw std::runtime_error("Inactive producer check failed: attempt=" + std::to_string(attempt + 1)
            + " alive=" + std::to_string(alive) + " inactive=" + std::to_string(inactive)
            + " observed=" + observed);
      }
    }
    connection.close();
    if (connection.alive()) throw std::runtime_error("Inactive producer worker remained alive after close.");
    std::cout << "Inactive producer: explicit initialization error; worker remained alive for three requests.\n";
    return 0;
  }
  const bool profile_producers = argc >= 4 && std::string_view(argv[1]) == "--profile-producers";
  if (profile_producers || ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "--profile-discovery")) {
    try {
      const auto budget = !profile_producers && argc == 5 ? std::chrono::milliseconds{std::stoll(argv[4])}
                                    : std::chrono::milliseconds{1000};
      onegen::worker::Coordinator coordinator(argv[2], profile_producers ? "" : argv[3], budget);
      if (profile_producers) {
        std::vector<std::filesystem::path> files;
        for (int index = 3; index < argc; ++index) files.emplace_back(argv[index]);
        assert(coordinator.set_producer_files(std::move(files)));
      }
      const auto result = coordinator.discover();
      std::cout << "Discovery: producers=" << result.finished_producers << '/' << result.total_producers
                << " interfaces=" << result.interfaces.size() << " devices=" << result.devices.size()
                << " failed-producers=" << result.failures.size() << '\n';
      for (const auto& device : result.devices)
        std::cout << "Device: id=" << device.id << " interface=" << device.interface_id
                  << " model=" << device.model << " serial=" << device.serial
                  << " cti=" << device.cti_path << '\n';
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
  if ((argc == 6 || argc == 7) && std::string_view(argv[1]) == "--profile-live") {
    try {
      using namespace std::chrono_literals;
      const auto expected = std::clamp(std::stoi(argv[5]), 1, 600);
      const auto discovery_timeout = argc == 7 ? std::chrono::milliseconds{std::stoll(argv[6])}
                                               : std::chrono::seconds{2};
      onegen::worker::Coordinator coordinator(argv[2], argv[3], discovery_timeout);
      const auto discovery = coordinator.discover();
      const auto selected = std::find_if(discovery.devices.begin(), discovery.devices.end(),
          [&](const auto& device) { return device.id == argv[4]; });
      if (selected == discovery.devices.end()) throw std::runtime_error("Live profile camera not found");
      auto lease = coordinator.open(*selected);
      std::mutex mutex;
      std::condition_variable ready;
      int frames = 0;
      std::size_t bytes = 0;
      std::uint64_t last_frame = 0;
      lease->set_frame_callback([&](const auto& frame) {
        {
          std::lock_guard lock(mutex);
          ++frames;
          bytes = frame.payload.size();
          last_frame = frame.frame_id;
        }
        ready.notify_all();
      });
      const auto started = std::chrono::steady_clock::now();
      lease->live();
      std::unique_lock lock(mutex);
      if (!ready.wait_for(lock, 120s, [&] { return frames >= expected; }))
        throw std::runtime_error("Live profile timed out: frames=" + std::to_string(frames));
      lock.unlock();
      const auto elapsed = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started).count();
      lease->stop();
      lease.reset();
      coordinator.shutdown();
      std::cout << "Live profile: frames=" << frames << " bytes=" << bytes
                << " last_frame=" << last_frame << " elapsed_ms=" << elapsed
                << " fps=" << (frames * 1000.0 / elapsed) << '\n';
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 3 && std::string_view(argv[1]) == "--profile-large-frames") {
    try {
      using namespace std::chrono_literals;
      const auto count = std::clamp(std::stoi(argv[2]), 1, 60);
      const auto root = std::filesystem::temp_directory_path() / "onegen-large-frame-profile";
      std::filesystem::create_directories(root);
      const auto cti = root / "LargeFrameBoard.cti";
      std::ofstream(cti).put('\0');
      onegen::worker::Connection connection(argv[0], cti);
      std::mutex mutex;
      std::condition_variable ready;
      int delivered = 0;
      std::uint64_t last_frame = 0;
      std::size_t last_bytes = 0;
      connection.set_frame_callback([&](const auto& frame) {
        {
          std::lock_guard lock(mutex);
          ++delivered;
          last_frame = frame.frame_id;
          last_bytes = frame.payload.size();
        }
        ready.notify_all();
      }, 1);
      onegen::worker::Writer open;
      open.string("fast");
      open.string("board");
      connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
      connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
      const auto started = std::chrono::steady_clock::now();
      double minimum = 0, maximum = 0, total = 0;
      for (int index = 0; index < count; ++index) {
        const auto trigger_started = std::chrono::steady_clock::now();
        connection.request(onegen::worker::Operation::Trigger, {}, 60s, 1);
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, 120s, [&] { return delivered > index; }))
          throw std::runtime_error("Large-frame profile timed out.");
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - trigger_started).count();
        minimum = index ? std::min(minimum, elapsed) : elapsed;
        maximum = std::max(maximum, elapsed);
        total += elapsed;
        std::cout << "Large frame=" << last_frame << " bytes=" << last_bytes
                  << " round_trip_ms=" << elapsed << '\n';
      }
      const auto elapsed_total = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - started).count();
      connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
      connection.close();
      std::cout << "Large-frame profile: frames=" << count
                << " bytes_per_frame=" << last_bytes
                << " mean_ms=" << (total / count)
                << " min_ms=" << minimum << " max_ms=" << maximum
                << " elapsed_ms=" << elapsed_total
                << " fps=" << (count * 1000.0 / elapsed_total) << '\n';
      return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
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
  assert(startup.status().phase == onegen::worker::StartupDiscovery::Phase::Running);
  continue_scan.set_value();
  assert(startup.finish({}).size() == 2);
  assert(startup.finish({}).size() == 2); // Reusable by another session.
  assert(!startup.running());
  assert(startup.status().phase == onegen::worker::StartupDiscovery::Phase::Succeeded);

  // Cancelling one consumer wait does not cancel or destroy the reusable scan.
  {
    std::promise<void> release;
    auto signal = release.get_future();
    std::atomic<bool> cancelled{false};
    onegen::worker::StartupDiscovery scan([&](auto) {
      signal.wait();
      onegen::DeviceInfo device; device.id = "after-cancel";
      return std::vector<onegen::DeviceInfo>{device};
    });
    auto waiter = std::async(std::launch::async, [&] {
      try { scan.waitForCamera(10s, [&] { return cancelled.load(); }); }
      catch (const std::runtime_error&) { return true; }
      return false;
    });
    cancelled = true;
    const auto returned = waiter.wait_for(500ms) == std::future_status::ready;
    release.set_value(); // Drain even when the assertion below fails.
    assert(returned && waiter.get());
    assert(scan.waitForCamera(1s).front().id == "after-cancel");
  }

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
      assert(failed.status().phase == onegen::worker::StartupDiscovery::Phase::Failed);
      assert(failed.status().detail.find("Synthetic startup failure") != std::string::npos);
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

  // Producer failures preserve partial devices; all failures remain distinguishable
  // from a successful empty inventory. State callbacks do not depend on a host SDK.
  using Scan = onegen::worker::StartupDiscovery;
  for (bool partial : {false, true}) {
    std::vector<Scan::Phase> phases;
    {
      Scan scan([&](auto progress) {
        onegen::worker::DiscoveryResult update;
        update.total_producers = partial ? 2 : 1;
        update.finished_producers = update.total_producers;
        update.failures = {"synthetic producer failure"};
        if (partial) update.devices.push_back(onegen::DeviceInfo{});
        progress(update);
        return update.devices;
      }, [&](const auto&, const auto& after) { phases.push_back(after.phase); });
      scan.finish({});
      const auto result = scan.status();
      assert(result.phase == (partial ? Scan::Phase::Partial : Scan::Phase::Failed));
      assert(result.detail.find("synthetic producer failure") != std::string::npos);
    }
    assert(phases == std::vector<Scan::Phase>({Scan::Phase::Running,
        partial ? Scan::Phase::Partial : Scan::Phase::Failed, Scan::Phase::Stopping, Scan::Phase::Stopped}));
  }
  assert(empty.status().phase == Scan::Phase::Succeeded);

  // Joining an in-flight scan suppresses its late terminal state before unload.
  {
    std::promise<void> stopping;
    auto stopped_signal = stopping.get_future();
    std::promise<void> release;
    auto release_signal = release.get_future();
    std::vector<Scan::Phase> phases;
    auto scan = std::make_unique<Scan>([&](auto) {
      release_signal.wait();
      return std::vector<onegen::DeviceInfo>{};
    }, [&](const auto&, const auto& after) {
      phases.push_back(after.phase);
      if (after.phase == Scan::Phase::Stopping) stopping.set_value();
    });
    auto joining = std::async(std::launch::async, [scan = std::move(scan)]() mutable { scan.reset(); });
    assert(stopped_signal.wait_for(1s) == std::future_status::ready);
    assert(joining.wait_for(20ms) == std::future_status::timeout);
    release.set_value();
    joining.get();
    assert(phases == std::vector<Scan::Phase>({Scan::Phase::Running, Scan::Phase::Stopping, Scan::Phase::Stopped}));
  }


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
    if (update.finished_producers == 0) return;
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
        if (type == QtDebugMsg && message == QStringLiteral("[GenICam] discovery diagnostic: Interface XML test failure"))
          ++startup_diagnostics;
      });
  std::size_t progressive_updates = 0;
  auto complete = progressive.discover([&](const auto& update) {
    if (update.finished_producers == 0) return;
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
  // Retirement waits for an active worker request before closing the worker.
  {
    const auto boards = root / "slow-retire-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SlowProbe.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    onegen::worker::Writer payload;
    payload.scalar(std::uint32_t{1000});
    auto reply = std::async(std::launch::async, [&] {
      return connection.request(onegen::worker::Operation::Probe, payload.take(), 2s);
    });
    for (int attempt = 0; attempt < 80 && !std::filesystem::exists(cti.string() + ".probe-started"); ++attempt)
      std::this_thread::sleep_for(25ms);
    assert(std::filesystem::exists(cti.string() + ".probe-started"));
    const auto started = std::chrono::steady_clock::now();
    assert(connection.beginRetirement());
    assert(std::chrono::steady_clock::now() - started >= 200ms);
    (void)reply.get();
    connection.close();
#ifndef _WIN32
    assert(std::filesystem::exists(cti.string() + ".shutdown"));
#endif
  }
  // A timed-out retirement skips Shutdown and reclaims the process before a same-CTI restart.
  {
    const auto boards = root / "slow-timeout-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SlowProbeTimeout.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    onegen::worker::Writer payload;
    payload.scalar(std::uint32_t{1000});
    auto reply = std::async(std::launch::async, [&] {
      return connection.request(onegen::worker::Operation::Probe, payload.take(), 4s);
    });
    for (int attempt = 0; attempt < 80 && !std::filesystem::exists(cti.string() + ".probe-started"); ++attempt)
      std::this_thread::sleep_for(25ms);
    assert(std::filesystem::exists(cti.string() + ".probe-started"));
    assert(!connection.beginRetirement(100ms));
    connection.close();
    bool probe_failed = false;
    try { (void)reply.get(); } catch (const std::exception&) { probe_failed = true; }
    assert(probe_failed);
    assert(!connection.alive());
    assert(!std::filesystem::exists(cti.string() + ".shutdown"));
    // The same producer starts a fresh worker after the forced termination.
    onegen::worker::Connection restart(argv[0], cti);
    restart.request(onegen::worker::Operation::Probe);
    restart.close();
    assert(std::filesystem::exists(cti.string() + ".shutdown"));
    assert(line_count(cti.string() + ".starts") == 2);
  }
  // A same-CTI connection waits until the retiring worker has shut down.
  {
    const auto boards = root / "slow-retry-coordinator";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SlowRetryBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator coordinator(argv[0], boards, 100ms);
    const auto scan = coordinator.discover();
    assert(scan.interfaces.size() == 1 && scan.devices.size() == 1);
    auto feature = std::async(std::launch::async, [&] {
      return coordinator.interface_features(scan.interfaces.front());
    });
    for (int attempt = 0; attempt < 80 && !std::filesystem::exists(cti.string() + ".interface-started"); ++attempt)
      std::this_thread::sleep_for(25ms);
    assert(std::filesystem::exists(cti.string() + ".interface-started"));
    auto opening = std::async(std::launch::async, [&] {
      return coordinator.open(scan.devices.front());
    });
    for (int attempt = 0; attempt < 80 && !std::filesystem::exists(cti.string() + ".open-failed"); ++attempt)
      std::this_thread::sleep_for(25ms);
    assert(std::filesystem::exists(cti.string() + ".open-failed"));
    auto waiting_feature = std::async(std::launch::async, [&] {
      return coordinator.interface_features(scan.interfaces.front());
    });
    auto lease = opening.get();
    assert(lease && lease->info().id == "fast");
    assert(feature.get().name == "Root");
    assert(waiting_feature.get().name == "Root");
    assert(std::filesystem::exists(cti.string() + ".shutdown"));
    lease.reset();
    coordinator.shutdown();
  }
  // Equivalent producer path spellings reuse the retained discovery worker.
  {
    const auto boards = root / "path-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "PathBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator path(argv[0], boards, 100ms);
    const auto scan = path.discover();
    assert(scan.devices.size() == 1);
    auto device = scan.devices.front();
    device.cti_path = (cti.parent_path() / "." / cti.filename()).string();
    device.cti_candidates = {device.cti_path};
    auto lease = path.open(device);
    assert(lease && lease->info().id == "fast");
    lease.reset();
    path.shutdown();
    assert(line_count(cti.string() + ".starts") == 1);
  }
  // A discovery-only worker that still holds an exclusive Interface is replaced once on open failure.
  {
    const auto boards = root / "retry-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "RetryBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator retry(argv[0], boards, 100ms);
    const auto scan = retry.discover();
    assert(scan.devices.size() == 1);
    auto lease = retry.open(scan.devices.front());
    assert(lease && lease->info().id == "fast");
    lease.reset();
    retry.shutdown();
    assert(line_count(cti.string() + ".starts") == 2);
  }
  // Retry is bounded to one replacement per CTI even when the replacement also fails.
  {
    const auto boards = root / "retry-always-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "RetryBoardAlways.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator retry(argv[0], boards, 100ms);
    const auto scan = retry.discover();
    assert(scan.devices.size() == 1);
    bool rejected = false;
    try { retry.open(scan.devices.front()); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
    retry.shutdown();
    assert(line_count(cti.string() + ".starts") == 2);
  }
  // An open during the discovery scan must not retire the worker carrying the probe.
  {
    const auto boards = root / "discovery-retire-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "RetryBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator during(argv[0], boards, 100ms);
    bool early_failed = false;
    const auto scan = during.discover([&](const auto& update) {
      if (early_failed || update.finished_producers == 0 || update.devices.empty()) return;
      try { during.open(update.devices.front()); } catch (const std::exception&) { early_failed = true; }
    });
    assert(early_failed);
    assert(scan.devices.size() == 1);
    auto lease = during.open(scan.devices.front());
    assert(lease && lease->info().id == "fast");
    lease.reset();
    during.shutdown();
    assert(line_count(cti.string() + ".starts") == 2);
  }
  // Non-retryable open failures do not replace a discovery worker.
  {
    const auto boards = root / "nonretry-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "PathBoardBad.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator bad(argv[0], boards, 100ms);
    const auto scan = bad.discover();
    assert(scan.devices.size() == 1);
    auto device = scan.devices.front();
    device.interface_id = "wrong";
    device.cti_candidates = {cti.string()};
    device.cti_candidate_interfaces = {"wrong"};
    bool rejected = false;
    try { bad.open(device); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
    bad.shutdown();
    assert(line_count(cti.string() + ".starts") == 1);
  }
  // Initialization refusal stays inactive; a dead worker also waits for explicit retry.
  for (const auto name : {"Inactive", "CrashOnce"}) {
    const auto directory = root / name;
    std::filesystem::create_directories(directory);
    const auto cti = directory / (std::string(name) + ".cti");
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator coordinator(argv[0], directory, 100ms);
    const auto failed = coordinator.discover();
    assert(failed.completed && failed.failures.size() == 1 && failed.devices.empty());
    if (std::string_view(name) == "Inactive")
      assert(failed.failures.front().find("Producer inactive:") != std::string::npos);
    else assert(failed.failures.front().find("process exit code 71") != std::string::npos);
    assert(line_count(cti.string() + ".starts") == 1);
    std::this_thread::sleep_for(100ms);
    assert(line_count(cti.string() + ".starts") == 1); // No automatic restart.
    std::ofstream(cti.string() + ".ready").put('\0');
    const auto recovered = coordinator.discover();
    assert(recovered.failures.empty() && recovered.devices.size() == 1);
    assert(line_count(cti.string() + ".starts") == 2);
    assert(coordinator.discover().failures.empty());
    assert(line_count(cti.string() + ".starts") == 2); // Healthy worker remains shared.
    coordinator.shutdown();
  }
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
  // Idle reset rejects a same-producer Device claim that appears after its idle snapshot.
  {
    const auto boards = root / "reset-device-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "ResetBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator coordinator(argv[0], boards, 100ms);
    const auto scan = coordinator.discover();
    assert(scan.devices.size() == 1);
    std::promise<void> paused;
    std::promise<void> resume;
    auto paused_signal = paused.get_future();
    auto resume_signal = resume.get_future();
    auto resetting = std::async(std::launch::async, [&] {
      return coordinator.reset_idle_search([&] {
        paused.set_value();
        resume_signal.wait();
      });
    });
    assert(paused_signal.wait_for(2s) == std::future_status::ready);
    std::unique_ptr<onegen::worker::Lease> lease;
    try { lease = coordinator.open(scan.devices.front()); }
    catch (...) { resume.set_value(); throw; }
    resume.set_value();
    assert(!resetting.get());
    assert(lease && lease->data_streams().front().id == "fast");
    lease.reset();
    assert(coordinator.reset_idle_search());
    assert(line_count(cti.string() + ".starts") == 1);
    coordinator.shutdown();
  }
  // Idle reset rejects an Interface claim that appears after its idle snapshot.
  {
    const auto boards = root / "reset-interface-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "ResetBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator coordinator(argv[0], boards, 100ms);
    const auto scan = coordinator.discover();
    assert(scan.interfaces.size() == 1);
    std::promise<void> paused;
    std::promise<void> resume;
    auto paused_signal = paused.get_future();
    auto resume_signal = resume.get_future();
    auto resetting = std::async(std::launch::async, [&] {
      return coordinator.reset_idle_search([&] {
        paused.set_value();
        resume_signal.wait();
      });
    });
    assert(paused_signal.wait_for(2s) == std::future_status::ready);
    try { coordinator.open_interface(scan.interfaces.front()); }
    catch (...) { resume.set_value(); throw; }
    resume.set_value();
    assert(!resetting.get());
    assert(coordinator.interface_features(scan.interfaces.front()).name == "Root");
    coordinator.close_interface(scan.interfaces.front());
    assert(coordinator.reset_idle_search());
    assert(line_count(cti.string() + ".starts") == 1);
    coordinator.shutdown();
  }
  // Idle reset preserves a producer worker created after its idle snapshot.
  {
    const auto boards = root / "reset-new-producer-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "ResetBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Coordinator coordinator(argv[0], boards, 100ms);
    const auto scan = coordinator.discover();
    assert(scan.devices.size() == 1);
    const auto added = boards / "ResetBoard2.cti";
    std::ofstream(added).put('\0');
    const onegen::InterfaceInfo added_board{"board", added.string()};
    std::promise<void> paused;
    std::promise<void> resume;
    auto paused_signal = paused.get_future();
    auto resume_signal = resume.get_future();
    auto resetting = std::async(std::launch::async, [&] {
      return coordinator.reset_idle_search([&] {
        paused.set_value();
        resume_signal.wait();
      });
    });
    assert(paused_signal.wait_for(2s) == std::future_status::ready);
    try { assert(coordinator.interface_features(added_board).name == "Root"); }
    catch (...) { resume.set_value(); throw; }
    resume.set_value();
    assert(!resetting.get());
    assert(coordinator.interface_features(added_board).name == "Root");
    assert(line_count(added.string() + ".starts") == 1);
    assert(coordinator.reset_idle_search());
    coordinator.shutdown();
  }
  // clientCreditReturnsBeforeConsumer: the worker withholds the first Trigger
  // reply until it receives FrameAck, so the callback can only proceed after the
  // client returned the credit; a slow callback then leaves the next trigger
  // without a credit and the worker reports that drop.
  {
    const auto boards = root / "credit-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "CreditBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::atomic<int> drop_probe{0};
    connection.set_invalidate_callback([&](int, std::string_view name) {
      if (name == "DropProbe") ++drop_probe;
    }, 1);
    std::atomic<int> frames{0};
    std::atomic<bool> trigger_returned{false};
    std::atomic<bool> ack_before_return{false};
    std::atomic<bool> drop_while_busy{false};
    std::atomic<bool> request_failed{false};
    std::atomic<bool> callback_done{false};
    std::mutex mutex;
    std::condition_variable ready;
    connection.set_frame_callback([&](const auto&) {
      if (++frames == 1) {
        {
          std::unique_lock lock(mutex);
          if (ready.wait_for(lock, std::chrono::seconds(5),
                             [&] { return trigger_returned.load(std::memory_order_acquire); })) {
            ack_before_return.store(true, std::memory_order_release);
          }
        }
        // The credit is back, so one trigger sends the next frame and the
        // following trigger is dropped while that frame waits for delivery.
        try {
          connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
          connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
        } catch (...) {
          request_failed.store(true, std::memory_order_release);
        }
        const auto drop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!request_failed.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < drop_deadline) {
          if (drop_probe.load(std::memory_order_acquire) > 0) {
            drop_while_busy.store(true, std::memory_order_release);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        callback_done.store(true, std::memory_order_release);
      }
      ready.notify_all();
    }, 1);
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    // The gated reply arrives only after the delivery thread returned the
    // credit, which it does before invoking the consumer callback.
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    {
      std::lock_guard lock(mutex);
      trigger_returned.store(true, std::memory_order_release);
    }
    ready.notify_all();
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return callback_done.load(); }));
    }
    assert(!request_failed.load());
    assert(ack_before_return.load());
    assert(drop_while_busy.load());
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return frames.load() >= 2; }));
    }
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return frames.load() >= 3; }));
    }
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.close();
  }
  // slotReleaseStillOnLastReader: host-owned shared slots keep bounded reuse and
  // exact release tokens; a retained lease keeps its mapping after detach.
  {
    const auto boards = root / "shared-pool-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    for (int index = 0; index < 3; ++index) {
      connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return frames.size() > index; }));
      assert(frames[index].descriptor.used_bytes == 1);
      assert(frames[index].lease && frames[index].lease->bytes().size() == 1);
      assert(frames[index].lease->bytes()[0] == 1);
    }
    bool busy = false;
    try { connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); }
    catch (const std::exception& failure) {
      busy = std::string(failure.what()).find("no free slot") != std::string::npos;
    }
    assert(busy);
    {
      std::lock_guard lock(mutex);
      frames.resize(1);
    }
    bool delivered = false;
    for (int attempt = 0; attempt < 100 && !delivered; ++attempt) {
      try { connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); }
      catch (...) { std::this_thread::sleep_for(10ms); continue; }
      std::unique_lock lock(mutex);
      delivered = ready.wait_for(lock, 5s, [&] { return frames.size() > 1; });
    }
    assert(delivered);
    {
      std::lock_guard lock(mutex);
      frames.clear();
    }
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // A retained lease keeps its host mapping after detach and worker shutdown.
  {
    const auto boards = root / "shared-detach-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    std::unique_lock lock(mutex);
    assert(ready.wait_for(lock, 30s, [&] { return !frames.empty(); }));
    auto retained = frames.front();
    frames.clear();
    lock.unlock();
    connection.destroyFramePool(1);
    connection.close();
    assert(retained.lease && retained.lease->bytes().size() == 1);
    assert(retained.lease->bytes()[0] == 1);
  }
  // Invalid metadata releases the slot without delivering or failing the connection.
  {
    const auto boards = root / "shared-malformed-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "MalformedPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    std::this_thread::sleep_for(200ms);
    {
      std::lock_guard lock(mutex);
      assert(frames.empty());
    }
    bool delivered = false;
    for (int attempt = 0; attempt < 100 && !delivered; ++attempt) {
      try { connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); }
      catch (...) { std::this_thread::sleep_for(10ms); continue; }
      std::unique_lock lock(mutex);
      delivered = ready.wait_for(lock, 5s, [&] { return !frames.empty(); });
    }
    assert(delivered && connection.alive());
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // A worker exit with a retained lease leaves the host mapping readable.
  {
    const auto boards = root / "shared-crash-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "CrashPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    std::unique_lock lock(mutex);
    assert(ready.wait_for(lock, 30s, [&] { return !frames.empty(); }));
    auto retained = frames.front();
    lock.unlock();
    try { connection.request(onegen::worker::Operation::Trigger, {}, 5s, 1); } catch (...) {}
    for (int attempt = 0; attempt < 200 && connection.alive(); ++attempt)
      std::this_thread::sleep_for(10ms);
    assert(!connection.alive());
    assert(retained.lease && retained.lease->bytes().size() == 1);
    assert(retained.lease->bytes()[0] == 1);
    connection.close();
  }
  // A detached pool with a retained lease stays charged until the last lease dies.
  {
    const auto boards = root / "shared-budget-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedPoolBudgetBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    onegen::worker::SharedFrame retained;
    {
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return !frames.empty(); }));
      retained = frames.front();
      frames.clear();
    }
    connection.destroyFramePool(1);
    error.clear();
    assert(!connection.createFramePool(1, 3, 4096, error));
    assert(error.find("byte budget exceeded") != std::string::npos);
    retained = {};
    assert(connection.createFramePool(1, 3, 4096, error));
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // Impossible row layouts and unknown PFNC formats fail closed without delivery.
  {
    const auto boards = root / "shared-malformed-format-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "MalformedFormatBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    for (int invalid = 0; invalid < 2; ++invalid) {
      connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
      std::this_thread::sleep_for(200ms);
      std::lock_guard lock(mutex);
      assert(frames.empty());
    }
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    std::unique_lock lock(mutex);
    assert(ready.wait_for(lock, 30s, [&] { return !frames.empty(); }));
    assert(frames.front().descriptor.used_bytes == 1);
    lock.unlock();
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // A malformed descriptor must not poison subsequent valid frame acceptance.
  {
    const auto boards = root / "shared-malformed-sequence-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "MalformedSequenceBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); // malformed sequence
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); // valid sequence 2
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); // valid sequence 3
    std::unique_lock lock(mutex);
    assert(ready.wait_for(lock, 30s, [&] { return frames.size() >= 2; }));
    assert(frames.size() == 2);
    assert(frames[0].descriptor.sequence == 2);
    assert(frames[1].descriptor.sequence == 3);
    lock.unlock();
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // Detach drains an active callback, then discards the already-admitted second frame.
  {
    const auto boards = root / "shared-fence-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedFenceBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::atomic<int> started{0};
    std::promise<void> first_entered;
    auto first_signal = first_entered.get_future();
    std::promise<void> new_entered;
    auto new_signal = new_entered.get_future();
    std::promise<void> release_first;
    auto release = release_first.get_future();
    std::uint64_t first_epoch = 0;
    connection.setSharedFrameCallback([&](const onegen::worker::SharedFrame& frame) {
      if (++started == 1) {
        first_epoch = frame.descriptor.epoch;
        first_entered.set_value();
        release.wait();
      } else if (frame.descriptor.epoch != first_epoch) {
        new_entered.set_value();
      }
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 2, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    assert(first_signal.wait_for(30s) == std::future_status::ready);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    bool second_slot_held = false;
    try { connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1); }
    catch (const std::exception& failure) {
      second_slot_held = std::string(failure.what()).find("no free slot") != std::string::npos;
    }
    assert(second_slot_held);
    std::promise<void> detached;
    auto detach_signal = detached.get_future();
    std::thread detacher([&] {
      connection.destroyFramePool(1);
      detached.set_value();
    });
    bool detach_requested = false;
    for (int attempt = 0; attempt < 300 && !detach_requested; ++attempt) {
      detach_requested = std::filesystem::exists(cti.string() + ".detached");
      if (!detach_requested) std::this_thread::sleep_for(10ms);
    }
    assert(detach_requested);
    assert(detach_signal.wait_for(200ms) == std::future_status::timeout);
    release_first.set_value();
    assert(detach_signal.wait_for(30s) == std::future_status::ready);
    detacher.join();
    assert(connection.createFramePool(1, 1, 4096, error));
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    assert(new_signal.wait_for(30s) == std::future_status::ready);
    assert(started.load() == 2);
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // Duplicate external detach must wait for the same in-flight callback.
  {
    const auto boards = root / "shared-duplicate-detach-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedFenceBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::atomic<int> started{0};
    std::promise<void> entered;
    auto entered_signal = entered.get_future();
    std::promise<void> release_callback;
    auto release = release_callback.get_future();
    connection.setSharedFrameCallback([&](const auto&) {
      if (++started == 1) {
        entered.set_value();
        release.wait();
      }
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 1, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    assert(entered_signal.wait_for(30s) == std::future_status::ready);
    std::promise<void> first_done;
    std::promise<void> second_done;
    auto first = first_done.get_future();
    auto second = second_done.get_future();
    std::thread first_detacher([&] {
      connection.destroyFramePool(1);
      first_done.set_value();
    });
    bool detach_requested = false;
    for (int attempt = 0; attempt < 300 && !detach_requested; ++attempt) {
      detach_requested = std::filesystem::exists(cti.string() + ".detached");
      if (!detach_requested) std::this_thread::sleep_for(10ms);
    }
    assert(detach_requested);
    std::thread second_detacher([&] {
      connection.destroyFramePool(1);
      second_done.set_value();
    });
    assert(first.wait_for(200ms) == std::future_status::timeout);
    assert(second.wait_for(200ms) == std::future_status::timeout);
    release_callback.set_value();
    assert(first.wait_for(30s) == std::future_status::ready);
    assert(second.wait_for(30s) == std::future_status::ready);
    first_detacher.join();
    second_detacher.join();
    assert(started.load() == 1);
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.close();
  }
  // A callback may detach its own pool and an external detach still waits for it.
  {
    const auto boards = root / "shared-reentrant-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::promise<void> internal_detached;
    auto internal_signal = internal_detached.get_future();
    std::promise<void> release_callback;
    auto release = release_callback.get_future();
    connection.setSharedFrameCallback([&](const auto&) {
      connection.destroyFramePool(1);
      internal_detached.set_value();
      release.wait();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 1, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
    assert(internal_signal.wait_for(30s) == std::future_status::ready);
    std::promise<void> external_done;
    auto external = external_done.get_future();
    std::thread external_detacher([&] {
      connection.destroyFramePool(1);
      external_done.set_value();
    });
    assert(external.wait_for(200ms) == std::future_status::timeout);
    release_callback.set_value();
    assert(external.wait_for(30s) == std::future_status::ready);
    external_detacher.join();
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.close();
  }
  // Legacy host acceptance stays separate from direct layout capability.
  {
    const auto boards = root / "layout-class-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "LayoutClassBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<onegen::worker::SharedFrame> frames;
    connection.setSharedFrameCallback([&](const auto& frame) {
      {
        std::lock_guard lock(mutex);
        frames.push_back(frame);
      }
      ready.notify_all();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 1024, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);

    const std::uint64_t formats[] = {0, 0x02180014, 0x02100032, 0x010a0046,
        0x01100007, 0x0110002a, 0x010a0046, 0x021000a5, 0x02180021, 0x01080001};
    const std::uint64_t used[] = {0, 32, 16, 160, 16, 12, 8, 12, 0, 1};
    for (int sequence = 1; sequence <= 9; ++sequence) {
      connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
      if (sequence == 8) {
        std::this_thread::sleep_for(200ms);
        std::lock_guard lock(mutex);
        assert(frames.empty());
        continue;
      }
      std::unique_lock lock(mutex);
      assert(ready.wait_for(lock, 30s, [&] { return !frames.empty(); }));
      const auto frame = frames.front();
      frames.clear();
      lock.unlock();
      assert(frame.lease);
      assert(frame.descriptor.used_bytes == used[sequence]);
      assert(frame.lease->bytes().size() == used[sequence]);
      assert(frame.lease->metadata().pixel_format == formats[sequence]);
      if (sequence == 4) assert(frame.lease->metadata().pixel_endianness == 2);
    }
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // PFNC storage-size validation accepts full rows and rejects short ones.
  {
    const auto boards = root / "shared-format-storage-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "FormatStorageBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::mutex mutex;
    std::vector<std::uint64_t> formats;
    std::promise<void> marker;
    auto marker_signal = marker.get_future();
    connection.setSharedFrameCallback([&](const onegen::worker::SharedFrame& frame) {
      {
        std::lock_guard lock(mutex);
        formats.push_back(frame.descriptor.pixel_format);
      }
      if (frame.descriptor.pixel_format == 0x01080001) marker.set_value();
    }, 1);
    std::string error;
    assert(connection.createFramePool(1, 3, 16384, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    const auto trigger = [&] {
      for (int attempt = 0; attempt < 100; ++attempt) {
        try {
          connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
          return;
        } catch (const std::exception& failure) {
          if (std::string(failure.what()).find("no free slot") == std::string::npos) throw;
          std::this_thread::sleep_for(10ms);
        }
      }
      assert(false);
    };
    for (int index = 0; index < 8; ++index) trigger();
    assert(marker_signal.wait_for(30s) == std::future_status::ready);
    {
      std::lock_guard lock(mutex);
      const std::vector<std::uint64_t> expected{
          0x021000a5, 0x02300018, 0x021400a8, 0x01080001};
      assert(formats == expected);
    }
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // Same-session attach/detach transitions stay ordered under concurrent reconnect.
  {
    const auto boards = root / "shared-lifecycle-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "SharedPoolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::atomic<int> delivered{0};
    connection.setSharedFrameCallback([&](const auto&) { ++delivered; }, 1);
    std::string error;
    assert(connection.createFramePool(1, 1, 4096, error));
    onegen::worker::Writer open;
    open.string("fast");
    open.string("board");
    connection.request(onegen::worker::Operation::Open, open.take(), 30s, 1);
    connection.request(onegen::worker::Operation::StartLive, {}, 30s, 1);
    for (int iteration = 0; iteration < 25; ++iteration) {
      std::promise<void> start;
      auto start_signal = start.get_future().share();
      std::atomic<int> failed_creates{0};
      std::vector<std::thread> threads;
      for (int worker = 0; worker < 2; ++worker) {
        threads.emplace_back([&] {
          start_signal.wait();
          connection.destroyFramePool(1);
          std::string local_error;
          if (!connection.createFramePool(1, 1, 4096, local_error)) ++failed_creates;
        });
      }
      start.set_value();
      for (auto& thread : threads) thread.join();
      assert(failed_creates <= 1);
      connection.request(onegen::worker::Operation::Trigger, {}, 30s, 1);
      for (int wait = 0; wait < 300 && delivered.load() == 0; ++wait)
        std::this_thread::sleep_for(10ms);
      assert(delivered.load() > 0);
      delivered.store(0);
    }
    connection.request(onegen::worker::Operation::StopLive, {}, 30s, 1);
    connection.destroyFramePool(1);
    connection.close();
  }
  // A v15 producer reply is rejected without leaving the request pending.
  {
    const auto boards = root / "legacy-protocol-board";
    std::filesystem::create_directories(boards);
    const auto cti = boards / "LegacyProtocolBoard.cti";
    std::ofstream(cti).put('\0');
    onegen::worker::Connection connection(argv[0], cti);
    std::string failure;
    bool rejected = false;
    try {
      connection.request(onegen::worker::Operation::Probe, {}, 2s);
    } catch (const std::exception& error) {
      rejected = true;
      failure = error.what();
    }
    assert(rejected);
    assert(failure.find("Unsupported OneGen worker protocol version") != std::string::npos);
    assert(!connection.alive());
  }
  std::filesystem::remove_all(root);
  std::cout << "test_worker ok\n";
  return 0;
}

int main(int argc, char** argv)
{
  try { return runTests(argc, argv); }
  catch (const std::exception& error) {
    std::cerr << "test_worker failed: " << error.what();
  } catch (...) {
    std::cerr << "test_worker failed: Unknown C++ exception.";
  }
  for (int index = 1; index < argc; ++index) std::cerr << " arg" << index << '=' << argv[index];
  std::cerr << std::endl;
  return 1;
}
