#pragma once

#include <cstdarg>
#include <cstdio>

namespace onegen {

/** Writes one flushed `[GenICam]` stage line to stderr for host forwarding. */
inline void stage_log(const char* fmt, ...)
{
  std::fputs("[GenICam] ", stderr);
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stderr, fmt, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
}

}  // namespace onegen
