#include "gentl.hpp"

#include <sstream>
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
  HMODULE h = LoadLibraryW(p.wstring().c_str());
  if (!h) throw Error("loadCti: LoadLibrary failed: " + p.string());
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
  lib = load_lib(cti);
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

  check(GCInitLib(), "GCInitLib");
  check(TLOpen(&tl), "TLOpen");
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
