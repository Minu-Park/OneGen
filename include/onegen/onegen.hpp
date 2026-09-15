#pragma once

#include "onegen/camera.hpp"
#include "onegen/types.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace onegen {

namespace defaults {
// Documented locations only. Nothing is loaded until loadCti().
std::filesystem::path pylonCti();
std::vector<std::filesystem::path> pylonCtis();
/** Return canonical CTIs recursively found below one trusted package root. */
std::vector<std::filesystem::path> producerCtis(const std::filesystem::path& root);
/** Return canonical CTIs from ONEGEN_CTI_ROOT, standard GenTL paths, and documented locations. */
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
  void unloadCti(const std::filesystem::path& cti);
  /** @return Whether at least one GenTL producer is loaded. */
  bool hasCti() const;

  std::vector<DeviceInfo> enumerate(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

  Camera& addCamera();
  void removeCamera(Camera& cam);
  std::span<Camera* const> cameras() const;

private:
  friend class Camera;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
