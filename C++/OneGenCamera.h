#pragma once

#include "OneGenTypes.h"
#include "OneGenNode.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace onegen {

class OneGen;

class Camera {
public:
  enum class Module { Interface, Device, DataStream, RemoteDevice, System };
  Camera(const Camera&) = delete;
  Camera& operator=(const Camera&) = delete;
  ~Camera();

  void open(std::string_view query);
  void close();
  bool is_open() const;

  const DeviceInfo& info() const;
  std::vector<DataStreamInfo> data_streams() const;
  void select_data_stream(std::uint32_t index);

  /** Reopens the selected GenTL stream and allocates buffers for the current feature state. */
  void prepare();
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
  /** Polls GenApi intervals and drains supported events on the owning worker thread. */
  void poll_features(std::int64_t elapsed_ms);

private:
  friend class OneGen;
  explicit Camera(OneGen* owner);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace onegen
