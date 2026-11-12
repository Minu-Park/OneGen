// Same-name producer libraries: refuse a different bound file, accept identical contents,
// runtime redistributables and unrelated names, and refuse the CTI before it is loaded.
#include "Internal/Gentl.h"
#include "Internal/ModuleGuard.h"
#include "OneGenError.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

void write(const fs::path& path, const std::string& contents) {
  std::ofstream(path, std::ios::binary) << contents;
}

void load(const fs::path& path) {
#ifdef _WIN32
  assert(LoadLibraryW(path.wstring().c_str()));
#else
  assert(dlopen(path.c_str(), RTLD_NOW));
#endif
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc >= 2);
  const fs::path probe = fs::canonical(argv[1]);
  load(probe);

  const auto root = fs::temp_directory_path() / ("onegen-guard-" + std::to_string(
#ifdef _WIN32
      GetCurrentProcessId()
#else
      getpid()
#endif
      ));
  fs::remove_all(root);
  // A CTI in `bin` is scanned from the producer root, so a sibling `lib` tree counts.
  fs::create_directories(root / "bin");
  fs::create_directories(root / "lib");
  const auto cti = root / "bin" / "Guarded.cti";
  write(cti, "not a library");

  assert(onegen::detail::producer_module_conflicts(cti).empty());

  write(root / "lib" / probe.filename(), "different build");
  const auto conflicts = onegen::detail::producer_module_conflicts(cti);
  assert(conflicts.size() == 1);
  assert(conflicts.front().shipped.filename() == probe.filename());
  assert(fs::equivalent(conflicts.front().loaded, probe));
  const auto message = onegen::detail::describe_module_conflicts(cti, conflicts);
  assert(message.find(probe.filename().string()) != std::string::npos);
  assert(message.find("Restart") != std::string::npos);

  // The loader refuses before mapping the producer.
  onegen::Gentl gentl;
  bool refused = false;
  try {
    gentl.load(cti);
  } catch (const onegen::Error& error) {
    refused = std::string(error.what()).find("refused") != std::string::npos;
  }
  assert(refused);
  assert(!gentl.lib);

  // Identical contents bind safely.
  fs::copy_file(probe, root / "lib" / probe.filename(), fs::copy_options::overwrite_existing);
  assert(onegen::detail::producer_module_conflicts(cti).empty());

  // Runtime redistributables and names not loaded in the process are not conflicts.
#ifdef _WIN32
  write(root / "bin" / "msvcp140.dll", "redistributable");
  write(root / "bin" / "vcruntime140.dll", "redistributable");
  write(root / "bin" / "kernel32.dll", "system copy");
  write(root / "bin" / "unrelated_vendor_runtime.dll", "vendor");
#else
  write(root / "bin" / "libstdc++.so.6", "redistributable");
  write(root / "bin" / "libunrelated_vendor_runtime.so", "vendor");
#endif
  assert(onegen::detail::producer_module_conflicts(cti).empty());

#ifdef _WIN32
  // A delay-loaded import reachable only through PATH: refused while missing, registered when
  // PATH provides it, and resolved on first call although the process does not search PATH.
  assert(argc == 4);
  const fs::path dependency = fs::canonical(argv[3]);
  fs::create_directories(root / "closure");
  const auto delayed = root / "closure" / "Delayed.cti";
  fs::copy_file(argv[2], delayed);

  auto environment = onegen::detail::inspect_producer(delayed);
  assert(environment.missing.size() == 1);
  assert(environment.missing.front().name == dependency.filename().string());
  assert(onegen::detail::describe_producer_refusal(delayed, environment).find("not found")
         != std::string::npos);
  onegen::Gentl missing;
  refused = false;
  try {
    missing.load(delayed);
  } catch (const onegen::Error& error) {
    refused = std::string(error.what()).find("not found") != std::string::npos;
  }
  assert(refused);
  assert(!missing.lib);

  std::wstring path(32768, L'\0');
  path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
  assert(SetEnvironmentVariableW(L"PATH",
                                 (dependency.parent_path().wstring() + L";" + path).c_str()));
  environment = onegen::detail::inspect_producer(delayed);
  assert(environment.missing.empty());
  assert(environment.conflicts.empty());
  const auto registers = [&](const fs::path& directory) {
    for (const auto& item : environment.directories)
      if (fs::equivalent(item, directory)) return true;
    return false;
  };
  assert(registers(dependency.parent_path()));
  assert(registers(delayed.parent_path()));

  // A different same-named file in the application directory would win the delay load.
  std::wstring executable(32768, L'\0');
  executable.resize(GetModuleFileNameW(nullptr, executable.data(),
                                       static_cast<DWORD>(executable.size())));
  const auto shadow = fs::path(executable).parent_path() / dependency.filename();
  write(shadow, "different build");
  const auto shadowed = onegen::detail::inspect_producer(delayed);
  fs::remove(shadow);
  assert(shadowed.conflicts.size() == 1);
  assert(shadowed.conflicts.front().shipped.filename() == dependency.filename());

  assert(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS));
  auto registrations = onegen::detail::register_producer_directories(environment.directories);
  const HMODULE module = LoadLibraryExW(
      delayed.wstring().c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  assert(module);
  const auto call = reinterpret_cast<int (*)()>(GetProcAddress(module, "onegen_guard_delayed"));
  assert(call && call() == 7);
  FreeLibrary(module);
  const auto directory_probe = dependency.parent_path() / "directory-lifetime-probe.dll";
  fs::copy_file(probe, directory_probe, fs::copy_options::overwrite_existing);
  const auto registered_probe = LoadLibraryExW(directory_probe.filename().c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_USER_DIRS);
  assert(registered_probe);
  FreeLibrary(registered_probe);
  registrations.clear();
  assert(!LoadLibraryExW(directory_probe.filename().c_str(), nullptr, LOAD_LIBRARY_SEARCH_USER_DIRS));
  fs::remove(directory_probe);
#endif

  fs::remove_all(root);
  std::cout << "Producer module conflicts are refused before load.\n";
  return 0;
}
