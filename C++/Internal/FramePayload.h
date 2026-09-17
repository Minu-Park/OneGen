#pragma once

#include "OneGenTypes.h"
#include "OneGenError.h"
#include <GenTL/GenTL.h>
#include <limits>

namespace onegen {

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
  const auto offset = frame.buffer_info.image_offset;
  if (offset > filled) throw Error("grab: image offset exceeds delivered data");
  auto end = filled;
  std::size_t bytes = 0;
  if (frame.payload_type != GenTL::PAYLOAD_TYPE_JPEG) {
    if (!frame.stride || !frame.height
        || frame.height > (std::numeric_limits<std::size_t>::max)() / frame.stride)
      throw Error("grab: invalid single-image dimensions or stride");
    bytes = frame.stride * frame.height;
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
  frame.payload.assign(base + offset, base + end);
}

} // namespace onegen
