#include "OneGenWorkerClient.h"

#include "OneGenWorkerMessages.h"

#include <QDir>
#include <QDebug>
#include <QProcess>
#include <QProcessEnvironment>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace onegen::worker {

namespace {

QByteArray framed(Message message)
{
  const auto body = encode(std::move(message));
  const auto header = frameSize(static_cast<std::uint32_t>(body.size()));
  QByteArray bytes(reinterpret_cast<const char*>(header.data()), header.size());
  bytes.append(reinterpret_cast<const char*>(body.data()), static_cast<qsizetype>(body.size()));
  return bytes;
}

bool takeMessage(QByteArray& buffer, Message& message)
{
  if (buffer.size() < 4) return false;
  std::array<std::byte, 4> header;
  std::memcpy(header.data(), buffer.constData(), header.size());
  const auto size = frameSize(header);
  if (size > maximum_message_size) throw std::runtime_error("OneGen worker message is too large.");
  if (buffer.size() < 4 + static_cast<qsizetype>(size)) return false;
  std::vector<std::byte> body(size);
  std::memcpy(body.data(), buffer.constData() + 4, size);
  buffer.remove(0, 4 + static_cast<qsizetype>(size));
  message = decode(body);
  return true;
}

} // namespace

struct Connection::Impl {
  std::filesystem::path executable;
  std::filesystem::path cti;
  std::thread io;
  std::thread delivery;
  mutable std::mutex mutex;
  std::mutex close_mutex;
  std::condition_variable cv;
  std::queue<Message> outgoing;
  std::queue<std::pair<std::uint32_t, Frame>> frames;
  std::map<std::uint32_t, Message> replies;
  std::uint32_t next_id = 1;
  bool running = true;
  bool started = false;
  std::string start_error;
  std::string close_error;
  struct Callbacks { FrameFn frame; ErrorFn error; InvalidateFn invalidate; };
  std::map<std::uint32_t, Callbacks> callbacks;
  ClaimFn claim;
  std::map<std::string, std::shared_ptr<void>> interface_claims;
#ifdef _WIN32
  HANDLE job = nullptr;
#endif
};

Connection::Connection(std::filesystem::path worker_executable, std::filesystem::path cti, ClaimFn claim)
    : impl_(std::make_unique<Impl>())
{
  impl_->executable = std::move(worker_executable);
  impl_->cti = std::move(cti);
  impl_->claim = std::move(claim);
  impl_->delivery = std::thread([this] {
    while (true) {
      Frame frame;
      std::uint32_t session = 0;
      FrameFn callback;
      {
        std::unique_lock lock(impl_->mutex);
        impl_->cv.wait(lock, [&] { return !impl_->running || !impl_->frames.empty(); });
        if (!impl_->running && impl_->frames.empty()) return;
        session = impl_->frames.front().first;
        frame = std::move(impl_->frames.front().second);
        impl_->frames.pop();
        callback = impl_->callbacks[session].frame;
        if (!impl_->running) callback = {};
      }
      try { if (callback) callback(frame); } catch (...) { qWarning() << "[GenICam] frame consumer failed"; }
      {
        std::lock_guard lock(impl_->mutex);
        if (impl_->running) { Message ack{Operation::FrameAck}; ack.session = session; impl_->outgoing.push(std::move(ack)); }
      }
    }
  });
  impl_->io = std::thread([this] {
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    const auto separator = QDir::listSeparator();
    const auto worker_runtime = QString::fromStdString(
        (impl_->executable.parent_path() / "runtime").string());
    const auto producer_runtime = QString::fromStdString(impl_->cti.parent_path().string());
    environment.insert(QStringLiteral("PATH"),
                       worker_runtime + separator + producer_runtime + separator
                           + environment.value(QStringLiteral("PATH")));
    process.setProcessEnvironment(environment);
    QStringList arguments{QStringLiteral("--cti"), QString::fromStdString(impl_->cti.string())};
    if (impl_->claim) arguments.append(QStringLiteral("--resource-claims"));
    process.start(QString::fromStdString(impl_->executable.string()), arguments);
    {
      std::lock_guard lock(impl_->mutex);
      impl_->started = true;
      if (!process.waitForStarted(5000)) {
        impl_->start_error = process.errorString().toStdString();
        impl_->running = false;
      } else {
        qInfo().noquote() << QStringLiteral("[GenICam] worker process started: pid=%1 cti=%2")
                               .arg(process.processId())
                               .arg(QString::fromStdString(impl_->cti.string()));
#ifdef _WIN32
        impl_->job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        const auto process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                                FALSE, process.processId());
        if (!impl_->job
            || !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                        &limits, sizeof(limits))
            || !process_handle
            || !AssignProcessToJobObject(impl_->job, process_handle)) {
          if (impl_->job) CloseHandle(impl_->job);
          impl_->job = nullptr;
        }
        if (process_handle) CloseHandle(process_handle);
#endif
      }
      impl_->cv.notify_all();
    }
    if (process.state() == QProcess::NotRunning) return;

    QByteArray buffer;
    QByteArray diagnostic;
    QByteArray timing_lines;
    while (process.state() != QProcess::NotRunning) {
      std::queue<Message> pending;
      {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) break;
        pending.swap(impl_->outgoing);
      }
      while (!pending.empty()) {
        process.write(framed(std::move(pending.front())));
        pending.pop();
      }
      process.waitForBytesWritten(10);
      process.waitForReadyRead(10);
      try {
        buffer.append(process.readAllStandardOutput());
        const auto stderr_bytes = process.readAllStandardError();
        diagnostic.append(stderr_bytes);
        timing_lines.append(stderr_bytes);
        for (auto newline = timing_lines.indexOf('\n'); newline >= 0; newline = timing_lines.indexOf('\n')) {
          const auto line = timing_lines.left(newline).trimmed();
          timing_lines.remove(0, newline + 1);
          if (line.startsWith("[GenICam]"))
            qInfo().noquote() << QString::fromUtf8(line) << "cti=" << QString::fromStdString(impl_->cti.string());
        }
        if (timing_lines.size() > 65536) timing_lines.clear();
        if (diagnostic.size() > 65536) diagnostic.remove(0, diagnostic.size() - 65536);
        Message message;
        while (takeMessage(buffer, message)) {
          if (message.operation == Operation::ClaimInterface) {
            Reader reader(message.payload);
            const auto info = readInterfaceInfo(reader);
            const auto& id = info.id;
            Message reply{Operation::ClaimInterface};
            try { impl_->interface_claims[id] = impl_->claim ? impl_->claim(info) : std::shared_ptr<void>{}; }
            catch (const std::exception& error) { reply.success = false; reply.error = std::string("Interface resource busy: ") + error.what(); }
            catch (...) { reply.success = false; reply.error = "Interface resource claim failed."; }
            process.write(framed(std::move(reply)));
          } else if (message.operation == Operation::ReleaseInterfaceClaim) {
            Reader reader(message.payload);
            impl_->interface_claims.erase(reader.string());
          } else if (message.operation == Operation::Frame) {
            Reader reader(message.payload);
            std::lock_guard lock(impl_->mutex);
            impl_->frames.emplace(message.session, readFrame(reader));
            impl_->cv.notify_all();
          } else if (message.operation == Operation::Error) {
            ErrorFn callback;
            { std::lock_guard lock(impl_->mutex); callback = impl_->callbacks[message.session].error; }
            if (callback) callback(message.error);
            else qWarning().noquote() << "[GenICam]" << QString::fromStdString(message.error);
          } else if (message.operation == Operation::Invalidate) {
            Reader reader(message.payload);
            const auto module = reader.scalar<std::int32_t>();
            const auto name = reader.string();
            InvalidateFn callback;
            { std::lock_guard lock(impl_->mutex); callback = impl_->callbacks[message.session].invalidate; }
            if (callback) callback(module, name);
          } else {
            std::lock_guard lock(impl_->mutex);
            impl_->replies[message.id] = std::move(message);
            impl_->cv.notify_all();
          }
        }
      } catch (const std::exception& error) {
        {
          std::lock_guard lock(impl_->mutex);
          impl_->close_error = error.what();
          impl_->running = false;
          impl_->cv.notify_all();
        }
        process.kill();
        break;
      }
    }
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(5000);
    }
    impl_->interface_claims.clear();
    {
      diagnostic.append(process.readAllStandardError());
      QString last_error;
      auto remainder = QString::fromUtf8(diagnostic);
      for (auto line : remainder.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("[GenICam]"))) continue;
        last_error = line;
      }
      const auto crashed = process.exitStatus() == QProcess::CrashExit;
      qWarning().noquote()
          << QStringLiteral("[GenICam] worker process exited: pid=%1 code=%2 crashed=%3 cti=%4")
                 .arg(process.processId())
                 .arg(process.exitCode())
                 .arg(crashed ? QStringLiteral("yes") : QStringLiteral("no"))
                 .arg(QString::fromStdString(impl_->cti.string()));
      std::lock_guard lock(impl_->mutex);
      if (impl_->close_error.empty()) {
        impl_->close_error = crashed ? "Worker process crashed." : "Worker process exited.";
        if (!last_error.isEmpty()) {
          impl_->close_error += ' ';
          impl_->close_error += last_error.toStdString();
        }
      }
      impl_->running = false;
      impl_->cv.notify_all();
    }
  });
  std::unique_lock lock(impl_->mutex);
  impl_->cv.wait(lock, [&] { return impl_->started; });
  if (!impl_->start_error.empty()) {
    const auto error = impl_->start_error;
    lock.unlock();
    if (impl_->io.joinable()) impl_->io.join();
    if (impl_->delivery.joinable()) impl_->delivery.join();
    throw std::runtime_error(error);
  }
}

Connection::~Connection()
{
  close();
}

void Connection::close()
{
  std::lock_guard closing(impl_->close_mutex);
#ifndef _WIN32
  try { request(Operation::Shutdown, {}, std::chrono::milliseconds{500}); } catch (...) {}
#else
  const auto job = std::exchange(impl_->job, nullptr);
  if (job) CloseHandle(job);
#endif
  {
    std::lock_guard lock(impl_->mutex);
    impl_->running = false;
    impl_->cv.notify_all();
  }
  if (impl_->io.joinable()) impl_->io.join();
  if (impl_->delivery.joinable()) impl_->delivery.join();
}

std::vector<std::byte> Connection::request(Operation operation, std::vector<std::byte> payload,
                                            const std::chrono::milliseconds timeout, std::uint32_t session)
{
  std::uint32_t id;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) {
      throw std::runtime_error(impl_->close_error.empty()
                                   ? "Worker connection is closed."
                                   : impl_->close_error);
    }
    id = impl_->next_id++;
    impl_->outgoing.push({operation, id, true, {}, std::move(payload), session});
  }
  std::unique_lock lock(impl_->mutex);
  if (!impl_->cv.wait_for(lock, timeout, [&] { return impl_->replies.count(id) || !impl_->running; }))
    throw std::runtime_error("Worker request timed out.");
  const auto found = impl_->replies.find(id);
  if (found == impl_->replies.end()) {
    throw std::runtime_error(impl_->close_error.empty()
                                 ? "Worker connection is closed."
                                 : impl_->close_error);
  }
  auto reply = std::move(found->second);
  impl_->replies.erase(found);
  if (!reply.success) throw std::runtime_error(reply.error);
  return std::move(reply.payload);
}

bool Connection::alive() const
{
  std::lock_guard lock(impl_->mutex);
  return impl_->running;
}

void Connection::set_frame_callback(FrameFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].frame = std::move(callback); }
void Connection::set_error_callback(ErrorFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].error = std::move(callback); }
void Connection::set_invalidate_callback(InvalidateFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].invalidate = std::move(callback); }

} // namespace onegen::worker
