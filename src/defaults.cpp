#include "onegen/onegen.hpp"

#include <cstdlib>
#include <filesystem>

namespace onegen::defaults {

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

  for (const auto& p : candidates)
    if (std::filesystem::exists(p)) out.push_back(p);

  std::vector<std::filesystem::path> extra;
  for (const auto& p : out) {
    auto u3v = p.parent_path() / "ProducerU3V.cti";
    if (std::filesystem::exists(u3v)) extra.push_back(u3v);
  }
  out.insert(out.end(), extra.begin(), extra.end());
  return out;
}

std::filesystem::path pylonCti() {
  auto all = pylonCtis();
  return all.empty() ? std::filesystem::path{} : all.front();
}

}  // namespace onegen::defaults
