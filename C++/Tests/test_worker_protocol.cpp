#include "OneGenWorkerMessages.h"
#include "OneGenParameterMessages.h"
#include "WorkerCaptureMark.h"
#include "../Internal/FramePayload.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

void checkCaptureMarks()
{
  using onegen::worker::CaptureBoard;
  using onegen::worker::CaptureMark;
  using onegen::worker::CaptureSequence;

  // The sequence follows GenTL frame ids, unwraps a 16-bit wrap and rejects repeats.
  CaptureSequence sequence;
  assert(sequence.observe(5).value() == 5);
  assert(sequence.observe(9).value() == 9);
  assert(!sequence.observe(9).has_value());
  assert(sequence.current() == 9);
  assert(sequence.observe(65535).value() == 65535);
  assert(sequence.observe(1).value() == 65536);
  sequence.reset();
  assert(sequence.observe(1).value() == 1);

  CaptureMark mark;
  mark.valid = true;
  mark.generation = 3;
  mark.frame_id = 12;
  mark.sequence = 65548;
  mark.trigger_id = 7;
  mark.dropped = 2;
  onegen::worker::Writer out;
  onegen::worker::write(out, mark);
  const auto bytes = out.take();
  onegen::worker::Reader in(bytes);
  const auto decoded = onegen::worker::readCaptureMark(in);
  assert(decoded.valid && decoded.generation == 3 && decoded.frame_id == 12);
  assert(decoded.sequence == 65548 && decoded.trigger_id == 7 && decoded.dropped == 2);

  // A frame is stamped only by the mark of its own buffer.
  CaptureBoard board;
  assert(!board.stamp(1, 12).valid);
  board.update(1, mark);
  const auto stamped = board.stamp(1, 12);
  assert(stamped.valid && stamped.generation == 3 && stamped.sequence == 65548);
  assert(stamped.trigger_id == 7);
  assert(!board.stamp(1, 13).valid);
  assert(!board.stamp(2, 12).valid);
  assert(board.latest(1)->dropped == 2);
  board.clear(1);
  assert(!board.latest(1).has_value());
}

}  // namespace

int main()
{
  checkCaptureMarks();
  onegen::worker::Message message{onegen::worker::Operation::Frame};
  message.session = 42;
  assert(onegen::worker::decode(onegen::worker::encode(message)).session == 42);
  auto old_protocol = onegen::worker::encode(message);
  old_protocol[0] = std::byte(onegen::worker::protocol_version - 1);
  bool old_protocol_rejected = false;
  try { onegen::worker::decode(old_protocol); }
  catch (const std::runtime_error&) { old_protocol_rejected = true; }
  assert(old_protocol_rejected);
  onegen::worker::NodeWriteResult outcome;
  outcome.effect = onegen::worker::NodeWriteEffect::Applied;
  outcome.observation = onegen::worker::NodeObservation{};
  outcome.observation->stream_index = 7; outcome.observation->stream_generation = UINT64_MAX;
  outcome.observation->node.name = "Width"; outcome.observation->node.int_value = INT64_MIN;
  onegen::worker::Writer encodedOutcome; onegen::worker::write(encodedOutcome, outcome);
  const auto outcomeBytes = encodedOutcome.take(); onegen::worker::Reader outcomeReader(outcomeBytes);
  const auto decodedOutcome = onegen::worker::readNodeWriteResult(outcomeReader);
  assert(decodedOutcome.effect == outcome.effect && decodedOutcome.observation->stream_generation == UINT64_MAX);
  assert(decodedOutcome.observation->node.int_value == INT64_MIN);
  onegen::InterfaceInfo iface{"id", "producer", "display", "vendor", "CXP"};
  onegen::worker::Writer interface_writer;
  onegen::worker::write(interface_writer, iface);
  auto interface_bytes = interface_writer.take();
  onegen::worker::Reader interface_reader(interface_bytes);
  const auto iface_copy = onegen::worker::readInterfaceInfo(interface_reader);
  assert(iface_copy.id == iface.id && iface_copy.vendor == iface.vendor && iface_copy.tl_type == iface.tl_type);
  assert(interface_reader.empty());
  onegen::DeviceInfo device;
  device.id = "preferred"; device.interface_id = "board";
  device.cti_candidates = {"a", "b"}; device.cti_candidate_ids = {"preferred", "alternate"};
  device.cti_candidate_interfaces = {"board", "alternate-board"};
  onegen::worker::Writer device_writer; onegen::worker::write(device_writer, device);
  auto device_bytes = device_writer.take();
  onegen::worker::Reader device_reader(device_bytes);
  const auto device_copy = onegen::worker::readDeviceInfo(device_reader);
  assert(device_copy.interface_id == "board" && device_copy.cti_candidate_ids[1] == "alternate");
  assert(device_copy.cti_candidate_interfaces[1] == "alternate-board");
  assert(device_reader.empty());
  onegen::FeatureNode feature;
  feature.kind = onegen::NodeKind::Register;
  feature.unit = "mm";
  feature.representation = "Linear";
  feature.float_inc = 0.0000001;
  feature.register_length = 4;
  onegen::FeatureNode selector;
  selector.name = "Selector"; selector.kind = onegen::NodeKind::Integer;
  selector.readable = true; selector.int_value = INT64_MAX; selector.int_min = INT64_MIN;
  feature.selecting_features.push_back(selector);
  feature.observation_error = "Constraint read failed";
  feature.children.push_back(feature);
  onegen::worker::Writer feature_writer;
  onegen::worker::write(feature_writer, feature);
  const auto feature_bytes = feature_writer.take();
  onegen::worker::Reader feature_reader(feature_bytes);
  const auto feature_copy = onegen::worker::readFeatureNode(feature_reader);
  assert(feature_reader.empty());
  assert(feature_copy.unit == feature.unit && feature_copy.float_inc == feature.float_inc);
  assert(feature_copy.register_length == 4 && feature_copy.children.size() == 1);
  assert(feature_copy.selecting_features.size() == 1);
  assert(feature_copy.selecting_features[0].int_value == INT64_MAX);
  assert(feature_copy.selecting_features[0].int_min == INT64_MIN);
  assert(feature_copy.observation_error == feature.observation_error);
  assert(feature_copy.children[0].selecting_features[0].name == "Selector");
  onegen::Frame source;
  source.components = {{1, "Intensity"}, {65281, "Phase"}};
  source.payload = {1, 2, 3, 4};
  source.width = 2;
  source.height = 2;
  source.pixel_format = 0x01080001;
  source.buffer_info.size_filled = source.payload.size();
  source.chunks.push_back({17, 8, {9, 10, 11}});
  source.scan3d_geometry.emplace();
  source.scan3d_geometry->scale = {0.1, 0.2, 0.3};
  source.scan3d_geometry->invalid_enabled = {true, false, true};
  source.scan3d_geometry->output_mode = "RectifiedC";
  source.scan3d_geometry->coordinate_system = "Cartesian";
  source.scan3d_geometry->calibrated_axes = {false, true, true};

  onegen::worker::Writer writer;
  onegen::worker::write(writer, source);
  const auto bytes = writer.take();
  onegen::worker::Reader reader(bytes);
  const auto copy = onegen::worker::readFrame(reader);
  assert(copy.components.size() == 2 && copy.components[0].id == 1);
  assert(copy.components[1].id == 65281 && copy.components[1].name == "Phase");

  assert(reader.empty());
  assert(copy.payload == source.payload);
  assert(copy.chunks.size() == 1 && copy.chunks[0].id == 17 && copy.chunks[0].offset == 8);
  assert(copy.chunks[0].payload == source.chunks[0].payload);
  assert(copy.width == source.width);
  assert(copy.height == source.height);
  assert(copy.pixel_format == source.pixel_format);
  assert(copy.buffer_info.size_filled == source.buffer_info.size_filled);
  assert(copy.scan3d_geometry);
  assert(copy.scan3d_geometry->scale == source.scan3d_geometry->scale);
  assert(copy.scan3d_geometry->output_mode == "RectifiedC");
  assert(copy.scan3d_geometry->coordinate_system == "Cartesian");
  assert(copy.scan3d_geometry->calibrated_axes == source.scan3d_geometry->calibrated_axes);
  assert(copy.scan3d_geometry->invalid_enabled == source.scan3d_geometry->invalid_enabled);
  assert(std::isnan(copy.scan3d_geometry->focal_length));
  source.scan3d_geometry->focal_length = 12.5;
  source.scan3d_geometry->baseline = 80;
  source.scan3d_geometry->principal_u = 1;
  source.scan3d_geometry->principal_v = 2;
  onegen::worker::Writer stereo_writer;
  onegen::worker::write(stereo_writer, source);
  const auto stereo_bytes = stereo_writer.take();
  onegen::worker::Reader stereo_reader(stereo_bytes);
  const auto stereo_copy = onegen::worker::readFrame(stereo_reader);
  assert(stereo_copy.scan3d_geometry->focal_length == 12.5);
  assert(stereo_copy.scan3d_geometry->baseline == 80);
  assert(stereo_copy.scan3d_geometry->principal_u == 1);
  assert(stereo_copy.scan3d_geometry->principal_v == 2);

  const std::vector<std::uint8_t> transport{99, 98, 1, 2, 0, 0, 3, 4, 0, 0, 7, 8};
  onegen::Frame image;
  image.payload_type = GenTL::PAYLOAD_TYPE_CHUNK_DATA;
  image.buffer_info.image_present = true;
  image.buffer_info.contains_chunk_data = true;
  image.buffer_info.image_offset = 2;
  image.width = 2; image.height = 2; image.stride = 4;
  image.chunks.push_back({1, 10, {7, 8}});
  onegen::copy_single_payload(image, transport.data(), transport.size());
  assert(image.payload == std::vector<std::uint8_t>({1, 2, 0, 0, 3, 4, 0, 0}));
  assert(image.chunks[0].payload == std::vector<std::uint8_t>({7, 8}));
  // Legacy image chunks include the image itself rather than only metadata.
  image.chunks.insert(image.chunks.begin(), {2, 2, image.payload});
  onegen::copy_single_payload(image, transport.data(), transport.size());
  assert(image.payload.size() == 8 && image.payload[0] == 1);
  const auto rejected = [&](onegen::Frame invalid, std::size_t filled) {
    try { onegen::copy_single_payload(invalid, transport.data(), filled); }
    catch (const onegen::Error&) { return true; }
    return false;
  };
  auto invalid = image;
  invalid.buffer_info.image_offset = 13;
  assert(rejected(invalid, transport.size()));
  invalid = image;
  invalid.chunks = {{1, 8, {7, 8}}};
  assert(rejected(invalid, transport.size()));
  invalid = image;
  invalid.chunks = {{1, 11, {7, 8}}};
  assert(rejected(invalid, transport.size()));
  invalid = image;
  invalid.chunks.clear();
  assert(rejected(invalid, 9));
  invalid.height = 2; invalid.stride = (std::numeric_limits<std::size_t>::max)();
  assert(rejected(invalid, transport.size()));
  auto jpeg = image;
  jpeg.payload_type = GenTL::PAYLOAD_TYPE_JPEG;
  onegen::copy_single_payload(jpeg, transport.data(), transport.size());
  assert(jpeg.payload == image.payload);
  jpeg.chunks.clear();
  assert(rejected(jpeg, transport.size()));
  onegen::Frame planarTransport;
  planarTransport.payload_type = GenTL::PAYLOAD_TYPE_IMAGE;
  planarTransport.pixel_format = 0x026000c1;
  planarTransport.width = 2; planarTransport.height = 2; planarTransport.stride = 10;
  planarTransport.buffer_info.image_offset = 2;
  std::vector<std::uint8_t> planarBytes(66, 0x77);
  planarBytes[2] = 1; planarBytes[61] = 2;
  onegen::copy_single_payload(planarTransport, planarBytes.data(), planarBytes.size());
  assert(planarTransport.payload.size() == 60 && planarTransport.payload.front() == 1 && planarTransport.payload.back() == 2);
  assert(rejected(planarTransport, 12));
  auto chunk_only = image;
  chunk_only.payload_type = GenTL::PAYLOAD_TYPE_CHUNK_ONLY;
  chunk_only.buffer_info.image_present = false;
  onegen::copy_single_payload(chunk_only, transport.data(), transport.size());
  assert(chunk_only.payload == transport && chunk_only.chunks.size() == 2);

  onegen::worker::SlotIdentity identity;
  identity.session = 7;
  identity.epoch = 11;
  identity.token = 13;
  identity.slot = 1;
  identity.generation = 17;
  identity.sequence = 19;
  identity.used_bytes = 4;
  onegen::Frame metadata;
  metadata.components = {{1, "Intensity"}, {65281, "Phase"}};
  metadata.buffer_info.capacity = 8192;
  metadata.buffer_info.size_filled = 4;
  metadata.buffer_info.data_size = 4;
  metadata.buffer_info.image_offset = 2;
  metadata.buffer_info.y_padding = 8;
  metadata.buffer_info.delivered_image_height = 2;
  metadata.buffer_info.incomplete = false;
  metadata.buffer_info.image_present = true;
  metadata.buffer_info.contains_chunk_data = false;
  metadata.buffer_info.diagnostic = "complete";
  metadata.scan3d_geometry.emplace();
  metadata.scan3d_geometry->scale = {0.1, 0.2, 0.3};
  metadata.scan3d_geometry->offset = {1, 2, 3};
  metadata.scan3d_geometry->invalid_value = {-1, -2, -3};
  metadata.scan3d_geometry->invalid_enabled = {true, false, true};
  metadata.scan3d_geometry->distance_unit = "mm";
  metadata.scan3d_geometry->output_mode = "RectifiedC";
  metadata.scan3d_geometry->coordinate_system = "Cartesian";
  metadata.scan3d_geometry->calibrated_axes = {false, true, true};
  metadata.scan3d_geometry->focal_length = 12.5;
  metadata.scan3d_geometry->baseline = 80;
  metadata.scan3d_geometry->principal_u = 1;
  metadata.scan3d_geometry->principal_v = 2;
  metadata.width = 2;
  metadata.height = 2;
  metadata.stride = 2;
  metadata.payload_type = 1;
  metadata.pixel_format = 0x01080001;
  metadata.format_namespace = 0;
  metadata.pixel_endianness = 2;
  metadata.frame_id = 23;
  metadata.timestamp = 29;
  metadata.timestamp_ns = 31;
  metadata.stream_index = 2;
  metadata.stream_id = "stream-0";
  const onegen::worker::FrameReadyRecord ready{identity, &metadata};
  onegen::worker::Writer ready_writer;
  onegen::worker::write(ready_writer, ready);
  const auto ready_bytes = ready_writer.take();
  onegen::worker::Reader ready_reader(ready_bytes);
  const auto decoded_ready = onegen::worker::readFrameReady(ready_reader);
  assert(ready_reader.empty());
  assert(decoded_ready.identity.session == identity.session);
  assert(decoded_ready.identity.used_bytes == identity.used_bytes);
  assert(decoded_ready.metadata.payload.empty());
  assert(decoded_ready.metadata.parts.empty());
  assert(decoded_ready.metadata.chunks.empty());
  assert(decoded_ready.metadata.components.size() == 2);
  assert(decoded_ready.metadata.components[1].name == "Phase");
  assert(decoded_ready.metadata.pixel_endianness == 2);
  assert(decoded_ready.metadata.buffer_info.capacity == 8192);
  assert(decoded_ready.metadata.buffer_info.size_filled == 4);
  assert(decoded_ready.metadata.buffer_info.data_size == 4);
  assert(decoded_ready.metadata.buffer_info.image_offset == 2);
  assert(decoded_ready.metadata.buffer_info.y_padding == 8);
  assert(decoded_ready.metadata.buffer_info.delivered_image_height == 2);
  assert(decoded_ready.metadata.buffer_info.image_present);
  assert(decoded_ready.metadata.buffer_info.diagnostic == "complete");
  assert(decoded_ready.metadata.scan3d_geometry);
  assert(decoded_ready.metadata.scan3d_geometry->scale == metadata.scan3d_geometry->scale);
  assert(decoded_ready.metadata.scan3d_geometry->offset == metadata.scan3d_geometry->offset);
  assert(decoded_ready.metadata.scan3d_geometry->invalid_value == metadata.scan3d_geometry->invalid_value);
  assert(decoded_ready.metadata.scan3d_geometry->invalid_enabled == metadata.scan3d_geometry->invalid_enabled);
  assert(decoded_ready.metadata.scan3d_geometry->distance_unit == "mm");
  assert(decoded_ready.metadata.scan3d_geometry->output_mode == "RectifiedC");
  assert(decoded_ready.metadata.scan3d_geometry->coordinate_system == "Cartesian");
  assert(decoded_ready.metadata.scan3d_geometry->calibrated_axes == metadata.scan3d_geometry->calibrated_axes);
  assert(decoded_ready.metadata.scan3d_geometry->focal_length == 12.5);
  assert(decoded_ready.metadata.scan3d_geometry->baseline == 80);
  assert(decoded_ready.metadata.scan3d_geometry->principal_u == 1);
  assert(decoded_ready.metadata.scan3d_geometry->principal_v == 2);
  assert(decoded_ready.metadata.width == 2 && decoded_ready.metadata.height == 2);
  assert(decoded_ready.metadata.stride == 2);
  assert(decoded_ready.metadata.payload_type == 1);
  assert(decoded_ready.metadata.pixel_format == 0x01080001);
  assert(decoded_ready.metadata.frame_id == 23);
  assert(decoded_ready.metadata.timestamp == 29);
  assert(decoded_ready.metadata.timestamp_ns == 31);
  assert(decoded_ready.metadata.stream_index == 2);
  assert(decoded_ready.metadata.stream_id == "stream-0");
  const auto derived = onegen::worker::describeFrame(decoded_ready.identity, decoded_ready.metadata);
  assert(derived.session == identity.session && derived.slot == identity.slot);
  assert(derived.sequence == identity.sequence && derived.used_bytes == identity.used_bytes);
  assert(derived.frame_id == metadata.frame_id);
  assert(derived.stream_id == metadata.stream_id);

  onegen::worker::Writer tail_writer;
  onegen::worker::writeFrameMetadataTail(tail_writer, metadata);
  const auto tail_bytes = tail_writer.take();
  assert(ready_bytes.size() == 48 + tail_bytes.size());
  assert(std::equal(tail_bytes.begin(), tail_bytes.end(), ready_bytes.begin() + 48));
  onegen::Frame owned_source = metadata;
  owned_source.payload = {1, 2, 3, 4};
  onegen::worker::Writer owned_writer;
  onegen::worker::write(owned_writer, owned_source);
  const auto owned_bytes = owned_writer.take();
  assert(owned_bytes.size() >= tail_bytes.size());
  assert(std::equal(tail_bytes.begin(), tail_bytes.end(),
                    owned_bytes.end() - static_cast<std::ptrdiff_t>(tail_bytes.size())));
  assert(onegen::worker::encodedFrameReadySize(metadata) == ready_bytes.size());

  const auto decode_throws = [](std::vector<std::byte> bytes) {
    try {
      onegen::worker::Reader reader(bytes);
      onegen::worker::readFrameReady(reader);
    } catch (const std::runtime_error&) {
      return true;
    }
    return false;
  };
  auto truncated = ready_bytes;
  truncated.pop_back();
  assert(decode_throws(std::move(truncated)));
  auto trailing = ready_bytes;
  trailing.push_back(std::byte{0});
  assert(decode_throws(std::move(trailing)));
  auto invalid_bool = ready_bytes;
  const auto component_bytes = (8U + 4U + metadata.components[0].name.size())
      + (8U + 4U + metadata.components[1].name.size());
  const auto incomplete_offset = 48U + 4U + component_bytes + 40U + 4U;
  invalid_bool[incomplete_offset] = std::byte{2};
  assert(decode_throws(std::move(invalid_bool)));
  auto oversized_count = ready_bytes;
  const std::uint32_t too_many = onegen::worker::maximum_metadata_components + 1;
  std::memcpy(oversized_count.data() + 48, &too_many, sizeof(too_many));
  assert(decode_throws(std::move(oversized_count)));
  auto oversized_name = ready_bytes;
  const std::uint32_t too_long = onegen::worker::maximum_metadata_string_bytes + 1;
  std::memcpy(oversized_name.data() + 48 + 4 + 8, &too_long, sizeof(too_long));
  assert(decode_throws(std::move(oversized_name)));

  onegen::Frame boundary;
  boundary.buffer_info.diagnostic.assign(onegen::worker::maximum_metadata_diagnostic_bytes, 'd');
  boundary.stream_id.assign(onegen::worker::maximum_metadata_string_bytes, 's');
  boundary.components.resize(1024);
  std::size_t name_bytes = 0;
  for (std::size_t index = 0; index < boundary.components.size(); ++index) {
    boundary.components[index].id = index;
    const auto remaining = 32588 - name_bytes;
    const auto length = index + 1 == boundary.components.size() ? remaining : 31;
    boundary.components[index].name.assign(length, 'c');
    name_bytes += length;
  }
  assert(name_bytes == 32588);
  assert(onegen::worker::encodedFrameReadySize(boundary) ==
         onegen::worker::maximum_frame_ready_size);
  assert(onegen::worker::frameMetadataWithinLimits(boundary));
  onegen::worker::Writer boundary_writer;
  onegen::worker::write(boundary_writer, onegen::worker::FrameReadyRecord{identity, &boundary});
  const auto boundary_bytes = boundary_writer.take();
  assert(boundary_bytes.size() == onegen::worker::maximum_frame_ready_size);
  onegen::worker::Reader boundary_reader(boundary_bytes);
  const auto boundary_copy = onegen::worker::readFrameReady(boundary_reader);
  assert(boundary_copy.metadata.components.size() == 1024);
  boundary.components.back().name.push_back('c');
  assert(onegen::worker::encodedFrameReadySize(boundary) ==
         onegen::worker::maximum_frame_ready_size + 1);
  assert(!onegen::worker::frameMetadataWithinLimits(boundary));
  onegen::worker::Writer oversized_writer;
  bool oversized_rejected = false;
  try {
    onegen::worker::write(oversized_writer, onegen::worker::FrameReadyRecord{identity, &boundary});
  } catch (const std::length_error&) {
    oversized_rejected = true;
  }
  assert(oversized_rejected);
  auto oversized_body = boundary_bytes;
  oversized_body.push_back(std::byte{0});
  assert(decode_throws(std::move(oversized_body)));
}
