#pragma once

#include <string_view>

namespace onegen {

/** @brief Severity of one OneGen diagnostic record. */
enum class LogLevel : unsigned char { Debug, Info, Warning };

/** @brief Receives complete diagnostic records; it must not throw or block. */
using LogSink = void (*)(LogLevel level, std::string_view message) noexcept;

/** @brief Routes OneGen diagnostics to a consumer sink; null restores the standard stream. */
void set_log_sink(LogSink sink) noexcept;

}  // namespace onegen
