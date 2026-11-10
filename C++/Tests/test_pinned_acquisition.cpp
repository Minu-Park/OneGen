#include "Internal/FramePayload.h"
#include "Internal/PinnedAcquisition.h"

#include <GenTL/GenTL.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t mono8 = 0x01080001;
constexpr std::uint64_t bayer_rg8 = 0x01080009;
constexpr std::uint64_t undefined_pfnc = 0x01100004;
constexpr std::uint64_t planar_abc32f = 0x026000c1;
constexpr std::uint8_t guard_byte = 0xA5;
constexpr std::uint8_t queued_byte = 0xDD;

std::atomic<std::size_t> g_largest_allocation{0};
std::atomic<bool> g_track_allocations{false};

void record_allocation(std::size_t size) {
  if (!g_track_allocations.load(std::memory_order_relaxed)) return;
  auto current = g_largest_allocation.load(std::memory_order_relaxed);
  while (size > current
         && !g_largest_allocation.compare_exchange_weak(current, size, std::memory_order_relaxed)) {
  }
}

struct FakeBuffer {
  std::vector<std::uint8_t> transport;
  std::size_t capacity = 0;
  std::size_t size_filled = 0;
  std::size_t data_size = 0;
  std::size_t image_offset = 0;
  std::size_t y_padding = 0;
  std::uint32_t delivered_height = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::size_t xpadding = 0;
  std::uint64_t pixel_format = 0;
  std::uint64_t payload_type = 0;
  std::uint64_t format_namespace = 0;
  std::uint32_t pixel_endianness = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t timestamp = 0;
  std::uint64_t timestamp_ns = 0;
  bool incomplete = false;
  bool image_present = false;
  bool contains_chunk_data = false;
  bool data_larger_than_buffer = false;
  bool null_base = false;
  struct Chunk {
    std::uint64_t id = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
  };
  struct Part {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t delivered_height = 0;
    std::uint64_t data_type = 0;
    std::uint64_t data_format = 0;
    std::uint64_t format_namespace = 0;
    std::uint64_t source_id = 0;
    std::uint64_t region_id = 0;
    std::uint64_t purpose_id = 0;
    std::size_t xpadding = 0;
  };
  std::vector<Chunk> chunks;
  std::vector<Part> parts;
};

struct FakeState {
  FakeBuffer buffer;
  GenTL::GC_ERROR queue_result = GenTL::GC_ERR_SUCCESS;
  int queue_count = 0;
  bool timeout = false;
  bool abort = false;
  bool null_handle = false;
  std::vector<std::string> log;
};

FakeState* g_state = nullptr;

void fill_transport(std::vector<std::uint8_t>& bytes) {
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::uint8_t>(index % 100U + 1U);
}

FakeBuffer make_buffer(std::uint32_t width, std::uint32_t height, std::size_t xpadding,
                       std::uint64_t pixel_format, std::size_t image_offset = 0) {
  FakeBuffer buffer;
  buffer.width = width;
  buffer.height = height;
  buffer.xpadding = xpadding;
  buffer.pixel_format = pixel_format;
  buffer.payload_type = GenTL::PAYLOAD_TYPE_IMAGE;
  buffer.image_offset = image_offset;
  buffer.image_present = true;
  buffer.frame_id = 1234;
  buffer.timestamp = 5678;
  buffer.timestamp_ns = 9012;
  const auto layout = onegen::coordinate_format(pixel_format);
  const auto bits = layout.planar ? layout.sample_bits
      : static_cast<std::size_t>((pixel_format >> 16U) & 0xffU);
  const auto row_bytes = (static_cast<std::size_t>(width) * bits + 7U) / 8U;
  const auto stride = row_bytes + xpadding;
  const auto planes = layout.planar ? layout.components : 1U;
  const auto image_bytes = stride * height * planes;
  buffer.size_filled = image_offset + image_bytes;
  buffer.capacity = buffer.size_filled;
  buffer.data_size = buffer.size_filled;
  buffer.transport.resize(buffer.size_filled);
  fill_transport(buffer.transport);
  return buffer;
}

GenTL::GC_ERROR give_value(GenTL::INFO_DATATYPE* type, void* value, size_t* size,
                           GenTL::INFO_DATATYPE out_type, const void* data, std::size_t bytes) {
  if (type) *type = out_type;
  if (value && size) {
    const auto copied = *size < bytes ? *size : bytes;
    std::memcpy(value, data, copied);
    *size = copied;
  }
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR GC_CALLTYPE fake_event_get_data(GenTL::EVENT_HANDLE, void* data, size_t* size,
                                                std::uint64_t) {
  auto& state = *g_state;
  state.log.push_back("event");
  (void)size;
  if (state.timeout) return GenTL::GC_ERR_TIMEOUT;
  if (state.abort) return GenTL::GC_ERR_ABORT;
  auto* event = static_cast<GenTL::EVENT_NEW_BUFFER_DATA*>(data);
  event->pUserPointer = nullptr;
  event->BufferHandle = state.null_handle
      ? nullptr : reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR GC_CALLTYPE fake_event_kill(GenTL::EVENT_HANDLE) {
  g_state->abort = true;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR GC_CALLTYPE fake_get_buffer_info(GenTL::DS_HANDLE, GenTL::BUFFER_HANDLE,
                                                 GenTL::BUFFER_INFO_CMD command,
                                                 GenTL::INFO_DATATYPE* type, void* value,
                                                 size_t* size) {
  const auto& buffer = g_state->buffer;
  const auto put_u64 = [&](std::uint64_t v, GenTL::INFO_DATATYPE out_type) {
    return give_value(type, value, size, out_type, &v, sizeof(v));
  };
  const auto put_size = [&](std::size_t v) {
    return give_value(type, value, size, GenTL::INFO_DATATYPE_SIZET, &v, sizeof(v));
  };
  const auto put_u32 = [&](std::uint32_t v) {
    return give_value(type, value, size, GenTL::INFO_DATATYPE_UINT32, &v, sizeof(v));
  };
  const auto put_bool = [&](bool v) {
    bool8_t raw = v;
    return give_value(type, value, size, GenTL::INFO_DATATYPE_BOOL8, &raw, sizeof(raw));
  };
  switch (command) {
    case GenTL::BUFFER_INFO_BASE: {
      const auto address = buffer.null_base
          ? std::uint64_t{0}
          : reinterpret_cast<std::uint64_t>(buffer.transport.data());
      return put_u64(address, GenTL::INFO_DATATYPE_PTR);
    }
    case GenTL::BUFFER_INFO_SIZE: return put_size(buffer.capacity);
    case GenTL::BUFFER_INFO_SIZE_FILLED: return put_size(buffer.size_filled);
    case GenTL::BUFFER_INFO_DATA_SIZE: return put_size(buffer.data_size);
    case GenTL::BUFFER_INFO_IMAGEOFFSET: return put_size(buffer.image_offset);
    case GenTL::BUFFER_INFO_YPADDING: return put_size(buffer.y_padding);
    case GenTL::BUFFER_INFO_DELIVERED_IMAGEHEIGHT: return put_size(buffer.delivered_height);
    case GenTL::BUFFER_INFO_IS_INCOMPLETE: return put_bool(buffer.incomplete);
    case GenTL::BUFFER_INFO_IMAGEPRESENT: return put_bool(buffer.image_present);
    case GenTL::BUFFER_INFO_CONTAINS_CHUNKDATA: return put_bool(buffer.contains_chunk_data);
    case GenTL::BUFFER_INFO_DATA_LARGER_THAN_BUFFER: return put_bool(buffer.data_larger_than_buffer);
    case GenTL::BUFFER_INFO_WIDTH: return put_u32(buffer.width);
    case GenTL::BUFFER_INFO_HEIGHT: return put_u32(buffer.height);
    case GenTL::BUFFER_INFO_PIXELFORMAT: return put_u64(buffer.pixel_format, GenTL::INFO_DATATYPE_UINT64);
    case GenTL::BUFFER_INFO_PAYLOADTYPE: return put_size(static_cast<std::size_t>(buffer.payload_type));
    case GenTL::BUFFER_INFO_PIXELFORMAT_NAMESPACE: return put_u64(buffer.format_namespace, GenTL::INFO_DATATYPE_UINT64);
    case GenTL::BUFFER_INFO_PIXEL_ENDIANNESS: return put_u32(buffer.pixel_endianness);
    case GenTL::BUFFER_INFO_XPADDING: return put_size(buffer.xpadding);
    case GenTL::BUFFER_INFO_FRAMEID: return put_u64(buffer.frame_id, GenTL::INFO_DATATYPE_UINT64);
    case GenTL::BUFFER_INFO_TIMESTAMP: return put_u64(buffer.timestamp, GenTL::INFO_DATATYPE_UINT64);
    case GenTL::BUFFER_INFO_TIMESTAMP_NS: return put_u64(buffer.timestamp_ns, GenTL::INFO_DATATYPE_UINT64);
    default: return GenTL::GC_ERR_NOT_IMPLEMENTED;
  }
}

GenTL::GC_ERROR GC_CALLTYPE fake_get_stream_info(GenTL::DS_HANDLE, GenTL::STREAM_INFO_CMD,
                                                 GenTL::INFO_DATATYPE*, void*, size_t*) {
  return GenTL::GC_ERR_ERROR;
}

GenTL::GC_ERROR GC_CALLTYPE fake_queue_buffer(GenTL::DS_HANDLE, GenTL::BUFFER_HANDLE) {
  auto& state = *g_state;
  ++state.queue_count;
  state.log.push_back("queue");
  if (!state.buffer.transport.empty())
    std::memset(state.buffer.transport.data(), queued_byte, state.buffer.transport.size());
  return state.queue_result;
}

GenTL::GC_ERROR GC_CALLTYPE fake_get_chunk_data(GenTL::DS_HANDLE, GenTL::BUFFER_HANDLE,
                                                GenTL::SINGLE_CHUNK_DATA* data, size_t* count) {
  const auto& chunks = g_state->buffer.chunks;
  if (!data) {
    *count = chunks.size();
    return chunks.empty() ? GenTL::GC_ERR_SUCCESS : GenTL::GC_ERR_BUFFER_TOO_SMALL;
  }
  const auto capacity = *count;
  const auto copied = capacity < chunks.size() ? capacity : chunks.size();
  for (std::size_t index = 0; index < copied; ++index) {
    data[index].ChunkID = chunks[index].id;
    data[index].ChunkOffset = static_cast<ptrdiff_t>(chunks[index].offset);
    data[index].ChunkLength = chunks[index].length;
  }
  *count = chunks.size();
  return copied == chunks.size() ? GenTL::GC_ERR_SUCCESS : GenTL::GC_ERR_BUFFER_TOO_SMALL;
}

GenTL::GC_ERROR GC_CALLTYPE fake_get_num_parts(GenTL::DS_HANDLE, GenTL::BUFFER_HANDLE,
                                               std::uint32_t* count) {
  *count = static_cast<std::uint32_t>(g_state->buffer.parts.size());
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR GC_CALLTYPE fake_get_part_info(GenTL::DS_HANDLE, GenTL::BUFFER_HANDLE,
                                               std::uint32_t index,
                                               GenTL::BUFFER_PART_INFO_CMD command,
                                               GenTL::INFO_DATATYPE* type, void* value,
                                               size_t* size) {
  const auto& parts = g_state->buffer.parts;
  if (index >= parts.size()) return GenTL::GC_ERR_INVALID_ID;
  const auto& part = parts[index];
  const auto put_u64 = [&](std::uint64_t v) {
    return give_value(type, value, size, GenTL::INFO_DATATYPE_UINT64, &v, sizeof(v));
  };
  const auto put_size = [&](std::size_t v) {
    return give_value(type, value, size, GenTL::INFO_DATATYPE_SIZET, &v, sizeof(v));
  };
  switch (command) {
    case GenTL::BUFFER_PART_INFO_BASE: {
      const auto address = reinterpret_cast<std::uint64_t>(
          g_state->buffer.transport.data() + part.offset);
      return give_value(type, value, size, GenTL::INFO_DATATYPE_PTR, &address, sizeof(address));
    }
    case GenTL::BUFFER_PART_INFO_DATA_SIZE: return put_size(part.size);
    case GenTL::BUFFER_PART_INFO_WIDTH: return put_size(part.width);
    case GenTL::BUFFER_PART_INFO_HEIGHT: return put_size(part.height);
    case GenTL::BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT: return put_size(part.delivered_height);
    case GenTL::BUFFER_PART_INFO_DATA_TYPE: return put_size(static_cast<std::size_t>(part.data_type));
    case GenTL::BUFFER_PART_INFO_DATA_FORMAT: return put_u64(part.data_format);
    case GenTL::BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE: return put_u64(part.format_namespace);
    case GenTL::BUFFER_PART_INFO_SOURCE_ID: return put_u64(part.source_id);
    case GenTL::BUFFER_PART_INFO_REGION_ID: return put_u64(part.region_id);
    case GenTL::BUFFER_PART_INFO_DATA_PURPOSE_ID: return put_u64(part.purpose_id);
    case GenTL::BUFFER_PART_INFO_XPADDING: return put_size(part.xpadding);
    default: return GenTL::GC_ERR_NOT_IMPLEMENTED;
  }
}

onegen::Gentl make_fake_gentl() {
  onegen::Gentl gentl;
  gentl.EventGetData = &fake_event_get_data;
  gentl.EventKill = &fake_event_kill;
  gentl.DSGetBufferInfo = &fake_get_buffer_info;
  gentl.DSGetInfo = &fake_get_stream_info;
  gentl.DSQueueBuffer = &fake_queue_buffer;
  gentl.DSGetBufferChunkData = &fake_get_chunk_data;
  gentl.DSGetNumBufferParts = &fake_get_num_parts;
  gentl.DSGetBufferPartInfo = &fake_get_part_info;
  return gentl;
}

struct Fixture {
  onegen::Gentl gentl = make_fake_gentl();
  onegen::NodeMap features;
  onegen::NodeMap stream_features;
  std::string stream_id = "stream-7";
  std::optional<onegen::Frame::Scan3dGeometry> scan3d_geometry;
  std::vector<onegen::Frame::ComponentIdentity> components;
  onegen::detail::AcquisitionContext context;
  FakeState state;

  Fixture() {
    g_state = &state;
    context.gentl = &gentl;
    context.stream = reinterpret_cast<GenTL::DS_HANDLE>(static_cast<std::uintptr_t>(0x10));
    context.new_buffer_event =
        reinterpret_cast<GenTL::EVENT_HANDLE>(static_cast<std::uintptr_t>(0x20));
    context.features = &features;
    context.stream_features = &stream_features;
    context.stream_index = 7;
    context.stream_id = &stream_id;
    context.scan3d_geometry = &scan3d_geometry;
    context.components = &components;
  }
  ~Fixture() { g_state = nullptr; }
};

void reset_fake(Fixture& fixture, FakeBuffer buffer) {
  fixture.state.buffer = std::move(buffer);
  fixture.state.queue_result = GenTL::GC_ERR_SUCCESS;
  fixture.state.queue_count = 0;
  fixture.state.timeout = false;
  fixture.state.abort = false;
  fixture.state.null_handle = false;
  fixture.state.log.clear();
}

bool same_buffer_info(const onegen::Frame::BufferInfo& left,
                      const onegen::Frame::BufferInfo& right) {
  return left.capacity == right.capacity && left.size_filled == right.size_filled
      && left.data_size == right.data_size && left.image_offset == right.image_offset
      && left.y_padding == right.y_padding
      && left.delivered_image_height == right.delivered_image_height
      && left.incomplete == right.incomplete && left.image_present == right.image_present
      && left.contains_chunk_data == right.contains_chunk_data
      && left.diagnostic == right.diagnostic;
}

bool same_double(double left, double right) {
  return left == right || (std::isnan(left) && std::isnan(right));
}

bool same_geometry(const std::optional<onegen::Frame::Scan3dGeometry>& left,
                   const std::optional<onegen::Frame::Scan3dGeometry>& right) {
  if (left.has_value() != right.has_value()) return false;
  if (!left) return true;
  const auto& a = *left;
  const auto& b = *right;
  return a.scale == b.scale && a.offset == b.offset && a.invalid_value == b.invalid_value
      && a.invalid_enabled == b.invalid_enabled && a.distance_unit == b.distance_unit
      && a.output_mode == b.output_mode && a.coordinate_system == b.coordinate_system
      && a.calibrated_axes == b.calibrated_axes
      && same_double(a.focal_length, b.focal_length) && same_double(a.baseline, b.baseline)
      && same_double(a.principal_u, b.principal_u) && same_double(a.principal_v, b.principal_v);
}

bool same_metadata(const onegen::Frame& left, const onegen::Frame& right) {
  if (left.width != right.width || left.height != right.height || left.stride != right.stride
      || left.payload_type != right.payload_type || left.pixel_format != right.pixel_format
      || left.format_namespace != right.format_namespace
      || left.pixel_endianness != right.pixel_endianness || left.frame_id != right.frame_id
      || left.timestamp != right.timestamp || left.timestamp_ns != right.timestamp_ns
      || left.stream_index != right.stream_index || left.stream_id != right.stream_id
      || !same_buffer_info(left.buffer_info, right.buffer_info)
      || !same_geometry(left.scan3d_geometry, right.scan3d_geometry)
      || left.components.size() != right.components.size())
    return false;
  for (std::size_t index = 0; index < left.components.size(); ++index)
    if (left.components[index].id != right.components[index].id
        || left.components[index].name != right.components[index].name)
      return false;
  return true;
}

bool contains_byte(const std::vector<std::uint8_t>& bytes, std::uint8_t value) {
  return std::any_of(bytes.begin(), bytes.end(), [value](std::uint8_t b) { return b == value; });
}

bool all_guard(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end) {
  return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(end),
                     [](std::uint8_t b) { return b == guard_byte; });
}

// T1
void owned_parity_mono8_bayer_padded() {
  for (const auto format : {mono8, bayer_rg8}) {
    Fixture fixture;
    auto input = make_buffer(4, 3, 2, format, 2);
    const auto transport = input.transport;
    reset_fake(fixture, std::move(input));
    fixture.scan3d_geometry.emplace();
    fixture.scan3d_geometry->scale = {0.5, 1.5, 2.5};
    fixture.scan3d_geometry->distance_unit = "mm";
    fixture.components = {{1, "Intensity"}, {65281, "Phase"}};

    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);
    const std::vector<std::uint8_t> expected_payload(transport.begin() + 2, transport.end());
    assert(owned.payload == expected_payload);
    assert(owned.parts.empty() && owned.chunks.empty());
    assert(owned.width == 4 && owned.height == 3 && owned.stride == 6);
    assert(owned.pixel_format == format);
    assert(owned.payload_type == GenTL::PAYLOAD_TYPE_IMAGE);
    assert(owned.format_namespace == 0 && owned.pixel_endianness == 0);
    assert(owned.frame_id == 1234 && owned.timestamp == 5678 && owned.timestamp_ns == 9012);
    assert(owned.buffer_info.capacity == transport.size());
    assert(owned.buffer_info.size_filled == transport.size());
    assert(owned.buffer_info.data_size == transport.size());
    assert(owned.buffer_info.image_offset == 2);
    assert(owned.buffer_info.y_padding == 0);
    assert(owned.buffer_info.delivered_image_height == 0);
    assert(!owned.buffer_info.incomplete && owned.buffer_info.image_present);
    assert(!owned.buffer_info.contains_chunk_data && owned.buffer_info.diagnostic.empty());
    assert(owned.stream_index == 7 && owned.stream_id == "stream-7");
    assert(owned.scan3d_geometry && owned.scan3d_geometry->distance_unit == "mm");
    assert((owned.scan3d_geometry->scale == std::array<double, 3>{0.5, 1.5, 2.5}));
    assert(owned.components.size() == 2 && owned.components[1].id == 65281);

    onegen::Frame reference = owned;
    reference.payload.clear();
    onegen::copy_single_payload(reference, transport.data(), transport.size());
    assert(reference.payload == owned.payload);
    assert(same_metadata(reference, owned));
  }
}

// T2
void owned_parity_parts_and_chunks() {
  {
    Fixture fixture;
    auto input = make_buffer(4, 3, 0, mono8);
    const auto transport = input.transport;
    input.parts.push_back({0, 6, 2, 3, 2, GenTL::PART_DATATYPE_2D_IMAGE, mono8, 0, 10, 20, 30, 0});
    input.parts.push_back({6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    reset_fake(fixture, std::move(input));
    fixture.components = {{9, "Depth"}};

    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);
    assert(owned.parts.size() == 1);
    assert(owned.payload.empty());
    assert(owned.chunks.empty());
    assert(owned.parts[0].payload
           == std::vector<std::uint8_t>(transport.begin(), transport.begin() + 6));
    assert(owned.parts[0].width == 2 && owned.parts[0].height == 2);
    assert(owned.parts[0].stride == 2);
    assert(owned.parts[0].data_type == GenTL::PART_DATATYPE_2D_IMAGE);
    assert(owned.parts[0].data_format == mono8);
    assert(owned.parts[0].source_id == 10 && owned.parts[0].region_id == 20
           && owned.parts[0].purpose_id == 30);
    assert(owned.components.size() == 1 && owned.components[0].name == "Depth");
  }
  {
    Fixture fixture;
    auto input = make_buffer(4, 3, 0, mono8);
    input.contains_chunk_data = true;
    input.chunks.push_back({17, 12, 2});
    input.chunks.push_back({18, 14, 1});
    input.size_filled = 15;
    input.capacity = 15;
    input.data_size = 15;
    input.transport.resize(15);
    fill_transport(input.transport);
    const auto transport = input.transport;
    reset_fake(fixture, std::move(input));

    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);
    assert(owned.parts.empty());
    assert(owned.chunks.size() == 2);
    assert(owned.chunks[0].id == 17 && owned.chunks[0].offset == 12);
    assert(owned.chunks[0].payload == std::vector<std::uint8_t>({transport[12], transport[13]}));
    assert(owned.chunks[1].id == 18 && owned.chunks[1].payload
           == std::vector<std::uint8_t>({transport[14]}));
    assert(owned.payload == std::vector<std::uint8_t>(transport.begin(), transport.begin() + 12));
    assert(owned.buffer_info.contains_chunk_data);
  }
}

// T3
void borrowed_matches_owned() {
  for (const auto format : {mono8, bayer_rg8}) {
    Fixture fixture;
    auto input = make_buffer(4, 3, 2, format, 2);
    reset_fake(fixture, input);
    fixture.scan3d_geometry.emplace();
    fixture.scan3d_geometry->scale = {1.0, 2.0, 3.0};
    fixture.components = {{7, "Intensity"}};
    const auto owned = onegen::detail::grab_owned(fixture.context, 100);

    reset_fake(fixture, input);
    std::vector<std::uint8_t> backing(owned.payload.size() + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, owned.payload.size());
    const auto borrowed =
        onegen::detail::grab_one_into(fixture.context, 100, {destination});
    assert(fixture.state.queue_count == 1);
    assert(borrowed.write.status == onegen::FrameWriteStatus::Written);
    assert(borrowed.write.used_bytes == owned.payload.size());
    assert(borrowed.write.required_bytes == owned.payload.size());
    assert(std::equal(destination.begin(), destination.end(), owned.payload.begin()));
    assert(same_metadata(owned, borrowed.metadata));
    assert(borrowed.metadata.payload.empty());
    assert(borrowed.metadata.parts.empty() && borrowed.metadata.chunks.empty());
    assert(all_guard(backing, 0, 16));
    assert(all_guard(backing, 16 + owned.payload.size(), backing.size()));
  }
}

// T4
void borrowed_copy_happens_while_pinned() {
  Fixture fixture;
  auto input = make_buffer(4, 3, 0, mono8);
  const auto required = input.size_filled;
  reset_fake(fixture, std::move(input));
  std::vector<std::uint8_t> backing(required + 32, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, required);

  onegen::detail::with_pinned_buffer(
      fixture.context, 100, onegen::detail::PinnedExtraction::BorrowedImageOnly,
      [&](onegen::detail::PinnedBuffer& buffer) {
        fixture.state.log.push_back("visit-begin");
        const onegen::BorrowedFrameView view(buffer.metadata(), buffer.transport());
        const auto written = onegen::write_frame(view, {destination});
        assert(written.status == onegen::FrameWriteStatus::Written);
        fixture.state.log.push_back("visit-end");
      });

  assert(fixture.state.queue_count == 1);
  const std::vector<std::string> expected{"event", "visit-begin", "visit-end", "queue"};
  assert(fixture.state.log == expected);
  assert(!contains_byte(backing, queued_byte));
  assert(all_guard(backing, 0, 16));
  assert(all_guard(backing, 16 + required, backing.size()));
}

// T5
void view_not_retained() {
  static_assert(!std::is_copy_constructible_v<onegen::BorrowedFrameView>);
  static_assert(!std::is_copy_assignable_v<onegen::BorrowedFrameView>);
  static_assert(!std::is_move_constructible_v<onegen::BorrowedFrameView>);
  static_assert(!std::is_move_assignable_v<onegen::BorrowedFrameView>);
  static_assert(!std::is_copy_constructible_v<onegen::detail::PinnedBuffer>);
  static_assert(!std::is_copy_assignable_v<onegen::detail::PinnedBuffer>);
  static_assert(!std::is_move_constructible_v<onegen::detail::PinnedBuffer>);
  static_assert(!std::is_move_assignable_v<onegen::detail::PinnedBuffer>);

  Fixture fixture;
  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  const std::uint8_t* stashed = nullptr;
  onegen::detail::with_pinned_buffer(
      fixture.context, 100, onegen::detail::PinnedExtraction::BorrowedImageOnly,
      [&](onegen::detail::PinnedBuffer& buffer) { stashed = buffer.transport().data(); });
  assert(stashed != nullptr);
  assert(*stashed == queued_byte);
}

// T6
void no_intermediate_image_allocation() {
  Fixture fixture;
  auto input = make_buffer(1024, 1024, 0, mono8);
  const auto image_bytes = input.transport.size();
  assert(image_bytes == 1024U * 1024U);
  reset_fake(fixture, std::move(input));
  std::vector<std::uint8_t> backing(image_bytes + 32, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, image_bytes);

  g_largest_allocation.store(0, std::memory_order_relaxed);
  g_track_allocations.store(true, std::memory_order_relaxed);
  onegen::detail::BorrowedGrabResult result;
  try {
    result = onegen::detail::grab_one_into(fixture.context, 100, {destination});
  } catch (...) {
    g_track_allocations.store(false, std::memory_order_relaxed);
    throw;
  }
  g_track_allocations.store(false, std::memory_order_relaxed);
  assert(result.write.status == onegen::FrameWriteStatus::Written);
  assert(result.write.used_bytes == image_bytes);
  assert(g_largest_allocation.load(std::memory_order_relaxed) < image_bytes);
}

// T7
void capacity_rejections() {
  Fixture fixture;
  auto input = make_buffer(4, 3, 0, mono8);
  const auto transport = input.transport;
  const std::size_t required = input.size_filled;
  const auto rejected = [&](std::size_t capacity, bool empty_span) {
    reset_fake(fixture, input);
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    const auto destination = empty_span
        ? std::span<std::uint8_t>{}
        : std::span<std::uint8_t>(backing).subspan(16, capacity);
    const auto result = onegen::detail::grab_one_into(fixture.context, 100, {destination});
    assert(result.write.status == onegen::FrameWriteStatus::InsufficientCapacity);
    assert(result.write.used_bytes == 0);
    assert(result.write.required_bytes == required);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
    assert(fixture.state.queue_count == 1);
    assert(result.metadata.payload.empty());
  };

  rejected(required - 1, false);
  rejected(0, false);
  rejected(0, true);

  reset_fake(fixture, input);
  std::vector<std::uint8_t> backing(required + 32, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, required + 16);
  const auto result = onegen::detail::grab_one_into(fixture.context, 100, {destination});
  assert(result.write.status == onegen::FrameWriteStatus::Written);
  assert(result.write.used_bytes == required && result.write.required_bytes == required);
  assert(std::equal(transport.begin(), transport.end(), destination.begin()));
  assert(all_guard(backing, 0, 16));
  assert(all_guard(backing, 16 + required, backing.size()));
  assert(fixture.state.queue_count == 1);
}

// T8
void malformed_rejections() {
  Fixture fixture;
  const auto rejected = [&](const FakeBuffer& input) {
    reset_fake(fixture, input);
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    bool threw = false;
    try {
      onegen::detail::grab_one_into(fixture.context, 100, {destination});
    } catch (const onegen::Error&) {
      threw = true;
    }
    assert(threw);
    assert(fixture.state.queue_count == 1);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };

  rejected(make_buffer(0, 3, 0, mono8));
  rejected(make_buffer(4, 0, 0, mono8));

  {
    FakeBuffer input;
    input.width = 1;
    input.height = 4;
    input.xpadding = (std::numeric_limits<std::size_t>::max)() - 4;
    input.pixel_format = mono8;
    input.payload_type = GenTL::PAYLOAD_TYPE_IMAGE;
    input.image_present = true;
    input.size_filled = 16;
    input.capacity = 16;
    input.data_size = 16;
    input.transport.assign(16, 1);
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.image_offset = input.size_filled + 1;
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.size_filled -= 1;
    input.data_size = input.size_filled;
    input.capacity = input.size_filled;
    input.transport.resize(input.size_filled);
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.capacity -= 1;
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.null_base = true;
    rejected(input);
  }
}

// T9
void unsupported_layouts() {
  Fixture fixture;
  const auto expect_unsupported = [&](FakeBuffer input, std::size_t owned_payload,
                                      std::size_t owned_parts, std::size_t owned_chunks) {
    const auto original = input;
    reset_fake(fixture, std::move(input));
    std::vector<std::uint8_t> backing(128, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 64);
    const auto borrowed = onegen::detail::grab_one_into(fixture.context, 100, {destination});
    assert(fixture.state.queue_count == 1);
    assert(borrowed.write.status == onegen::FrameWriteStatus::UnsupportedLayout);
    assert(borrowed.write.used_bytes == 0 && borrowed.write.required_bytes == 0);
    assert(borrowed.metadata.payload.empty());
    assert(borrowed.metadata.parts.empty() && borrowed.metadata.chunks.empty());
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));

    reset_fake(fixture, original);
    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);
    assert(owned.payload.size() == owned_payload);
    assert(owned.parts.size() == owned_parts);
    assert(owned.chunks.size() == owned_chunks);
  };

  expect_unsupported(make_buffer(4, 2, 0, undefined_pfnc), 16, 0, 0);
  expect_unsupported(make_buffer(2, 2, 0, planar_abc32f), 48, 0, 0);

  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.payload_type = GenTL::PAYLOAD_TYPE_JPEG;
    expect_unsupported(std::move(input), 8, 0, 0);
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.contains_chunk_data = true;
    input.chunks.push_back({17, 8, 2});
    input.size_filled = 10;
    input.capacity = 10;
    input.data_size = 10;
    input.transport.resize(10);
    fill_transport(input.transport);
    expect_unsupported(std::move(input), 8, 0, 1);
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.parts.push_back({0, 8, 4, 2, 2, GenTL::PART_DATATYPE_2D_IMAGE, mono8, 0, 3, 4, 5, 0});
    expect_unsupported(std::move(input), 0, 1, 0);
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.payload_type = GenTL::PAYLOAD_TYPE_CHUNK_ONLY;
    input.image_present = false;
    expect_unsupported(std::move(input), 8, 0, 0);
  }
}

// T10
void requeue_exactly_once_on_visitor_exception() {
  Fixture fixture;
  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  onegen::detail::with_pinned_buffer(
      fixture.context, 100, onegen::detail::PinnedExtraction::Owned,
      [](onegen::detail::PinnedBuffer&) {});
  assert(fixture.state.queue_count == 1);

  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  bool runtime_caught = false;
  try {
    onegen::detail::with_pinned_buffer(
        fixture.context, 100, onegen::detail::PinnedExtraction::Owned,
        [](onegen::detail::PinnedBuffer&) { throw std::runtime_error("boom"); });
  } catch (const std::runtime_error& error) {
    runtime_caught = std::string(error.what()) == "boom";
  }
  assert(runtime_caught);
  assert(fixture.state.queue_count == 1);

  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  int integer_caught = 0;
  try {
    onegen::detail::with_pinned_buffer(
        fixture.context, 100, onegen::detail::PinnedExtraction::Owned,
        [](onegen::detail::PinnedBuffer&) { throw 7; });
  } catch (int value) {
    integer_caught = value;
  }
  assert(integer_caught == 7);
  assert(fixture.state.queue_count == 1);
}

// T11
void requeue_failure_not_retried() {
  Fixture fixture;
  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  fixture.state.queue_result = GenTL::GC_ERR_ERROR;
  std::vector<std::uint8_t> backing(64, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
  bool threw = false;
  try {
    onegen::detail::grab_one_into(fixture.context, 100, {destination});
  } catch (const onegen::Error&) {
    threw = true;
  }
  assert(threw);
  assert(fixture.state.queue_count == 1);
}

// T12
void no_buffer_no_requeue() {
  Fixture fixture;
  const auto input = make_buffer(4, 2, 0, mono8);
  const auto expect_error = [&](const char* expected_text) {
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    bool threw = false;
    try {
      onegen::detail::grab_one_into(fixture.context, 100, {destination});
    } catch (const onegen::Error& error) {
      threw = true;
      if (expected_text) assert(std::string(error.what()) == expected_text);
    }
    assert(threw);
    assert(fixture.state.queue_count == 0);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };

  reset_fake(fixture, input);
  fixture.state.timeout = true;
  expect_error("grab timeout");

  reset_fake(fixture, input);
  fixture.state.abort = true;
  expect_error(nullptr);

  reset_fake(fixture, input);
  fixture.state.null_handle = true;
  expect_error(nullptr);
}

// T13
void stop_while_pinned() {
  Fixture fixture;
  reset_fake(fixture, make_buffer(4, 2, 0, mono8));
  bool stopped = false;
  onegen::detail::with_pinned_buffer(
      fixture.context, 100, onegen::detail::PinnedExtraction::Owned,
      [&](onegen::detail::PinnedBuffer&) {
        stopped = true;
        fixture.context.gentl->EventKill(fixture.context.new_buffer_event);
      });
  assert(stopped);
  assert(fixture.state.queue_count == 1);

  bool threw = false;
  try {
    onegen::detail::grab_owned(fixture.context, 100);
  } catch (const onegen::Error&) {
    threw = true;
  }
  assert(threw);
  assert(fixture.state.queue_count == 1);
}

// T14
void incomplete_buffer_parity() {
  Fixture fixture;
  auto input = make_buffer(4, 4, 0, mono8);
  input.capacity = 16;
  input.size_filled = 12;
  input.data_size = 12;
  input.transport.resize(12);
  fill_transport(input.transport);
  input.delivered_height = 3;
  input.incomplete = true;
  const auto transport = input.transport;

  reset_fake(fixture, input);
  const auto owned = onegen::detail::grab_owned(fixture.context, 100);
  assert(fixture.state.queue_count == 1);
  assert(owned.height == 3);
  assert(owned.buffer_info.incomplete);
  assert(!owned.buffer_info.diagnostic.empty());
  assert(owned.payload == transport);

  reset_fake(fixture, input);
  std::vector<std::uint8_t> backing(12 + 32, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, 12);
  const auto borrowed = onegen::detail::grab_one_into(fixture.context, 100, {destination});
  assert(fixture.state.queue_count == 1);
  assert(borrowed.write.status == onegen::FrameWriteStatus::Written);
  assert(borrowed.write.used_bytes == 3 * 4);
  assert(borrowed.write.required_bytes == 3 * 4);
  assert(borrowed.metadata.height == owned.height);
  assert(borrowed.metadata.buffer_info.incomplete);
  assert(borrowed.metadata.buffer_info.diagnostic == owned.buffer_info.diagnostic);
  assert(same_metadata(owned, borrowed.metadata));
  assert(std::equal(transport.begin(), transport.end(), destination.begin()));
}

// T15
onegen::Frame pure_frame(std::uint64_t format, std::uint32_t width, std::uint32_t height,
                         std::size_t stride, std::size_t offset) {
  onegen::Frame frame;
  frame.payload_type = GenTL::PAYLOAD_TYPE_IMAGE;
  frame.pixel_format = format;
  frame.width = width;
  frame.height = height;
  frame.stride = stride;
  frame.buffer_info.image_offset = offset;
  return frame;
}

void write_frame_pure() {
  std::vector<std::uint8_t> transport(64);
  fill_transport(transport);

  const auto written = [&](const onegen::Frame& metadata, std::size_t required) {
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required);
    const onegen::BorrowedFrameView view(metadata, transport);
    const auto result = onegen::write_frame(view, {destination});
    assert(result.status == onegen::FrameWriteStatus::Written);
    assert(result.used_bytes == required && result.required_bytes == required);
    assert(all_guard(backing, 0, 16));
    assert(all_guard(backing, 16 + required, backing.size()));
    for (std::size_t index = 0; index < required; ++index)
      assert(destination[index] == transport[metadata.buffer_info.image_offset + index]);
  };

  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 2);
    written(metadata, 8);
  }
  {
    auto metadata = pure_frame(bayer_rg8, 4, 4, 5, 1);
    written(metadata, 20);
  }

  const auto unsupported = [&](onegen::Frame metadata) {
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    const onegen::BorrowedFrameView view(metadata, transport);
    const auto result = onegen::write_frame(view, {destination});
    assert(result.status == onegen::FrameWriteStatus::UnsupportedLayout);
    assert(result.used_bytes == 0 && result.required_bytes == 0);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };
  unsupported(pure_frame(undefined_pfnc, 3, 2, 6, 0));
  unsupported(pure_frame(planar_abc32f, 2, 2, 8, 0));
  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    metadata.payload_type = GenTL::PAYLOAD_TYPE_JPEG;
    unsupported(metadata);
  }
  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    metadata.payload_type = GenTL::PAYLOAD_TYPE_CHUNK_ONLY;
    unsupported(metadata);
  }
  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    metadata.buffer_info.contains_chunk_data = true;
    unsupported(metadata);
  }
  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    metadata.chunks.push_back({1, 0, {}});
    unsupported(metadata);
  }
  {
    auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    metadata.parts.emplace_back();
    unsupported(metadata);
  }
  unsupported(pure_frame(mono8, 8, 1, 4, 0));

  const auto insufficient = [&](std::span<std::uint8_t> destination, std::size_t required,
                                std::vector<std::uint8_t>& backing) {
    const auto metadata = pure_frame(mono8, 3, 2, 4, 0);
    const onegen::BorrowedFrameView view(metadata, transport);
    const auto result = onegen::write_frame(view, {destination});
    assert(result.status == onegen::FrameWriteStatus::InsufficientCapacity);
    assert(result.used_bytes == 0 && result.required_bytes == required);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };
  {
    std::vector<std::uint8_t> backing(40, guard_byte);
    insufficient(std::span<std::uint8_t>(backing).subspan(16, 7), 8, backing);
  }
  {
    std::vector<std::uint8_t> backing(40, guard_byte);
    insufficient(std::span<std::uint8_t>(backing).subspan(16, 0), 8, backing);
  }
  {
    std::vector<std::uint8_t> backing(40, guard_byte);
    insufficient(std::span<std::uint8_t>{}, 8, backing);
  }

  const auto malformed = [&](onegen::Frame metadata) {
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    bool threw = false;
    try {
      const onegen::BorrowedFrameView view(metadata, transport);
      onegen::write_frame(view, {destination});
    } catch (const onegen::Error&) {
      threw = true;
    }
    assert(threw);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };
  malformed(pure_frame(mono8, 4, 0, 4, 0));
  malformed(pure_frame(mono8, 4, 2, 0, 0));
  malformed(pure_frame(mono8, 4, 3, (std::numeric_limits<std::size_t>::max)() / 2, 0));
  malformed(pure_frame(mono8, 4, 2, 4, transport.size() + 1));
  malformed(pure_frame(mono8, 8, 3, 32, 0));
}

void direct_malformed_message_stability() {
  std::vector<std::uint8_t> transport(64);
  fill_transport(transport);
  auto metadata = pure_frame(mono8, 4, 2, 4, transport.size() + 1);
  auto check = onegen::check_direct_image(metadata, transport);
  assert(check.verdict == onegen::LayoutVerdict::Malformed);
  const std::string expected = "grab: image offset exceeds delivered data";
  std::vector<std::string> churn;
  churn.reserve(512);
  for (int index = 0; index < 512; ++index)
    churn.emplace_back(expected.size(), static_cast<char>('A' + index % 26));
  assert(std::string_view(check.malformed_message) == expected);
}

bool same_frame(const onegen::Frame& left, const onegen::Frame& right) {
  if (!same_metadata(left, right) || left.payload != right.payload
      || left.parts.size() != right.parts.size() || left.chunks.size() != right.chunks.size())
    return false;
  for (std::size_t index = 0; index < left.parts.size(); ++index) {
    const auto& a = left.parts[index];
    const auto& b = right.parts[index];
    if (a.payload != b.payload || a.width != b.width || a.height != b.height
        || a.stride != b.stride || a.data_type != b.data_type || a.data_format != b.data_format
        || a.format_namespace != b.format_namespace || a.source_id != b.source_id
        || a.region_id != b.region_id || a.purpose_id != b.purpose_id)
      return false;
  }
  for (std::size_t index = 0; index < left.chunks.size(); ++index)
    if (left.chunks[index].id != right.chunks[index].id
        || left.chunks[index].offset != right.chunks[index].offset
        || left.chunks[index].payload != right.chunks[index].payload)
      return false;
  return true;
}

struct TestSink final : onegen::detail::DirectFrameSink {
  onegen::detail::ReserveStatus reserve_status = onegen::detail::ReserveStatus::Reserved;
  onegen::detail::FallbackReason gate = onegen::detail::FallbackReason::None;
  bool throw_publish = false;
  onegen::detail::DirectReservation reservation;
  std::vector<std::string> log;
  std::vector<std::string>* trace = nullptr;
  onegen::Frame published;
  std::size_t published_used = 0;
  onegen::Frame fallback;
  onegen::detail::FallbackReason fallback_reason = onegen::detail::FallbackReason::None;
  onegen::detail::CancelCause cancel_cause = onegen::detail::CancelCause::Exception;
  std::size_t cancel_count = 0;
  std::size_t slot_exhausted_count = 0;
  std::size_t report_count = 0;
  std::size_t reported_required = 0;
  std::size_t reported_available = 0;

  void mark(const char* value) {
    log.push_back(value);
    if (trace) trace->push_back(value);
  }

  onegen::detail::ReserveStatus reserve(onegen::detail::DirectReservation& out) override {
    mark("reserve");
    if (reserve_status != onegen::detail::ReserveStatus::Reserved) return reserve_status;
    out = reservation;
    return onegen::detail::ReserveStatus::Reserved;
  }
  void publish(const onegen::detail::DirectReservation&, const onegen::Frame& metadata,
               std::size_t used_bytes) override {
    mark("publish");
    published = metadata;
    published_used = used_bytes;
    if (throw_publish) throw std::runtime_error("publish");
  }
  void cancel(const onegen::detail::DirectReservation&,
              onegen::detail::CancelCause cause) noexcept override {
    mark("cancel");
    ++cancel_count;
    cancel_cause = cause;
  }
  void note_slot_exhausted() noexcept override {
    mark("slot_exhausted");
    ++slot_exhausted_count;
  }
  void note_fallback(onegen::detail::FallbackReason reason,
                     const onegen::Frame& frame) noexcept override {
    mark("note");
    fallback_reason = reason;
    fallback = frame;
  }
  onegen::detail::FallbackReason metadata_gate(const onegen::Frame&) const noexcept override {
    return gate;
  }
  void report_capacity_failure(const onegen::Frame&, std::size_t required,
                               std::size_t available) noexcept override {
    mark("capacity");
    ++report_count;
    reported_required = required;
    reported_available = available;
  }
  void wake() noexcept override { mark("wake"); }
};

// T16
void direct_matches_owned() {
  for (const auto format : {mono8, bayer_rg8}) {
    Fixture fixture;
    auto input = make_buffer(4, 3, 3, format, 16);
    const auto transport = input.transport;
    reset_fake(fixture, input);
    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);
    const auto required = owned.payload.size();
    assert(required == 21);

    reset_fake(fixture, input);
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required);
    const auto direct =
        onegen::detail::acquire_into_or_owned(fixture.context, 100, {destination});
    assert(fixture.state.queue_count == 1);
    assert(direct.status == onegen::detail::DirectStatus::SharedReady);
    assert(direct.reason == onegen::detail::FallbackReason::None);
    assert(direct.used_bytes == required && direct.required_bytes == required);
    assert(direct.frame.payload.empty());
    assert(same_metadata(owned, direct.frame));
    assert(std::equal(owned.payload.begin(), owned.payload.end(), destination.begin()));
    assert(all_guard(backing, 0, 16));
    assert(all_guard(backing, 16 + required, backing.size()));
    const std::vector<std::string> expected{"event", "queue"};
    assert(fixture.state.log == expected);
    (void)transport;
  }
}

// T17
void direct_fallbacks_match_owned() {
  struct Case {
    FakeBuffer input;
    onegen::detail::FallbackReason reason;
  };
  std::vector<Case> cases;
  cases.push_back({make_buffer(4, 2, 0, undefined_pfnc), onegen::detail::FallbackReason::UnknownLayout});
  cases.push_back(
      {make_buffer(2, 2, 0, planar_abc32f), onegen::detail::FallbackReason::Planar});
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.parts.push_back({0, 8, 4, 2, 2, GenTL::PART_DATATYPE_2D_IMAGE, mono8, 0, 3, 4, 5, 0});
    cases.push_back({std::move(input), onegen::detail::FallbackReason::MultiPart});
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.contains_chunk_data = true;
    input.chunks.push_back({17, 8, 2});
    input.size_filled = 10;
    input.capacity = 10;
    input.data_size = 10;
    input.transport.resize(10);
    fill_transport(input.transport);
    cases.push_back({std::move(input), onegen::detail::FallbackReason::ChunkData});
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.payload_type = GenTL::PAYLOAD_TYPE_CHUNK_ONLY;
    input.image_present = false;
    cases.push_back({std::move(input), onegen::detail::FallbackReason::NonImagePayload});
  }
  {
    auto input = make_buffer(4, 2, 0, mono8);
    input.payload_type = GenTL::PAYLOAD_TYPE_JPEG;
    cases.push_back({std::move(input), onegen::detail::FallbackReason::Jpeg});
  }
  {
    auto input = make_buffer(4, 4, 0, mono8);
    input.capacity = 16;
    input.size_filled = 12;
    input.data_size = 12;
    input.transport.resize(12);
    fill_transport(input.transport);
    input.delivered_height = 3;
    input.incomplete = true;
    cases.push_back({std::move(input), onegen::detail::FallbackReason::Incomplete});
  }

  Fixture fixture;
  for (const auto& test : cases) {
    reset_fake(fixture, test.input);
    const auto owned = onegen::detail::grab_owned(fixture.context, 100);
    assert(fixture.state.queue_count == 1);

    reset_fake(fixture, test.input);
    std::vector<std::uint8_t> backing(128, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 64);
    const auto direct =
        onegen::detail::acquire_into_or_owned(fixture.context, 100, {destination});
    assert(fixture.state.queue_count == 1);
    assert(direct.status == onegen::detail::DirectStatus::OwnedFallback);
    assert(direct.reason == test.reason);
    assert(direct.used_bytes == 0 && direct.required_bytes == 0);
    assert(same_frame(owned, direct.frame));
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  }
}

// T18
void direct_capacity_keeps_guard() {
  Fixture fixture;
  auto input = make_buffer(4, 3, 0, mono8);
  reset_fake(fixture, input);
  const auto owned = onegen::detail::grab_owned(fixture.context, 100);
  const auto required = owned.payload.size();
  assert(required == 12);

  reset_fake(fixture, input);
  std::vector<std::uint8_t> backing(64, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, required - 1);
  const auto direct = onegen::detail::acquire_into_or_owned(fixture.context, 100, {destination});
  assert(fixture.state.queue_count == 1);
  assert(direct.status == onegen::detail::DirectStatus::CapacityFailure);
  assert(direct.reason == onegen::detail::FallbackReason::None);
  assert(direct.used_bytes == 0 && direct.required_bytes == required);
  assert(direct.frame.payload.empty());
  assert(std::all_of(backing.begin(), backing.end(),
                     [](std::uint8_t b) { return b == guard_byte; }));
  const std::vector<std::string> expected{"event", "queue"};
  assert(fixture.state.log == expected);
}

// T19
void direct_malformed_rejects() {
  Fixture fixture;
  const auto rejected = [&](const FakeBuffer& input) {
    reset_fake(fixture, input);
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    bool threw = false;
    try {
      onegen::detail::acquire_into_or_owned(fixture.context, 100, {destination});
    } catch (const onegen::Error&) {
      threw = true;
    }
    assert(threw);
    assert(fixture.state.queue_count == 1);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  };

  rejected(make_buffer(0, 3, 0, mono8));
  rejected(make_buffer(4, 0, 0, mono8));
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.image_offset = input.size_filled + 1;
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.size_filled -= 1;
    input.data_size = input.size_filled;
    input.capacity = input.size_filled;
    input.transport.resize(input.size_filled);
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.capacity -= 1;
    rejected(input);
  }
  {
    auto input = make_buffer(4, 3, 0, mono8);
    input.null_base = true;
    rejected(input);
  }
}

// T20
void direct_run_step_order_and_cancel() {
  Fixture fixture;
  const auto input = make_buffer(4, 3, 0, mono8);
  reset_fake(fixture, input);
  const auto owned = onegen::detail::grab_owned(fixture.context, 100);
  const auto required = owned.payload.size();

  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required);
    sink.reservation.destination = destination;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::Published);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log == std::vector<std::string>{"reserve", "event", "queue", "publish"}));
    assert(sink.cancel_count == 0);
    assert(sink.published_used == required);
    assert(same_metadata(owned, sink.published));
    assert(std::equal(owned.payload.begin(), owned.payload.end(), destination.begin()));
    assert(all_guard(backing, 0, 16));
    assert(all_guard(backing, 16 + required, backing.size()));
  }

  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    sink.gate = onegen::detail::FallbackReason::MetadataTooLarge;
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required);
    sink.reservation.destination = destination;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::Fallback);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log
            == std::vector<std::string>{"reserve", "event", "queue", "cancel", "note"}));
    assert(sink.cancel_count == 1
           && sink.cancel_cause == onegen::detail::CancelCause::Fallback);
    assert(sink.fallback_reason == onegen::detail::FallbackReason::MetadataTooLarge);
    assert(same_frame(owned, fallback));
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  }

  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    std::vector<std::uint8_t> backing(64, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required - 1);
    sink.reservation.destination = destination;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::CapacityFailed);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log
            == std::vector<std::string>{"reserve", "event", "queue", "cancel", "capacity"}));
    assert(sink.cancel_count == 1
           && sink.cancel_cause == onegen::detail::CancelCause::Capacity);
    assert(sink.report_count == 1 && sink.reported_required == required
           && sink.reported_available == required - 1);
    assert(std::all_of(backing.begin(), backing.end(),
                       [](std::uint8_t b) { return b == guard_byte; }));
  }

  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    sink.reserve_status = onegen::detail::ReserveStatus::NoPool;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::NoPool);
    assert((fixture.state.log == std::vector<std::string>{"reserve"}));
    assert(fixture.state.queue_count == 0);
  }
  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    sink.reserve_status = onegen::detail::ReserveStatus::Stopped;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::Stopped);
    assert((fixture.state.log == std::vector<std::string>{"reserve"}));
    assert(fixture.state.queue_count == 0);
  }
  {
    // Busy consumes and requeues exactly one frame and counts only that drop.
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    sink.reserve_status = onegen::detail::ReserveStatus::Busy;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::Busy);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log
            == std::vector<std::string>{"reserve", "event", "queue", "slot_exhausted"}));
    assert(sink.slot_exhausted_count == 1);
    assert(sink.cancel_count == 0 && sink.published_used == 0);
    assert(std::all_of(fixture.state.buffer.transport.begin(),
                       fixture.state.buffer.transport.end(),
                       [](std::uint8_t b) { return b == queued_byte; }));
  }
  {
    // A Busy reserve with no frame available is a retry, not a drop.
    reset_fake(fixture, input);
    fixture.state.timeout = true;
    TestSink sink;
    sink.trace = &fixture.state.log;
    sink.reserve_status = onegen::detail::ReserveStatus::Busy;
    onegen::Frame fallback;
    const auto step = onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    assert(step == onegen::detail::DirectStep::Busy);
    assert(fixture.state.queue_count == 0);
    assert((fixture.state.log == std::vector<std::string>{"reserve", "event"}));
    assert(sink.slot_exhausted_count == 0);
  }

  {
    reset_fake(fixture, input);
    TestSink sink;
    sink.trace = &fixture.state.log;
    std::vector<std::uint8_t> backing(required + 32, guard_byte);
    auto destination = std::span<std::uint8_t>(backing).subspan(16, required);
    sink.reservation.destination = destination;
    sink.throw_publish = true;
    onegen::Frame fallback;
    bool threw = false;
    try {
      onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log == std::vector<std::string>{"reserve", "event", "queue", "publish"}));
    assert(sink.cancel_count == 0);
  }

  {
    reset_fake(fixture, make_buffer(4, 0, 0, mono8));
    TestSink sink;
    sink.trace = &fixture.state.log;
    std::vector<std::uint8_t> backing(64, guard_byte);
    sink.reservation.destination = std::span<std::uint8_t>(backing).subspan(16, 32);
    onegen::Frame fallback;
    bool threw = false;
    try {
      onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
    } catch (const onegen::Error&) {
      threw = true;
    }
    assert(threw);
    assert(fixture.state.queue_count == 1);
    assert((fixture.state.log == std::vector<std::string>{"reserve", "event", "queue", "cancel"}));
    assert(sink.cancel_count == 1
           && sink.cancel_cause == onegen::detail::CancelCause::Exception);
  }
}

// T21
void direct_no_intermediate_allocation() {
  Fixture fixture;
  auto input = make_buffer(1024, 1024, 0, mono8);
  const auto image_bytes = input.transport.size();
  reset_fake(fixture, std::move(input));
  std::vector<std::uint8_t> backing(image_bytes + 32, guard_byte);
  auto destination = std::span<std::uint8_t>(backing).subspan(16, image_bytes);

  g_largest_allocation.store(0, std::memory_order_relaxed);
  g_track_allocations.store(true, std::memory_order_relaxed);
  onegen::detail::DirectAcquisition result;
  try {
    result = onegen::detail::acquire_into_or_owned(fixture.context, 100, {destination});
  } catch (...) {
    g_track_allocations.store(false, std::memory_order_relaxed);
    throw;
  }
  g_track_allocations.store(false, std::memory_order_relaxed);
  assert(result.status == onegen::detail::DirectStatus::SharedReady);
  assert(result.used_bytes == image_bytes && result.required_bytes == image_bytes);
  assert(g_largest_allocation.load(std::memory_order_relaxed) < image_bytes);
  assert(fixture.state.queue_count == 1);
}

// T22
void direct_timeout_cancels_once() {
  Fixture fixture;
  reset_fake(fixture, make_buffer(4, 3, 0, mono8));
  fixture.state.timeout = true;
  TestSink sink;
  std::vector<std::uint8_t> backing(64, guard_byte);
  sink.reservation.destination = std::span<std::uint8_t>(backing).subspan(16, 32);
  onegen::Frame fallback;
  bool threw = false;
  try {
    onegen::detail::run_direct_step(fixture.context, 100, sink, fallback);
  } catch (const onegen::Error& error) {
    threw = std::string(error.what()) == "grab timeout";
  }
  assert(threw);
  assert(fixture.state.queue_count == 0);
  assert(sink.cancel_count == 1
         && sink.cancel_cause == onegen::detail::CancelCause::Timeout);
}

}  // namespace


void* operator new(std::size_t size) {
  record_allocation(size);
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  record_allocation(size);
  if (void* memory = std::malloc(size ? size : 1)) return memory;
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  record_allocation(size);
  return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  record_allocation(size);
  return std::malloc(size ? size : 1);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

int main() {
  requeue_exactly_once_on_visitor_exception();
  borrowed_copy_happens_while_pinned();
  owned_parity_mono8_bayer_padded();
  owned_parity_parts_and_chunks();
  borrowed_matches_owned();
  view_not_retained();
  no_intermediate_image_allocation();
  capacity_rejections();
  malformed_rejections();
  direct_malformed_message_stability();
  unsupported_layouts();
  requeue_failure_not_retried();
  no_buffer_no_requeue();
  stop_while_pinned();
  incomplete_buffer_parity();
  write_frame_pure();
  direct_matches_owned();
  direct_fallbacks_match_owned();
  direct_capacity_keeps_guard();
  direct_malformed_rejects();
  direct_run_step_order_and_cancel();
  direct_no_intermediate_allocation();
  direct_timeout_cancels_once();
  return 0;
}
