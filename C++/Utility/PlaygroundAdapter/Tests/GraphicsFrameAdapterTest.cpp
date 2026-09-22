#include "OneGenGraphicsFrameAdapter.h"
#include <array>
#include <algorithm>
#include <bit>

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

int main()
{
  onegen::Frame source;
  source.width = 2;
  source.height = 2;
  source.stride = 4;
  source.pixel_format = 0x01080001;
  source.payload = {1, 2, 0, 0, 3, 4, 0, 0};

  const auto frame = oneGenGraphicsFrame(source, 7);
  assert(frame && frame->hasImage());
  assert(frame->images.front().payload.pixelFormat == GraphicsImagePixelFormat::Mono8);
  assert(frame->images.front().payload.stride == 4);
  assert(frame->images.front().payload.frameSequence == 7);

  source.width = 4;
  source.height = 1;
  source.stride = 8;
  source.frame_id = 42;
  source.pixel_format = 0x0110000d;
  source.payload.resize(8);
  const auto bayer = oneGenGraphicsFrame(source, 8);
  assert(bayer && bayer->images.front().payload.pixelFormat == GraphicsImagePixelFormat::BayerRG10);
  assert(bayer->metadata.frameId == "42");

  source.stride = 5;
  source.pixel_format = 0x010a0046;
  source.payload.resize(5);
  const auto packed = oneGenGraphicsFrame(source, 9);
  assert(packed && packed->images.front().payload.pixelFormat == GraphicsImagePixelFormat::Mono10Packed);

  source.width = 2;
  source.stride = 12;
  source.pixel_format = 0x02300018;
  source.payload.resize(12);
  const auto rgb10 = oneGenGraphicsFrame(source, 10);
  assert(rgb10 && rgb10->images.front().payload.pixelFormat == GraphicsImagePixelFormat::RGB10Packed);
  assert(rgb10->images.front().payload.bitAlignment == GraphicsImageBitAlignment::LeastSignificant);

  source.width = 4;
  source.stride = 8;
  source.pixel_format = 0x02100032;
  source.payload.resize(8);
  assert(oneGenGraphicsFrame(source, 11)->images.front().payload.pixelFormat
         == GraphicsImagePixelFormat::YCbCr422_8);

  source.pixel_format = 0x010c0004;
  assert(!oneGenGraphicsFrame(source, 12));

  source.pixel_format = 0;
  assert(!oneGenGraphicsFrame(source, 13));

  source.payload_type = 5;
  source.width = 640;
  source.height = 480;
  source.stride = source.payload.size();
  assert(oneGenGraphicsFrame(source, 13)->images.front().payload.pixelFormat == GraphicsImagePixelFormat::Jpeg);
  source.payload_type = 0;

  source.pixel_format = 0x01100007;
  source.width = 2;
  source.height = 1;
  source.stride = 4;
  source.payload = {1, 2, 3, 4};
  source.pixel_endianness = 2;
  const auto swapped = oneGenGraphicsFrame(source, 13);
  assert(swapped && swapped->hasImage());
  const auto* bytes = swapped->images.front().payload.storage.get();
  if (std::endian::native == std::endian::little)
    assert(bytes[0] == 2 && bytes[1] == 1 && bytes[2] == 4 && bytes[3] == 3);
  else
    assert(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4);
  source.pixel_endianness = 0;
  std::string rejectionReason;
  source.payload_type = 8;
  assert(!oneGenGraphicsFrame(source, 13, &rejectionReason));
  source.payload_type = 4;
  source.buffer_info.image_present = false;
  assert(!oneGenGraphicsFrame(source, 13, &rejectionReason));
  source.buffer_info.image_present = true;
  assert(oneGenGraphicsFrame(source, 13)->hasImage());
  source.payload_type = 2;
  assert(!oneGenGraphicsFrame(source, 13));
  source.payload_type = 0;

  onegen::Frame multipart;
  multipart.frame_id = 77;
  multipart.scan3d_geometry.emplace();
  multipart.scan3d_geometry->scale = {1.0, 1.0, 0.5};
  multipart.scan3d_geometry->offset = {0.0, 0.0, -1.0};
  multipart.scan3d_geometry->distance_unit = "Millimeter";
  onegen::Frame::Part xyz;
  xyz.width = 2;
  xyz.height = 1;
  xyz.stride = 24;
  xyz.data_type = 5;
  xyz.data_format = 0x026000c0;
  const float points[] = {1.0F, 2.0F, 6.0F, 4.0F, 5.0F, 8.0F};
  xyz.payload.resize(sizeof(points));
  std::memcpy(xyz.payload.data(), points, sizeof(points));
  multipart.parts.push_back(xyz);

  onegen::Frame::Part confidence;
  confidence.width = 2;
  confidence.height = 1;
  confidence.stride = 2;
  confidence.data_type = 9;
  confidence.data_format = 0x010800c6;
  confidence.payload = {10, 20};
  multipart.parts.push_back(confidence);

  const auto scene = oneGenGraphicsFrame(multipart, 14);
  assert(scene && scene->hasRangeFrame());
  assert(scene->ranges.front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
  assert(scene->ranges.front().payload.zValues[0] == 2.0F);
  assert(scene->ranges.front().payload.confidence == std::vector<float>({10.0F, 20.0F}));
  assert(scene->metadata.frameId == "77");

  onegen::Frame c16Frame;
  c16Frame.scan3d_geometry = multipart.scan3d_geometry;
  c16Frame.scan3d_geometry->invalid_enabled[2] = true;
  c16Frame.scan3d_geometry->invalid_value[2] = 0.0;
  onegen::Frame::Part c16;
  c16.width = 2;
  c16.height = 1;
  c16.stride = 4;
  c16.data_type = 5;
  c16.data_format = 0x011000b8;
  c16.payload = {6, 0, 0, 0};
  c16Frame.parts.push_back(c16);
  const auto c16Scene = oneGenGraphicsFrame(c16Frame, 15);
  assert(c16Scene && c16Scene->hasRangeFrame());
  assert(c16Scene->ranges.front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(!std::isfinite(c16Scene->ranges.front().payload.xScale));
  assert(c16Scene->ranges.front().payload.physicalZAt(0) == 2.0);
  assert(c16Scene->ranges.front().payload.validMask == std::vector<std::uint8_t>({1, 0}));

  onegen::Frame floating;
  floating.scan3d_geometry = multipart.scan3d_geometry;
  const auto scalarPart = [](std::uint64_t format, std::uint64_t type,
                             const std::vector<float>& samples) {
    onegen::Frame::Part part;
    part.width = 2; part.height = 2; part.stride = 12;
    part.data_format = format; part.data_type = type;
    part.payload.resize(24, 0xff);
    std::memcpy(part.payload.data(), samples.data(), 8);
    std::memcpy(part.payload.data() + 12, samples.data() + 2, 8);
    return part;
  };
  const auto nan = std::numeric_limits<float>::quiet_NaN();
  floating.parts.push_back(scalarPart(0x012000bf, 5, {0, -2, nan, 6}));
  floating.parts.push_back(scalarPart(0x012000c8, 9, {0.25F, 0.75F, nan, 2}));
  const auto floatScene = oneGenGraphicsFrame(floating, 17);
  assert(floatScene && floatScene->hasRangeFrame());
  const auto& range = floatScene->ranges.front().payload;
  assert(range.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(!std::isfinite(range.xScale) && !std::isfinite(range.yScale));
  assert(range.physicalZAt(0) == -1 && range.physicalZAt(1) == -2 && range.physicalZAt(3) == 2);
  assert(range.validMask == std::vector<std::uint8_t>({1, 1, 0, 1}));
  assert(range.rangeRaw.empty() && range.confidenceRaw.empty());
  assert(range.rangeBits == 32 && range.confidenceBits == 32);
  assert(range.confidence[0] == 0.25F && range.confidence[3] == 2);
  assert(range.confidenceValidMask == std::vector<std::uint8_t>({1, 1, 0, 1}));
  assert(range.confidenceField.bitsPerSample == 32);
  onegen::Frame single;
  single.width = 2; single.height = 2; single.stride = 12;
  single.pixel_format = 0x012000bf;
  single.payload = floating.parts[0].payload;
  single.scan3d_geometry = floating.scan3d_geometry;
  const auto singleRange = oneGenGraphicsFrame(single, 23);
  assert(singleRange && singleRange->ranges.front().payload.physicalZAt(3) == 2);
  single.scan3d_geometry.reset();
  assert(!oneGenGraphicsFrame(single, 24));
  single.pixel_format = 0x012000c8;
  single.payload = floating.parts[1].payload;
  const auto confidencePreview = oneGenGraphicsFrame(single, 25);
  assert(confidencePreview && confidencePreview->hasRangeFrame());
  assert(confidencePreview->ranges.front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(confidencePreview->ranges.front().payload.zValues[3] == 2);
  assert(!std::isfinite(confidencePreview->ranges.front().payload.xScale));
  assert(confidencePreview->ranges.front().payload.validMask == std::vector<std::uint8_t>({1, 1, 0, 1}));
  single.parts = {floating.parts[1]};
  assert(oneGenGraphicsFrame(single, 26)->ranges.front().payload.rangeField.displayName == "Confidence");
  single.pixel_endianness = 3;
  assert(!oneGenGraphicsFrame(single, 27));
  // The complete vendor PixelFormat enumeration, including padded monochrome rows.
  for (const auto format : {0x01080001U, 0x01100007U}) {
    onegen::Frame mono;
    mono.width = 2; mono.height = 2; mono.stride = 6;
    mono.pixel_format = format; mono.payload.resize(12);
    assert(oneGenGraphicsFrame(mono, 28)->hasImage());
    onegen::Frame::Part imagePart;
    imagePart.width = 2; imagePart.height = 2; imagePart.stride = 6;
    imagePart.data_type = 1; imagePart.data_format = format; imagePart.payload = mono.payload;
    mono.parts = {imagePart};
    assert(oneGenGraphicsFrame(mono, 29)->hasImage());
    mono.scan3d_geometry = floating.scan3d_geometry;
    mono.parts.insert(mono.parts.begin(), floating.parts[0]);
    const auto paired = oneGenGraphicsFrame(mono, 30);
    assert(paired && paired->hasImage() && paired->hasRangeFrame());
    assert(paired->images.front().descriptor.groupId == "0:0");
    assert(paired->ranges.front().descriptor.groupId == "0:0");
    assert(paired->ranges.front().payload.intensity == std::vector<float>({0, 0, 0, 0}));
    assert(paired->ranges.front().payload.intensityField.bitsPerSample == (format == 0x01080001U ? 8 : 16));

    auto secondRangePart = floating.parts[0];
    secondRangePart.region_id = 1;
    auto secondImagePart = imagePart;
    secondImagePart.region_id = 1;
    mono.parts.push_back(std::move(secondRangePart));
    mono.parts.push_back(std::move(secondImagePart));
    const auto multiple = oneGenGraphicsFrame(mono, 31);
    assert(multiple && multiple->ranges.size() == 2 && multiple->images.size() == 2);
    assert(multiple->ranges[0].descriptor.id != multiple->ranges[1].descriptor.id);
    assert(multiple->images[0].descriptor.id != multiple->images[1].descriptor.id);
    assert(multiple->ranges[1].descriptor.groupId == "0:1");
    assert(multiple->images[1].descriptor.groupId == "0:1");
  }
  floating.scan3d_geometry->invalid_enabled[2] = true;
  floating.scan3d_geometry->invalid_value[2] = -2;
  assert(oneGenGraphicsFrame(floating, 18)->ranges.front().payload.validMask[1] == 0);
  floating.parts[1].payload.pop_back();
  assert(oneGenGraphicsFrame(floating, 19)->ranges.front().payload.confidence.empty());
  floating.pixel_endianness = 3;
  assert(!oneGenGraphicsFrame(floating, 20));
  floating.pixel_endianness = 0;
  floating.parts[0].payload.pop_back();
  assert(!oneGenGraphicsFrame(floating, 21));
  floating.parts[0].payload.resize(24);
  floating.scan3d_geometry.reset();
  assert(!oneGenGraphicsFrame(floating, 22));

  // Independent PFNC fixtures exercise every unpacked C/AC/ABC layout in both byte orders.
  struct LayoutCase { std::uint64_t format; unsigned components, bytes; bool planar, floating; };
  const LayoutCase layouts[] = {
    {0x010800b1, 1, 1, false, false}, {0x011000b8, 1, 2, false, false}, {0x012000bf, 1, 4, false, true},
    {0x021800b2, 3, 1, false, false}, {0x021800b3, 3, 1, true, false},
    {0x023000b9, 3, 2, false, false}, {0x023000ba, 3, 2, true, false},
    {0x026000c0, 3, 4, false, true}, {0x026000c1, 3, 4, true, true},
    {0x021000b4, 2, 1, false, false}, {0x021000b5, 2, 1, true, false},
    {0x022000bb, 2, 2, false, false}, {0x022000bc, 2, 2, true, false},
    {0x024000c2, 2, 4, false, true}, {0x024000c3, 2, 4, true, true}
  };
  for (const auto& layout : layouts) for (unsigned endian : {1U, 2U}) {
    onegen::Frame coordinates;
    coordinates.width = 2; coordinates.height = 2; coordinates.pixel_format = layout.format;
    coordinates.pixel_endianness = endian;
    coordinates.stride = 2 * layout.bytes * (layout.planar ? 1 : layout.components) + 2;
    coordinates.scan3d_geometry.emplace();
    auto& geometry = *coordinates.scan3d_geometry;
    geometry.distance_unit = "Millimeter"; geometry.coordinate_system = "Cartesian";
    geometry.output_mode = layout.components == 1 ? "CalibratedC"
        : (layout.components == 2 ? "CalibratedAC" : "CalibratedABC_Grid");
    geometry.scale = {2, 3, 0.5}; geometry.offset = {10, 20, -1};
    const auto planeSize = coordinates.stride * 2;
    coordinates.payload.resize(planeSize * (layout.planar ? layout.components : 1), 0xab);
    for (unsigned i = 0; i < 4; ++i) for (unsigned component = 0; component < layout.components; ++component) {
      const unsigned axis = layout.components == 3 ? component : (component + 1 == layout.components ? 2 : 0);
      const unsigned value = axis == 2 && i == 1 ? 0 : 1 + i + 10 * axis;
      std::uint32_t code = value;
      if (layout.floating) { const float sample = static_cast<float>(value); std::memcpy(&code, &sample, 4); }
      const auto position = (i / 2) * coordinates.stride + (layout.planar
          ? component * planeSize + (i % 2) * layout.bytes
          : ((i % 2) * layout.components + component) * layout.bytes);
      for (unsigned byte = 0; byte < layout.bytes; ++byte)
        coordinates.payload[position + byte] = static_cast<std::uint8_t>(code >> (8 * (endian == 2 ? layout.bytes - 1 - byte : byte)));
    }
    const auto converted = oneGenGraphicsFrame(coordinates, 31);
    assert(converted && converted->hasRangeFrame());
    const auto& decoded = converted->ranges.front().payload;
    assert(decoded.physicalZAt(0) == 9.5 && decoded.physicalZAt(1) == -1);
    assert(decoded.validMask == std::vector<std::uint8_t>({1, 1, 1, 1})); // Zero is valid unless declared invalid.
    if (layout.components > 1) {
      assert(decoded.xValues[0] == 12 && decoded.xValues[3] == 18);
      assert(decoded.yValues[0] == (layout.components == 3 ? 53 : 20));
      assert(decoded.yValues[3] == (layout.components == 3 ? 62 : 23));
    }
    onegen::Frame::Part coordinatePart;
    coordinatePart.width = 2; coordinatePart.height = 2; coordinatePart.stride = coordinates.stride;
    coordinatePart.data_type = 5; coordinatePart.data_format = layout.format; coordinatePart.payload = coordinates.payload;
    coordinates.parts = {coordinatePart};
    assert(oneGenGraphicsFrame(coordinates, 32)->ranges.front().payload.physicalZAt(0) == 9.5);
    if (layout.components == 3 && layout.planar) {
      const std::array<std::uint64_t, 3> planeFormats = layout.bytes == 1
          ? std::array<std::uint64_t, 3>{0x010800af, 0x010800b0, 0x010800b1}
          : (layout.bytes == 2 ? std::array<std::uint64_t, 3>{0x011000b6, 0x011000b7, 0x011000b8}
                              : std::array<std::uint64_t, 3>{0x012000bd, 0x012000be, 0x012000bf});
      coordinates.parts.clear();
      for (unsigned axis = 0; axis < 3; ++axis) {
        auto plane = coordinatePart;
        plane.data_format = planeFormats[axis];
        plane.payload.assign(coordinates.payload.begin() + axis * planeSize,
                             coordinates.payload.begin() + (axis + 1) * planeSize);
        coordinates.parts.push_back(std::move(plane));
      }
      const auto separated = oneGenGraphicsFrame(coordinates, 42);
      assert(separated && separated->ranges.front().payload.xValues[0] == 12 && separated->ranges.front().payload.physicalZAt(0) == 9.5);
      const auto firstPlanarGroup = coordinates.parts;
      for (auto plane : firstPlanarGroup) {
        plane.region_id = 1;
        coordinates.parts.push_back(std::move(plane));
      }
      const auto multiplePlanar = oneGenGraphicsFrame(coordinates, 68);
      assert(multiplePlanar && multiplePlanar->ranges.size() == 2);
      assert(multiplePlanar->ranges[0].descriptor.groupId == "0:0");
      assert(multiplePlanar->ranges[1].descriptor.groupId == "0:1");
      coordinates.parts.resize(firstPlanarGroup.size());
      coordinates.parts[1].source_id = 9;
      assert(!oneGenGraphicsFrame(coordinates, 43));
      coordinates.parts[1].source_id = 0;
      coordinates.parts.push_back(coordinates.parts.front());
      assert(!oneGenGraphicsFrame(coordinates, 44));
    }
    if (layout.components == 3) {
      coordinates.parts = {coordinatePart};
      coordinates.parts.push_back(scalarPart(0x012000c8, 9, {0.25F, 0.75F, nan, 2}));
      if (endian == 2) {
        auto& bytes = coordinates.parts.back().payload;
        for (unsigned row = 0; row < 2; ++row) for (unsigned col = 0; col < 2; ++col)
          std::reverse(bytes.begin() + row * 12 + col * 4, bytes.begin() + row * 12 + col * 4 + 4);
      }
      coordinates.parts.back().source_id = 1;
      assert(oneGenGraphicsFrame(coordinates, 45)->ranges.front().payload.confidence.empty());
      coordinates.parts.back().source_id = 0;
      assert(oneGenGraphicsFrame(coordinates, 46)->ranges.front().payload.confidence[0] == 0.25F);
      geometry.output_mode = "CalibratedABC_PointCloud";
      const auto cloud = oneGenGraphicsFrame(coordinates, 47);
      assert(cloud && cloud->hasPointCloud() && !cloud->hasRangeFrame() && !cloud->hasSurfaceMesh());
      assert(cloud->pointClouds.front().payload.xyz.size() == 12 && cloud->pointClouds.front().payload.xyz[0] == 12 && cloud->pointClouds.front().payload.xyz[2] == 9.5);
      assert(cloud->pointClouds.front().payload.scalarFields.size() == 1 && cloud->pointClouds.front().payload.scalarFields[0].values[0] == 0.25F);
      geometry.invalid_enabled[2] = true;
      const auto filteredCloud = oneGenGraphicsFrame(coordinates, 48);
      assert(filteredCloud && filteredCloud->pointClouds.front().payload.xyz.size() == 9);
      assert(filteredCloud->pointClouds.front().payload.scalarFields[0].values.size() == 3);
      geometry.invalid_enabled[2] = false;
      coordinates.parts.clear();
      assert(oneGenGraphicsFrame(coordinates, 49)->hasPointCloud());
      geometry.output_mode = "CalibratedABC_Grid";
    }
    coordinates.parts.clear();
    geometry.invalid_enabled[2] = true; geometry.invalid_value[2] = 0;
    assert(oneGenGraphicsFrame(coordinates, 33)->ranges.front().payload.validMask[1] == 0);
    coordinates.payload.pop_back();
    assert(!oneGenGraphicsFrame(coordinates, 34));
  }
  onegen::Frame rectifiedFrame;
  rectifiedFrame.scan3d_geometry = multipart.scan3d_geometry;
  rectifiedFrame.scan3d_geometry->output_mode = "RectifiedC";
  rectifiedFrame.scan3d_geometry->coordinate_system = "Cartesian";
  rectifiedFrame.scan3d_geometry->scale = {2, -3, 0.5};
  rectifiedFrame.scan3d_geometry->offset = {10, 20, -1};
  rectifiedFrame.parts = {scalarPart(0x012000bf, 5, {0, 1, 2, 3})};
  auto rectifiedScene = oneGenGraphicsFrame(rectifiedFrame, 35);
  assert(rectifiedScene && rectifiedScene->ranges.front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::UniformPhysical);
  assert(rectifiedScene->ranges.front().payload.xScale == 2 && rectifiedScene->ranges.front().payload.yScale == -3);
  assert(rectifiedScene->ranges.front().payload.xOffset == 10 && rectifiedScene->ranges.front().payload.yOffset == 20);
  rectifiedFrame.scan3d_geometry->calibrated_axes[0] = false;
  assert(!oneGenGraphicsFrame(rectifiedFrame, 36));
  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedC";
  assert(oneGenGraphicsFrame(rectifiedFrame, 37));
  rectifiedFrame.scan3d_geometry->coordinate_system = "Spherical";
  assert(!oneGenGraphicsFrame(rectifiedFrame, 38));
  rectifiedFrame.scan3d_geometry->coordinate_system = "Cartesian";
  rectifiedFrame.scan3d_geometry->output_mode = "DisparityC";
  assert(!oneGenGraphicsFrame(rectifiedFrame, 39));
  rectifiedFrame.scan3d_geometry->output_mode = "RectifiedC_Linescan";
  assert(!oneGenGraphicsFrame(rectifiedFrame, 40));
  rectifiedFrame.scan3d_geometry->output_mode = "UncalibratedC";
  rectifiedFrame.scan3d_geometry->distance_unit.clear();
  const auto rawScene = oneGenGraphicsFrame(rectifiedFrame, 41);
  assert(rawScene && rawScene->ranges.front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(rawScene->ranges.front().payload.zValues[3] == 3 && !std::isfinite(rawScene->ranges.front().payload.xScale));

  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedC";
  rectifiedFrame.scan3d_geometry->distance_unit = "Millimeter";
  rectifiedFrame.scan3d_geometry->scale[2] = 1e100;
  const auto overflowScene = oneGenGraphicsFrame(rectifiedFrame, 50);
  assert(overflowScene && overflowScene->ranges.front().payload.validMask == std::vector<std::uint8_t>({1, 0, 0, 0}));

  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedABC_PointCloud";
  rectifiedFrame.parts = {scalarPart(0x012000c8, 9, {0.25F, 0.75F, nan, 2})};
  const auto confidenceOnly = oneGenGraphicsFrame(rectifiedFrame, 51);
  assert(confidenceOnly && confidenceOnly->hasRangeFrame() && !confidenceOnly->hasPointCloud());
  assert(confidenceOnly->ranges.front().payload.rangeField.domain == MeasurementValueDomain::Native);

  onegen::Frame packedC;
  packedC.width = 2; packedC.height = 1; packedC.pixel_format = 0x010a00d7;
  packedC.stride = 4;
  packedC.payload = {0, 0, 0, 0};
  packedC.payload[0] = 0x01;
  packedC.payload[1] = 0x08; // pixel1 bit 11 = value 2
  packedC.scan3d_geometry.emplace();
  packedC.scan3d_geometry->distance_unit = "Millimeter";
  packedC.scan3d_geometry->output_mode = "CalibratedC";
  packedC.scan3d_geometry->scale = {1, 1, 1};
  packedC.scan3d_geometry->offset = {0, 0, -1};
  const auto packedScene = oneGenGraphicsFrame(packedC, 52);
  assert(packedScene && packedScene->ranges.front().payload.physicalZAt(0) == 0 && packedScene->ranges.front().payload.physicalZAt(1) == 1);

  onegen::Frame f64;
  f64.width = 2; f64.height = 1; f64.pixel_format = 0x01400128;
  f64.scan3d_geometry.emplace();
  f64.scan3d_geometry->distance_unit = "Millimeter";
  f64.scan3d_geometry->output_mode = "CalibratedC";
  f64.scan3d_geometry->scale = {1, 1, 2};
  f64.scan3d_geometry->offset = {0, 0, -1};
  const double f64Samples[] = {1.0, 3.0};
  f64.payload.resize(16);
  std::memcpy(f64.payload.data(), f64Samples, sizeof(f64Samples));
  const auto f64Scene = oneGenGraphicsFrame(f64, 53);
  assert(f64Scene && f64Scene->ranges.front().payload.physicalZAt(0) == 1 && f64Scene->ranges.front().payload.physicalZAt(1) == 5);
  auto be64 = f64;
  be64.pixel_endianness = 2;
  if (std::endian::native == std::endian::little)
    for (std::size_t i = 0; i + 8 <= be64.payload.size(); i += 8)
      std::reverse(be64.payload.begin() + i, be64.payload.begin() + i + 8);
  assert(oneGenGraphicsFrame(be64, 67)->ranges.front().payload.physicalZAt(0) == 1);
  f64.scan3d_geometry->distance_unit = "Meter";
  f64.scan3d_geometry->scale[2] = 1;
  f64.scan3d_geometry->offset[2] = 0;
  const auto meterScene = oneGenGraphicsFrame(f64, 54);
  assert(meterScene && meterScene->ranges.front().payload.physicalZAt(0) == 1000);
  assert(meterScene->ranges.front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(!std::isfinite(meterScene->ranges.front().payload.xScale));
  f64.scan3d_geometry->distance_unit = "Inch";
  assert(oneGenGraphicsFrame(f64, 60)->ranges.front().payload.physicalZAt(0) == 25.4F);

  onegen::Frame disparity;
  disparity.parts = {scalarPart(0x012000bf, 5, {2, 4, 2, 4})};
  disparity.scan3d_geometry.emplace();
  disparity.scan3d_geometry->output_mode = "DisparityC";
  disparity.scan3d_geometry->distance_unit = "Millimeter";
  disparity.scan3d_geometry->scale = {1, 1, 1};
  disparity.scan3d_geometry->focal_length = 10;
  disparity.scan3d_geometry->baseline = 5;
  const auto disparityScene = oneGenGraphicsFrame(disparity, 55);
  assert(disparityScene && disparityScene->ranges.front().payload.physicalZAt(0) == 25);
  assert(disparityScene->ranges.front().payload.physicalZAt(1) == 12.5F);
  disparity.scan3d_geometry->principal_u = 0;
  disparity.scan3d_geometry->principal_v = 0;
  const auto projected = oneGenGraphicsFrame(disparity, 56);
  assert(projected && projected->ranges.front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
  assert(projected->ranges.front().payload.xValues[1] == 1.25F);

  onegen::Frame spherical;
  spherical.scan3d_geometry.emplace();
  spherical.scan3d_geometry->output_mode = "CalibratedABC_Grid";
  spherical.scan3d_geometry->coordinate_system = "Spherical";
  spherical.scan3d_geometry->distance_unit = "Millimeter";
  onegen::Frame::Part abc;
  abc.width = 1; abc.height = 1; abc.stride = 12; abc.data_type = 5; abc.data_format = 0x026000c0;
  const float polar[] = {0.F, 0.F, 2.F};
  abc.payload.resize(12);
  std::memcpy(abc.payload.data(), polar, sizeof(polar));
  spherical.parts = {abc};
  const auto sphere = oneGenGraphicsFrame(spherical, 57);
  assert(sphere && sphere->ranges.front().payload.xValues[0] == 0 && sphere->ranges.front().payload.yValues[0] == 0);
  assert(sphere->ranges.front().payload.zValues[0] == 2);

  onegen::Frame cylindrical = spherical;
  cylindrical.scan3d_geometry->coordinate_system = "Cylindrical";
  const float cyl[] = {0.F, 3.F, 2.F};
  std::memcpy(cylindrical.parts[0].payload.data(), cyl, sizeof(cyl));
  const auto cylinder = oneGenGraphicsFrame(cylindrical, 58);
  assert(cylinder && cylinder->ranges.front().payload.xValues[0] == 2 && cylinder->ranges.front().payload.yValues[0] == 3);
  assert(cylinder->ranges.front().payload.zValues[0] == 0);

  onegen::Frame meterCloud = spherical;
  meterCloud.scan3d_geometry->output_mode = "CalibratedABC_PointCloud";
  meterCloud.scan3d_geometry->coordinate_system = "Cartesian";
  meterCloud.scan3d_geometry->distance_unit = "Meter";
  const float meterAbc[] = {1.F, 2.F, 3.F};
  std::memcpy(meterCloud.parts[0].payload.data(), meterAbc, sizeof(meterAbc));
  const auto meterPoints = oneGenGraphicsFrame(meterCloud, 66);
  assert(meterPoints && meterPoints->hasPointCloud() && meterPoints->pointClouds.front().payload.xyz.size() == 3);
  assert(meterPoints->pointClouds.front().payload.coordinateUnit == GraphicsLengthUnit::Millimeter);
  assert(meterPoints->pointClouds.front().payload.xyz[0] == 1000 && meterPoints->pointClouds.front().payload.xyz[1] == 2000);
  assert(meterPoints->pointClouds.front().payload.xyz[2] == 3000);

  onegen::Frame confidenceBits;
  confidenceBits.parts = {scalarPart(0x012000bf, 5, {0, 1, 2, 3}), {}};
  confidenceBits.parts[1].width = 2; confidenceBits.parts[1].height = 2;
  confidenceBits.parts[1].stride = 2; confidenceBits.parts[1].data_type = 9;
  confidenceBits.parts[1].data_format = 0x010800c4;
  confidenceBits.parts[1].payload = {1, 0, 3, 0};
  confidenceBits.scan3d_geometry.emplace();
  confidenceBits.scan3d_geometry->distance_unit = "Millimeter";
  confidenceBits.scan3d_geometry->output_mode = "CalibratedC";
  confidenceBits.scan3d_geometry->scale = {1, 1, 0.5};
  confidenceBits.scan3d_geometry->offset = {0, 0, -1};
  const auto bitConfidence = oneGenGraphicsFrame(confidenceBits, 59);
  assert(bitConfidence && bitConfidence->ranges.front().payload.confidence == std::vector<float>({1, 0, 1, 0}));
  assert(bitConfidence->ranges.front().payload.confidenceBits == 1);
  confidenceBits.parts[1].data_format = 0x010100c5;
  confidenceBits.parts[1].stride = 1;
  confidenceBits.parts[1].payload = {0x05, 0x05};
  const auto packedConfidence = oneGenGraphicsFrame(confidenceBits, 61);
  assert(packedConfidence && packedConfidence->ranges.front().payload.confidence == std::vector<float>({1, 0, 1, 0}));
  assert(packedConfidence->ranges.front().payload.confidenceBits == 1);

  onegen::Frame packedAc;
  packedAc.parts.resize(1);
  packedAc.parts[0].width = 1; packedAc.parts[0].height = 1; packedAc.parts[0].stride = 3;
  packedAc.parts[0].data_type = 5; packedAc.parts[0].data_format = 0x021400f0;
  packedAc.parts[0].payload = {0x01, 0x08, 0x00};
  packedAc.scan3d_geometry.emplace();
  packedAc.scan3d_geometry->output_mode = "RectifiedAC";
  packedAc.scan3d_geometry->distance_unit = "Millimeter";
  packedAc.scan3d_geometry->scale = {1, 1, 1};
  packedAc.scan3d_geometry->offset = {0, 5, 0};
  const auto rectifiedAc = oneGenGraphicsFrame(packedAc, 62);
  assert(rectifiedAc && rectifiedAc->ranges.front().payload.xValues[0] == 1);
  assert(rectifiedAc->ranges.front().payload.yValues[0] == 5 && rectifiedAc->ranges.front().payload.zValues[0] == 2);
  packedAc.scan3d_geometry->output_mode = "CalibratedAC";
  assert(oneGenGraphicsFrame(packedAc, 63)->ranges.front().payload.zValues[0] == 2);

  onegen::Frame planar64;
  planar64.scan3d_geometry.emplace();
  planar64.scan3d_geometry->output_mode = "CalibratedABC_Grid";
  planar64.scan3d_geometry->distance_unit = "Millimeter";
  const std::uint64_t plane64[] = {0x01400126, 0x01400127, 0x01400128};
  const double planeSamples[] = {1.0, 2.0, 3.0};
  for (unsigned axis = 0; axis < 3; ++axis) {
    onegen::Frame::Part plane;
    plane.width = 1; plane.height = 1; plane.stride = 8; plane.data_type = 5;
    plane.data_format = plane64[axis];
    plane.payload.resize(8);
    std::memcpy(plane.payload.data(), &planeSamples[axis], 8);
    planar64.parts.push_back(std::move(plane));
  }
  const auto assembled64 = oneGenGraphicsFrame(planar64, 64);
  assert(assembled64 && assembled64->ranges.front().payload.xValues[0] == 1);
  assert(assembled64->ranges.front().payload.yValues[0] == 2 && assembled64->ranges.front().payload.zValues[0] == 3);

  onegen::Frame sphereRectified;
  sphereRectified.parts = {scalarPart(0x012000bf, 5, {2, 2, 2, 2})};
  sphereRectified.scan3d_geometry.emplace();
  sphereRectified.scan3d_geometry->output_mode = "RectifiedC";
  sphereRectified.scan3d_geometry->coordinate_system = "Spherical";
  sphereRectified.scan3d_geometry->distance_unit = "Millimeter";
  sphereRectified.scan3d_geometry->scale = {1, 1, 1};
  sphereRectified.scan3d_geometry->offset = {0, 0, 0};
  const auto sphereGrid = oneGenGraphicsFrame(sphereRectified, 65);
  assert(sphereGrid && sphereGrid->ranges.front().payload.xValues[0] == 0);
  assert(sphereGrid->ranges.front().payload.yValues[0] == 0 && sphereGrid->ranges.front().payload.zValues[0] == 2);

  multipart.scan3d_geometry->distance_unit.clear();
  assert(!oneGenGraphicsFrame(multipart, 16));
}
