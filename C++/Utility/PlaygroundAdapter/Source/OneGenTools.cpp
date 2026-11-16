#include "OneGenSourceController.h"
#include "ProducerCoordinator.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace {
using Admission = ApiResult<void>;
using ToolResult = ApiResult<QJsonValue>;

QJsonObject textField(const int maxLength = 512) {
  return {{"type", "string"}, {"format", "text"}, {"maxLength", maxLength}};
}
QJsonObject enumField(const QStringList& values) {
  QJsonArray allowed;
  for (const auto& value : values) allowed.append(value);
  return {{"type", "string"}, {"format", "enum"}, {"enum", allowed}};
}
QJsonObject objectSchema(const QJsonObject& properties, const QStringList& required) {
  QJsonArray requiredFields;
  for (const auto& field : required) requiredFields.append(field);
  return {{"type", "object"}, {"properties", properties},
      {"required", requiredFields}, {"additionalProperties", false}};
}
QJsonObject streamObject() {
  return {{"type", "object"}, {"properties", QJsonObject{
      {"index", QJsonObject{{"type", "integer"}}},
      {"id", QJsonObject{{"type", "string"}}}}},
      {"required", QJsonArray{QStringLiteral("index"), QStringLiteral("id")}},
      {"additionalProperties", false}};
}
QJsonArray streamsJson(const std::vector<onegen::DataStreamInfo>& streams) {
  QJsonArray result;
  for (const auto& stream : streams) {
    result.append(QJsonObject{{"index", static_cast<int>(stream.index)},
        {"id", QString::fromStdString(stream.id)}});
  }
  return result;
}
QStringList moduleNames() {
  return {QStringLiteral("RemoteDevice"), QStringLiteral("DataStream"),
      QStringLiteral("Device"), QStringLiteral("Interface"), QStringLiteral("System/TL")};
}
std::optional<int> moduleIndex(const QString& module) {
  const auto names = moduleNames();
  const auto index = names.indexOf(module);
  if (index < 0) return std::nullopt;
  return index;
}
QStringList streamOptionsFor(OneGenSourceController& controller) {
  QStringList options;
  for (const auto& stream : controller.controlStreams())
    options.push_back(QString::number(stream.index));
  if (options.isEmpty()) options.push_back(QStringLiteral("0"));
  return options;
}
bool exactString(const QJsonValue& value, const int maxLength) {
  if (!value.isString()) return false;
  const auto text = value.toString();
  return !text.isEmpty() && text.size() <= maxLength && !text.contains(QChar::Null);
}
}

QList<ToolDescriptor> OneGenSourceController::toolDescriptors() const {
  Q_ASSERT(QThread::currentThread() == thread());
  QStringList streamOptions;
  {
    std::lock_guard lock(mutex_);
    for (const auto& stream : control_streams_) streamOptions.push_back(QString::number(stream.index));
  }
  if (streamOptions.isEmpty()) streamOptions.push_back(QStringLiteral("0"));
  const bool connected = coordinator_ && (interface_ || owned_lease_ || !children_.empty());
  const QString unavailable = coordinator_ ? QStringLiteral("Connect a device first.")
                                           : QStringLiteral("Device control has no coordinator.");
  const QJsonObject connectionField = textField();
  const QJsonObject streamOutput = objectSchema({
      {"connectionId", textField()},
      {"stream", streamObject()},
      {"streams", QJsonObject{{"type", "array"}, {"items", streamObject()}}}},
      {"connectionId", "stream", "streams"});
  const QJsonObject settingsOutput = objectSchema({
      {"connectionId", textField()},
      {"module", enumField(moduleNames())},
      {"settings", textField(1 << 25)}},
      {"connectionId", "module", "settings"});
  const QJsonObject restoreOutput = objectSchema({
      {"connectionId", textField()},
      {"module", enumField(moduleNames())},
      {"restored", QJsonObject{{"type", "boolean"}}}},
      {"connectionId", "module", "restored"});
  QList<ToolDescriptor> result{
      {"onegen.stream-select", 1, "Select data stream",
          "Select the GenTL data stream of one camera connection. Stop Live before changing the stream.",
          ToolEffect::Device, ToolEgress::Metadata, connected, unavailable,
          objectSchema({{"connectionId", connectionField},
              {"stream", enumField(streamOptions)}}, {"connectionId"}),
          streamOutput, {QStringLiteral("onegen.stream-select"), {}, QStringLiteral("onegen"), 10}},
      {"onegen.copy-module-settings", 1, "Copy module settings",
          "Serialize one module's GenApi feature bag. Camera connections expose RemoteDevice, "
          "DataStream and Device; the Interface connection exposes Interface and System/TL.",
          ToolEffect::Observe, ToolEgress::Metadata, connected, unavailable,
          objectSchema({{"connectionId", connectionField},
              {"module", enumField(moduleNames())}}, {"connectionId"}),
          settingsOutput, {QStringLiteral("onegen.copy-module-settings"), {}, QStringLiteral("onegen"), 20}},
      {"onegen.restore-module-settings", 1, "Restore module settings",
          "Restore and verify one module's GenApi feature bag. Stop Live before restoring camera settings.",
          ToolEffect::Device, ToolEgress::Metadata, connected, unavailable,
          objectSchema({{"connectionId", connectionField},
              {"module", enumField(moduleNames())},
              {"settings", textField(1 << 25)}}, {"connectionId"}),
          restoreOutput, {QStringLiteral("onegen.restore-module-settings"), {}, QStringLiteral("onegen"), 21}}};
  return result;
}

ApiResult<void> OneGenSourceController::requestTool(const QString& name, const int version,
    QJsonObject arguments, IToolProvider::Completion completion) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!completion) return Admission::failure(ApiErrorCode::InvalidArgument, "Completion is required.");
  std::optional<ToolDescriptor> descriptor;
  for (const auto& candidate : toolDescriptors())
    if (candidate.name == name) descriptor = candidate;
  if (!descriptor) return Admission::failure(ApiErrorCode::NotFound, "Tool is not declared.");
  if (version != descriptor->version)
    return Admission::failure(ApiErrorCode::Unsupported, "Unsupported tool version.");
  if (!descriptor->available)
    return Admission::failure(ApiErrorCode::Unavailable, descriptor->unavailableReason);
  const auto properties = descriptor->inputSchema.value("properties").toObject();
  for (auto it = arguments.begin(); it != arguments.end(); ++it) {
    if (!properties.contains(it.key()))
      return Admission::failure(ApiErrorCode::InvalidArgument, "Unknown tool argument: " + it.key());
    if (!it.value().isString())
      return Admission::failure(ApiErrorCode::InvalidArgument, "Tool arguments must be exact text values.");
  }
  const auto connectionId = arguments.value(QStringLiteral("connectionId")).toString();
  if (!exactString(arguments.value(QStringLiteral("connectionId")), 512))
    return Admission::failure(ApiErrorCode::InvalidArgument, "Exact connectionId is required.");
  const auto endpoint = resolveParameterEndpoint(connectionId.toStdString());
  if (!endpoint) return Admission::failure(ApiErrorCode::NotFound,
      "Connection generation is not active; observe the connection again.");
  if (!coordinator_)
    return Admission::failure(ApiErrorCode::Unavailable, "Device control has no coordinator.");
  auto* selected = endpoint->owner ? endpoint->owner : this;
  if (external_busy_.load() || selected->external_busy_.load()
      || parameter_work_.load() > 0 || selected->parameter_work_.load() > 0
      || selected->active_work_.load() > 0)
    return Admission::failure(ApiErrorCode::Busy, "A control operation is active on this connection.");

  if (name == QLatin1String("onegen.stream-select")) {
    if (endpoint->isInterface)
      return Admission::failure(ApiErrorCode::InvalidArgument,
          "Select a camera connection for the data stream.");
    if (endpoint->owner->isGrabbing())
      return Admission::failure(ApiErrorCode::Conflict, "Stop Live before selecting a data stream.");
    const auto streamText = arguments.value(QStringLiteral("stream")).toString();
    if (streamText.isEmpty()) {
      completion(argumentRequiredFailure(QStringLiteral("stream"), QStringLiteral("enum"),
          QStringLiteral("Select the data stream."), streamOptionsFor(*selected)));
      return Admission::success();
    }
    bool validIndex = false;
    const auto streamIndex = streamText.toUInt(&validIndex);
    const auto stream = endpoint;
    admitParameterWork(selected);
    auto work = QtConcurrent::run([this, selected, stream, streamIndex, validIndex,
                                   completion = std::move(completion)] {
      ToolResult result = ToolResult::failure(ApiErrorCode::Internal, "The action did not complete.");
      try {
        std::lock_guard lock(parameter_write_mutex_);
        const auto streams = stream->lease->data_streams();
        std::optional<onegen::DataStreamInfo> found;
        for (const auto& candidate : streams)
          if (validIndex && candidate.index == streamIndex) found = candidate;
        if (!found) {
          result = ToolResult::failure(ApiErrorCode::InvalidArgument,
              "The stream value is not a declared data stream index.");
        } else {
          stream->lease->select_data_stream(streamIndex);
          try { selected->refreshControlSnapshot(); } catch (...) {}
          try { notifyParameterResync(stream->connectionId, false, "Data stream selected."); } catch (...) {}
          const auto updated = stream->lease->data_streams();
          result = ToolResult::success(QJsonObject{
              {"connectionId", QString::fromStdString(stream->connectionId)},
              {"stream", QJsonObject{{"index", static_cast<int>(found->index)},
                  {"id", QString::fromStdString(found->id)}}},
              {"streams", streamsJson(updated)}});
        }
      } catch (const std::exception& error) {
        result = ToolResult::failure(ApiErrorCode::Conflict, error.what());
      } catch (...) {
        result = ToolResult::failure(ApiErrorCode::Internal, "Unexpected stream selection failure.");
      }
      finishParameterWork(selected);
      try { completion(std::move(result)); }
      catch (...) { qWarning() << "[GenICam] tool consumer callback threw."; }
    });
    trackParameterWork(work);
    return Admission::success();
  }

  QString moduleName = arguments.value(QStringLiteral("module")).toString();
  if (moduleName.isEmpty()) {
    completion(argumentRequiredFailure(QStringLiteral("module"), QStringLiteral("enum"),
        QStringLiteral("Select the settings module."), moduleNames()));
    return Admission::success();
  }
  const auto module = moduleIndex(moduleName);
  if (!module)
    return Admission::failure(ApiErrorCode::InvalidArgument, "The settings module is not declared.");
  if (endpoint->isInterface && *module < 3)
    return Admission::failure(ApiErrorCode::InvalidArgument,
        "The Interface connection exposes only Interface and System/TL settings.");
  if (!endpoint->isInterface && *module >= 3)
    return Admission::failure(ApiErrorCode::InvalidArgument,
        "Use the Interface connection for Interface and System/TL settings.");
  const bool restore = name == QLatin1String("onegen.restore-module-settings");
  auto settings = arguments.value(QStringLiteral("settings")).toString();
  if (restore && settings.isEmpty()) {
    completion(argumentRequiredFailure(QStringLiteral("settings"), QStringLiteral("text"),
        QStringLiteral("Paste the copied module settings.")));
    return Admission::success();
  }
  if (restore && settings.size() > (1 << 25))
    return Admission::failure(ApiErrorCode::InvalidArgument, "The settings payload exceeds its bound.");
  if (restore && *module == 3 && !endpoint->isInterface)
    return Admission::failure(ApiErrorCode::InvalidArgument,
        "Use the Interface connection for Interface settings.");
  if (restore && *module == 3) {
    bool camerasLeased = false;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [key, child] : children_) {
        std::lock_guard cameraLock(child.controller->mutex_);
        camerasLeased = camerasLeased || bool(child.controller->owned_lease_);
      }
    }
    if (camerasLeased)
      return Admission::failure(ApiErrorCode::Conflict,
          "Disconnect all cameras on this Interface before restoring Interface settings.");
  }
  admitParameterWork(selected);
  auto work = QtConcurrent::run([this, selected, endpoint, module = *module, restore,
                                 settings = settings.toStdString(),
                                 completion = std::move(completion)] {
    ToolResult result = ToolResult::failure(ApiErrorCode::Internal, "The action did not complete.");
    try {
      std::optional<std::string> bag;
      if (restore) {
        std::lock_guard lock(parameter_write_mutex_);
        if (endpoint->isInterface) {
          if (module == 3) coordinator_->load_interface_features(endpoint->info, settings);
          else coordinator_->load_system_features(endpoint->info, settings);
        } else {
          endpoint->lease->load_features(module, settings);
        }
      } else {
        if (endpoint->isInterface)
          bag = module == 3 ? coordinator_->save_interface_features(endpoint->info)
                            : coordinator_->save_system_features(endpoint->info);
        else
          bag = endpoint->lease->save_features(module);
      }
      if (restore && !endpoint->isInterface) {
        try { selected->refreshControlSnapshot(); } catch (...) {}
      }
      if (restore) {
        try {
          notifyParameterResync(endpoint->connectionId, module == 4, "Module settings restored.");
        } catch (...) {}
      }
      const auto moduleText = moduleNames().at(module);
      result = ToolResult::success(restore
          ? QJsonObject{{"connectionId", QString::fromStdString(endpoint->connectionId)},
              {"module", moduleText}, {"restored", true}}
          : QJsonObject{{"connectionId", QString::fromStdString(endpoint->connectionId)},
              {"module", moduleText}, {"settings", QString::fromStdString(*bag)}});
    } catch (const std::exception& error) {
      result = ToolResult::failure(restore ? ApiErrorCode::Conflict : ApiErrorCode::Unavailable,
          error.what());
    } catch (...) {
      result = ToolResult::failure(ApiErrorCode::Internal, "Unexpected module settings failure.");
    }
    finishParameterWork(selected);
    try { completion(std::move(result)); }
    catch (...) { qWarning() << "[GenICam] tool consumer callback threw."; }
  });
  trackParameterWork(work);
  return Admission::success();
}
