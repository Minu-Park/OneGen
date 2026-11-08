#pragma once

#include <cstdint>
#include <optional>

namespace onegen::direct_pool {

enum class Action : std::uint8_t {
  Disabled,
  Create,
  Reuse,
};

struct Decision {
  Action action = Action::Disabled;
  std::uint64_t slot_bytes = 0;
  const char* reason = "disabled";
};

/**
 * Pure pool decision shared by the production live-start path and its regression test.
 * Opt-in and callback availability are checked by the caller. A replaced lease has no
 * attached pool, and a payload change must create a new pool rather than reuse the old size.
 */
inline Decision decide(bool pool_attached,
                       std::optional<std::uint64_t> attached_slot_bytes,
                       std::optional<std::uint64_t> current_payload_bytes) noexcept
{
  if (!current_payload_bytes || *current_payload_bytes == 0)
    return {Action::Disabled, 0, "payload-unavailable"};
  if (!pool_attached) return {Action::Create, *current_payload_bytes, "not-attached"};
  if (!attached_slot_bytes) return {Action::Create, *current_payload_bytes, "slot-unavailable"};
  if (*attached_slot_bytes != *current_payload_bytes)
    return {Action::Create, *current_payload_bytes, "payload-changed"};
  return {Action::Reuse, *attached_slot_bytes, "unchanged"};
}

} // namespace onegen::direct_pool
