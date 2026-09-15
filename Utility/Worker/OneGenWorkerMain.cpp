#include "OneGenWorkerMessages.h"

#include "onegen/onegen.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <utility>

namespace {

struct Outgoing {
  QJsonObject json;
  QByteArray binary;
};

struct LiveState {
  std::mutex mutex;
  std::condition_variable ack;
  std::queue<Outgoing> out;
  bool acked = false;
  bool stop = false;
};

int moduleIndex(onegen::Camera::Module module)
{
  switch (module) {
    case onegen::Camera::Module::RemoteDevice: return 0;
    case onegen::Camera::Module::DataStream: return 1;
    case onegen::Camera::Module::Device: return 2;
    default: return 3;
  }
}

onegen::Camera::Module moduleFromIndex(const int index)
{
  if (index == 0) return onegen::Camera::Module::RemoteDevice;
  if (index == 1) return onegen::Camera::Module::DataStream;
  if (index == 2) return onegen::Camera::Module::Device;
  return onegen::Camera::Module::Interface;
}

} // namespace

int main(int argc, char** argv)
{
  QCoreApplication application(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption({QStringLiteral("server"), QStringLiteral("Coordinator server name."), QStringLiteral("name")});
  parser.addOption({QStringLiteral("cti"), QStringLiteral("Single CTI owned by this worker."), QStringLiteral("path")});
  parser.process(application);
  if (!parser.isSet(QStringLiteral("server")) || !parser.isSet(QStringLiteral("cti"))) return 2;

  QLocalSocket socket;
  QEventLoop connect_loop;
  QTimer connect_timer;
  connect_timer.setSingleShot(true);
  QObject::connect(&socket, &QLocalSocket::connected, &connect_loop, &QEventLoop::quit);
  QObject::connect(&connect_timer, &QTimer::timeout, &connect_loop, &QEventLoop::quit);
  socket.connectToServer(parser.value(QStringLiteral("server")));
  if (socket.state() != QLocalSocket::ConnectedState) {
    connect_timer.start(5000);
    connect_loop.exec();
  }
  if (socket.state() != QLocalSocket::ConnectedState) return 3;

  onegen::OneGen system;
  onegen::Camera* camera = nullptr;
  LiveState live;
  std::function<void()> flush;
  const auto enqueue = [&](QJsonObject json, QByteArray binary = {}) {
    {
      std::lock_guard lock(live.mutex);
      live.out.push({std::move(json), std::move(binary)});
    }
    QMetaObject::invokeMethod(&socket, [&flush] { flush(); }, Qt::QueuedConnection);
  };
  const auto ensure_camera = [&]() -> onegen::Camera& {
    if (!camera) {
      system.loadCti(parser.value(QStringLiteral("cti")).toStdString());
      camera = &system.addCamera();
      camera->set_frame_callback([&](const onegen::Frame& frame) {
        auto [meta, binary] = onegen::worker::frameMessage(frame);
        std::unique_lock lock(live.mutex);
        live.out.push({std::move(meta), std::move(binary)});
        live.acked = false;
        QMetaObject::invokeMethod(&socket, [&flush] { flush(); }, Qt::QueuedConnection);
        live.ack.wait(lock, [&] { return live.acked || live.stop; });
      });
      camera->set_error_callback([&](const std::string_view error) {
        enqueue({{QStringLiteral("operation"), QStringLiteral("error")},
                 {QStringLiteral("error"),
                  QString::fromLocal8Bit(error.data(), static_cast<qsizetype>(error.size()))}});
      });
    }
    return *camera;
  };
  const auto wire_invalidates = [&](onegen::Camera& opened) {
    for (const auto module : {onegen::Camera::Module::RemoteDevice,
                              onegen::Camera::Module::DataStream,
                              onegen::Camera::Module::Device,
                              onegen::Camera::Module::Interface}) {
      opened.features(module).set_invalidate_callback(
          [&, index = moduleIndex(module)](std::string_view name) {
            enqueue({{QStringLiteral("operation"), QStringLiteral("invalidate")},
                     {QStringLiteral("module"), index},
                     {QStringLiteral("name"), QString::fromUtf8(name.data(), static_cast<int>(name.size()))}});
          });
    }
  };

  flush = [&] {
    std::queue<Outgoing> pending;
    {
      std::lock_guard lock(live.mutex);
      pending.swap(live.out);
    }
    while (!pending.empty()) {
      auto message = std::move(pending.front());
      pending.pop();
      socket.write(onegen::worker::encode(message.json, message.binary));
    }
  };

  QByteArray buffer;
  QObject::connect(&socket, &QLocalSocket::readyRead, &application, [&] {
    buffer.append(socket.readAll());
    QJsonObject request;
    QByteArray binary;
    QString protocol_error;
    while (onegen::worker::decode(buffer, &request, &binary, &protocol_error)) {
      const auto operation = request.value(QStringLiteral("operation")).toString();
      if (operation == QLatin1String("frameAck")) {
        std::lock_guard lock(live.mutex);
        live.acked = true;
        live.ack.notify_all();
        continue;
      }
      QJsonObject response{{QStringLiteral("version"), onegen::worker::protocol_version},
                           {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                           {QStringLiteral("operation"), operation},
                           {QStringLiteral("success"), true}};
      bool shutdown = false;
      bool binary_response = false;
      try {
        if (request.value(QStringLiteral("version")).toInt() != onegen::worker::protocol_version)
          throw std::runtime_error("Unsupported worker request.");
        auto& device = ensure_camera();
        if (operation == QLatin1String("shutdown")) {
          {
            std::lock_guard lock(live.mutex);
            live.stop = true;
            live.ack.notify_all();
          }
          device.stop();
          device.close();
          shutdown = true;
        }
        else if (operation == QLatin1String("probe")) {
          const int timeout_ms = std::clamp(request.value(QStringLiteral("timeoutMs")).toInt(), 1, 60000);
          QJsonArray values;
          for (const auto& found : system.enumerate(std::chrono::milliseconds(timeout_ms)))
            values.append(onegen::worker::deviceJson(found));
          response.insert(QStringLiteral("devices"), values);
        } else if (operation == QLatin1String("open")) {
          if (device.is_open()) device.close();
          device.open(request.value(QStringLiteral("deviceId")).toString().toStdString());
          device.prepare();
          wire_invalidates(device);
          response.insert(QStringLiteral("device"), onegen::worker::deviceJson(device.info()));
        } else if (operation == QLatin1String("close")) {
          device.stop();
          device.close();
        } else if (operation == QLatin1String("listStreams")) {
          QJsonArray values;
          for (const auto& stream : device.data_streams())
            values.append(QJsonObject{{QStringLiteral("index"), static_cast<int>(stream.index)},
                                      {QStringLiteral("id"), QString::fromStdString(stream.id)}});
          response.insert(QStringLiteral("streams"), values);
        } else if (operation == QLatin1String("selectStream")) {
          device.select_data_stream(
              static_cast<std::uint32_t>(request.value(QStringLiteral("index")).toInt()));
          device.prepare();
        } else if (operation == QLatin1String("describeFeatures")) {
          QJsonArray modules;
          for (const auto module : {onegen::Camera::Module::RemoteDevice,
                                    onegen::Camera::Module::DataStream,
                                    onegen::Camera::Module::Device,
                                    onegen::Camera::Module::Interface}) {
            modules.append(QJsonObject{
                {QStringLiteral("index"), moduleIndex(module)},
                {QStringLiteral("root"), onegen::worker::featureJson(onegen::snapshot(device.features(module).root()))}});
          }
          response.insert(QStringLiteral("modules"), modules);
        } else if (operation == QLatin1String("nodeRead")) {
          const auto module = moduleFromIndex(request.value(QStringLiteral("module")).toInt());
          const auto node = device.features(module).find(
              request.value(QStringLiteral("name")).toString().toStdString());
          response.insert(QStringLiteral("node"), onegen::worker::featureJson(onegen::snapshot(node)));
        } else if (operation == QLatin1String("nodeWrite")) {
          const auto module = moduleFromIndex(request.value(QStringLiteral("module")).toInt());
          auto node = device.features(module).find(
              request.value(QStringLiteral("name")).toString().toStdString());
          onegen::apply_feature(node, request.value(QStringLiteral("op")).toString().toStdString(),
                                request.value(QStringLiteral("value")).toString().toStdString());
          response.insert(QStringLiteral("node"), onegen::worker::featureJson(onegen::snapshot(node)));
        } else if (operation == QLatin1String("grabOne")) {
          const auto frames = device.single(1);
          if (frames.empty()) throw std::runtime_error("Single acquisition returned no frame.");
          auto [meta, payload] = onegen::worker::frameMessage(frames.front());
          response.insert(QStringLiteral("frame"), meta);
          socket.write(onegen::worker::encode(response, payload));
          binary_response = true;
        } else if (operation == QLatin1String("startLive")) {
          {
            std::lock_guard lock(live.mutex);
            live.stop = false;
          }
          device.live();
        } else if (operation == QLatin1String("stopLive")) {
          {
            std::lock_guard lock(live.mutex);
            live.stop = true;
            live.ack.notify_all();
          }
          device.stop();
        } else if (operation == QLatin1String("trigger")) {
          device.trigger();
        } else {
          throw std::runtime_error("Unsupported worker request.");
        }
      } catch (const std::exception& error) {
        response.insert(QStringLiteral("success"), false);
        response.insert(QStringLiteral("error"), QString::fromLocal8Bit(error.what()));
      }
      if (!binary_response) socket.write(onegen::worker::encode(response));
      if (shutdown) {
        socket.flush();
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
        return;
      }
    }
    if (!protocol_error.isEmpty()) application.quit();
  });
  QObject::connect(&socket, &QLocalSocket::disconnected,
                   &application, &QCoreApplication::quit);
  QTimer::singleShot(0, &socket, [&] {
    flush();
  });
  application.exec();
  {
    std::lock_guard lock(live.mutex);
    live.stop = true;
    live.ack.notify_all();
  }
  if (camera) camera->stop();
  return 0;
}
