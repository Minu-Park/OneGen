#include "OneGenWorkerMessages.h"
#include "../Internal/FramePayload.h"

#include <cassert>
#include <cmath>

int main()
{
  onegen::FeatureNode feature;
  feature.kind = onegen::NodeKind::Register;
  feature.unit = "mm";
  feature.representation = "Linear";
  feature.float_inc = 0.0000001;
  feature.register_length = 4;
  feature.children.push_back(feature);
  onegen::worker::Writer feature_writer;
  onegen::worker::write(feature_writer, feature);
  const auto feature_bytes = feature_writer.take();
  onegen::worker::Reader feature_reader(feature_bytes);
  const auto feature_copy = onegen::worker::readFeatureNode(feature_reader);
  assert(feature_reader.empty());
  assert(feature_copy.unit == feature.unit && feature_copy.float_inc == feature.float_inc);
  assert(feature_copy.register_length == 4 && feature_copy.children.size() == 1);
  onegen::Frame source;
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
}
