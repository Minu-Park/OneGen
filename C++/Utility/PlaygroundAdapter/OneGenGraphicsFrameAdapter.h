#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "engine/GraphicsFrameBuilder.h"
#include "engine/GraphicsFrameContext.h"
#include "engine/GraphicsImageWriter.h"
#include "OneGenTypes.h"

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

/** @brief Rule and layout observed for the single 2D image path; the frame is still fitted and published. */
struct OneGenImageRejection {
    enum class Conversion { None, Borrow, Copy, SwapByteOrder, JpegCopy };
    std::optional<GraphicsImageFailure> failure; ///< Empty when a chunk payload lacks an image.
    Conversion conversion;
    std::uint64_t effectiveStride;
    std::uint64_t minimumRowBytes;
    std::uint64_t requiredBytes;
};

/** @brief One anomaly observed while fitting a part, a range or an A/B/C group.
 * Axis masks use bits 0, 1 and 2 for A, B and C. Mismatches describe same-source,
 * same-region candidates excluded by metadata checks. The data is fitted and published;
 * only a part without any bytes or an unrepresentable extent is omitted.
 */
struct OneGenPlanarRejection {
    enum class Reason {
        MissingAxis, DuplicateAxis, InvalidLayout, StrideBelowMinimum,
        TruncatedPlane, JoinedRangeRejected, MissingCalibration, InvalidCalibration,
        UnsupportedGeometry, UnsupportedUnit, AuxiliaryLayoutRejected,
        RangeInvalidDimensions, RangeSizeLimitExceeded, RangeStorageRefused,
        UnsupportedPartFormat, InvalidPartDimensions, BufferRepaired
    };
    static constexpr std::size_t reasonCount = static_cast<std::size_t>(Reason::BufferRepaired) + 1U;
    Reason reason = Reason::MissingAxis;
    std::size_t anchorPart = 0;
    std::optional<std::size_t> affectedPart;
    unsigned matchedAxes = 0;
    unsigned duplicateAxes = 0;
    unsigned namespaceMismatchAxes = 0;
    unsigned shapeMismatchAxes = 0;
    std::size_t reportedStride = 0;
    std::size_t effectiveStride = 0;
    std::size_t minimumRowBytes = 0;
    std::size_t readableBytes = 0;
    std::size_t requiredBytes = 0; ///< Zero when the extent does not fit.
    std::size_t requestedStorageBytes = 0; ///< Destination size for a rejected range writer.
};

/** @brief Bounded, allocation-free anomaly record of one frame.
 * Entries beyond the capacity are counted per reason in the overflow arrays, so every anomaly
 * is accounted for without allocating per frame.
 */
struct OneGenFrameAnomalies {
    static constexpr std::size_t capacity = 8;
    static constexpr std::size_t imageReasonCount = static_cast<std::size_t>(GraphicsImageFailure::StorageRefused) + 2U;
    std::array<OneGenPlanarRejection, capacity> parts{};
    std::size_t partCount = 0;
    std::array<std::size_t, OneGenPlanarRejection::reasonCount> partOverflow{};
    std::array<OneGenImageRejection, capacity> images{};
    std::size_t imageCount = 0;
    std::array<std::size_t, imageReasonCount> imageOverflow{};

    /** @brief Index of an image rule in imageOverflow; zero is an image without a failure rule. */
    [[nodiscard]] static std::size_t imageIndex(const OneGenImageRejection& evidence) noexcept {
        return evidence.failure ? static_cast<std::size_t>(*evidence.failure) + 1U : 0U;
    }
    void add(const OneGenPlanarRejection& evidence) noexcept {
        if (partCount < parts.size()) parts[partCount++] = evidence;
        else ++partOverflow[static_cast<std::size_t>(evidence.reason)];
    }
    void add(const OneGenImageRejection& evidence) noexcept {
        if (imageCount < images.size()) images[imageCount++] = evidence;
        else ++imageOverflow[imageIndex(evidence)];
    }
    /** @brief Number of anomalies recorded, including those counted only in the overflow arrays. */
    [[nodiscard]] std::size_t total() const noexcept {
        std::size_t sum = partCount + imageCount;
        for (const auto count : partOverflow) sum += count;
        for (const auto count : imageOverflow) sum += count;
        return sum;
    }
};

/** @brief Fits owned PFNC data into GraphicsFrame resources using @p context_source.
 * Data that exists is always published: unknown formats map to the nearest GraphicsEngine format or
 * to raw Mono8 bytes, short buffers are zero-filled at the declared size, and a bad stride is rewritten
 * row by row. Every anomaly goes to @p anomalies (and the first one to the single-evidence outputs).
 * Nothing is published only when no part has any bytes, or a storage request is refused.
 */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameContextSource& context_source,
    std::string* rejection_reason = nullptr,
    std::optional<OneGenImageRejection>* image_rejection = nullptr,
    std::optional<OneGenPlanarRejection>* planar_rejection = nullptr,
    const std::string& source_name = {},
    GraphicsFrameError* storage_error = nullptr,
    OneGenFrameAnomalies* anomalies = nullptr);

/** @brief Completes a zero-copy live direct 2D frame from an already-reserved write region. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenBuildDirectImageFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameBuilder& builder, GraphicsWriteRegion<std::uint8_t> region,
    std::size_t used_bytes, std::string* rejection_reason = nullptr,
    std::optional<OneGenImageRejection>* image_rejection = nullptr,
    const std::string& source_name = {},
    OneGenFrameAnomalies* anomalies = nullptr);
