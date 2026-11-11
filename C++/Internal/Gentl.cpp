#include "Internal/Gentl.h"
#include "Internal/ModuleGuard.h"
#include "Internal/StageLog.h"

#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <mutex>

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

void* sym(void* h, const char* name) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), name));
#else
  return dlsym(h, name);
#endif
}

}  // namespace

void Gentl::fail(const char* where, GenTL::GC_ERROR result) const {
  // Preserve the primary failure even if a producer faults during diagnostic lookup.
  stage_log("GenTL operation failed: operation=%s code=%d", where, static_cast<int>(result));
  char buf[1024] = {};
  size_t n = sizeof(buf);
  GenTL::GC_ERROR code = GenTL::GC_ERR_ERROR;
  const auto detail_result = GCGetLastError ? GCGetLastError(&code, buf, &n) : GenTL::GC_ERR_NOT_IMPLEMENTED;
  buf[sizeof(buf) - 1] = '\0';
  std::ostringstream o;
  o << where << " failed (" << result << ")";
  if (detail_result == GenTL::GC_ERR_SUCCESS) o << ": " << buf;
  else o << "; error detail unavailable (" << detail_result << ")";
  throw Error(o.str());
}

void Gentl::check(GenTL::GC_ERROR err, const char* where) const {
  if (err != GenTL::GC_ERR_SUCCESS) fail(where, err);
}

void Gentl::load(const std::filesystem::path& cti) {
  // Serialize inspection, process-wide directory registration and initialization among
  // OneGen loads. Independent consumer loaders do not participate in this lock; see
  // docs/IN_PROCESS_PRODUCERS.md for the remaining cross-loader race.
  static std::mutex producer_load_mutex;
  const std::lock_guard load_lock(producer_load_mutex);
  const auto started = std::chrono::steady_clock::now();
  // A same-name library already bound to another file would be silently shared by this
  // producer; refuse before the loader runs any of its code.
  // A missing delay-loaded import would terminate the process on first use. The consumer
  // process does not search PATH, so the producer's library directories are registered.
  const auto environment = detail::inspect_producer(cti);
  if (!environment.conflicts.empty() || !environment.missing.empty()) {
    stage_log_warning("producer refused: cti=%s conflicts=%zu missing=%zu", cti.string().c_str(),
                      environment.conflicts.size(), environment.missing.size());
    throw Error(detail::describe_producer_refusal(cti, environment));
  }
  detail::register_producer_directories(environment.directories);
  lib = load_lib(cti);
  const auto loaded = std::chrono::steady_clock::now();
#define OG_SYM(name)                                                \
  do {                                                              \
    name = reinterpret_cast<decltype(name)>(sym(lib, #name));       \
    if (!name) {                                                    \
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

  const auto init_result = GCInitLib();
  if (init_result != GenTL::GC_ERR_SUCCESS) {
    // A failed initialization does not establish a usable producer runtime.
    // Preserve its return code without calling GCGetLastError or GCCloseLib.
    *this = Gentl{};
    throw Error("GCInitLib failed (" + std::to_string(init_result) + ")");
  }
  const auto initialized = std::chrono::steady_clock::now();
  EventGetInfo = reinterpret_cast<decltype(EventGetInfo)>(sym(lib, "EventGetInfo"));
  EventGetDataInfo = reinterpret_cast<decltype(EventGetDataInfo)>(sym(lib, "EventGetDataInfo"));
  check(TLOpen(&tl), "TLOpen");
  const auto opened = std::chrono::steady_clock::now();
  const auto ms = [](const auto begin, const auto end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
  };
  stage_log("producer initialized: load_library_ms=%lld gc_init_ms=%lld tl_open_ms=%lld total_ms=%lld",
            static_cast<long long>(ms(started, loaded)),
            static_cast<long long>(ms(loaded, initialized)),
            static_cast<long long>(ms(initialized, opened)),
            static_cast<long long>(ms(started, opened)));
}

void Gentl::close_tl() {
  if (tl && TLClose) {
    TLClose(tl);
    tl = nullptr;
  }
}

// Producer libraries stay mapped until process exit. Unloading a vendor runtime that may
// still own threads, callbacks or process-global state is not safe in the consumer process;
// loading the same canonical CTI again reuses the mapped module.
void Gentl::unload() {
  close_tl();
  if (GCCloseLib) GCCloseLib();
  lib = nullptr;
}

}  // namespace onegen
