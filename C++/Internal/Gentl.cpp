#include "Internal/Gentl.h"
#include "Internal/StageLog.h"

#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace onegen {
namespace {

void* load_lib(const std::filesystem::path& p) {
#ifdef _WIN32
  const HMODULE h = LoadLibraryExW(
      p.wstring().c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!h) {
    throw Error("loadCti: LoadLibrary failed: " + p.string() + " ("
                + std::to_string(GetLastError()) + ")");
  }
  return h;
#else
  void* h = dlopen(p.c_str(), RTLD_NOW);
  if (!h) throw Error(std::string("loadCti: dlopen failed: ") + dlerror());
  return h;
#endif
}

void unload_lib(void* h) {
  if (!h) return;
#ifdef _WIN32
  FreeLibrary(static_cast<HMODULE>(h));
#else
  dlclose(h);
#endif
}

void* sym(void* h, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
#else
  return dlsym(h, name);
#endif
}

}  // namespace

void Gentl::fail(const char* where) const {
  char buf[1024] = {};
  size_t n = sizeof(buf);
  GenTL::GC_ERROR code = GenTL::GC_ERR_ERROR;
  if (GCGetLastError) GCGetLastError(&code, buf, &n);
  std::ostringstream o;
  o << where << ": " << buf << " (" << code << ")";
  throw Error(o.str());
}

void Gentl::check(GenTL::GC_ERROR err, const char* where) const {
  if (err != GenTL::GC_ERR_SUCCESS) fail(where);
}

void Gentl::load(const std::filesystem::path& cti) {
  stage_log("worker LoadLibrary start cti=%s", cti.string().c_str());
  lib = load_lib(cti);
  stage_log("worker LoadLibrary ok");
#define OG_SYM(name)                                                \
  do {                                                              \
    name = reinterpret_cast<decltype(name)>(sym(lib, #name));       \
    if (!name) {                                                    \
      unload_lib(lib);                                              \
      lib = nullptr;                                                \
      throw Error(std::string("loadCti: missing symbol ") + #name); \
    }                                                               \
  } while (0);
  OG_GENTL_SYMS(OG_SYM)
#undef OG_SYM

  DSGetNumBufferParts = reinterpret_cast<decltype(DSGetNumBufferParts)>(
      sym(lib, "DSGetNumBufferParts"));
  DSGetBufferPartInfo = reinterpret_cast<decltype(DSGetBufferPartInfo)>(
      sym(lib, "DSGetBufferPartInfo"));
  DSGetBufferChunkData = reinterpret_cast<decltype(DSGetBufferChunkData)>(
      sym(lib, "DSGetBufferChunkData"));

  stage_log("worker GCInitLib start");
  check(GCInitLib(), "GCInitLib");
  stage_log("worker GCInitLib ok");
  EventGetInfo = reinterpret_cast<decltype(EventGetInfo)>(sym(lib, "EventGetInfo"));
  EventGetDataInfo = reinterpret_cast<decltype(EventGetDataInfo)>(sym(lib, "EventGetDataInfo"));
  stage_log("worker TLOpen start");
  check(TLOpen(&tl), "TLOpen");
  stage_log("worker TLOpen ok");
}

void Gentl::close_tl() {
  if (tl && TLClose) {
    TLClose(tl);
    tl = nullptr;
  }
}

void Gentl::unload() {
  close_tl();
  if (GCCloseLib) GCCloseLib();
  unload_lib(lib);
  lib = nullptr;
}

}  // namespace onegen
