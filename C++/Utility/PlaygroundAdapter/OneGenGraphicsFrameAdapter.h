#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "engine/GraphicsImageWriter.h"
#include "OneGenTypes.h"

#include <cstdint>
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

/** @brief Converts PFNC data and supplies optional typed evidence for a rejected single 2D image. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    std::string* rejection_reason = nullptr,
    std::span<const std::uint8_t> image_payload = {},
    std::shared_ptr<const void> payload_owner = {},
    GraphicsImageStorage image_storage = {},
    std::optional<OneGenImageRejection>* image_rejection = nullptr);
