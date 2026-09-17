#include "OneGenGraphicsFrameAdapter.h"

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
  assert(frame->image->pixelFormat == GraphicsImagePixelFormat::Mono8);
  assert(frame->image->stride == 4);
  assert(frame->image->frameSequence == 7);

  source.width = 4;
  source.height = 1;
  source.stride = 8;
  source.frame_id = 42;
  source.pixel_format = 0x0110000d;
  source.payload.resize(8);
  const auto bayer = oneGenGraphicsFrame(source, 8);
  assert(bayer && bayer->image->pixelFormat == GraphicsImagePixelFormat::BayerRG10);
  assert(bayer->metadata.frameId == "42");

  source.stride = 5;
  source.pixel_format = 0x010a0046;
  source.payload.resize(5);
  const auto packed = oneGenGraphicsFrame(source, 9);
  assert(packed && packed->image->pixelFormat == GraphicsImagePixelFormat::Mono10Packed);

  source.width = 2;
  source.stride = 12;
  source.pixel_format = 0x02300018;
  source.payload.resize(12);
  const auto rgb10 = oneGenGraphicsFrame(source, 10);
  assert(rgb10 && rgb10->image->pixelFormat == GraphicsImagePixelFormat::RGB10Packed);
  assert(rgb10->image->bitAlignment == GraphicsImageBitAlignment::LeastSignificant);

  source.width = 4;
  source.stride = 8;
  source.pixel_format = 0x02100032;
  source.payload.resize(8);
  assert(oneGenGraphicsFrame(source, 11)->image->pixelFormat
         == GraphicsImagePixelFormat::YCbCr422_8);

  source.pixel_format = 0x010c0004;
  assert(!oneGenGraphicsFrame(source, 12));

  source.pixel_format = 0;
  assert(!oneGenGraphicsFrame(source, 13));

  source.payload_type = 5;
  source.width = 640;
  source.height = 480;
  source.stride = source.payload.size();
  assert(oneGenGraphicsFrame(source, 13)->image->pixelFormat == GraphicsImagePixelFormat::Jpeg);
  source.payload_type = 0;

  source.pixel_format = 0x01100007;
  source.width = 2;
  source.height = 1;
  source.stride = 4;
  source.payload.resize(4);
  source.pixel_endianness = 2;
  std::string rejectionReason;
  assert(!oneGenGraphicsFrame(source, 13, &rejectionReason));
  assert(rejectionReason == "big-endian multi-byte pixels are unsupported");
  source.pixel_endianness = 0;
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
  assert(scene->rangeFrame->xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
  assert(scene->rangeFrame->zValues[0] == 2.0F);
  assert(scene->rangeFrame->confidence == std::vector<float>({10.0F, 20.0F}));
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
  assert(c16Scene->rangeFrame->xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(c16Scene->rangeFrame->worldZAt(0) == 2.0);
  assert(c16Scene->rangeFrame->validMask == std::vector<std::uint8_t>({1, 0}));

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
  const auto& range = *floatScene->rangeFrame;
  assert(range.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(range.worldZAt(0) == -1 && range.worldZAt(1) == -2 && range.worldZAt(3) == 2);
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
  assert(singleRange && singleRange->rangeFrame->worldZAt(3) == 2);
  single.scan3d_geometry.reset();
  assert(!oneGenGraphicsFrame(single, 24));
  single.pixel_format = 0x012000c8;
  single.payload = floating.parts[1].payload;
  const auto confidencePreview = oneGenGraphicsFrame(single, 25);
  assert(confidencePreview && confidencePreview->hasRangeFrame());
  assert(confidencePreview->rangeFrame->rangeField.domain == MeasurementValueDomain::Native);
  assert(confidencePreview->rangeFrame->zValues[3] == 2);
  assert(!std::isfinite(confidencePreview->rangeFrame->xScale));
  assert(confidencePreview->rangeFrame->validMask == std::vector<std::uint8_t>({1, 1, 0, 1}));
  single.parts = {floating.parts[1]};
  assert(oneGenGraphicsFrame(single, 26)->rangeFrame->rangeField.displayName == "Confidence");
  single.pixel_endianness = 2;
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
    assert(paired->rangeFrame->intensity == std::vector<float>({0, 0, 0, 0}));
    assert(paired->rangeFrame->intensityField.bitsPerSample == (format == 0x01080001U ? 8 : 16));
  }
  floating.scan3d_geometry->invalid_enabled[2] = true;
  floating.scan3d_geometry->invalid_value[2] = -2;
  assert(oneGenGraphicsFrame(floating, 18)->rangeFrame->validMask[1] == 0);
  floating.parts[1].payload.pop_back();
  assert(oneGenGraphicsFrame(floating, 19)->rangeFrame->confidence.empty());
  floating.pixel_endianness = 2;
  assert(!oneGenGraphicsFrame(floating, 20));
  floating.pixel_endianness = 0;
  floating.parts[0].payload.pop_back();
  assert(!oneGenGraphicsFrame(floating, 21));
  floating.parts[0].payload.resize(24);
  floating.scan3d_geometry.reset();
  assert(!oneGenGraphicsFrame(floating, 22));

  multipart.scan3d_geometry->distance_unit.clear();
  assert(!oneGenGraphicsFrame(multipart, 16));
}
