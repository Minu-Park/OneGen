#include "OneGenGraphicsFrameAdapter.h"
#include "graphics/data/GraphicsRangeFrameWriter.h"
#include "Internal/CoordinateFormat.h"
#include <algorithm>
#include <array>
#include <bit>

#include <limits>
#include <cmath>
#include <cctype>
#include <cstring>
#include <memory>
#include <sstream>
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
/** @brief Recognizes GenTL 3D images and individual planes of planar 3D images. */
bool is3dPart(std::uint64_t type) noexcept { return type >= kPart3dImage && type <= 8; }
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

enum class AuxiliaryFormat { Unsupported, Float, MaskBit, PackedBit, Byte, Word };

struct AuxiliaryPart
{
  const onegen::Frame::Part* part = nullptr;
  bool confidence = false;
};

struct AuxiliaryPlan
{
  std::vector<AuxiliaryPart> parts;
  std::vector<std::size_t> used_indices;
  bool intensity = false;
  bool confidence = false;
  bool intensity_raw = false;
  bool confidence_raw = false;
  bool intensity_mask = false;
  bool confidence_mask = false;
  std::uint8_t intensity_bits = 0;
  std::uint8_t confidence_bits = 0;
};

[[nodiscard]] AuxiliaryFormat auxiliaryFormat(const onegen::Frame::Part& part,
                                              bool& confidence, std::uint8_t& bits)
{
  confidence = part.data_type == kPartConfidence
      || part.data_format == kConfidence1 || part.data_format == kConfidence1p
      || part.data_format == kConfidence8 || part.data_format == kConfidence16
      || part.data_format == kConfidence32f;
  const auto format = pixelFormat(part.data_format);
  if (!confidence && format != GraphicsImagePixelFormat::Mono8
      && format != GraphicsImagePixelFormat::Mono16) return AuxiliaryFormat::Unsupported;
  bits = static_cast<std::uint8_t>((part.data_format >> 16U) & 0xffU);
  if (part.data_format == kConfidence32f) return AuxiliaryFormat::Float;
  if (part.data_format == kConfidence1) return AuxiliaryFormat::MaskBit;
  if (part.data_format == kConfidence1p) return AuxiliaryFormat::PackedBit;
  if (bits == 8) return AuxiliaryFormat::Byte;
  if (bits == 16) return AuxiliaryFormat::Word;
  return AuxiliaryFormat::Unsupported;
}

template <typename T>
[[nodiscard]] bool scalarPartFits(const onegen::Frame::Part& part)
{
  if (!part.width || !part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return false;
  const auto rowBytes = static_cast<std::size_t>(part.width) * sizeof(T);
  return part.stride >= rowBytes && part.height <= part.payload.size() / part.stride;
}

[[nodiscard]] bool auxiliaryPayloadFits(const onegen::Frame::Part& part,
                                        const AuxiliaryFormat format)
{
  if (format == AuxiliaryFormat::Float) return scalarPartFits<float>(part);
  if (format == AuxiliaryFormat::MaskBit || format == AuxiliaryFormat::Byte)
    return scalarPartFits<std::uint8_t>(part);
  if (format == AuxiliaryFormat::Word) return scalarPartFits<std::uint16_t>(part);
  if (format == AuxiliaryFormat::PackedBit) {
    const auto row_bytes = onegen::coordinate_row_bytes({1, 1, false, false, true}, part.width);
    const auto stride = part.stride ? part.stride : row_bytes;
    return row_bytes && stride >= row_bytes && part.height <= part.payload.size() / stride;
  }
  return false;
}
[[nodiscard]] AuxiliaryPlan planAuxiliary(const onegen::Frame& source,
                                          const onegen::Frame::Part& selected,
                                          const std::uint32_t width,
                                          const std::uint32_t height)
{
  AuxiliaryPlan plan;
  if (source.pixel_endianness > 2) return plan;
  for (std::size_t index = 0; index < source.parts.size(); ++index) {
    const auto& part = source.parts[index];
    if (part.data_type != kPartConfidence && part.data_type != kPart2dImage) continue;
    if (part.source_id != selected.source_id || part.region_id != selected.region_id
        || part.width != width || part.height != height) continue;
    bool confidence = false;
    std::uint8_t bits = 0;
    const auto format = auxiliaryFormat(part, confidence, bits);
    if (format == AuxiliaryFormat::Unsupported || !auxiliaryPayloadFits(part, format)) continue;
    plan.parts.push_back({&part, confidence});
    plan.used_indices.push_back(index);
    if (confidence) {
      plan.confidence = true;
      plan.confidence_bits = (part.data_format == kConfidence1
          || part.data_format == kConfidence1p) ? 1 : bits;
      plan.confidence_raw = format == AuxiliaryFormat::Word;
      plan.confidence_mask = format == AuxiliaryFormat::Float;
    } else {
      plan.intensity = true;
      plan.intensity_bits = bits;
      plan.intensity_raw = format == AuxiliaryFormat::Word;
      plan.intensity_mask = false;
    }
  }
  return plan;
}

template <typename T>
bool copyScalarPart(const onegen::Frame::Part& part, float* values,
                    std::uint16_t* raw, std::uint64_t endianness)
{
  if (!part.width || !part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / part.height
      || part.width > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) return false;
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  const auto rowBytes = static_cast<std::size_t>(part.width) * sizeof(T);
  if (part.stride < rowBytes || part.height > part.payload.size() / part.stride)
    return false;
  for (std::uint32_t y = 0; y < part.height; ++y) {
    const auto* row = part.payload.data() + static_cast<std::size_t>(y) * part.stride;
    for (std::uint32_t x = 0; x < part.width; ++x) {
      const auto sample = readSample<T>(row + static_cast<std::size_t>(x) * sizeof(T), endianness);
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      values[index] = static_cast<float>(sample);
      if (raw) raw[index] = static_cast<std::uint16_t>(sample);
    }
  }
  return true;
}

bool writeAuxiliary(const AuxiliaryPart& source, GraphicsRangeFrameWriter& writer,
                    const std::uint64_t endianness)
{
  const auto& part = *source.part;
  bool confidence = false;
  std::uint8_t bits = 0;
  const auto format = auxiliaryFormat(part, confidence, bits);
  if (format == AuxiliaryFormat::Unsupported) return false;
  float* const values = confidence ? writer.confidenceData() : writer.intensityData();
  std::uint16_t* const raw = confidence
      ? writer.confidenceRawData() : writer.intensityRawData();
  std::uint8_t* const mask = confidence
      ? writer.confidenceValidMaskData() : writer.intensityValidMaskData();
  if (values == nullptr) return false;

  if (format == AuxiliaryFormat::Float) {
    if (!copyScalarPart<float>(part, values, nullptr, endianness)) return false;
    if (mask != nullptr) {
      const auto count = static_cast<std::size_t>(part.width) * part.height;
      for (std::size_t index = 0; index < count; ++index)
        mask[index] = std::isfinite(values[index]) ? 1U : 0U;
    }
    return true;
  }
  if (format == AuxiliaryFormat::MaskBit) {
    if (!copyScalarPart<std::uint8_t>(part, values, nullptr, 0)) return false;
    const auto count = static_cast<std::size_t>(part.width) * part.height;
    for (std::size_t index = 0; index < count; ++index)
      values[index] = (static_cast<std::uint8_t>(values[index]) & 1U) ? 1.F : 0.F;
    return true;
  }
  if (format == AuxiliaryFormat::PackedBit) {
    const auto row_bytes = onegen::coordinate_row_bytes({1, 1, false, false, true}, part.width);
    const auto stride = part.stride ? part.stride : row_bytes;
    if (!row_bytes || stride < row_bytes || part.height > part.payload.size() / stride)
      return false;
    for (std::uint32_t y = 0; y < part.height; ++y)
      for (std::uint32_t x = 0; x < part.width; ++x)
        values[static_cast<std::size_t>(y) * part.width + x] =
            onegen::packed_bits(part.payload.data() + static_cast<std::size_t>(y) * stride, stride,
                                static_cast<std::uint64_t>(x), 1) ? 1.F : 0.F;
    return true;
  }
  if (format == AuxiliaryFormat::Byte) {
    return copyScalarPart<std::uint8_t>(part, values, nullptr, 0);
  }
  return copyScalarPart<std::uint16_t>(part, values, raw, endianness);
}
struct RangeRejection {
  OneGenPlanarRejection::Reason reason = OneGenPlanarRejection::Reason::JoinedRangeRejected;
  std::size_t requestedStorageBytes = 0;
};

struct PlannedRangeSpec
{
  onegen::Frame::Part part;
  onegen::CoordinateFormat layout;
  onegen::Frame::Scan3dGeometry geometry;
  bool native = false;
  bool disparity = false;
  bool polar = false;
  bool project = false;
  AuxiliaryPlan auxiliary;
  GraphicsRangeWriteLayout writePlan;
  GraphicsLengthUnit lengthUnit = GraphicsLengthUnit::Millimeter;
  double xScale = 1.0;
  double yScale = 1.0;
  double zScale = 1.0;
  double xOffset = 0.0;
  double yOffset = 0.0;
  double zOffset = 0.0;
  RangeFrameXYCoordinateMode xyCoordinateMode = RangeFrameXYCoordinateMode::UniformPhysical;
  double unitScale = 1.0;
  std::uint64_t pixelEndianness = 0;
  std::uint64_t frameId = 0;
};

[[nodiscard]] bool prepareRangeSpec(const onegen::Frame::Part& part,
                                    const onegen::Frame& source,
                                    const AuxiliaryPlan& auxiliary,
                                    PlannedRangeSpec& out,
                                    std::string* diagnostic = nullptr,
                                    RangeRejection* evidence = nullptr)
{
  using Reason = OneGenPlanarRejection::Reason;
  const auto reject = [diagnostic, evidence](const std::string& reason,
      Reason failedRule = Reason::JoinedRangeRejected,
      std::size_t requestedStorage = 0) {
    if (diagnostic) *diagnostic = reason;
    if (evidence) {
      evidence->reason = failedRule;
      evidence->requestedStorageBytes = requestedStorage;
    }
    return false;
  };
  const auto layout = onegen::coordinate_format(part.data_format);
  // Coord3D is a range whether the producer labels the part 3D, 2D, or unknown.
  const bool rangePartType = is3dPart(part.data_type)
      || part.data_type == kPart2dImage || part.data_type == 0;
  if (!layout.components || !rangePartType || !part.width || !part.height
      || source.pixel_endianness > 2
      || part.width > (std::numeric_limits<std::size_t>::max)() / part.height)
    return reject("unsupported coordinate format/part type, dimensions, or byte order");
  if (part.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || part.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return reject("coordinate dimensions exceed the supported integer range");
  // Scalar C without calibration is still an organized height map. Multi-axis
  // modes stay rejected so missing A/B/C calibration is not invented.
  onegen::Frame::Scan3dGeometry pixelGridPreview;
  const bool pixelGridPreviewActive = !source.scan3d_geometry && layout.components == 1;
  if (pixelGridPreviewActive) pixelGridPreview.output_mode = "UncalibratedC";
  if (!source.scan3d_geometry && !pixelGridPreviewActive)
    return reject("multi-axis coordinates require Scan3d calibration", Reason::MissingCalibration);
  const auto& geometry = pixelGridPreviewActive ? pixelGridPreview : *source.scan3d_geometry;
  const auto mode = geometry.output_mode;
  const bool native = mode == "UncalibratedC";
  const auto system = geometry.coordinate_system;
  if (layout.components == 1) {
    if (!mode.empty() && mode != "CalibratedC" && mode != "RectifiedC"
        && mode != "DisparityC" && !native)
      return reject("unsupported scalar Scan3dOutputMode=" + mode, Reason::UnsupportedGeometry);
  } else if (layout.components == 2) {
    if (mode != "CalibratedAC" && mode != "RectifiedAC")
      return reject("unsupported AC Scan3dOutputMode=" + mode, Reason::UnsupportedGeometry);
  } else if (!mode.empty() && mode != "CalibratedABC_Grid" && mode != "CalibratedABC_PointCloud") {
    return reject("unsupported ABC Scan3dOutputMode=" + mode, Reason::UnsupportedGeometry);
  }
  if (!native && !system.empty() && system != "Cartesian" && system != "Spherical"
      && system != "Cylindrical")
    return reject("unsupported Scan3dCoordinateSystem=" + system, Reason::UnsupportedGeometry);
  if ((system == "Spherical" || system == "Cylindrical") && layout.components == 1
      && mode != "RectifiedC")
    return reject("scalar angular coordinates require RectifiedC", Reason::UnsupportedGeometry);
  if (mode == "DisparityC" && (!std::isfinite(geometry.focal_length)
      || !std::isfinite(geometry.baseline) || geometry.focal_length == 0 || geometry.baseline == 0))
    return reject("DisparityC requires finite nonzero focal length and baseline", Reason::InvalidCalibration);
  const bool full_axes = layout.components > 1 || mode == "RectifiedC";
  for (std::size_t axis = full_axes ? 0 : 2; axis < 3; ++axis)
    if (!native && (!geometry.calibrated_axes[axis] || !std::isfinite(geometry.scale[axis])
        || !std::isfinite(geometry.offset[axis])))
      return reject("missing or invalid calibration for axis=" + std::to_string(axis), Reason::InvalidCalibration);

  GraphicsLengthUnit lengthUnit = GraphicsLengthUnit::Millimeter;
  const auto unit = lower(geometry.distance_unit);
  double unitScale = 1.0;
  if (!native) {
    if (unit == "millimeter" || unit == "mm") lengthUnit = GraphicsLengthUnit::Millimeter;
    else if (unit == "micrometer" || unit == "um") lengthUnit = GraphicsLengthUnit::Micrometer;
    else if (unit == "nanometer" || unit == "nm") lengthUnit = GraphicsLengthUnit::Nanometer;
    else if (unit == "meter" || unit == "m") { lengthUnit = GraphicsLengthUnit::Millimeter; unitScale = 1000.0; }
    else if (unit == "inch" || unit == "in") { lengthUnit = GraphicsLengthUnit::Millimeter; unitScale = 25.4; }
    else return reject("unsupported Scan3dDistanceUnit=" + geometry.distance_unit, Reason::UnsupportedUnit);
  }

  const bool disparity = mode == "DisparityC";
  const bool polar = system == "Spherical" || system == "Cylindrical";
  const bool project = disparity && std::isfinite(geometry.principal_u)
      && std::isfinite(geometry.principal_v);
  const bool explicitXY = layout.components > 1 || (disparity && project) || polar;
  const bool hasRangeRaw = layout.components == 1 && !layout.floating
      && layout.sample_bits <= 16 && !disparity && !polar;

  const auto row_bytes = onegen::coordinate_row_bytes(layout, part.width);
  if (!row_bytes) return reject("coordinate row size is unsupported");
  const auto stride = part.stride ? part.stride : row_bytes;
  const auto planes = layout.planar ? layout.components : 1U;
  if (stride < row_bytes || part.height > part.payload.size() / stride / planes)
    return reject("coordinate payload is truncated or stride is smaller than the row size");

  GraphicsRangeFrameLayout frameLayout;
  frameLayout.width = static_cast<int>(part.width);
  frameLayout.height = static_cast<int>(part.height);
  frameLayout.channels.x = explicitXY;
  frameLayout.channels.y = explicitXY;
  frameLayout.channels.validMask = true;
  frameLayout.channels.rangeRaw = hasRangeRaw;
  frameLayout.channels.intensity = auxiliary.intensity;
  frameLayout.channels.intensityRaw = auxiliary.intensity_raw;
  frameLayout.channels.intensityValidMask = auxiliary.intensity_mask;
  frameLayout.channels.confidence = auxiliary.confidence;
  frameLayout.channels.confidenceRaw = auxiliary.confidence_raw;
  frameLayout.channels.confidenceValidMask = auxiliary.confidence_mask;

  const auto writePlan = planGraphicsRangeWrite(frameLayout);
  if (!writePlan) {
    if (writePlan.error() == GraphicsFrameError::Capacity)
      return reject("range writer size limit exceeded", Reason::RangeSizeLimitExceeded);
    return reject("invalid range writer dimensions", Reason::RangeInvalidDimensions);
  }

  out = PlannedRangeSpec{};
  out.part = part;
  out.part.stride = stride;
  out.layout = layout;
  out.geometry = geometry;
  out.native = native;
  out.disparity = disparity;
  out.polar = polar;
  out.project = project;
  out.auxiliary = auxiliary;
  out.writePlan = writePlan.value();
  out.lengthUnit = lengthUnit;
  out.unitScale = unitScale;
  out.pixelEndianness = source.pixel_endianness;
  out.frameId = source.frame_id;
  if (layout.components > 1) {
    out.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
  } else {
    out.zScale = native ? 1.0 : geometry.scale[2];
    out.zOffset = native ? 0.0 : geometry.offset[2];
    if (mode == "RectifiedC") {
      out.xScale = geometry.scale[0]; out.xOffset = geometry.offset[0];
      out.yScale = geometry.scale[1]; out.yOffset = geometry.offset[1];
    } else {
      // SFNC CalibratedC guarantees calibrated C only, even if A/B nodes exist.
      // Pixel coordinates are not physical XY; display fitting belongs to the consumer.
      out.xyCoordinateMode = RangeFrameXYCoordinateMode::PixelGrid;
      out.xScale = out.yScale = std::numeric_limits<double>::quiet_NaN();
      out.xOffset = out.yOffset = std::numeric_limits<double>::quiet_NaN();
    }
  }
  if (disparity && project) out.xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
  return true;
}

std::optional<RangeFrame> writeRangeSpec(const PlannedRangeSpec& spec,
                                         GraphicsFrameBuilder& builder,
                                         std::string* diagnostic = nullptr,
                                         RangeRejection* evidence = nullptr)
{
  using Reason = OneGenPlanarRejection::Reason;
  const auto reject = [diagnostic, evidence](const std::string& reason,
      Reason failedRule = Reason::JoinedRangeRejected,
      std::size_t requestedStorage = 0) {
    if (diagnostic) *diagnostic = reason;
    if (evidence) {
      evidence->reason = failedRule;
      evidence->requestedStorageBytes = requestedStorage;
    }
    return std::optional<RangeFrame>{};
  };

  GraphicsRangeFrameWriter writer(builder, spec.writePlan.source);
  if (!writer.isValid()) {
    switch (writer.failure()) {
    case GraphicsRangeFrameWriterFailure::InvalidDimensions:
      return reject("invalid range writer dimensions", Reason::RangeInvalidDimensions, writer.blockBytes());
    case GraphicsRangeFrameWriterFailure::SizeLimitExceeded:
      return reject("range writer size limit exceeded", Reason::RangeSizeLimitExceeded, writer.blockBytes());
    case GraphicsRangeFrameWriterFailure::StorageRefused:
      return reject("provided storage refused the range frame", Reason::RangeStorageRefused, writer.blockBytes());
    case GraphicsRangeFrameWriterFailure::None:
      break;
    }
    return reject("range writer did not produce a valid destination", Reason::JoinedRangeRejected, writer.blockBytes());
  }

  const auto& part = spec.part;
  const auto& layout = spec.layout;
  const auto& geometry = spec.geometry;
  const auto stride = part.stride;
  const auto plane_bytes = stride * part.height;
  const auto count = static_cast<std::size_t>(part.width) * part.height;

  float* const zValues = writer.zData();
  float* const xValues = writer.xData();
  float* const yValues = writer.yData();
  std::uint8_t* const validMask = writer.validMaskData();
  std::uint16_t* const rangeRaw = writer.rangeRawData();

  double xScale = spec.xScale;
  double yScale = spec.yScale;
  double zScale = spec.zScale;
  double xOffset = spec.xOffset;
  double yOffset = spec.yOffset;
  double zOffset = spec.zOffset;
  RangeFrameXYCoordinateMode xyCoordinateMode = spec.xyCoordinateMode;
  const auto system = geometry.coordinate_system;

  for (std::uint32_t y = 0; y < part.height; ++y) {
    for (std::uint32_t x = 0; x < part.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      bool valid = true;
      for (unsigned component = 0; component < layout.components; ++component) {
        const auto axis = layout.components == 3 ? component
            : (component == layout.components - 1 ? 2U : 0U);
        const double sample = readCoordinate(part, layout, stride, plane_bytes, x, y, component,
                                             spec.pixelEndianness);
        const double world = spec.native ? sample : sample * geometry.scale[axis] + geometry.offset[axis];
        const auto stored = static_cast<float>(layout.components == 1 ? sample : world);
        valid = valid && std::isfinite(sample) && std::isfinite(world)
            && std::isfinite(static_cast<float>(world)) && std::isfinite(stored)
            && !(geometry.invalid_enabled[axis] && sample == geometry.invalid_value[axis]);
        if (axis == 0) { if (xValues) xValues[index] = stored; }
        else if (axis == 1) { if (yValues) yValues[index] = stored; }
        else {
          zValues[index] = stored;
          if (rangeRaw) rangeRaw[index] = static_cast<std::uint16_t>(sample);
        }
      }
      if (layout.components == 2) {
        if (yValues) yValues[index] = static_cast<float>(y * geometry.scale[1] + geometry.offset[1]);
        valid = valid && (!yValues || std::isfinite(yValues[index]));
      }
      validMask[index] = valid ? 1U : 0U;
    }
  }
  const auto finite_store = [](double value, float& stored) {
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value))) return false;
    stored = static_cast<float>(value);
    return true;
  };
  if (spec.disparity) {
    const auto scale = zScale;
    const auto offset = zOffset;
    zScale = 1;
    zOffset = 0;
    for (std::uint32_t y = 0; y < part.height; ++y) {
      for (std::uint32_t x = 0; x < part.width; ++x) {
        const auto index = static_cast<std::size_t>(y) * part.width + x;
        if (!validMask[index]) continue;
        const double disparityValue = offset + static_cast<double>(zValues[index]) * scale;
        if (disparityValue == 0 || !std::isfinite(disparityValue)) { validMask[index] = 0; continue; }
        const double z = geometry.baseline * geometry.focal_length / disparityValue;
        if (!finite_store(z, zValues[index])) { validMask[index] = 0; continue; }
        if (spec.project) {
          if (!xValues || !yValues
              || !finite_store((static_cast<double>(x) - geometry.principal_u) * z / geometry.focal_length,
                               xValues[index])
              || !finite_store((static_cast<double>(y) - geometry.principal_v) * z / geometry.focal_length,
                               yValues[index]))
            validMask[index] = 0;
        }
      }
    }
  } else if (spec.polar) {
    xyCoordinateMode = RangeFrameXYCoordinateMode::ExplicitXY;
    zScale = 1;
    zOffset = 0;
    for (std::uint32_t y = 0; y < part.height; ++y) {
      for (std::uint32_t x = 0; x < part.width; ++x) {
        const auto index = static_cast<std::size_t>(y) * part.width + x;
        if (!validMask[index]) continue;
        double theta = 0, second = 0, rho = 0;
        if (layout.components >= 2) {
          theta = xValues[index]; second = yValues[index]; rho = zValues[index];
        } else {
          theta = x * geometry.scale[0] + geometry.offset[0];
          second = y * geometry.scale[1] + geometry.offset[1];
          rho = geometry.offset[2] + static_cast<double>(zValues[index]) * geometry.scale[2];
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
          validMask[index] = 0;
        else {
          xValues[index] = stored_x;
          yValues[index] = stored_y;
          zValues[index] = stored_z;
        }
      }
    }
  }
  if (spec.unitScale != 1.0) {
    zScale *= spec.unitScale;
    zOffset *= spec.unitScale;
    if (xyCoordinateMode != RangeFrameXYCoordinateMode::PixelGrid) {
      xScale *= spec.unitScale;
      xOffset *= spec.unitScale;
      yScale *= spec.unitScale;
      yOffset *= spec.unitScale;
      if (xValues)
        for (std::size_t index = 0; index < count; ++index)
          xValues[index] = static_cast<float>(xValues[index] * spec.unitScale);
      if (yValues)
        for (std::size_t index = 0; index < count; ++index)
          yValues[index] = static_cast<float>(yValues[index] * spec.unitScale);
      for (std::size_t index = 0; index < count; ++index) {
        if (!validMask[index]) continue;
        if ((xValues && !std::isfinite(xValues[index]))
            || (yValues && !std::isfinite(yValues[index])))
          validMask[index] = 0;
      }
    }
  }
  for (const auto& auxiliaryPart : spec.auxiliary.parts) {
    if (!writeAuxiliary(auxiliaryPart, writer, spec.pixelEndianness))
      return reject("auxiliary channel payload is unsupported", Reason::AuxiliaryLayoutRejected);
  }

  RangeFrame range = writer.takeFrame();
  range.lengthUnit = spec.lengthUnit;
  range.xScale = xScale;
  range.yScale = yScale;
  range.zScale = zScale;
  range.xOffset = xOffset;
  range.yOffset = yOffset;
  range.zOffset = zOffset;
  range.xyCoordinateMode = xyCoordinateMode;
  range.sensorType = spec.native ? "GenTL 3D (native scalar preview)" : "GenTL 3D";
  range.frameId = spec.frameId ? std::to_string(spec.frameId) : std::string{};
  range.rangeBits = static_cast<std::uint8_t>(layout.sample_bits);
  range.rangeField = {"gentl.coord3d-c", spec.native ? "Coordinate C (uncalibrated)" : "Coordinate C", "",
      spec.native ? MeasurementValueDomain::Native : MeasurementValueDomain::Calibrated,
      MeasurementSampleKind::GridSample, range.rangeBits};
  if (spec.auxiliary.intensity) {
    range.intensityBits = spec.auxiliary.intensity_bits;
    range.intensityField.bitsPerSample = spec.auxiliary.intensity_bits;
  }
  if (spec.auxiliary.confidence) {
    range.confidenceBits = spec.auxiliary.confidence_bits;
    range.confidenceField.bitsPerSample = spec.auxiliary.confidence_bits;
  }
  return range.isValid() ? std::optional<RangeFrame>(std::move(range)) : reject("converted range layout is invalid");
}

struct PlanarRangeAssembly
{
  RangeFrame range;
  std::size_t selected_index = 0;
  std::array<std::size_t, 3> consumed_indices{};
  std::vector<std::size_t> used_auxiliary_indices;
};

constexpr std::array<std::array<std::uint64_t, 4>, 6> planarFormats{{
    {0x010800af, 0x010800b0, 0x010800b1, 0x021800b3},
    {0x011000b6, 0x011000b7, 0x011000b8, 0x023000ba},
    {0x012000bd, 0x012000be, 0x012000bf, 0x026000c1},
    {0x01400126, 0x01400127, 0x01400128, 0x02c0012a},
    {0x010a00d5, 0x010a00d6, 0x010a00d7, 0x021e00dc},
    {0x010c00d8, 0x010c00d9, 0x010c00da, 0x022400df},
}};

const std::array<std::uint64_t, 4>* planarFamily(std::uint64_t format)
{
  for (const auto& family : planarFormats)
    if (std::find(family.begin(), family.begin() + 3, format) != family.begin() + 3)
      return &family;
  return nullptr;
}

bool samePlanarSource(const onegen::Frame::Part& first, const onegen::Frame::Part& second)
{
  return is3dPart(second.data_type) && first.source_id == second.source_id
      && first.region_id == second.region_id;
}

bool samePlanarLayout(const onegen::Frame::Part& first, const onegen::Frame::Part& second)
{
  return first.format_namespace == second.format_namespace && first.width == second.width
      && first.height == second.height;
}

struct PlannedPlanarRange
{
  PlannedRangeSpec range;
  std::size_t selected_index = 0;
  std::array<std::size_t, 3> consumed_indices{};
  std::vector<std::size_t> used_auxiliary_indices;
  std::size_t anchor_part = 0;
};

[[nodiscard]] std::vector<PlannedPlanarRange> preparePlanarRangeSpecs(
    const onegen::Frame& source, std::optional<OneGenPlanarRejection>* rejection)
{
  std::vector<PlannedPlanarRange> assemblies;
  for (std::size_t base_index = 0; base_index < source.parts.size(); ++base_index) {
    const auto& a = source.parts[base_index];
    if (!is3dPart(a.data_type)) continue;
    const auto* family = planarFamily(a.data_format);
    if (!family) continue;
    const auto& formats = *family;
    const bool primaryAxis = a.data_format == formats[0];
    if (!primaryAxis && (!rejection || *rejection)) continue;
    std::array<const onegen::Frame::Part*, 3> planes{};
    std::array<std::size_t, 3> plane_indices{};
    const auto axisFor = [&](const onegen::Frame::Part& part) {
      if (part.data_format == formats[0]) return 0;
      if (part.data_format == formats[1]) return 1;
      if (part.data_format == formats[2]) return 2;
      return -1;
    };
    const auto sameGroup = [&](const onegen::Frame::Part& part) {
      return samePlanarSource(a, part);
    };
    const auto sameShape = [&](const onegen::Frame::Part& part) {
      return part.width == a.width && part.height == a.height;
    };
    using Reason = OneGenPlanarRejection::Reason;
    const auto record = [&](Reason reason, std::optional<std::size_t> affectedAxis = std::nullopt,
                            std::size_t minimum = 0, std::size_t requestedStorage = 0) {
      if (!rejection || *rejection) return;
      OneGenPlanarRejection evidence;
      evidence.reason = reason;
      evidence.anchorPart = base_index;
      evidence.requestedStorageBytes = requestedStorage;
      for (std::size_t axis = 0; axis < planes.size(); ++axis)
        if (planes[axis]) evidence.matchedAxes |= 1U << axis;
      if (reason == Reason::MissingAxis || reason == Reason::DuplicateAxis) {
        unsigned seenAxes = 0;
        for (std::size_t i = 0; i < source.parts.size(); ++i) {
          const auto& part = source.parts[i];
          if (!sameGroup(part)) continue;
          const auto axis = axisFor(part);
          if (axis < 0) continue;
          const unsigned bit = 1U << axis;
          const bool namespaceMatches = part.format_namespace == a.format_namespace;
          const bool shapeMatches = sameShape(part);
          if (!namespaceMatches) evidence.namespaceMismatchAxes |= bit;
          if (!shapeMatches) evidence.shapeMismatchAxes |= bit;
          if (!namespaceMatches || !shapeMatches) continue;
          if (seenAxes & bit) {
            if (!evidence.affectedPart) evidence.affectedPart = i;
            evidence.duplicateAxes |= bit;
          }
          seenAxes |= bit;
        }
      }
      if (affectedAxis) {
        const auto& plane = *planes[*affectedAxis];
        evidence.affectedPart = plane_indices[*affectedAxis];
        evidence.reportedStride = plane.stride;
        evidence.effectiveStride = plane.stride ? plane.stride : minimum;
        evidence.minimumRowBytes = minimum;
        evidence.readableBytes = plane.payload.size();
        const auto requiredStride = std::max(evidence.effectiveStride, minimum);
        if (plane.height <= (std::numeric_limits<std::size_t>::max)() / requiredStride)
          evidence.requiredBytes = requiredStride * plane.height;
      }
      *rejection = evidence;
    };
    bool ambiguous = false;
    for (std::size_t part_index = 0; part_index < source.parts.size(); ++part_index) {
      const auto& part = source.parts[part_index];
      if (!sameGroup(part) || part.format_namespace != a.format_namespace || !sameShape(part)) continue;
      const auto axis = axisFor(part);
      if (axis < 0) continue;
      if (planes[axis]) ambiguous = true;
      planes[axis] = &part;
      plane_indices[axis] = part_index;
    }
    // Without an A anchor, only matching B and C imply a planar group.
    if (!primaryAxis && (planes[0] || !planes[1] || !planes[2])) continue;
    if (ambiguous || !planes[0] || !planes[1] || !planes[2]) {
      record(!planes[0] || !ambiguous ? Reason::MissingAxis : Reason::DuplicateAxis);
      continue;
    }
    const auto layout = onegen::coordinate_format(formats[3]);
    const auto row_bytes = onegen::coordinate_row_bytes(
        {1, layout.sample_bits, false, layout.floating, layout.packed}, a.width);
    if (!row_bytes || !a.height) {
      record(Reason::InvalidLayout);
      continue;
    }
    onegen::Frame::Part joined;
    joined.width = a.width;
    joined.height = a.height;
    joined.data_type = kPart3dImage;
    joined.stride = row_bytes;
    joined.data_format = formats[3];
    joined.format_namespace = a.format_namespace;
    joined.source_id = a.source_id;
    joined.region_id = a.region_id;
    bool valid = true;
    for (std::size_t axis = 0; axis < planes.size(); ++axis) {
      const auto* plane = planes[axis];
      const auto stride = plane->stride ? plane->stride : row_bytes;
      if (stride < row_bytes || plane->height > plane->payload.size() / stride) {
        record(stride < row_bytes ? Reason::StrideBelowMinimum : Reason::TruncatedPlane, axis, row_bytes);
        valid = false;
        break;
      }
      for (std::uint32_t row = 0; row < plane->height; ++row) {
        const auto first = plane->payload.begin() + static_cast<std::size_t>(row) * stride;
        joined.payload.insert(joined.payload.end(), first, first + row_bytes);
      }
    }
    if (!valid) continue;
    const auto auxiliary = planAuxiliary(source, a, a.width, a.height);
    PlannedRangeSpec rangeSpec;
    RangeRejection rangeEvidence;
    if (prepareRangeSpec(joined, source, auxiliary, rangeSpec, nullptr,
                         rejection && !*rejection ? &rangeEvidence : nullptr)) {
      assemblies.push_back({std::move(rangeSpec), base_index, plane_indices,
                            auxiliary.used_indices, base_index});
    } else {
      record(rangeEvidence.reason, std::nullopt, 0, rangeEvidence.requestedStorageBytes);
    }
  }
  return assemblies;
}

struct PlannedImageSpec
{
  bool isJpeg = false;
  GraphicsImageWriterSource writerSource{};
  std::uint64_t pfnc = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t stride = 0;
  std::size_t minimum = 0;
  std::size_t bytes = 0;
  std::size_t alignment = 1;
  OneGenImageRejection::Conversion conversion = OneGenImageRejection::Conversion::Copy;
  GraphicsImagePixelFormat format = GraphicsImagePixelFormat::Unknown;
  std::uint64_t sequence = 0;
};

[[nodiscard]] bool prepare2dImageSpec(
    std::uint64_t payload_type, bool image_present, std::uint64_t pfnc,
    std::uint32_t width, std::uint32_t height, std::uint64_t raw_stride,
    std::uint64_t pixel_endianness, std::uint64_t sequence,
    std::span<const std::uint8_t> payload_bytes,
    PlannedImageSpec& out,
    std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection)
{
  using Conversion = OneGenImageRejection::Conversion;
  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return false;
  };
  const auto rejectImage = [&](const char* message, std::optional<GraphicsImageFailure> failure,
                               Conversion conversion = Conversion::None,
                               std::uint64_t stride = 0, std::uint64_t minimum = 0) {
    if (image_rejection) {
      const auto requiredStride = std::max(stride, minimum);
      const auto requiredBytes = requiredStride
          && height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride
          ? requiredStride * height : 0;
      *image_rejection = OneGenImageRejection{failure, conversion, stride, minimum, requiredBytes};
    }
    return reject(message);
  };
  if (payload_type != 0 && payload_type != 1
      && payload_type != 4 && payload_type != kPayloadJpeg)
    return rejectImage("payload type does not describe a supported image", GraphicsImageFailure::UnsupportedFormat);
  if (payload_type == 4 && !image_present)
    return rejectImage("chunk payload contains no image", std::nullopt);

  const auto format = payload_type == kPayloadJpeg
      ? GraphicsImagePixelFormat::Jpeg : pixelFormat(pfnc);
  if (format == GraphicsImagePixelFormat::Unknown)
    return rejectImage("unknown PFNC pixel format", GraphicsImageFailure::UnsupportedFormat);
  if (payload_bytes.empty())
    return rejectImage("empty payload", GraphicsImageFailure::MissingBytes);
  if (width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return rejectImage("image dimensions exceed the supported integer range", GraphicsImageFailure::InvalidDimensions);

  if (pixel_endianness == 2 && !isByteOrdered(format)) {
    const auto size_bits = static_cast<unsigned>((pfnc >> 16U) & 0xffU);
    if (size_bits == 10 || size_bits == 12 || size_bits == 14)
      return rejectImage("big-endian packed pixels are unsupported", GraphicsImageFailure::UnsupportedConversion);
  }

  const auto minimum = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(width));
  const auto stride = raw_stride ? raw_stride : minimum;
  if (stride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return rejectImage("stride exceeds the supported integer range", GraphicsImageFailure::InvalidStride,
        Conversion::None, stride, minimum);

  const bool byteSwap = pixel_endianness == 2 && !isByteOrdered(format);
  out = PlannedImageSpec{};
  out.pfnc = pfnc;
  out.width = width;
  out.height = height;
  out.stride = static_cast<std::size_t>(stride);
  out.minimum = minimum;
  out.format = format;
  out.sequence = sequence;

  if (format == GraphicsImagePixelFormat::Jpeg) {
    out.isJpeg = true;
    out.conversion = Conversion::JpegCopy;
    out.bytes = payload_bytes.size();
    out.alignment = 1U;
    out.writerSource.data = payload_bytes.data();
    out.writerSource.size = payload_bytes.size();
    out.writerSource.width = static_cast<int>(width);
    out.writerSource.height = static_cast<int>(height);
    out.writerSource.stride = static_cast<int>(stride);
    out.writerSource.pixelFormat = format;
    const auto layoutCheck = GraphicsImageValidation::layoutFailure(
        format, out.writerSource.width, out.writerSource.height,
        out.writerSource.stride, out.bytes);
    if (layoutCheck != GraphicsImageFailure::None) {
      return rejectImage("invalid image layout or insufficient payload bytes", layoutCheck,
          out.conversion, 0, 0);
    }
    return true;
  }

  out.writerSource.data = payload_bytes.data();
  out.writerSource.size = payload_bytes.size();
  out.writerSource.width = static_cast<int>(width);
  out.writerSource.height = static_cast<int>(height);
  out.writerSource.stride = static_cast<int>(stride);
  out.writerSource.pixelFormat = format;
  out.writerSource.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(format);
  out.writerSource.sourceFormat = static_cast<int>(pfnc);
  out.writerSource.declaredMonoSourceBits = monoBits(pfnc);
  out.writerSource.frameSequence = sequence;
  out.conversion = Conversion::Copy;
  if (byteSwap) {
    const auto size_bits = static_cast<unsigned>((pfnc >> 16U) & 0xffU);
    if (size_bits == 32 || size_bits % 16U == 0) {
      out.conversion = Conversion::SwapByteOrder;
      out.writerSource.operation = GraphicsImageWriterOperation::SwapByteOrder;
      const auto wordBits = size_bits == 32 ? 32U : 16U;
      if (graphicsImagePixelFormatInfo(format).componentBits != wordBits)
        out.writerSource.pixelFormat = wordBits == 32
            ? GraphicsImagePixelFormat::Mono32 : GraphicsImagePixelFormat::Mono16;
    } else {
      out.writerSource.operation = GraphicsImageWriterOperation::Copy;
    }
  } else {
    out.writerSource.operation = GraphicsImageWriterOperation::Copy;
  }

  GraphicsImageFailure failure = GraphicsImageFailure::None;
  const auto plan = planGraphicsImageWrite(out.writerSource, &failure);
  if (!plan) {
    const auto writerMinimum = GraphicsImageValidation::minimumRowBytes(
        out.writerSource.pixelFormat, out.writerSource.width);
    return rejectImage("invalid image layout or insufficient payload bytes", failure,
        out.conversion, stride ? stride : writerMinimum, writerMinimum);
  }
  out.bytes = plan.value().bytes;
  out.alignment = plan.value().alignment;
  return true;
}

[[nodiscard]] std::optional<GraphicsImage> write2dImageSpec(
    const PlannedImageSpec& spec,
    GraphicsFrameBuilder& builder,
    std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection)
{
  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsImage>{};
  };
  const auto rejectImage = [&](const char* message, std::optional<GraphicsImageFailure> failure,
                               std::uint64_t stride, std::uint64_t minimum) {
    if (image_rejection) {
      const auto requiredStride = std::max(stride, minimum);
      const auto requiredBytes = requiredStride
          && spec.height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride
          ? requiredStride * spec.height : 0;
      *image_rejection = OneGenImageRejection{failure, spec.conversion, stride, minimum, requiredBytes};
    }
    return reject(message);
  };

  GraphicsSpan<std::uint8_t> storage;
  if (spec.isJpeg) {
    auto copied = builder.copy<std::uint8_t>(spec.writerSource.data, spec.writerSource.size);
    if (!copied) return rejectImage("image storage refused", GraphicsImageFailure::StorageRefused, 0, 0);
    storage = std::move(copied).value();
  } else {
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    auto written = writeGraphicsImageWithFailure(builder, spec.writerSource, image_rejection ? &failure : nullptr);
    if (!written.isValid()) {
      if (failure != GraphicsImageFailure::None) {
        const auto writerMinimum = GraphicsImageValidation::minimumRowBytes(
            spec.writerSource.pixelFormat, spec.writerSource.width);
        return rejectImage("invalid image layout or insufficient payload bytes", failure,
            spec.stride ? spec.stride : writerMinimum, writerMinimum);
      }
      return reject("invalid image layout or insufficient payload bytes");
    }
    storage = std::move(written.pixels);
  }

  GraphicsImage image;
  image.pixels = std::move(storage);
  image.width = static_cast<int>(spec.width);
  image.height = static_cast<int>(spec.height);
  image.stride = static_cast<int>(spec.stride);
  image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(spec.format);
  image.declaredMonoSourceBits = monoBits(spec.pfnc);
  image.sourceFormat = static_cast<int>(spec.pfnc);
  image.pixelFormat = spec.format;
  const auto effectiveBits = static_cast<int>((spec.pfnc >> 16U) & 0xffU);
  image.bitAlignment = effectiveBits > image.bitsPerPixel
      || (spec.format == GraphicsImagePixelFormat::Mono16
          && image.declaredMonoSourceBits > 0 && image.declaredMonoSourceBits < 16)
      ? GraphicsImageBitAlignment::LeastSignificant : GraphicsImageBitAlignment::Packed;
  image.frameSequence = spec.sequence;
  if (!image.isValid()) {
    if (image_rejection) {
      const auto failure = GraphicsImageValidation::layoutFailure(
          image.pixelFormat, image.width, image.height, image.stride, image.pixels.size());
      if (failure != GraphicsImageFailure::None) {
        const auto rowBytes = spec.format == GraphicsImagePixelFormat::Jpeg ? 0 : spec.minimum;
        const auto effectiveStride = spec.format == GraphicsImagePixelFormat::Jpeg ? 0 : spec.stride;
        return rejectImage("invalid image layout or insufficient payload bytes", failure,
            effectiveStride, rowBytes);
      }
    }
    return reject("invalid image layout or insufficient payload bytes");
  }
  return image;
}

[[nodiscard]] bool prepareConfidencePreviewSpec(const onegen::Frame::Part& part,
                                                std::uint64_t pixel_endianness,
                                                GraphicsRangeWriteLayout& outPlan)
{
  if (pixel_endianness > 2
      || part.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || part.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return false;
  onegen::Frame::Part normalized = part;
  if (normalized.stride == 0U && normalized.width <= (std::numeric_limits<std::size_t>::max)() / 4U)
    normalized.stride = static_cast<std::size_t>(normalized.width) * 4U;
  if (!scalarPartFits<float>(normalized)) return false;
  GraphicsRangeFrameLayout layout;
  layout.width = static_cast<int>(part.width);
  layout.height = static_cast<int>(part.height);
  layout.channels.validMask = true;
  const auto plan = planGraphicsRangeWrite(layout);
  if (!plan) return false;
  outPlan = plan.value();
  return true;
}

[[nodiscard]] std::optional<RangeFrame> writeConfidencePreview(
    const onegen::Frame::Part& part, std::uint64_t pixel_endianness,
    const GraphicsRangeWriteLayout& plan, GraphicsFrameBuilder& builder)
{
  onegen::Frame::Part normalized = part;
  if (normalized.stride == 0U && normalized.width <= (std::numeric_limits<std::size_t>::max)() / 4U)
    normalized.stride = static_cast<std::size_t>(normalized.width) * 4U;
  GraphicsRangeFrameWriter writer(builder, plan.source);
  if (!writer.isValid() || !copyScalarPart<float>(normalized, writer.zData(), nullptr, pixel_endianness))
    return std::nullopt;
  float* const values = writer.zData();
  std::uint8_t* const mask = writer.validMaskData();
  for (std::size_t i = 0; i < writer.sampleCount(); ++i)
    mask[i] = std::isfinite(values[i]) ? 1U : 0U;
  RangeFrame preview = writer.takeFrame();
  preview.xScale = preview.yScale = std::numeric_limits<double>::quiet_NaN();
  preview.rangeBits = 32;
  preview.rangeField = {"gentl.confidence", "Confidence", "",
      MeasurementValueDomain::Native, MeasurementSampleKind::GridSample, 32};
  preview.sensorType = "GenTL confidence (native scalar preview)";
  return preview;
}

[[nodiscard]] const char* frameErrorMessage(GraphicsFrameError error) noexcept
{
  switch (error) {
  case GraphicsFrameError::None: return "storage failed";
  case GraphicsFrameError::InvalidInput: return "invalid frame storage request";
  case GraphicsFrameError::Capacity: return "frame storage capacity exceeded";
  case GraphicsFrameError::Allocation: return "frame storage allocation failed";
  case GraphicsFrameError::Disconnected: return "frame storage disconnected";
  case GraphicsFrameError::StaleGeneration: return "frame storage grant expired";
  case GraphicsFrameError::UnsealedWrite: return "frame write region remained open";
  case GraphicsFrameError::ClassNotPrepared: return "frame storage class not prepared";
  case GraphicsFrameError::NoReadyCredit: return "frame storage has no ready credit";
  case GraphicsFrameError::ClassRefused: return "frame storage class refused";
  }
  return "frame storage allocation failed";
}

} // namespace

std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameContextSource& context_source,
    std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection,
    std::optional<OneGenPlanarRejection>* planar_rejection,
    const std::string& source_name,
    GraphicsFrameError* storage_error)
{
  if (rejection_reason) rejection_reason->clear();
  if (image_rejection) image_rejection->reset();
  if (planar_rejection) planar_rejection->reset();
  if (storage_error) *storage_error = GraphicsFrameError::None;

  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsFrame>{};
  };

  if (!source.parts.empty()) {
    std::string droppedParts;
    bool representedByPlanarEvidence = false;
    const auto dropped = [&](std::size_t index, const std::string& reason) {
      const auto& part = source.parts[index];
      if (planar_rejection && *planar_rejection) {
        const auto& evidence = **planar_rejection;
        const auto& anchor = source.parts[evidence.anchorPart];
        const bool describesMismatches = evidence.reason == OneGenPlanarRejection::Reason::MissingAxis
            || evidence.reason == OneGenPlanarRejection::Reason::DuplicateAxis;
        if (samePlanarSource(anchor, part) && planarFamily(anchor.data_format) == planarFamily(part.data_format)
            && (describesMismatches || samePlanarLayout(anchor, part))) {
          representedByPlanarEvidence = true;
          return;
        }
      }
      std::ostringstream detail;
      detail << "part=" << index << " type=" << part.data_type << " PFNC=0x" << std::hex
          << part.data_format << std::dec << " namespace=" << part.format_namespace
          << " shape=" << part.width << 'x' << part.height << " stride=" << part.stride
          << " bytes=" << part.payload.size() << " source=" << part.source_id << " region=" << part.region_id
          << " cause=" << reason;
      if (!droppedParts.empty()) droppedParts += "; ";
      droppedParts += detail.str();
    };

    struct MultipartRangeItem
    {
      PlannedRangeSpec spec;
      std::string id;
      std::string displayName;
      std::string group;
      bool planar = false;
      std::size_t anchorPart = 0;
    };
    struct MultipartImageItem
    {
      PlannedImageSpec spec;
      std::string id;
      std::string displayName;
      std::string group;
    };
    struct MultipartConfidenceItem
    {
      std::size_t partIndex = 0;
      GraphicsRangeWriteLayout plan;
      std::string id;
      std::string group;
    };

    std::size_t totalBytes = 0;
    std::vector<MultipartRangeItem> plannedRanges;
    std::vector<MultipartImageItem> plannedImages;
    std::vector<MultipartConfidenceItem> plannedConfidences;
    std::vector<bool> consumed_parts(source.parts.size(), false);
    std::vector<bool> confidence_used(source.parts.size(), false);

    auto appendBytes = [&](std::size_t bytes, std::size_t alignment) -> bool {
      return appendAligned(totalBytes, bytes, alignment);
    };

    for (auto& assembly : preparePlanarRangeSpecs(source, planar_rejection)) {
      if (!appendBytes(assembly.range.writePlan.totalBytes, assembly.range.writePlan.alignment))
        return reject("frame size exceeds supported allocation bounds");
      const auto& selected = source.parts[assembly.selected_index];
      const auto group = std::to_string(selected.source_id) + ":" + std::to_string(selected.region_id);
      const auto id = "range." + group + "." + std::to_string(assembly.selected_index);
      for (const std::size_t index : assembly.consumed_indices) consumed_parts[index] = true;
      for (const std::size_t index : assembly.used_auxiliary_indices) confidence_used[index] = true;
      plannedRanges.push_back({std::move(assembly.range), id, "Range", group, true, assembly.anchor_part});
    }

    for (std::size_t index = 0; index < source.parts.size(); ++index) {
      const auto& part = source.parts[index];
      if (!consumed_parts[index]) {
        std::string rangeDiagnostic;
        const auto auxiliary = planAuxiliary(source, part, part.width, part.height);
        PlannedRangeSpec spec;
        if (prepareRangeSpec(part, source, auxiliary, spec, &rangeDiagnostic)) {
          if (!appendBytes(spec.writePlan.totalBytes, spec.writePlan.alignment))
            return reject("frame size exceeds supported allocation bounds");
          const auto group = std::to_string(part.source_id) + ":" + std::to_string(part.region_id);
          const auto id = "range." + group + "." + std::to_string(index);
          for (const std::size_t auxiliaryIndex : auxiliary.used_indices)
            confidence_used[auxiliaryIndex] = true;
          plannedRanges.push_back({std::move(spec), id, "Range", group, false, index});
        } else if (onegen::coordinate_format(part.data_format).components || is3dPart(part.data_type)) {
          dropped(index, rangeDiagnostic);
        }
      }
      if (part.data_type == kPart2dImage && !onegen::coordinate_format(part.data_format).components) {
        std::string imageDiagnostic;
        PlannedImageSpec imageSpec;
        if (prepare2dImageSpec(0, false, part.data_format, part.width, part.height, part.stride,
                source.pixel_endianness, sequence, part.payload, imageSpec, &imageDiagnostic, nullptr)) {
          if (!appendBytes(imageSpec.bytes, imageSpec.alignment))
            return reject("frame size exceeds supported allocation bounds");
          const auto group = std::to_string(part.source_id) + ":" + std::to_string(part.region_id);
          std::string displayName = "Image";
          const onegen::Frame::ComponentIdentity* matched = nullptr;
          std::size_t matches = 0;
          for (const auto& component : source.components) {
            if (component.id == part.purpose_id && !component.name.empty()) {
              matched = &component;
              ++matches;
            }
          }
          if (matches == 1) displayName = matched->name;
          plannedImages.push_back({std::move(imageSpec),
              "image." + group + "." + std::to_string(index), std::move(displayName), group});
        } else {
          dropped(index, imageDiagnostic);
        }
      }
      if (!consumed_parts[index] && !is3dPart(part.data_type)
          && part.data_type != kPart2dImage && part.data_type != kPartConfidence
          && !onegen::coordinate_format(part.data_format).components) {
        dropped(index, "unsupported multipart part type or PFNC format");
      }
    }
    if (plannedImages.size() > 1) {
      for (std::size_t imageIndex = 0; imageIndex < plannedImages.size(); ++imageIndex)
        if (plannedImages[imageIndex].displayName == "Image")
          plannedImages[imageIndex].displayName = "Image " + std::to_string(imageIndex + 1);
    }

    if (plannedRanges.empty() && plannedImages.empty()) {
      for (std::size_t index = 0; index < source.parts.size(); ++index) {
        const auto& part = source.parts[index];
        if (part.data_format != kConfidence32f) continue;
        GraphicsRangeWriteLayout confPlan;
        if (prepareConfidencePreviewSpec(part, source.pixel_endianness, confPlan)) {
          if (!appendBytes(confPlan.totalBytes, confPlan.alignment))
            return reject("frame size exceeds supported allocation bounds");
          const auto group = std::to_string(part.source_id) + ":" + std::to_string(part.region_id);
          plannedConfidences.push_back({index, confPlan, "confidence." + group + "." + std::to_string(index), group});
          confidence_used[index] = true;
        }
      }
    }
    for (std::size_t index = 0; index < source.parts.size(); ++index)
      if (source.parts[index].data_type == kPartConfidence && !confidence_used[index])
        dropped(index, "confidence part has no matching range or a supported auxiliary layout");

    if (plannedRanges.empty() && plannedImages.empty() && plannedConfidences.empty()) {
      if (rejection_reason && !droppedParts.empty()) {
        *rejection_reason = "multipart payload has no supported image or range data; " + droppedParts;
        return std::nullopt;
      }
      if (representedByPlanarEvidence) return std::nullopt;
      return reject("multipart payload has no supported image or range data");
    }

    auto contextResult = context_source.beginFrame(totalBytes);
    if (!contextResult) {
      if (storage_error) *storage_error = contextResult.error();
      if (planar_rejection && !*planar_rejection) {
        for (const auto& item : plannedRanges) {
          if (!item.planar) continue;
          OneGenPlanarRejection evidence;
          evidence.reason = OneGenPlanarRejection::Reason::RangeStorageRefused;
          evidence.anchorPart = item.anchorPart;
          evidence.requestedStorageBytes = item.spec.writePlan.totalBytes;
          *planar_rejection = evidence;
          break;
        }
      }
      if (rejection_reason) *rejection_reason = frameErrorMessage(contextResult.error());
      return std::nullopt;
    }

    GraphicsFrameBuilder builder(contextResult.value());
    GraphicsFrameContent result;
    for (const auto& item : plannedRanges) {
      std::string rangeDiagnostic;
      RangeRejection rangeEvidence;
      if (auto range = writeRangeSpec(item.spec, builder, &rangeDiagnostic,
              item.planar && planar_rejection && !*planar_rejection ? &rangeEvidence : nullptr)) {
        result.addRange(item.id, std::move(*range), item.displayName, item.group);
      } else if (item.planar && planar_rejection && !*planar_rejection) {
        OneGenPlanarRejection evidence;
        evidence.reason = rangeEvidence.reason;
        evidence.anchorPart = item.anchorPart;
        evidence.requestedStorageBytes = rangeEvidence.requestedStorageBytes;
        *planar_rejection = evidence;
      }
    }
    for (const auto& item : plannedImages) {
      std::string imageDiagnostic;
      if (auto image = write2dImageSpec(item.spec, builder, &imageDiagnostic, nullptr))
        result.addImage(item.id, std::move(*image), item.displayName, item.group);
    }
    for (const auto& item : plannedConfidences) {
      if (auto range = writeConfidencePreview(source.parts[item.partIndex], source.pixel_endianness, item.plan, builder))
        result.addRange(item.id, std::move(*range), "Confidence", item.group);
    }
    result.metadata.sourceName = source_name;
    result.metadata.frameIndex = sequence;
    if (source.frame_id) result.metadata.frameId = std::to_string(source.frame_id);
    if (rejection_reason) *rejection_reason = droppedParts;
    if (!result.isValid()) return reject("multipart payload has no supported image or range data");
    builder.content() = std::move(result);
    auto published = std::move(builder).publish();
    if (!published) return reject("frame publication failed");
    return std::move(published).value();
  }

  // Single-payload frame: validate and plan first, then call beginFrame(totalBytes) once.
  const auto coordinateLayout = onegen::coordinate_format(source.pixel_format);
  if (source.payload_type != 0 && source.payload_type != 1
      && source.payload_type != 4 && source.payload_type != kPayloadJpeg) {
    if (image_rejection)
      *image_rejection = OneGenImageRejection{GraphicsImageFailure::UnsupportedFormat, OneGenImageRejection::Conversion::None, 0, 0, 0};
    return reject("payload type does not describe a supported image");
  }
  if (source.payload_type == 4 && !source.buffer_info.image_present) {
    if (image_rejection)
      *image_rejection = OneGenImageRejection{std::nullopt, OneGenImageRejection::Conversion::None, 0, 0, 0};
    return reject("chunk payload contains no image");
  }

  if (coordinateLayout.components || source.pixel_format == kConfidence32f) {
    const auto row_bytes = coordinateLayout.components
        ? onegen::coordinate_row_bytes(coordinateLayout, source.width)
        : (static_cast<std::uint64_t>(source.width)
               > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)() / 4U)
            ? 0 : static_cast<std::size_t>(source.width) * 4U);
    if (!row_bytes) return reject("coordinate row size exceeds the supported integer range");
    onegen::Frame::Part part;
    part.payload = source.payload;
    part.width = source.width;
    part.height = source.height;
    part.stride = source.stride ? source.stride : row_bytes;
    part.data_format = source.pixel_format;
    part.data_type = kPart3dImage;

    if (source.pixel_format == kConfidence32f) {
      GraphicsRangeWriteLayout confPlan;
      if (!prepareConfidencePreviewSpec(part, source.pixel_endianness, confPlan))
        return reject("invalid floating confidence layout or byte order");
      auto contextResult = context_source.beginFrame(confPlan.totalBytes);
      if (!contextResult) {
        if (storage_error) *storage_error = contextResult.error();
        return reject(frameErrorMessage(contextResult.error()));
      }
      GraphicsFrameBuilder builder(contextResult.value());
      auto preview = writeConfidencePreview(part, source.pixel_endianness, confPlan, builder);
      if (!preview) return reject("invalid floating confidence layout or byte order");
      builder.addRange("confidence", std::move(*preview), "Confidence");
      GraphicsFrameMetadata metadata;
      metadata.sourceName = source_name;
      metadata.frameIndex = sequence;
      if (source.frame_id) metadata.frameId = std::to_string(source.frame_id);
      builder.setMetadata(std::move(metadata));
      auto published = std::move(builder).publish();
      if (!published) return reject("frame publication failed");
      return std::move(published).value();
    }

    PlannedRangeSpec rangeSpec;
    std::string diagnostic;
    if (!prepareRangeSpec(part, source, {}, rangeSpec, &diagnostic)) {
      if (rejection_reason) *rejection_reason = diagnostic;
      return std::nullopt;
    }
    auto contextResult = context_source.beginFrame(rangeSpec.writePlan.totalBytes);
    if (!contextResult) {
      if (storage_error) *storage_error = contextResult.error();
      return reject(frameErrorMessage(contextResult.error()));
    }
    GraphicsFrameBuilder builder(contextResult.value());
    auto range = writeRangeSpec(rangeSpec, builder, &diagnostic);
    if (!range) {
      if (rejection_reason) *rejection_reason = diagnostic;
      return std::nullopt;
    }
    builder.addRange("range", std::move(*range), "Range");
    GraphicsFrameMetadata metadata;
    metadata.sourceName = source_name;
    metadata.frameIndex = sequence;
    if (source.frame_id) metadata.frameId = std::to_string(source.frame_id);
    builder.setMetadata(std::move(metadata));
    auto published = std::move(builder).publish();
    if (!published) return reject("frame publication failed");
    return std::move(published).value();
  }

  PlannedImageSpec imageSpec;
  if (!prepare2dImageSpec(source.payload_type, source.buffer_info.image_present,
          source.pixel_format, source.width, source.height, source.stride,
          source.pixel_endianness, sequence, source.payload, imageSpec,
          rejection_reason, image_rejection))
    return std::nullopt;

  auto contextResult = context_source.beginFrame(imageSpec.bytes);
  if (!contextResult) {
    if (storage_error) *storage_error = contextResult.error();
    if (image_rejection) {
      const auto requiredStride = std::max<std::uint64_t>(imageSpec.stride, imageSpec.minimum);
      const auto requiredBytes = requiredStride
          && source.height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride
          ? requiredStride * source.height : 0;
      *image_rejection = OneGenImageRejection{
          GraphicsImageFailure::StorageRefused, imageSpec.conversion,
          imageSpec.stride, imageSpec.minimum, requiredBytes};
    }
    return reject(frameErrorMessage(contextResult.error()));
  }

  GraphicsFrameBuilder builder(contextResult.value());
  auto image = write2dImageSpec(imageSpec, builder, rejection_reason, image_rejection);
  if (!image) return std::nullopt;
  builder.addImage("image", std::move(*image), "Image");
  GraphicsFrameMetadata metadata;
  metadata.sourceName = source_name;
  metadata.frameIndex = sequence;
  if (source.frame_id) metadata.frameId = std::to_string(source.frame_id);
  builder.setMetadata(std::move(metadata));
  auto published = std::move(builder).publish();
  if (!published) return reject("frame publication failed");
  return std::move(published).value();
}

std::optional<GraphicsFrame> oneGenBuildDirectImageFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameBuilder& builder, GraphicsWriteRegion<std::uint8_t> region,
    std::size_t used_bytes, std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection,
    const std::string& source_name)
{
  if (rejection_reason) rejection_reason->clear();
  if (image_rejection) image_rejection->reset();

  using Conversion = OneGenImageRejection::Conversion;
  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsFrame>{};
  };
  const auto rejectImage = [&](const char* message, std::optional<GraphicsImageFailure> failure,
                               Conversion conversion = Conversion::None,
                               std::uint64_t stride = 0, std::uint64_t minimum = 0) {
    if (image_rejection) {
      const auto requiredStride = std::max(stride, minimum);
      const auto requiredBytes = requiredStride
          && source.height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride
          ? requiredStride * source.height : 0;
      *image_rejection = OneGenImageRejection{failure, conversion, stride, minimum, requiredBytes};
    }
    return reject(message);
  };

  if (source.payload_type != 0 && source.payload_type != 1 && source.payload_type != 4)
    return rejectImage("payload type does not describe a supported image", GraphicsImageFailure::UnsupportedFormat);
  if (source.payload_type == 4 && !source.buffer_info.image_present)
    return rejectImage("chunk payload contains no image", std::nullopt);
  const auto format = pixelFormat(source.pixel_format);
  if (format == GraphicsImagePixelFormat::Unknown || format == GraphicsImagePixelFormat::Jpeg)
    return rejectImage("unknown PFNC pixel format", GraphicsImageFailure::UnsupportedFormat);
  if (used_bytes == 0U || used_bytes > region.size())
    return rejectImage("empty or out-of-range direct payload",
        used_bytes == 0U ? GraphicsImageFailure::MissingBytes : GraphicsImageFailure::TruncatedBytes);
  if (source.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())
      || source.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()))
    return rejectImage("image dimensions exceed the supported integer range", GraphicsImageFailure::InvalidDimensions);

  if (source.pixel_endianness == 2 && !isByteOrdered(format)) {
    const auto size_bits = static_cast<unsigned>((source.pixel_format >> 16U) & 0xffU);
    if (size_bits == 10 || size_bits == 12 || size_bits == 14)
      return rejectImage("big-endian packed pixels are unsupported", GraphicsImageFailure::UnsupportedConversion);
    return rejectImage("big-endian direct pixels are unsupported", GraphicsImageFailure::UnsupportedConversion);
  }

  const auto minimum = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(source.width));
  const auto stride = source.stride ? source.stride : minimum;
  if (stride > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    return rejectImage("stride exceeds the supported integer range", GraphicsImageFailure::InvalidStride,
        Conversion::None, stride, minimum);

  const auto failure = GraphicsImageValidation::layoutFailure(
      format, static_cast<int>(source.width), static_cast<int>(source.height),
      static_cast<int>(stride), used_bytes);
  if (failure != GraphicsImageFailure::None)
    return rejectImage("invalid image layout or insufficient payload bytes", failure,
        Conversion::None, stride, minimum);

  GraphicsImage image;
  image.pixels = builder.seal(std::move(region)).subspan(0, used_bytes);
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
  if (!image.isValid())
    return rejectImage("invalid image layout or insufficient payload bytes",
        GraphicsImageFailure::InvalidDimensions, Conversion::None, stride, minimum);

  builder.addImage("image", std::move(image), "Image");
  GraphicsFrameMetadata metadata;
  metadata.sourceName = source_name;
  metadata.frameIndex = sequence;
  if (source.frame_id) metadata.frameId = std::to_string(source.frame_id);
  builder.setMetadata(std::move(metadata));
  auto published = std::move(builder).publish();
  if (!published) return reject("frame publication failed");
  return std::move(published).value();
}
