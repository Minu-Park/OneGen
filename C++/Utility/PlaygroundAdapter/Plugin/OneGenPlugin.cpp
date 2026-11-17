#include "DeviceRuntime/Contract/DevicePluginSession.h"

#include "ProducerCoordinator.h"
#include "Utility/PlaygroundAdapter/OneGenLogSink.h"
#include "OneGenSourceController.h"
#include "OneGen.h"
#include "StartupDiscovery.h"
#include <QThread>

#include <QDebug>
#include <QPointer>
#include <QStringList>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {

/** @brief Translates module-owned scan outcomes into copied consumer diagnostics. */
GenTLDiscoverySnapshot consumerStatus(const onegen::producer::StartupDiscovery::Status& status) {
  using Phase = onegen::producer::StartupDiscovery::Phase;
  GenTLDiscoveryPhase phase = GenTLDiscoveryPhase::Waiting;
  switch (status.phase) {
  case Phase::Waiting: break;
  case Phase::Running: phase = GenTLDiscoveryPhase::Running; break;
  case Phase::Succeeded: phase = GenTLDiscoveryPhase::Succeeded; break;
  case Phase::Partial: phase = GenTLDiscoveryPhase::Partial; break;
  case Phase::Failed: phase = GenTLDiscoveryPhase::Failed; break;
  case Phase::Stopping: phase = GenTLDiscoveryPhase::Stopping; break;
  case Phase::Stopped: phase = GenTLDiscoveryPhase::Stopped; break;
  }
  return {phase, status.scan, QString::fromStdString(status.detail)};
}

/** @brief Logs lifecycle transitions without exposing scan ownership or updating UI. */
void logDiscoveryState(const onegen::producer::StartupDiscovery::Status& before,
                       const onegen::producer::StartupDiscovery::Status& after) {
  const auto old_state = consumerStatus(before);
  const auto state = consumerStatus(after);
  qDebug().noquote() << QStringLiteral("[GenTL] consumer=onegen scan=%1 %2 -> %3 reason=%4")
      .arg(state.generation).arg(QLatin1String(GenTLDiscoveryState::name(old_state.phase)))
      .arg(QLatin1String(GenTLDiscoveryState::name(state.phase)), state.detail);
}

std::vector<onegen::DeviceInfo> discover(
    const std::shared_ptr<onegen::producer::Coordinator>& coordinator,
    onegen::producer::Coordinator::ProgressFn progress)
{
  if (!coordinator) throw std::runtime_error("GenICam discovery is shutting down.");
  const auto started = std::chrono::steady_clock::now();
  qInfo().noquote() << QStringLiteral("[GenICam] camera search started with consumer-provided producers");
  onegen::producer::DiscoveryResult result;
  try {
    result = coordinator->discover(std::move(progress));
  } catch (const std::exception& error) {
    qInfo().noquote() << QStringLiteral("[GenICam] camera search completed: status=failed reason=%1")
        .arg(QString::fromLocal8Bit(error.what()));
    throw;
  } catch (...) {
    qInfo().noquote() << "[GenICam] camera search completed: status=failed reason=unknown discovery exception";
    throw;
  }
  for (const auto& producer : result.producer_results)
    qDebug().noquote() << "[GenICam] producer search result:" << QString::fromStdString(producer);
  const auto outcome = result.failures.empty() ? QStringLiteral("succeeded")
      : result.devices.empty() && result.failures.size() == result.total_producers
          ? QStringLiteral("failed") : QStringLiteral("partial");
  qInfo().noquote() << QStringLiteral("[GenICam] camera search completed: status=%7, %1 camera(s), %2/%3 producers checked, %4 producer(s) with search failures, elapsed=%5 ms%6")
      .arg(result.devices.size()).arg(result.finished_producers).arg(result.total_producers)
      .arg(result.failures.size())
      .arg(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count())
      .arg(result.total_producers == 0 ? QStringLiteral("; no configured CTI producers")
          : result.devices.empty() ? QStringLiteral("; no connected camera found") : QString{})
      .arg(outcome);
  for (const auto& failure : result.failures)
    qInfo().noquote() << "[GenICam] producer search failed:"
        << QString::fromStdString(failure).simplified();
  for (const auto& device : result.devices)
    qDebug().noquote() << QStringLiteral("[GenICam] camera found: model=%1 serial=%2 id=%3 transport=%4 ip=%5 cti=%6")
        .arg(QString::fromStdString(device.model), QString::fromStdString(device.serial),
             QString::fromStdString(device.id), QString::fromStdString(device.tl_type),
             QString::fromStdString(device.ip), QString::fromStdString(device.cti_path));
  if (!result.completed && !result.failures.empty()) {
    QStringList failures;
    for (const auto& failure : result.failures)
      failures.append(QString::fromStdString(failure));
    throw std::runtime_error(failures.join(QLatin1Char('\n')).toStdString());
  }
  return result.devices;
}

class OneGenPluginSession final : public IDevicePluginSession {
public:
  OneGenPluginSession(std::shared_ptr<onegen::producer::Coordinator> coordinator,
                      std::vector<onegen::DeviceInfo> devices,
                      std::function<void(OneGenSourceController*)> registerController)
      : coordinator_(std::move(coordinator)), devices_(std::move(devices)),
        controller_(std::make_unique<OneGenSourceController>()) {
    registerController(controller_.get());
    controller_->configureControl(coordinator_, devices_);
  }

  QString title() const override { return title_; }
  AbstractSourceController* sourceController() const override { return controller_.get(); }
  unsigned int capabilities() const noexcept override
  {
    return DevicePluginSessionCapability::GraphicsEngine
        | DevicePluginSessionCapability::ScriptEditor;
  }
  void setTitleChangedCallback(std::function<void(const QString&)> callback) override
  { title_changed_ = std::move(callback); }

private:
  std::shared_ptr<onegen::producer::Coordinator> coordinator_;
  std::vector<onegen::DeviceInfo> devices_;
  std::unique_ptr<OneGenSourceController> controller_;
  QString title_ = QStringLiteral("GenICam Session");
  std::function<void(const QString&)> title_changed_;
};

} // namespace

class OneGenPlugin final : public QObject, public IDeviceRuntime, public IDevicePluginSettings,
    public IDevicePluginResourceClaimsReceiver, public IDevicePluginGenTLPaths,
    public IDevicePluginGenTLProducers, public IDevicePluginGenTLDiscoveryStatus, public IDevicePluginGenTLRetry {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID PlaygroundDeviceRuntime_iid)
  Q_INTERFACES(IDeviceRuntime IDevicePluginSettings IDevicePluginResourceClaimsReceiver IDevicePluginGenTLPaths IDevicePluginGenTLProducers IDevicePluginGenTLDiscoveryStatus IDevicePluginGenTLRetry)
public:
  /** Producers run in the hosting process; their diagnostics go to its Qt log. */
  OneGenPlugin() { onegen::qt::installLogSink(); }

  /** Ignores the host settings path; the Host panel owns the favourites store. */
  void setSettingsFile(const QString&) override {}
  /** Retains the host service used to claim a Siso board before its Interface opens. */
  void setResourceClaims(IDevicePluginResourceClaims* claims) override {
    claims_.store(claims, std::memory_order_release);
  }

  /** @brief Accepts exact CTI files without consulting process search variables. */
  bool setGenTLProducerFiles(const QStringList& files, QString* error) override {
    try {
      const auto reason = genTLPathChangeBlockReason();
      if (!reason.isEmpty()) throw std::runtime_error(reason.toStdString());
      const auto producer_coordinator = coordinator();
      std::vector<std::filesystem::path> producers;
      for (const auto& file : files) {
#ifdef _WIN32
        producers.emplace_back(file.toStdWString());
#else
        producers.emplace_back(file.toStdString());
#endif
      }
      if (!producer_coordinator || !producer_coordinator->set_producer_files(std::move(producers)))
        throw std::runtime_error("Disconnect GenICam devices and interfaces before changing producer files.");
      { std::lock_guard lock(mutex_); producers_configured_ = true; discovery_state_.transition(GenTLDiscoveryEvent::Reset); }
      qDebug().noquote() << QStringLiteral("[GenICam] consumer producer inventory: %1")
          .arg(files.isEmpty() ? QStringLiteral("(none)") : files.join(QLatin1Char('\n')));
      return true;
    } catch (const std::exception& exception) {
      const auto detail = QString::fromLocal8Bit(exception.what());
      { std::lock_guard lock(mutex_); discovery_state_.transition(GenTLDiscoveryEvent::Reject, detail); }
      if (error) *error = detail;
      return false;
    }
  }

  /** @brief Exposes actual asynchronous completion independently of start admission. */
  GenTLDiscoverySnapshot genTLDiscoveryStatus() const override {
    std::lock_guard lock(mutex_);
    if (shutting_down_ || !startup_) return discovery_state_.snapshot();
    return consumerStatus(startup_->status());
  }

  /** @brief Starts one retained scan shared by startup and later control panels. */
  bool startGenTLDiscovery(QString* error) override {
    try {
      const auto producer_coordinator = coordinator();
      std::lock_guard lock(mutex_);
      if (!producer_coordinator || !producers_configured_)
        throw std::runtime_error("The consumer must provide GenICam producer files before discovery.");
      if (!startup_) startup_ = std::make_shared<onegen::producer::StartupDiscovery>(
          [producer_coordinator](auto progress) { return discover(producer_coordinator, std::move(progress)); }, logDiscoveryState);
      return true;
    } catch (const std::exception& exception) {
      const auto detail = QString::fromLocal8Bit(exception.what());
      { std::lock_guard lock(mutex_); discovery_state_.transition(GenTLDiscoveryEvent::Reject, detail); }
      if (error) *error = detail;
      return false;
    }
  }

  DevicePluginDescriptor descriptor() const override
  { return {QStringLiteral("onegen"), playgroundDevicePluginDisplayName(), playgroundDevicePluginVersion(), false}; }

  bool discoverDevices(QString* error) override
  {
    try {
      QString startup_error;
      if (!startGenTLDiscovery(&startup_error)) throw std::runtime_error(startup_error.toStdString());
      std::shared_ptr<onegen::producer::StartupDiscovery> startup;
      { std::lock_guard lock(mutex_); startup = startup_; }
      const auto devices = startup->waitForCamera(std::chrono::milliseconds{0}, [] {
        return QThread::currentThread()->isInterruptionRequested();
      });
      qDebug().noquote() << (devices.empty()
          ? QStringLiteral("[GenICam] opening session without a camera; available startup results are reused")
          : QStringLiteral("[GenICam] camera available: opening session with %1 camera(s); remaining producer search continues").arg(devices.size()));
      { std::lock_guard lock(mutex_); discovered_devices_ = devices; }
      return true;
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return false;
    }
  }

  std::unique_ptr<IDevicePluginSession> createSession(
      const QVariantMap& sessionOptions, QString* error) override
  {
    Q_UNUSED(sessionOptions)
    try {
      const auto producer_coordinator = coordinator();
      if (!producer_coordinator) return {};
      std::vector<onegen::DeviceInfo> devices;
      { std::lock_guard lock(mutex_); devices = discovered_devices_; }
      return std::make_unique<OneGenPluginSession>(
          producer_coordinator, std::move(devices),
          [this](OneGenSourceController* controller) {
            std::lock_guard lock(mutex_);
            controllers_.push_back(controller);
          });
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return {};
    }
  }

  /** @brief Keeps connected cameras, Interfaces and ongoing searches intact. */
  QString genTLPathChangeBlockReason() const override {
    { std::lock_guard lock(mutex_);
      if (shutting_down_) return QStringLiteral("GenICam is shutting down.");
      if (startup_ && startup_->running()) return QStringLiteral("Wait for the GenICam startup search to finish before applying paths.");
    }
    return sessionBlockReason();
  }

  /** @brief Retries the retained inventory without closing successful producers. */
  bool retryGenTLDiscovery(QString* error) override {
    const auto reason = genTLPathChangeBlockReason();
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    std::shared_ptr<onegen::producer::StartupDiscovery> scan;
    std::shared_ptr<onegen::producer::Coordinator> producer_coordinator;
    try {
      producer_coordinator = coordinator();
      std::lock_guard lock(mutex_);
      if (!producer_coordinator || !producers_configured_)
        throw std::runtime_error("The consumer must provide GenICam producer files before discovery.");
      scan = std::make_shared<onegen::producer::StartupDiscovery>(
          [producer_coordinator](auto progress) { return discover(producer_coordinator, std::move(progress)); }, logDiscoveryState);
      startup_ = scan;
    } catch (const std::exception& exception) {
      if (error) *error = QString::fromLocal8Bit(exception.what());
      return false;
    }
    // Every live session consumes the same retained scan, so one retry produces one producer search.
    const auto discovery = sharedDiscovery(scan);
    for (const auto& controller : liveControllers()) {
      if (!controller) continue;
      QString refusal;
      if (!controller->retryDiscovery(&refusal, discovery)) {
        if (error) *error = refusal;
        return false;
      }
    }
    return true;
  }

  /** @brief Closes idle producers without unloading the plugin. */
  bool resetGenTLSearch(QString* error) override {
    const auto reason = genTLPathChangeBlockReason();
    if (!reason.isEmpty()) { if (error) *error = reason; return false; }
    std::lock_guard lock(mutex_);
    if (coordinator_ && !coordinator_->reset_idle_search()) {
      if (error) *error = QStringLiteral("GenICam is still searching or connected. Wait for it to become idle.");
      return false;
    }
    startup_.reset();
    discovery_state_.transition(GenTLDiscoveryEvent::Reset, QStringLiteral("producer inventory changing"));
    return true;
  }

  /** @brief Refreshes the retained inventory and notifies live sessions after the host applies paths. */
  void rescanGenTLPaths() override {
    // The host starts the retained scan immediately before this call, so the scan-running
    // preflight would reject the very rescan it requested.
    const auto reason = sessionBlockReason();
    if (!reason.isEmpty()) { qInfo().noquote() << "[GenICam]" << reason; return; }
    {
      std::lock_guard lock(mutex_);
      // A completed startup scan is stale after a path change; a running one was just started for this rescan.
      if (startup_ && !startup_->running()) startup_.reset();
    }
    QString error;
    if (!startGenTLDiscovery(&error)) { qInfo().noquote() << "[GenICam]" << error; return; }
    std::shared_ptr<onegen::producer::StartupDiscovery> scan;
    { std::lock_guard lock(mutex_); scan = startup_; }
    if (!scan) { qInfo().noquote() << "[GenICam] Discovery could not start."; return; }
    const auto discovery = sharedDiscovery(scan);
    for (const auto& controller : liveControllers()) {
      if (!controller) continue;
      QString refusal;
      if (!controller->retryDiscovery(&refusal, discovery)) qInfo().noquote() << "[GenICam]" << refusal;
    }
  }

  void shutdown() override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    discovery_state_.transition(GenTLDiscoveryEvent::Shutdown, QStringLiteral("consumer shutdown"));
    startup_.reset();
    if (coordinator_) coordinator_->shutdown();
    coordinator_.reset();
    discovery_state_.transition(GenTLDiscoveryEvent::Joined);
  }

private:
  /** @brief Reports shutdown or a live session that still owns a connection or control work. */
  QString sessionBlockReason() const {
    { std::lock_guard lock(mutex_); if (shutting_down_) return QStringLiteral("GenICam is shutting down."); }
    for (const auto& controller : liveControllers()) {
      if (!controller) continue;
      const auto reason = controller->genTLPathChangeBlockReason();
      if (!reason.isEmpty()) return reason;
    }
    return {};
  }

  /** @brief Supplies one retained scan to every live session without starting another producer search. */
  static OneGenSourceController::DiscoveryFn sharedDiscovery(
      const std::shared_ptr<onegen::producer::StartupDiscovery>& scan) {
    return [scan] { return scan->finish({}); };
  }

  /** @brief Copies live session controllers so discovery notifications never hold the plugin lock. */
  std::vector<QPointer<OneGenSourceController>> liveControllers() const {
    std::lock_guard lock(mutex_);
    std::erase_if(controllers_, [](const auto& controller) { return controller.isNull(); });
    return std::vector<QPointer<OneGenSourceController>>(controllers_.begin(), controllers_.end());
  }

  mutable std::vector<QPointer<OneGenSourceController>> controllers_;
  std::shared_ptr<onegen::producer::Coordinator> coordinator()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) return {};
    if (!coordinator_) {
      coordinator_ = std::make_shared<onegen::producer::Coordinator>(
          std::filesystem::path{}, std::chrono::milliseconds{10000},
          [this](const onegen::InterfaceInfo& info) { return claimInterface(info); },
          [this](const onegen::InterfaceInfo& info, const onegen::DeviceInfo& device) {
            return claimCamera(info, device);
          });
    }
    return coordinator_;
  }

  /** @brief Claims one exclusive Interface; network and USB Interfaces are claimed per camera. */
  std::shared_ptr<void> claimInterface(const onegen::InterfaceInfo& info)
  {
    const auto scope = onegen::interfaceClaimScope(info.tl_type);
    if (scope == onegen::InterfaceClaimScope::Camera) return {};
    auto* claims = claims_.load(std::memory_order_acquire);
    QString key = playgroundSisoBoardResourceKeyFromInterfaceId(QString::fromStdString(info.id));
    // A board without a Siso serial still needs one claim per Interface, including an unknown type.
    if (key.isEmpty() && claims)
      key = QStringLiteral("onegen-interface:") + QString::fromStdString(info.cti_path)
          + QLatin1Char('|') + QString::fromStdString(info.id);
    if (key.isEmpty()) return {};
    if (!claims) throw onegen::DeviceClaimRefused("The host did not provide hardware resource claims.");
    QString error;
    auto claim = claims->claimResource(key, &error);
    if (!claim) throw onegen::DeviceClaimRefused(error.toStdString());
    return claim;
  }

  /** @brief Claims one camera on a network or USB Interface before its Device opens. */
  std::shared_ptr<void> claimCamera(const onegen::InterfaceInfo& info,
      const onegen::DeviceInfo& device)
  {
    if (onegen::interfaceClaimScope(info.tl_type) != onegen::InterfaceClaimScope::Camera)
      return {};
    const auto key = playgroundCameraResourceKey(QString::fromStdString(device.serial));
    if (key.isEmpty()) {
      // One camera claims its serial exactly once. Without a serial the device cannot be
      // identified by the shared key rule, so it stays unclaimed and the reason is recorded.
      qWarning().noquote() << QStringLiteral(
          "[GenICam] camera=%1 interface=%2 is unclaimed: the producer reported no serial.")
          .arg(QString::fromStdString(device.id), QString::fromStdString(info.id));
      return {};
    }
    auto* claims = claims_.load(std::memory_order_acquire);
    if (!claims) throw onegen::DeviceClaimRefused("The host did not provide hardware resource claims.");
    QString error;
    auto claim = claims->claimResource(key, &error);
    if (!claim) throw onegen::DeviceClaimRefused(error.toStdString());
    return claim;
  }

  mutable std::mutex mutex_;
  std::atomic<IDevicePluginResourceClaims*> claims_{nullptr};
  std::shared_ptr<onegen::producer::Coordinator> coordinator_;
  std::shared_ptr<onegen::producer::StartupDiscovery> startup_;
  /** @brief Discovery result retained for the session opened from this plugin instance. */
  std::vector<onegen::DeviceInfo> discovered_devices_;
  GenTLDiscoveryState discovery_state_;
  bool shutting_down_ = false;
  bool producers_configured_ = false;
};

#include "OneGenPlugin.moc"
