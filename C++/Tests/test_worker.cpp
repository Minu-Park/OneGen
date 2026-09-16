#include "OneGenCoordinator.h"
#include "OneGenWorkerClient.h"
#include "OneGenWorkerMessages.h"

#include <QCoreApplication>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

// One executable also acts as a hardware-free protocol peer for completion-order checks.
int fakeWorker(const std::filesystem::path& cti)
{
  using namespace onegen::worker;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::array<std::byte, 4> header;
  while (std::cin.read(reinterpret_cast<char*>(header.data()), header.size())) {
    const auto size = frameSize(header);
    if (size > maximum_message_size) return 1;
    std::vector<std::byte> bytes(size);
    if (!std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return 1;
    const auto request = decode(bytes);
    Writer payload;
    if (request.operation == Operation::Probe) {
      const bool slow = cti.parent_path().filename() == "vendor-a";
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
  assert(argc == 2);

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

  const auto root = std::filesystem::temp_directory_path() / "onegen-worker-ctis";
  std::filesystem::create_directories(root / "vendor-a");
  std::filesystem::create_directories(root / "vendor-b");
  std::ofstream(root / "vendor-a" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti").put('\0');
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
  progressive.shutdown();

  std::filesystem::remove_all(root);
  std::cout << "test_worker ok\n";
}
