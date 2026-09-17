#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace onegen {
/** Emits opt-in monotonic connection timings without changing the worker protocol. */
class ConnectTiming {
public:
  explicit ConnectTiming(const char* scope) : scope_(scope) {}
  void mark(const char* stage) {
    const auto now = std::chrono::steady_clock::now();
    if (const auto* enabled = std::getenv("ONEGEN_TRACE_CONNECT"); enabled && enabled[0] == '1') {
      std::fprintf(stderr, "[GenICam] connect timing: scope=%s stage=%s duration=%.3f ms total=%.3f ms\n",
          scope_, stage, std::chrono::duration<double, std::milli>(now - last_).count(),
          std::chrono::duration<double, std::milli>(now - start_).count());
      std::fflush(stderr);
    }
    last_ = now;
  }
private:
  const char* scope_;
  std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_ = start_;
};
}
