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
  enum class Module { Interface, Device, RemoteDevice };
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;
  ~Camera();

  void open(std::string_view query);
  void close();
  bool is_open() const;

  const DeviceInfo& info() const;
  std::vector<DataStreamInfo> data_streams() const;
  void select_data_stream(std::uint32_t index);
  std::uint32_t selected_data_stream() const;
  void set_buffer_count(std::size_t count);
  std::size_t buffer_count() const;
  void set_single_timeout(std::chrono::milliseconds timeout);
  std::chrono::milliseconds single_timeout() const;

  std::vector<Frame> single(std::size_t n);
  void live();
  void trigger();
  void stop();
  bool is_live() const;

  using FrameFn = std::function<void(const Frame&)>;
  using ErrorFn = std::function<void(std::string_view)>;
  /** Replaces the live-frame callback. The callback may call stop(), but must not destroy this camera. */
  void set_frame_callback(FrameFn cb);
  /** Replaces the live-error callback. The callback may call stop(), but must not destroy this camera. */
  void set_error_callback(ErrorFn cb);

  NodeMap& features();
  NodeMap& features(Module module);
  Node feature(std::string_view name);

  /** Saves streamable GenApi features without a vendor configuration API. */
  void save_features(std::string_view path, Module module = Module::RemoteDevice);
  /** Loads streamable GenApi features and optionally verifies their readback. */
  void load_features(std::string_view path, Module module = Module::RemoteDevice,
                     bool verify = true);

private:
  friend class OneGen;
  explicit Camera(OneGen* owner);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
