#include "Internal/Gentl.h"

#include <cassert>
#include <string>
#include <cstring>

namespace {
GenTL::GC_ERROR GC_CALLTYPE detail_unavailable(GenTL::GC_ERROR*, char*, size_t*)
{
  return GenTL::GC_ERR_ERROR;
}
GenTL::GC_ERROR GC_CALLTYPE detail_available(GenTL::GC_ERROR* code, char* text, size_t*)
{
  *code = static_cast<GenTL::GC_ERROR>(-12345);
  std::strcpy(text, "fixture diagnostic");
  return GenTL::GC_ERR_SUCCESS;
}
}

int main(int argc, char** argv)
{
  assert(argc == 2);
  onegen::Gentl producer;
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool rejected = false;
    try {
      producer.load(argv[1]);
    } catch (const onegen::Error& error) {
      rejected = std::string(error.what()) == "GCInitLib failed (-1001)";
    }
    assert(rejected);
    assert(producer.lib == nullptr);
    assert(producer.GCGetLastError == nullptr);
    assert(producer.GCCloseLib == nullptr);
    producer.unload();
  }
  // A diagnostic failure or a different diagnostic code cannot replace the call result.
  for (const auto detail : {detail_unavailable, detail_available}) {
    onegen::Gentl reporting;
    reporting.GCGetLastError = detail;
    bool preserved = false;
    try { reporting.check(GenTL::GC_ERR_ACCESS_DENIED, "IFOpenDevice"); }
    catch (const onegen::Error& error) {
      const std::string text = error.what();
      preserved = text.starts_with("IFOpenDevice failed ("
          + std::to_string(GenTL::GC_ERR_ACCESS_DENIED) + ")")
          && text.find("-12345") == std::string::npos;
      assert(text.find(detail == detail_available ? "fixture diagnostic" : "error detail unavailable")
          != std::string::npos);
    }
    assert(preserved);
  }
}
