#pragma once

/** @file OneGenGraphicsFrameAdapter.h @brief Converts owned OneGen frames to GraphicsFrame. */

#include "engine/GraphicsFrame.h"
#include "OneGenTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

/** @brief Converts PFNC data; diagnostics describe rejected parts even when other resources survive. */
[[nodiscard]] std::optional<GraphicsFrame> oneGenGraphicsFrame(
    const onegen::Frame& source, std::uint64_t sequence,
    std::string* rejection_reason = nullptr,
    std::span<const std::uint8_t> image_payload = {},
    std::shared_ptr<const void> payload_owner = {});
