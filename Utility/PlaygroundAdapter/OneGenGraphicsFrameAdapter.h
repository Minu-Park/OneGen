#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "onegen/types.hpp"

#include <cstdint>
#include <optional>
#include <string>

/** Convert one supported PFNC image without depending on a vendor SDK. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    std::string* rejection_reason = nullptr);
