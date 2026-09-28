#pragma once

#include "OneGenWorkerProtocol.h"
#include "OneGenTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace onegen::worker {

class Connection {
public:
  using FrameFn = std::function<void(const Frame&)>;
  using ErrorFn = std::function<void(std::string_view)>;
  using InvalidateFn = std::function<void(int, std::string_view)>;
  using ClaimFn = std::function<std::shared_ptr<void>(const InterfaceInfo& interface)>;

  /** Claims are owned in the consumer process and released when their worker exits. */
  Connection(std::filesystem::path worker_executable, std::filesystem::path cti, ClaimFn claim = {});
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /** Routes camera requests to a board channel; session zero retains the direct-camera path. */
  std::vector<std::byte> request(
      Operation operation, std::vector<std::byte> payload = {},
      std::chrono::milliseconds timeout = std::chrono::seconds{30},
      std::uint32_t session = 0);
  /** True while the worker process is still accepting requests. */
  bool alive() const;
  /** Stops this worker and releases its native resources before producer fallback. */
  void close();
  void set_frame_callback(FrameFn callback, std::uint32_t session = 0);
  void set_error_callback(ErrorFn callback, std::uint32_t session = 0);
  void set_invalidate_callback(InvalidateFn callback, std::uint32_t session = 0);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace onegen::worker
