#pragma once

#include "OneGenTypes.h"
#include "OneGenError.h"
#include "Internal/CoordinateFormat.h"
#include <GenTL/GenTL.h>
#include <cstring>
#include <limits>
#include <span>

namespace onegen {

/** Span [offset, end) of the single image inside the delivered transport bytes. */
struct ImageSpan {
  std::size_t offset = 0;
  std::size_t end = 0;
};

/** Validates a single image exactly like copy_single_payload and returns its transport span. */
inline ImageSpan single_image_span(const Frame& frame, const std::uint8_t* base, std::size_t filled)
{
  if (filled && !base) throw Error("grab: null payload storage");
  const auto offset = frame.buffer_info.image_offset;
  if (offset > filled) throw Error("grab: image offset exceeds delivered data");
  auto end = filled;
  std::size_t bytes = 0;
  if (frame.payload_type != GenTL::PAYLOAD_TYPE_JPEG) {
    if (!frame.stride || !frame.height
        || frame.height > (std::numeric_limits<std::size_t>::max)() / frame.stride)
      throw Error("grab: invalid single-image dimensions or stride");
    bytes = frame.stride * frame.height;
    const auto layout = coordinate_format(frame.pixel_format);
    if (layout.planar) {
      if (bytes > (std::numeric_limits<std::size_t>::max)() / layout.components)
        throw Error("grab: planar image size exceeds the supported range");
      bytes *= layout.components;
    }
    if (bytes > filled - offset) throw Error("grab: image exceeds delivered data");
  }
  bool boundary_known = !frame.buffer_info.contains_chunk_data;
  for (const auto& chunk : frame.chunks) {
    if (chunk.offset > filled || chunk.payload.size() > filled - chunk.offset)
      throw Error("grab: chunk exceeds delivered data");
    const auto start = static_cast<std::size_t>(chunk.offset);
    const auto chunk_end = start + chunk.payload.size();
    if (start == offset) {
      // A legacy image chunk can describe the image bytes themselves.
      if (bytes && chunk.payload.size() < bytes)
        throw Error("grab: image chunk is shorter than its declared layout");
      if (!bytes && chunk_end < end) end = chunk_end;
      boundary_known = true;
    } else if (start < offset) {
      if (chunk_end > offset) throw Error("grab: chunk overlaps image start");
    } else {
      if (bytes && start < offset + bytes)
        throw Error("grab: image overlaps chunk metadata");
      if (start < end) end = start;
      boundary_known = true;
    }
  }
  if (bytes) end = offset + bytes;
  else if (!boundary_known)
    throw Error("grab: compressed image chunk boundary is unavailable");
  return {offset, end};
}

/** Copy a single image from its transport offset, retaining raw non-image payloads. */
inline void copy_single_payload(Frame& frame, const std::uint8_t* base, std::size_t filled)
{
  if (!filled) return;
  if (!base) throw Error("grab: null payload storage");
  const bool image = frame.payload_type == GenTL::PAYLOAD_TYPE_IMAGE
      || frame.payload_type == GenTL::PAYLOAD_TYPE_JPEG
      || (frame.payload_type == GenTL::PAYLOAD_TYPE_CHUNK_DATA && frame.buffer_info.image_present);
  if (!image) {
    frame.payload.assign(base, base + filled);
    return;
  }
  const auto span = single_image_span(frame, base, filled);
  frame.payload.assign(base + span.offset, base + span.end);
}

/** Caller-owned writable storage; ownership never moves into acquisition. */
struct FrameDestination {
  std::span<std::uint8_t> bytes;
};

/** Metadata by reference plus a read-only transport span; valid only inside the pinned visit. */
class BorrowedFrameView {
public:
  BorrowedFrameView(const Frame& metadata, std::span<const std::uint8_t> transport) noexcept
      : metadata_(&metadata), transport_(transport) {}
  BorrowedFrameView(const BorrowedFrameView&) = delete;
  BorrowedFrameView& operator=(const BorrowedFrameView&) = delete;
  BorrowedFrameView(BorrowedFrameView&&) = delete;
  BorrowedFrameView& operator=(BorrowedFrameView&&) = delete;

  const Frame& metadata() const noexcept { return *metadata_; }
  std::span<const std::uint8_t> transport() const noexcept { return transport_; }

private:
  const Frame* metadata_ = nullptr;
  std::span<const std::uint8_t> transport_;
};

enum class FrameWriteStatus : std::uint8_t { Written, InsufficientCapacity, UnsupportedLayout };

struct FrameWriteResult {
  FrameWriteStatus status = FrameWriteStatus::UnsupportedLayout;
  std::size_t used_bytes = 0;
  std::size_t required_bytes = 0;
};

/** Bounded write of the single image into caller storage; one memcpy after all checks. */
inline FrameWriteResult write_frame(const BorrowedFrameView& source, FrameDestination destination)
{
  const auto& metadata = source.metadata();
  const auto layout = coordinate_format(metadata.pixel_format);
  const bool borrowable = metadata.payload_type == GenTL::PAYLOAD_TYPE_IMAGE
      && metadata.parts.empty() && metadata.chunks.empty()
      && !metadata.buffer_info.contains_chunk_data
      && (metadata.pixel_format == 0x01080001 || metadata.pixel_format == 0x01080009)
      && !layout.planar;
  if (!borrowable) return {};
  const auto transport = source.transport();
  const auto span = single_image_span(metadata, transport.data(), transport.size());
  if (metadata.stride < metadata.width)
    throw Error("grab: stride is shorter than the image row");
  const auto required = metadata.stride * metadata.height;
  if (destination.bytes.size() < required)
    return {FrameWriteStatus::InsufficientCapacity, 0, required};
  std::memcpy(destination.bytes.data(), transport.data() + span.offset, required);
  return {FrameWriteStatus::Written, required, required};
}

} // namespace onegen
