#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "OneGenTypes.h"

#include <cstdint>
#include <optional>
#include <string>

/** Convert PFNC images, calibrated coordinates and native confidence previews without a vendor SDK. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    std::string* rejection_reason = nullptr);
