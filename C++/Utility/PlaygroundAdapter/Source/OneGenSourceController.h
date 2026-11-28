#pragma once

#include "DeviceRuntime/Contract/FrameContextClient.h"
#include "DeviceRuntime/Contract/AbstractSourceController.h"
#include "DeviceRuntime/Contract/DeviceDiagnostics.h"
#include "DeviceRuntime/Contract/DeviceImageDiagnostics.h"
#include "engine/GraphicsFrameAdapter.h"
#include "DeviceRuntime/Contract/DeviceConnectionControl.h"
#include "DeviceRuntime/Contract/DeviceParameterControl.h"
#include "Tools/ToolProvider.h"
#include "OneGenTypes.h"
#include "OneGenNode.h"
#include <QFuture>

#include <condition_variable>
#include <functional>
#include <thread>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace onegen {
struct Frame;
namespace producer {
class Lease;
class Coordinator;
struct SharedFrame;
class DirectLeaseSink;
}
}

struct OneGenImageRejection;
struct OneGenPlanarRejection;
struct OneGenFrameAnomalies;

/** @brief Reports a refused acquisition-mode write before any acquisition effect. */
class OneGenAcquisitionModeRefused : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/** Owns OneGen live acquisition and delivers SessionFrame values synchronously. */
class OneGenSourceController final : public AbstractSourceController, public IDeviceConnectionControl, public IDeviceParameterControl,
                                     public IFrameContextClient, public IToolProvider {
  Q_OBJECT
  Q_INTERFACES(IDeviceConnectionControl IDeviceParameterControl IFrameContextClient IToolProvider)
public:
  struct DirectDestinationPool;
  struct DirectDestinationState;
  /** @brief Applies to this controller and its camera controllers from their next Single/Live start. */
  void setFrameContextSource(std::shared_ptr<GraphicsFrameContextSource> source) override;
  using TargetsCompletion = IDeviceConnectionControl::TargetsCompletion;
  explicit OneGenSourceController(QObject* parent = nullptr,
      DeviceImageRejectionSink imageRejectionSink = nullptr,
      DevicePlanarRejectionSink planarRejectionSink = nullptr, std::uint32_t imageSourceIndex = 0);
  ~OneGenSourceController() override;

  /** Returns a stable camera controller whose frames share the consumer's source topology. */
  OneGenSourceController* cameraController(const std::string& id, const std::string& label);
  /** Removes a child after its camera panel has drained acquisition and callbacks. */
  void removeCameraController(const std::string& id);
  /** Resets presentation sources after all child panels have drained. */
  void clearCameraControllers();
  /** Reports one stable presentation source per child camera. */
  std::vector<GraphicsSourceDescriptor> sourceDescriptors() const override;
  void setLease(onegen::producer::Lease* lease);
  void start() override;
  void stop() override;
  /** @brief Stops acquisition with acknowledged success or an explicit backend failure. */
  void stopLive();
  /** @brief Stops only this controller's own lease, without its children. */
  void stopOwnAcquisition();
  /** @brief Captures one frame, optionally retaining raw bytes in a consumer diagnostic directory. */
  void grabOne(const QString& diagnosticDirectory = {});
  void trigger();
  bool isGrabbing() const override;
  /** @brief Copies the cameras that failed the most recent all-camera start or stop. */
  std::vector<SourceOperationFailure> sourceOperationFailures() const override;
  void setFrameConsumer(FrameConsumer consumer) override;
  bool supports3D() const override { return true; }
  /**
   * @brief Returns producer counters for the device statusbar.
   * @param sourceIndex Child-camera source index, or zero for a direct camera.
   * @note Reads atomics only; it never waits for acquisition or the producer.
   */
  [[nodiscard]] SourceRuntimeDiagnostics runtimeDiagnostics(unsigned sourceIndex) const;
  /** @brief Copies the same per-camera counters sampled by the panel. */
  std::optional<SourceRuntimeDiagnostics> sourceRuntimeDiagnostics(unsigned sourceIndex) const override;
  /** @brief Configures existing module discovery and copied device inventory. */
  void configureControl(std::shared_ptr<onegen::producer::Coordinator> coordinator, std::vector<onegen::DeviceInfo> devices);
  /** @brief Replaces copied discovery results on the consumer thread. */
  void setDevices(std::vector<onegen::DeviceInfo> devices);
  /** @brief Copies the module's current producer-scoped inventory. */
  std::vector<onegen::DeviceInfo> devices() const;
  /** @brief Reports why producer search paths cannot change while this session is open. */
  QString genTLPathChangeBlockReason() const;
  /** @brief Supplies one already-running shared scan's final inventory to a discovery admission. */
  using DiscoveryFn = std::function<std::vector<onegen::DeviceInfo>()>;
  /** @brief Retries discovery through the same admission and inventory path as device.discover. */
  bool retryDiscovery(QString* error, DiscoveryFn discovery = {});
  /** @brief Observes the GUI's pending discovery/Interface guard without calling widgets. */
  void setControlBlocked(bool blocked) { control_blocked_ = blocked; }
  /** @brief Shares existing GUI operation admission with external commands. */
  void setGuiBusy(bool busy) { gui_busy_.store(busy); }
  /** @brief Reports external work to GUI command admission. */
  bool externalBusy() const { return external_busy_.load(); }
  /** @brief Drains accepted API work before consumer panels are removed. */
  void drainControl();
  /** @brief Opens an exact device through the existing Coordinator and owns its lease. */
  std::shared_ptr<onegen::producer::Lease> openDevice(std::shared_ptr<onegen::producer::Coordinator> coordinator, const onegen::DeviceInfo& device);
  /** @brief Stops and closes the owning device channel, preserving failed-close state. */
  void closeDevice();
  /** @brief Copies a borrowed lease reference for existing feature controls. */
  std::shared_ptr<onegen::producer::Lease> deviceLease() const;
  /** @brief Copies cached feature/stream observations without SDK calls on the GUI thread. */
  std::vector<std::pair<std::string, onegen::FeatureNode>> controlFeatures() const;
  std::vector<onegen::DataStreamInfo> controlStreams() const;
  /** @brief Declares every lifecycle operation while a discovery coordinator is configured. */
  DeviceConnectionCapabilities connectionCapabilities() const override;
  /** @brief Copies the cached producer/interface/device targets without refreshing them. */
  DeviceConnectionResult requestConnectionTargets(TargetsCompletion completion) override;
  /** @brief Observes the primary connection and converted-frame count. */
  DeviceConnectionResult requestConnectionState(IDeviceConnectionControl::Completion completion) override;
  /** @brief Observes the Interface and every camera connection with per-connection state. */
  DeviceConnectionResult requestConnectionStates(IDeviceConnectionControl::StatesCompletion completion) override;
  /** @brief Runs producer discovery; while connected it only annotates lost cameras. */
  DeviceConnectionResult requestDiscover(TargetsCompletion completion) override;
  /** @brief Opens an Interface explicitly, or a camera and its Interface implicitly. */
  DeviceConnectionResult requestConnect(std::string targetId, IDeviceConnectionControl::Completion completion) override;
  /** @brief Closes a camera, or an explicit Interface with its cameras. */
  DeviceConnectionResult requestDisconnect(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
  /** @brief Waits for native single acquisition and conversion on the exact connection. */
  DeviceConnectionResult requestSingle(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
  /** @brief Starts acquisition on the exact connection; completion is producer acknowledgement. */
  DeviceConnectionResult requestLiveStart(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
  /** @brief Stops acquisition on the exact connection. */
  DeviceConnectionResult requestLiveStop(std::string connectionId, IDeviceConnectionControl::Completion completion) override;
  /** @brief Copies typed device features using the existing lease authority. */
  DeviceParameterResult requestParameters(InventoryCompletion completion) override;
  /** @brief Reads one exact current connection and selector context. */
  DeviceParameterResult requestReadParameter(DeviceParameterTarget target, IDeviceParameterControl::Completion completion) override;
  /** @brief Validates and writes through the same lease used by GUI controls. */
  DeviceParameterResult requestWriteParameter(DeviceParameterTarget target, DeviceParameterValue value,
      std::optional<std::uint64_t> revision, IDeviceParameterControl::Completion completion) override;
  /** @brief Executes an existing command without implicit acquisition changes. */
  DeviceParameterResult requestExecuteParameter(DeviceParameterTarget target,
      std::optional<std::uint64_t> revision, IDeviceParameterControl::Completion completion) override;
  /** @brief Admits a snapshot-plus-delta subscription for one exact connection. */
  DeviceParameterResult requestParameterChanges(std::string connectionId, std::string subscriptionId,
      ChangeListener delivery) override;
  /** @brief Stops delivery for one subscription and completes after it drained. */
  DeviceParameterResult releaseParameterChanges(std::string connectionId, std::string subscriptionId,
      IDeviceParameterControl::Completion completion) override;
  /** @brief Declares the OneGen module actions with semantic presentation metadata. */
  QList<ToolDescriptor> toolDescriptors() const override;
  /** @brief Admits one declared OneGen module action. */
  ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments,
      IToolProvider::Completion completion) override;

signals:
  /** @brief Publishes domain state; consumer views refresh on their owning thread. */
  void controlChanged();
  /** @brief Requests optional presentation of an explicitly selected opaque device key. */
  void presentDevice(const QString& key);
  /** @brief Publishes completed copied discovery without exposing widget state. */
  void inventoryChanged();

private:
  friend class FeatureTreeTests;
  friend class OneGenSourceControllerTests;
  struct ContextSourceState {
    std::shared_ptr<GraphicsFrameContextSource> source;
  };
  struct DirectRunRecord {
    std::shared_ptr<ContextSourceState> sourceState;
    GraphicsFrameCallbackGate::Token gateToken;
    std::string sourceName;
  };
  void deliver(const onegen::Frame& source, const std::string& sourceName, bool single = false);
  void deliverDirect(const onegen::producer::SharedFrame& source);
  std::function<void(const onegen::Frame&)> makeAcquisitionCallback(
      const std::string& sourceName, GraphicsFrameCallbackGate::Token token);
  GraphicsFrameCallbackGate::Token ensureCallbackGateLocked(
      onegen::producer::Lease& lease, const std::string& sourceName);
  std::shared_ptr<ContextSourceState> activeContextSource(std::string* error = nullptr) const;
  /** @brief Counts @p repeat occurrences of an image anomaly and forwards the first and every hundredth. */
  bool reportImageRejection(const OneGenImageRejection& evidence, const onegen::Frame& source,
      std::uint64_t sequence, std::size_t readableBytes, bool directInput, bool single,
      std::size_t repeat = 1) noexcept;
  /** @brief Counts @p repeat occurrences of a part anomaly and forwards the first and every hundredth. */
  void reportPlanarRejection(const OneGenPlanarRejection& evidence, const onegen::Frame& source,
      std::uint64_t sequence, const std::optional<GraphicsFrame>& output, bool single,
      std::size_t repeat = 1) noexcept;
  /** @brief Reports every anomaly of one frame, including those counted only in the overflow arrays.
   * @return True when at least one anomaly reached a diagnostic sink.
   */
  bool reportAnomalies(const OneGenFrameAnomalies& anomalies, const onegen::Frame& source,
      std::uint64_t sequence, std::size_t readableBytes, bool directInput,
      const std::optional<GraphicsFrame>& output, bool single) noexcept;
  void bindLease();
  /** @brief Frames dropped for lack of a free slot or ready credit since the last start. */
  std::uint64_t droppedFrames() const;
  /** @brief Attaches the context-backed direct lease sink while caller holds direct_lease_mutex_. */
  void configureDirectSink(onegen::producer::Lease& lease, const std::string& sourceName);
  mutable std::mutex mutex_;
  std::shared_ptr<ContextSourceState> context_source_;
  std::shared_ptr<DirectDestinationPool> destination_pool_;
  std::shared_ptr<const DirectRunRecord> active_direct_run_;
  std::string last_single_failure_;
  /** @brief Serializes direct sink attachment with lease replacement without callbacks under mutex_. */
  std::mutex direct_lease_mutex_;
  struct Child { unsigned int index; std::string label; std::unique_ptr<OneGenSourceController> controller; };
  std::map<std::string, Child> children_;
  unsigned int next_source_index_ = 0;
  onegen::producer::Lease* lease_ = nullptr;
  std::unique_ptr<GraphicsFrameCallbackGate> callback_gate_;
  /** Attached lease sink; diagnostics copy this pointer under mutex_ and read exhausted() atomically. */
  std::shared_ptr<const onegen::producer::DirectLeaseSink> lease_sink_;
  /** Lease-sink exhaustion count at the last start, so failed counts one run. */
  std::atomic<std::uint64_t> exhausted_baseline_{0};
  FrameConsumer consumer_;
  // A child counts delivery in the parent forwarding callback, after the final consumer returns.
  bool forwarded_by_parent_ = false;
  const DeviceImageRejectionSink image_rejection_sink_;
  const DevicePlanarRejectionSink planar_rejection_sink_;
  const std::uint32_t image_source_index_;
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DeviceImageRejection::NoImage) + 1>
      image_rejection_counts_{};
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DevicePlanarRejection::RangeStorageRefused) + 1>
      planar_rejection_counts_{};
  std::atomic<bool> grabbing_{false};
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  std::atomic<std::uint64_t> incomplete_frames_{0};
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> converted_frames_{0};
  std::atomic<std::uint64_t> delivered_frames_{0};
  std::atomic<std::uint64_t> partial_frames_{0};
  std::shared_ptr<onegen::producer::Lease> owned_lease_;
  std::shared_ptr<onegen::producer::Coordinator> coordinator_;
  std::vector<onegen::DeviceInfo> devices_;
  QString connection_id_;
  /** @brief Parent Interface connection id for one API-connected camera. */
  QString parent_connection_id_;
  std::atomic<bool> gui_busy_{false}, external_busy_{false};
  /** @brief Accepted lifecycle or parameter work not yet completed; cleared before its callback. */
  std::atomic<int> active_work_{0};
  /** @brief Accepted structural work; per-connection work must wait for it. */
  std::atomic<int> structural_work_{0};
  bool control_blocked_ = false;
  QFuture<void> tool_work_;
  /** @brief Serializes admitted work bookkeeping across the GUI thread and completions. */
  mutable std::mutex work_mutex_;
  std::vector<QFuture<void>> admitted_work_;
  /** @brief Disconnected children destroyed after their own admitted work finished. */
  std::vector<std::unique_ptr<OneGenSourceController>> retired_;
  std::vector<std::pair<std::string, onegen::FeatureNode>> control_features_;
  std::vector<onegen::DataStreamInfo> control_streams_;
  void refreshControlSnapshot();
  /** @brief The one Interface this session opened, explicitly or through a camera. */
  struct InterfaceConnection {
    onegen::InterfaceInfo info;
    QString connectionId;
    bool implicit = false;
    int refCount = 0;
    std::string error;
  };
  std::optional<InterfaceConnection> interface_;
  struct ErrorState {
    std::mutex mutex;
    std::string message;
  };
  std::shared_ptr<ErrorState> error_state_ = std::make_shared<ErrorState>();
  std::string discovery_error_;
  std::vector<SourceOperationFailure> operation_failures_;
  void handleLeaseError(std::string message);
  void setDiscoveryError(std::string message);
  /** @brief Copies the primary connection: the Interface, else the first camera by index. */
  DeviceConnectionState connectionState() const;
  /** @brief Copies one connection by id, including its per-connection capabilities. */
  DeviceConnectionState stateForConnection(const std::string& connectionId) const;
  /** @brief Copies every open connection, Interface first and cameras by source index. */
  std::vector<DeviceConnectionState> connectionStates() const;
  DeviceConnectionState interfaceStateLocked() const;
  DeviceConnectionState cameraStateLocked(const std::string& key, const Child& child) const;
  DeviceConnectionAvailabilitySet interfaceAvailability() const;
  /** @brief Resolves one published target id to its Interface or camera facts. */
  struct ResolvedTarget {
    bool isInterface = false;
    onegen::DeviceInfo device;
    onegen::InterfaceInfo interfaceInfo;
  };
  std::optional<ResolvedTarget> resolveTarget(const std::string& targetId) const;
  onegen::InterfaceInfo interfaceInfoFor(const onegen::DeviceInfo& device) const;
  /** @brief Opens the camera's Interface implicitly when the session owns none yet. */
  onegen::InterfaceInfo ensureCameraInterface(const onegen::DeviceInfo& device);
  /** @brief Closes an implicit Interface after its last camera connection left. */
  void closeIdleInterface();
  void annotateDiscovery(const std::vector<onegen::DeviceInfo>& discovered, bool interfacePresent);
  /** @brief Whether one exact controller has active accepted work or GUI work. */
  static bool controllerBusy(const OneGenSourceController& controller);
  /** @brief One applied AcquisitionMode transition for the owning acquisition action. */
  struct AcquisitionModeChange {
    bool changed = false;
    std::string previous;
    std::string current;
    std::string sourceName;
  };
  /** @brief Writes the action's AcquisitionMode and throws `OneGenAcquisitionModeRefused` on refusal. */
  AcquisitionModeChange applyAcquisitionMode(bool single);
  /** @brief Records an applied mode change in the module log and structured diagnostics. */
  void reportAcquisitionMode(const AcquisitionModeChange& change, const char* action) const;
  /** @brief Derives Single availability from the cached Remote Device node map. */
  DeviceConnectionAvailability singleAvailability() const;
  /** @brief Derives the availability of one AcquisitionMode value from the cached node map. */
  static DeviceConnectionAvailability modeAvailability(
      const std::vector<std::pair<std::string, onegen::FeatureNode>>& features,
      const char* mode, const char* label);
  /** @brief Finds one copied feature by name in a feature tree. */
  static const onegen::FeatureNode* findFeature(const onegen::FeatureNode& root, std::string_view name);
  enum class Lifecycle { Discover, Connect, Disconnect, Single, LiveStart, LiveStop };
  /** @brief Admits one bounded lifecycle command and drains it before destruction. */
  DeviceConnectionResult admitLifecycle(Lifecycle operation, std::string argument,
      IDeviceConnectionControl::Completion completion, TargetsCompletion targetsCompletion,
      DiscoveryFn discovery = {});
  DeviceParameterResult parameterAdmission();
  std::vector<OneGenSourceController*> parameterControllers() const;
  /** @brief One addressable connection for a parameter request. */
  struct ParameterEndpoint {
    bool isInterface = false;
    std::string connectionId;
    onegen::InterfaceInfo info;
    std::shared_ptr<onegen::producer::Lease> lease;
    OneGenSourceController* owner = nullptr;
  };
  std::optional<ParameterEndpoint> resolveParameterEndpoint(const std::string& connectionId) const;
  struct ParameterWatch;
  DeviceParameterDescriptor observeParameter(const std::shared_ptr<onegen::producer::Lease>& lease,
      const std::string& connection, int module, const onegen::FeatureNode& node,
      const onegen::FeatureNode* moduleRoot = nullptr,
      bool completeContext = false, const std::string& scopeOverride = {});
  DeviceParameterDescriptor observeCoordinatorParameter(const ParameterEndpoint& endpoint, int module,
      const onegen::FeatureNode& node, const onegen::FeatureNode* moduleRoot,
      const std::string& scopeOverride = {});
  DeviceParameterDescriptor recordParameterObservation(DeviceParameterDescriptor descriptor);
  DeviceParameterInventory connectionInventory(const std::string& connectionId, std::string* error);
  DeviceParameterResult requestParameter(DeviceParameterTarget target,
      std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
      bool mutate, IDeviceParameterControl::Completion completion);
  DeviceParameterResult requestCoordinatorParameter(const ParameterEndpoint& endpoint,
      DeviceParameterTarget target, std::optional<DeviceParameterValue> value,
      std::optional<std::uint64_t> revision, bool mutate,
      IDeviceParameterControl::Completion completion);
  void notifyParameterChanged(const std::string& connectionId, int module,
      const DeviceParameterDescriptor& descriptor);
  /** @brief Replaces one connection's cached changes with a resync snapshot after a bulk action. */
  void notifyParameterResync(const std::string& connectionId, bool producerShared, std::string reason);
  void deliverParameterSnapshot(const std::shared_ptr<ParameterWatch>& watch,
      std::vector<DeviceParameterDescriptor> descriptors, bool resync, std::string reason);
  /** @brief Tracks one admitted parameter/tool future until completion drains. */
  void trackParameterWork(const QFuture<void>& work);
  /** @brief Marks one parameter/tool request active before its worker starts. */
  void admitParameterWork(OneGenSourceController* selected);
  /** @brief Clears one request's active mark before its completion callback runs. */
  void finishParameterWork(OneGenSourceController* selected);
  void stopParameterWatches();
  std::mutex parameter_mutex_;
  std::mutex parameter_write_mutex_;
  std::mutex parameter_watch_mutex_;
  std::map<std::string, DeviceParameterDescriptor> parameter_observations_;
  std::map<std::string, std::shared_ptr<ParameterWatch>> parameter_watches_;
  std::uint64_t parameter_revision_ = 0;
  /** @brief Admitted parameter/tool work on this controller; zero before the completion callback. */
  std::atomic<int> parameter_work_{0};
};
