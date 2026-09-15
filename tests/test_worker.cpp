#include "OneGenWorkerClient.h"
#include "OneGenCoordinator.h"
#include "OneGenWorkerProtocol.h"

#include <QCoreApplication>
#include <QThread>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

int main(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  if (application.arguments().contains(QStringLiteral("--server"))) {
    QThread::msleep(500);
    return 0;
  }
  assert(argc == 2);
  const auto temp = std::filesystem::temp_directory_path() / "onegen-worker-invalid.cti";
  std::ofstream(temp).put('\0');

  const auto invalid = onegen::worker::probe(argv[1], temp, std::chrono::seconds(2));
  assert(invalid.completed);
  assert(!invalid.success);
  assert(!invalid.timed_out);

  bool off_thread_completed = false;
  bool off_thread_success = true;
  std::unique_ptr<QThread> off_thread(QThread::create([&] {
    const auto result = onegen::worker::probe(argv[1], temp, std::chrono::seconds(2));
    off_thread_completed = result.completed;
    off_thread_success = result.success;
  }));
  off_thread->start();
  off_thread->wait();
  assert(off_thread_completed);
  assert(!off_thread_success);

  const auto disconnected = onegen::worker::probe(
      QCoreApplication::applicationFilePath().toStdString(), temp, std::chrono::seconds(2));
  assert(!disconnected.completed);
  assert(!disconnected.timed_out);

  onegen::DeviceInfo first;
  first.id = "first";
  first.serial = "123";
  first.tl_type = "GEV";
  first.cti_path = "b/Producer.cti";
  onegen::DeviceInfo preferred = first;
  preferred.id = "preferred";
  preferred.cti_path = "a/Producer.cti";
  onegen::DeviceInfo second = first;
  second.id = "second";
  second.serial = "456";
  const auto merged = onegen::worker::Coordinator::merge({first, preferred, second});
  assert(merged.size() == 2);
  assert(merged.front().id == "preferred");
  assert(merged.front().cti_candidates.size() == 2);
  assert(merged.front().cti_candidates.front() == "a/Producer.cti");
  assert(merged.front().cti_candidates.back() == "b/Producer.cti");

  onegen::DeviceInfo ip_first;
  ip_first.id = "ip-b";
  ip_first.ip = "10.0.0.8";
  ip_first.tl_type = "GEV";
  ip_first.cti_path = "b/Producer.cti";
  onegen::DeviceInfo ip_preferred = ip_first;
  ip_preferred.id = "ip-a";
  ip_preferred.cti_path = "a/Producer.cti";
  const auto ip_merged = onegen::worker::Coordinator::merge({ip_first, ip_preferred});
  assert(ip_merged.size() == 1);
  assert(ip_merged.front().id == "ip-a");

  onegen::DeviceInfo id_first;
  id_first.id = "same";
  id_first.tl_type = "U3V";
  id_first.cti_path = "b/Producer.cti";
  onegen::DeviceInfo id_preferred = id_first;
  id_preferred.cti_path = "a/Producer.cti";
  const auto id_merged = onegen::worker::Coordinator::merge({id_first, id_preferred});
  assert(id_merged.size() == 1);
  assert(id_merged.front().cti_path == "a/Producer.cti");

  const auto root = temp.parent_path() / "onegen-worker-ctis";
  std::filesystem::create_directories(root / "vendor-a");
  std::filesystem::create_directories(root / "vendor-b");
  std::ofstream(root / "vendor-a" / "Producer.cti").put('\0');
  std::ofstream(root / "vendor-b" / "Producer.cti").put('\0');
  onegen::worker::Coordinator coordinator(argv[1], root, std::chrono::seconds(2));
  const auto discovery = coordinator.discover();
  assert(discovery.completed);
  assert(discovery.devices.empty());
  assert(discovery.failures.size() == 2);

  {
    QByteArray binary("raw-frame");
    auto encoded = onegen::worker::encode(
        {{QStringLiteral("operation"), QStringLiteral("frame")}}, binary);
    QJsonObject message;
    QByteArray decoded;
    QString error;
    assert(onegen::worker::decode(encoded, &message, &decoded, &error));
    assert(decoded == binary);
    assert(message.value(QStringLiteral("binarySize")).toInt() == binary.size());
  }

  {
    QByteArray binary(17 * 1024 * 1024, 'x');
    auto encoded = onegen::worker::encode(
        {{QStringLiteral("operation"), QStringLiteral("frame")}}, binary);
    QJsonObject message;
    QByteArray decoded;
    QString error;
    assert(onegen::worker::decode(encoded, &message, &decoded, &error));
    assert(decoded == binary);
  }

  {
    onegen::worker::Connection connection(argv[1], temp);
    bool failed = false;
    try {
      connection.rpc({{QStringLiteral("operation"), QStringLiteral("open")},
                      {QStringLiteral("deviceId"), QStringLiteral("missing")}},
                     std::chrono::seconds{3});
    } catch (const std::exception&) {
      failed = true;
    }
    assert(failed);
  }

  std::filesystem::remove(temp);
  std::filesystem::remove_all(root);
  std::cout << "test_worker ok\n";
  return 0;
}
