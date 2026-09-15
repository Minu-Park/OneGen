#include "OneGenGraphicsFrameAdapter.h"

#include <cassert>

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

  multipart.scan3d_geometry->distance_unit.clear();
  assert(!oneGenGraphicsFrame(multipart, 16));
}
