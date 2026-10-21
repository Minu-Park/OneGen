#pragma once

#include "Internal/StageLog.h"

#include <exception>
#include <string>

namespace onegen::worker {

/** @brief Describes the active C++ exception; call only from an exception handler. */
inline std::string currentExceptionMessage()
{
  try { throw; }
  catch (const std::exception& error) { return error.what(); }
  catch (...) { return "Unknown C++ exception."; }
}

/** @brief Reports an active request exception and returns its failed-reply detail. */
inline std::string reportRequestException(const std::string& context)
{
  try { throw; }
  catch (const std::exception& error) {
    onegen::stage_log("%s detail=%s", context.c_str(), error.what());
    return error.what();
  }
  catch (...) {
    const auto detail = currentExceptionMessage();
    onegen::stage_log_warning("%s detail=%s", context.c_str(), detail.c_str());
    return detail;
  }
}

} // namespace onegen::worker
