#pragma once
#include "OneGenWorkerMessages.h"
#include "OneGenParameterMessages.h"
#include "Internal/FrameTrace.h"
#include "Internal/SharedMemoryRegion.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <limits>
#include <cmath>
#include <cstdlib>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

inline int fakeWorker(const std::filesystem::path& cti)
{
  std::ofstream(cti.string() + ".starts", std::ios::app) << "start\n";
  const bool retry_always = cti.stem() == "RetryBoardAlways";
  const bool slow_retry_first = cti.stem() == "SlowRetryBoard"
      && !std::filesystem::exists(cti.string() + ".active");
  if (slow_retry_first) std::ofstream(cti.string() + ".active").put('1');
  if (cti.stem() == "SlowRetryBoard" && !slow_retry_first
      && std::filesystem::exists(cti.string() + ".active")
      && !std::filesystem::exists(cti.string() + ".shutdown")) std::_Exit(71);
  const bool retry_first_worker = cti.stem() == "RetryBoard"
      && !std::filesystem::exists(cti.string() + ".first-worker");
  if (retry_first_worker) std::ofstream(cti.string() + ".first-worker").put('1');
  const bool slow_timeout_first = cti.stem() == "SlowProbeTimeout"
      && !std::filesystem::exists(cti.string() + ".first-worker");
  if (slow_timeout_first) std::ofstream(cti.string() + ".first-worker").put('1');
  const bool inactive = cti.stem() == "Inactive" && !std::filesystem::exists(cti.string() + ".ready");
  if (cti.stem() == "CrashOnce" && !std::filesystem::exists(cti.string() + ".ready"))
    std::_Exit(71);
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
  std::map<std::uint32_t, std::uint64_t> stream_generations;
  struct SharedPool {
    std::shared_ptr<onegen::SharedMemoryRegion> region;
    std::uint64_t epoch = 0;
    std::uint64_t token = 0;
    std::uint32_t slot_count = 0;
    std::uint64_t slot_bytes = 0;
    std::vector<std::uint64_t> generations;
    std::uint64_t next_generation = 1;
    std::uint64_t sequence = 0;
  };
  std::map<std::uint32_t, SharedPool> shared_pools;
  std::map<std::uint32_t, std::map<std::string, onegen::FeatureNode>> parameter_nodes;
  std::map<std::uint32_t, int> parameter_writes;
  std::set<std::pair<std::uint32_t, std::string>> post_write_read_failures;
  const auto parameterNode = [&](std::uint32_t session, const std::string& name) {
    auto& nodes = parameter_nodes[session];
    if (nodes.empty()) {
      for (const auto& [key, kind] : std::vector<std::pair<std::string, onegen::NodeKind>>{
          {"Count", onegen::NodeKind::Integer}, {"Gain", onegen::NodeKind::Float},
          {"Enabled", onegen::NodeKind::Boolean}, {"Label", onegen::NodeKind::String},
          {"Selector", onegen::NodeKind::Enumeration}, {"ParentSelector", onegen::NodeKind::Enumeration},
          {"SelectedCount", onegen::NodeKind::Integer}, {"ReadOnly", onegen::NodeKind::Integer},
          {"SelectorError", onegen::NodeKind::Integer}, {"Cyclic", onegen::NodeKind::Integer},
          {"Increment", onegen::NodeKind::Integer},
          {"NonFinite", onegen::NodeKind::Float},
          {"StreamCount", onegen::NodeKind::Integer}, {"DeviceCount", onegen::NodeKind::Integer},
          {"Prepared", onegen::NodeKind::Integer}, {"PostReadFailure", onegen::NodeKind::Integer},
          {"WorkerRace", onegen::NodeKind::Integer},
          {"Unreadable", onegen::NodeKind::Integer}, {"Writes", onegen::NodeKind::Integer},
          {"FailWrite", onegen::NodeKind::Integer}, {"FailReadback", onegen::NodeKind::Integer},
          {"Unavailable", onegen::NodeKind::Boolean}, {"Raw", onegen::NodeKind::Register},
          {"Reset", onegen::NodeKind::Command}, {"TriggerSoftware", onegen::NodeKind::Command}}) {
        onegen::FeatureNode node; node.name = node.display_name = key; node.kind = kind;
        node.visible = node.readable = node.writable = true;
        node.int_min = std::numeric_limits<std::int64_t>::min();
        node.int_max = std::numeric_limits<std::int64_t>::max(); node.int_inc = 1;
        node.float_min = 0; node.float_max = 10; node.float_inc = .1; node.float_value = 1;
        node.enum_entries = {"A", "B"}; node.enum_value = "A";
        if (key == "ReadOnly" || key == "Writes") node.writable = false;
        if (key == "Unreadable" || kind == onegen::NodeKind::Command) node.readable = false;
        if (key == "Unavailable") node.readable = node.writable = false;
        if (key == "Increment") { node.int_min = 0; node.int_max = 10; node.int_inc = 2; }
        if (key == "NonFinite") node.float_value = std::numeric_limits<double>::quiet_NaN();
        if (key == "Raw") { node.readable = false; node.register_length = 4; }
        nodes.emplace(key, node);
      }
    }
    auto node = nodes[name];
    if (name == "Writes") node.int_value = parameter_writes[session];
    if (name == "SelectedCount") node.selecting_features = {nodes["Selector"]};
    if (name == "Selector") node.selecting_features = {nodes["ParentSelector"]};
    if (name == "SelectorError") node.selecting_features = {nodes["Unreadable"]};
    if (name == "Cyclic") node.selecting_features = {nodes["Cyclic"]};
    return node;
  };
  const auto observeParameter = [&](std::uint32_t session, int module, const std::string& name) {
    NodeObservation result; result.stream_index = streams[session]; result.stream_generation = stream_generations[session] + 1;
    result.node = parameterNode(session, name);
    if (module != 0 && !(module == 1 && name == "StreamCount") && !(module == 2 && name == "DeviceCount")) result.node = {};
    std::map<std::string, onegen::FeatureNode> selectors;
    std::set<std::string> active{name};
    std::function<void(const onegen::FeatureNode&)> collect = [&](const auto& current) {
      for (const auto& relation : current.selecting_features) {
        if (active.count(relation.name)) { result.node.observation_error = "Cyclic selector context"; continue; }
        if (selectors.count(relation.name)) continue;
        const auto selected = parameterNode(session, relation.name);
        active.insert(relation.name); collect(selected); active.erase(relation.name);
        auto copied = selected; copied.selecting_features.clear(); selectors.emplace(relation.name, copied);
      }
    };
    collect(result.node); result.node.selecting_features.clear();
    for (auto& [key, selector] : selectors) result.node.selecting_features.push_back(selector);
    return result;
  };
  struct SendTiming {
    double encode_ms = 0;
    double write_ms = 0;
  };
  const auto transmit = [](Message response, SendTiming* timing = nullptr) {
    const auto encode_started = std::chrono::steady_clock::now();
    const auto body = encode(std::move(response));
    const auto encoded = std::chrono::steady_clock::now();
    const auto header = frameSize(static_cast<std::uint32_t>(body.size()));
    std::cout.write(reinterpret_cast<const char*>(header.data()), header.size());
    std::cout.write(reinterpret_cast<const char*>(body.data()), body.size());
    std::cout.flush();
    if (timing) {
      const auto written = std::chrono::steady_clock::now();
      timing->encode_ms = onegen::frame_trace_ms(encode_started, encoded);
      timing->write_ms = onegen::frame_trace_ms(encoded, written);
    }
  };
  int probes = 0;
  int interface_closes = 0;
  bool close_failure_delivered = false, stop_failure_delivered = false;
  bool interface_value = false;
  std::map<std::uint32_t, unsigned int> frame_counts;
  const auto frame = [&streams, &cti, &frame_counts](std::uint32_t session) {
    onegen::Frame result;
    if (cti.stem() == "LargeFrameBoard") {
      const auto count = ++frame_counts[session];
      result.width = 13376; result.height = 9528; result.stride = 13376;
      result.pixel_format = 0x01080001; result.payload_type = 0;
      result.payload.assign(13376U * 9528U, 0x5a);
      result.buffer_info.image_present = true;
      result.buffer_info.size_filled = result.payload.size();
      result.stream_index = streams[session];
      result.frame_id = count;
      return result;
    }
    result.payload = {1}; result.width = 1; result.height = 1;
    result.pixel_format = 0x01080001; result.stride = 1;
    result.buffer_info.image_present = true; result.stream_index = streams[session];
    if (cti.stem() == "AdapterDropBoard") {
      onegen::Frame::Part part;
      part.data_type = 7; part.data_format = 0x011000b8;
      part.width = part.height = 1; part.stride = 2;
      const auto count = ++frame_counts[session];
      part.payload = (count == 1 || count >= 5)
          ? std::vector<std::uint8_t>{7} : std::vector<std::uint8_t>{7, 0};
      result.parts.push_back(std::move(part));
    }
    return result;
  };
  while (std::cin.read(reinterpret_cast<char*>(header.data()), header.size())) {
    const auto size = frameSize(header);
    if (size > maximum_message_size) return 1;
    std::vector<std::byte> bytes(size);
    if (!std::cin.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return 1;
    const auto request = decode(bytes);
    if (inactive && request.operation != Operation::Shutdown) {
      transmit({Operation::ProducerInactive, request.id, false,
                "Producer inactive: GCInitLib failed (-1001)", {}, request.session});
      continue;
    }
    if (request.operation == Operation::FramePoolAttach) {
      Reader input(request.payload);
      const auto attach = readFramePoolAttach(input);
      std::string error;
      bool valid = !attach.region.empty() && attach.region.size() <= 128
          && attach.slot_count > 0 && attach.slot_count <= 16 && attach.slot_bytes > 0
          && attach.epoch && attach.token
          && attach.slot_bytes <= (std::numeric_limits<std::uint64_t>::max)() / attach.slot_count
          && attach.region_bytes == attach.slot_count * attach.slot_bytes;
      if (valid) {
        auto region = onegen::SharedMemoryRegion::open(attach.region,
            static_cast<std::size_t>(attach.region_bytes), error);
        if (!region) valid = false;
        else {
          SharedPool pool;
          pool.region = std::move(region);
          pool.epoch = attach.epoch;
          pool.token = attach.token;
          pool.slot_count = attach.slot_count;
          pool.slot_bytes = attach.slot_bytes;
          pool.generations.assign(attach.slot_count, 0);
          shared_pools[request.session] = std::move(pool);
        }
      }
      transmit({request.operation, request.id, valid,
                valid ? std::string{} : (error.empty() ? "Invalid frame pool attach." : error),
                {}, request.session});
      continue;
    }
    if (request.operation == Operation::FramePoolDetach) {
      if (cti.stem() == "SharedFenceBoard")
        std::ofstream(cti.string() + ".detached").put('1');
      shared_pools.erase(request.session);
      transmit({request.operation, request.id, true, {}, {}, request.session});
      continue;
    }
    if (request.operation == Operation::SlotRelease) {
      Reader input(request.payload);
      const auto release = readSlotRelease(input);
      if (const auto found = shared_pools.find(request.session); found != shared_pools.end()) {
        auto& pool = found->second;
        if (release.epoch == pool.epoch && release.token == pool.token
            && release.slot < pool.slot_count && release.generation
            && pool.generations[release.slot] == release.generation)
          pool.generations[release.slot] = 0;
      }
      continue;
    }
    if (request.operation == Operation::Close && cti.stem() == "ApiCloseFailBoard" && !close_failure_delivered) {
      close_failure_delivered = true;
      transmit({request.operation, request.id, false, "Close: test refusal", {}, request.session});
      continue;
    }
    if (request.operation == Operation::StopLive && cti.stem() == "ApiStopFailBoard" && live.count(request.session) && !stop_failure_delivered) {
      stop_failure_delivered = true;
      transmit({request.operation, request.id, false, "StopLive: test refusal", {}, request.session});
      continue;
    }
    Writer payload;
    if (request.operation == Operation::ObserveNode || request.operation == Operation::WriteNodeChecked) {
      Reader input(request.payload); const auto module = input.scalar<std::int32_t>(); const auto name = input.string();
      auto current = observeParameter(request.session, module, name);
      if (request.operation == Operation::ObserveNode) write(payload, current);
      else {
        const auto operation = input.string(); const auto value = input.string();
        const auto expected = readNodeObservation(input); const auto compareValue = input.scalar<bool>();
        if (name == "WorkerRace") { ++parameter_nodes[request.session][name].int_inc; current = observeParameter(request.session, module, name); }
        NodeWriteResult result;
        if ((module == 1 && (current.stream_index != expected.stream_index || current.stream_generation != expected.stream_generation))
            || !onegen::same_parameter_context(current.node, expected.node, compareValue)) {
          result.status = NodeWriteStatus::Conflict;
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds{35}); ++parameter_writes[request.session];
          auto& node = parameter_nodes[request.session][name];
          if (operation == "int") node.int_value = std::stoll(value);
          else if (operation == "float") node.float_value = std::floor(std::stod(value) * 10) / 10;
          else if (operation == "bool") node.bool_value = value == "1";
          else if (operation == "string") node.string_value = value;
          else if (operation == "enum") node.enum_value = value;
          if (name == "Prepared") ++node.int_value;
          if (name == "TriggerSoftware" && operation == "execute" && live.count(request.session)) {
            Writer framePayload; write(framePayload, frame(request.session));
            transmit({Operation::Frame, 0, true, {}, framePayload.take(), request.session});
          }
          result.effect = NodeWriteEffect::Applied;
          if (name == "FailWrite") { result.status = NodeWriteStatus::BackendFailure; result.effect = NodeWriteEffect::Unknown; }
          else if (name == "PostReadFailure") result.status = NodeWriteStatus::BackendFailure;
          else {
            result.observation = observeParameter(request.session, module, name);
            if (name == "FailReadback") { result.status = NodeWriteStatus::BackendFailure; result.observation->node.observation_error = "Readback failure"; }
          }
        }
        write(payload, result);
      }
      transmit({request.operation, request.id, true, {}, payload.take(), request.session}); continue;
    }
    if (cti.stem() == "ApiBoard" && (request.operation == Operation::NodeRead || request.operation == Operation::NodeWrite)) {
      Reader input(request.payload); const auto module = input.scalar<std::int32_t>();
      const auto name = input.string();
      if (request.operation == Operation::NodeRead && post_write_read_failures.erase({request.session, name})) {
        transmit({request.operation, request.id, false, "Observation failed after setter completion", {}, request.session}); continue;
      }
      auto node = parameterNode(request.session, name);
      if (module != 0 && !(module == 1 && name == "StreamCount") && !(module == 2 && name == "DeviceCount")) node = {};
      if (request.operation == Operation::NodeWrite) {
        const auto operation = input.string(); const auto value = input.string();
        std::this_thread::sleep_for(std::chrono::milliseconds{35});
        ++parameter_writes[request.session];
        if (operation == "int") node.int_value = std::stoll(value);
        else if (operation == "float") node.float_value = std::floor(std::stod(value) * 10) / 10;
        else if (operation == "bool") node.bool_value = value == "1";
        else if (operation == "string") node.string_value = value;
        else if (operation == "enum") node.enum_value = value;
        parameter_nodes[request.session][name] = node;
        if (name == "Prepared") ++parameter_nodes[request.session][name].int_value;
        if (name == "PostReadFailure") post_write_read_failures.emplace(request.session, name);
        if (name == "FailWrite") {
          transmit({request.operation, request.id, false, "Failure after mutation", {}, request.session}); continue;
        }
        if (name == "FailReadback") node.observation_error = "Readback failed after mutation";
      }
      write(payload, node);
      transmit({request.operation, request.id, true, {}, payload.take(), request.session}); continue;
    }
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
      if (slow_retry_first) std::ofstream(cti.string() + ".open-failed").put('1');
      if (retry_first_worker || retry_always || slow_retry_first) {
        transmit({request.operation, request.id, false, "TLOpenInterface: test resource busy", {}, request.session});
        continue;
      }
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
      ++stream_generations[request.session];
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
          if (cti.stem() == "ApiBoard") {
            parameterNode(request.session, "Count"); root.children.clear();
            for (const auto& [name, node] : parameter_nodes[request.session])
              if (name != "StreamCount" && name != "DeviceCount") root.children.push_back(parameterNode(request.session, name));
          }
        }
        if ((cti.stem() == "ApiBoard" || cti.stem() == "MultiStreamBoard") && (module == 1 || module == 2))
          root.children.push_back(parameterNode(request.session, module == 1 ? "StreamCount" : "DeviceCount"));
        write(payload, root);
      }
    }
    if (request.operation == Operation::ListInterfaces) {
      payload.scalar(std::uint32_t{1});
      write(payload, onegen::InterfaceInfo{"board", cti.string(), "Test board", "TEST", "TEST"});
    }
    if (request.operation == Operation::InterfaceFeatures || request.operation == Operation::InterfaceWrite) {
      if (request.operation == Operation::InterfaceFeatures && slow_retry_first) {
        std::ofstream(cti.string() + ".interface-started").put('1');
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
      }
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
      if (cti.stem() == "ApiBoard") write(payload, frame(request.session));
      else {
        pending_single[request.session] = request;
        continue; // Explicit triggering or cancellation completes Single.
      }
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
      const bool shared_stem = cti.stem() == "SharedPoolBoard"
          || cti.stem() == "SharedPoolBudgetBoard" || cti.stem() == "SharedFenceBoard"
          || cti.stem() == "FormatStorageBoard"
          || cti.stem() == "MalformedPoolBoard" || cti.stem() == "MalformedFormatBoard"
          || cti.stem() == "CrashPoolBoard";
      if (shared_stem) {
        const auto found = shared_pools.find(request.session);
        if (found == shared_pools.end()) {
          transmit({request.operation, request.id, false, "Shared frame pool is not attached.",
                    {}, request.session});
          continue;
        }
        auto& pool = found->second;
        if (cti.stem() == "CrashPoolBoard" && pool.sequence >= 1) std::_Exit(71);
        std::uint32_t slot = pool.slot_count;
        for (std::uint32_t index = 0; index < pool.slot_count; ++index)
          if (!pool.generations[index]) { slot = index; break; }
        if (slot == pool.slot_count) {
          transmit({request.operation, request.id, false,
                    "Shared frame pool has no free slot.", {}, request.session});
          continue;
        }
        auto produced = frame(request.session);
        if (produced.payload.size() > pool.slot_bytes) {
          transmit({request.operation, request.id, false,
                    "Frame exceeds the shared slot size.", {}, request.session});
          continue;
        }
        const auto generation = pool.next_generation++;
        pool.generations[slot] = generation;
        const auto offset = static_cast<std::size_t>(slot)
            * static_cast<std::size_t>(pool.slot_bytes);
        if (!produced.payload.empty())
          std::memcpy(pool.region->writable() + offset, produced.payload.data(),
                      produced.payload.size());
        FrameDescriptor descriptor;
        descriptor.session = request.session;
        descriptor.epoch = pool.epoch;
        descriptor.token = pool.token;
        descriptor.slot = slot;
        descriptor.generation = generation;
        descriptor.frame_id = produced.frame_id;
        descriptor.sequence = ++pool.sequence;
        descriptor.used_bytes = produced.payload.size();
        descriptor.width = produced.width;
        descriptor.height = produced.height;
        descriptor.stride = produced.stride;
        descriptor.pixel_format = produced.pixel_format;
        descriptor.format_namespace = produced.format_namespace;
        descriptor.pixel_endianness = produced.pixel_endianness;
        descriptor.payload_type = produced.payload_type;
        descriptor.stream_index = produced.stream_index;
        descriptor.stream_id = produced.stream_id;
        if (cti.stem() == "MalformedPoolBoard" && descriptor.sequence == 1)
          descriptor.used_bytes = pool.slot_bytes + 1;
        if (cti.stem() == "MalformedFormatBoard") descriptor.payload_type = 1; // GenTL IMAGE
        if (cti.stem() == "MalformedFormatBoard" && descriptor.sequence == 1) {
          descriptor.width = 1000;
          descriptor.height = 2;
          descriptor.stride = 200;
          descriptor.used_bytes = 400;
        }
        if (cti.stem() == "MalformedFormatBoard" && descriptor.sequence == 2)
          descriptor.pixel_format = 0x010800ff;
        if (cti.stem() == "FormatStorageBoard") {
          descriptor.payload_type = 1; // GenTL IMAGE
          descriptor.width = 1000;
          descriptor.height = 2;
          switch (descriptor.sequence) {
            case 1: // BiColorRGBG8 (16-bit storage), full rows.
              descriptor.pixel_format = 0x021000a5;
              descriptor.stride = 2000;
              descriptor.used_bytes = 4000;
              break;
            case 2: // BiColorRGBG8 short row.
              descriptor.pixel_format = 0x021000a5;
              descriptor.stride = 1000;
              descriptor.used_bytes = 2000;
              break;
            case 3: // RGB10 unpacked (48-bit storage), full rows.
              descriptor.pixel_format = 0x02300018;
              descriptor.stride = 6000;
              descriptor.used_bytes = 12000;
              break;
            case 4: // RGB10 unpacked short row.
              descriptor.pixel_format = 0x02300018;
              descriptor.stride = 4000;
              descriptor.used_bytes = 8000;
              break;
            case 5: // BiColorRGBG10p (20-bit storage), full rows.
              descriptor.pixel_format = 0x021400a8;
              descriptor.stride = 2500;
              descriptor.used_bytes = 5000;
              break;
            case 6: // BiColorRGBG10p short row.
              descriptor.pixel_format = 0x021400a8;
              descriptor.stride = 1500;
              descriptor.used_bytes = 3000;
              break;
            case 7: // BiColorRGBG8 short payload (one row missing).
              descriptor.pixel_format = 0x021000a5;
              descriptor.stride = 2000;
              descriptor.used_bytes = 2000;
              break;
            case 8: // Marker frame after all format cases.
              descriptor.pixel_format = 0x01080001;
              descriptor.width = 1;
              descriptor.height = 1;
              descriptor.stride = 1;
              descriptor.used_bytes = 1;
              break;
            default:
              break;
          }
        }
        Writer descriptor_payload;
        write(descriptor_payload, descriptor);
        transmit({Operation::FrameReady, 0, true, {}, descriptor_payload.take(), request.session});
      } else {
        const bool trace = cti.stem() == "LargeFrameBoard" && onegen::frame_trace_enabled();
        const auto started = trace ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
        auto produced = frame(request.session);
        const auto produced_at = trace ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{};
        const auto frame_id = produced.frame_id;
        const auto bytes = produced.payload.size();
        Writer frame_payload; write(frame_payload, produced);
        const auto serialized = trace ? std::chrono::steady_clock::now()
                                      : std::chrono::steady_clock::time_point{};
        SendTiming send;
        transmit({Operation::Frame, 0, true, {}, frame_payload.take(), request.session},
            trace ? &send : nullptr);
        if (trace) {
          const auto written = std::chrono::steady_clock::now();
          onegen::stage_log_debug(
              "frame transport: side=fake-worker session=%u frame=%llu bytes=%zu produce_ms=%.3f "
              "serialize_ms=%.3f encode_ms=%.3f write_ms=%.3f",
              request.session, static_cast<unsigned long long>(frame_id), bytes,
              onegen::frame_trace_ms(started, produced_at),
              onegen::frame_trace_ms(produced_at, serialized), send.encode_ms,
              onegen::frame_trace_ms(serialized, written));
        }
      }
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
      if (cti.stem() == "FatalDiagnostics") {
        std::cerr << "[GenICam] [ERROR] fixture fatal detail" << std::flush;
        return 71;
      }
      if (cti.stem() == "MalformedDiagnostics") {
        const auto invalid = frameSize(maximum_message_size + 1);
        std::cout.write(reinterpret_cast<const char*>(invalid.data()), invalid.size());
        std::cout.flush();
        return 71;
      }
      if (cti.stem() == "SlowProbe" || slow_timeout_first) {
        std::ofstream(cti.string() + ".probe-started").put('1');
        std::this_thread::sleep_for(cti.stem() == "SlowProbeTimeout"
            ? std::chrono::seconds{2} : std::chrono::milliseconds{300});
      }
      if (cti.stem() == "InvalidateDiagnostics") {
        Writer notification; notification.scalar(std::int32_t{0}); notification.string("Gain");
        transmit({Operation::Invalidate, 0, true, {}, notification.take(), request.session});
      }
      if (cti.stem() == "FrameDiagnostics") {
        Writer notification; write(notification, frame(request.session));
        transmit({Operation::Frame, 0, true, {}, notification.take(), request.session});
      }
      std::ofstream(cti.string() + ".all-probes", std::ios::app) << "probe\n";
      if (cti.stem() == "CrashProbe") return 53;
      ++probes;
      const auto diagnostic = encode({Operation::Error, 0, false, "Interface XML test failure"});
      const auto diagnostic_header = frameSize(static_cast<std::uint32_t>(diagnostic.size()));
      std::cout.write(reinterpret_cast<const char*>(diagnostic_header.data()), diagnostic_header.size());
      std::cout.write(reinterpret_cast<const char*>(diagnostic.data()), diagnostic.size());
      std::cout.flush();
      const bool slow = cti.parent_path().filename() == "vendor-a";
      if (cti.stem() == "Heartbeat") std::this_thread::sleep_for(std::chrono::seconds{6});
      if (slow) std::this_thread::sleep_for(std::chrono::seconds{1});
      if (cti.stem() == "EmptyBoard" || cti.stem() == "ProbeFail"
          || (cti.stem() == "Retry" && probes < 2)) {
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
      const bool failed = cti.stem() == "ProbeFail" || cti.stem() == "PartialProbe"
          || (cti.stem() == "SessionProbeFailure"
              && std::any_of(opened.begin(), opened.end(), [](const auto& item) { return !item.second.empty(); }));
      writeStrings(payload, failed ? std::vector<std::string>{"interface=busy-board: TLOpenInterface: test failure"}
                                   : std::vector<std::string>{});
      if (failed) std::ofstream(cti.string() + ".probes", std::ios::app) << "probe\n";
    }
    transmit({request.operation, request.id, true, {}, payload.take(), request.session});
    if (request.operation == Operation::Shutdown) {
      std::ofstream(cti.string() + ".shutdown").put('1');
      return 0;
    }
  }
  return 0;
}
