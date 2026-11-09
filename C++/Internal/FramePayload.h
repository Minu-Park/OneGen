#pragma once

#include "OneGenTypes.h"
#include "OneGenError.h"
#include "Internal/CoordinateFormat.h"
#include "Internal/DirectFrameSink.h"
#include "Internal/PixelLayout.h"
#include <GenTL/GenTL.h>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>

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

struct LayoutCheck;

/** Checked image region inside the original transport; only check_direct_image creates it. */
class CheckedImageSpan {
public:
  std::size_t offset() const noexcept { return offset_; }
  std::size_t bytes() const noexcept { return bytes_; }

private:
  friend struct LayoutCheck;
  friend LayoutCheck check_direct_image(const Frame&, std::span<const std::uint8_t>) noexcept;
  std::size_t offset_ = 0;
  std::size_t bytes_ = 0;
};

enum class LayoutVerdict : std::uint8_t { Eligible, Fallback, Malformed };

struct LayoutCheck {
  LayoutVerdict verdict = LayoutVerdict::Fallback;
  detail::FallbackReason reason = detail::FallbackReason::UnknownLayout;
  CheckedImageSpan span;
  /** Always points to static storage; safe after the caught exception is destroyed. */
  const char* malformed_message = "";
};

inline const char* stableMalformedMessage(std::string_view message) noexcept
{
  if (message == "grab: null payload storage") return "grab: null payload storage";
  if (message == "grab: image offset exceeds delivered data")
    return "grab: image offset exceeds delivered data";
  if (message == "grab: invalid single-image dimensions or stride")
    return "grab: invalid single-image dimensions or stride";
  if (message == "grab: planar image size exceeds the supported range")
    return "grab: planar image size exceeds the supported range";
  if (message == "grab: image exceeds delivered data") return "grab: image exceeds delivered data";
  if (message == "grab: chunk exceeds delivered data") return "grab: chunk exceeds delivered data";
  if (message == "grab: chunk overlaps image start") return "grab: chunk overlaps image start";
  if (message == "grab: image overlaps chunk metadata")
    return "grab: image overlaps chunk metadata";
  if (message == "grab: image chunk is shorter than its declared layout")
    return "grab: image chunk is shorter than its declared layout";
  if (message == "grab: compressed image chunk boundary is unavailable")
    return "grab: compressed image chunk boundary is unavailable";
  return "grab: image span validation failed";
}

/** Classifies one pin-bounded image without allocating or touching caller storage. */
inline LayoutCheck check_direct_image(const Frame& metadata,
                                      std::span<const std::uint8_t> transport) noexcept
{
  const auto layout = pixel_layout(metadata.pixel_format);
  const auto fallback = [](detail::FallbackReason reason) {
    LayoutCheck result;
    result.verdict = LayoutVerdict::Fallback;
    result.reason = reason;
    return result;
  };
  if (layout.packing == PixelPacking::Unknown) return fallback(detail::FallbackReason::UnknownLayout);
  if (layout.packing == PixelPacking::Planar) return fallback(detail::FallbackReason::Planar);
  if (layout.legacy_host_only) return fallback(detail::FallbackReason::UnknownLayout);
  if (!layout.host_shared) return fallback(detail::FallbackReason::HostUnsupported);
  if (layout.width_multiple > 1 && (metadata.width % layout.width_multiple))
    return fallback(detail::FallbackReason::LayoutConstraint);
  if (layout.packing == PixelPacking::PackedLsb) {
    if (metadata.width > (std::numeric_limits<std::uint64_t>::max)() / layout.bits_per_pixel)
      return fallback(detail::FallbackReason::GeometryUnrepresentable);
    if ((static_cast<std::uint64_t>(metadata.width) * layout.bits_per_pixel) % 8U)
      return fallback(detail::FallbackReason::PackedRowAlignment);
  }

  ImageSpan image;
  try {
    image = single_image_span(metadata, transport.data(), transport.size());
  } catch (const Error& error) {
    LayoutCheck result;
    result.verdict = LayoutVerdict::Malformed;
    result.malformed_message = stableMalformedMessage(error.what());
    return result;
  } catch (...) {
    LayoutCheck result;
    result.verdict = LayoutVerdict::Malformed;
    result.malformed_message = "grab: image span validation failed";
    return result;
  }

  const auto verdict = direct_representable(metadata.pixel_format, metadata.width, metadata.height,
                                            metadata.stride, metadata.stride * metadata.height);
  switch (verdict) {
    case RepresentVerdict::Ok: break;
    case RepresentVerdict::UnknownLayout:
      return fallback(detail::FallbackReason::UnknownLayout);
    case RepresentVerdict::Planar: return fallback(detail::FallbackReason::Planar);
    case RepresentVerdict::HostUnsupported:
      return fallback(detail::FallbackReason::HostUnsupported);
    case RepresentVerdict::GeometryUnrepresentable:
      return fallback(detail::FallbackReason::GeometryUnrepresentable);
    case RepresentVerdict::LayoutConstraint:
      return fallback(detail::FallbackReason::LayoutConstraint);
    case RepresentVerdict::PackedRowAlignment:
      return fallback(detail::FallbackReason::PackedRowAlignment);
  }
  if (image.end < image.offset || image.end - image.offset != metadata.stride * metadata.height) {
    LayoutCheck result;
    result.verdict = LayoutVerdict::Malformed;
    result.malformed_message = "grab: image extent does not match stride*height";
    return result;
  }
  LayoutCheck result;
  result.verdict = LayoutVerdict::Eligible;
  result.reason = detail::FallbackReason::None;
  result.span.offset_ = image.offset;
  result.span.bytes_ = image.end - image.offset;
  return result;
}

/** Copies the checked span after capacity validation; no layout work is repeated. */
inline FrameWriteResult write_checked(const CheckedImageSpan& span,
                                      std::span<const std::uint8_t> transport,
                                      FrameDestination destination) noexcept
{
  if (destination.bytes.size() < span.bytes())
    return {FrameWriteStatus::InsufficientCapacity, 0, span.bytes()};
  if (span.bytes())
    std::memcpy(destination.bytes.data(), transport.data() + span.offset(), span.bytes());
  return {FrameWriteStatus::Written, span.bytes(), span.bytes()};
}

/** Bounded write of the single image into caller storage; one memcpy after all checks. */
inline FrameWriteResult write_frame(const BorrowedFrameView& source, FrameDestination destination)
{
  const auto& metadata = source.metadata();
  if (metadata.payload_type != GenTL::PAYLOAD_TYPE_IMAGE || !metadata.parts.empty()
      || !metadata.chunks.empty() || metadata.buffer_info.contains_chunk_data)
    return {};
  const auto transport = source.transport();
  const auto check = check_direct_image(metadata, transport);
  if (check.verdict == LayoutVerdict::Malformed) throw Error(check.malformed_message);
  if (check.verdict != LayoutVerdict::Eligible) return {};
  return write_checked(check.span, transport, destination);
}

} // namespace onegen
