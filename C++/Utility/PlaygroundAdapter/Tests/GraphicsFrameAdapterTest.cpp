#include "OneGenGraphicsFrameAdapter.h"
#include <type_traits>
#include <array>
#include <algorithm>
#include <bit>
#include <memory>
#include <span>

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

template <typename T>
bool samplesEqual(const GraphicsSpan<T>& actual, const std::vector<T>& expected)
{
  return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin());
}

static_assert(std::is_const_v<std::remove_reference_t<decltype(*std::declval<GraphicsSpan<std::uint8_t>>().data())>>);

class RecordingContextSource final : public GraphicsFrameContextSource
{
public:
  std::size_t calls = 0;
  std::size_t allocations = 0;
  std::vector<std::size_t> beginSizes;
  std::vector<std::size_t> sizes;
  std::vector<const std::uint8_t*> buffers;

  [[nodiscard]] GraphicsResult<GraphicsFrameContext> beginFrame(
      const std::size_t sampleBytes) noexcept override
  {
    ++calls;
    beginSizes.push_back(sampleBytes);
    class RecordingAllocator final : public GraphicsFrameAllocator
    {
    public:
      explicit RecordingAllocator(RecordingContextSource* owner) : m_owner(owner) {}
      GraphicsResult<GraphicsAllocationSlice> allocate(
          const std::size_t bytes, const std::size_t alignment) noexcept override
      {
        ++m_owner->allocations;
        m_owner->sizes.push_back(bytes);
        auto allocation = GraphicsFrameContext::local().allocate(bytes, alignment);
        if (allocation) {
          m_owner->buffers.push_back(reinterpret_cast<const std::uint8_t*>(
              allocation.value().backing->base() + allocation.value().offset));
        }
        return allocation;
      }
    private:
      RecordingContextSource* m_owner;
    };
    return GraphicsFrameContext(std::make_shared<RecordingAllocator>(this));
  }
};

class RefusingContextSource final : public GraphicsFrameContextSource
{
public:
  explicit RefusingContextSource(GraphicsFrameError error = GraphicsFrameError::ClassRefused) noexcept
      : m_error(error)
  {
  }

  [[nodiscard]] GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t) noexcept override
  {
    return m_error;
  }

private:
  GraphicsFrameError m_error;
};

class FailingNthAllocationContextSource final : public GraphicsFrameContextSource
{
public:
  explicit FailingNthAllocationContextSource(const std::size_t failingAllocation) noexcept
      : m_failing(failingAllocation)
  {
  }

  [[nodiscard]] GraphicsResult<GraphicsFrameContext> beginFrame(std::size_t) noexcept override
  {
    class Allocator final : public GraphicsFrameAllocator
    {
    public:
      explicit Allocator(std::size_t failing) : m_failing(failing) {}
      GraphicsResult<GraphicsAllocationSlice> allocate(
          const std::size_t bytes, const std::size_t alignment) noexcept override
      {
        if (++m_count == m_failing) return GraphicsFrameError::Capacity;
        return GraphicsFrameContext::local().allocate(bytes, alignment);
      }
    private:
      std::size_t m_failing;
      std::size_t m_count = 0;
    };
    return GraphicsFrameContext(std::make_shared<Allocator>(m_failing));
  }

private:
  std::size_t m_failing;
};

} // namespace

int main()
{
  GraphicsLocalFrameContextSource localSource;

  onegen::Frame source;
  source.width = 2;
  source.height = 2;
  source.stride = 4;
  source.pixel_format = 0x01080001;
  source.payload = {1, 2, 0, 0, 3, 4, 0, 0};

  const auto frame = oneGenGraphicsFrame(source, 7, localSource);
  assert(frame && frame->hasImage());
  assert(frame->images().front().payload.pixelFormat == GraphicsImagePixelFormat::Mono8);
  assert(frame->images().front().payload.stride == 4);
  assert(frame->images().front().payload.frameSequence == 7);

  // Direct destination path: beginFrame(slotBytes) once -> reserve<uint8_t> -> seal -> subspan(0, usedBytes) with 0 copies.
  {
    onegen::Frame directMeta;
    directMeta.width = 2;
    directMeta.height = 2;
    directMeta.stride = 2;
    directMeta.pixel_format = 0x01080009;
    RecordingContextSource directSource;
    constexpr std::size_t slotBytes = 16U;
    constexpr std::size_t usedBytes = 4U;
    auto ctxResult = directSource.beginFrame(slotBytes);
    assert(ctxResult.hasValue());
    GraphicsFrameBuilder builder(ctxResult.value());
    auto region = builder.reserve<std::uint8_t>(slotBytes, 1U);
    assert(region.hasValue());
    std::uint8_t* const reservedPtr = region.value().data();
    reservedPtr[0] = 1;
    reservedPtr[1] = 2;
    reservedPtr[2] = 3;
    reservedPtr[3] = 4;
    const auto direct = oneGenBuildDirectImageFrame(
        directMeta, 8, builder, std::move(region).value(), usedBytes);
    assert(directSource.calls == 1 && directSource.beginSizes.front() == slotBytes);
    assert(direct && direct->hasImage());
    assert(direct->images().front().payload.pixels.data() == reservedPtr);
    assert(direct->images().front().payload.pixels.size() == usedBytes);
    assert(direct->images().front().payload.pixels[0] == 1 && direct->images().front().payload.pixels[3] == 4);
  }

  // Direct path swaps big-endian 16-bit pixels in place, row by row, like the owned path.
  {
    onegen::Frame swapMeta;
    swapMeta.width = 2;
    swapMeta.height = 2;
    swapMeta.stride = 6;
    swapMeta.pixel_format = 0x01100007;
    swapMeta.pixel_endianness = 2;
    RecordingContextSource swapSource;
    auto ctxResult = swapSource.beginFrame(16U);
    assert(ctxResult.hasValue());
    GraphicsFrameBuilder builder(ctxResult.value());
    auto region = builder.reserve<std::uint8_t>(16U, 2U);
    assert(region.hasValue());
    std::uint8_t* const swapPtr = region.value().data();
    const std::array<std::uint8_t, 12> raw{1, 2, 3, 4, 9, 9, 5, 6, 7, 8, 9, 9};
    std::copy(raw.begin(), raw.end(), swapPtr);
    std::optional<OneGenImageRejection> swapObs;
    const auto swapped = oneGenBuildDirectImageFrame(
        swapMeta, 10, builder, std::move(region).value(), raw.size(), nullptr, &swapObs);
    assert(swapped && swapped->hasImage() && !swapObs);
    const auto px = swapped->images().front().payload.pixels;
    assert(px.data() == swapPtr && px.size() == raw.size());
    const std::array<std::uint8_t, 12> expectedSwap{2, 1, 4, 3, 9, 9, 6, 5, 8, 7, 9, 9};
    assert(std::equal(px.begin(), px.end(), expectedSwap.begin()));
  }

  // Direct path publishes the bytes as delivered and counts the packed big-endian conversion it cannot perform.
  {
    onegen::Frame packedMeta;
    packedMeta.width = 64;
    packedMeta.height = 1;
    packedMeta.pixel_format = 0x010a0046;
    packedMeta.pixel_endianness = 2;
    RecordingContextSource packedSource;
    auto ctxResult = packedSource.beginFrame(128U);
    GraphicsFrameBuilder builder(ctxResult.value());
    auto region = builder.reserve<std::uint8_t>(16U, 1U);
    assert(region.hasValue());
    std::optional<OneGenImageRejection> packedObs;
    OneGenFrameAnomalies packedAnomalies;
    const auto packedFrame = oneGenBuildDirectImageFrame(
        packedMeta, 11, builder, std::move(region).value(), 16U, nullptr, &packedObs, {}, &packedAnomalies);
    assert(packedFrame && packedFrame->hasImage());
    assert(packedObs && packedObs->failure == GraphicsImageFailure::UnsupportedConversion);
    // The 16 delivered bytes are shorter than the declared 64 pixels, so they are published as a raw row.
    assert(packedAnomalies.imageCount == 2 && packedAnomalies.total() == 2);
    assert(packedAnomalies.images[1].failure == GraphicsImageFailure::TruncatedBytes);
    assert(packedFrame->images().front().payload.pixelFormat == GraphicsImagePixelFormat::Mono8);
    assert(packedFrame->images().front().payload.width == 16 && packedFrame->images().front().payload.height == 1);
  }

  // A direct stride below the row size is published from the delivered bytes and counted.
  {
    onegen::Frame badDirect;
    badDirect.width = 2;
    badDirect.height = 2;
    badDirect.stride = 1;
    badDirect.pixel_format = 0x01080001;
    auto ctxResult = localSource.beginFrame(16U);
    assert(ctxResult.hasValue());
    GraphicsFrameBuilder builder(ctxResult.value());
    auto region = builder.reserve<std::uint8_t>(16U, 1U);
    assert(region.hasValue());
    std::optional<OneGenImageRejection> directObs;
    const auto badFrame = oneGenBuildDirectImageFrame(badDirect, 9, builder, std::move(region).value(), 4U, nullptr, &directObs);
    assert(badFrame && badFrame->hasImage());
    assert(directObs && directObs->failure == GraphicsImageFailure::StrideBelowMinimum);
    assert(builder.openWriteRegions() == 0U);
  }

  onegen::Frame multipleImages;
  onegen::Frame::Part imagePart;
  imagePart.width = 2; imagePart.height = 2; imagePart.stride = 2;
  imagePart.data_type = 1; imagePart.data_format = 0x01080001;
  imagePart.payload = {1, 2, 3, 4};
  multipleImages.parts = {imagePart, imagePart, imagePart};
  const auto labeled = oneGenGraphicsFrame(multipleImages, 7, localSource);
  assert(labeled && labeled->images().size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    assert(labeled->images()[i].descriptor.displayName == "Image " + std::to_string(i + 1));
    assert(labeled->images()[i].descriptor.id == "image.0:0." + std::to_string(i));
  }
  multipleImages.parts[0].purpose_id = 1;
  multipleImages.parts[1].purpose_id = 65281;
  multipleImages.parts[2].purpose_id = 5;
  multipleImages.components = {{1, "Intensity"}, {65281, "Phase"}, {5, "Reflectance"}};
  const auto named = oneGenGraphicsFrame(multipleImages, 7, localSource);
  assert(named && named->images()[0].descriptor.displayName == "Intensity");
  assert(named->images()[1].descriptor.displayName == "Phase");
  assert(named->images()[2].descriptor.displayName == "Reflectance");
  multipleImages.components.push_back({1, "Ambiguous"});
  const auto ambiguousNames = oneGenGraphicsFrame(multipleImages, 7, localSource);
  assert(ambiguousNames && ambiguousNames->images()[0].descriptor.displayName == "Image 1");
  assert(ambiguousNames->images()[1].descriptor.displayName == "Phase");
  // A part whose storage write fails is dropped and diagnosed; the other parts still publish.
  {
    multipleImages.components.clear();
    FailingNthAllocationContextSource failSecond(2U);
    std::string droppedDiagnostic;
    const auto partial = oneGenGraphicsFrame(multipleImages, 7, failSecond, &droppedDiagnostic);
    assert(partial && partial->images().size() == 2U);
    assert(droppedDiagnostic.find("part=1") != std::string::npos);
    assert(droppedDiagnostic.find("part=0") == std::string::npos);
  }

  multipleImages.components.clear();
  multipleImages.parts.resize(1);
  const auto singleImage = oneGenGraphicsFrame(multipleImages, 7, localSource);
  assert(singleImage && singleImage->images().front().descriptor.displayName == "Image");

  source.width = 4;
  source.height = 1;
  source.stride = 8;
  source.frame_id = 42;
  source.pixel_format = 0x0110000d;
  source.payload.resize(8);
  const auto bayer = oneGenGraphicsFrame(source, 8, localSource);
  assert(bayer && bayer->images().front().payload.pixelFormat == GraphicsImagePixelFormat::BayerRG10);
  assert(bayer->metadata().frameId == "42");

  source.stride = 5;
  source.pixel_format = 0x010a0046;
  source.payload.resize(5);
  const auto packed = oneGenGraphicsFrame(source, 9, localSource);
  assert(packed && packed->images().front().payload.pixelFormat == GraphicsImagePixelFormat::Mono10Packed);

  source.width = 2;
  source.stride = 12;
  source.pixel_format = 0x02300018;
  source.payload.resize(12);
  const auto rgb10 = oneGenGraphicsFrame(source, 10, localSource);
  assert(rgb10 && rgb10->images().front().payload.pixelFormat == GraphicsImagePixelFormat::RGB10Packed);
  assert(rgb10->images().front().payload.bitAlignment == GraphicsImageBitAlignment::LeastSignificant);

  source.width = 4;
  source.stride = 8;
  source.pixel_format = 0x02100032;
  source.payload.resize(8);
  assert(oneGenGraphicsFrame(source, 11, localSource)->images().front().payload.pixelFormat
         == GraphicsImagePixelFormat::YCbCr422_8);

  // An unknown PFNC is fitted: 12 bits per pixel has no whole element size, so rows become raw Mono8 bytes.
  source.pixel_format = 0x010c0004;
  {
    OneGenFrameAnomalies unknownAnomalies;
    const auto unknown = oneGenGraphicsFrame(source, 12, localSource, nullptr, nullptr, nullptr, {}, nullptr, &unknownAnomalies);
    assert(unknown && unknown->hasImage());
    assert(unknown->images().front().payload.pixelFormat == GraphicsImagePixelFormat::Mono8);
    assert(unknown->images().front().payload.width == 6 && unknown->images().front().payload.height == 1);
    assert(unknownAnomalies.imageCount >= 1 && unknownAnomalies.images[0].failure == GraphicsImageFailure::UnsupportedFormat);
  }

  source.pixel_format = 0;
  assert(oneGenGraphicsFrame(source, 13, localSource)->hasImage());

  source.payload_type = 5;
  source.width = 640;
  source.height = 480;
  source.stride = source.payload.size();
  assert(oneGenGraphicsFrame(source, 13, localSource)->images().front().payload.pixelFormat == GraphicsImagePixelFormat::Jpeg);
  source.payload_type = 0;

  source.pixel_format = 0x01100007;
  source.width = 2;
  source.height = 1;
  source.stride = 4;
  source.payload = {1, 2, 3, 4};
  source.pixel_endianness = 2;
  const auto swapped = oneGenGraphicsFrame(source, 13, localSource);
  assert(swapped && swapped->hasImage());
  const auto* bytes = swapped->images().front().payload.pixels.data();
  if (std::endian::native == std::endian::little)
    assert(bytes[0] == 2 && bytes[1] == 1 && bytes[2] == 4 && bytes[3] == 3);
  else
    assert(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4);
  source.pixel_endianness = 0;

  RecordingContextSource copyStorage;
  const auto copied = oneGenGraphicsFrame(source, 70, copyStorage);
  assert(copied && copied->hasImage());
  assert(copyStorage.calls == 1 && copyStorage.buffers.size() == 1);
  assert(copied->images().front().payload.pixels.data() == copyStorage.buffers.front());
  assert(copyStorage.sizes.front() == source.payload.size() && std::equal(source.payload.begin(), source.payload.end(), copyStorage.buffers.front()));

  RecordingContextSource swapStorage;
  source.pixel_endianness = 2;
  const auto swappedIntoStorage = oneGenGraphicsFrame(source, 71, swapStorage);
  assert(swappedIntoStorage && swappedIntoStorage->hasImage());
  assert(swapStorage.calls == 1 && swapStorage.buffers.size() == 1);
  assert(swappedIntoStorage->images().front().payload.pixels.data()
      == swapStorage.buffers.front());
  assert(swapStorage.sizes.front() == 4 && std::equal(swapStorage.buffers.front(), swapStorage.buffers.front() + 4, std::array<std::uint8_t, 4>{2, 1, 4, 3}.begin()));
  source.pixel_endianness = 0;

  onegen::Frame wordSource;
  wordSource.width = 1;
  wordSource.height = 1;
  wordSource.stride = 4;
  wordSource.pixel_format = 0x022000a7;
  wordSource.pixel_endianness = 2;
  wordSource.payload = {1, 2, 3, 4};
  RecordingContextSource wordStorage;
  const auto wordSwapped = oneGenGraphicsFrame(wordSource, 72, wordStorage);
  assert(wordSwapped && wordSwapped->hasImage());
  assert(wordSwapped->images().front().payload.pixelFormat
      == GraphicsImagePixelFormat::BiColorRGBG10);
  assert(wordStorage.calls == 1 && wordStorage.buffers.size() == 1);
  assert(wordStorage.sizes.front() == 4 && std::equal(wordStorage.buffers.front(), wordStorage.buffers.front() + 4, std::array<std::uint8_t, 4>{4, 3, 2, 1}.begin()));

  RefusingContextSource refusedClass(GraphicsFrameError::ClassRefused);
  GraphicsFrameError storageErr = GraphicsFrameError::None;
  const auto dropped = oneGenGraphicsFrame(source, 73, refusedClass, nullptr, nullptr, nullptr, {}, &storageErr);
  assert(!dropped && storageErr == GraphicsFrameError::ClassRefused);

  onegen::Frame mono16;
  mono16.width = 4;
  mono16.height = 2;
  mono16.stride = 7;
  mono16.pixel_format = 0x01100007;
  mono16.payload.resize(16);
  using Conversion = OneGenImageRejection::Conversion;
  std::optional<OneGenImageRejection> observation;
  RecordingContextSource invalidPlanSource;
  const auto narrowStride = oneGenGraphicsFrame(mono16, 75, invalidPlanSource, nullptr, &observation);
  assert(narrowStride && narrowStride->hasImage());
  assert(invalidPlanSource.calls == 1);
  assert(narrowStride->images().front().payload.stride == 8 && narrowStride->images().front().payload.height == 2);
  assert(observation && observation->failure == GraphicsImageFailure::StrideBelowMinimum);
  assert(observation->conversion == Conversion::Copy && observation->effectiveStride == 7);
  assert(observation->minimumRowBytes == 8 && observation->requiredBytes == 16);
  mono16.stride = 8;
  mono16.payload.resize(15);
  mono16.payload.assign(15, 0xff);
  OneGenFrameAnomalies shortAnomalies;
  const auto shortFrame = oneGenGraphicsFrame(mono16, 78, invalidPlanSource, nullptr, &observation, nullptr, {}, nullptr, &shortAnomalies);
  assert(shortFrame && shortFrame->hasImage());
  assert(invalidPlanSource.calls == 2);
  {
    // Planned at the declared 16 bytes: the 15 delivered bytes are copied and the last one is zero.
    const auto pixels = shortFrame->images().front().payload.pixels;
    assert(pixels.size() == 16 && pixels[14] == 0xff && pixels[15] == 0);
  }
  assert(shortAnomalies.total() == 1 && shortAnomalies.imageCount == 1);
  assert(observation && observation->failure == GraphicsImageFailure::TruncatedBytes);
  assert(observation->conversion == Conversion::Copy && observation->requiredBytes == 16);
  mono16.payload.resize(16);
  RefusingContextSource refuse(GraphicsFrameError::NoReadyCredit);
  GraphicsFrameError monoErr = GraphicsFrameError::None;
  assert(!oneGenGraphicsFrame(mono16, 79, refuse, nullptr, &observation, nullptr, {}, &monoErr));
  assert(monoErr == GraphicsFrameError::NoReadyCredit);
  assert(observation && observation->failure == GraphicsImageFailure::StorageRefused);
  mono16.stride = 0;
  assert(oneGenGraphicsFrame(mono16, 81, localSource, nullptr, &observation));
  assert(!observation);

  std::string rejectionReason;
  source.payload_type = 8;
  assert(oneGenGraphicsFrame(source, 13, localSource, &rejectionReason)->hasImage());
  source.payload_type = 4;
  source.buffer_info.image_present = false;
  assert(!oneGenGraphicsFrame(source, 13, localSource, &rejectionReason));
  source.buffer_info.image_present = true;
  assert(oneGenGraphicsFrame(source, 13, localSource)->hasImage());
  source.payload_type = 2;
  assert(oneGenGraphicsFrame(source, 13, localSource)->hasImage());
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

  const auto scene = oneGenGraphicsFrame(multipart, 14, localSource);
  assert(scene && scene->hasRangeFrame());
  assert(scene->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
  assert(scene->ranges().front().payload.zValues[0] == 2.0F);
  assert(samplesEqual(scene->ranges().front().payload.confidence, std::vector<float>({10.0F, 20.0F})));
  assert(scene->metadata().frameId == "77");

  // Provided storage receives every range channel once; the sealed spans share the block.
  RecordingContextSource rangeStorage;
  const auto storedScene = oneGenGraphicsFrame(multipart, 14, rangeStorage, nullptr);
  assert(storedScene && storedScene->hasRangeFrame());
  assert(rangeStorage.calls == 1 && rangeStorage.buffers.size() == 1);
  const auto& storedRange = storedScene->ranges().front().payload;
  assert(storedRange.zValues.data()
      == reinterpret_cast<const float*>(rangeStorage.buffers.front()));
  assert(storedRange.zValues[0] == 2.0F && storedRange.zValues[1] == 3.0F);
  assert(storedRange.xValues[0] == 1.0F && storedRange.xValues[1] == 4.0F);
  assert(storedRange.yValues[0] == 2.0F && storedRange.yValues[1] == 5.0F);
  assert(samplesEqual(storedRange.validMask, std::vector<std::uint8_t>({1, 1})));
  assert(storedRange.confidence[0] == 10.0F && storedRange.confidence[1] == 20.0F);

  RecordingContextSource refusedRangeStorage;
  const auto refusedScene = oneGenGraphicsFrame(multipart, 14, refuse);
  assert(!refusedScene);

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
  std::optional<OneGenPlanarRejection> standalonePlanarRejection;
  const auto c16Scene = oneGenGraphicsFrame(c16Frame, 15, localSource, &rejectionReason, nullptr, &standalonePlanarRejection);
  assert(c16Scene && c16Scene->hasRangeFrame()
      && rejectionReason.empty() && !standalonePlanarRejection);
  assert(c16Scene->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  for (const auto type : {6U, 7U, 8U}) {
    auto planarC = c16Frame;
    planarC.parts.front().data_type = type;
    const auto plane = oneGenGraphicsFrame(planarC, 15, localSource, &rejectionReason, nullptr, &standalonePlanarRejection);
    assert(plane && plane->hasRangeFrame() && rejectionReason.empty() && !standalonePlanarRejection);
  }
  RecordingContextSource c16Storage;
  const auto c16Stored = oneGenGraphicsFrame(c16Frame, 15, c16Storage, nullptr);
  assert(c16Stored && c16Stored->hasRangeFrame());
  const auto& c16Range = c16Stored->ranges().front().payload;
  assert(c16Range.zValues[0] == 6.0F && c16Range.rangeRaw[0] == 6U);
  auto c16BigEndian = c16Frame;
  c16BigEndian.pixel_endianness = 2;
  c16BigEndian.parts[0].payload = {0, 6, 0, 0};
  RecordingContextSource c16BigEndianStorage;
  const auto c16BigEndianScene = oneGenGraphicsFrame(c16BigEndian, 15, c16BigEndianStorage, nullptr);
  assert(c16BigEndianScene && c16BigEndianScene->hasRangeFrame());
  assert(c16BigEndianScene->ranges().front().payload.rangeRaw[0] == 6U);
  auto missingCalibration = c16Frame;
  missingCalibration.scan3d_geometry->calibrated_axes[2] = false;
  // Without usable Z calibration the scalar part is published as an uncalibrated pixel grid and counted.
  const auto uncalibratedC = oneGenGraphicsFrame(missingCalibration, 15, localSource, &rejectionReason, nullptr, &standalonePlanarRejection);
  assert(uncalibratedC && uncalibratedC->hasRangeFrame() && rejectionReason.empty());
  assert(uncalibratedC->ranges().front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(standalonePlanarRejection && standalonePlanarRejection->reason == OneGenPlanarRejection::Reason::InvalidCalibration);
  onegen::Frame::Part survivingImage;
  survivingImage.data_type = 1;
  survivingImage.data_format = 0x01080001;
  survivingImage.width = 2; survivingImage.height = 1; survivingImage.stride = 2;
  survivingImage.payload = {1, 2};
  missingCalibration.parts.push_back(survivingImage);
  const auto partial = oneGenGraphicsFrame(missingCalibration, 15, localSource, &rejectionReason);
  assert(partial && partial->hasImage() && partial->hasRangeFrame() && rejectionReason.empty());
  missingCalibration.scan3d_geometry.reset();
  const auto native = oneGenGraphicsFrame(missingCalibration, 15, localSource, &rejectionReason);
  assert(native && native->hasRangeFrame() && rejectionReason.empty()
      && native->ranges().front().payload.canDeriveSurface()
      && !native->ranges().front().payload.canDerivePointCloud());
  assert(!std::isfinite(c16Scene->ranges().front().payload.xScale));
  assert(c16Scene->ranges().front().payload.physicalZAt(0) == 2.0);
  assert(samplesEqual(c16Scene->ranges().front().payload.validMask, std::vector<std::uint8_t>({1, 0})));
  auto discardedParts = c16Frame;
  auto unsupportedPart = survivingImage;
  unsupportedPart.data_type = 2;
  discardedParts.parts.push_back(unsupportedPart);
  auto orphanConfidence = survivingImage;
  orphanConfidence.data_type = 9;
  orphanConfidence.source_id = 999;
  discardedParts.parts.push_back(orphanConfidence);
  const auto retainedRange = oneGenGraphicsFrame(discardedParts, 16, localSource, &rejectionReason);
  // A part of an unsupported type and a confidence part without a range are fitted and published as images.
  assert(retainedRange && retainedRange->hasRangeFrame() && retainedRange->images().size() == 2);
  assert(rejectionReason.empty());

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
  const auto floatScene = oneGenGraphicsFrame(floating, 17, localSource);
  assert(floatScene && floatScene->hasRangeFrame());
  const auto& range = floatScene->ranges().front().payload;
  assert(range.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(!std::isfinite(range.xScale) && !std::isfinite(range.yScale));
  assert(range.physicalZAt(0) == -1 && range.physicalZAt(1) == -2 && range.physicalZAt(3) == 2);
  assert(samplesEqual(range.validMask, std::vector<std::uint8_t>({1, 1, 0, 1})));
  assert(range.rangeRaw.empty() && range.confidenceRaw.empty());
  assert(range.rangeBits == 32 && range.confidenceBits == 32);
  assert(range.confidence[0] == 0.25F && range.confidence[3] == 2);
  assert(samplesEqual(range.confidenceValidMask, std::vector<std::uint8_t>({1, 1, 0, 1})));
  assert(range.confidenceField.bitsPerSample == 32);
  onegen::Frame single;
  single.width = 2; single.height = 2; single.stride = 12;
  single.pixel_format = 0x012000bf;
  single.payload = floating.parts[0].payload;
  single.scan3d_geometry = floating.scan3d_geometry;
  const auto singleRange = oneGenGraphicsFrame(single, 23, localSource);
  assert(singleRange && singleRange->ranges().front().payload.physicalZAt(3) == 2);
  single.scan3d_geometry.reset();
  const auto preview = oneGenGraphicsFrame(single, 24, localSource);
  assert(preview && preview->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(preview->ranges().front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(preview->ranges().front().payload.zValues[3] == 6);
  assert(preview->ranges().front().payload.canDeriveSurface());
  assert(!preview->ranges().front().payload.canDerivePointCloud());
  single.parts = {floating.parts[0]};
  single.parts[0].data_type = 1;
  single.scan3d_geometry.reset();
  assert(oneGenGraphicsFrame(single, 24, localSource)->ranges().front().payload.canDeriveSurface());
  single.parts.clear();
  single.pixel_format = 0x012000c8;
  single.payload = floating.parts[1].payload;
  const auto confidencePreview = oneGenGraphicsFrame(single, 25, localSource);
  assert(confidencePreview && confidencePreview->hasRangeFrame());
  assert(confidencePreview->ranges().front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(confidencePreview->ranges().front().payload.zValues[3] == 2);
  assert(!std::isfinite(confidencePreview->ranges().front().payload.xScale));
  assert(samplesEqual(confidencePreview->ranges().front().payload.validMask, std::vector<std::uint8_t>({1, 1, 0, 1})));
  single.parts = {floating.parts[1]};
  assert(oneGenGraphicsFrame(single, 26, localSource)->ranges().front().payload.rangeField.displayName == "Confidence");
  single.pixel_endianness = 3;
  {
    OneGenFrameAnomalies endianAnomalies;
    const auto endianFrame = oneGenGraphicsFrame(single, 27, localSource, nullptr, nullptr, nullptr, {}, nullptr, &endianAnomalies);
    assert(endianFrame && endianFrame->hasRangeFrame());
    assert(endianAnomalies.partCount == 1 && endianAnomalies.parts[0].reason == OneGenPlanarRejection::Reason::InvalidLayout);
  }
  // The complete vendor PixelFormat enumeration, including padded monochrome rows.
  for (const auto format : {0x01080001U, 0x01100007U}) {
    onegen::Frame mono;
    mono.width = 2; mono.height = 2; mono.stride = 6;
    mono.pixel_format = format; mono.payload.resize(12);
    assert(oneGenGraphicsFrame(mono, 28, localSource)->hasImage());
    onegen::Frame::Part imagePart;
    imagePart.width = 2; imagePart.height = 2; imagePart.stride = 6;
    imagePart.data_type = 1; imagePart.data_format = format; imagePart.payload = mono.payload;
    mono.parts = {imagePart};
    assert(oneGenGraphicsFrame(mono, 29, localSource)->hasImage());
    mono.scan3d_geometry = floating.scan3d_geometry;
    mono.parts.insert(mono.parts.begin(), floating.parts[0]);
    const auto paired = oneGenGraphicsFrame(mono, 30, localSource);
    assert(paired && paired->hasImage() && paired->hasRangeFrame());
    assert(paired->images().front().descriptor.groupId == "0:0");
    assert(paired->ranges().front().descriptor.groupId == "0:0");
    assert(samplesEqual(paired->ranges().front().payload.intensity, std::vector<float>({0, 0, 0, 0})));
    assert(paired->ranges().front().payload.intensityField.bitsPerSample == (format == 0x01080001U ? 8 : 16));

    auto secondRangePart = floating.parts[0];
    secondRangePart.region_id = 1;
    auto secondImagePart = imagePart;
    secondImagePart.region_id = 1;
    mono.parts.push_back(std::move(secondRangePart));
    mono.parts.push_back(std::move(secondImagePart));
    const auto multiple = oneGenGraphicsFrame(mono, 31, localSource);
    assert(multiple && multiple->ranges().size() == 2 && multiple->images().size() == 2);
    assert(multiple->ranges()[0].descriptor.id != multiple->ranges()[1].descriptor.id);
    assert(multiple->images()[0].descriptor.id != multiple->images()[1].descriptor.id);
    assert(multiple->ranges()[1].descriptor.groupId == "0:1");
    assert(multiple->images()[1].descriptor.groupId == "0:1");
  }
  floating.scan3d_geometry->invalid_enabled[2] = true;
  floating.scan3d_geometry->invalid_value[2] = -2;
  assert(oneGenGraphicsFrame(floating, 18, localSource)->ranges().front().payload.validMask[1] == 0);
  floating.parts[1].payload.pop_back();
  {
    // The confidence part lacks its last padding byte: the channel stays, the shortfall is counted.
    OneGenFrameAnomalies confidenceAnomalies;
    const auto shortConfidence = oneGenGraphicsFrame(floating, 19, localSource, nullptr, nullptr, nullptr, {}, nullptr, &confidenceAnomalies);
    assert(shortConfidence && shortConfidence->ranges().front().payload.confidence.size() == 4);
    assert(confidenceAnomalies.partCount == 1
        && confidenceAnomalies.parts[0].reason == OneGenPlanarRejection::Reason::AuxiliaryLayoutRejected);
  }
  floating.pixel_endianness = 3;
  {
    OneGenFrameAnomalies endianAnomalies;
    const auto endianScene = oneGenGraphicsFrame(floating, 20, localSource, nullptr, nullptr, nullptr, {}, nullptr, &endianAnomalies);
    assert(endianScene && endianScene->hasRangeFrame() && endianAnomalies.partCount >= 1);
    assert(endianAnomalies.parts[0].reason == OneGenPlanarRejection::Reason::InvalidLayout);
  }
  floating.pixel_endianness = 0;
  floating.parts[0].payload.pop_back();
  {
    OneGenFrameAnomalies truncatedAnomalies;
    const auto truncatedRange = oneGenGraphicsFrame(floating, 21, localSource, nullptr, nullptr, nullptr, {}, nullptr, &truncatedAnomalies);
    assert(truncatedRange && truncatedRange->hasRangeFrame());
    assert(truncatedAnomalies.partCount >= 1
        && truncatedAnomalies.parts[0].reason == OneGenPlanarRejection::Reason::TruncatedPlane);
  }
  floating.parts[0].payload.resize(24);
  floating.scan3d_geometry.reset();
  const auto uncalibrated = oneGenGraphicsFrame(floating, 22, localSource);
  assert(uncalibrated && uncalibrated->hasRangeFrame());
  assert(uncalibrated->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(uncalibrated->ranges().front().payload.canDeriveSurface());

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
    const auto converted = oneGenGraphicsFrame(coordinates, 31, localSource);
    assert(converted && converted->hasRangeFrame());
    const auto& decoded = converted->ranges().front().payload;
    assert(decoded.physicalZAt(0) == 9.5 && decoded.physicalZAt(1) == -1);
    assert(samplesEqual(decoded.validMask, std::vector<std::uint8_t>({1, 1, 1, 1}))); // Zero is valid unless declared invalid.
    if (layout.components > 1) {
      assert(decoded.xValues[0] == 12 && decoded.xValues[3] == 18);
      assert(decoded.yValues[0] == (layout.components == 3 ? 53 : 20));
      assert(decoded.yValues[3] == (layout.components == 3 ? 62 : 23));
    }
    onegen::Frame::Part coordinatePart;
    coordinatePart.width = 2; coordinatePart.height = 2; coordinatePart.stride = coordinates.stride;
    coordinatePart.data_type = 5; coordinatePart.data_format = layout.format; coordinatePart.payload = coordinates.payload;
    coordinates.parts = {coordinatePart};
    assert(oneGenGraphicsFrame(coordinates, 32, localSource)->ranges().front().payload.physicalZAt(0) == 9.5);
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
      const auto separated = oneGenGraphicsFrame(coordinates, 42, localSource);
      assert(separated && separated->ranges().front().payload.xValues[0] == 12 && separated->ranges().front().payload.physicalZAt(0) == 9.5);
      for (auto& plane : coordinates.parts) plane.data_type = 7;
      const auto declaredPlanes = oneGenGraphicsFrame(coordinates, 42, localSource);
      assert(declaredPlanes && declaredPlanes->hasRangeFrame()
          && declaredPlanes->ranges().front().payload.xValues[0] == 12);
      const auto firstPlanarGroup = coordinates.parts;
      for (auto plane : firstPlanarGroup) {
        plane.region_id = 1;
        coordinates.parts.push_back(std::move(plane));
      }
      const auto multiplePlanar = oneGenGraphicsFrame(coordinates, 68, localSource);
      assert(multiplePlanar && multiplePlanar->ranges().size() == 2);
      assert(multiplePlanar->ranges()[0].descriptor.groupId == "0:0");
      assert(multiplePlanar->ranges()[1].descriptor.groupId == "0:1");
      coordinates.parts.resize(firstPlanarGroup.size());

      using PlanarReason = OneGenPlanarRejection::Reason;
      std::optional<OneGenPlanarRejection> planarRejection;
      const auto inspectPlanar = [&] {
        return oneGenGraphicsFrame(coordinates, 43, localSource, nullptr, nullptr, &planarRejection);
      };
      coordinates.parts[1].source_id = 9;
      assert(inspectPlanar());
      assert(planarRejection && planarRejection->reason == PlanarReason::MissingAxis);
      assert(planarRejection->anchorPart == 0 && planarRejection->matchedAxes == 0b101U);
      assert(planarRejection->shapeMismatchAxes == 0 && planarRejection->namespaceMismatchAxes == 0);
      coordinates.parts[1].source_id = 0;

      coordinates.parts.erase(coordinates.parts.begin());
      assert(inspectPlanar());
      assert(planarRejection && planarRejection->reason == PlanarReason::MissingAxis);
      assert(planarRejection->anchorPart == 0 && planarRejection->matchedAxes == 0b110U);
      coordinates.parts = firstPlanarGroup;

      ++coordinates.parts[1].width;
      ++coordinates.parts[1].format_namespace;
      assert(inspectPlanar());
      assert(planarRejection && planarRejection->reason == PlanarReason::MissingAxis);
      assert(planarRejection->matchedAxes == 0b101U && planarRejection->shapeMismatchAxes == 0b010U);
      assert(planarRejection->namespaceMismatchAxes == 0b010U);
      coordinates.parts = firstPlanarGroup;

      coordinates.parts.push_back(coordinates.parts.front());
      assert(inspectPlanar());
      assert(planarRejection && planarRejection->reason == PlanarReason::DuplicateAxis);
      assert(planarRejection->duplicateAxes == 0b001U && planarRejection->affectedPart == 3);
      coordinates.parts.pop_back();

      coordinates.parts[1].stride = 2 * layout.bytes - 1;
      {
        // The plane is joined row by row; the missing byte of each row is zero and the stride is counted.
        const auto joinedNarrow = inspectPlanar();
        assert(joinedNarrow && joinedNarrow->hasRangeFrame());
      }
      assert(planarRejection && planarRejection->reason == PlanarReason::StrideBelowMinimum);
      assert(planarRejection->affectedPart == 1);
      assert(planarRejection->minimumRowBytes == 2 * layout.bytes);
      assert(planarRejection->requiredBytes == 4 * layout.bytes);
      coordinates.parts = firstPlanarGroup;

      coordinates.parts[1].payload.pop_back();
      {
        const auto joinedShort = inspectPlanar();
        assert(joinedShort && joinedShort->hasRangeFrame());
      }
      assert(planarRejection && planarRejection->reason == PlanarReason::TruncatedPlane);
      assert(planarRejection->affectedPart == 1 && planarRejection->readableBytes == planeSize - 1);
      assert(planarRejection->requiredBytes == planeSize);

      onegen::Frame::Part survivingImage;
      survivingImage.width = 2; survivingImage.height = 2; survivingImage.stride = 2;
      survivingImage.data_type = 1; survivingImage.data_format = 0x01080001;
      survivingImage.payload = {1, 2, 3, 4};
      coordinates.parts.push_back(survivingImage);
      const auto partial = inspectPlanar();
      assert(partial && partial->hasImage() && partial->hasRangeFrame());
      assert(planarRejection && planarRejection->reason == PlanarReason::TruncatedPlane);

      auto unusableImage = survivingImage;
      unusableImage.payload.clear();
      coordinates.parts.push_back(std::move(unusableImage));
      std::string otherRejections;
      const auto mixed = oneGenGraphicsFrame(coordinates, 43, localSource, &otherRejections, nullptr, &planarRejection);
      assert(mixed && mixed->hasImage() && planarRejection);
      assert(otherRejections.find("part=4 ") != std::string::npos);
      assert(otherRejections.find("part=1 ") == std::string::npos);

      coordinates.parts = firstPlanarGroup;
      assert(inspectPlanar()->hasRangeFrame());
      assert(!planarRejection);
      auto missingCalibration = coordinates;
      missingCalibration.scan3d_geometry.reset();
      (void)oneGenGraphicsFrame(missingCalibration, 44, localSource, nullptr, nullptr, &planarRejection);
      assert(planarRejection && planarRejection->reason == PlanarReason::MissingCalibration);
      assert(planarRejection->matchedAxes == 0b111U && !planarRejection->affectedPart);

      assert(!oneGenGraphicsFrame(coordinates, 45, refuse, nullptr, nullptr, &planarRejection));
      assert(planarRejection && planarRejection->reason == PlanarReason::RangeStorageRefused);
      assert(planarRejection->requestedStorageBytes >= 4 * sizeof(float));
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
      assert(oneGenGraphicsFrame(coordinates, 45, localSource)->ranges().front().payload.confidence.empty());
      coordinates.parts.back().source_id = 0;
      assert(oneGenGraphicsFrame(coordinates, 46, localSource)->ranges().front().payload.confidence[0] == 0.25F);
      geometry.output_mode = "CalibratedABC_PointCloud";
      const auto cloudFrame = oneGenGraphicsFrame(coordinates, 47, localSource);
      assert(cloudFrame && cloudFrame->hasRangeFrame() && !cloudFrame->hasPointCloud()
          && !cloudFrame->hasSurfaceMesh());
      const RangeFrame& cloudRange = cloudFrame->ranges().front().payload;
      assert(cloudRange.canDerivePointCloud());
      assert(cloudRange.xValues.size() == 4 && cloudRange.xValues[0] == 12
          && cloudRange.yValues[0] == 53 && cloudRange.physicalZAt(0) == 9.5);
      assert(cloudRange.confidence.size() == 4 && cloudRange.confidence[0] == 0.25F);
      geometry.invalid_enabled[2] = true;
      const auto filteredFrame = oneGenGraphicsFrame(coordinates, 48, localSource);
      assert(filteredFrame && filteredFrame->hasRangeFrame() && !filteredFrame->hasPointCloud());
      assert(samplesEqual(filteredFrame->ranges().front().payload.validMask, std::vector<std::uint8_t>({1, 0, 1, 1})));
      geometry.invalid_enabled[2] = false;
      coordinates.parts.clear();
      const auto singleCloudFrame = oneGenGraphicsFrame(coordinates, 49, localSource);
      assert(singleCloudFrame && singleCloudFrame->hasRangeFrame()
          && singleCloudFrame->ranges().front().payload.canDerivePointCloud());
      geometry.output_mode = "CalibratedABC_Grid";
    }
    coordinates.parts.clear();
    geometry.invalid_enabled[2] = true; geometry.invalid_value[2] = 0;
    assert(oneGenGraphicsFrame(coordinates, 33, localSource)->ranges().front().payload.validMask[1] == 0);
    coordinates.payload.pop_back();
    {
      std::optional<OneGenPlanarRejection> shortCoordinates;
      const auto shortScene = oneGenGraphicsFrame(coordinates, 34, localSource, nullptr, nullptr, &shortCoordinates);
      assert(shortScene && shortScene->hasRangeFrame());
      assert(shortCoordinates && shortCoordinates->reason == OneGenPlanarRejection::Reason::TruncatedPlane);
    }
  }
  onegen::Frame rectifiedFrame;
  rectifiedFrame.scan3d_geometry = multipart.scan3d_geometry;
  rectifiedFrame.scan3d_geometry->output_mode = "RectifiedC";
  rectifiedFrame.scan3d_geometry->coordinate_system = "Cartesian";
  rectifiedFrame.scan3d_geometry->scale = {2, -3, 0.5};
  rectifiedFrame.scan3d_geometry->offset = {10, 20, -1};
  rectifiedFrame.parts = {scalarPart(0x012000bf, 5, {0, 1, 2, 3})};
  auto rectifiedScene = oneGenGraphicsFrame(rectifiedFrame, 35, localSource);
  assert(rectifiedScene && rectifiedScene->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::UniformPhysical);
  assert(rectifiedScene->ranges().front().payload.xScale == 2 && rectifiedScene->ranges().front().payload.yScale == -3);
  assert(rectifiedScene->ranges().front().payload.xOffset == 10 && rectifiedScene->ranges().front().payload.yOffset == 20);
  rectifiedFrame.scan3d_geometry->calibrated_axes[0] = false;
  // Rectified C without all axes is fitted as an uncalibrated pixel grid; each fit is counted.
  const auto reasonOf = [&](std::uint64_t sequence) {
    std::optional<OneGenPlanarRejection> evidence;
    const auto fitted = oneGenGraphicsFrame(rectifiedFrame, sequence, localSource, nullptr, nullptr, &evidence);
    assert(fitted && fitted->hasRangeFrame());
    return evidence ? std::optional(evidence->reason) : std::nullopt;
  };
  assert(reasonOf(36) == OneGenPlanarRejection::Reason::InvalidCalibration);
  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedC";
  assert(!reasonOf(37));
  rectifiedFrame.scan3d_geometry->coordinate_system = "Spherical";
  assert(reasonOf(38) == OneGenPlanarRejection::Reason::UnsupportedGeometry);
  rectifiedFrame.scan3d_geometry->coordinate_system = "Cartesian";
  rectifiedFrame.scan3d_geometry->output_mode = "DisparityC";
  assert(reasonOf(39) == OneGenPlanarRejection::Reason::InvalidCalibration);
  rectifiedFrame.scan3d_geometry->output_mode = "RectifiedC_Linescan";
  assert(reasonOf(40) == OneGenPlanarRejection::Reason::UnsupportedGeometry);
  rectifiedFrame.scan3d_geometry->output_mode = "UncalibratedC";
  rectifiedFrame.scan3d_geometry->distance_unit.clear();
  const auto rawScene = oneGenGraphicsFrame(rectifiedFrame, 41, localSource);
  assert(rawScene && rawScene->ranges().front().payload.rangeField.domain == MeasurementValueDomain::Native);
  assert(rawScene->ranges().front().payload.zValues[3] == 3 && !std::isfinite(rawScene->ranges().front().payload.xScale));

  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedC";
  rectifiedFrame.scan3d_geometry->distance_unit = "Millimeter";
  rectifiedFrame.scan3d_geometry->scale[2] = 1e100;
  const auto overflowScene = oneGenGraphicsFrame(rectifiedFrame, 50, localSource);
  assert(overflowScene && samplesEqual(overflowScene->ranges().front().payload.validMask, std::vector<std::uint8_t>({1, 0, 0, 0})));

  rectifiedFrame.scan3d_geometry->output_mode = "CalibratedABC_PointCloud";
  rectifiedFrame.parts = {scalarPart(0x012000c8, 9, {0.25F, 0.75F, nan, 2})};
  const auto confidenceOnly = oneGenGraphicsFrame(rectifiedFrame, 51, localSource);
  assert(confidenceOnly && confidenceOnly->hasRangeFrame() && !confidenceOnly->hasPointCloud());
  assert(confidenceOnly->ranges().front().payload.rangeField.domain == MeasurementValueDomain::Native);

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
  const auto packedScene = oneGenGraphicsFrame(packedC, 52, localSource);
  assert(packedScene && packedScene->ranges().front().payload.physicalZAt(0) == 0 && packedScene->ranges().front().payload.physicalZAt(1) == 1);

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
  const auto f64Scene = oneGenGraphicsFrame(f64, 53, localSource);
  assert(f64Scene && f64Scene->ranges().front().payload.physicalZAt(0) == 1 && f64Scene->ranges().front().payload.physicalZAt(1) == 5);
  auto be64 = f64;
  be64.pixel_endianness = 2;
  if (std::endian::native == std::endian::little)
    for (std::size_t i = 0; i + 8 <= be64.payload.size(); i += 8)
      std::reverse(be64.payload.begin() + i, be64.payload.begin() + i + 8);
  assert(oneGenGraphicsFrame(be64, 67, localSource)->ranges().front().payload.physicalZAt(0) == 1);
  f64.scan3d_geometry->distance_unit = "Meter";
  f64.scan3d_geometry->scale[2] = 1;
  f64.scan3d_geometry->offset[2] = 0;
  const auto meterScene = oneGenGraphicsFrame(f64, 54, localSource);
  assert(meterScene && meterScene->ranges().front().payload.physicalZAt(0) == 1000);
  assert(meterScene->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::PixelGrid);
  assert(!std::isfinite(meterScene->ranges().front().payload.xScale));
  f64.scan3d_geometry->distance_unit = "Inch";
  assert(oneGenGraphicsFrame(f64, 60, localSource)->ranges().front().payload.physicalZAt(0) == 25.4F);

  onegen::Frame disparity;
  disparity.parts = {scalarPart(0x012000bf, 5, {2, 4, 2, 4})};
  disparity.scan3d_geometry.emplace();
  disparity.scan3d_geometry->output_mode = "DisparityC";
  disparity.scan3d_geometry->distance_unit = "Millimeter";
  disparity.scan3d_geometry->scale = {1, 1, 1};
  disparity.scan3d_geometry->focal_length = 10;
  disparity.scan3d_geometry->baseline = 5;
  const auto disparityScene = oneGenGraphicsFrame(disparity, 55, localSource);
  assert(disparityScene && disparityScene->ranges().front().payload.physicalZAt(0) == 25);
  assert(disparityScene->ranges().front().payload.physicalZAt(1) == 12.5F);
  disparity.scan3d_geometry->principal_u = 0;
  disparity.scan3d_geometry->principal_v = 0;
  const auto projected = oneGenGraphicsFrame(disparity, 56, localSource);
  assert(projected && projected->ranges().front().payload.xyCoordinateMode == RangeFrameXYCoordinateMode::ExplicitXY);
  assert(projected->ranges().front().payload.xValues[1] == 1.25F);

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
  const auto sphere = oneGenGraphicsFrame(spherical, 57, localSource);
  assert(sphere && sphere->ranges().front().payload.xValues[0] == 0 && sphere->ranges().front().payload.yValues[0] == 0);
  assert(sphere->ranges().front().payload.zValues[0] == 2);

  onegen::Frame cylindrical = spherical;
  cylindrical.scan3d_geometry->coordinate_system = "Cylindrical";
  const float cyl[] = {0.F, 3.F, 2.F};
  std::memcpy(cylindrical.parts[0].payload.data(), cyl, sizeof(cyl));
  const auto cylinder = oneGenGraphicsFrame(cylindrical, 58, localSource);
  assert(cylinder && cylinder->ranges().front().payload.xValues[0] == 2 && cylinder->ranges().front().payload.yValues[0] == 3);
  assert(cylinder->ranges().front().payload.zValues[0] == 0);

  onegen::Frame meterCloud = spherical;
  meterCloud.scan3d_geometry->output_mode = "CalibratedABC_PointCloud";
  meterCloud.scan3d_geometry->coordinate_system = "Cartesian";
  meterCloud.scan3d_geometry->distance_unit = "Meter";
  const float meterAbc[] = {1.F, 2.F, 3.F};
  std::memcpy(meterCloud.parts[0].payload.data(), meterAbc, sizeof(meterAbc));
  const auto meterPoints = oneGenGraphicsFrame(meterCloud, 66, localSource);
  assert(meterPoints && meterPoints->hasRangeFrame() && !meterPoints->hasPointCloud());
  const RangeFrame& meterRange = meterPoints->ranges().front().payload;
  assert(meterRange.canDerivePointCloud());
  assert(meterRange.lengthUnit == GraphicsLengthUnit::Millimeter);
  assert(meterRange.xValues[0] == 1000 && meterRange.yValues[0] == 2000);
  assert(meterRange.physicalZAt(0) == 3000);

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
  const auto bitConfidence = oneGenGraphicsFrame(confidenceBits, 59, localSource);
  assert(bitConfidence && samplesEqual(bitConfidence->ranges().front().payload.confidence, std::vector<float>({1, 0, 1, 0})));
  assert(bitConfidence->ranges().front().payload.confidenceBits == 1);
  confidenceBits.parts[1].data_format = 0x010100c5;
  confidenceBits.parts[1].stride = 1;
  confidenceBits.parts[1].payload = {0x05, 0x05};
  const auto packedConfidence = oneGenGraphicsFrame(confidenceBits, 61, localSource);
  assert(packedConfidence && samplesEqual(packedConfidence->ranges().front().payload.confidence, std::vector<float>({1, 0, 1, 0})));
  assert(packedConfidence->ranges().front().payload.confidenceBits == 1);

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
  const auto rectifiedAc = oneGenGraphicsFrame(packedAc, 62, localSource);
  assert(rectifiedAc && rectifiedAc->ranges().front().payload.xValues[0] == 1);
  assert(rectifiedAc->ranges().front().payload.yValues[0] == 5 && rectifiedAc->ranges().front().payload.zValues[0] == 2);
  packedAc.scan3d_geometry->output_mode = "CalibratedAC";
  assert(oneGenGraphicsFrame(packedAc, 63, localSource)->ranges().front().payload.zValues[0] == 2);

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
  const auto assembled64 = oneGenGraphicsFrame(planar64, 64, localSource);
  assert(assembled64 && assembled64->ranges().front().payload.xValues[0] == 1);
  assert(assembled64->ranges().front().payload.yValues[0] == 2 && assembled64->ranges().front().payload.zValues[0] == 3);

  onegen::Frame sphereRectified;
  sphereRectified.parts = {scalarPart(0x012000bf, 5, {2, 2, 2, 2})};
  sphereRectified.scan3d_geometry.emplace();
  sphereRectified.scan3d_geometry->output_mode = "RectifiedC";
  sphereRectified.scan3d_geometry->coordinate_system = "Spherical";
  sphereRectified.scan3d_geometry->distance_unit = "Millimeter";
  sphereRectified.scan3d_geometry->scale = {1, 1, 1};
  sphereRectified.scan3d_geometry->offset = {0, 0, 0};
  const auto sphereGrid = oneGenGraphicsFrame(sphereRectified, 65, localSource);
  assert(sphereGrid && sphereGrid->ranges().front().payload.xValues[0] == 0);
  assert(sphereGrid->ranges().front().payload.yValues[0] == 0 && sphereGrid->ranges().front().payload.zValues[0] == 2);

  multipart.scan3d_geometry->distance_unit.clear();
  {
    // An unknown distance unit keeps the values, labeled millimeters, and counts the anomaly.
    OneGenFrameAnomalies unitAnomalies;
    const auto unitScene = oneGenGraphicsFrame(multipart, 16, localSource, nullptr, nullptr, nullptr, {}, nullptr, &unitAnomalies);
    assert(unitScene && unitScene->hasRangeFrame());
    assert(unitScene->ranges().front().payload.lengthUnit == GraphicsLengthUnit::Millimeter);
    assert(unitAnomalies.partCount == 1 && unitAnomalies.parts[0].reason == OneGenPlanarRejection::Reason::UnsupportedUnit);
  }

  // ---- Fit instead of reject: every anomaly is counted, the data is published. ----
  // The per-frame record is fixed-size: it never allocates per frame.
  static_assert(std::is_trivially_destructible_v<OneGenFrameAnomalies>
      && std::is_trivially_copyable_v<OneGenFrameAnomalies>);
  {
    // An unknown PFNC maps to the GraphicsEngine format with the same bytes per element.
    struct UnknownCase { std::uint64_t pfnc; GraphicsImagePixelFormat format; int width; };
    const UnknownCase unknownCases[] = {
      {0x01080999, GraphicsImagePixelFormat::Mono8, 4},
      {0x01100999, GraphicsImagePixelFormat::Mono16, 4},
      {0x02180999, GraphicsImagePixelFormat::RGB24, 4},
      {0x02200999, GraphicsImagePixelFormat::Mono32, 4},
      {0x01400999, GraphicsImagePixelFormat::Mono8, 32}, // 64 bits per pixel: eight raw bytes per pixel
    };
    for (const auto& unknownCase : unknownCases) {
      onegen::Frame frameSource;
      frameSource.width = 4; frameSource.height = 2;
      frameSource.pixel_format = unknownCase.pfnc;
      const auto bytesPerPixel = static_cast<std::size_t>((unknownCase.pfnc >> 16U) & 0xffU) / 8U;
      frameSource.stride = 4 * bytesPerPixel;
      frameSource.payload.assign(frameSource.stride * 2, 0x5a);
      OneGenFrameAnomalies counted;
      const auto fitted = oneGenGraphicsFrame(frameSource, 90, localSource, nullptr, nullptr, nullptr, {}, nullptr, &counted);
      assert(fitted && fitted->hasImage());
      const auto& image = fitted->images().front().payload;
      assert(image.pixelFormat == unknownCase.format && image.width == unknownCase.width && image.height == 2);
      assert(image.pixels.size() == frameSource.payload.size() && image.pixels[0] == 0x5a);
      assert(counted.total() == 1 && counted.imageCount == 1);
      assert(counted.images[0].failure == GraphicsImageFailure::UnsupportedFormat);
    }
  }
  {
    // A short buffer is planned at its declared size: delivered bytes are copied, the rest is zero.
    onegen::Frame shortFrame;
    shortFrame.width = 4; shortFrame.height = 4; shortFrame.stride = 4;
    shortFrame.pixel_format = 0x01080001;
    shortFrame.payload.assign(10, 0x7f);
    OneGenFrameAnomalies counted;
    const auto fitted = oneGenGraphicsFrame(shortFrame, 91, localSource, nullptr, nullptr, nullptr, {}, nullptr, &counted);
    assert(fitted && fitted->hasImage());
    const auto pixels = fitted->images().front().payload.pixels;
    assert(pixels.size() == 16 && pixels[9] == 0x7f && pixels[10] == 0 && pixels[15] == 0);
    assert(counted.total() == 1 && counted.images[0].failure == GraphicsImageFailure::TruncatedBytes);
    assert(counted.images[0].requiredBytes == 16);

    // A range part with fewer bytes than its layout keeps zeros and marks the missing samples invalid.
    onegen::Frame shortRange;
    shortRange.width = 2; shortRange.height = 2; shortRange.stride = 4;
    shortRange.pixel_format = 0x011000b8;
    shortRange.payload = {5, 0, 6, 0, 7, 0}; // the last sample is missing
    shortRange.scan3d_geometry.emplace();
    shortRange.scan3d_geometry->distance_unit = "Millimeter";
    shortRange.scan3d_geometry->output_mode = "CalibratedC";
    OneGenFrameAnomalies rangeCounted;
    const auto rangeFrame = oneGenGraphicsFrame(shortRange, 92, localSource, nullptr, nullptr, nullptr, {}, nullptr, &rangeCounted);
    assert(rangeFrame && rangeFrame->hasRangeFrame());
    const auto& shortRangePayload = rangeFrame->ranges().front().payload;
    assert(shortRangePayload.zValues[3] == 0.0F);
    assert(samplesEqual(shortRangePayload.validMask, std::vector<std::uint8_t>({1, 1, 1, 0})));
    assert(rangeCounted.partCount == 1 && rangeCounted.parts[0].reason == OneGenPlanarRejection::Reason::TruncatedPlane);
  }
  {
    // A stride below the row size or beyond the integer range is rewritten row by row into a valid stride.
    onegen::Frame narrow;
    narrow.width = 4; narrow.height = 3; narrow.stride = 2;
    narrow.pixel_format = 0x01080001;
    narrow.payload = {1, 2, 3, 4, 5, 6};
    OneGenFrameAnomalies counted;
    const auto fitted = oneGenGraphicsFrame(narrow, 93, localSource, nullptr, nullptr, nullptr, {}, nullptr, &counted);
    assert(fitted && fitted->hasImage());
    const auto& image = fitted->images().front().payload;
    assert(image.stride == 4 && image.height == 3 && image.pixels.size() == 12);
    assert(samplesEqual(image.pixels, std::vector<std::uint8_t>({1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0})));
    assert(counted.images[0].failure == GraphicsImageFailure::StrideBelowMinimum);

    onegen::Frame huge = narrow;
    huge.stride = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 100U;
    OneGenFrameAnomalies hugeCounted;
    const auto hugeFrame = oneGenGraphicsFrame(huge, 94, localSource, nullptr, nullptr, nullptr, {}, nullptr, &hugeCounted);
    assert(hugeFrame && hugeFrame->hasImage() && hugeFrame->images().front().payload.stride == 4);
    assert(hugeCounted.images[0].failure == GraphicsImageFailure::InvalidStride);

    // Invalid dimensions publish the bytes as a raw row layout instead of dropping them.
    onegen::Frame empty = narrow;
    empty.width = 0;
    OneGenFrameAnomalies dimensionCounted;
    const auto dimensionFrame = oneGenGraphicsFrame(empty, 95, localSource, nullptr, nullptr, nullptr, {}, nullptr, &dimensionCounted);
    assert(dimensionFrame && dimensionFrame->images().front().payload.pixels.size() == 6);
    assert(dimensionCounted.images[0].failure == GraphicsImageFailure::InvalidDimensions);
  }
  {
    // One bad part never removes another: every other part still publishes, and an absent part is only described.
    onegen::Frame parts;
    onegen::Frame::Part good;
    good.width = 2; good.height = 2; good.stride = 2; good.data_type = 1; good.data_format = 0x01080001;
    good.payload = {1, 2, 3, 4};
    auto truncated = good; truncated.payload = {9, 9, 9};
    auto unknown = good; unknown.data_format = 0x01100999; unknown.stride = 4; unknown.payload.assign(8, 7);
    auto absent = good; absent.payload.clear();
    parts.parts = {good, truncated, unknown, absent};
    OneGenFrameAnomalies counted;
    std::string description;
    const auto fitted = oneGenGraphicsFrame(parts, 96, localSource, &description, nullptr, nullptr, {}, nullptr, &counted);
    assert(fitted && fitted->images().size() == 3);
    assert(description.find("part=3 ") != std::string::npos && description.find("part=1 ") == std::string::npos);
    assert(counted.partCount == 2 && counted.total() == 2);
    assert(counted.parts[0].reason == OneGenPlanarRejection::Reason::TruncatedPlane && counted.parts[0].affectedPart == 1);
    assert(counted.parts[1].reason == OneGenPlanarRejection::Reason::UnsupportedPartFormat && counted.parts[1].affectedPart == 2);
    assert(fitted->images()[1].payload.pixels[3] == 0 && fitted->images()[1].payload.pixels[2] == 9);

    // Nothing is published only when no part has any bytes.
    auto none = parts;
    for (auto& part : none.parts) part.payload.clear();
    std::string noneDescription;
    assert(!oneGenGraphicsFrame(none, 97, localSource, &noneDescription));
    assert(noneDescription.find("no part with data") != std::string::npos);

    // Entries beyond the fixed capacity are still counted per reason.
    onegen::Frame many;
    for (int i = 0; i < 12; ++i) many.parts.push_back(unknown);
    OneGenFrameAnomalies overflow;
    const auto manyFrame = oneGenGraphicsFrame(many, 98, localSource, nullptr, nullptr, nullptr, {}, nullptr, &overflow);
    assert(manyFrame && manyFrame->images().size() == 12);
    assert(overflow.partCount == OneGenFrameAnomalies::capacity && overflow.total() == 12);
    assert(overflow.partOverflow[static_cast<std::size_t>(OneGenPlanarRejection::Reason::UnsupportedPartFormat)] == 4);
  }
  {
    // An auxiliary part never removes its range; a range without calibration is published as raw bytes.
    onegen::Frame ranged;
    ranged.scan3d_geometry.emplace();
    ranged.scan3d_geometry->distance_unit = "Millimeter";
    ranged.scan3d_geometry->output_mode = "CalibratedC";
    onegen::Frame::Part c;
    c.width = 2; c.height = 1; c.stride = 4; c.data_type = 5; c.data_format = 0x011000b8; c.payload = {1, 0, 2, 0};
    onegen::Frame::Part intensity;
    intensity.width = 2; intensity.height = 1; intensity.stride = 4; intensity.data_type = 1; intensity.data_format = 0x01100007;
    intensity.payload = {3, 0}; // one 16-bit sample only
    ranged.parts = {c, intensity};
    OneGenFrameAnomalies counted;
    const auto scene = oneGenGraphicsFrame(ranged, 99, localSource, nullptr, nullptr, nullptr, {}, nullptr, &counted);
    assert(scene && scene->hasRangeFrame());
    assert(samplesEqual(scene->ranges().front().payload.intensity, std::vector<float>({3.0F, 0.0F})));
    // The intensity part is also an image of its own; both uses of the short part are counted.
    assert(counted.partCount == 2 && counted.total() == 2);
    assert(counted.parts[0].reason == OneGenPlanarRejection::Reason::TruncatedPlane);
    assert(counted.parts[1].reason == OneGenPlanarRejection::Reason::AuxiliaryLayoutRejected);

    onegen::Frame abc;
    onegen::Frame::Part abcPart;
    abcPart.width = 1; abcPart.height = 1; abcPart.stride = 12; abcPart.data_type = 5; abcPart.data_format = 0x026000c0;
    abcPart.payload.assign(12, 1);
    abc.parts = {abcPart};
    OneGenFrameAnomalies abcCounted;
    const auto rawAbc = oneGenGraphicsFrame(abc, 100, localSource, nullptr, nullptr, nullptr, {}, nullptr, &abcCounted);
    assert(rawAbc && rawAbc->hasImage() && !rawAbc->hasRangeFrame());
    assert(rawAbc->images().front().payload.pixels.size() == 12);
    assert(abcCounted.partCount >= 1 && abcCounted.parts[0].reason == OneGenPlanarRejection::Reason::MissingCalibration);
  }
}
