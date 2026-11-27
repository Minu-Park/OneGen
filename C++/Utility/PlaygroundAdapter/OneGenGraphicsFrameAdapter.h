#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "engine/GraphicsFrameBuilder.h"
#include "engine/GraphicsFrameContext.h"
#include "engine/GraphicsImageWriter.h"
#include "OneGenTypes.h"

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

/** @brief Failed rule and layout used by the single 2D image path. */
struct OneGenImageRejection {
    enum class Conversion { None, Borrow, Copy, SwapByteOrder, JpegCopy };
    std::optional<GraphicsImageFailure> failure; ///< Empty when a chunk payload lacks an image.
    Conversion conversion;
    std::uint64_t effectiveStride;
    std::uint64_t minimumRowBytes;
    std::uint64_t requiredBytes;
};

/** @brief First failed assembly for a multipart A/B/C group.
 * Axis masks use bits 0, 1 and 2 for A, B and C. Mismatches describe same-source,
 * same-region candidates excluded by metadata checks.
 */
struct OneGenPlanarRejection {
    enum class Reason {
        MissingAxis, DuplicateAxis, InvalidLayout, StrideBelowMinimum,
        TruncatedPlane, JoinedRangeRejected, MissingCalibration, InvalidCalibration,
        UnsupportedGeometry, UnsupportedUnit, AuxiliaryLayoutRejected,
        RangeInvalidDimensions, RangeSizeLimitExceeded, RangeStorageRefused
    };
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

/** @brief Converts owned PFNC data using @p context_source and optionally describes rejections. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameContextSource& context_source,
    std::string* rejection_reason = nullptr,
    std::optional<OneGenImageRejection>* image_rejection = nullptr,
    std::optional<OneGenPlanarRejection>* planar_rejection = nullptr,
    const std::string& source_name = {},
    GraphicsFrameError* storage_error = nullptr);

/** @brief Completes a zero-copy live direct 2D frame from an already-reserved write region. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenBuildDirectImageFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    GraphicsFrameBuilder& builder, GraphicsWriteRegion<std::uint8_t> region,
    std::size_t used_bytes, std::string* rejection_reason = nullptr,
    std::optional<OneGenImageRejection>* image_rejection = nullptr,
    const std::string& source_name = {});
