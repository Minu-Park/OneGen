#pragma once

#include <string>
#include <string_view>

namespace onegen::producer {

/**
 * @brief Formats a producer load failure with exactly one terminal period before retry guidance.
 *
 * Refusal descriptions already end a sentence. Appending another period there produced
 * `producer.. Retry ...` in discovery failures, so the separator is owned by this composition.
 */
inline std::string inactiveProducerMessage(std::string_view reason) {
  std::string message = "Producer inactive";
  if (reason.empty()) {
    message += '.';
  } else {
    message += ": ";
    message += reason;
    const char last = message.back();
    if (last != '.' && last != '?' && last != '!') message += '.';
  }
  message += " Retry discovery explicitly after correcting the producer configuration.";
  return message;
}

}  // namespace onegen::producer
