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

  Connection(std::filesystem::path worker_executable, std::filesystem::path cti);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  std::vector<std::byte> request(
      Operation operation, std::vector<std::byte> payload = {},
      std::chrono::milliseconds timeout = std::chrono::seconds{30});
  void set_frame_callback(FrameFn callback);
  void set_error_callback(ErrorFn callback);
  void set_invalidate_callback(InvalidateFn callback);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace onegen::worker
