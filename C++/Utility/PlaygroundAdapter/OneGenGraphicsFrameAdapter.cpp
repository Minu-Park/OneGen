#include "OneGenGraphicsFrameAdapter.h"
#include "Internal/CoordinateFormat.h"
#include <algorithm>
#include <array>
#include <bit>

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
constexpr std::uint64_t kConfidence1 = 0x010800c4;
constexpr std::uint64_t kConfidence1p = 0x010100c5;
constexpr std::uint64_t kConfidence8 = 0x010800c6;
constexpr std::uint64_t kConfidence16 = 0x011000c7;
constexpr std::uint64_t kConfidence32f = 0x012000c8;

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
  frame.scan3d_geometry = source.scan3d_geometry;
  return frame;
}

template <typename T>
T readSample(const std::uint8_t* bytes, const std::uint64_t endianness)
{
  std::array<std::uint8_t, sizeof(T)> storage{};
  std::memcpy(storage.data(), bytes, sizeof(T));
  const bool big = endianness == 2;
  if (sizeof(T) > 1 && endianness != 0 && big != (std::endian::native == std::endian::big))
    std::reverse(storage.begin(), storage.end());
  T value{};
  std::memcpy(&value, storage.data(), sizeof(T));
  return value;
}

double readCoordinate(const onegen::Frame::Part& part, const onegen::CoordinateFormat& layout,
                      const std::size_t stride, const std::size_t plane_bytes,
                      const std::uint32_t x, const std::uint32_t y, const unsigned component,
                      const std::uint64_t endianness)
{
  if (layout.packed) {
    const auto comps = layout.planar ? 1U : layout.components;
    const auto* plane = part.payload.data() + (layout.planar ? component * plane_bytes : 0);
    const auto bit = static_cast<std::uint64_t>(y) * stride * 8U
        + (static_cast<std::uint64_t>(x) * comps + (layout.planar ? 0U : component)) * layout.sample_bits;
    return static_cast<double>(onegen::packed_bits(plane, layout.planar ? plane_bytes : part.payload.size(),
                                                   bit, layout.sample_bits));
  }
  const auto sample_bytes = layout.sample_bits / 8U;
  const auto address = layout.planar
      ? component * plane_bytes + static_cast<std::size_t>(y) * stride + x * sample_bytes
      : static_cast<std::size_t>(y) * stride
          + (static_cast<std::size_t>(x) * layout.components + component) * sample_bytes;
  const auto* bytes = part.payload.data() + address;
  if (layout.floating)
    return layout.sample_bits == 64 ? readSample<double>(bytes, endianness)
                                    : readSample<float>(bytes, endianness);
  if (sample_bytes == 1) return *bytes;
  return readSample<std::uint16_t>(bytes, endianness);
}

template <typename T>
bool copyScalarPart(const onegen::Frame::Part& part, std::vector<float>& values,
                    std::vector<std::uint16_t>* raw = nullptr, std::uint64_t endianness = 0)
{
  if (!part.width || !part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return false;
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  const auto rowBytes = static_cast<std::size_t>(part.width) * sizeof(T);
  if (part.stride < rowBytes || part.height > part.payload.size() / part.stride)
    return false;
  values.resize(count);
  if (raw) raw->resize(count);
  for (std::uint32_t y = 0; y < part.height; ++y) {
    const auto* row = part.payload.data() + static_cast<std::size_t>(y) * part.stride;
    for (std::uint32_t x = 0; x < part.width; ++x) {
      const auto sample = readSample<T>(row + static_cast<std::size_t>(x) * sizeof(T), endianness);
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

bool appendAuxiliary(const onegen::Frame::Part& part, RangeFrame& range,
                     const std::uint64_t endianness)
{
  if (!sameShape(part, range)) return false;
  const bool confidence = part.data_type == kPartConfidence
      || part.data_format == kConfidence1 || part.data_format == kConfidence1p
      || part.data_format == kConfidence8 || part.data_format == kConfidence16
      || part.data_format == kConfidence32f;
  const auto format = pixelFormat(part.data_format);
  if (!confidence && format != GraphicsImagePixelFormat::Mono8
      && format != GraphicsImagePixelFormat::Mono16) return false;
  const auto bits = static_cast<std::uint8_t>((part.data_format >> 16U) & 0xffU);
  if (endianness > 2) return false;
  auto& values = confidence ? range.confidence : range.intensity;
  auto& raw = confidence ? range.confidenceRaw : range.intensityRaw;
  bool copied = false;
  if (part.data_format == kConfidence32f) {
    copied = copyScalarPart<float>(part, values, nullptr, endianness);
    if (copied) {
      raw.clear(); // Floating samples have no lossless uint16 representation.
      range.confidenceValidMask.resize(values.size());
      for (std::size_t index = 0; index < values.size(); ++index)
        range.confidenceValidMask[index] = std::isfinite(values[index]) ? 1U : 0U;
    }
  } else if (part.data_format == kConfidence1 || part.data_format == kConfidence1p) {
    const auto count = static_cast<std::size_t>(part.width) * part.height;
    if (!count || part.width > (std::numeric_limits<std::size_t>::max)() / part.height)
      return false;
    values.resize(count);
    if (part.data_format == kConfidence1) {
      copied = copyScalarPart<std::uint8_t>(part, values);
      if (copied) for (auto& value : values) value = (static_cast<std::uint8_t>(value) & 1U) ? 1.F : 0.F;
    } else {
      const auto row_bytes = onegen::coordinate_row_bytes({1, 1, false, false, true}, part.width);
      const auto stride = part.stride ? part.stride : row_bytes;
      if (!row_bytes || stride < row_bytes || part.height > part.payload.size() / stride)
        return false;
      for (std::uint32_t y = 0; y < part.height; ++y)
        for (std::uint32_t x = 0; x < part.width; ++x)
          values[static_cast<std::size_t>(y) * part.width + x] =
              onegen::packed_bits(part.payload.data() + static_cast<std::size_t>(y) * stride, stride,
                                  static_cast<std::uint64_t>(x), 1) ? 1.F : 0.F;
      copied = true;
    }
  } else if (bits == 8) {
    copied = copyScalarPart<std::uint8_t>(part, values);
  } else if (bits == 16) {
    copied = copyScalarPart<std::uint16_t>(part, values, &raw, endianness);
  }
  if (!copied) return false;
  const auto stored_bits = (part.data_format == kConfidence1 || part.data_format == kConfidence1p)
      ? 1 : bits;
  (confidence ? range.confidenceBits : range.intensityBits) = stored_bits;
  (confidence ? range.confidenceField : range.intensityField).bitsPerSample = stored_bits;
  return true;
}

std::optional<RangeFrame> rangeFrame(const onegen::Frame::Part& part,
                                     const onegen::Frame& source)
{
  if (part.data_type != kPart3dImage || !part.width || !part.height) return std::nullopt;
  const auto layout = onegen::coordinate_format(part.data_format);
  if (!layout.components || source.pixel_endianness > 2
      || part.width > (std::numeric_limits<std::size_t>::max)() / part.height)
    return std::nullopt;
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  if (part.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || part.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return std::nullopt;
  if (!source.scan3d_geometry) return std::nullopt;
  const auto& geometry = *source.scan3d_geometry;
  const auto mode = geometry.output_mode;
  const bool native = mode == "UncalibratedC";
  const auto system = geometry.coordinate_system;
  if (layout.components == 1) {
    if (!mode.empty() && mode != "CalibratedC" && mode != "RectifiedC"
        && mode != "DisparityC" && !native)
      return std::nullopt;
  } else if (layout.components == 2) {
    if (mode != "CalibratedAC" && mode != "RectifiedAC") return std::nullopt;
  } else if (!mode.empty() && mode != "CalibratedABC_Grid" && mode != "CalibratedABC_PointCloud") {
    return std::nullopt;
  }
  if (!native && !system.empty() && system != "Cartesian" && system != "Spherical"
      && system != "Cylindrical")
    return std::nullopt;
  if ((system == "Spherical" || system == "Cylindrical") && layout.components == 1
      && mode != "RectifiedC")
    return std::nullopt;
  if (mode == "DisparityC" && (!std::isfinite(geometry.focal_length)
      || !std::isfinite(geometry.baseline) || geometry.focal_length == 0 || geometry.baseline == 0))
    return std::nullopt;
  const bool full_axes = layout.components > 1 || mode == "RectifiedC";
  for (std::size_t axis = full_axes ? 0 : 2; axis < 3; ++axis)
    if (!native && (!geometry.calibrated_axes[axis] || !std::isfinite(geometry.scale[axis])
        || !std::isfinite(geometry.offset[axis]))) return std::nullopt;
  RangeFrame range;
  range.width = static_cast<int>(part.width);
  range.height = static_cast<int>(part.height);
  range.sensorType = native ? "GenTL 3D (native scalar preview)" : "GenTL 3D";
  range.frameId = source.frame_id ? std::to_string(source.frame_id) : std::string{};
  const auto unit = lower(geometry.distance_unit);
  double unit_scale = 1;
  if (!native) {
    if (unit == "millimeter" || unit == "mm") range.lengthUnit = GraphicsLengthUnit::Millimeter;
    else if (unit == "micrometer" || unit == "um") range.lengthUnit = GraphicsLengthUnit::Micrometer;
    else if (unit == "nanometer" || unit == "nm") range.lengthUnit = GraphicsLengthUnit::Nanometer;
    else if (unit == "meter" || unit == "m") { range.lengthUnit = GraphicsLengthUnit::Millimeter; unit_scale = 1000; }
    else if (unit == "inch" || unit == "in") { range.lengthUnit = GraphicsLengthUnit::Millimeter; unit_scale = 25.4; }
    else return std::nullopt;
  }
  const auto row_bytes = onegen::coordinate_row_bytes(layout, part.width);
  if (!row_bytes) return std::nullopt;
  const auto stride = part.stride ? part.stride : row_bytes;
  const auto planes = layout.planar ? layout.components : 1U;
  if (stride < row_bytes || part.height > part.payload.size() / stride / planes)
    return std::nullopt;
  const auto plane_bytes = stride * part.height;
  range.zValues.resize(count);
  range.validMask.resize(count);
  range.rangeBits = static_cast<std::uint8_t>(layout.sample_bits);
  range.rangeField = {"gentl.coord3d-c", native ? "Coordinate C (uncalibrated)" : "Coordinate C", "",
      native ? MeasurementValueDomain::Native : MeasurementValueDomain::Calibrated,
      MeasurementSampleKind::GridSample, range.rangeBits};
  if (layout.components > 1) {
    range.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    range.xValues.resize(count);
    range.yValues.resize(count);
  } else {
    range.zScale = native ? 1 : geometry.scale[2];
    range.zOffset = native ? 0 : geometry.offset[2];
    if (!layout.floating && layout.sample_bits <= 16) range.rangeRaw.resize(count);
    if (mode == "RectifiedC") {
      range.xScale = geometry.scale[0]; range.xOffset = geometry.offset[0];
      range.yScale = geometry.scale[1]; range.yOffset = geometry.offset[1];
    } else {
      range.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
      range.xScale = range.yScale = std::numeric_limits<double>::quiet_NaN();
      range.xOffset = range.yOffset = std::numeric_limits<double>::quiet_NaN();
    }
  }
  for (std::uint32_t y = 0; y < part.height; ++y) {
    for (std::uint32_t x = 0; x < part.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      bool valid = true;
      for (unsigned component = 0; component < layout.components; ++component) {
        const auto axis = layout.components == 3 ? component
            : (component == layout.components - 1 ? 2U : 0U);
        const double sample = readCoordinate(part, layout, stride, plane_bytes, x, y, component,
                                             source.pixel_endianness);
        const double world = native ? sample : sample * geometry.scale[axis] + geometry.offset[axis];
        const auto stored = static_cast<float>(layout.components == 1 ? sample : world);
        valid = valid && std::isfinite(sample) && std::isfinite(world)
            && std::isfinite(static_cast<float>(world)) && std::isfinite(stored)
            && !(geometry.invalid_enabled[axis] && sample == geometry.invalid_value[axis]);
        if (axis == 0) range.xValues[index] = stored;
        else if (axis == 1) range.yValues[index] = stored;
        else {
          range.zValues[index] = stored;
          if (!range.rangeRaw.empty()) range.rangeRaw[index] = static_cast<std::uint16_t>(sample);
        }
      }
      if (layout.components == 2) {
        range.yValues[index] = static_cast<float>(y * geometry.scale[1] + geometry.offset[1]);
        valid = valid && std::isfinite(range.yValues[index]);
      }
      range.validMask[index] = valid ? 1U : 0U;
    }
  }
  const auto finite_store = [](double value, float& stored) {
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value))) return false;
    stored = static_cast<float>(value);
    return true;
  };
  if (mode == "DisparityC") {
    const auto scale = range.zScale;
    const auto offset = range.zOffset;
    range.zScale = 1;
    range.zOffset = 0;
    range.rangeRaw.clear();
    const bool project = std::isfinite(geometry.principal_u) && std::isfinite(geometry.principal_v);
    if (project) {
      range.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
      range.xValues.resize(count);
      range.yValues.resize(count);
    }
    for (std::uint32_t y = 0; y < part.height; ++y) {
      for (std::uint32_t x = 0; x < part.width; ++x) {
        const auto index = static_cast<std::size_t>(y) * part.width + x;
        if (!range.validMask[index]) continue;
        const double disparity = offset + static_cast<double>(range.zValues[index]) * scale;
        if (disparity == 0 || !std::isfinite(disparity)) { range.validMask[index] = 0; continue; }
        const double z = geometry.baseline * geometry.focal_length / disparity;
        if (!finite_store(z, range.zValues[index])) { range.validMask[index] = 0; continue; }
        if (project) {
          if (!finite_store((static_cast<double>(x) - geometry.principal_u) * z / geometry.focal_length,
                            range.xValues[index])
              || !finite_store((static_cast<double>(y) - geometry.principal_v) * z / geometry.focal_length,
                               range.yValues[index]))
            range.validMask[index] = 0;
        }
      }
    }
  } else if (system == "Spherical" || system == "Cylindrical") {
    range.rangeRaw.clear();
    range.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    range.xValues.resize(count);
    range.yValues.resize(count);
    range.zScale = 1;
    range.zOffset = 0;
    for (std::uint32_t y = 0; y < part.height; ++y) {
      for (std::uint32_t x = 0; x < part.width; ++x) {
        const auto index = static_cast<std::size_t>(y) * part.width + x;
        if (!range.validMask[index]) continue;
        double theta = 0, second = 0, rho = 0;
        if (layout.components >= 2) {
          theta = range.xValues[index]; second = range.yValues[index]; rho = range.zValues[index];
        } else {
          theta = x * geometry.scale[0] + geometry.offset[0];
          second = y * geometry.scale[1] + geometry.offset[1];
          rho = geometry.offset[2] + static_cast<double>(range.zValues[index]) * geometry.scale[2];
        }
        double wx = 0, wy = 0, wz = 0;
        if (system == "Spherical") {
          wx = rho * std::sin(second) * std::cos(theta);
          wy = rho * std::sin(second) * std::sin(theta);
          wz = rho * std::cos(second);
        } else {
          wx = rho * std::cos(theta);
          wy = second;
          wz = rho * std::sin(theta);
        }
        float stored_x = 0, stored_y = 0, stored_z = 0;
        if (!finite_store(wx, stored_x) || !finite_store(wy, stored_y) || !finite_store(wz, stored_z))
          range.validMask[index] = 0;
        else {
          range.xValues[index] = stored_x;
          range.yValues[index] = stored_y;
          range.zValues[index] = stored_z;
        }
      }
    }
  }
  if (unit_scale != 1) {
    range.zScale *= unit_scale;
    range.zOffset *= unit_scale;
    if (range.xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid) {
      range.xScale *= unit_scale;
      range.xOffset *= unit_scale;
      range.yScale *= unit_scale;
      range.yOffset *= unit_scale;
    for (auto& value : range.xValues) value = static_cast<float>(value * unit_scale);
    for (auto& value : range.yValues) value = static_cast<float>(value * unit_scale);
    for (std::size_t index = 0; index < range.validMask.size(); ++index) {
      if (!range.validMask[index]) continue;
      if ((!range.xValues.empty() && !std::isfinite(range.xValues[index]))
          || (!range.yValues.empty() && !std::isfinite(range.yValues[index])))
        range.validMask[index] = 0;
    }
    }
  }
  return range.isValid() ? std::optional<RangeFrame>(std::move(range)) : std::nullopt;
}

void presentPointCloud(GraphicsFrame& frame, const onegen::Frame& source)
{
  if (!frame.rangeFrame || !source.scan3d_geometry
      || source.scan3d_geometry->output_mode != "CalibratedABC_PointCloud") return;
  const auto& range = *frame.rangeFrame;
  if (range.xyCoordinateMode != RangeFrameXYCoordinateMode::ExplicitXY
      || range.xValues.size() != range.zValues.size() || range.yValues.size() != range.zValues.size())
    return;
  PointCloudData cloud;
  cloud.coordinateUnit = range.lengthUnit;
  ScalarField intensity, confidence;
  intensity.name = range.intensityField.displayName;
  intensity.descriptor = range.intensityField;
  confidence.name = range.confidenceField.displayName;
  confidence.descriptor = range.confidenceField;
  intensity.descriptor.sampleKind = confidence.descriptor.sampleKind = MeasurementSampleKind::Point;
  const auto unit = range.lengthUnit;
  for (std::size_t i = 0; i < range.zValues.size(); ++i) {
    if (!range.rangeSampleIsValid(i)) continue;
    const int x = range.width > 0 ? static_cast<int>(i % static_cast<std::size_t>(range.width)) : 0;
    const int y = range.width > 0 ? static_cast<int>(i / static_cast<std::size_t>(range.width)) : 0;
    const auto wx = range.physicalXAt(i, x, unit);
    const auto wy = range.physicalYAt(i, y, unit);
    const auto wz = range.physicalZAt(i, unit);
    if (!std::isfinite(wx) || !std::isfinite(wy) || !std::isfinite(wz)) continue;
    cloud.xyz.insert(cloud.xyz.end(), {wx, wy, wz});
    if (!range.intensity.empty()) intensity.values.push_back(range.intensitySampleIsValid(i)
        ? range.intensity[i] : std::numeric_limits<float>::quiet_NaN());
    if (!range.confidence.empty()) confidence.values.push_back(range.confidenceSampleIsValid(i)
        ? range.confidence[i] : std::numeric_limits<float>::quiet_NaN());
  }
  if (!intensity.values.empty()) cloud.scalarFields.push_back(std::move(intensity));
  if (!confidence.values.empty()) cloud.scalarFields.push_back(std::move(confidence));
  frame.rangeFrame.reset(); // Unorganized points must not acquire an invented grid surface.
  if (cloud.isValid()) frame.pointCloud = std::move(cloud);
}

std::optional<RangeFrame> planarPartsRange(
    const onegen::Frame& source, const onegen::Frame::Part*& selected)
{
  for (const auto& a : source.parts) {
    if (a.data_type != kPart3dImage) continue;
    std::array<std::uint64_t, 3> formats{};
    switch (a.data_format) {
      case 0x010800af: formats = {0x010800b0, 0x010800b1, 0x021800b3}; break;
      case 0x011000b6: formats = {0x011000b7, 0x011000b8, 0x023000ba}; break;
      case 0x012000bd: formats = {0x012000be, 0x012000bf, 0x026000c1}; break;
      case 0x01400126: formats = {0x01400127, 0x01400128, 0x02c0012a}; break;
      case 0x010a00d5: formats = {0x010a00d6, 0x010a00d7, 0x021e00dc}; break;
      case 0x010c00d8: formats = {0x010c00d9, 0x010c00da, 0x022400df}; break;
      default: continue;
    }
    std::array<const onegen::Frame::Part*, 3> planes{};
    bool ambiguous = false;
    for (const auto& part : source.parts) {
      if (part.data_type != kPart3dImage || part.source_id != a.source_id
          || part.region_id != a.region_id || part.format_namespace != a.format_namespace
          || part.width != a.width || part.height != a.height) continue;
      const auto axis = part.data_format == a.data_format ? 0
          : (part.data_format == formats[0] ? 1 : (part.data_format == formats[1] ? 2 : -1));
      if (axis < 0) continue;
      if (planes[axis]) ambiguous = true;
      planes[axis] = &part;
    }
    if (ambiguous || !planes[0] || !planes[1] || !planes[2]) continue;
    const auto layout = onegen::coordinate_format(formats[2]);
    const auto row_bytes = onegen::coordinate_row_bytes(
        {1, layout.sample_bits, false, layout.floating, layout.packed}, a.width);
    if (!row_bytes || !a.height) continue;
    onegen::Frame::Part joined;
    joined.width = a.width;
    joined.height = a.height;
    joined.data_type = kPart3dImage;
    joined.stride = row_bytes;
    joined.data_format = formats[2];
    bool valid = true;
    for (const auto* plane : planes) {
      const auto stride = plane->stride ? plane->stride : row_bytes;
      if (stride < row_bytes || plane->height > plane->payload.size() / stride) {
        valid = false;
        break;
      }
      for (std::uint32_t row = 0; row < plane->height; ++row) {
        const auto first = plane->payload.begin() + static_cast<std::size_t>(row) * stride;
        joined.payload.insert(joined.payload.end(), first, first + row_bytes);
      }
    }
    if (valid) {
      auto range = rangeFrame(joined, source);
      if (range) { selected = &a; return range; }
    }
  }
  return std::nullopt;
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
    const onegen::Frame::Part* selected = nullptr;
    result.rangeFrame = planarPartsRange(source, selected);
    for (const auto& part : source.parts) {
      if (!result.rangeFrame) {
        result.rangeFrame = rangeFrame(part, source);
        if (result.rangeFrame) selected = &part;
      }
      if (!result.image && part.data_type == kPart2dImage) {
        auto imageFrame = oneGenGraphicsFrame(partFrame(part, source), sequence, rejection_reason);
        if (imageFrame) result.image = std::move(imageFrame->image);
      }
    }
    if (result.rangeFrame) {
      for (const auto& part : source.parts)
        if ((part.data_type == kPartConfidence || part.data_type == kPart2dImage)
            && selected && part.source_id == selected->source_id && part.region_id == selected->region_id)
          appendAuxiliary(part, *result.rangeFrame, source.pixel_endianness);
    }
    if (!result.rangeFrame && !result.image) {
      for (const auto& part : source.parts) {
        if (part.data_format != kConfidence32f) continue;
        auto preview = oneGenGraphicsFrame(partFrame(part, source), sequence, rejection_reason);
        if (preview) {
          result.rangeFrame = std::move(preview->rangeFrame);
          break;
        }
      }
    }
    result.metadata.frameIndex = sequence;
    if (source.frame_id) result.metadata.frameId = std::to_string(source.frame_id);
    presentPointCloud(result, source);
    return result.isValid() ? std::optional<GraphicsFrame>(std::move(result))
                            : reject("multipart payload has no supported image or range data");
  }
  if (source.payload_type != 0 && source.payload_type != 1
      && source.payload_type != 4 && source.payload_type != kPayloadJpeg)
    return reject("payload type does not describe a supported image");
  if (source.payload_type == 4 && !source.buffer_info.image_present)
    return reject("chunk payload contains no image");
  const auto coordinateLayout = onegen::coordinate_format(source.pixel_format);
  if (coordinateLayout.components || source.pixel_format == kConfidence32f) {
    const auto row_bytes = coordinateLayout.components
        ? onegen::coordinate_row_bytes(coordinateLayout, source.width)
        : (source.width > (std::numeric_limits<std::size_t>::max)() / 4U
            ? 0 : static_cast<std::size_t>(source.width) * 4U);
    if (!row_bytes)
      return reject("coordinate row size exceeds the supported integer range");
    onegen::Frame::Part part;
    part.payload = source.payload;
    part.width = source.width;
    part.height = source.height;
    part.stride = source.stride ? source.stride : row_bytes;
    part.data_format = source.pixel_format;
    part.data_type = kPart3dImage;
    GraphicsFrame frame;
    if (source.pixel_format == kConfidence32f) {
      // Reuse the native scalar preview contract; confidence is not depth.
      RangeFrame preview;
      if (source.pixel_endianness > 2
          || source.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
          || source.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
          || !copyScalarPart<float>(part, preview.zValues, nullptr, source.pixel_endianness))
        return reject("invalid floating confidence layout or byte order");
      preview.width = static_cast<int>(part.width);
      preview.height = static_cast<int>(part.height);
      preview.xScale = preview.yScale = std::numeric_limits<double>::quiet_NaN();
      preview.rangeBits = 32;
      preview.rangeField = {"gentl.confidence", "Confidence", "",
          MeasurementValueDomain::Native, MeasurementSampleKind::GridSample, 32};
      preview.sensorType = "GenTL confidence (native scalar preview)";
      preview.validMask.resize(preview.zValues.size());
      for (std::size_t i = 0; i < preview.zValues.size(); ++i)
        preview.validMask[i] = std::isfinite(preview.zValues[i]) ? 1U : 0U;
      frame.rangeFrame = std::move(preview);
    } else {
      frame.rangeFrame = rangeFrame(part, source);
    }
    frame.metadata.frameIndex = sequence;
    if (source.frame_id) frame.metadata.frameId = std::to_string(source.frame_id);
    presentPointCloud(frame, source);
    return frame.isValid() ? std::optional<GraphicsFrame>(std::move(frame))
                           : reject("invalid coordinate layout or missing Scan3d calibration");
  }
  const auto format = source.payload_type == kPayloadJpeg
      ? GraphicsImagePixelFormat::Jpeg : pixelFormat(source.pixel_format);
  if (format == GraphicsImagePixelFormat::Unknown) return reject("unknown PFNC pixel format");
  if (source.payload.empty()) return reject("empty payload");
  if (source.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || source.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return reject("image dimensions exceed the supported integer range");

  if (source.pixel_endianness == 2 && !isByteOrdered(format)) {
    const auto size_bits = static_cast<unsigned>((source.pixel_format >> 16U) & 0xffU);
    if (size_bits == 10 || size_bits == 12 || size_bits == 14)
      return reject("big-endian packed pixels are unsupported");
  }

  const auto minimum = GraphicsImageValidation::minimumRowBytes(
      format, static_cast<int>(source.width));
  const auto stride = source.stride ? source.stride : minimum;
  if (stride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return reject("stride exceeds the supported integer range");

  auto storage = std::make_shared<std::vector<std::uint8_t>>(source.payload);
  if (source.pixel_endianness == 2 && !isByteOrdered(format)) {
    const auto size_bits = static_cast<unsigned>((source.pixel_format >> 16U) & 0xffU);
    const auto word = size_bits == 32 ? 4U : 2U;
    if (size_bits == 32 || size_bits % 16U == 0) {
      for (std::uint32_t y = 0; y < source.height; ++y) {
        auto* row = storage->data() + static_cast<std::size_t>(y) * stride;
        const auto row_bytes = std::min(stride, storage->size() - static_cast<std::size_t>(y) * stride);
        for (std::size_t offset = 0; offset + word <= row_bytes; offset += word)
          std::reverse(row + offset, row + offset + word);
      }
    }
  }
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
