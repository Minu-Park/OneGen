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
#include <optional>
#include <span>
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

constexpr std::size_t kIntMax = static_cast<std::size_t>((std::numeric_limits<int>::max)());
constexpr std::size_t kSizeMax = (std::numeric_limits<std::size_t>::max)();
/** Row width of the 2D layout used for bytes that match no image format. */
constexpr std::size_t kRawRowBytes = 4096U;

/** Receives the anomalies of one frame: the full bounded list and the first evidence of each kind. */
struct Report {
  OneGenFrameAnomalies* list = nullptr;
  std::optional<OneGenImageRejection>* firstImage = nullptr;
  std::optional<OneGenPlanarRejection>* firstPlanar = nullptr;
  /** When set, part anomalies describe the A/B/C group anchored here instead of a single part. */
  std::optional<std::size_t> groupAnchor;
  unsigned groupAxes = 0;

  [[nodiscard]] bool enabled() const noexcept { return list || firstImage || firstPlanar; }
  void image(const OneGenImageRejection& evidence) const noexcept {
    if (list) list->add(evidence);
    if (firstImage && !*firstImage) *firstImage = evidence;
  }
  void part(OneGenPlanarRejection evidence) const noexcept {
    if (groupAnchor) {
      evidence.anchorPart = *groupAnchor;
      evidence.affectedPart.reset();
      evidence.matchedAxes = groupAxes;
    }
    if (list) list->add(evidence);
    if (firstPlanar && !*firstPlanar) *firstPlanar = evidence;
  }
};

using PlanarReason = OneGenPlanarRejection::Reason;

void notePart(const Report& report, const PlanarReason reason, const std::optional<std::size_t> part,
              const std::uint64_t reportedStride = 0, const std::uint64_t effectiveStride = 0,
              const std::uint64_t minimum = 0, const std::uint64_t readable = 0,
              const std::uint64_t required = 0, const std::uint64_t storage = 0) noexcept
{
  if (!report.enabled()) return;
  OneGenPlanarRejection evidence;
  evidence.reason = reason;
  evidence.anchorPart = part.value_or(0);
  evidence.affectedPart = part;
  evidence.reportedStride = static_cast<std::size_t>(reportedStride);
  evidence.effectiveStride = static_cast<std::size_t>(effectiveStride);
  evidence.minimumRowBytes = static_cast<std::size_t>(minimum);
  evidence.readableBytes = static_cast<std::size_t>(readable);
  evidence.requiredBytes = static_cast<std::size_t>(required);
  evidence.requestedStorageBytes = static_cast<std::size_t>(storage);
  report.part(evidence);
}

[[nodiscard]] std::optional<PlanarReason> planarReason(const GraphicsImageFailure failure) noexcept
{
  switch (failure) {
    case GraphicsImageFailure::InvalidDimensions: return PlanarReason::InvalidPartDimensions;
    case GraphicsImageFailure::UnsupportedFormat: return PlanarReason::UnsupportedPartFormat;
    case GraphicsImageFailure::InvalidStride:
    case GraphicsImageFailure::StrideBelowMinimum: return PlanarReason::StrideBelowMinimum;
    case GraphicsImageFailure::ExtentOverflow: return PlanarReason::RangeSizeLimitExceeded;
    case GraphicsImageFailure::TruncatedBytes: return PlanarReason::TruncatedPlane;
    case GraphicsImageFailure::OddWidth:
    case GraphicsImageFailure::UnsupportedConversion: return PlanarReason::InvalidLayout;
    case GraphicsImageFailure::StorageRefused: return PlanarReason::RangeStorageRefused;
    case GraphicsImageFailure::None:
    case GraphicsImageFailure::MissingBytes:
    case GraphicsImageFailure::ExcessBytes: return std::nullopt;
  }
  return std::nullopt;
}

/** Records an image anomaly: a part anomaly for a multipart part, otherwise an image anomaly. */
void noteImage(const Report& report, const std::optional<std::size_t> part,
               const std::optional<GraphicsImageFailure> failure,
               const OneGenImageRejection::Conversion conversion, const std::uint64_t reportedStride,
               const std::uint64_t effectiveStride, const std::uint64_t minimum,
               const std::uint64_t readable, const std::uint64_t required) noexcept
{
  if (!report.enabled()) return;
  if (part) {
    if (!failure) return;
    if (const auto reason = planarReason(*failure))
      notePart(report, *reason, part, reportedStride, effectiveStride, minimum, readable, required);
    return;
  }
  report.image(OneGenImageRejection{failure, conversion, effectiveStride, minimum, required});
}

/** Bytes a layout needs at @p stride for @p rows rows, or zero when the product does not fit. */
[[nodiscard]] std::size_t checkedExtent(const std::size_t stride, const std::size_t rows) noexcept
{
  if (!stride || !rows || rows > kSizeMax / stride) return 0U;
  return stride * rows;
}

double readCoordinateUnchecked(const std::uint8_t* bytes, const onegen::CoordinateFormat& layout,
                               const std::uint64_t endianness)
{
  const auto sample_bytes = layout.sample_bits / 8U;
  if (layout.floating)
    return layout.sample_bits == 64 ? readSample<double>(bytes, endianness)
                                    : readSample<float>(bytes, endianness);
  if (sample_bytes == 1) return *bytes;
  return readSample<std::uint16_t>(bytes, endianness);
}

/**
 * Reads one sample of a part. Returns false, leaving @p value zero, when the sample is not backed by
 * bytes the part actually holds: beyond the payload, or beyond the row bytes the stride delivers.
 */
bool readCoordinate(const onegen::Frame::Part& part, const onegen::CoordinateFormat& layout,
                    const std::size_t stride, const std::size_t row_bytes, const std::size_t plane_bytes,
                    const std::uint32_t x, const std::uint32_t y, const unsigned component,
                    const std::uint64_t endianness, double& value)
{
  value = 0.0;
  const auto readable_row = (std::min)(stride, row_bytes);
  const auto plane_offset = layout.planar ? static_cast<std::size_t>(component) * plane_bytes : 0U;
  if (plane_offset >= part.payload.size()) return false;
  const auto plane_limit = (std::min)(layout.planar ? plane_bytes : part.payload.size(),
                                      part.payload.size() - plane_offset);
  const auto row_start = static_cast<std::size_t>(y) * stride;
  if (layout.packed) {
    const auto comps = layout.planar ? 1U : layout.components;
    const auto in_row_bit = (static_cast<std::uint64_t>(x) * comps + (layout.planar ? 0U : component))
        * layout.sample_bits;
    if (in_row_bit + layout.sample_bits > static_cast<std::uint64_t>(readable_row) * 8U) return false;
    const auto bit = static_cast<std::uint64_t>(row_start) * 8U + in_row_bit;
    if (bit + layout.sample_bits > static_cast<std::uint64_t>(plane_limit) * 8U) return false;
    value = static_cast<double>(onegen::packed_bits(part.payload.data() + plane_offset, plane_limit,
                                                    bit, layout.sample_bits));
    return true;
  }
  const auto sample_bytes = layout.sample_bits / 8U;
  const auto in_row = (layout.planar ? static_cast<std::size_t>(x)
                                     : static_cast<std::size_t>(x) * layout.components + component)
      * sample_bytes;
  if (in_row + sample_bytes > readable_row) return false;
  const auto address = row_start + in_row;
  if (address + sample_bytes > plane_limit) return false;
  value = readCoordinateUnchecked(part.payload.data() + plane_offset + address, layout, endianness);
  return true;
}

enum class AuxiliaryFormat { Unsupported, Float, MaskBit, PackedBit, Byte, Word };

struct AuxiliaryPart
{
  const onegen::Frame::Part* part = nullptr;
  bool confidence = false;
  std::size_t index = 0;
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

/** Plans the auxiliary parts that match a range's source, region and extent; unmatched parts stay unused. */
[[nodiscard]] AuxiliaryPlan planAuxiliary(const onegen::Frame& source,
                                          const onegen::Frame::Part& selected,
                                          const std::uint32_t width,
                                          const std::uint32_t height)
{
  AuxiliaryPlan plan;
  for (std::size_t index = 0; index < source.parts.size(); ++index) {
    const auto& part = source.parts[index];
    if (part.data_type != kPartConfidence && part.data_type != kPart2dImage) continue;
    if (part.source_id != selected.source_id || part.region_id != selected.region_id
        || part.width != width || part.height != height || part.payload.empty()) continue;
    bool confidence = false;
    std::uint8_t bits = 0;
    const auto format = auxiliaryFormat(part, confidence, bits);
    if (format == AuxiliaryFormat::Unsupported) continue;
    plan.parts.push_back({&part, confidence, index});
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

/** Layout of a scalar plane: bytes per sample, row bytes, and the stride used for reading it. */
struct ScalarLayout
{
  std::size_t rowBytes = 0;
  std::size_t stride = 0;
};

[[nodiscard]] bool scalarLayout(const onegen::Frame::Part& part, const std::size_t sampleBytes,
                                ScalarLayout& out) noexcept
{
  if (!part.width || !part.height || part.width > kSizeMax / sampleBytes) return false;
  out.rowBytes = static_cast<std::size_t>(part.width) * sampleBytes;
  out.stride = part.stride ? part.stride : out.rowBytes;
  return true;
}

/**
 * Copies a scalar plane into @p values (and @p raw). Samples the part does not hold stay zero.
 * Returns false when the stride is below the row size or the payload is shorter than its layout.
 */
template <typename T>
bool copyScalarPart(const onegen::Frame::Part& part, float* values,
                    std::uint16_t* raw, std::uint64_t endianness)
{
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  for (std::size_t index = 0; index < count; ++index) {
    values[index] = 0.F;
    if (raw) raw[index] = 0;
  }
  ScalarLayout layout;
  if (!scalarLayout(part, sizeof(T), layout)) return false;
  bool complete = layout.stride >= layout.rowBytes
      && checkedExtent(layout.stride, part.height) != 0U
      && part.payload.size() >= layout.stride * part.height;
  const auto readable = (std::min)(layout.stride, layout.rowBytes);
  for (std::uint32_t y = 0; y < part.height; ++y) {
    if (layout.stride && y > kSizeMax / layout.stride) break;
    const auto row_start = static_cast<std::size_t>(y) * layout.stride;
    if (row_start >= part.payload.size()) break;
    const auto row_limit = (std::min)(readable, part.payload.size() - row_start);
    const auto* row = part.payload.data() + row_start;
    for (std::uint32_t x = 0; x < part.width; ++x) {
      if (static_cast<std::size_t>(x + 1U) * sizeof(T) > row_limit) break;
      const auto sample = readSample<T>(row + static_cast<std::size_t>(x) * sizeof(T), endianness);
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      values[index] = static_cast<float>(sample);
      if (raw) raw[index] = static_cast<std::uint16_t>(sample);
    }
  }
  return complete;
}

/** Copies one auxiliary part; a part the buffer does not fully hold keeps zeros and is counted. */
void writeAuxiliary(const AuxiliaryPart& source, GraphicsRangeFrameWriter& writer,
                    const std::uint64_t endianness, const Report& report)
{
  const auto& part = *source.part;
  bool confidence = false;
  std::uint8_t bits = 0;
  const auto format = auxiliaryFormat(part, confidence, bits);
  float* const values = confidence ? writer.confidenceData() : writer.intensityData();
  std::uint16_t* const raw = confidence
      ? writer.confidenceRawData() : writer.intensityRawData();
  std::uint8_t* const mask = confidence
      ? writer.confidenceValidMaskData() : writer.intensityValidMaskData();
  if (values == nullptr || format == AuxiliaryFormat::Unsupported) return;
  const auto count = static_cast<std::size_t>(part.width) * part.height;
  bool complete = true;

  if (format == AuxiliaryFormat::Float) {
    complete = copyScalarPart<float>(part, values, nullptr, endianness);
    if (mask != nullptr)
      for (std::size_t index = 0; index < count; ++index)
        mask[index] = std::isfinite(values[index]) ? 1U : 0U;
  } else if (format == AuxiliaryFormat::MaskBit) {
    complete = copyScalarPart<std::uint8_t>(part, values, nullptr, 0);
    for (std::size_t index = 0; index < count; ++index)
      values[index] = (static_cast<std::uint8_t>(values[index]) & 1U) ? 1.F : 0.F;
  } else if (format == AuxiliaryFormat::PackedBit) {
    const auto row_bytes = onegen::coordinate_row_bytes({1, 1, false, false, true}, part.width);
    const auto stride = part.stride ? part.stride : row_bytes;
    complete = row_bytes && stride >= row_bytes && checkedExtent(stride, part.height) != 0U
        && part.payload.size() >= stride * part.height;
    for (std::uint32_t y = 0; y < part.height; ++y) {
      const auto row_start = stride && y <= kSizeMax / stride ? static_cast<std::size_t>(y) * stride : kSizeMax;
      const auto available = row_start < part.payload.size()
          ? (std::min)((std::min)(stride, row_bytes), part.payload.size() - row_start) : 0U;
      for (std::uint32_t x = 0; x < part.width; ++x)
        values[static_cast<std::size_t>(y) * part.width + x] = available
            ? (onegen::packed_bits(part.payload.data() + row_start, available,
                                   static_cast<std::uint64_t>(x), 1) ? 1.F : 0.F)
            : 0.F;
    }
  } else if (format == AuxiliaryFormat::Byte) {
    complete = copyScalarPart<std::uint8_t>(part, values, nullptr, 0);
  } else {
    complete = copyScalarPart<std::uint16_t>(part, values, raw, endianness);
  }
  if (!complete)
    notePart(report, PlanarReason::AuxiliaryLayoutRejected, source.index, part.stride, part.stride, 0,
             part.payload.size(), 0);
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
  std::size_t rowBytes = 0;
  std::string system;
};

/** Outcome of planning a part as a range. */
enum class RangePlan {
  NotRange,    ///< The part is not a coordinate part.
  Planned,     ///< The range is planned; missing samples are zero-filled and marked invalid.
  Unplannable, ///< No range can be built from the part; its bytes are published as an image.
  TooLarge     ///< The range extent cannot be represented; the part is omitted and counted.
};

/**
 * Plans a coordinate part. A calibration, geometry or unit the range model cannot use never rejects
 * the data: scalar coordinates fall back to an uncalibrated pixel grid, an unknown unit keeps the
 * values as millimeters, and a multi-axis part without usable calibration is published as raw bytes.
 */
[[nodiscard]] RangePlan prepareRangeSpec(const onegen::Frame::Part& part,
                                         const onegen::Frame& source,
                                         const AuxiliaryPlan& auxiliary,
                                         PlannedRangeSpec& out,
                                         const Report& report,
                                         const std::optional<std::size_t> partIndex,
                                         RangeRejection* evidence = nullptr)
{
  const auto note = [&](PlanarReason reason, std::uint64_t reported = 0, std::uint64_t effective = 0,
                        std::uint64_t minimum = 0, std::uint64_t required = 0, std::uint64_t storage = 0) {
    notePart(report, reason, partIndex, reported, effective, minimum, part.payload.size(), required, storage);
    if (evidence) { evidence->reason = reason; evidence->requestedStorageBytes = static_cast<std::size_t>(storage); }
  };
  const auto layout = onegen::coordinate_format(part.data_format);
  // Coord3D is a range whether the producer labels the part 3D, 2D, or unknown.
  const bool rangePartType = is3dPart(part.data_type)
      || part.data_type == kPart2dImage || part.data_type == 0;
  if (!layout.components || !rangePartType) return RangePlan::NotRange;
  if (!part.width || !part.height || part.width > kIntMax || part.height > kIntMax
      || part.width > kSizeMax / part.height) {
    note(PlanarReason::RangeInvalidDimensions);
    return RangePlan::Unplannable;
  }
  std::uint64_t endianness = source.pixel_endianness;
  if (endianness > 2) {
    note(PlanarReason::InvalidLayout);
    endianness = 0;
  }
  // Scalar C without calibration is still an organized height map. A multi-axis part without
  // calibration cannot be placed in space, so its bytes are published as raw data instead.
  onegen::Frame::Scan3dGeometry pixelGridPreview;
  const bool pixelGridPreviewActive = !source.scan3d_geometry && layout.components == 1;
  if (pixelGridPreviewActive) pixelGridPreview.output_mode = "UncalibratedC";
  if (!source.scan3d_geometry && !pixelGridPreviewActive) {
    note(PlanarReason::MissingCalibration);
    return RangePlan::Unplannable;
  }
  const auto& geometry = pixelGridPreviewActive ? pixelGridPreview : *source.scan3d_geometry;
  auto mode = geometry.output_mode;
  auto system = geometry.coordinate_system;
  bool native = mode == "UncalibratedC";
  if (layout.components == 1) {
    if (!mode.empty() && mode != "CalibratedC" && mode != "RectifiedC"
        && mode != "DisparityC" && !native) {
      note(PlanarReason::UnsupportedGeometry);
      mode = "CalibratedC";
    }
  } else if (layout.components == 2) {
    if (mode != "CalibratedAC" && mode != "RectifiedAC") note(PlanarReason::UnsupportedGeometry);
  } else if (!mode.empty() && mode != "CalibratedABC_Grid" && mode != "CalibratedABC_PointCloud") {
    note(PlanarReason::UnsupportedGeometry);
  }
  if (!native && !system.empty() && system != "Cartesian" && system != "Spherical"
      && system != "Cylindrical") {
    note(PlanarReason::UnsupportedGeometry);
    system = "Cartesian";
  }
  if ((system == "Spherical" || system == "Cylindrical") && layout.components == 1
      && mode != "RectifiedC") {
    note(PlanarReason::UnsupportedGeometry);
    system = "Cartesian";
  }
  if (mode == "DisparityC" && (!std::isfinite(geometry.focal_length)
      || !std::isfinite(geometry.baseline) || geometry.focal_length == 0 || geometry.baseline == 0)) {
    note(PlanarReason::InvalidCalibration);
    mode = "CalibratedC";
  }
  const bool full_axes = layout.components > 1 || mode == "RectifiedC";
  bool calibrationUsable = true;
  for (std::size_t axis = full_axes ? 0 : 2; axis < 3; ++axis)
    if (!native && (!geometry.calibrated_axes[axis] || !std::isfinite(geometry.scale[axis])
        || !std::isfinite(geometry.offset[axis])))
      calibrationUsable = false;
  if (!calibrationUsable) {
    note(PlanarReason::InvalidCalibration);
    if (layout.components != 1) return RangePlan::Unplannable;
    native = true;
    mode = "UncalibratedC";
    system.clear();
  }

  GraphicsLengthUnit lengthUnit = GraphicsLengthUnit::Millimeter;
  const auto unit = lower(geometry.distance_unit);
  double unitScale = 1.0;
  if (!native) {
    if (unit == "millimeter" || unit == "mm") lengthUnit = GraphicsLengthUnit::Millimeter;
    else if (unit == "micrometer" || unit == "um") lengthUnit = GraphicsLengthUnit::Micrometer;
    else if (unit == "nanometer" || unit == "nm") lengthUnit = GraphicsLengthUnit::Nanometer;
    else if (unit == "meter" || unit == "m") { lengthUnit = GraphicsLengthUnit::Millimeter; unitScale = 1000.0; }
    else if (unit == "inch" || unit == "in") { lengthUnit = GraphicsLengthUnit::Millimeter; unitScale = 25.4; }
    else note(PlanarReason::UnsupportedUnit); // The values stay as delivered, labeled millimeters.
  }

  const bool disparity = mode == "DisparityC";
  const bool polar = system == "Spherical" || system == "Cylindrical";
  const bool project = disparity && std::isfinite(geometry.principal_u)
      && std::isfinite(geometry.principal_v);
  const bool explicitXY = layout.components > 1 || (disparity && project) || polar;
  const bool hasRangeRaw = layout.components == 1 && !layout.floating
      && layout.sample_bits <= 16 && !disparity && !polar;

  const auto row_bytes = onegen::coordinate_row_bytes(layout, part.width);
  if (!row_bytes) {
    note(PlanarReason::InvalidLayout);
    return RangePlan::Unplannable;
  }
  const auto stride = part.stride ? part.stride : row_bytes;
  const auto planes = layout.planar ? layout.components : 1U;
  if (part.stride && part.stride < row_bytes)
    note(PlanarReason::StrideBelowMinimum, part.stride, stride, row_bytes);
  const auto plane_bytes = checkedExtent(stride, part.height);
  if (!plane_bytes || plane_bytes > kSizeMax / planes) {
    note(PlanarReason::RangeSizeLimitExceeded, part.stride, stride, row_bytes);
    return RangePlan::TooLarge;
  }
  const auto required = plane_bytes * planes;
  if (part.payload.size() < required)
    note(PlanarReason::TruncatedPlane, part.stride, stride, row_bytes, required);

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
    if (writePlan.error() == GraphicsFrameError::Capacity) {
      note(PlanarReason::RangeSizeLimitExceeded);
      return RangePlan::TooLarge;
    }
    note(PlanarReason::RangeInvalidDimensions);
    return RangePlan::Unplannable;
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
  out.system = system;
  out.auxiliary = auxiliary;
  out.writePlan = writePlan.value();
  out.lengthUnit = lengthUnit;
  out.unitScale = unitScale;
  out.pixelEndianness = endianness;
  out.frameId = source.frame_id;
  out.rowBytes = row_bytes;
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
  return RangePlan::Planned;
}

std::optional<RangeFrame> writeRangeSpec(const PlannedRangeSpec& spec,
                                         GraphicsFrameBuilder& builder,
                                         std::string* diagnostic = nullptr,
                                         RangeRejection* evidence = nullptr,
                                         const Report& report = {})
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
  const auto& system = spec.system;

  for (std::uint32_t y = 0; y < part.height; ++y) {
    for (std::uint32_t x = 0; x < part.width; ++x) {
      const auto index = static_cast<std::size_t>(y) * part.width + x;
      bool valid = true;
      for (unsigned component = 0; component < layout.components; ++component) {
        const auto axis = layout.components == 3 ? component
            : (component == layout.components - 1 ? 2U : 0U);
        double sample = 0.0;
        // A sample the buffer does not hold is zero and invalid; it never reads past the payload.
        const bool backed = readCoordinate(part, layout, stride, spec.rowBytes, plane_bytes, x, y,
                                           component, spec.pixelEndianness, sample);
        const double world = spec.native ? sample : sample * geometry.scale[axis] + geometry.offset[axis];
        const auto stored = static_cast<float>(layout.components == 1 ? sample : world);
        valid = valid && backed && std::isfinite(sample) && std::isfinite(world)
            && std::isfinite(static_cast<float>(world)) && std::isfinite(stored)
            && !(geometry.invalid_enabled[axis] && sample == geometry.invalid_value[axis]);
        if (axis == 0) { if (xValues) xValues[index] = backed ? stored : 0.F; }
        else if (axis == 1) { if (yValues) yValues[index] = backed ? stored : 0.F; }
        else {
          zValues[index] = backed ? stored : 0.F;
          if (rangeRaw) rangeRaw[index] = backed ? static_cast<std::uint16_t>(sample) : 0U;
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
  // An auxiliary channel never removes the range: what its part lacks stays zero and is counted.
  for (const auto& auxiliaryPart : spec.auxiliary.parts)
    writeAuxiliary(auxiliaryPart, writer, spec.pixelEndianness, report);

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

struct PlannedPlanarRange
{
  PlannedRangeSpec range;
  std::size_t selected_index = 0;
  std::array<std::size_t, 3> consumed_indices{};
  std::vector<std::size_t> used_auxiliary_indices;
  std::size_t anchor_part = 0;
};

/**
 * Joins matching A/B/C planes into one planar range. Planes of a group that cannot be joined
 * (missing or duplicated axis, unusable layout) stay unconsumed and are published on their own.
 * A plane with a short buffer or a small stride is copied row by row into the joined payload,
 * which keeps zeros where the plane has no bytes.
 */
[[nodiscard]] std::vector<PlannedPlanarRange> preparePlanarRangeSpecs(
    const onegen::Frame& source, const Report& report)
{
  std::vector<PlannedPlanarRange> assemblies;
  bool groupAnomalyReported = false;
  for (std::size_t base_index = 0; base_index < source.parts.size(); ++base_index) {
    const auto& a = source.parts[base_index];
    if (!is3dPart(a.data_type)) continue;
    const auto* family = planarFamily(a.data_format);
    if (!family) continue;
    const auto& formats = *family;
    const bool primaryAxis = a.data_format == formats[0];
    if (!primaryAxis && (!report.enabled() || groupAnomalyReported)) continue;
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
    const auto record = [&](PlanarReason reason, std::optional<std::size_t> affectedAxis = std::nullopt,
                            std::size_t minimum = 0) {
      if (!report.enabled()) return;
      OneGenPlanarRejection evidence;
      evidence.reason = reason;
      evidence.anchorPart = base_index;
      for (std::size_t axis = 0; axis < planes.size(); ++axis)
        if (planes[axis]) evidence.matchedAxes |= 1U << axis;
      if (reason == PlanarReason::MissingAxis || reason == PlanarReason::DuplicateAxis) {
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
        if (requiredStride && plane.height <= kSizeMax / requiredStride)
          evidence.requiredBytes = requiredStride * plane.height;
      }
      report.part(evidence);
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
      record(!planes[0] || !ambiguous ? PlanarReason::MissingAxis : PlanarReason::DuplicateAxis);
      groupAnomalyReported = true;
      continue;
    }
    const auto layout = onegen::coordinate_format(formats[3]);
    const auto row_bytes = onegen::coordinate_row_bytes(
        {1, layout.sample_bits, false, layout.floating, layout.packed}, a.width);
    if (!row_bytes || !a.height || a.height > kSizeMax / row_bytes / 3U) {
      record(PlanarReason::InvalidLayout);
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
    joined.payload.assign(row_bytes * a.height * planes.size(), 0);
    for (std::size_t axis = 0; axis < planes.size(); ++axis) {
      const auto* plane = planes[axis];
      const auto stride = plane->stride ? plane->stride : row_bytes;
      const auto required = checkedExtent(stride, plane->height);
      if (stride < row_bytes) record(PlanarReason::StrideBelowMinimum, axis, row_bytes);
      if (!required || plane->payload.size() < required) record(PlanarReason::TruncatedPlane, axis, row_bytes);
      auto* target = joined.payload.data() + axis * row_bytes * a.height;
      for (std::uint32_t row = 0; row < plane->height; ++row) {
        if (stride && row > kSizeMax / stride) break;
        const auto offset = static_cast<std::size_t>(row) * stride;
        if (offset >= plane->payload.size()) break;
        std::memcpy(target + static_cast<std::size_t>(row) * row_bytes, plane->payload.data() + offset,
                    (std::min)({row_bytes, stride, plane->payload.size() - offset}));
      }
    }
    const auto auxiliary = planAuxiliary(source, a, a.width, a.height);
    PlannedRangeSpec rangeSpec;
    RangeRejection rangeEvidence;
    Report groupReport = report;
    groupReport.groupAnchor = base_index;
    groupReport.groupAxes = 0b111U;
    if (prepareRangeSpec(joined, source, auxiliary, rangeSpec, groupReport, base_index, &rangeEvidence)
        == RangePlan::Planned) {
      assemblies.push_back({std::move(rangeSpec), base_index, plane_indices,
                            auxiliary.used_indices, base_index});
    }
  }
  return assemblies;
}

/** One planned 2D image: the layout it is published with and how its bytes are copied. */
struct PlannedImageSpec
{
  bool isJpeg = false;
  /** True when the GraphicsEngine writer takes the source as delivered; otherwise the rows are fitted. */
  bool clean = false;
  GraphicsImageWriterSource writerSource{};
  std::uint64_t pfnc = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t stride = 0;       ///< Destination stride, valid for GraphicsEngine.
  std::size_t minimum = 0;      ///< Bytes of one row of pixels.
  std::size_t bytes = 0;        ///< Destination bytes, including zero fill.
  std::size_t alignment = 1;
  const std::uint8_t* data = nullptr;
  std::size_t size = 0;         ///< Source bytes that exist.
  std::size_t sourceStride = 0; ///< Row pitch of the source bytes.
  unsigned swapWord = 0;        ///< Bytes per word to byte-swap after the copy, or zero.
  bool sourceFormatKept = true; ///< False when the bytes are published as a substitute format.
  OneGenImageRejection::Conversion conversion = OneGenImageRejection::Conversion::Copy;
  GraphicsImagePixelFormat format = GraphicsImagePixelFormat::Unknown;
  std::uint64_t sequence = 0;
};

/** Source of one 2D image: a single payload, a multipart part or a part published as raw bytes. */
struct ImageInput
{
  std::uint64_t payloadType = 0;
  bool imagePresent = false;
  std::uint64_t pfnc = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t stride = 0;
  std::uint64_t endianness = 0;
  std::uint64_t sequence = 0;
  std::span<const std::uint8_t> payload;
};

/** Sets the 1-D raw layout used for bytes that cannot be given an image layout of their own. */
void planRawBytes(PlannedImageSpec& out, const std::size_t available)
{
  const auto width = (std::min)(available, kRawRowBytes);
  const auto rows = (available + width - 1U) / width;
  out.isJpeg = false;
  out.clean = false;
  out.format = GraphicsImagePixelFormat::Mono8;
  out.width = static_cast<std::uint32_t>(width);
  out.height = static_cast<std::uint32_t>(rows);
  out.stride = width;
  out.minimum = width;
  out.sourceStride = width;
  out.bytes = width * rows;
  out.alignment = 1U;
  out.swapWord = 0;
  out.sourceFormatKept = false;
  out.conversion = OneGenImageRejection::Conversion::Copy;
}

/**
 * Fits an image into a GraphicsEngine layout. Only an image without bytes is reported absent.
 * An unknown pixel format maps to the format with the same bytes per element, else to raw Mono8;
 * invalid dimensions publish the bytes in the 1-D raw layout; a short buffer is planned at its
 * declared size and zero-filled; a stride below the row size or beyond the integer range is rewritten
 * to a valid stride. Each of these is recorded in @p report.
 */
[[nodiscard]] bool planImage(const ImageInput& in, PlannedImageSpec& out, std::string* rejection_reason,
                             const Report& report, const std::optional<std::size_t> part)
{
  using Conversion = OneGenImageRejection::Conversion;
  const auto available = in.payload.size();
  const auto note = [&](std::optional<GraphicsImageFailure> failure, Conversion conversion,
                        std::uint64_t effective, std::uint64_t minimum, std::uint64_t required) {
    noteImage(report, part, failure, conversion, in.stride, effective, minimum, available, required);
  };
  const auto absent = [&](const char* reason, std::optional<GraphicsImageFailure> failure) {
    if (rejection_reason) *rejection_reason = reason;
    note(failure, Conversion::None, 0, 0, 0);
    return false;
  };
  if (in.payloadType == 4 && !in.imagePresent) return absent("chunk payload contains no image", std::nullopt);
  if (!available) return absent("empty payload", GraphicsImageFailure::MissingBytes);

  out = PlannedImageSpec{};
  out.pfnc = in.pfnc;
  out.sequence = in.sequence;
  out.data = in.payload.data();
  out.size = available;
  if (in.payloadType != 0 && in.payloadType != 1 && in.payloadType != 4 && in.payloadType != kPayloadJpeg)
    note(GraphicsImageFailure::UnsupportedFormat, Conversion::Copy, 0, 0, 0);

  if (in.payloadType == kPayloadJpeg) {
    // A compressed stream is copied as delivered; missing dimensions are reported, not rejected.
    out.isJpeg = true;
    out.clean = true;
    out.format = GraphicsImagePixelFormat::Jpeg;
    out.conversion = Conversion::JpegCopy;
    out.width = in.width && in.width <= kIntMax ? in.width : 1U;
    out.height = in.height && in.height <= kIntMax ? in.height : 1U;
    if (!in.width || !in.height || in.width > kIntMax || in.height > kIntMax)
      note(GraphicsImageFailure::InvalidDimensions, Conversion::JpegCopy, 0, 0, 0);
    out.stride = in.stride && in.stride <= kIntMax ? static_cast<std::size_t>(in.stride)
                                                   : (std::min)(available, kIntMax);
    out.bytes = available;
    out.alignment = 1U;
    out.writerSource.data = out.data;
    out.writerSource.size = available;
    return true;
  }

  auto format = pixelFormat(in.pfnc);
  bool substituted = false;
  unsigned swapWord = 0;
  std::size_t imageWidth = in.width;
  bool dimensionsValid = in.width && in.height && in.width <= kIntMax && in.height <= kIntMax;
  if (!dimensionsValid) note(GraphicsImageFailure::InvalidDimensions, Conversion::Copy, 0, 0, 0);
  const auto sizeBits = static_cast<unsigned>((in.pfnc >> 16U) & 0xffU);
  if (format == GraphicsImagePixelFormat::Unknown) {
    note(GraphicsImageFailure::UnsupportedFormat, Conversion::Copy, 0, 0, 0);
    substituted = true;
    const unsigned elementBytes = sizeBits && sizeBits % 8U == 0U ? sizeBits / 8U : 0U;
    switch (elementBytes) {
      case 1U: format = GraphicsImagePixelFormat::Mono8; break;
      case 2U: format = GraphicsImagePixelFormat::Mono16; break;
      case 3U: format = GraphicsImagePixelFormat::RGB24; break;
      case 4U: format = GraphicsImagePixelFormat::Mono32; break;
      default: format = GraphicsImagePixelFormat::Mono8; break;
    }
    if (elementBytes == 2U || elementBytes == 4U) swapWord = in.endianness == 2 ? elementBytes : 0U;
    if (dimensionsValid && (elementBytes == 0U || elementBytes > 4U)) {
      // Raw bytes of an unknown element: one image row per source row.
      if (sizeBits && in.width <= kSizeMax / sizeBits) imageWidth = (static_cast<std::size_t>(in.width) * sizeBits + 7U) / 8U;
      else if (in.stride && in.stride <= kIntMax) imageWidth = static_cast<std::size_t>(in.stride);
      else dimensionsValid = false;
    }
  } else if (in.endianness == 2 && !isByteOrdered(format)) {
    if (sizeBits == 10 || sizeBits == 12 || sizeBits == 14)
      note(GraphicsImageFailure::UnsupportedConversion, Conversion::None, 0, 0, 0); // Kept as delivered.
    else if (sizeBits == 32 || sizeBits % 16U == 0) swapWord = sizeBits == 32 ? 4U : 2U;
  }
  if (!dimensionsValid || imageWidth > kIntMax) { planRawBytes(out, available); return true; }

  const auto conversion = swapWord ? Conversion::SwapByteOrder : Conversion::Copy;
  auto minimum = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(imageWidth));
  if (GraphicsImageValidation::requiresEvenWidth(format) && (imageWidth & 1U) != 0U && minimum) {
    // An odd subsampled width cannot be addressed as pixels; its bytes are published as raw rows.
    note(GraphicsImageFailure::OddWidth, conversion, 0, minimum, 0);
    format = GraphicsImagePixelFormat::Mono8;
    imageWidth = minimum;
    substituted = true;
    swapWord = 0;
    if (imageWidth > kIntMax) { planRawBytes(out, available); return true; }
    minimum = GraphicsImageValidation::minimumRowBytes(format, static_cast<int>(imageWidth));
  }
  if (!minimum) {
    note(GraphicsImageFailure::ExtentOverflow, conversion, 0, 0, 0);
    planRawBytes(out, available);
    return true;
  }
  const std::size_t sourceStride = in.stride ? static_cast<std::size_t>(in.stride) : minimum;
  const auto height = in.height;
  const auto requiredStride = (std::max)(sourceStride, minimum);
  const auto requiredBytes = checkedExtent(requiredStride, height);
  if (!requiredBytes) {
    note(GraphicsImageFailure::ExtentOverflow, conversion, sourceStride, minimum, 0);
    planRawBytes(out, available);
    return true;
  }
  bool clean = true;
  if (sourceStride > kIntMax) {
    note(GraphicsImageFailure::InvalidStride, Conversion::None, sourceStride, minimum, requiredBytes);
    clean = false;
  } else if (sourceStride < minimum) {
    note(GraphicsImageFailure::StrideBelowMinimum, conversion, sourceStride, minimum, requiredBytes);
    clean = false;
  }
  if (available < requiredBytes) {
    note(GraphicsImageFailure::TruncatedBytes, conversion, sourceStride, minimum, requiredBytes);
    clean = false;
  }

  out.format = format;
  out.width = static_cast<std::uint32_t>(imageWidth);
  out.height = height;
  out.minimum = minimum;
  out.sourceStride = sourceStride;
  out.sourceFormatKept = !substituted;
  out.conversion = conversion;
  out.swapWord = swapWord;
  if (clean) {
    out.writerSource.data = out.data;
    out.writerSource.size = available;
    out.writerSource.width = static_cast<int>(imageWidth);
    out.writerSource.height = static_cast<int>(height);
    out.writerSource.stride = static_cast<int>(sourceStride);
    out.writerSource.pixelFormat = format;
    out.writerSource.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(format);
    out.writerSource.sourceFormat = static_cast<int>(in.pfnc);
    out.writerSource.declaredMonoSourceBits = substituted ? 0U : monoBits(in.pfnc);
    out.writerSource.frameSequence = in.sequence;
    out.writerSource.operation = swapWord ? GraphicsImageWriterOperation::SwapByteOrder
                                          : GraphicsImageWriterOperation::Copy;
    if (swapWord && graphicsImagePixelFormatInfo(format).componentBits != swapWord * 8U)
      out.writerSource.pixelFormat = swapWord == 4U ? GraphicsImagePixelFormat::Mono32
                                                    : GraphicsImagePixelFormat::Mono16;
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    const auto plan = planGraphicsImageWrite(out.writerSource, &failure);
    if (plan) {
      out.clean = true;
      out.stride = static_cast<std::size_t>(plan.value().strideBytes);
      out.bytes = plan.value().bytes;
      out.alignment = plan.value().alignment;
      return true;
    }
  }
  // The rows are rewritten into a valid stride at the declared size; missing bytes stay zero.
  out.clean = false;
  out.stride = sourceStride >= minimum && sourceStride <= kIntMax ? sourceStride : minimum;
  out.bytes = checkedExtent(out.stride, height);
  const auto& info = graphicsImagePixelFormatInfo(format);
  out.alignment = info.componentBits >= 32U ? 4U : info.componentBits >= 16U ? 2U : 1U;
  if (!out.bytes || out.stride > kIntMax
      || GraphicsImageValidation::layoutFailure(format, static_cast<int>(imageWidth), static_cast<int>(height),
                                                static_cast<int>(out.stride), out.bytes)
          != GraphicsImageFailure::None) {
    planRawBytes(out, available);
  }
  return true;
}

[[nodiscard]] GraphicsImage describeImage(const PlannedImageSpec& spec, GraphicsSpan<std::uint8_t> pixels)
{
  GraphicsImage image;
  image.pixels = std::move(pixels);
  image.width = static_cast<int>(spec.width);
  image.height = static_cast<int>(spec.height);
  image.stride = static_cast<int>(spec.stride);
  image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(spec.format);
  image.pixelFormat = spec.format;
  image.sourceFormat = static_cast<int>(spec.pfnc);
  if (spec.sourceFormatKept) {
    image.declaredMonoSourceBits = monoBits(spec.pfnc);
    const auto effectiveBits = static_cast<int>((spec.pfnc >> 16U) & 0xffU);
    image.bitAlignment = effectiveBits > image.bitsPerPixel
        || (spec.format == GraphicsImagePixelFormat::Mono16
            && image.declaredMonoSourceBits > 0 && image.declaredMonoSourceBits < 16)
        ? GraphicsImageBitAlignment::LeastSignificant : GraphicsImageBitAlignment::Packed;
  }
  image.frameSequence = spec.sequence;
  return image;
}

/** Copies what the source holds row by row, never past its end; the rest of the destination is zero. */
void copyImageRows(std::uint8_t* target, const PlannedImageSpec& spec) noexcept
{
  for (std::size_t row = 0; row < spec.height; ++row) {
    if (spec.sourceStride && row > kSizeMax / spec.sourceStride) break;
    const auto offset = row * spec.sourceStride;
    if (offset >= spec.size) break;
    std::memcpy(target + row * spec.stride, spec.data + offset,
                (std::min)({spec.minimum, spec.sourceStride, spec.size - offset}));
  }
  if (!spec.swapWord) return;
  for (std::size_t row = 0; row < spec.height; ++row) {
    auto* bytes = target + row * spec.stride;
    for (std::size_t offset = 0; offset + spec.swapWord <= spec.minimum; offset += spec.swapWord)
      std::reverse(bytes + offset, bytes + offset + spec.swapWord);
  }
}

[[nodiscard]] std::optional<GraphicsImage> write2dImageSpec(
    const PlannedImageSpec& spec,
    GraphicsFrameBuilder& builder,
    std::string* rejection_reason,
    const Report& report,
    const std::optional<std::size_t> part)
{
  const auto refused = [&]() {
    if (rejection_reason) *rejection_reason = "image storage refused";
    noteImage(report, part, GraphicsImageFailure::StorageRefused, spec.conversion, spec.sourceStride,
              spec.stride, spec.minimum, spec.size, spec.bytes);
    return std::optional<GraphicsImage>{};
  };
  if (spec.isJpeg) {
    auto copied = builder.copy<std::uint8_t>(spec.writerSource.data, spec.writerSource.size);
    if (!copied) return refused();
    return describeImage(spec, std::move(copied).value());
  }
  if (spec.clean) {
    GraphicsImageFailure failure = GraphicsImageFailure::None;
    auto written = writeGraphicsImageWithFailure(builder, spec.writerSource, &failure);
    if (!written.isValid()) {
      if (failure == GraphicsImageFailure::StorageRefused) return refused();
      if (rejection_reason) *rejection_reason = "invalid image layout";
      return std::nullopt;
    }
    return describeImage(spec, std::move(written.pixels));
  }
  auto reserved = builder.reserve<std::uint8_t>(spec.bytes, spec.alignment);
  if (!reserved) return refused();
  GraphicsWriteRegion<std::uint8_t> region = std::move(reserved).value();
  std::memset(region.data(), 0, region.size());
  copyImageRows(region.data(), spec);
  return describeImage(spec, builder.seal(std::move(region)));
}

/**
 * Plans a scalar float confidence part as a native preview range. Missing samples stay zero and the
 * shortfall is counted. Returns false only when the extent cannot be a range, so the bytes are
 * published as an image instead.
 */
[[nodiscard]] bool prepareConfidencePreviewSpec(const onegen::Frame::Part& part,
                                                std::uint64_t pixel_endianness,
                                                GraphicsRangeWriteLayout& outPlan,
                                                const Report& report,
                                                const std::optional<std::size_t> partIndex)
{
  if (!part.width || !part.height || part.width > kIntMax || part.height > kIntMax
      || part.width > kSizeMax / 4U) return false;
  if (pixel_endianness > 2) notePart(report, PlanarReason::InvalidLayout, partIndex);
  ScalarLayout layout;
  if (!scalarLayout(part, sizeof(float), layout)) return false;
  if (layout.stride < layout.rowBytes)
    notePart(report, PlanarReason::StrideBelowMinimum, partIndex, part.stride, layout.stride, layout.rowBytes,
             part.payload.size());
  const auto required = checkedExtent(layout.stride, part.height);
  if (!required) return false;
  if (part.payload.size() < required)
    notePart(report, PlanarReason::TruncatedPlane, partIndex, part.stride, layout.stride, layout.rowBytes,
             part.payload.size(), required);
  GraphicsRangeFrameLayout rangeLayout;
  rangeLayout.width = static_cast<int>(part.width);
  rangeLayout.height = static_cast<int>(part.height);
  rangeLayout.channels.validMask = true;
  const auto plan = planGraphicsRangeWrite(rangeLayout);
  if (!plan) return false;
  outPlan = plan.value();
  return true;
}

[[nodiscard]] std::optional<RangeFrame> writeConfidencePreview(
    const onegen::Frame::Part& part, std::uint64_t pixel_endianness,
    const GraphicsRangeWriteLayout& plan, GraphicsFrameBuilder& builder)
{
  GraphicsRangeFrameWriter writer(builder, plan.source);
  if (!writer.isValid()) return std::nullopt;
  copyScalarPart<float>(part, writer.zData(), nullptr, pixel_endianness > 2 ? 0 : pixel_endianness);
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

/** Records the transport repairs acquisition made, so a repaired buffer is counted with the frame. */
void noteRepairs(const Report& report, const onegen::Frame& source) noexcept
{
  const auto repaired = source.buffer_info.repaired;
  if (!repaired) return;
  for (std::uint32_t bit = 1U; bit != 0U && bit <= repaired; bit <<= 1U)
    if (repaired & bit) notePart(report, PlanarReason::BufferRepaired, std::nullopt);
}

} // namespace

std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameContextSource& context_source,
    std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection,
    std::optional<OneGenPlanarRejection>* planar_rejection,
    const std::string& source_name,
    GraphicsFrameError* storage_error,
    OneGenFrameAnomalies* anomalies)
{
  if (rejection_reason) rejection_reason->clear();
  if (image_rejection) image_rejection->reset();
  if (planar_rejection) planar_rejection->reset();
  if (storage_error) *storage_error = GraphicsFrameError::None;
  if (anomalies) *anomalies = OneGenFrameAnomalies{};
  const Report report{anomalies, image_rejection, planar_rejection};
  noteRepairs(report, source);

  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsFrame>{};
  };

  if (!source.parts.empty()) {
    // Only a part without bytes, or one whose extent cannot be represented, is omitted and described.
    std::string droppedParts;
    const auto dropped = [&](std::size_t index, const std::string& reason) {
      const auto& part = source.parts[index];
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
      std::size_t partIndex = 0;
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
    std::vector<bool> represented(source.parts.size(), false);

    auto appendBytes = [&](std::size_t bytes, std::size_t alignment) -> bool {
      return appendAligned(totalBytes, bytes, alignment);
    };
    const auto groupOf = [&](const onegen::Frame::Part& part) {
      return std::to_string(part.source_id) + ":" + std::to_string(part.region_id);
    };
    const auto addImage = [&](std::size_t index, PlannedImageSpec&& imageSpec) {
      const auto& part = source.parts[index];
      const auto group = groupOf(part);
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
          "image." + group + "." + std::to_string(index), std::move(displayName), group, index});
    };
    // Plans a part as an image; an extent the allocation bounds cannot hold is the only omission.
    const auto planPartImage = [&](std::size_t index) {
      const auto& part = source.parts[index];
      ImageInput input;
      input.pfnc = part.data_format;
      input.width = part.width;
      input.height = part.height;
      input.stride = part.stride;
      input.endianness = source.pixel_endianness;
      input.sequence = sequence;
      input.payload = part.payload;
      std::string imageDiagnostic;
      PlannedImageSpec imageSpec;
      represented[index] = true;
      if (!planImage(input, imageSpec, &imageDiagnostic, report, index)) {
        dropped(index, imageDiagnostic);
        return;
      }
      if (!appendBytes(imageSpec.bytes, imageSpec.alignment)) {
        notePart(report, PlanarReason::RangeSizeLimitExceeded, index);
        dropped(index, "image size exceeds supported allocation bounds");
        return;
      }
      addImage(index, std::move(imageSpec));
    };

    for (auto& assembly : preparePlanarRangeSpecs(source, report)) {
      for (const std::size_t index : assembly.consumed_indices) { consumed_parts[index] = true; represented[index] = true; }
      if (!appendBytes(assembly.range.writePlan.totalBytes, assembly.range.writePlan.alignment)) {
        notePart(report, PlanarReason::RangeSizeLimitExceeded, assembly.selected_index);
        dropped(assembly.selected_index, "range size exceeds supported allocation bounds");
        continue;
      }
      const auto& selected = source.parts[assembly.selected_index];
      const auto group = groupOf(selected);
      const auto id = "range." + group + "." + std::to_string(assembly.selected_index);
      for (const std::size_t index : assembly.used_auxiliary_indices) confidence_used[index] = true;
      plannedRanges.push_back({std::move(assembly.range), id, "Range", group, true, assembly.anchor_part});
    }

    for (std::size_t index = 0; index < source.parts.size(); ++index) {
      const auto& part = source.parts[index];
      if (!consumed_parts[index]) {
        const auto auxiliary = planAuxiliary(source, part, part.width, part.height);
        PlannedRangeSpec spec;
        const auto rangePlan = prepareRangeSpec(part, source, auxiliary, spec, report, index);
        if (rangePlan == RangePlan::Planned) {
          if (appendBytes(spec.writePlan.totalBytes, spec.writePlan.alignment)) {
            const auto group = groupOf(part);
            const auto id = "range." + group + "." + std::to_string(index);
            for (const std::size_t auxiliaryIndex : auxiliary.used_indices)
              confidence_used[auxiliaryIndex] = true;
            plannedRanges.push_back({std::move(spec), id, "Range", group, false, index});
          } else {
            notePart(report, PlanarReason::RangeSizeLimitExceeded, index);
            dropped(index, "range size exceeds supported allocation bounds");
          }
          represented[index] = true;
        } else if (rangePlan == RangePlan::TooLarge) {
          dropped(index, "range size exceeds supported allocation bounds");
          represented[index] = true;
        }
      }
      if (part.data_type == kPart2dImage && !represented[index]
          && !onegen::coordinate_format(part.data_format).components)
        planPartImage(index);
    }
    for (std::size_t imageIndex = 0; imageIndex < plannedImages.size() && plannedImages.size() > 1; ++imageIndex)
      if (plannedImages[imageIndex].displayName == "Image")
        plannedImages[imageIndex].displayName = "Image " + std::to_string(imageIndex + 1);

    if (plannedRanges.empty() && plannedImages.empty()) {
      for (std::size_t index = 0; index < source.parts.size(); ++index) {
        const auto& part = source.parts[index];
        if (part.data_format != kConfidence32f || represented[index] || part.payload.empty()) continue;
        GraphicsRangeWriteLayout confPlan;
        if (prepareConfidencePreviewSpec(part, source.pixel_endianness, confPlan, report, index)) {
          confidence_used[index] = true;
          represented[index] = true;
          if (!appendBytes(confPlan.totalBytes, confPlan.alignment)) {
            notePart(report, PlanarReason::RangeSizeLimitExceeded, index);
            dropped(index, "confidence size exceeds supported allocation bounds");
            continue;
          }
          const auto group = groupOf(part);
          plannedConfidences.push_back({index, confPlan, "confidence." + group + "." + std::to_string(index), group});
        }
      }
    }
    // Everything else that holds bytes is published as an image fitted to the nearest format.
    for (std::size_t index = 0; index < source.parts.size(); ++index) {
      if (represented[index] || confidence_used[index]) continue;
      if (source.parts[index].payload.empty()) {
        dropped(index, "part has no data");
        continue;
      }
      planPartImage(index);
    }

    if (plannedRanges.empty() && plannedImages.empty() && plannedConfidences.empty()) {
      if (rejection_reason && !droppedParts.empty()) {
        *rejection_reason = "multipart payload has no part with data; " + droppedParts;
        return std::nullopt;
      }
      return reject("multipart payload has no part with data");
    }

    auto contextResult = context_source.beginFrame(totalBytes);
    if (!contextResult) {
      if (storage_error) *storage_error = contextResult.error();
      for (const auto& item : plannedRanges) {
        if (!item.planar) continue;
        notePart(report, PlanarReason::RangeStorageRefused, item.anchorPart, 0, 0, 0, 0, 0,
                 item.spec.writePlan.totalBytes);
        break;
      }
      if (rejection_reason) *rejection_reason = frameErrorMessage(contextResult.error());
      return std::nullopt;
    }

    GraphicsFrameBuilder builder(contextResult.value());
    GraphicsFrameContent result;
    for (const auto& item : plannedRanges) {
      std::string rangeDiagnostic;
      RangeRejection rangeEvidence;
      if (auto range = writeRangeSpec(item.spec, builder, &rangeDiagnostic, &rangeEvidence, report)) {
        result.addRange(item.id, std::move(*range), item.displayName, item.group);
      } else {
        notePart(report, rangeEvidence.reason, item.anchorPart, 0, 0, 0, 0, 0, rangeEvidence.requestedStorageBytes);
        dropped(item.anchorPart, rangeDiagnostic.empty() ? "range write failed" : rangeDiagnostic);
      }
    }
    for (const auto& item : plannedImages) {
      std::string imageDiagnostic;
      if (auto image = write2dImageSpec(item.spec, builder, &imageDiagnostic, report, item.partIndex))
        result.addImage(item.id, std::move(*image), item.displayName, item.group);
      else
        dropped(item.partIndex, imageDiagnostic.empty() ? "image write failed" : imageDiagnostic);
    }
    for (const auto& item : plannedConfidences) {
      if (auto range = writeConfidencePreview(source.parts[item.partIndex], source.pixel_endianness, item.plan, builder))
        result.addRange(item.id, std::move(*range), "Confidence", item.group);
      else
        dropped(item.partIndex, "confidence write failed");
    }
    result.metadata.sourceName = source_name;
    result.metadata.frameIndex = sequence;
    if (source.frame_id) result.metadata.frameId = std::to_string(source.frame_id);
    if (rejection_reason) *rejection_reason = droppedParts;
    if (!result.isValid()) {
      if (rejection_reason && !droppedParts.empty())
        *rejection_reason = "multipart payload has no part with data; " + droppedParts;
      else
        reject("multipart payload has no part with data");
      return std::nullopt;
    }
    builder.content() = std::move(result);
    auto published = std::move(builder).publish();
    if (!published) return reject("frame publication failed");
    return std::move(published).value();
  }

  // Single-payload frame: fit and plan first, then call beginFrame(totalBytes) once.
  const auto coordinateLayout = onegen::coordinate_format(source.pixel_format);
  if (source.payload_type == 4 && !source.buffer_info.image_present) {
    report.image(OneGenImageRejection{std::nullopt, OneGenImageRejection::Conversion::None, 0, 0, 0});
    return reject("chunk payload contains no image");
  }

  const auto finishFrame = [&](GraphicsFrameBuilder& builder) -> std::optional<GraphicsFrame> {
    GraphicsFrameMetadata metadata;
    metadata.sourceName = source_name;
    metadata.frameIndex = sequence;
    if (source.frame_id) metadata.frameId = std::to_string(source.frame_id);
    builder.setMetadata(std::move(metadata));
    auto published = std::move(builder).publish();
    if (!published) return reject("frame publication failed");
    return std::move(published).value();
  };

  if (coordinateLayout.components || source.pixel_format == kConfidence32f) {
    const auto row_bytes = coordinateLayout.components
        ? onegen::coordinate_row_bytes(coordinateLayout, source.width)
        : (static_cast<std::uint64_t>(source.width)
               > static_cast<std::uint64_t>(kSizeMax / 4U)
            ? 0 : static_cast<std::size_t>(source.width) * 4U);
    onegen::Frame::Part part;
    part.payload = source.payload;
    part.width = source.width;
    part.height = source.height;
    part.stride = source.stride ? source.stride : row_bytes;
    part.data_format = source.pixel_format;
    part.data_type = kPart3dImage;

    if (source.pixel_format == kConfidence32f) {
      GraphicsRangeWriteLayout confPlan;
      if (!part.payload.empty() && prepareConfidencePreviewSpec(part, source.pixel_endianness, confPlan, report, std::nullopt)) {
        auto contextResult = context_source.beginFrame(confPlan.totalBytes);
        if (!contextResult) {
          if (storage_error) *storage_error = contextResult.error();
          return reject(frameErrorMessage(contextResult.error()));
        }
        GraphicsFrameBuilder builder(contextResult.value());
        auto preview = writeConfidencePreview(part, source.pixel_endianness, confPlan, builder);
        if (!preview) return reject("confidence preview could not be written");
        builder.addRange("confidence", std::move(*preview), "Confidence");
        return finishFrame(builder);
      }
    } else {
      PlannedRangeSpec rangeSpec;
      RangeRejection rangeEvidence;
      const auto rangePlan = part.payload.empty() ? RangePlan::Unplannable
          : prepareRangeSpec(part, source, {}, rangeSpec, report, std::nullopt, &rangeEvidence);
      if (rangePlan == RangePlan::TooLarge) return reject("range size exceeds supported allocation bounds");
      if (rangePlan == RangePlan::Planned) {
        auto contextResult = context_source.beginFrame(rangeSpec.writePlan.totalBytes);
        if (!contextResult) {
          if (storage_error) *storage_error = contextResult.error();
          return reject(frameErrorMessage(contextResult.error()));
        }
        GraphicsFrameBuilder builder(contextResult.value());
        std::string diagnostic;
        auto range = writeRangeSpec(rangeSpec, builder, &diagnostic, nullptr, report);
        if (!range) {
          if (rejection_reason) *rejection_reason = diagnostic;
          return std::nullopt;
        }
        builder.addRange("range", std::move(*range), "Range");
        return finishFrame(builder);
      }
    }
    // The bytes cannot be a range: they fall through and are published as an image.
  }

  ImageInput input;
  input.payloadType = source.payload_type;
  input.imagePresent = source.buffer_info.image_present;
  input.pfnc = source.pixel_format;
  input.width = source.width;
  input.height = source.height;
  input.stride = source.stride;
  input.endianness = source.pixel_endianness;
  input.sequence = sequence;
  input.payload = source.payload;
  PlannedImageSpec imageSpec;
  if (!planImage(input, imageSpec, rejection_reason, report, std::nullopt)) return std::nullopt;

  auto contextResult = context_source.beginFrame(imageSpec.bytes);
  if (!contextResult) {
    if (storage_error) *storage_error = contextResult.error();
    report.image(OneGenImageRejection{GraphicsImageFailure::StorageRefused, imageSpec.conversion,
        imageSpec.stride, imageSpec.minimum, imageSpec.bytes});
    return reject(frameErrorMessage(contextResult.error()));
  }

  GraphicsFrameBuilder builder(contextResult.value());
  auto image = write2dImageSpec(imageSpec, builder, rejection_reason, report, std::nullopt);
  if (!image) return std::nullopt;
  builder.addImage("image", std::move(*image), "Image");
  return finishFrame(builder);
}

std::optional<GraphicsFrame> oneGenBuildDirectImageFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameBuilder& builder, GraphicsWriteRegion<std::uint8_t> region,
    std::size_t used_bytes, std::string* rejection_reason,
    std::optional<OneGenImageRejection>* image_rejection,
    const std::string& source_name,
    OneGenFrameAnomalies* anomalies)
{
  if (rejection_reason) rejection_reason->clear();
  if (image_rejection) image_rejection->reset();
  if (anomalies) *anomalies = OneGenFrameAnomalies{};
  const Report report{anomalies, image_rejection, nullptr};
  noteRepairs(report, source);

  using Conversion = OneGenImageRejection::Conversion;
  const auto reject = [rejection_reason](const char* reason) {
    if (rejection_reason) *rejection_reason = reason;
    return std::optional<GraphicsFrame>{};
  };
  const auto note = [&](GraphicsImageFailure failure, Conversion conversion = Conversion::None,
                        std::uint64_t stride = 0, std::uint64_t minimum = 0) {
    const auto requiredStride = (std::max)(stride, minimum);
    const auto required = requiredStride && source.height <= (std::numeric_limits<std::uint64_t>::max)() / requiredStride
        ? requiredStride * source.height : 0;
    noteImage(report, std::nullopt, failure, conversion, source.stride, stride, minimum, used_bytes, required);
  };

  if (source.payload_type == 4 && !source.buffer_info.image_present) {
    report.image(OneGenImageRejection{std::nullopt, Conversion::None, 0, 0, 0});
    return reject("chunk payload contains no image");
  }
  if (used_bytes == 0U) {
    note(GraphicsImageFailure::MissingBytes);
    return reject("empty direct payload");
  }
  if (used_bytes > region.size()) {
    note(GraphicsImageFailure::TruncatedBytes);
    used_bytes = region.size();
  }

  // The producer already wrote the raw bytes into the region. The frame is published from them in
  // place; when its layout cannot describe the region, the same bytes become a 1-D raw image.
  const bool isJpeg = source.payload_type == kPayloadJpeg;
  const auto declared = isJpeg ? GraphicsImagePixelFormat::Jpeg : pixelFormat(source.pixel_format);
  const auto sizeBits = static_cast<unsigned>((source.pixel_format >> 16U) & 0xffU);
  const bool byteSwap = source.pixel_endianness == 2 && !isByteOrdered(declared);
  if (source.payload_type != 0 && source.payload_type != 1 && source.payload_type != 4 && !isJpeg)
    note(GraphicsImageFailure::UnsupportedFormat);
  if (declared == GraphicsImagePixelFormat::Unknown) note(GraphicsImageFailure::UnsupportedFormat);
  if (byteSwap && (sizeBits == 10 || sizeBits == 12 || sizeBits == 14))
    note(GraphicsImageFailure::UnsupportedConversion);
  const bool swapWords = byteSwap && declared != GraphicsImagePixelFormat::Unknown
      && (sizeBits == 32 || sizeBits % 16U == 0);
  const std::size_t swapWord = sizeBits == 32 ? 4U : 2U;
  auto layoutFormat = declared;
  if (swapWords && graphicsImagePixelFormatInfo(declared).componentBits != swapWord * 8U)
    layoutFormat = swapWord == 4U ? GraphicsImagePixelFormat::Mono32 : GraphicsImagePixelFormat::Mono16;

  const auto minimum = isJpeg ? 0U
      : GraphicsImageValidation::minimumRowBytes(layoutFormat, static_cast<int>(source.width));
  const std::uint64_t stride = isJpeg ? (source.stride ? source.stride : used_bytes)
      : (source.stride ? source.stride : minimum);
  const bool dimensionsValid = source.width && source.height
      && source.width <= kIntMax && source.height <= kIntMax;
  if (!dimensionsValid) note(GraphicsImageFailure::InvalidDimensions);
  if (stride > kIntMax) note(GraphicsImageFailure::InvalidStride, Conversion::None, stride, minimum);
  else if (!isJpeg && stride < minimum) note(GraphicsImageFailure::StrideBelowMinimum, Conversion::None, stride, minimum);
  const auto failure = GraphicsImageValidation::layoutFailure(
      layoutFormat, static_cast<int>((std::min<std::uint64_t>)(source.width, kIntMax)),
      static_cast<int>((std::min<std::uint64_t>)(source.height, kIntMax)),
      static_cast<int>((std::min<std::uint64_t>)(stride, kIntMax)), used_bytes);
  const bool fits = dimensionsValid && failure == GraphicsImageFailure::None && stride <= kIntMax
      && declared != GraphicsImagePixelFormat::Unknown;
  if (!fits && failure != GraphicsImageFailure::None && declared != GraphicsImagePixelFormat::Unknown
      && dimensionsValid)
    note(failure, swapWords ? Conversion::SwapByteOrder : isJpeg ? Conversion::JpegCopy : Conversion::Copy,
         stride, minimum);

  GraphicsImage image;
  if (fits) {
    if (swapWords) {
      std::uint8_t* const bytes = region.data();
      const auto strideBytes = static_cast<std::size_t>(stride);
      std::size_t rowStart = 0U;
      for (std::uint64_t row = 0U; row < source.height && rowStart < used_bytes; ++row) {
        const std::size_t rowBytes = std::min(strideBytes, used_bytes - rowStart);
        for (std::size_t offset = 0U; offset + swapWord <= rowBytes; offset += swapWord)
          std::reverse(bytes + rowStart + offset, bytes + rowStart + offset + swapWord);
        rowStart += strideBytes;
      }
    }
    image.pixels = builder.seal(std::move(region)).subspan(0, used_bytes);
    image.width = static_cast<int>(source.width);
    image.height = static_cast<int>(source.height);
    image.stride = static_cast<int>(stride);
    image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(declared);
    image.declaredMonoSourceBits = monoBits(source.pixel_format);
    image.sourceFormat = static_cast<int>(source.pixel_format);
    image.pixelFormat = declared;
    const auto effectiveBits = static_cast<int>((source.pixel_format >> 16U) & 0xffU);
    image.bitAlignment = effectiveBits > image.bitsPerPixel
        || (declared == GraphicsImagePixelFormat::Mono16
            && image.declaredMonoSourceBits > 0 && image.declaredMonoSourceBits < 16)
        ? GraphicsImageBitAlignment::LeastSignificant : GraphicsImageBitAlignment::Packed;
  } else {
    // The region holds the delivered bytes exactly: publish them as a 1-D raw row.
    const auto width = (std::min)(used_bytes, kIntMax);
    image.pixels = builder.seal(std::move(region)).subspan(0, width);
    image.width = static_cast<int>(width);
    image.height = 1;
    image.stride = static_cast<int>(width);
    image.bitsPerPixel = GraphicsImageValidation::packedBitsPerPixel(GraphicsImagePixelFormat::Mono8);
    image.sourceFormat = static_cast<int>(source.pixel_format);
    image.pixelFormat = GraphicsImagePixelFormat::Mono8;
  }
  image.frameSequence = sequence;
  if (!image.isValid()) return reject("image layout could not be fitted");

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
