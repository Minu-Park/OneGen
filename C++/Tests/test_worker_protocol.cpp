#include "OneGenWorkerMessages.h"

#include <cassert>

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
  source.scan3d_geometry.emplace();
  source.scan3d_geometry->scale = {0.1, 0.2, 0.3};
  source.scan3d_geometry->invalid_enabled = {true, false, true};

  onegen::worker::Writer writer;
  onegen::worker::write(writer, source);
  const auto bytes = writer.take();
  onegen::worker::Reader reader(bytes);
  const auto copy = onegen::worker::readFrame(reader);

  assert(reader.empty());
  assert(copy.payload == source.payload);
  assert(copy.width == source.width);
  assert(copy.height == source.height);
  assert(copy.pixel_format == source.pixel_format);
  assert(copy.buffer_info.size_filled == source.buffer_info.size_filled);
  assert(copy.scan3d_geometry);
  assert(copy.scan3d_geometry->scale == source.scan3d_geometry->scale);
  assert(copy.scan3d_geometry->invalid_enabled == source.scan3d_geometry->invalid_enabled);
}
