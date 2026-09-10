#pragma once

#include "onegen/types.hpp"
#include "onegen/node.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace onegen {

class OneGen;

class Camera {
public:
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;
  ~Camera();

  void open(std::string_view query);
  void close();
  bool is_open() const;

  const DeviceInfo& info() const;

  std::vector<Frame> single(std::size_t n);
  void live();
  void stop();
  bool is_live() const;

  using FrameFn = std::function<void(const Frame&)>;
  void set_frame_callback(FrameFn cb);

  NodeMap& features();
  Node feature(std::string_view name);

private:
  friend class OneGen;
  explicit Camera(OneGen* owner);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
