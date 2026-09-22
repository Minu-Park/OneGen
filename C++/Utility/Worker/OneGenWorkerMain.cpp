#include "OneGenWorkerMessages.h"

#include "OneGen.h"
#include "Internal/ConnectTiming.h"
#include "Internal/StageLog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
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
std::mutex ack_mutex;
std::condition_variable ack_ready;
bool frame_acked = false;
bool stopping = false;
std::FILE* protocol_output = nullptr;

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
  if (!protocol_output) return 1;
  try {
    const auto cti = ctiArgument(argc, argv);
    onegen::OneGen system;
    system.loadCti(cti);
    onegen::Camera* camera = nullptr;
    std::thread grab_thread;

    const auto joinGrab = [&] {
      if (grab_thread.joinable()) grab_thread.join();
    };
    const auto abortGrab = [&] {
      if (camera) camera->stop();
      joinGrab();
    };

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
        opened.features(module).set_invalidate_callback([index = moduleIndex(module)](std::string_view name) {
          // Runs inside a GenApi/producer invalidate dispatch that must not see
          // an exception. Drop the notification if the host pipe write fails.
          try {
            Writer out;
            out.scalar(index);
            out.string(name);
            send({Operation::Invalidate, 0, true, {}, out.take()});
          } catch (...) {
          }
        });
      }
    };

    Message request;
    while (readMessage(request)) {
      if (request.operation == Operation::FrameAck) {
        std::lock_guard lock(ack_mutex);
        frame_acked = true;
        ack_ready.notify_all();
        continue;
      }
      Message response{request.operation, request.id};
      bool shutdown = false;
      try {
        Reader input(request.payload);
        Writer output;
        switch (request.operation) {
          case Operation::Probe: {
            const auto timeout = std::clamp(input.scalar<std::uint32_t>(), 1U, 60000U);
            auto devices = system.enumerate(std::chrono::milliseconds{timeout});
            writeVector(output, devices);
            break;
          }
          case Operation::Open: {
            abortGrab();
            onegen::ConnectTiming timing("worker-open");
            auto& opened = ensureCamera();
            if (opened.is_open()) opened.close();
            opened.set_error_callback([](std::string_view error) {
              send({Operation::Error, 0, false, std::string(error)});
            });
            opened.open(input.string());
            timing.mark("camera-open");
            opened.prepare();
            timing.mark("camera-prepare");
            wireInvalidates(opened);
            try { opened.poll_features(0); }
            catch (const std::exception& error) {
              send({Operation::Error, 0, false, std::string("Feature event setup failed: ") + error.what()});
            }
            opened.set_frame_callback([](const onegen::Frame& frame) {
              Writer out;
              onegen::worker::write(out, frame);
              {
                std::lock_guard lock(ack_mutex);
                frame_acked = false;
              }
              send({Operation::Frame, 0, true, {}, out.take()});
              std::unique_lock lock(ack_mutex);
              ack_ready.wait(lock, [] { return frame_acked || stopping; });
            });
            timing.mark("callbacks-and-event-setup");
            break;
          }
          case Operation::Close:
            abortGrab();
            if (camera) camera->close();
            break;
          case Operation::ListStreams:
            writeVector(output, ensureCamera().data_streams());
            break;
          case Operation::SelectStream:
            ensureCamera().select_data_stream(input.scalar<std::uint32_t>());
            ensureCamera().prepare();
            break;
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
            auto node = opened.features(module).find(name);
            onegen::apply_feature(node, operation, value);
            auto changed = onegen::snapshot(node);
            if (module == onegen::Camera::Module::RemoteDevice
                && operation != "execute" && !opened.is_live())
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
            const auto name = input.string();
            const auto bytes = onegen::worker::readBytes(input);
            if (ensureCamera().is_live()) throw std::runtime_error("Stop Live before raw register writes.");
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
            if (ensureCamera().is_live()) throw std::runtime_error("Stop Live before restoring features.");
            ensureCamera().features(module).load_features(input.string());
            ensureCamera().prepare();
            break;
          }
          case Operation::GrabOne: {
            abortGrab();
            if (ensureCamera().is_live())
              throw std::runtime_error("Stop Live before Single acquisition.");
            const auto id = request.id;
            grab_thread = std::thread([&, id] {
              Message reply{Operation::GrabOne, id};
              try {
                onegen::ConnectTiming timing("worker-single");
                const auto frames = ensureCamera().single(1);
                timing.mark("acquire-and-stop");
                if (frames.empty())
                  throw std::runtime_error("Single acquisition returned no frame.");
                Writer payload;
                onegen::worker::write(payload, frames.front());
                reply.payload = payload.take();
                timing.mark("frame-serialization");
              } catch (const std::exception& error) {
                reply.success = false;
                reply.error = error.what();
              }
              try {
                send(std::move(reply));
              } catch (...) {
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
            { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
            abortGrab();
            shutdown = true;
            break;
          default:
            throw std::runtime_error("Unsupported OneGen worker request.");
        }
        response.payload = output.take();
      } catch (const std::exception& error) {
        response.success = false;
        response.error = error.what();
      }
      send(std::move(response));
      if (shutdown) break;
    }
    { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
    abortGrab();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
