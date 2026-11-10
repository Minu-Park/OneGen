#pragma once

#include "OneGenWorkerProtocol.h"
#include "OneGenTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace onegen::worker {

struct ReleaseMailbox;
struct PoolValidity;
struct PoolBudgetReservation;
class SharedFrameLease;

/** @brief Validated slot-backed frame; the lease keeps the host mapping and slot alive. */
struct SharedFrame {
  FrameDescriptor descriptor;
  std::shared_ptr<const SharedFrameLease> lease;
};

/** @brief Releases its exact slot token when the last consumer reference dies. */
class SharedFrameLease {
public:
  ~SharedFrameLease();
  SharedFrameLease(const SharedFrameLease&) = delete;
  SharedFrameLease& operator=(const SharedFrameLease&) = delete;
  [[nodiscard]] const FrameDescriptor& descriptor() const noexcept { return descriptor_; }
  /** @brief Complete owned metadata; pixel bytes remain in the shared slot. */
  [[nodiscard]] const Frame& metadata() const noexcept { return metadata_; }
  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return bytes_; }
  /** @brief Registers an in-flight callback; false once the pool was detached. */
  [[nodiscard]] bool beginDelivery() const noexcept;
  /** @brief Completes the in-flight callback registration for detach fencing. */
  void endDelivery() const noexcept;

private:
  friend class Connection;
  SharedFrameLease(std::shared_ptr<const void> region, std::span<const std::uint8_t> bytes,
                   SlotIdentity identity, Frame metadata, std::shared_ptr<ReleaseMailbox> mailbox,
                   std::shared_ptr<PoolValidity> validity,
                   std::shared_ptr<PoolBudgetReservation> reservation);
  std::shared_ptr<const void> region_;
  std::span<const std::uint8_t> bytes_;
  Frame metadata_;
  FrameDescriptor descriptor_;
  std::shared_ptr<ReleaseMailbox> mailbox_;
  std::shared_ptr<PoolValidity> validity_;
  std::shared_ptr<PoolBudgetReservation> reservation_;
};

/** @brief Isolates worker failures and reports C++ errors without escaping client IO threads. */
class Connection {
public:
  using FrameFn = std::function<void(const Frame&)>;
  using SharedFrameFn = std::function<void(const SharedFrame&)>;
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
  /** Marks the worker as retiring and waits briefly for active requests to finish. */
  bool beginRetirement(std::chrono::milliseconds timeout = std::chrono::seconds{2});
  /** True when initialization failed and the worker accepts only inactive replies and shutdown. */
  bool inactive() const;
  /** Stops this worker and releases its native resources before producer fallback. */
  void close();
  /** @brief Reports callback exceptions to diagnostics while retaining frame delivery. */
  void set_frame_callback(FrameFn callback, std::uint32_t session = 0);
  /** @brief Delivers validated shared-slot frames; the lease owns the exact release token. */
  void setSharedFrameCallback(SharedFrameFn callback, std::uint32_t session = 0);
  /** @brief Registers error delivery; callback exceptions fail the connection and wake requests. */
  void set_error_callback(ErrorFn callback, std::uint32_t session = 0);
  /** @brief Registers invalidation delivery with the same failure boundary as error callbacks. */
  void set_invalidate_callback(InvalidateFn callback, std::uint32_t session = 0);
  /** @brief Creates a host-owned pool under the per-session three-slot byte budget. */
  bool createFramePool(std::uint32_t session, std::uint32_t slotCount, std::uint64_t slotBytes,
                       std::string& error);
  /** @brief Detaches a pool; retained leases keep the mapping and budget charged. */
  void destroyFramePool(std::uint32_t session);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace onegen::worker
