#include "onegen/onegen.hpp"

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
  if (!error) paths.push_back(canonical);
}

void appendCtisRecursively(std::vector<std::filesystem::path>& paths,
                           const std::filesystem::path& root) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator it(root, error), last;
       !error && it != last; it.increment(error))
    appendCti(paths, it->path());
}

}  // namespace

std::vector<std::filesystem::path> pylonCtis() {
  std::vector<std::filesystem::path> out;
  std::vector<std::filesystem::path> candidates;

#if defined(__APPLE__)
  candidates = {
      "/Library/Frameworks/pylon.framework/Libraries/gentlproducer/gtl/ProducerGEV.cti",
      "/Library/Frameworks/pylon.framework/Versions/A/Libraries/gentlproducer/gtl/ProducerGEV.cti",
  };
#elif defined(_WIN32)
  if (const char* root = std::getenv("PYLON_ROOT")) {
    candidates.push_back(std::filesystem::path(root) / "Runtime" / "x64" / "ProducerGEV.cti");
  }
  candidates.push_back("C:/Program Files/Basler/pylon 8/Runtime/x64/ProducerGEV.cti");
  candidates.push_back("C:/Program Files/Basler/pylon 7/Runtime/x64/ProducerGEV.cti");
#else
  candidates = {
      "/opt/pylon/lib/gentlproducer/gtl/ProducerGEV.cti",
  };
#endif

  for (const auto& p : candidates) {
    if (!std::filesystem::exists(p)) continue;
    const auto canonical = std::filesystem::weakly_canonical(p);
    if (std::find(out.begin(), out.end(), canonical) == out.end()) out.push_back(canonical);
  }

  std::vector<std::filesystem::path> extra;
  for (const auto& p : out) {
    auto u3v = p.parent_path() / "ProducerU3V.cti";
    if (std::filesystem::exists(u3v)) extra.push_back(std::filesystem::weakly_canonical(u3v));
  }
  out.insert(out.end(), extra.begin(), extra.end());
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::filesystem::path pylonCti() {
  auto all = pylonCtis();
  return all.empty() ? std::filesystem::path{} : all.front();
}

std::vector<std::filesystem::path> producerCtis(const std::filesystem::path& root) {
  std::vector<std::filesystem::path> paths;
  std::error_code error;
  if (std::filesystem::is_directory(root, error)) appendCtisRecursively(paths, root);
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

std::vector<std::filesystem::path> producerCtis() {
  auto paths = pylonCtis();
  if (const char* package_root = std::getenv("ONEGEN_CTI_ROOT")) {
    const auto package_paths = producerCtis(std::filesystem::path(package_root));
    paths.insert(paths.end(), package_paths.begin(), package_paths.end());
  }
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
