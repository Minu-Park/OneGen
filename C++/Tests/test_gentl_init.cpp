#include "Internal/Gentl.h"

#include <cassert>
#include <string>
#include <cstring>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
bool mapped(const char* path) {
#ifdef _WIN32
  return GetModuleHandleW(std::filesystem::path(path).filename().c_str()) != nullptr;
#else
  void* module = dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
  if (module) dlclose(module);
  return module != nullptr;
#endif
}
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
  assert(argc == 4);
  onegen::Gentl producer;
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool rejected = false;
    try { producer.load(argv[3]); }
    catch (const onegen::Error& error) {
      rejected = std::string(error.what()).find("missing symbol") != std::string::npos;
    }
    assert(rejected && !producer.lib && !producer.GCInitLib);
    assert(!mapped(argv[3]));
  }
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
    assert(!mapped(argv[1]));
    producer.unload();
  }
  // Closing the runtime releases the native mapping. Reopening starts a fresh lifetime;
  // moves transfer ownership and repeated explicit cleanup cannot double-close it.
  for (int attempt = 0; attempt < 3; ++attempt) {
    assert(!mapped(argv[2]));
    {
      onegen::Gentl loaded;
      loaded.load(argv[2]);
      assert(mapped(argv[2]));
      onegen::Gentl moved = std::move(loaded);
      loaded.unload();
      assert(mapped(argv[2]));
      if (attempt == 0) { moved.unload(); moved.unload(); }
    }
    assert(!mapped(argv[2]));
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
