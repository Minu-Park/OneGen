#pragma once

#include "AbstractSourceController.h"
#include "engine/GraphicsFrameAdapter.h"
#include "ToolProvider.h"
#include "DeviceParameterControl.h"
#include "OneGenTypes.h"
#include "OneGenNode.h"
#include <QFuture>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <map>
#include <string>

namespace onegen {
struct Frame;
namespace worker {
class Lease;
class Coordinator;
}
}

/** Owns OneGen live acquisition and delivers SessionFrame values synchronously. */
class OneGenSourceController final : public AbstractSourceController, public IToolProvider, public IDeviceParameterControl {
  Q_OBJECT
  Q_INTERFACES(IToolProvider IDeviceParameterControl)
public:
  explicit OneGenSourceController(QObject* parent = nullptr);
  ~OneGenSourceController() override;

  /** Returns a stable camera controller whose frames share the consumer's source topology. */
  OneGenSourceController* cameraController(const std::string& id, const std::string& label);
  /** Removes a child after its camera panel has drained acquisition and callbacks. */
  void removeCameraController(const std::string& id);
  /** Resets presentation sources after all child panels have drained. */
  void clearCameraControllers();
  /** Reports one stable presentation source per child camera. */
  std::vector<GraphicsSourceDescriptor> sourceDescriptors() const override;
  void setLease(onegen::worker::Lease* lease);
  void start() override;
  void stop() override;
  /** @brief Stops acquisition with acknowledged success or an explicit backend failure. */
  void stopLive();
  /** @brief Captures one frame, optionally retaining raw bytes in a consumer diagnostic directory. */
  void grabOne(const QString& diagnosticDirectory = {});
  void trigger();
  bool isGrabbing() const override;
  void setFrameConsumer(FrameConsumer consumer) override;
  bool supports3D() const override { return true; }
  /** @brief Configures existing module discovery and copied device inventory. */
  void configureControl(std::shared_ptr<onegen::worker::Coordinator> coordinator, std::vector<onegen::DeviceInfo> devices);
  /** @brief Replaces copied discovery results on the consumer thread. */
  void setDevices(std::vector<onegen::DeviceInfo> devices);
  /** @brief Copies the module's current producer-scoped inventory. */
  std::vector<onegen::DeviceInfo> devices() const;
  /** @brief Observes the GUI's pending discovery/Interface guard without calling widgets. */
  void setControlBlocked(bool blocked) { control_blocked_ = blocked; }
  /** @brief Shares existing GUI operation admission with external commands. */
  void setGuiBusy(bool busy) { gui_busy_.store(busy); }
  /** @brief Reports external work to GUI command admission. */
  bool externalBusy() const { return external_busy_.load(); }
  /** @brief Drains accepted API work before consumer panels are removed. */
  void drainControl() { tool_work_.waitForFinished(); }
  /** @brief Opens an exact device through the existing Coordinator and owns its lease. */
  std::shared_ptr<onegen::worker::Lease> openDevice(std::shared_ptr<onegen::worker::Coordinator> coordinator, const onegen::DeviceInfo& device);
  /** @brief Stops and closes the owning device channel, preserving failed-close state. */
  void closeDevice();
  /** @brief Copies a borrowed lease reference for existing feature controls. */
  std::shared_ptr<onegen::worker::Lease> deviceLease() const;
  /** @brief Copies cached feature/stream observations without SDK calls on the GUI thread. */
  std::vector<std::pair<std::string, onegen::FeatureNode>> controlFeatures() const;
  std::vector<onegen::DataStreamInfo> controlStreams() const;
  /** @brief Declares neutral device commands independently of visible controls. */
  QList<ToolDescriptor> toolDescriptors() const override;
  /** @brief Admits one bounded external command and drains it before destruction. */
  ApiResult<void> requestTool(const QString& name, int version, QJsonObject arguments, IToolProvider::Completion completion) override;
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

signals:
  /** @brief Publishes domain state; consumer views refresh on their owning thread. */
  void controlChanged();
  /** @brief Requests optional presentation of an explicitly selected opaque device key. */
  void presentDevice(const QString& key);
  /** @brief Publishes completed copied discovery without exposing widget state. */
  void inventoryChanged();

private:
  void deliver(const onegen::Frame& source, const std::string& sourceName);
  void bindLease();
  mutable std::mutex mutex_;
  struct Child { unsigned int index; std::string label; std::unique_ptr<OneGenSourceController> controller; };
  std::map<std::string, Child> children_;
  unsigned int next_source_index_ = 0;
  onegen::worker::Lease* lease_ = nullptr;
  std::unique_ptr<GraphicsFrameCallbackGate> callback_gate_;
  FrameConsumer consumer_;
  std::atomic<bool> grabbing_{false};
  std::atomic<std::uint64_t> sequence_{0};
  std::atomic<std::uint64_t> rejected_frames_{0};
  std::atomic<std::uint64_t> incomplete_frames_{0};
  std::atomic<std::uint64_t> received_frames_{0};
  std::atomic<std::uint64_t> converted_frames_{0};
  std::atomic<std::uint64_t> partial_frames_{0};
  std::shared_ptr<onegen::worker::Lease> owned_lease_;
  std::shared_ptr<onegen::worker::Coordinator> coordinator_;
  std::vector<onegen::DeviceInfo> devices_;
  QString connection_id_;
  std::atomic<bool> gui_busy_{false}, external_busy_{false};
  bool control_blocked_ = false;
  QFuture<void> tool_work_;
  std::vector<std::pair<std::string, onegen::FeatureNode>> control_features_;
  std::vector<onegen::DataStreamInfo> control_streams_;
  void refreshControlSnapshot();
  QJsonObject controlState() const;
  DeviceParameterResult parameterAdmission();
  std::vector<OneGenSourceController*> parameterControllers() const;
  DeviceParameterDescriptor observeParameter(const std::shared_ptr<onegen::worker::Lease>& lease,
      const std::string& connection, int module, const onegen::FeatureNode& node,
      bool completeContext = false, const std::string& scopeOverride = {});
  DeviceParameterResult requestParameter(DeviceParameterTarget target,
      std::optional<DeviceParameterValue> value, std::optional<std::uint64_t> revision,
      bool mutate, IDeviceParameterControl::Completion completion);
  std::map<std::string, DeviceParameterDescriptor> parameter_observations_;
  std::uint64_t parameter_revision_ = 0;
};
