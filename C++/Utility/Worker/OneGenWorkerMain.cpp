#include "OneGenWorkerMessages.h"
#include "OneGenParameterMessages.h"
#include "WorkerException.h"

#include "OneGen.h"
#include "Internal/ConnectTiming.h"
#include "Internal/StageLog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <exception>
#include <iostream>
#include <mutex>
#include <queue>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using onegen::worker::Message;
using onegen::worker::Operation;
using onegen::worker::Reader;
using onegen::worker::Writer;

std::mutex output_mutex;
std::FILE* protocol_output = nullptr;
std::queue<Message> deferred_requests;

bool readExact(void* destination, std::size_t size)
{
  std::cin.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
  return static_cast<std::size_t>(std::cin.gcount()) == size;
}

bool readMessage(Message& message)
{
  std::array<std::byte, 4> header;
  if (!readExact(header.data(), header.size())) return false;
  const auto size = onegen::worker::frameSize(header);
  if (size > onegen::worker::maximum_message_size)
    throw std::runtime_error("OneGen worker message is too large.");
  std::vector<std::byte> body(size);
  if (!readExact(body.data(), body.size())) throw std::runtime_error("Truncated OneGen worker message.");
  message = onegen::worker::decode(body);
  return true;
}

void send(Message message)
{
  const auto body = onegen::worker::encode(std::move(message));
  const auto header = onegen::worker::frameSize(static_cast<std::uint32_t>(body.size()));
  std::lock_guard lock(output_mutex);
  if (std::fwrite(header.data(), 1, header.size(), protocol_output) != header.size()
      || std::fwrite(body.data(), 1, body.size(), protocol_output) != body.size()
      || std::fflush(protocol_output) != 0)
    throw std::runtime_error("Could not write OneGen worker response.");
}

/** Camera acquisition state scoped to a channel in the shared producer worker. */
struct CameraSession {
  onegen::Camera* camera = nullptr;
  std::thread grab;
  std::mutex ack_mutex;
  std::condition_variable ack_ready;
  bool frame_acked = false;
  bool stopping = false;
  std::uint64_t stream_generation = 1;
  void stop() {
    { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
    std::exception_ptr error;
    try { if (camera) camera->stop(); } catch (...) { error = std::current_exception(); }
    if (grab.joinable()) grab.join();
    if (error) std::rethrow_exception(error);
  }
  ~CameraSession() {
    try { stop(); if (camera) camera->close(); }
    catch (...) { onegen::stage_log_warning("worker session cleanup failed: %s", onegen::worker::currentExceptionMessage().c_str()); }
  }
};

void sendFor(std::uint32_t session, Message message) {
  message.session = session;
  send(std::move(message));
}

int moduleIndex(onegen::Camera::Module module)
{
  switch (module) {
    case onegen::Camera::Module::RemoteDevice: return 0;
    case onegen::Camera::Module::DataStream: return 1;
    case onegen::Camera::Module::Device: return 2;
    case onegen::Camera::Module::Interface: return 3;
    default: return 4;
  }
}

onegen::Camera::Module moduleFromIndex(int index)
{
  if (index == 0) return onegen::Camera::Module::RemoteDevice;
  if (index == 1) return onegen::Camera::Module::DataStream;
  if (index == 2) return onegen::Camera::Module::Device;
  if (index == 3) return onegen::Camera::Module::Interface;
  if (index == 4) return onegen::Camera::Module::System;
  throw std::runtime_error("Invalid feature module index.");
}

template <class T>
void writeVector(Writer& out, const std::vector<T>& values)
{
  out.scalar(static_cast<std::uint32_t>(values.size()));
  for (const auto& value : values) onegen::worker::write(out, value);
}

std::filesystem::path ctiArgument(int argc, char** argv)
{
  for (int index = 1; index + 1 < argc; ++index)
    if (std::string_view(argv[index]) == "--cti") return argv[index + 1];
  throw std::runtime_error("OneGenWorker requires --cti <path>.");
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  protocol_output = _fdopen(_dup(_fileno(stdout)), "wb");
  _dup2(_fileno(stderr), _fileno(stdout));
  SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
#else
  protocol_output = fdopen(dup(fileno(stdout)), "wb");
  dup2(fileno(stderr), fileno(stdout));
#endif
  if (!protocol_output) {
    std::fprintf(stderr, "[GenICam] [WARN] worker startup failed: could not open protocol output\n");
    std::fflush(stderr);
    return 1;
  }
  std::string producer;
  try {
    const auto cti = ctiArgument(argc, argv);
    producer = cti.string();
    onegen::OneGen system;
    std::string inactive_error;
    try { system.loadCti(cti); }
    catch (const std::exception& error) {
      inactive_error = std::string("Producer inactive: ") + error.what()
          + ". Retry discovery explicitly after correcting the producer configuration.";
      std::cerr << inactive_error << '\n';
    }
    if (std::any_of(argv + 1, argv + argc, [](const char* arg) {
          return std::string_view(arg) == "--resource-claims";
        })) {
      system.set_interface_claim_callback([](const onegen::InterfaceInfo& info) -> std::shared_ptr<void> {
        Writer out;
        onegen::worker::write(out, info);
        send({Operation::ClaimInterface, 0, true, {}, out.take()});
        Message reply;
        while (readMessage(reply)) {
          if (reply.operation != Operation::ClaimInterface) {
            deferred_requests.push(std::move(reply));
            continue;
          }
          if (!reply.success) throw std::runtime_error(reply.error);
          return std::shared_ptr<void>(new std::string(info.id), [](void* value) {
            std::unique_ptr<std::string> id(static_cast<std::string*>(value));
            try {
              Writer out;
              out.string(*id);
              send({Operation::ReleaseInterfaceClaim, 0, true, {}, out.take()});
            } catch (...) {}
          });
        }
        throw std::runtime_error("Consumer disconnected during interface claim.");
      });
    }
    std::map<std::uint32_t, std::unique_ptr<CameraSession>> sessions;
    Message request;
    while (!deferred_requests.empty() || readMessage(request)) {
      if (!deferred_requests.empty()) {
        request = std::move(deferred_requests.front());
        deferred_requests.pop();
      }
      if (!inactive_error.empty()) {
        if (request.operation == Operation::Shutdown) {
          send({request.operation, request.id, true, {}, {}, request.session});
          break;
        }
        send({Operation::ProducerInactive, request.id, false, inactive_error, {}, request.session});
        continue;
      }
      auto& session_ptr = sessions[request.session];
      if (!session_ptr) session_ptr = std::make_unique<CameraSession>();
      auto& channel = *session_ptr;
      auto& camera = channel.camera;
      auto& grab_thread = channel.grab;
      auto& ack_mutex = channel.ack_mutex;
      auto& ack_ready = channel.ack_ready;
      auto& frame_acked = channel.frame_acked;
      auto& stopping = channel.stopping;
      const auto abortGrab = [&channel] { channel.stop(); };
      const auto ensureCamera = [&]() -> onegen::Camera& {
        if (!camera) camera = &system.addCamera();
        return *camera;
      };
      const auto wireInvalidates = [&](onegen::Camera& opened) {
        for (const auto module : {onegen::Camera::Module::RemoteDevice,
                                  onegen::Camera::Module::DataStream,
                                  onegen::Camera::Module::Device,
                                  onegen::Camera::Module::Interface,
                                        onegen::Camera::Module::System}) {
          opened.features(module).set_invalidate_callback([session = request.session, index = moduleIndex(module)](std::string_view name) {
            // Runs inside a GenApi/producer invalidate dispatch that must not see
            // an exception. Drop the notification if the host pipe write fails.
            try {
              Writer out;
              out.scalar(index);
              out.string(name);
              sendFor(session, {Operation::Invalidate, 0, true, {}, out.take()});
            } catch (...) {
              onegen::stage_log_warning("worker invalidate notification failed: session=%u detail=%s",
                  session, onegen::worker::currentExceptionMessage().c_str());
            }
          });
        }
      };

      if (request.operation == Operation::FrameAck) {
        std::lock_guard lock(ack_mutex);
        frame_acked = true;
        ack_ready.notify_all();
        continue;
      }
      Message response{request.operation, request.id};
      response.session = request.session;
      bool shutdown = false;
      try {
        Reader input(request.payload);
        Writer output;
        switch (request.operation) {
          case Operation::OpenInterface:
            system.open_interface(onegen::InterfaceInfo{input.string(), cti.string()});
            break;
          case Operation::ProbeInterface: {
            const auto id = input.string();
            writeVector(output, system.enumerate_interface(onegen::InterfaceInfo{id, cti.string()}));
            break;
          }
          case Operation::CloseInterface:
            system.release_interface(onegen::InterfaceInfo{input.string(), cti.string()});
            break;
          case Operation::ListInterfaces:
            writeVector(output, system.interfaces());
            break;
          case Operation::InterfaceFeatures:
            onegen::worker::write(output, onegen::snapshot(system.interface_features(onegen::InterfaceInfo{input.string(), cti.string()}).root()));
            break;
          case Operation::InterfaceRead: {
            auto& map = system.interface_features(onegen::InterfaceInfo{input.string(), cti.string()});
            onegen::worker::write(output, onegen::snapshot(map.find(input.string())));
            break;
          }
          case Operation::InterfaceWrite: {
            const auto id = input.string();
            for (const auto& [key, item] : sessions)
              if (item->camera && item->camera->is_open() && item->camera->info().interface_id == id)
                throw std::runtime_error("Disconnect this interface's cameras before reconfiguration.");
            auto& map = system.interface_features(onegen::InterfaceInfo{id, cti.string()});
            auto node = map.find(input.string());
            const auto operation = input.string();
            const auto value = input.string();
            onegen::apply_feature(node, operation, value);
            onegen::worker::write(output, onegen::snapshot(map.root()));
            break;
          }
          case Operation::ReleaseInterfaces:
            system.release_interfaces();
            break;
          case Operation::Probe: {
            const auto timeout = std::clamp(input.scalar<std::uint32_t>(), 1U, 60000U);
            try {
              std::vector<std::string> failures;
              auto devices = system.enumerate(std::chrono::milliseconds{timeout}, failures);
              system.release_idle_interfaces();
              writeVector(output, devices);
              onegen::worker::writeStrings(output, failures);
            } catch (...) { system.release_idle_interfaces(); throw; }
            break;
          }
          case Operation::Open: {
            abortGrab();
            onegen::ConnectTiming timing("worker-open");
            auto& opened = ensureCamera();
            if (opened.is_open()) opened.close();
            opened.set_error_callback([session = request.session](std::string_view error) {
              sendFor(session, {Operation::Error, 0, false, std::string(error)});
            });
            onegen::DeviceInfo selected;
            selected.id = input.string();
            selected.interface_id = input.string();
            selected.cti_path = std::filesystem::weakly_canonical(cti).string();
            opened.open(selected);
            ++channel.stream_generation;
            timing.mark("camera-open");
            opened.prepare();
            timing.mark("camera-prepare");
            wireInvalidates(opened);
            try { opened.poll_features(0); }
            catch (const std::exception& error) {
              sendFor(request.session, {Operation::Error, 0, false, std::string("Feature event setup failed: ") + error.what()});
            }
            opened.set_frame_callback([&channel, session = request.session](const onegen::Frame& frame) {
              Writer out;
              onegen::worker::write(out, frame);
              {
                std::lock_guard lock(channel.ack_mutex);
                channel.frame_acked = false;
              }
              sendFor(session, {Operation::Frame, 0, true, {}, out.take()});
              std::unique_lock lock(channel.ack_mutex);
              channel.ack_ready.wait(lock, [&channel] { return channel.frame_acked || channel.stopping; });
            });
            timing.mark("callbacks-and-event-setup");
            break;
          }
          case Operation::Close:
            abortGrab();
            if (camera) camera->close();
            system.release_idle_interfaces();
            break;
          case Operation::ListStreams:
            writeVector(output, ensureCamera().data_streams());
            break;
          case Operation::SelectStream: {
            const auto index = input.scalar<std::uint32_t>();
            const auto before = ensureCamera().selected_data_stream();
            ensureCamera().select_data_stream(index);
            if (before != index) ++channel.stream_generation;
            ensureCamera().prepare();
            break;
          }
          case Operation::ObserveNode: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            auto& opened = ensureCamera();
            const auto name = input.string();
            onegen::worker::write(output, onegen::worker::NodeObservation{
                opened.features(module).find(name).parameter_snapshot(),
                opened.selected_data_stream(), channel.stream_generation});
            break;
          }
          case Operation::WriteNodeChecked: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            const auto name = input.string(); const auto operation = input.string(); const auto value = input.string();
            const auto expected = onegen::worker::readNodeObservation(input);
            const auto compareValue = input.scalar<bool>();
            using Status = onegen::worker::NodeWriteStatus;
            using Effect = onegen::worker::NodeWriteEffect;
            onegen::worker::NodeWriteResult result;
            try {
              auto& opened = ensureCamera();
              if (module == onegen::Camera::Module::Interface || module == onegen::Camera::Module::System) {
                result.status = Status::Unavailable; result.message = "Shared maps are not device parameter targets.";
              } else {
                auto node = opened.features(module).find(name);
                auto current = onegen::worker::NodeObservation{node.parameter_snapshot(),
                    opened.selected_data_stream(), channel.stream_generation};
                if (!current.node.writable || !current.node.observation_error.empty()) {
                  result.status = Status::Unavailable; result.message = "Parameter is not writable or cannot be observed.";
                } else if ((module == onegen::Camera::Module::DataStream &&
                    (current.stream_index != expected.stream_index || current.stream_generation != expected.stream_generation))
                    || !onegen::same_parameter_context(current.node, expected.node, compareValue)) {
                  result.status = Status::Conflict; result.message = "Parameter or selected stream changed before worker mutation.";
                } else {
                  result.effect = Effect::Unknown;
                  const bool trigger = module == onegen::Camera::Module::RemoteDevice && name == "TriggerSoftware" && operation == "execute";
                  if (trigger) opened.trigger();
                  else if (!node.apply_checked(operation, value, expected.node, compareValue)) {
                    result.effect = Effect::None; result.status = Status::Conflict;
                    result.message = "Node-map context changed before SDK setter.";
                    onegen::worker::write(output, result); break;
                  }
                  result.effect = Effect::Applied;
                  if (module == onegen::Camera::Module::RemoteDevice && operation != "execute" && !opened.is_live() && !opened.is_acquiring()) opened.prepare();
                  result.observation = trigger ? current : onegen::worker::NodeObservation{
                      opened.features(module).find(name).parameter_snapshot(), opened.selected_data_stream(), channel.stream_generation};
                  if (!result.observation->node.observation_error.empty()) {
                    result.status = Status::BackendFailure; result.message = result.observation->node.observation_error;
                  }
                }
              }
            } catch (const std::exception& error) { result.status = Status::BackendFailure; result.message = error.what(); }
            catch (...) { result.status = Status::BackendFailure; result.message = "Unexpected checked-write failure."; }
            onegen::worker::write(output, result);
            break;
          }
          case Operation::DescribeFeatures: {
            onegen::ConnectTiming timing("describe-features");
            output.scalar(std::uint32_t{5});
            for (const auto module : {onegen::Camera::Module::RemoteDevice,
                                      onegen::Camera::Module::DataStream,
                                      onegen::Camera::Module::Device,
                                      onegen::Camera::Module::Interface,
                                      onegen::Camera::Module::System}) {
              output.scalar(moduleIndex(module));
              onegen::worker::write(output, onegen::snapshot(ensureCamera().features(module).root()));
              timing.mark(module == onegen::Camera::Module::RemoteDevice ? "Remote-Device"
                  : module == onegen::Camera::Module::DataStream ? "Data-Stream"
                  : module == onegen::Camera::Module::Device ? "Device"
                  : module == onegen::Camera::Module::Interface ? "Interface" : "System/TL");
            }
            break;
          }
          case Operation::NodeRead: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            onegen::worker::write(output, onegen::snapshot(ensureCamera().features(module).find(input.string())));
            break;
          }
          case Operation::NodeWrite: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            const auto name = input.string();
            const auto operation = input.string();
            const auto value = input.string();
            auto& opened = ensureCamera();
            if (request.session && (module == onegen::Camera::Module::Interface || module == onegen::Camera::Module::System))
              throw std::runtime_error("Use the interface settings tab for shared configuration.");
            auto node = opened.features(module).find(name);
            onegen::apply_feature(node, operation, value);
            auto changed = onegen::snapshot(node);
            if (module == onegen::Camera::Module::RemoteDevice
                && operation != "execute" && !opened.is_live() && !opened.is_acquiring())
              opened.prepare();
            onegen::worker::write(output, changed);
            break;
          }
          case Operation::PollFeatures:
            ensureCamera().poll_features((std::min)(input.scalar<std::uint32_t>(), 60000U));
            break;
          case Operation::RegisterRead: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            const auto bytes = ensureCamera().features(module).find(input.string()).read_register();
            onegen::worker::writeBytes(output, bytes);
            break;
          }
          case Operation::RegisterWrite: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            if (request.session && (module == onegen::Camera::Module::Interface || module == onegen::Camera::Module::System))
              throw std::runtime_error("Use the interface settings tab for shared configuration.");
            const auto name = input.string();
            const auto bytes = onegen::worker::readBytes(input);
            if (ensureCamera().is_live() || ensureCamera().is_acquiring())
              throw std::runtime_error("Stop acquisition before raw register writes.");
            ensureCamera().features(module).find(name).write_register(bytes);
            ensureCamera().prepare();
            break;
          }
          case Operation::SaveFeatures: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            output.string(ensureCamera().features(module).save_features());
            break;
          }
          case Operation::LoadFeatures: {
            const auto module = moduleFromIndex(input.scalar<std::int32_t>());
            if (request.session && (module == onegen::Camera::Module::Interface || module == onegen::Camera::Module::System))
              throw std::runtime_error("Use the interface settings tab for shared configuration.");
            if (ensureCamera().is_live() || ensureCamera().is_acquiring())
              throw std::runtime_error("Stop acquisition before restoring features.");
            ensureCamera().features(module).load_features(input.string());
            ensureCamera().prepare();
            break;
          }
          case Operation::GrabOne: {
            abortGrab();
            if (ensureCamera().is_live())
              throw std::runtime_error("Stop Live before Single acquisition.");
            const auto id = request.id;
            auto* target = &ensureCamera();
            const auto session = request.session;
            grab_thread = std::thread([target, id, session] {
              Message reply{Operation::GrabOne, id};
              reply.session = session;
              try {
                onegen::ConnectTiming timing("worker-single");
                const auto frames = target->single(1);
                timing.mark("acquire-and-stop");
                if (frames.empty())
                  throw std::runtime_error("Single acquisition returned no frame.");
                Writer payload;
                onegen::worker::write(payload, frames.front());
                reply.payload = payload.take();
                timing.mark("frame-serialization");
              } catch (...) {
                reply.success = false;
                reply.error = onegen::worker::reportRequestException(
                    "worker single acquisition failed: id=" + std::to_string(id)
                    + " session=" + std::to_string(session));
              }
              try {
                send(std::move(reply));
              } catch (...) {
                onegen::stage_log_warning("worker single response failed: id=%u session=%u detail=%s",
                    id, session, onegen::worker::currentExceptionMessage().c_str());
              }
            });
            continue;
          }
          case Operation::StartLive:
            abortGrab();
            { std::lock_guard lock(ack_mutex); stopping = false; }
            ensureCamera().live();
            break;
          case Operation::StopLive:
            { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
            abortGrab();
            break;
          case Operation::Trigger:
            ensureCamera().trigger();
            break;
          case Operation::Shutdown:
            for (auto& [key, item] : sessions) item->stop();
            shutdown = true;
            break;
          default:
            throw std::runtime_error("Unsupported OneGen worker request.");
        }
        response.payload = output.take();
      } catch (...) {
        response.success = false;
        response.error = onegen::worker::reportRequestException(
            "worker request failed: operation=" + std::to_string(static_cast<unsigned>(request.operation))
            + " id=" + std::to_string(request.id) + " session=" + std::to_string(request.session));
      }
      send(std::move(response));
      if (shutdown) break;
    }
    sessions.clear();
    return 0;
  } catch (...) {
    onegen::stage_log_warning("worker fatal error: cti=%s detail=%s",
        producer.c_str(), onegen::worker::currentExceptionMessage().c_str());
    return 1;
  }
}
