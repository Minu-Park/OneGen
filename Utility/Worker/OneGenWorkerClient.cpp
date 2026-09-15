#include "OneGenWorkerClient.h"

#include "OneGenWorkerMessages.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>

namespace onegen::worker {

namespace {

struct Outgoing {
  QJsonObject json;
  QByteArray binary;
};

void startWorker(QProcess& process, const std::filesystem::path& executable,
                 const std::filesystem::path& cti, const QString& server_name)
{
  process.start(QString::fromStdString(executable.string()),
                {QStringLiteral("--server"), server_name,
                 QStringLiteral("--cti"), QString::fromStdString(cti.string())});
}

void installThreadDispatcher()
{
  if (QAbstractEventDispatcher::instance()) return;
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start(0);
  loop.exec();
}

bool startAndAccept(QLocalServer& server, QProcess& process,
                    const std::filesystem::path& executable, const std::filesystem::path& cti,
                    const int timeout_ms)
{
  installThreadDispatcher();
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&server, &QLocalServer::newConnection, &loop, &QEventLoop::quit);
  QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
  QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  startWorker(process, executable, cti, server.fullServerName());
  if (!server.hasPendingConnections()) {
    timer.start(timeout_ms);
    loop.exec();
  }
  return server.hasPendingConnections();
}

void killWorker(QProcess& process)
{
  if (process.state() == QProcess::NotRunning) return;
  process.kill();
  process.waitForFinished(5000);
}

void stopWorker(QProcess& process, const int wait_ms)
{
  if (process.state() == QProcess::NotRunning) return;
  if (!process.waitForFinished(wait_ms)) killWorker(process);
}

ProbeResult probeOnce(const std::filesystem::path& worker_executable,
                      const std::filesystem::path& cti,
                      const std::chrono::milliseconds timeout)
{
  ProbeResult result;
  const int timeout_ms = static_cast<int>(std::clamp<std::int64_t>(timeout.count(), 1, 60000));
  const QString server_name = QStringLiteral("onegen-%1-%2")
      .arg(QCoreApplication::applicationPid())
      .arg(QUuid::createUuid().toString(QUuid::Id128));
  QLocalServer::removeServer(server_name);
  QLocalServer server;
  server.setSocketOptions(QLocalServer::UserAccessOption);
  if (!server.listen(server_name)) {
    result.error = server.errorString().toStdString();
    return result;
  }

  QProcess process;
  if (!startAndAccept(server, process, worker_executable, cti, 5000)) {
    result.timed_out = process.state() != QProcess::NotRunning;
    result.error = result.timed_out ? "Worker probe timed out." : process.errorString().toStdString();
    if (result.error.empty()) result.error = "Worker did not connect.";
    killWorker(process);
    return result;
  }

  std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
  if (!socket) {
    result.error = "Worker connected without a local socket.";
    killWorker(process);
    return result;
  }
  socket->write(encode({{QStringLiteral("version"), protocol_version},
                        {QStringLiteral("operation"), QStringLiteral("probe")},
                        {QStringLiteral("timeoutMs"), timeout_ms}}));
  if (!socket->waitForBytesWritten(5000)) {
    result.error = "Could not send the worker probe request.";
  } else {
    QByteArray buffer;
    QString decode_error;
    QJsonObject response;
    QByteArray binary;
    while (true) {
      if (!socket->bytesAvailable() && !socket->waitForReadyRead(100)) {
        process.waitForFinished(0);
        if (process.state() == QProcess::NotRunning) break;
        continue;
      }
      buffer.append(socket->readAll());
      if (decode(buffer, &response, &binary, &decode_error)) {
        result.completed = true;
        result.success = response.value(QStringLiteral("success")).toBool();
        result.error = response.value(QStringLiteral("error")).toString().toStdString();
        for (const auto value : response.value(QStringLiteral("devices")).toArray())
          result.devices.push_back(deviceInfo(value.toObject()));
        break;
      }
      if (!decode_error.isEmpty()) {
        result.error = decode_error.toStdString();
        break;
      }
    }
  }
  if (!result.completed && result.error.empty())
    result.error = "Worker closed without a response.";
  stopWorker(process, 5000);
  return result;
}

} // namespace

ProbeResult probe(const std::filesystem::path& worker_executable,
                  const std::filesystem::path& cti,
                  const std::chrono::milliseconds timeout)
{
  QElapsedTimer elapsed;
  elapsed.start();
  auto result = probeOnce(worker_executable, cti, timeout);
  qInfo().noquote() << QStringLiteral("OneGen probe %1: %2 (%3 ms)")
                           .arg(QString::fromStdString(cti.string()),
                                result.success
                                    ? QStringLiteral("%1 device(s)").arg(result.devices.size())
                                    : QString::fromStdString(result.error.empty() ? "failed"
                                                                                  : result.error),
                                QString::number(elapsed.elapsed()));
  return result;
}

struct Connection::Impl {
  std::filesystem::path executable;
  std::filesystem::path cti;
  QString server_name;
  std::thread io;
  std::thread delivery;
  std::mutex mutex;
  std::condition_variable cv;
  std::queue<Outgoing> outgoing;
  std::queue<Frame> frames;
  std::map<int, Connection::RpcResult> replies;
  int next_id = 1;
  bool running = true;
  std::string start_error;
  bool started = false;
  FrameFn on_frame;
  ErrorFn on_error;
  InvalidateFn on_invalidate;
  QPointer<QLocalSocket> socket;
};

Connection::Connection(std::filesystem::path worker_executable, std::filesystem::path cti)
{
  impl_ = std::make_unique<Impl>();
  impl_->executable = std::move(worker_executable);
  impl_->cti = std::move(cti);
  impl_->server_name = QStringLiteral("onegen-%1-%2")
      .arg(QCoreApplication::applicationPid())
      .arg(QUuid::createUuid().toString(QUuid::Id128));
  impl_->delivery = std::thread([this] {
    while (true) {
      Frame frame;
      FrameFn callback;
      {
        std::unique_lock lock(impl_->mutex);
        impl_->cv.wait(lock, [&] { return !impl_->running || !impl_->frames.empty(); });
        if (!impl_->running && impl_->frames.empty()) return;
        frame = std::move(impl_->frames.front());
        impl_->frames.pop();
        callback = impl_->on_frame;
      }
      if (callback) callback(frame);
      {
        std::lock_guard lock(impl_->mutex);
        impl_->outgoing.push({QJsonObject{{QStringLiteral("operation"), QStringLiteral("frameAck")}}, {}});
      }
      wake();
    }
  });
  impl_->io = std::thread([this] {
    installThreadDispatcher();
    QLocalServer::removeServer(impl_->server_name);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(impl_->server_name)) {
      std::lock_guard lock(impl_->mutex);
      impl_->start_error = server.errorString().toStdString();
      impl_->started = true;
      impl_->cv.notify_all();
      return;
    }
    QProcess process;
    if (!startAndAccept(server, process, impl_->executable, impl_->cti, 5000)) {
      std::lock_guard lock(impl_->mutex);
      impl_->start_error = process.errorString().toStdString();
      if (impl_->start_error.empty()) impl_->start_error = "Worker did not connect.";
      impl_->started = true;
      impl_->running = false;
      impl_->cv.notify_all();
      killWorker(process);
      return;
    }
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    {
      std::lock_guard lock(impl_->mutex);
      impl_->started = true;
      if (!socket) impl_->start_error = "Worker connected without a local socket.";
      impl_->cv.notify_all();
    }
    if (!socket) return;
    QByteArray buffer;
    QEventLoop loop;
    QObject::connect(socket.get(), &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(socket.get(), &QLocalSocket::readyRead, &loop, [this, &buffer, &loop, socket = socket.get()] {
      buffer.append(socket->readAll());
      QJsonObject message;
      QByteArray binary;
      QString error;
      while (decode(buffer, &message, &binary, &error)) {
        const auto operation = message.value(QStringLiteral("operation")).toString();
        if (operation == QLatin1String("frame")) {
          std::lock_guard lock(impl_->mutex);
          impl_->frames.push(frameFromMessage(message, binary));
          impl_->cv.notify_all();
        } else if (operation == QLatin1String("error")) {
          ErrorFn callback;
          {
            std::lock_guard lock(impl_->mutex);
            callback = impl_->on_error;
          }
          if (callback)
            callback(message.value(QStringLiteral("error")).toString().toStdString());
        } else if (operation == QLatin1String("invalidate")) {
          InvalidateFn callback;
          {
            std::lock_guard lock(impl_->mutex);
            callback = impl_->on_invalidate;
          }
          if (callback)
            callback(message.value(QStringLiteral("module")).toInt(),
                     message.value(QStringLiteral("name")).toString().toStdString());
        } else {
          std::lock_guard lock(impl_->mutex);
          impl_->replies[message.value(QStringLiteral("id")).toInt()] = {message, binary};
          impl_->cv.notify_all();
        }
      }
      if (!error.isEmpty()) loop.quit();
    });
    {
      std::lock_guard lock(impl_->mutex);
      impl_->socket = socket.get();
    }
    wake();
    loop.exec();
    {
      std::lock_guard lock(impl_->mutex);
      impl_->socket.clear();
    }
    killWorker(process);
    std::lock_guard lock(impl_->mutex);
    impl_->running = false;
    impl_->cv.notify_all();
  });
  std::unique_lock lock(impl_->mutex);
  impl_->cv.wait(lock, [&] { return impl_->started; });
  if (!impl_->start_error.empty())
    throw std::runtime_error(impl_->start_error);
}

Connection::~Connection()
{
  try {
    rpc({{QStringLiteral("operation"), QStringLiteral("shutdown")}}, std::chrono::seconds{2});
  } catch (...) {
  }
  QPointer<QLocalSocket> socket;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->running = false;
    socket = impl_->socket;
    impl_->cv.notify_all();
  }
  if (socket) QMetaObject::invokeMethod(socket, &QLocalSocket::abort, Qt::QueuedConnection);
  if (impl_->io.joinable()) impl_->io.join();
  if (impl_->delivery.joinable()) impl_->delivery.join();
}

Connection::RpcResult Connection::rpc(QJsonObject request, const std::chrono::milliseconds timeout)
{
  int id = 0;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) throw std::runtime_error("Worker connection is closed.");
    id = impl_->next_id++;
    request.insert(QStringLiteral("version"), protocol_version);
    request.insert(QStringLiteral("id"), id);
    impl_->outgoing.push({std::move(request), {}});
  }
  wake();
  std::unique_lock lock(impl_->mutex);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  if (!impl_->cv.wait_until(lock, deadline, [&] {
        return impl_->replies.count(id) || !impl_->running;
      }))
    throw std::runtime_error("Worker request timed out.");
  const auto it = impl_->replies.find(id);
  if (it == impl_->replies.end()) throw std::runtime_error("Worker connection is closed.");
  auto response = std::move(it->second);
  impl_->replies.erase(it);
  lock.unlock();
  if (!response.json.value(QStringLiteral("success")).toBool())
    throw std::runtime_error(response.json.value(QStringLiteral("error")).toString().toStdString());
  return response;
}

void Connection::wake()
{
  QPointer<QLocalSocket> socket;
  {
    std::lock_guard lock(impl_->mutex);
    socket = impl_->socket;
  }
  if (!socket) return;
  QMetaObject::invokeMethod(socket, [this, socket] {
    if (!socket) return;
    std::queue<Outgoing> pending;
    {
      std::lock_guard lock(impl_->mutex);
      pending.swap(impl_->outgoing);
    }
    while (!pending.empty()) {
      auto message = std::move(pending.front());
      pending.pop();
      socket->write(encode(message.json, message.binary));
    }
  }, Qt::QueuedConnection);
}

void Connection::set_frame_callback(FrameFn cb)
{
  std::lock_guard lock(impl_->mutex);
  impl_->on_frame = std::move(cb);
}

void Connection::set_error_callback(ErrorFn cb)
{
  std::lock_guard lock(impl_->mutex);
  impl_->on_error = std::move(cb);
}

void Connection::set_invalidate_callback(InvalidateFn cb)
{
  std::lock_guard lock(impl_->mutex);
  impl_->on_invalidate = std::move(cb);
}

} // namespace onegen::worker
