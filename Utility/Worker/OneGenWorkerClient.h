#pragma once

#include "onegen/node.hpp"
#include "onegen/types.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace onegen::worker {

struct ProbeResult {
  bool completed = false;
  bool success = false;
  bool timed_out = false;
  std::vector<DeviceInfo> devices;
  std::string error;
};

/** Probe one CTI in a disposable worker. `timeout` is the GenTL list-update wait. */
ProbeResult probe(const std::filesystem::path& worker_executable,
                  const std::filesystem::path& cti,
                  std::chrono::milliseconds timeout);

class Connection {
public:
  using FrameFn = std::function<void(const Frame&)>;
  using ErrorFn = std::function<void(std::string_view)>;
  using InvalidateFn = std::function<void(int, std::string_view)>;

  Connection(std::filesystem::path worker_executable, std::filesystem::path cti);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  struct RpcResult {
    QJsonObject json;
    QByteArray binary;
  };
  RpcResult rpc(QJsonObject request,
                std::chrono::milliseconds timeout = std::chrono::seconds{30});
  void set_frame_callback(FrameFn cb);
  void set_error_callback(ErrorFn cb);
  void set_invalidate_callback(InvalidateFn cb);

private:
  void wake();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace onegen::worker
