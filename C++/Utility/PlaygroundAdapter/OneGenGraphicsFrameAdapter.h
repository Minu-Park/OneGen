#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
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
        RangeInvalidDimensions, RangeSizeLimitExceeded, RangeStorageRefused, RangeStorageMisaligned
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

/** @brief Converts PFNC data and optionally describes a single 2D or planar assembly rejection. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    std::string* rejection_reason = nullptr,
    std::span<const std::uint8_t> image_payload = {},
    std::shared_ptr<const void> payload_owner = {},
    GraphicsImageStorage image_storage = {},
    std::optional<OneGenImageRejection>* image_rejection = nullptr,
    std::optional<OneGenPlanarRejection>* planar_rejection = nullptr);
