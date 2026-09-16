#include "OneGenGraphicsFrameAdapter.h"

#include <limits>
#include <cmath>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kPayloadJpeg = 5;

bool isByteOrdered(const GraphicsImagePixelFormat format) noexcept
{
  switch (format) {
    case GraphicsImagePixelFormat::Mono8:
    case GraphicsImagePixelFormat::RGB24:
    case GraphicsImagePixelFormat::BGR24:
    case GraphicsImagePixelFormat::RGBA32:
    case GraphicsImagePixelFormat::BGRA32:
    case GraphicsImagePixelFormat::YCbCr422_8:
    case GraphicsImagePixelFormat::YCbCr422_8_CbYCrY:
    case GraphicsImagePixelFormat::BayerGR8:
    case GraphicsImagePixelFormat::BayerRG8:
    case GraphicsImagePixelFormat::BayerGB8:
    case GraphicsImagePixelFormat::BayerBG8:
    case GraphicsImagePixelFormat::BiColorRGBG8:
    case GraphicsImagePixelFormat::BiColorBGRG8:
    case GraphicsImagePixelFormat::Jpeg:
      return true;
    default:
      return false;
  }
}

GraphicsImagePixelFormat pixelFormat(std::uint64_t pfnc) noexcept
{
  switch (static_cast<std::uint32_t>(pfnc)) {
    case 0x01080001: return GraphicsImagePixelFormat::Mono8;
    case 0x01100003:
    case 0x01100005:
    case 0x01100025:
    case 0x01100007: return GraphicsImagePixelFormat::Mono16;
    case 0x010a0046: return GraphicsImagePixelFormat::Mono10Packed;
    case 0x010c0047: return GraphicsImagePixelFormat::Mono12Packed;
    case 0x02180014: return GraphicsImagePixelFormat::RGB24;
    case 0x02180015: return GraphicsImagePixelFormat::BGR24;
    case 0x02200016: return GraphicsImagePixelFormat::RGBA32;
    case 0x02200017: return GraphicsImagePixelFormat::BGRA32;
    case 0x02300018: return GraphicsImagePixelFormat::RGB10Packed;
    case 0x02300019: return GraphicsImagePixelFormat::BGR10Packed;
    case 0x0230001a: return GraphicsImagePixelFormat::RGB12Packed;
    case 0x0230001b: return GraphicsImagePixelFormat::BGR12Packed;
    case 0x02300033: return GraphicsImagePixelFormat::RGB48;
    case 0x0210001f: return GraphicsImagePixelFormat::YCbCr422_8_CbYCrY;
    case 0x02100032: return GraphicsImagePixelFormat::YCbCr422_8;
    case 0x01080008: return GraphicsImagePixelFormat::BayerGR8;
    case 0x01080009: return GraphicsImagePixelFormat::BayerRG8;
    case 0x0108000a: return GraphicsImagePixelFormat::BayerGB8;
    case 0x0108000b: return GraphicsImagePixelFormat::BayerBG8;
    case 0x0110000c: return GraphicsImagePixelFormat::BayerGR10;
    case 0x0110000d: return GraphicsImagePixelFormat::BayerRG10;
    case 0x0110000e: return GraphicsImagePixelFormat::BayerGB10;
    case 0x0110000f: return GraphicsImagePixelFormat::BayerBG10;
    case 0x01100010: return GraphicsImagePixelFormat::BayerGR12;
    case 0x01100011: return GraphicsImagePixelFormat::BayerRG12;
    case 0x01100012: return GraphicsImagePixelFormat::BayerGB12;
    case 0x01100013: return GraphicsImagePixelFormat::BayerBG12;
    case 0x0110002a: return GraphicsImagePixelFormat::BayerGR14;
    case 0x0110002b: return GraphicsImagePixelFormat::BayerRG14;
    case 0x0110002c: return GraphicsImagePixelFormat::BayerGB14;
    case 0x0110002d: return GraphicsImagePixelFormat::BayerBG14;
    case 0x0110002e: return GraphicsImagePixelFormat::BayerGR16;
    case 0x0110002f: return GraphicsImagePixelFormat::BayerRG16;
    case 0x01100030: return GraphicsImagePixelFormat::BayerGB16;
    case 0x01100031: return GraphicsImagePixelFormat::BayerBG16;
    case 0x010a0056: return GraphicsImagePixelFormat::BayerGR10;
    case 0x010a0058: return GraphicsImagePixelFormat::BayerRG10;
    case 0x010a0054: return GraphicsImagePixelFormat::BayerGB10;
    case 0x010a0052: return GraphicsImagePixelFormat::BayerBG10;
    case 0x010c0057: return GraphicsImagePixelFormat::BayerGR12;
    case 0x010c0059: return GraphicsImagePixelFormat::BayerRG12;
    case 0x010c0055: return GraphicsImagePixelFormat::BayerGB12;
    case 0x010c0053: return GraphicsImagePixelFormat::BayerBG12;
    case 0x021000a5: return GraphicsImagePixelFormat::BiColorRGBG8;
    case 0x021000a6: return GraphicsImagePixelFormat::BiColorBGRG8;
    case 0x022000a7:
    case 0x021400a8: return GraphicsImagePixelFormat::BiColorRGBG10;
    case 0x022000a9:
    case 0x021400aa: return GraphicsImagePixelFormat::BiColorBGRG10;
    case 0x022000ab:
    case 0x021800ac: return GraphicsImagePixelFormat::BiColorRGBG12;
    case 0x022000ad:
    case 0x021800ae: return GraphicsImagePixelFormat::BiColorBGRG12;
    default: return GraphicsImagePixelFormat::Unknown;
  }
}

std::uint8_t monoBits(std::uint64_t pfnc) noexcept
{
  switch (static_cast<std::uint32_t>(pfnc)) {
    case 0x01100003:
    case 0x010a0046: return 10;
    case 0x01100005:
    case 0x010c0047: return 12;
    case 0x01100025: return 14;
    case 0x01100007: return 16;
    default: return 0;
  }
}

constexpr std::uint64_t kPart2dImage = 1;
constexpr std::uint64_t kPart3dImage = 5;
constexpr std::uint64_t kPartConfidence = 9;
constexpr std::uint64_t kCoord3dC16 = 0x011000b8;
constexpr std::uint64_t kCoord3dAbc32f = 0x026000c0;
constexpr std::uint64_t kConfidence8 = 0x010800c6;
constexpr std::uint64_t kConfidence16 = 0x011000c7;

std::string lower(std::string value)
{
  for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

onegen::Frame partFrame(const onegen::Frame::Part& part, const onegen::Frame& source)
{
  onegen::Frame frame;
  frame.payload = part.payload;
  frame.width = part.width;
  frame.height = part.height;
  frame.stride = part.stride;
  frame.pixel_format = part.data_format;
  frame.format_namespace = part.format_namespace;
  frame.pixel_endianness = source.pixel_endianness;
  frame.frame_id = source.frame_id;
  frame.timestamp = source.timestamp;
  frame.timestamp_ns = source.timestamp_ns;
  return frame;
}

template <typename T>
bool copyScalarPart(const onegen::Frame::Part& part, std::vector<float>& values,
                    std::vector<std::uint16_t>* raw = nullptr)
{
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  const auto rowBytes = static_cast<std::size_t>(part.width) * sizeof(T);
  if (!count || part.stride < rowBytes || part.payload.size() < part.stride * part.height)
    return false;
  values.resize(count);
  if (raw) raw->resize(count);
  for (std::uint32_t y = 0; y < part.height; ++y) {
    const auto* row = part.payload.data() + static_cast<std::size_t>(y) * part.stride;
    for (std::uint32_t x = 0; x < part.width; ++x) {
      T sample{};
      std::memcpy(&sample, row + static_cast<std::size_t>(x) * sizeof(T), sizeof(T));
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      values[index] = static_cast<float>(sample);
      if (raw) (*raw)[index] = static_cast<std::uint16_t>(sample);
    }
  }
  return true;
}

bool sameShape(const onegen::Frame::Part& part, const RangeFrame& range) noexcept
{
  return part.width == static_cast<std::uint32_t>(range.width)
      && part.height == static_cast<std::uint32_t>(range.height);
}

bool appendAuxiliary(const onegen::Frame::Part& part, RangeFrame& range)
{
  if (!sameShape(part, range)) return false;
  const bool confidence = part.data_type == kPartConfidence
      || part.data_format == kConfidence8 || part.data_format == kConfidence16;
  if (!confidence && (part.data_format & 0xff000000U) != 0x01000000U) return false;
  auto& values = confidence ? range.confidence : range.intensity;
  auto& raw = confidence ? range.confidenceRaw : range.intensityRaw;
  const auto bits = static_cast<std::uint8_t>((part.data_format >> 16U) & 0xffU);
  const bool copied = bits <= 8 ? copyScalarPart<std::uint8_t>(part, values)
                                : copyScalarPart<std::uint16_t>(part, values, &raw);
  if (!copied) return false;
  (confidence ? range.confidenceBits : range.intensityBits) = bits;
  return true;
}

std::optional<RangeFrame> rangeFrame(const onegen::Frame::Part& part,
                                     const onegen::Frame& source)
{
  if (part.data_type != kPart3dImage || !part.width || !part.height) return std::nullopt;
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  if (part.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || part.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return std::nullopt;

  RangeFrame range;
  range.width = static_cast<int>(part.width);
  range.height = static_cast<int>(part.height);
  range.sensorType = "GenTL 3D";
  range.frameId = source.frame_id ? std::to_string(source.frame_id) : std::string{};
  range.validMask.resize(count);
  if (!source.scan3d_geometry) return std::nullopt;
  const auto& geometry = *source.scan3d_geometry;
  const auto unit = lower(geometry.distance_unit);
  if (unit == "millimeter" || unit == "mm")
    range.lengthUnit = GraphicsLengthUnit::Millimeter;
  else if (unit == "micrometer" || unit == "um")
    range.lengthUnit = GraphicsLengthUnit::Micrometer;
  else if (unit == "nanometer" || unit == "nm")
    range.lengthUnit = GraphicsLengthUnit::Nanometer;
  else
    return std::nullopt;
  if (part.data_format == kCoord3dAbc32f) {
    const auto rowBytes = static_cast<std::size_t>(part.width) * 3U * sizeof(float);
    if (part.stride < rowBytes || part.payload.size() < part.stride * part.height)
      return std::nullopt;
    range.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    range.xValues.resize(count);
    range.yValues.resize(count);
    range.zValues.resize(count);
    for (std::uint32_t y = 0; y < part.height; ++y) {
      const auto* row = part.payload.data() + static_cast<std::size_t>(y) * part.stride;
      for (std::uint32_t x = 0; x < part.width; ++x) {
        float xyz[3]{};
        std::memcpy(xyz, row + static_cast<std::size_t>(x) * sizeof(xyz), sizeof(xyz));
        const auto index = static_cast<std::size_t>(y) * part.width + x;
        range.xValues[index] = static_cast<float>(xyz[0] * geometry.scale[0] + geometry.offset[0]);
        range.yValues[index] = static_cast<float>(xyz[1] * geometry.scale[1] + geometry.offset[1]);
        range.zValues[index] = static_cast<float>(xyz[2] * geometry.scale[2] + geometry.offset[2]);
        const bool invalidCode = (geometry.invalid_enabled[0] && xyz[0] == geometry.invalid_value[0])
            || (geometry.invalid_enabled[1] && xyz[1] == geometry.invalid_value[1])
            || (geometry.invalid_enabled[2] && xyz[2] == geometry.invalid_value[2]);
        range.validMask[index] = !invalidCode && std::isfinite(range.xValues[index])
            && std::isfinite(range.yValues[index]) && std::isfinite(range.zValues[index]) ? 1U : 0U;
      }
    }
    range.rangeBits = 32;
    range.rangeField = {"gentl.xyz.z", "Z coordinate", "",
                        MeasurementValueDomain::Calibrated,
                        MeasurementSampleKind::GridSample, 32};
  } else if (part.data_format == kCoord3dC16) {
    if (!copyScalarPart<std::uint16_t>(part, range.zValues, &range.rangeRaw))
      return std::nullopt;
    range.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
    range.zScale = geometry.scale[2];
    range.zOffset = geometry.offset[2];
    range.rangeBits = 16;
    range.rangeField = {"gentl.coord3d-c", "Coordinate C", "",
                        MeasurementValueDomain::Calibrated,
                        MeasurementSampleKind::GridSample, 16};
    for (std::size_t index = 0; index < count; ++index)
      range.validMask[index] = geometry.invalid_enabled[2]
          ? (range.rangeRaw[index] != geometry.invalid_value[2] ? 1U : 0U)
          : (range.rangeRaw[index] ? 1U : 0U);
  } else {
    return std::nullopt;
  }
  return range.isValid() ? std::optional<RangeFrame>(std::move(range)) : std::nullopt;
}

} // namespace

std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, const std::uint64_t sequence,
    std::string* rejection_reason)
{
  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsFrame>{};
  };
  if (!source.parts.empty()) {
    GraphicsFrame result;
    for (const auto& part : source.parts) {
      if (!result.rangeFrame) result.rangeFrame = rangeFrame(part, source);
      if (!result.image && part.data_type == kPart2dImage) {
        auto imageFrame = oneGenGraphicsFrame(partFrame(part, source), sequence, rejection_reason);
        if (imageFrame) result.image = std::move(imageFrame->image);
      }
    }
    if (result.rangeFrame) {
      for (const auto& part : source.parts)
        if (part.data_type == kPartConfidence || part.data_type == kPart2dImage)
          appendAuxiliary(part, *result.rangeFrame);
    }
    result.metadata.frameIndex = sequence;
    if (source.frame_id) result.metadata.frameId = std::to_string(source.frame_id);
    return result.isValid() ? std::optional<GraphicsFrame>(std::move(result))
                            : reject("multipart payload has no supported image or range data");
  }
  const auto format = source.payload_type == kPayloadJpeg
      ? GraphicsImagePixelFormat::Jpeg : pixelFormat(source.pixel_format);
  if (format == GraphicsImagePixelFormat::Unknown) return reject("unknown PFNC pixel format");
  if (source.payload.empty()) return reject("empty payload");
  if (source.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || source.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return reject("image dimensions exceed the supported integer range");

  if (source.pixel_endianness == 2 && !isByteOrdered(format))
    return reject("big-endian multi-byte pixels are unsupported");

  const auto minimum = GraphicsImageValidation::minimumRowBytes(
      format, static_cast<int>(source.width));
  const auto stride = source.stride ? source.stride : minimum;
  if (stride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return reject("stride exceeds the supported integer range");

  auto storage = std::make_shared<std::vector<std::uint8_t>>(source.payload);
  GraphicsImage image;
  image.storage = std::shared_ptr<const std::uint8_t>(storage, storage->data());
  image.size = storage->size();
  image.width = static_cast<int>(source.width);
  image.height = static_cast<int>(source.height);
  image.stride = static_cast<int>(stride);
  image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(format);
  image.declaredMonoSourceBits = monoBits(source.pixel_format);
  image.sourceFormat = static_cast<int>(source.pixel_format);
  image.pixelFormat = format;
  const auto effectiveBits = static_cast<int>((source.pixel_format >> 16U) & 0xffU);
  image.bitAlignment = effectiveBits > image.bitsPerPixel
      || (format == GraphicsImagePixelFormat::Mono16
          && image.declaredMonoSourceBits > 0 && image.declaredMonoSourceBits < 16)
      ? GraphicsImageBitAlignment::LeastSignificant : GraphicsImageBitAlignment::Packed;
  image.frameSequence = sequence;
  if (!image.isValid()) return reject("invalid image layout or insufficient payload bytes");

  GraphicsFrame frame;
  frame.setImage(std::move(image));
  frame.metadata.frameIndex = sequence;
  if (source.frame_id) frame.metadata.frameId = std::to_string(source.frame_id);
  return frame;
}
