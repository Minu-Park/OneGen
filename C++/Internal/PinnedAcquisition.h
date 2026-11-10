#pragma once

#include "Internal/DirectFrameSink.h"
#include "Internal/FramePayload.h"
#include "Internal/Gentl.h"
#include "Internal/NodeMap.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace onegen {
class Camera;
}

namespace onegen::detail {

struct AcquisitionContext {
  Gentl* gentl = nullptr;
  GenTL::DS_HANDLE stream = nullptr;
  GenTL::EVENT_HANDLE new_buffer_event = nullptr;
  NodeMap* features = nullptr;
  NodeMap* stream_features = nullptr;
  std::uint32_t stream_index = 0;
  const std::string* stream_id = nullptr;
  const std::optional<Frame::Scan3dGeometry>* scan3d_geometry = nullptr;
  const std::vector<Frame::ComponentIdentity>* components = nullptr;
  /** Optional observer called once per dequeued buffer, before extraction. */
  void (*capture)(void* user, std::uint64_t frame_id) noexcept = nullptr;
  void* capture_user = nullptr;
};

enum class PinnedExtraction : std::uint8_t { Owned, BorrowedImageOnly, DirectOrOwned };

struct PinnedPart {
  const std::uint8_t* base = nullptr;
  std::size_t size = 0;
};

class PinnedBuffer;

using PinnedVisitFn = void (*)(void* user, PinnedBuffer& buffer);

/** Stack-only view of one pinned GenTL buffer; invalid after the visitor returns. */
class PinnedBuffer {
public:
  PinnedBuffer(const PinnedBuffer&) = delete;
  PinnedBuffer& operator=(const PinnedBuffer&) = delete;
  PinnedBuffer(PinnedBuffer&&) = delete;
  PinnedBuffer& operator=(PinnedBuffer&&) = delete;

  Frame& metadata() noexcept { return *metadata_; }
  const Frame& metadata() const noexcept { return *metadata_; }
  std::span<const std::uint8_t> transport() const noexcept { return transport_; }
  std::span<const PinnedPart> parts() const noexcept { return parts_; }
  bool borrowable() const noexcept { return borrowable_; }

private:
  friend void visit_pinned_buffer(const AcquisitionContext&, std::uint64_t,
                                  PinnedExtraction, PinnedVisitFn, void*);

  PinnedBuffer(Frame& metadata, std::span<const std::uint8_t> transport,
               std::span<const PinnedPart> parts, bool borrowable) noexcept
      : metadata_(&metadata), transport_(transport), parts_(parts), borrowable_(borrowable) {}

  Frame* metadata_ = nullptr;
  std::span<const std::uint8_t> transport_;
  std::span<const PinnedPart> parts_;
  bool borrowable_ = false;
};

/** EventGetData -> requeue guard -> describe -> visit -> explicit requeue. */
void visit_pinned_buffer(const AcquisitionContext& context, std::uint64_t timeout_ms,
                         PinnedExtraction extraction, PinnedVisitFn visit, void* user);

template <class F>
void with_pinned_buffer(const AcquisitionContext& context, std::uint64_t timeout_ms,
                        PinnedExtraction extraction, F&& visit)
{
  struct Thunk {
    static void call(void* user, PinnedBuffer& buffer) {
      (*static_cast<std::remove_reference_t<F>*>(user))(buffer);
    }
  };
  using Visitor = std::remove_cvref_t<F>;
  visit_pinned_buffer(context, timeout_ms, extraction, &Thunk::call,
                      const_cast<Visitor*>(std::addressof(visit)));
}

/** Owned path: identical to the previous Camera::Impl::grab_one. */
Frame grab_owned(const AcquisitionContext& context, std::uint64_t timeout_ms);

struct BorrowedGrabResult {
  FrameWriteResult write;
  Frame metadata;
};

BorrowedGrabResult grab_one_into(const AcquisitionContext& context, std::uint64_t timeout_ms,
                                 FrameDestination destination);

enum class DirectStatus : std::uint8_t { SharedReady, OwnedFallback, CapacityFailure };

struct DirectAcquisition {
  DirectStatus status = DirectStatus::OwnedFallback;
  FallbackReason reason = FallbackReason::None;
  std::size_t used_bytes = 0;
  std::size_t required_bytes = 0;
  Frame frame;
};

/** Direct pinned-to-destination acquisition without a worker sink. */
DirectAcquisition acquire_into_or_owned(const AcquisitionContext& context, std::uint64_t timeout_ms,
                                        FrameDestination destination);

enum class DirectStep : std::uint8_t { Published, Fallback, CapacityFailed, NoPool, Stopped, Busy };

/** Reserve -> pinned direct/owned conversion -> publish/cancel; fallback_out is valid for Fallback. */
DirectStep run_direct_step(const AcquisitionContext& context, std::uint64_t timeout_ms,
                           DirectFrameSink& sink, Frame& fallback_out);

/** Installs or clears the optional direct sink; only the live loop observes it. */
void set_direct_frame_sink(Camera& camera, std::shared_ptr<DirectFrameSink> sink);

}  // namespace onegen::detail
