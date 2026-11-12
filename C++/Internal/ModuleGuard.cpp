#include "Internal/ModuleGuard.h"
#include <stdexcept>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif

namespace onegen::detail {
namespace {

constexpr std::size_t maximum_scanned_files = 4096;

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool is_library(const std::filesystem::path& path) {
  const auto ext = lower(path.extension().string());
#ifdef _WIN32
  return ext == ".dll";
#elif defined(__APPLE__)
  return ext == ".dylib";
#else
  return ext == ".so" || path.filename().string().find(".so.") != std::string::npos;
#endif
}

// Compiler runtime redistributables are backward compatible by vendor contract; binding a
// producer to the process copy is the supported behavior.
bool is_runtime_redistributable(const std::string& name) {
  static constexpr std::array prefixes{"msvcp1", "msvcr1", "vcruntime", "concrt", "vccorlib",
                                       "vcomp", "ucrtbase", "api-ms-win-", "ext-ms-win-",
                                       "libstdc++", "libgcc_s", "libc++", "libc.", "libm.",
                                       "libpthread", "libdl.", "librt."};
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [&](const char* prefix) { return name.rfind(prefix, 0) == 0; });
}

std::filesystem::path scan_root(const std::filesystem::path& cti) {
  const auto directory = cti.parent_path();
  if (lower(directory.filename().string()) == "bin" && directory.has_parent_path())
    return directory.parent_path();
  return directory;
}

/** Maps lower-case base names of the libraries already loaded in this process to their files. */
std::map<std::string, std::filesystem::path> loaded_modules() {
  std::map<std::string, std::filesystem::path> result;
#ifdef _WIN32
  // Base-name lookup is exactly what the loader uses for an already loaded import.
  (void)result;
#elif defined(__APPLE__)
  for (std::uint32_t index = 0, count = _dyld_image_count(); index < count; ++index) {
    if (const char* name = _dyld_get_image_name(index)) {
      const std::filesystem::path path(name);
      result.emplace(lower(path.filename().string()), path);
    }
  }
#else
  dl_iterate_phdr([](dl_phdr_info* info, size_t, void* user) {
    if (info->dlpi_name && *info->dlpi_name) {
      const std::filesystem::path path(info->dlpi_name);
      static_cast<std::map<std::string, std::filesystem::path>*>(user)->emplace(
          lower(path.filename().string()), path);
    }
    return 0;
  }, &result);
#endif
  return result;
}

std::optional<std::filesystem::path> loaded_path(
    const std::string& name, const std::map<std::string, std::filesystem::path>& loaded) {
#ifdef _WIN32
  (void)loaded;
  const auto wide = std::filesystem::path(name).wstring();
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, wide.c_str(), &module)
      || !module)
    return std::nullopt;
  std::wstring buffer(32768, L'\0');
  const auto length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0) return std::nullopt;
  buffer.resize(length);
  return std::filesystem::path(buffer);
#else
  if (const auto found = loaded.find(name); found != loaded.end()) return found->second;
  return std::nullopt;
#endif
}

bool is_system_library(const std::filesystem::path& path) {
#ifdef _WIN32
  std::wstring root(MAX_PATH, L'\0');
  const auto length = GetSystemDirectoryW(root.data(), static_cast<UINT>(root.size()));
  if (length == 0 || length >= root.size()) return false;
  root.resize(length);
  const auto file = lower(path.lexically_normal().string());
  const auto windows = lower(std::filesystem::path(root).lexically_normal().string());
  return file.rfind(windows + "\\", 0) == 0;
#else
  (void)path;
  return false;
#endif
}

bool same_file(const std::filesystem::path& left, const std::filesystem::path& right) {
  std::error_code error;
  if (std::filesystem::equivalent(left, right, error)) return true;
  const auto left_size = std::filesystem::file_size(left, error);
  if (error) return false;
  const auto right_size = std::filesystem::file_size(right, error);
  if (error || left_size != right_size) return false;
  std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
  if (!a || !b) return false;
  std::array<char, 65536> x{}, y{};
  while (a && b) {
    a.read(x.data(), x.size());
    b.read(y.data(), y.size());
    if (a.gcount() != b.gcount()) return false;
    if (!std::equal(x.begin(), x.begin() + a.gcount(), y.begin())) return false;
  }
  return a.eof() && b.eof();
}

#ifdef _WIN32
constexpr std::size_t maximum_closure_files = 512;
constexpr std::uintmax_t maximum_image_bytes = 512ULL << 20;

template <class T>
bool read_at(const std::vector<char>& image, std::size_t offset, T& value) {
  if (offset > image.size() || image.size() - offset < sizeof(T)) return false;
  std::memcpy(&value, image.data() + offset, sizeof(T));
  return true;
}

/** Static and delay-load import names of one PE image; empty when it cannot be parsed. */
std::vector<std::string> pe_imports(const std::filesystem::path& file) {
  std::vector<std::string> names;
  std::error_code error;
  const auto size = std::filesystem::file_size(file, error);
  if (error || size > maximum_image_bytes) return names;
  std::vector<char> image(static_cast<std::size_t>(size));
  {
    std::ifstream in(file, std::ios::binary);
    if (!in.read(image.data(), static_cast<std::streamsize>(image.size()))) return names;
  }
  std::uint32_t nt = 0, signature = 0;
  if (!read_at(image, 0x3C, nt) || !read_at(image, nt, signature) || signature != 0x00004550)
    return names;
  std::uint16_t sections = 0, optional_size = 0, magic = 0;
  const std::size_t file_header = nt + 4, optional = file_header + 20;
  if (!read_at(image, file_header + 2, sections) || !read_at(image, file_header + 16, optional_size)
      || !read_at(image, optional, magic))
    return names;
  const bool pe64 = magic == 0x20b;
  if (!pe64 && magic != 0x10b) return names;
  std::uint64_t image_base = 0;
  if (pe64) {
    read_at(image, optional + 24, image_base);
  } else {
    std::uint32_t base32 = 0;
    read_at(image, optional + 28, base32);
    image_base = base32;
  }
  const std::size_t directories = optional + (pe64 ? 112 : 96);
  const std::size_t section_table = optional + optional_size;
  const auto offset_of = [&](std::uint32_t rva, std::size_t& offset) {
    for (std::uint16_t index = 0; index < sections; ++index) {
      const std::size_t entry = section_table + index * 40U;
      std::uint32_t virtual_size = 0, virtual_address = 0, raw_size = 0, raw_pointer = 0;
      if (!read_at(image, entry + 8, virtual_size) || !read_at(image, entry + 12, virtual_address)
          || !read_at(image, entry + 16, raw_size) || !read_at(image, entry + 20, raw_pointer))
        return false;
      const auto extent = std::max(virtual_size, raw_size);
      if (rva >= virtual_address && rva - virtual_address < extent) {
        offset = raw_pointer + (rva - virtual_address);
        return offset < image.size();
      }
    }
    return false;
  };
  const auto name_at = [&](std::uint32_t rva) {
    std::size_t offset = 0;
    if (!offset_of(rva, offset)) return std::string{};
    const auto end = std::find(image.begin() + static_cast<std::ptrdiff_t>(offset), image.end(), '\0');
    return std::string(image.begin() + static_cast<std::ptrdiff_t>(offset), end);
  };
  const auto directory = [&](int index, std::uint32_t& rva) {
    std::uint32_t value = 0;
    if (!read_at(image, directories + static_cast<std::size_t>(index) * 8U, value)) return false;
    rva = value;
    return rva != 0;
  };

  std::uint32_t rva = 0;
  std::size_t offset = 0;
  if (directory(1, rva) && offset_of(rva, offset)) {
    for (std::size_t entry = offset;; entry += 20) {
      std::uint32_t lookup = 0, name = 0;
      if (!read_at(image, entry, lookup) || !read_at(image, entry + 12, name)) break;
      if (lookup == 0 && name == 0) break;
      if (auto text = name_at(name); !text.empty()) names.push_back(std::move(text));
    }
  }
  if (directory(13, rva) && offset_of(rva, offset)) {
    for (std::size_t entry = offset;; entry += 32) {
      std::uint32_t attributes = 0, name = 0;
      if (!read_at(image, entry, attributes) || !read_at(image, entry + 4, name)) break;
      if (name == 0) break;
      // Version-1 descriptors (attribute bit 0 clear) store virtual addresses.
      const auto name_rva = (attributes & 1U) ? name
          : static_cast<std::uint32_t>(name - static_cast<std::uint32_t>(image_base));
      if (auto text = name_at(name_rva); !text.empty()) names.push_back(std::move(text));
    }
  }
  return names;
}

std::filesystem::path windows_directory(bool system) {
  std::wstring buffer(MAX_PATH, L'\0');
  const auto length = system
      ? GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()))
      : GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer);
}

std::filesystem::path executable_directory() {
  std::wstring buffer(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

std::vector<std::filesystem::path> path_entries() {
  std::vector<std::filesystem::path> result;
  const auto length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  if (length == 0) return result;
  std::wstring value(length, L'\0');
  value.resize(GetEnvironmentVariableW(L"PATH", value.data(), length));
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(L';', start);
    auto item = value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    item.erase(std::remove(item.begin(), item.end(), L'"'), item.end());
    if (!item.empty()) result.emplace_back(item);
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return result;
}
#endif

}  // namespace

ProducerEnvironment inspect_producer(const std::filesystem::path& cti) {
  ProducerEnvironment result;
  result.conflicts = producer_module_conflicts(cti);
  std::error_code error;
  const auto cti_directory = std::filesystem::weakly_canonical(cti, error).parent_path();
  if (!cti_directory.empty()) result.directories.push_back(cti_directory);
#ifdef _WIN32
  const auto system = windows_directory(true);
  const auto windows = windows_directory(false);
  const auto path = path_entries();
  const auto application = executable_directory();
  const auto loaded = loaded_modules();
  std::vector<std::filesystem::path> resolved;
  std::map<std::string, bool> visited;
  std::vector<std::filesystem::path> pending{cti};
  const auto add_directory = [&](const std::filesystem::path& directory) {
    const auto key = lower(directory.lexically_normal().string());
    if (std::none_of(result.directories.begin(), result.directories.end(), [&](const auto& item) {
          return lower(item.lexically_normal().string()) == key;
        }))
      result.directories.push_back(directory.lexically_normal());
  };
  const auto add_conflict = [&](const std::filesystem::path& shipped,
                                const std::filesystem::path& bound) {
    if (std::none_of(result.conflicts.begin(), result.conflicts.end(), [&](const auto& item) {
          return lower(item.shipped.filename().string()) == lower(shipped.filename().string());
        }))
      result.conflicts.push_back({shipped, bound});
  };
  while (!pending.empty() && visited.size() < maximum_closure_files) {
    const auto file = pending.back();
    pending.pop_back();
    for (const auto& import : pe_imports(file)) {
      const auto name = lower(import);
      if (name.rfind("api-ms-win-", 0) == 0 || name.rfind("ext-ms-win-", 0) == 0) continue;
      if (visited.count(name)) continue;
      visited[name] = true;
      const auto in = [&](const std::filesystem::path& directory) {
        return !directory.empty() && std::filesystem::is_regular_file(directory / import, error);
      };
      if (!in(cti_directory) && (in(system) || in(windows))) continue;
      std::optional<std::filesystem::path> found;
      if (in(cti_directory)) {
        found = cti_directory / import;
      } else {
        for (const auto& directory : path) {
          if (in(directory)) {
            found = directory / import;
            break;
          }
        }
      }
      if (!found) {
        if (!loaded_path(name, loaded) && !in(application)) result.missing.push_back({import, file});
        continue;
      }
      if (is_runtime_redistributable(name)) continue;
      add_directory(found->parent_path());
      resolved.push_back(*found);
      if (const auto bound = loaded_path(name, loaded);
          bound && !is_system_library(*bound) && !same_file(*bound, *found))
        add_conflict(*found, *bound);
      pending.push_back(*found);
    }
  }
  // Delay-loaded imports resolve through the application directory and every registered
  // directory in unspecified order, so a different same-named file in any of them can win.
  std::vector<std::filesystem::path> process_search{application};
  process_search.insert(process_search.end(), result.directories.begin(), result.directories.end());
  for (const auto& file : resolved) {
    for (const auto& directory : process_search) {
      const auto other = directory / file.filename();
      if (!directory.empty() && std::filesystem::is_regular_file(other, error)
          && !same_file(other, file))
        add_conflict(file, other);
    }
  }
#endif
  return result;
}

std::vector<std::shared_ptr<void>> register_producer_directories(
    const std::vector<std::filesystem::path>& directories) {
  std::vector<std::shared_ptr<void>> registrations;
#ifdef _WIN32
  for (const auto& directory : directories) {
    auto* cookie = AddDllDirectory(directory.lexically_normal().wstring().c_str());
    if (!cookie) throw std::runtime_error("Cannot register producer DLL directory: " + directory.string());
    registrations.emplace_back(cookie, [](void* value) { RemoveDllDirectory(value); });
  }
#else
  (void)directories;
#endif
  return registrations;
}

std::string describe_producer_refusal(const std::filesystem::path& cti,
                                      const ProducerEnvironment& environment) {
  if (environment.missing.empty())
    return describe_module_conflicts(cti, environment.conflicts);
  std::string message = "loadCti: refused " + cti.string() + ": required libraries were not found (";
  for (std::size_t index = 0; index < environment.missing.size(); ++index) {
    if (index) message += "; ";
    if (index == 8) {
      message += std::to_string(environment.missing.size() - index) + " more";
      break;
    }
    message += environment.missing[index].name + " needed by "
        + environment.missing[index].needed_by.filename().string();
  }
  message += "). Install or repair the producer so its libraries are on its directory or PATH.";
  if (!environment.conflicts.empty())
    message += " " + describe_module_conflicts(cti, environment.conflicts);
  return message;
}

std::vector<ModuleConflict> producer_module_conflicts(const std::filesystem::path& cti) {
  std::vector<ModuleConflict> conflicts;
  const auto root = scan_root(cti);
  std::error_code error;
  if (root.empty() || !std::filesystem::is_directory(root, error)) return conflicts;
  const auto loaded = loaded_modules();
  std::size_t scanned = 0;
  auto it = std::filesystem::recursive_directory_iterator(
      root, std::filesystem::directory_options::skip_permission_denied, error);
  for (const auto end = std::filesystem::recursive_directory_iterator(); !error && it != end;
       it.increment(error)) {
    if (++scanned > maximum_scanned_files) break;
    const auto& entry = *it;
    if (!entry.is_regular_file(error) || !is_library(entry.path())) continue;
    const auto name = lower(entry.path().filename().string());
    if (is_runtime_redistributable(name)) continue;
    const auto bound = loaded_path(name, loaded);
    if (!bound || is_system_library(*bound) || same_file(*bound, entry.path())) continue;
    conflicts.push_back({entry.path(), *bound});
  }
  return conflicts;
}

std::string describe_module_conflicts(const std::filesystem::path& cti,
                                      const std::vector<ModuleConflict>& conflicts) {
  std::string message = "loadCti: refused " + cti.string()
      + ": the process already loaded different libraries with the same names (";
  for (std::size_t index = 0; index < conflicts.size(); ++index) {
    if (index) message += "; ";
    if (index == 8) {
      message += std::to_string(conflicts.size() - index) + " more";
      break;
    }
    message += conflicts[index].shipped.filename().string() + " loaded from "
        + conflicts[index].loaded.string();
  }
  message += "). Close the owning plugin's sessions and release retained frames before retrying. "
             "Restart without that plugin or producer if its runtime remains loaded.";
  return message;
}

}  // namespace onegen::detail
