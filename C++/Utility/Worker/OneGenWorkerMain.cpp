#include "OneGenWorkerMessages.h"

#include "OneGen.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

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
    default: return 3;
  }
}

onegen::Camera::Module moduleFromIndex(int index)
{
  if (index == 0) return onegen::Camera::Module::RemoteDevice;
  if (index == 1) return onegen::Camera::Module::DataStream;
  if (index == 2) return onegen::Camera::Module::Device;
  return onegen::Camera::Module::Interface;
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
    onegen::OneGen system;
    system.loadCti(ctiArgument(argc, argv));
    onegen::Camera* camera = nullptr;

    const auto ensureCamera = [&]() -> onegen::Camera& {
      if (!camera) camera = &system.addCamera();
      return *camera;
    };
    const auto wireInvalidates = [&](onegen::Camera& opened) {
      for (const auto module : {onegen::Camera::Module::RemoteDevice,
                                onegen::Camera::Module::DataStream,
                                onegen::Camera::Module::Device,
                                onegen::Camera::Module::Interface}) {
        opened.features(module).set_invalidate_callback([index = moduleIndex(module)](std::string_view name) {
          Writer out;
          out.scalar(index);
          out.string(name);
          send({Operation::Invalidate, 0, true, {}, out.take()});
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
            writeVector(output, system.enumerate(std::chrono::milliseconds{timeout}));
            break;
          }
          case Operation::Open: {
            auto& opened = ensureCamera();
            if (opened.is_open()) opened.close();
            opened.open(input.string());
            opened.prepare();
            wireInvalidates(opened);
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
            opened.set_error_callback([](std::string_view error) {
              send({Operation::Error, 0, false, std::string(error)});
            });
            break;
          }
          case Operation::Close:
            if (camera) { camera->stop(); camera->close(); }
            break;
          case Operation::ListStreams:
            writeVector(output, ensureCamera().data_streams());
            break;
          case Operation::SelectStream:
            ensureCamera().select_data_stream(input.scalar<std::uint32_t>());
            ensureCamera().prepare();
            break;
          case Operation::DescribeFeatures: {
            output.scalar(std::uint32_t{4});
            for (const auto module : {onegen::Camera::Module::RemoteDevice,
                                      onegen::Camera::Module::DataStream,
                                      onegen::Camera::Module::Device,
                                      onegen::Camera::Module::Interface}) {
              output.scalar(moduleIndex(module));
              onegen::worker::write(output, onegen::snapshot(ensureCamera().features(module).root()));
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
          case Operation::GrabOne: {
            const auto frames = ensureCamera().single(1);
            if (frames.empty()) throw std::runtime_error("Single acquisition returned no frame.");
            onegen::worker::write(output, frames.front());
            break;
          }
          case Operation::StartLive:
            { std::lock_guard lock(ack_mutex); stopping = false; }
            ensureCamera().live();
            break;
          case Operation::StopLive:
            { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
            ensureCamera().stop();
            break;
          case Operation::Trigger:
            ensureCamera().trigger();
            break;
          case Operation::Shutdown:
            { std::lock_guard lock(ack_mutex); stopping = true; ack_ready.notify_all(); }
            if (camera) camera->stop();
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
    if (camera) camera->stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
