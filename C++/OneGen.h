#pragma once

#include "OneGenCamera.h"
#include "OneGenTypes.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace onegen {

namespace defaults {
// Documented locations only. Nothing is loaded until loadCti().
std::vector<std::filesystem::path> pylonCtis();
/** Return canonical CTIs recursively found below one directory. */
std::vector<std::filesystem::path> producerCtis(const std::filesystem::path& root);
/** Return canonical CTIs from standard GenTL paths and documented vendor locations. */
std::vector<std::filesystem::path> producerCtis();
}  // namespace defaults

class OneGen {
public:
  OneGen();
  ~OneGen();
  OneGen(const OneGen&) = delete;
  OneGen& operator=(const OneGen&) = delete;

  /** Load a producer once; repeated calls for the same canonical path are harmless. */
  void loadCti(const std::filesystem::path& cti);
  std::vector<DeviceInfo> enumerate(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

  Camera& addCamera();

private:
  friend class Camera;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
