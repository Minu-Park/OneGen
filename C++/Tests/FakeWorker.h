#pragma once
#include "OneGenWorkerMessages.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

inline int fakeWorker(const std::filesystem::path& cti)
{
  std::ofstream(cti.string() + ".starts", std::ios::app) << "start\n";
  using namespace onegen::worker;
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  std::array<std::byte, 4> header;
  std::map<std::uint32_t, std::string> opened;
  std::set<std::uint32_t> live;
  std::map<std::uint32_t, Message> pending_single;
  std::map<std::uint32_t, std::uint32_t> streams;
  const auto transmit = [](Message response) {
    const auto body = encode(std::move(response));
    const auto header = frameSize(static_cast<std::uint32_t>(body.size()));
    std::cout.write(reinterpret_cast<const char*>(header.data()), header.size());
    std::cout.write(reinterpret_cast<const char*>(body.data()), body.size());
    std::cout.flush();
  };
  int probes = 0;
  int interface_closes = 0;
  bool interface_value = false;
  const auto frame = [&streams](std::uint32_t session) {
    onegen::Frame result;
    result.payload = {1}; result.width = 1; result.height = 1;
    result.pixel_format = 0x01080001; result.stride = 1;
    result.buffer_info.image_present = true; result.stream_index = streams[session];
    return result;
  };
  while (std::cin.read(reinterpret_cast<char*>(header.data()), header.size())) {
    const auto size = frameSize(header);
    if (size > maximum_message_size) return 1;
    std::vector<std::byte> bytes(size);
    if (!std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return 1;
    const auto request = decode(bytes);
    Writer payload;
    auto& opened_device = opened[request.session];
    if (request.operation == Operation::NodeRead) {
      Reader input(request.payload);
      (void)input.scalar<std::int32_t>();
      if (input.string() == "ExitWorker") return 0;
    }
    if (request.operation == Operation::CloseInterface && cti.stem() == "CloseFailBoard"
        && ++interface_closes == 1) {
      transmit({request.operation, request.id, false, "IFClose: test native failure", {}, request.session});
      continue;
    }
    if (request.operation == Operation::OpenInterface && cti.stem() == "FailBoard") {
      transmit({request.operation, request.id, false, "TLOpenInterface: test native failure", {}, request.session});
      continue;
    }
    if (request.operation == Operation::Open) {
      Reader input(request.payload);
      opened_device = input.string();
      if (input.string() != "board") {
        transmit({request.operation, request.id, false, "Wrong producer-specific InterfaceID", {}, request.session});
        continue;
      }
    } else if (request.operation == Operation::Close) opened_device.clear();
    else if (request.operation == Operation::ListStreams) {
      const auto count = cti.stem() == "MultiStreamBoard" ? 2U : 1U;
      payload.scalar(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        payload.scalar(index);
        payload.string(opened_device + (count == 1 ? "" : std::string(300, 'W') + std::to_string(index)));
      }
    }
    if (request.operation == Operation::SelectStream) {
      std::this_thread::sleep_for(std::chrono::milliseconds{150});
      Reader input(request.payload);
      streams[request.session] = input.scalar<std::uint32_t>();
    }
    if (request.operation == Operation::DescribeFeatures) {
      payload.scalar(std::uint32_t{5});
      for (int module = 0; module < 5; ++module) {
        payload.scalar(module);
        onegen::FeatureNode root; root.kind = onegen::NodeKind::Category; root.name = "Root";
        if (module == 0) {
          onegen::FeatureNode trigger;
          trigger.kind = onegen::NodeKind::Command;
          trigger.name = trigger.display_name = "TriggerSoftware";
          trigger.visible = trigger.writable = true;
          root.children.push_back(trigger);
        }
        write(payload, root);
      }
    }
    if (request.operation == Operation::ListInterfaces) {
      payload.scalar(std::uint32_t{1});
      write(payload, onegen::InterfaceInfo{"board", cti.string(), "Test board", "TEST", "TEST"});
    }
    if (request.operation == Operation::InterfaceFeatures || request.operation == Operation::InterfaceWrite) {
      if (request.operation == Operation::InterfaceWrite) {
        Reader input(request.payload);
        (void)input.string(); (void)input.string(); (void)input.string();
        interface_value = input.string() == "1";
      }
      onegen::FeatureNode root;
      root.kind = onegen::NodeKind::Category;
      root.name = "Root";
      onegen::FeatureNode category;
      category.kind = onegen::NodeKind::Category;
      category.name = category.display_name = "Settings";
      onegen::FeatureNode value;
      value.kind = onegen::NodeKind::Boolean;
      value.name = value.display_name = "Enabled";
      value.visible = value.readable = value.writable = true;
      value.bool_value = interface_value;
      category.children.push_back(value);
      root.children.push_back(category);
      write(payload, root);
    }
    if (request.operation == Operation::StartLive) live.insert(request.session);
    if (request.operation == Operation::GrabOne) {
      pending_single[request.session] = request;
      continue; // Explicit triggering or cancellation completes Single.
    }
    if (request.operation == Operation::StopLive || request.operation == Operation::Close) {
      if (const auto found = pending_single.find(request.session); found != pending_single.end()) {
        transmit({Operation::GrabOne, found->second.id, false, "Single cancelled", {}, request.session});
        pending_single.erase(found);
      }
    }
    if (request.operation == Operation::StopLive || request.operation == Operation::Close) live.erase(request.session);
    if (request.operation == Operation::Trigger) {
      if (const auto found = pending_single.find(request.session); found != pending_single.end()) {
        Writer reply; write(reply, frame(request.session));
        transmit({Operation::GrabOne, found->second.id, true, {}, reply.take(), request.session});
        pending_single.erase(found);
      }
    }
    if (request.operation == Operation::Trigger && live.count(request.session)) {
      Writer frame_payload; write(frame_payload, frame(request.session));
      transmit({Operation::Frame, 0, true, {}, frame_payload.take(), request.session});
    }
    if (request.operation == Operation::ProbeInterface) {
      const bool empty = cti.stem() == "EmptyBoard";
      payload.scalar(std::uint32_t{empty ? 0U : 2U});
      for (const auto id : {"channel-a", "channel-b"}) if (!empty) {
        onegen::DeviceInfo camera;
        camera.id = id; camera.serial = id; camera.tl_type = "TEST";
        camera.cti_path = cti.string(); camera.interface_id = "board";
        write(payload, camera);
      }
    }
    if (request.operation == Operation::Probe) {
      ++probes;
      const auto diagnostic = encode({Operation::Error, 0, false, "Interface XML test failure"});
      const auto diagnostic_header = frameSize(static_cast<std::uint32_t>(diagnostic.size()));
      std::cout.write(reinterpret_cast<const char*>(diagnostic_header.data()), diagnostic_header.size());
      std::cout.write(reinterpret_cast<const char*>(diagnostic.data()), diagnostic.size());
      std::cout.flush();
      const bool slow = cti.parent_path().filename() == "vendor-a";
      if (cti.stem() == "Heartbeat") std::this_thread::sleep_for(std::chrono::seconds{6});
      if (slow) std::this_thread::sleep_for(std::chrono::seconds{1});
      if (cti.stem() == "EmptyBoard" || (cti.stem() == "Retry" && probes < 2)) {
        payload.scalar(std::uint32_t{0});
      } else {
        onegen::DeviceInfo device;
        device.id = slow ? "slow" : "fast";
        device.serial = device.id;
        device.tl_type = "TEST";
        device.cti_path = cti.string();
        device.interface_id = "board";
        payload.scalar(std::uint32_t{1});
        write(payload, device);
      }
    }
    transmit({request.operation, request.id, true, {}, payload.take(), request.session});
    if (request.operation == Operation::Shutdown) return 0;
  }
  return 0;
}
