#include "OneGen.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace onegen::defaults {

namespace {

void appendCti(std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(candidate, error)) return;
  auto extension = candidate.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (extension != ".cti") return;
  const auto canonical = std::filesystem::weakly_canonical(candidate, error);
  if (error) return;
  for (const auto& path : paths) {
    error.clear();
    if (std::filesystem::equivalent(path, canonical, error) && !error) return;
  }
  paths.push_back(canonical);
}

void appendCtisRecursively(std::vector<std::filesystem::path>& paths,
                           const std::filesystem::path& root) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(root, error), last;
       !error && it != last; it.increment(error))
    appendCti(paths, it->path());
}

}  // namespace

std::vector<std::filesystem::path> producerCtis(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> paths;
  std::error_code error;
  if (std::filesystem::is_directory(root, error)) appendCtisRecursively(paths, root);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::filesystem::path> producerCtis() {
  std::vector<std::filesystem::path> paths;
  const char* environment = std::getenv(sizeof(void*) == 8
      ? "GENICAM_GENTL64_PATH" : "GENICAM_GENTL32_PATH");
  if (environment) {
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::string value(environment);
    for (std::size_t begin = 0; begin <= value.size();) {
      const auto end = value.find(separator, begin);
      const std::filesystem::path entry = value.substr(begin, end - begin);
      std::error_code error;
      if (!entry.empty()) {
        if (std::filesystem::is_directory(entry, error)) {
          for (std::filesystem::directory_iterator it(entry, error), last; !error && it != last;
               it.increment(error))
            appendCti(paths, it->path());
        } else {
          appendCti(paths, entry);
        }
      }
      if (end == std::string::npos) break;
      begin = end + 1;
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

}  // namespace onegen::defaults
