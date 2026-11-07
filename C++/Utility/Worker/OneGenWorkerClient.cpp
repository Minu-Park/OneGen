#include "OneGenWorkerClient.h"

#include "OneGenWorkerMessages.h"
#include "WorkerLogForwarder.h"
#include "WorkerException.h"
#include "Internal/FrameTrace.h"
#include "Internal/SharedMemoryRegion.h"

#include <QDir>
#include <QDebug>
#include <QProcess>
#include <QProcessEnvironment>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <random>
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

struct MessageTiming {
  double extract_ms = 0;
  double decode_ms = 0;
};

bool takeMessage(QByteArray& buffer, Message& message, MessageTiming* timing = nullptr)
{
  if (buffer.size() < 4) return false;
  std::array<std::byte, 4> header;
  std::memcpy(header.data(), buffer.constData(), header.size());
  const auto size = frameSize(header);
  if (size > maximum_message_size) throw std::runtime_error("OneGen worker message is too large.");
  if (buffer.size() < 4 + static_cast<qsizetype>(size)) return false;
  const auto extract_started = timing ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
  std::vector<std::byte> body(size);
  std::memcpy(body.data(), buffer.constData() + 4, size);
  buffer.remove(0, 4 + static_cast<qsizetype>(size));
  const auto extracted = timing ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  message = decode(body);
  if (timing) {
    const auto decoded = std::chrono::steady_clock::now();
    timing->extract_ms = onegen::frame_trace_ms(extract_started, extracted);
    timing->decode_ms = onegen::frame_trace_ms(extracted, decoded);
  }
  return true;
}

/** @brief One delivered-queue entry retains the producer identity and host-side stage samples. */
struct PendingFrame {
  std::uint32_t session = 0;
  Frame frame;
  std::chrono::steady_clock::time_point received;
  double read_ms = 0;
  double extract_ms = 0;
  double decode_ms = 0;
  double payload_ms = 0;
};

} // namespace

/** @brief Queues exact slot releases without blocking an arbitrary destructor thread. */
struct ReleaseMailbox {
  std::mutex mutex;
  std::queue<Message> messages;
  bool closed = false;
};

/** @brief Detach fence shared by a pool, its leases and queued deliveries. */
struct PoolValidity {
  std::atomic<bool> detached{false};
  std::mutex mutex;
  std::condition_variable cv;
  int active_callbacks = 0;
  std::thread::id delivery_thread;

  /** @brief Registers a callback before it starts; false once the pool is detached. */
  bool beginDelivery() noexcept
  {
    std::lock_guard lock(mutex);
    if (detached.load(std::memory_order_acquire)) return false;
    ++active_callbacks;
    delivery_thread = std::this_thread::get_id();
    return true;
  }

  void endDelivery() noexcept
  {
    std::lock_guard lock(mutex);
    if (active_callbacks > 0) --active_callbacks;
    cv.notify_all();
  }

  /** @brief Invalidates the epoch without waiting inside a callback. */
  void invalidate() noexcept
  {
    std::lock_guard lock(mutex);
    detached.store(true, std::memory_order_release);
  }

  /** @brief Waits for started callbacks; a callback that detaches re-enters safely. */
  void waitForCallbacks()
  {
    std::unique_lock lock(mutex);
    if (!active_callbacks) return;
    if (delivery_thread == std::this_thread::get_id()) return;
    cv.wait(lock, [&] { return active_callbacks == 0; });
  }
};

/** @brief Per-session accounting for active and retired lease-backed pool mappings. */
struct FramePoolBudget {
  std::mutex mutex;
  std::map<std::uint32_t, std::uint64_t> used;
};

/** @brief Keeps reserved bytes charged until the last pool or lease owner dies. */
struct PoolBudgetReservation {
  std::shared_ptr<FramePoolBudget> budget;
  std::uint32_t session = 0;
  std::uint64_t bytes = 0;
  ~PoolBudgetReservation()
  {
    if (!budget || !bytes) return;
    std::lock_guard lock(budget->mutex);
    const auto found = budget->used.find(session);
    if (found == budget->used.end()) return;
    if (found->second <= bytes) budget->used.erase(found);
    else found->second -= bytes;
  }
};

struct FramePoolState {
  std::string name;
  std::shared_ptr<onegen::SharedMemoryRegion> region;
  std::uint64_t epoch = 0;
  std::uint64_t token = 0;
  std::uint32_t slot_count = 0;
  std::uint64_t slot_bytes = 0;
  std::shared_ptr<ReleaseMailbox> mailbox;
  std::shared_ptr<PoolValidity> validity;
  std::shared_ptr<PoolBudgetReservation> reservation;
  std::mutex mutex;
  std::map<std::uint32_t, std::uint64_t> last_generation;
  std::uint64_t last_sequence = 0;
};

namespace {

/** @brief Enqueues a release for a validated slot identity without creating a consumer lease. */
void releaseSharedSlot(const std::shared_ptr<FramePoolState>& pool, const FrameDescriptor& descriptor)
{
  if (!pool->mailbox || (pool->validity && pool->validity->detached.load())) return;
  std::lock_guard lock(pool->mailbox->mutex);
  if (pool->mailbox->closed) return;
  try {
    Writer out;
    write(out, SlotRelease{descriptor.epoch, descriptor.token, descriptor.slot,
                          descriptor.generation});
    Message release{Operation::SlotRelease, 0, true, {}, out.take(), descriptor.session};
    pool->mailbox->messages.push(std::move(release));
  } catch (...) {
  }
}

/** @brief Returns the packed bits per pixel for PFNC formats the shared image path supports. */
bool sharedImageBitsPerPixel(const std::uint64_t format, std::uint32_t& bits, bool& ycbcr) noexcept
{
  ycbcr = format == 0x0210001f || format == 0x02100032;
  switch (static_cast<std::uint32_t>(format)) {
    case 0x01080001:
    case 0x01080008:
    case 0x01080009:
    case 0x0108000a:
    case 0x0108000b:
    case 0x021000a5:
    case 0x021000a6:
      bits = 8;
      return true;
    case 0x01100003:
    case 0x01100005:
    case 0x01100007:
    case 0x01100025:
    case 0x0110000c:
    case 0x0110000d:
    case 0x0110000e:
    case 0x0110000f:
    case 0x01100010:
    case 0x01100011:
    case 0x01100012:
    case 0x01100013:
    case 0x0110002a:
    case 0x0110002b:
    case 0x0110002c:
    case 0x0110002d:
    case 0x0110002e:
    case 0x0110002f:
    case 0x01100030:
    case 0x01100031:
    case 0x0210001f:
    case 0x02100032:
      bits = 16;
      return true;
    case 0x010a0046:
      bits = 10;
      return true;
    case 0x010c0047:
      bits = 12;
      return true;
    case 0x010a0052:
    case 0x010a0054:
    case 0x010a0056:
    case 0x010a0058:
    case 0x021400a8:
    case 0x021400aa:
    case 0x022000a7:
    case 0x022000a9:
      bits = 10;
      return true;
    case 0x010c0053:
    case 0x010c0055:
    case 0x010c0057:
    case 0x010c0059:
    case 0x021800ac:
    case 0x021800ae:
    case 0x022000ab:
    case 0x022000ad:
      bits = 12;
      return true;
    case 0x02180014:
    case 0x02180015:
      bits = 24;
      return true;
    case 0x02200016:
    case 0x02200017:
      bits = 32;
      return true;
    case 0x02300018:
    case 0x02300019:
      bits = 30;
      return true;
    case 0x0230001a:
    case 0x0230001b:
      bits = 36;
      return true;
    case 0x02300033:
      bits = 48;
      return true;
    default:
      return false;
  }
}

/** @brief Checks single-image metadata that must be valid before any consumer sees the slot. */
bool sharedMetadataValid(const FrameDescriptor& descriptor)
{
  // Shared descriptors cover uncompressed single images only. GenTL IMAGE is 1;
  // the fake peer uses 0 for its test image. Other payload types keep the owned path.
  if (descriptor.payload_type != 0 && descriptor.payload_type != 1) return false;
  if (!descriptor.width || !descriptor.height || !descriptor.stride) return false;
  if (descriptor.stride > descriptor.used_bytes) return false;
  if (descriptor.height > descriptor.used_bytes / descriptor.stride) return false;
  if (descriptor.stream_id.size() > 4096) return false;
  std::uint32_t bits = 0;
  bool ycbcr = false;
  if (!sharedImageBitsPerPixel(descriptor.pixel_format, bits, ycbcr)) return false;
  if (ycbcr && (descriptor.width % 2U)) return false;
  if (descriptor.width > (std::numeric_limits<std::uint64_t>::max)() / bits) return false;
  const auto row_bytes = (static_cast<std::uint64_t>(descriptor.width) * bits + 7U) / 8U;
  return row_bytes != 0 && descriptor.stride >= row_bytes;
}

/** @brief Reserves pool bytes against the per-session three-slot budget. */
std::shared_ptr<PoolBudgetReservation> reservePoolBytes(
    const std::shared_ptr<FramePoolBudget>& budget, const std::uint32_t session,
    const std::uint64_t bytes, const std::uint64_t limit, std::string& error)
{
  std::lock_guard lock(budget->mutex);
  const auto found = budget->used.find(session);
  const auto retained = found == budget->used.end() ? std::uint64_t{0} : found->second;
  if (bytes > limit || retained > limit - bytes) {
    error = "frame pool byte budget exceeded: session=" + std::to_string(session)
        + " requested=" + std::to_string(bytes) + " retained=" + std::to_string(retained)
        + " limit=" + std::to_string(limit);
    return {};
  }
  budget->used[session] = retained + bytes;
  auto reservation = std::make_shared<PoolBudgetReservation>();
  reservation->budget = budget;
  reservation->session = session;
  reservation->bytes = bytes;
  return reservation;
}

} // namespace

SharedFrameLease::SharedFrameLease(std::shared_ptr<const void> region,
    std::span<const std::uint8_t> bytes, FrameDescriptor descriptor,
    std::shared_ptr<ReleaseMailbox> mailbox, std::shared_ptr<PoolValidity> validity,
    std::shared_ptr<PoolBudgetReservation> reservation)
    : region_(std::move(region)), bytes_(bytes), descriptor_(std::move(descriptor)),
      mailbox_(std::move(mailbox)), validity_(std::move(validity)),
      reservation_(std::move(reservation))
{
}

bool SharedFrameLease::beginDelivery() const noexcept
{
  return !validity_ || validity_->beginDelivery();
}

void SharedFrameLease::endDelivery() const noexcept
{
  if (validity_) validity_->endDelivery();
}

SharedFrameLease::~SharedFrameLease()
{
  if (!mailbox_ || (validity_ && validity_->detached.load())) return;
  std::lock_guard lock(mailbox_->mutex);
  if (mailbox_->closed) return;
  try {
    Writer out;
    write(out, SlotRelease{descriptor_.epoch, descriptor_.token, descriptor_.slot,
                          descriptor_.generation});
    Message release{Operation::SlotRelease, 0, true, {}, out.take(), descriptor_.session};
    mailbox_->messages.push(std::move(release));
  } catch (...) {
  }
}

struct Connection::Impl {
  std::filesystem::path executable;
  std::filesystem::path cti;
  std::thread io;
  std::thread delivery;
  mutable std::mutex mutex;
  std::mutex close_mutex;
  std::condition_variable cv;
  std::queue<Message> outgoing;
  std::queue<PendingFrame> frames;
  std::queue<SharedFrame> shared_frames;
  std::map<std::uint32_t, Message> replies;
  std::map<std::uint32_t, std::shared_ptr<FramePoolState>> pools;
  std::shared_ptr<FramePoolBudget> frame_pool_budget = std::make_shared<FramePoolBudget>();
  std::map<std::uint32_t, std::shared_ptr<std::mutex>> pool_lifecycles;
  std::shared_ptr<ReleaseMailbox> release_mailbox = std::make_shared<ReleaseMailbox>();
  bool frame_trace = onegen::frame_trace_enabled();
  std::uint32_t next_id = 1;
  bool running = true;
  bool started = false;
  std::string start_error;
  std::string close_error;
  bool inactive = false;
  std::mutex lifecycle_mutex;
  std::condition_variable lifecycle_cv;
  int active_requests = 0;
  bool accepting = true;
  std::atomic<bool> discovery_log{true};

  bool beginRequest(const bool allow_retiring = false)
  {
    std::lock_guard lock(lifecycle_mutex);
    if (!accepting && !allow_retiring) return false;
    ++active_requests;
    return true;
  }
  void endRequest()
  {
    std::lock_guard lock(lifecycle_mutex);
    if (active_requests > 0) --active_requests;
    lifecycle_cv.notify_all();
  }
  /** @brief Serializes attach/detach transitions for one session without blocking others. */
  std::shared_ptr<std::mutex> poolLifecycle(const std::uint32_t session)
  {
    std::lock_guard lock(mutex);
    auto& lifecycle = pool_lifecycles[session];
    if (!lifecycle) lifecycle = std::make_shared<std::mutex>();
    return lifecycle;
  }
  bool beginRetirement(const std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(lifecycle_mutex);
    if (!accepting) return false;
    accepting = false;
    return lifecycle_cv.wait_for(lock, timeout, [&] { return active_requests == 0; });
  }
  struct Callbacks { FrameFn frame; SharedFrameFn shared_frame; ErrorFn error; InvalidateFn invalidate; };
  std::map<std::uint32_t, Callbacks> callbacks;
  ClaimFn claim;
  std::map<std::string, std::shared_ptr<void>> interface_claims;
  /** @brief Fails the connection and wakes startup/request waiters with contextual diagnostics. */
  void fail(const char* phase, const std::string& detail) {
    const auto message = QStringLiteral("Worker %1 failed: %2 worker=%3 cti=%4")
        .arg(QString::fromLatin1(phase), QString::fromStdString(detail),
             QString::fromStdString(executable.string()), QString::fromStdString(cti.string()));
    {
      std::lock_guard lock(mutex);
      if (close_error.empty()) close_error = message.toStdString();
      if (!started) { start_error = close_error; started = true; }
      running = false;
      cv.notify_all();
    }
    auto log = discovery_log.load() ? qDebug() : qWarning();
    log.noquote() << "[GenICam]" << message;
  }
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
    try {
    while (true) {
      PendingFrame pending;
      SharedFrame shared;
      FrameFn callback;
      SharedFrameFn shared_callback;
      bool shared_delivery = false;
      std::chrono::steady_clock::time_point delivery_started;
      {
        std::unique_lock lock(impl_->mutex);
        impl_->cv.wait(lock, [&] {
          return !impl_->running || !impl_->frames.empty() || !impl_->shared_frames.empty();
        });
        if (!impl_->running && impl_->frames.empty() && impl_->shared_frames.empty()) return;
        if (!impl_->frames.empty()) {
          pending = std::move(impl_->frames.front());
          impl_->frames.pop();
          callback = impl_->callbacks[pending.session].frame;
          delivery_started = impl_->frame_trace ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
          if (!impl_->running) callback = {};
        } else {
          shared_delivery = true;
          shared = std::move(impl_->shared_frames.front());
          impl_->shared_frames.pop();
          shared_callback = impl_->callbacks[shared.descriptor.session].shared_frame;
          delivery_started = impl_->frame_trace ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
          if (!impl_->running) shared_callback = {};
        }
      }
      if (shared_delivery) {
        if (!shared.lease || !shared.lease->beginDelivery()) continue;
        struct DeliveryCompletion {
          const SharedFrameLease* lease;
          ~DeliveryCompletion() { lease->endDelivery(); }
        } delivery_completion{shared.lease.get()};
        try { if (shared_callback) shared_callback(shared); }
        catch (...) {
          qWarning().noquote() << QStringLiteral(
              "[GenICam] shared frame consumer failed: session=%1 cti=%2 detail=%3")
              .arg(shared.descriptor.session).arg(QString::fromStdString(impl_->cti.string()),
                   QString::fromStdString(currentExceptionMessage()));
        }
        if (impl_->frame_trace) {
          qDebug().noquote() << QStringLiteral(
              "[GenICam] frame transport: side=host-shm session=%1 frame=%2 bytes=%3 "
              "slot=%4 generation=%5 callback_ms=%6")
              .arg(shared.descriptor.session)
              .arg(static_cast<qulonglong>(shared.descriptor.frame_id))
              .arg(static_cast<qulonglong>(shared.descriptor.used_bytes))
              .arg(shared.descriptor.slot)
              .arg(static_cast<qulonglong>(shared.descriptor.generation))
              .arg(onegen::frame_trace_ms(delivery_started, std::chrono::steady_clock::now()),
                   0, 'f', 3);
        }
        continue;
      }
      try { if (callback) callback(pending.frame); }
      catch (...) {
        qWarning().noquote() << QStringLiteral("[GenICam] frame consumer failed: session=%1 cti=%2 detail=%3")
            .arg(pending.session).arg(QString::fromStdString(impl_->cti.string()),
                             QString::fromStdString(currentExceptionMessage()));
      }
      const auto delivered = impl_->frame_trace ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
      {
        std::lock_guard lock(impl_->mutex);
        if (impl_->running) { Message ack{Operation::FrameAck}; ack.session = pending.session; impl_->outgoing.push(std::move(ack)); }
      }
      if (impl_->frame_trace) {
        qDebug().noquote() << QStringLiteral(
            "[GenICam] frame transport: side=host session=%1 frame=%2 bytes=%3 read_ms=%4 "
            "extract_ms=%5 decode_ms=%6 payload_ms=%7 queue_ms=%8 callback_ms=%9")
            .arg(pending.session)
            .arg(static_cast<qulonglong>(pending.frame.frame_id))
            .arg(static_cast<qulonglong>(pending.frame.payload.size()))
            .arg(pending.read_ms, 0, 'f', 3)
            .arg(pending.extract_ms, 0, 'f', 3)
            .arg(pending.decode_ms, 0, 'f', 3)
            .arg(pending.payload_ms, 0, 'f', 3)
            .arg(onegen::frame_trace_ms(pending.received, delivery_started), 0, 'f', 3)
            .arg(onegen::frame_trace_ms(delivery_started, delivered), 0, 'f', 3);
      }
    }
    } catch (...) { impl_->fail("frame delivery", currentExceptionMessage()); }
  });
  try {
  impl_->io = std::thread([this] {
    QProcess process;
    qint64 process_id = 0;
    QByteArray buffer;
    QByteArray diagnostic;
    WorkerLogForwarder log_forwarder(QString::fromStdString(impl_->cti.string()));
    try {
    { std::lock_guard lock(impl_->mutex); if (!impl_->running) return; }
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
        impl_->start_error = QStringLiteral("Worker start failed: %1 worker=%2 cti=%3")
            .arg(process.errorString(), QString::fromStdString(impl_->executable.string()),
                 QString::fromStdString(impl_->cti.string())).toStdString();
        qDebug().noquote() << "[GenICam]" << QString::fromStdString(impl_->start_error);
        impl_->running = false;
      } else {
        process_id = process.processId();
        qDebug().noquote() << QStringLiteral("[GenICam] worker process started: pid=%1 cti=%2")
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

    while (process.state() != QProcess::NotRunning) {
      std::queue<Message> pending;
      {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->running) break;
        pending.swap(impl_->outgoing);
      }
      {
        std::lock_guard lock(impl_->release_mailbox->mutex);
        while (!impl_->release_mailbox->messages.empty()) {
          pending.push(std::move(impl_->release_mailbox->messages.front()));
          impl_->release_mailbox->messages.pop();
        }
      }
      while (!pending.empty()) {
        process.write(framed(std::move(pending.front())));
        pending.pop();
      }
      process.waitForBytesWritten(10);
      process.waitForReadyRead(10);
      try {
        const auto read_started = impl_->frame_trace ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
        buffer.append(process.readAllStandardOutput());
        double pending_read_ms = impl_->frame_trace
            ? onegen::frame_trace_ms(read_started, std::chrono::steady_clock::now()) : 0;
        const auto stderr_bytes = process.readAllStandardError();
        diagnostic.append(stderr_bytes);
        log_forwarder.setDiscoveryMode(impl_->discovery_log.load());
        log_forwarder.append(stderr_bytes);
        if (diagnostic.size() > 65536) diagnostic.remove(0, diagnostic.size() - 65536);
        Message message;
        MessageTiming timing;
        while (takeMessage(buffer, message, impl_->frame_trace ? &timing : nullptr)) {
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
            const auto payload_started = impl_->frame_trace ? std::chrono::steady_clock::now()
                                                            : std::chrono::steady_clock::time_point{};
            Reader reader(message.payload);
            PendingFrame frame;
            frame.session = message.session;
            frame.frame = readFrame(reader);
            if (impl_->frame_trace) {
              frame.received = std::chrono::steady_clock::now();
              frame.read_ms = pending_read_ms;
              frame.extract_ms = timing.extract_ms;
              frame.decode_ms = timing.decode_ms;
              frame.payload_ms = onegen::frame_trace_ms(payload_started, frame.received);
            }
            pending_read_ms = 0;
            std::lock_guard lock(impl_->mutex);
            impl_->frames.push(std::move(frame));
            impl_->cv.notify_all();
          } else if (message.operation == Operation::FrameReady) {
            Reader reader(message.payload);
            auto descriptor = readFrameDescriptor(reader);
            std::shared_ptr<FramePoolState> pool;
            {
              std::lock_guard lock(impl_->mutex);
              if (const auto found = impl_->pools.find(message.session); found != impl_->pools.end())
                pool = found->second;
            }
            pending_read_ms = 0;
            if (!pool) {
              qDebug().noquote() << QStringLiteral(
                  "[GenICam] shared frame ignored: session=%1 has no attached pool")
                  .arg(message.session);
              continue;
            }
            if (pool->validity->detached.load()) continue;
            if (descriptor.session != message.session) {
              qWarning().noquote() << QStringLiteral(
                  "[GenICam] shared frame rejected: session=%1 descriptor-session=%2")
                  .arg(message.session).arg(descriptor.session);
              continue;
            }
            if (descriptor.epoch != pool->epoch || descriptor.token != pool->token
                || descriptor.slot >= pool->slot_count) {
              qWarning().noquote() << QStringLiteral(
                  "[GenICam] shared frame rejected: session=%1 frame=%2 slot=%3 reason=stale pool identity")
                  .arg(message.session).arg(static_cast<qulonglong>(descriptor.frame_id))
                  .arg(descriptor.slot);
              continue;
            }
            bool stale = false;
            bool invalid = false;
            {
              std::lock_guard pool_lock(pool->mutex);
              if (descriptor.generation <= pool->last_generation[descriptor.slot]) {
                stale = true;
              } else {
                pool->last_generation[descriptor.slot] = descriptor.generation;
                if (descriptor.sequence <= pool->last_sequence) invalid = true;
                else pool->last_sequence = descriptor.sequence;
              }
            }
            if (stale) {
              qDebug().noquote() << QStringLiteral(
                  "[GenICam] shared frame ignored: session=%1 frame=%2 slot=%3 stale generation=%4")
                  .arg(message.session).arg(static_cast<qulonglong>(descriptor.frame_id))
                  .arg(descriptor.slot).arg(static_cast<qulonglong>(descriptor.generation));
              continue;
            }
            if (!descriptor.used_bytes || descriptor.used_bytes > pool->slot_bytes
                || !descriptor.generation || !sharedMetadataValid(descriptor)) {
              qWarning().noquote() << QStringLiteral(
                  "[GenICam] shared frame rejected: session=%1 frame=%2 slot=%3 bytes=%4 reason=invalid descriptor")
                  .arg(message.session).arg(static_cast<qulonglong>(descriptor.frame_id))
                  .arg(descriptor.slot).arg(static_cast<qulonglong>(descriptor.used_bytes));
              releaseSharedSlot(pool, descriptor);
              continue;
            }
            if (invalid) {
              qWarning().noquote() << QStringLiteral(
                  "[GenICam] shared frame rejected: session=%1 frame=%2 sequence=%3 reason=stale sequence")
                  .arg(message.session).arg(static_cast<qulonglong>(descriptor.frame_id))
                  .arg(static_cast<qulonglong>(descriptor.sequence));
              releaseSharedSlot(pool, descriptor);
              continue;
            }
            const auto offset = static_cast<std::size_t>(descriptor.slot)
                * static_cast<std::size_t>(pool->slot_bytes);
            const auto bytes = std::span<const std::uint8_t>(
                pool->region->data() + offset, static_cast<std::size_t>(descriptor.used_bytes));
            auto lease = std::shared_ptr<SharedFrameLease>(
                new SharedFrameLease(pool->region, bytes, descriptor, pool->mailbox,
                                     pool->validity, pool->reservation));
            SharedFrame shared;
            shared.descriptor = std::move(descriptor);
            shared.lease = std::move(lease);
            {
              std::lock_guard lock(impl_->mutex);
              impl_->shared_frames.push(std::move(shared));
              impl_->cv.notify_all();
            }
          } else if (message.operation == Operation::Error) {
            ErrorFn callback;
            { std::lock_guard lock(impl_->mutex); callback = impl_->callbacks[message.session].error; }
            if (callback) callback(message.error);
            else if (impl_->discovery_log.load())
              qDebug().noquote() << "[GenICam] discovery diagnostic:" << QString::fromStdString(message.error);
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
            if (message.operation == Operation::ProducerInactive) impl_->inactive = true;
            impl_->replies[message.id] = std::move(message);
            impl_->cv.notify_all();
          }
        }
      } catch (...) {
        impl_->fail("message processing", currentExceptionMessage());
        process.kill();
        break;
      }
    }
    } catch (...) { impl_->fail("process IO", currentExceptionMessage()); }
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(5000);
    }
    impl_->interface_claims.clear();
    {
      const auto final_diagnostics = process.readAllStandardError();
      diagnostic.append(final_diagnostics);
      log_forwarder.setDiscoveryMode(impl_->discovery_log.load());
      log_forwarder.append(final_diagnostics);
      log_forwarder.finish();
      QString last_error;
      auto remainder = QString::fromUtf8(diagnostic);
      for (auto line : remainder.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1String("[GenICam]"))
            && !line.startsWith(QLatin1String("[GenICam] [WARN]"))
            && !line.startsWith(QLatin1String("[GenICam] [WARNING]"))
            && !line.startsWith(QLatin1String("[GenICam] [ERROR]"))
            && !line.startsWith(QLatin1String("[GenICam] [CRIT]"))
            && !line.startsWith(QLatin1String("[GenICam] [CRITICAL]"))
            && !line.startsWith(QLatin1String("[GenICam] [FATAL]"))) continue;
        last_error = line;
      }
      const auto crashed = process.exitStatus() == QProcess::CrashExit;
      const auto exit_hex = QString::number(static_cast<quint32>(process.exitCode()), 16).rightJustified(8, '0');
      auto exit_log = impl_->discovery_log.load() ? qDebug() : qWarning();
      exit_log.noquote()
          << QStringLiteral("[GenICam] worker process exited: pid=%1 code=%2 hex=0x%3 crashed=%4 cti=%5")
                 .arg(process_id)
                 .arg(process.exitCode())
                 .arg(exit_hex)
                 .arg(crashed ? QStringLiteral("yes") : QStringLiteral("no"))
                 .arg(QString::fromStdString(impl_->cti.string()));
      std::lock_guard lock(impl_->mutex);
      if (impl_->close_error.empty()) {
        impl_->close_error = crashed ? "Worker process crashed" : "Worker process exited";
        impl_->close_error += " (process exit code " + std::to_string(process.exitCode())
            + ", 0x" + exit_hex.toStdString() + ").";
        if (!last_error.isEmpty()) {
          impl_->close_error += ' ';
          impl_->close_error += last_error.toStdString();
        }
      }
      impl_->running = false;
      impl_->cv.notify_all();
    }
  });
  } catch (...) {
    { std::lock_guard lock(impl_->mutex); impl_->running = false; impl_->cv.notify_all(); }
    if (impl_->delivery.joinable()) impl_->delivery.join();
    throw;
  }
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
  {
    std::lock_guard lock(impl_->mutex);
    for (auto& [session, pool] : impl_->pools) {
      (void)session;
      if (pool->validity) pool->validity->invalidate();
    }
    impl_->pools.clear();
  }
  {
    std::lock_guard lock(impl_->release_mailbox->mutex);
    impl_->release_mailbox->closed = true;
  }
  try { request(Operation::Shutdown, {}, std::chrono::milliseconds{500}); } catch (...) {}
#ifdef _WIN32
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
  if (!impl_->beginRequest(operation == Operation::Shutdown)) throw std::runtime_error("Worker connection is retiring.");
  struct ActiveRequest {
    explicit ActiveRequest(Impl* value) : value(value) {}
    ~ActiveRequest() { value->endRequest(); }
    Impl* value;
  } active_request(impl_.get());
  std::uint32_t id;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->running) {
      throw std::runtime_error(impl_->close_error.empty()
                                   ? "Worker connection is closed."
                                   : impl_->close_error);
    }
    id = impl_->next_id++;
    // Once a device/interface is opened, retain operational warning/error levels.
    // Probe-only workers report search failures through the terminal scan log.
    if (operation == Operation::Open || operation == Operation::OpenInterface)
      impl_->discovery_log = false;
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

bool Connection::beginRetirement(const std::chrono::milliseconds timeout)
{
  return impl_->beginRetirement(timeout);
}

bool Connection::inactive() const
{
  std::lock_guard lock(impl_->mutex);
  return impl_->inactive;
}

void Connection::set_frame_callback(FrameFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].frame = std::move(callback); }
void Connection::setSharedFrameCallback(SharedFrameFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].shared_frame = std::move(callback); }
void Connection::set_error_callback(ErrorFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].error = std::move(callback); }
void Connection::set_invalidate_callback(InvalidateFn callback, std::uint32_t session)
{ std::lock_guard lock(impl_->mutex); impl_->callbacks[session].invalidate = std::move(callback); }

bool Connection::createFramePool(std::uint32_t session, std::uint32_t slotCount,
                                 std::uint64_t slotBytes, std::string& error)
{
  const auto lifecycle = impl_->poolLifecycle(session);
  std::lock_guard lifecycle_lock(*lifecycle);
  if (!slotCount || slotCount > 16 || !slotBytes) {
    error = "invalid frame pool geometry";
    return false;
  }
  if (slotBytes > (std::numeric_limits<std::uint64_t>::max)() / slotCount) {
    error = "frame pool size overflows";
    return false;
  }
  const auto bytes = slotBytes * slotCount;
  if (bytes > (std::numeric_limits<std::size_t>::max)()) {
    error = "frame pool exceeds the address space";
    return false;
  }
  if (slotBytes > (std::numeric_limits<std::uint64_t>::max)() / 3) {
    error = "frame pool byte budget overflows";
    return false;
  }
  const auto budget_bytes = slotBytes * 3;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->pools.count(session)) {
      error = "frame pool is already attached";
      return false;
    }
  }
  auto reservation = reservePoolBytes(impl_->frame_pool_budget, session, bytes,
                                      budget_bytes, error);
  if (!reservation) return false;
  auto region = onegen::SharedMemoryRegion::create(static_cast<std::size_t>(bytes), error);
  if (!region) return false;
  std::random_device random;
  const auto next = [&random] {
    return (static_cast<std::uint64_t>(random()) << 32) ^ random();
  };
  auto epoch = next();
  auto token = next();
  if (!epoch) epoch = 1;
  if (!token) token = 1;
  auto pool = std::make_shared<FramePoolState>();
  pool->name = region->name();
  pool->region = region;
  pool->epoch = epoch;
  pool->token = token;
  pool->slot_count = slotCount;
  pool->slot_bytes = slotBytes;
  pool->mailbox = impl_->release_mailbox;
  pool->validity = std::make_shared<PoolValidity>();
  pool->reservation = reservation;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->pools[session] = pool;
  }
  try {
    Writer out;
    write(out, FramePoolAttach{pool->name, bytes, slotCount, slotBytes, epoch, token});
    request(Operation::FramePoolAttach, out.take(), std::chrono::seconds{10}, session);
  } catch (const std::exception& exception) {
    std::lock_guard lock(impl_->mutex);
    impl_->pools.erase(session);
    error = exception.what();
    return false;
  }
  return true;
}

void Connection::destroyFramePool(std::uint32_t session)
{
  const auto lifecycle = impl_->poolLifecycle(session);
  std::shared_ptr<FramePoolState> pool;
  {
    std::lock_guard lifecycle_lock(*lifecycle);
    {
      std::lock_guard lock(impl_->mutex);
      const auto found = impl_->pools.find(session);
      if (found == impl_->pools.end()) return;
      pool = found->second;
      impl_->pools.erase(found);
    }
    if (pool->validity) pool->validity->invalidate();
    try { request(Operation::FramePoolDetach, {}, std::chrono::seconds{2}, session); } catch (...) {}
  }
  if (pool->validity) pool->validity->waitForCallbacks();
}

} // namespace onegen::worker
