#pragma once

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

} // namespace onegen::worker
