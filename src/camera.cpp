#include "onegen/camera.hpp"
#include "onegen/error.hpp"
#include "onegen/onegen.hpp"

#include "match.hpp"
#include "nodemap.hpp"
#include "onegen_impl.hpp"

#include <GenApi/GenApi.h>
#include <GenApi/ChunkAdapterGeneric.h>
#include <GenApi/Persistence.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace onegen {
namespace {

class BufferRequeue final {
public:
  BufferRequeue(Gentl* gentl, GenTL::DS_HANDLE stream, GenTL::BUFFER_HANDLE buffer)
      : gentl_(gentl), stream_(stream), buffer_(buffer) {}
  ~BufferRequeue() { if (gentl_ && buffer_) gentl_->DSQueueBuffer(stream_, buffer_); }
  void release() noexcept { buffer_ = nullptr; }

private:
  Gentl* gentl_ = nullptr;
  GenTL::DS_HANDLE stream_ = nullptr;
  GenTL::BUFFER_HANDLE buffer_ = nullptr;
};

std::uint64_t buffer_info(Gentl* g, GenTL::DS_HANDLE hds, GenTL::BUFFER_HANDLE b,
                          GenTL::BUFFER_INFO_CMD cmd) {
  unsigned char raw[16] = {};
  size_t n = sizeof(raw);
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  if (!g || g->DSGetBufferInfo(hds, b, cmd, &t, raw, &n) != GenTL::GC_ERR_SUCCESS) return 0;
  switch (t) {
    case GenTL::INFO_DATATYPE_BOOL8:
      return raw[0] != 0;
    case GenTL::INFO_DATATYPE_INT16:
    case GenTL::INFO_DATATYPE_UINT16: {
      std::uint16_t v = 0;
      std::memcpy(&v, raw, sizeof(v));
      return v;
    }
    case GenTL::INFO_DATATYPE_INT32:
    case GenTL::INFO_DATATYPE_UINT32: {
      std::uint32_t v = 0;
      std::memcpy(&v, raw, sizeof(v));
      return v;
    }
    case GenTL::INFO_DATATYPE_INT64:
    case GenTL::INFO_DATATYPE_UINT64:
    case GenTL::INFO_DATATYPE_SIZET:
    case GenTL::INFO_DATATYPE_PTR: {
      std::uint64_t v = 0;
      std::memcpy(&v, raw, n < 8 ? n : 8);
      return v;
    }
    default:
      return 0;
  }
}

void try_execute(NodeMap& map, const char* name) {
  auto node = map.find(name);
  if (!node.valid() || !node.is_writable()) return;
  try {
    node.execute();
  } catch (...) {
  }
}

void try_set_bool(NodeMap& map, const char* name, bool value) {
  auto node = map.find(name);
  if (!node.valid() || node.kind() != NodeKind::Boolean || !node.is_writable()) return;
  try {
    node.set_bool(value);
  } catch (...) {
  }
}

void try_set_int(NodeMap& map, const char* name, std::int64_t value) {
  auto node = map.find(name);
  if (!node.valid() || node.kind() != NodeKind::Integer || !node.is_writable()) return;
  try {
    const auto lo = node.int_min();
    const auto hi = node.int_max();
    if (hi >= lo) {
      if (value > hi) value = hi;
      if (value < lo) value = lo;
    }
    node.set_int(value);
  } catch (...) {
  }
}

std::uint64_t stream_info_u64(Gentl* g, GenTL::DS_HANDLE hds, GenTL::STREAM_INFO_CMD cmd) {
  unsigned char raw[16] = {};
  size_t n = sizeof(raw);
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  if (!g || g->DSGetInfo(hds, cmd, &type, raw, &n) != GenTL::GC_ERR_SUCCESS) return 0;
  std::uint64_t value = 0;
  std::memcpy(&value, raw, n < sizeof(value) ? n : sizeof(value));
  return value;
}

std::string readable_category(NodeMap& map, const char* name) {
  auto category = map.find(name);
  if (!category.valid() || category.kind() != NodeKind::Category) return {};
  std::string out;
  for (const auto& child : category.children()) {
    if (!child.is_readable()) continue;
    std::string text;
    try {
      text = child.value_text();
    } catch (...) {
      continue;
    }
    if (text.empty()) continue;
    if (!out.empty()) out += ", ";
    out += child.name() + '=' + text;
  }
  return out;
}

std::string incomplete_error(Gentl* g, GenTL::DS_HANDLE hds, const Frame::BufferInfo& info,
                             NodeMap& stream, std::uint64_t width, std::uint64_t height,
                             std::uint64_t pixel_format, std::uint64_t frame_id) {
  std::ostringstream out;
  out << "grab: producer marked buffer incomplete; PFNC 0x" << std::hex << pixel_format
      << std::dec << ", " << width << 'x' << height << ", frame " << frame_id
      << "; buffer{capacity=" << info.capacity << ", filled=" << info.size_filled
      << ", data=" << info.data_size << ", deliveredHeight=" << info.delivered_image_height
      << ", imageOffset=" << info.image_offset << ", yPadding=" << info.y_padding
      << ", imagePresent=" << info.image_present << ", chunk=" << info.contains_chunk_data
      << "}; stream{delivered=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_DELIVERED)
      << ", underrun=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_UNDERRUN)
      << ", announced=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_ANNOUNCED)
      << ", queued=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_QUEUED)
      << ", started=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_STARTED)
      << ", await=" << stream_info_u64(g, hds, GenTL::STREAM_INFO_NUM_AWAIT_DELIVERY) << '}';
  auto stats = readable_category(stream, "Statistic");
  if (stats.empty()) stats = readable_category(stream, "Statistics");
  if (!stats.empty()) out << "; " << stats;
  return out.str();
}

std::uint64_t part_info(Gentl* g, GenTL::DS_HANDLE hds, GenTL::BUFFER_HANDLE b,
                        std::uint32_t part, GenTL::BUFFER_PART_INFO_CMD cmd) {
  unsigned char raw[16] = {};
  size_t n = sizeof(raw);
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  if (!g || !g->DSGetBufferPartInfo ||
      g->DSGetBufferPartInfo(hds, b, part, cmd, &t, raw, &n) != GenTL::GC_ERR_SUCCESS)
    return 0;
  std::uint64_t value = 0;
  std::memcpy(&value, raw, n < sizeof(value) ? n : sizeof(value));
  return value;
}

std::optional<Frame::Scan3dGeometry> read_scan3d_geometry(
    NodeMap& features, const std::string_view prefix) {
  const auto named = [prefix](const char* suffix) { return std::string(prefix) + suffix; };
  auto selector = features.find(named("Scan3dCoordinateSelector"));
  auto scale = features.find(named("Scan3dCoordinateScale"));
  auto offset = features.find(named("Scan3dCoordinateOffset"));
  auto invalid_flag = features.find(named("Scan3dInvalidDataFlag"));
  auto invalid_value = features.find(named("Scan3dInvalidDataValue"));
  auto unit = features.find(named("Scan3dDistanceUnit"));
  if (!selector.valid() || !selector.is_readable() || !selector.is_writable()
      || !scale.valid() || !scale.is_readable() || !offset.valid() || !offset.is_readable()
      || !unit.valid() || !unit.is_readable())
    return std::nullopt;
  const auto previous = selector.get_enum();
  std::optional<Frame::Scan3dGeometry> result;
  try {
    Frame::Scan3dGeometry geometry;
    const char* coordinates[] = {"CoordinateA", "CoordinateB", "CoordinateC"};
    for (std::size_t index = 0; index < 3; ++index) {
      selector.set_enum(coordinates[index]);
      geometry.scale[index] = scale.get_float();
      geometry.offset[index] = offset.get_float();
      if (invalid_flag.valid() && invalid_flag.is_readable())
        geometry.invalid_enabled[index] = invalid_flag.kind() == NodeKind::Boolean
            ? invalid_flag.get_bool() : invalid_flag.get_int() != 0;
      if (geometry.invalid_enabled[index] && invalid_value.valid() && invalid_value.is_readable())
        geometry.invalid_value[index] = invalid_value.kind() == NodeKind::Float
            ? invalid_value.get_float() : static_cast<double>(invalid_value.get_int());
    }
    geometry.distance_unit = unit.kind() == NodeKind::Enumeration
        ? unit.get_enum() : unit.get_string();
    result = std::move(geometry);
  } catch (...) {
  }
  try {
    selector.set_enum(previous);
  } catch (...) {
    throw Error(named("Scan3dCoordinateSelector") + " could not be restored");
  }
  return result;
}

}  // namespace

struct Camera::Impl {
  OneGen* owner = nullptr;
  DeviceInfo info;
  bool opened = false;
  std::atomic<bool> live{false};
  std::atomic<bool> stop_live{false};
  std::thread live_thread;
  FrameFn on_frame;
  ErrorFn on_error;
  std::mutex callback_mu;

  Gentl* g = nullptr;
  GenTL::IF_HANDLE hif = nullptr;
  GenTL::DEV_HANDLE hdev = nullptr;
  GenTL::PORT_HANDLE hport = nullptr;
  GenTL::DS_HANDLE hds = nullptr;
  GenTL::EVENT_HANDLE hev = nullptr;
  size_t nbuf = 0;
  NodeMap features;
  NodeMap stream_features;
  NodeMap interface_features;
  NodeMap device_features;
  std::mutex acq_mu;
  std::string open_key;
  std::uint32_t stream_index = 0;
  std::string stream_id;
  std::size_t requested_buffer_count = 10;
  std::uint64_t single_timeout_ms = 10000;
  std::optional<Frame::Scan3dGeometry> scan3d_geometry;
  std::size_t buffer_capacity = 0;
  bool acquiring = false;

  void release_stream();
  void stop_acquisition();
  void prepare_stream();
  void start_stream(uint64_t n_frames);
  std::size_t payload_size();
  Frame grab_one(uint64_t timeout_ms);
  void cmd(const char* name);
  bool try_trigger();
  void set_locked(int64_t v);
  void snapshot_scan3d_geometry();
};

Camera::Camera(OneGen* owner) : impl_(std::make_unique<Impl>()) { impl_->owner = owner; }

Camera::~Camera() {
  try {
    close();
  } catch (...) {
  }
}

bool Camera::is_open() const { return impl_->opened; }
bool Camera::is_live() const { return impl_->live.load(); }
const DeviceInfo& Camera::info() const { return impl_->info; }
NodeMap& Camera::features() {
  return impl_->features.empty() ? impl_->device_features : impl_->features;
}
NodeMap& Camera::features(const Module module) {
  switch (module) {
    case Module::Interface: return impl_->interface_features;
    case Module::Device: return impl_->device_features;
    case Module::DataStream: return impl_->stream_features;
    case Module::RemoteDevice: return impl_->features;
  }
  return impl_->features;
}

std::vector<DataStreamInfo> Camera::data_streams() const {
  if (!impl_->opened || !impl_->g) throw Error("data_streams: camera not open");
  std::uint32_t count = 0;
  impl_->g->check(impl_->g->DevGetNumDataStreams(impl_->hdev, &count), "DevGetNumDataStreams");
  std::vector<DataStreamInfo> streams;
  streams.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    char id[256] = {};
    size_t size = sizeof(id);
    impl_->g->check(impl_->g->DevGetDataStreamID(impl_->hdev, index, id, &size),
                    "DevGetDataStreamID");
    streams.push_back({index, id});
  }
  return streams;
}

void Camera::select_data_stream(const std::uint32_t index) {
  if (impl_->live) throw Error("select_data_stream: acquisition is active");
  const auto streams = data_streams();
  if (index >= streams.size()) throw Error("select_data_stream: index out of range");
  if (index == impl_->stream_index) return;
  impl_->release_stream();
  impl_->stream_index = index;
  impl_->stream_id = streams[index].id;
}

std::uint32_t Camera::selected_data_stream() const { return impl_->stream_index; }

void Camera::set_buffer_count(const std::size_t count) {
  if (!count) throw Error("set_buffer_count: count must be positive");
  if (impl_->hds) throw Error("set_buffer_count: acquisition is active");
  impl_->requested_buffer_count = count;
}

std::size_t Camera::buffer_count() const { return impl_->requested_buffer_count; }

void Camera::set_single_timeout(const std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) throw Error("set_single_timeout: timeout must be positive");
  impl_->single_timeout_ms = static_cast<std::uint64_t>(timeout.count());
}

std::chrono::milliseconds Camera::single_timeout() const {
  return std::chrono::milliseconds(impl_->single_timeout_ms);
}

void Camera::save_features(const std::string_view path, const Module module) {
  if (impl_->hds) throw Error("save_features: acquisition is active");
  auto& map = features(module);
  auto* ref = static_cast<GenApi::CNodeMapRef*>(map.raw_nodemap());
  if (!ref || !ref->_Ptr) throw Error("save_features: node map is unavailable");
  std::ofstream output(std::string(path), std::ios::binary | std::ios::trunc);
  if (!output) throw Error("save_features: cannot open output file");
  GenApi::CFeatureBag bag;
  bag.StoreToBag(ref->_Ptr);
  output << bag;
  if (!output) throw Error("save_features: write failed");
}

void Camera::load_features(const std::string_view path, const Module module, const bool verify) {
  if (impl_->hds) throw Error("load_features: acquisition is active");
  auto& map = features(module);
  auto* ref = static_cast<GenApi::CNodeMapRef*>(map.raw_nodemap());
  if (!ref || !ref->_Ptr) throw Error("load_features: node map is unavailable");
  std::ifstream input(std::string(path), std::ios::binary);
  if (!input) throw Error("load_features: cannot open input file");
  GenApi::CFeatureBag bag;
  input >> bag;
  if (!input.eof() && input.fail()) throw Error("load_features: parse failed");
  GENICAM_NAMESPACE::gcstring_vector errors;
  if (!bag.LoadFromBag(ref->_Ptr, verify, &errors)) {
    const std::string detail = errors.empty() ? std::string{} : std::string(": ") + errors.front().c_str();
    throw Error("load_features: one or more features failed" + detail);
  }
}

Node Camera::feature(std::string_view name) {
  Node n = impl_->features.find(name);
  if (!n.valid()) n = impl_->stream_features.find(name);
  if (!n.valid()) n = impl_->device_features.find(name);
  if (!n.valid()) n = impl_->interface_features.find(name);
  if (!n.valid()) throw Error("feature not found: " + std::string(name));
  return n;
}

void Camera::set_frame_callback(FrameFn cb) {
  std::lock_guard<std::mutex> lk(impl_->callback_mu);
  impl_->on_frame = std::move(cb);
}
void Camera::set_error_callback(ErrorFn cb) {
  std::lock_guard<std::mutex> lk(impl_->callback_mu);
  impl_->on_error = std::move(cb);
}

void Camera::open(std::string_view query) {
  if (impl_->opened) throw Error("open: camera already open");
  auto* og = impl_->owner->impl_.get();
  std::lock_guard<std::recursive_mutex> lk(og->mu);
  if (og->producers.empty()) throw Error("open: no CTI loaded");

  // Probe left the GenTL interface open. Timeout 0 reuses that list; a closed
  // handle would force another full GigE wait and can miss the camera.
  auto found = scan_devices(og->producers, 0);
  if (found.empty()) found = scan_devices(og->producers, 10000);
  std::vector<DeviceInfo> infos;
  infos.reserve(found.size());
  for (auto& f : found) infos.push_back(f.info);
  const DeviceInfo& match = match_device(infos, query);

  FoundDevice* hit = nullptr;
  for (auto& f : found)
    if (f.info.id == match.id && f.info.cti_path == match.cti_path) hit = &f;
  if (!hit) throw Error("open: match lost");

  const std::string key = hit->info.cti_path + "|" + hit->info.id;
  if (og->open_keys.count(key)) throw Error("open: device already open: " + hit->info.id);

  auto& g = hit->producer->api;
  const auto if_it = hit->producer->interfaces.find(hit->if_id);
  if (if_it == hit->producer->interfaces.end() || !if_it->second)
    throw Error("open: interface is not open");
  GenTL::IF_HANDLE hif = if_it->second;

  GenTL::DEVICE_ACCESS_FLAGS modes[] = {GenTL::DEVICE_ACCESS_EXCLUSIVE,
                                        GenTL::DEVICE_ACCESS_CONTROL};
  GenTL::DEV_HANDLE hdev = nullptr;
  GenTL::GC_ERROR err = GenTL::GC_ERR_ERROR;
  for (auto m : modes) {
    err = g.IFOpenDevice(hif, hit->info.id.c_str(), m, &hdev);
    if (err == GenTL::GC_ERR_SUCCESS) break;
  }
  if (err != GenTL::GC_ERR_SUCCESS) {
    bool8_t ch = 0;
    g.check(g.IFUpdateDeviceList(hif, &ch, 10000), "IFUpdateDeviceList");
    for (auto m : modes) {
      err = g.IFOpenDevice(hif, hit->info.id.c_str(), m, &hdev);
      if (err == GenTL::GC_ERR_SUCCESS) break;
    }
  }
  if (err != GenTL::GC_ERR_SUCCESS) g.fail("IFOpenDevice");

  try {
    try { load_remote_nodemap(impl_->interface_features, g, hif); } catch (...) {}
    try { load_remote_nodemap(impl_->device_features, g, hdev); } catch (...) {}
    GenTL::PORT_HANDLE hport = nullptr;
    if (g.DevGetPort(hdev, &hport) == GenTL::GC_ERR_SUCCESS) {
      try {
        load_remote_nodemap(impl_->features, g, hport);
        impl_->hport = hport;
      } catch (...) {
        if (impl_->device_features.empty()) throw;
      }
    } else if (impl_->device_features.empty()) {
      throw Error("open: neither device nor remote-device node map is available");
    }
  } catch (...) {
    impl_->features.detach();
    impl_->device_features.detach();
    impl_->interface_features.detach();
    g.DevClose(hdev);
    throw;
  }

  impl_->g = &g;
  impl_->hif = hif;
  impl_->hdev = hdev;
  impl_->info = hit->info;
  impl_->open_key = key;
  impl_->opened = true;
  og->open_keys.insert(key);
}

void Camera::Impl::cmd(const char* name) {
  auto n = features.find(name);
  if (!n.valid()) return;
  try {
    n.execute();
  } catch (...) {
  }
}

bool Camera::Impl::try_trigger() {
  auto trigger = features.find("TriggerSoftware");
  if (!trigger.valid() || !trigger.is_writable()) return false;
  trigger.execute();
  return true;
}

void Camera::Impl::set_locked(int64_t v) {
  auto n = features.find("TLParamsLocked");
  if (n.valid() && n.is_writable()) n.set_int(v);
}

void Camera::Impl::snapshot_scan3d_geometry() {
  scan3d_geometry = read_scan3d_geometry(features, "");
}

void Camera::Impl::stop_acquisition() {
  stop_live = true;
  if (live_thread.joinable() && g && hev) g->EventKill(hev);
  if (live_thread.joinable() && live_thread.get_id() != std::this_thread::get_id())
    live_thread.join();
  live = false;

  if (!g || !hds || !acquiring) return;
  try {
    cmd("AcquisitionStop");
  } catch (...) {
  }
  g->DSStopAcquisition(hds, GenTL::ACQ_STOP_FLAGS_DEFAULT);
  g->DSFlushQueue(hds, GenTL::ACQ_QUEUE_ALL_TO_INPUT);
  if (hev) {
    g->GCUnregisterEvent(hds, GenTL::EVENT_NEW_BUFFER);
    hev = nullptr;
  }
  acquiring = false;
  try {
    set_locked(0);
  } catch (...) {
  }
}

void Camera::Impl::release_stream() {
  stop_acquisition();
  if (!g || !hds) return;
  stream_features.detach();
  if (hev) {
    g->GCUnregisterEvent(hds, GenTL::EVENT_NEW_BUFFER);
    hev = nullptr;
  }
  g->DSFlushQueue(hds, GenTL::ACQ_QUEUE_ALL_DISCARD);
  for (size_t i = 0; i < nbuf; ++i) {
    GenTL::BUFFER_HANDLE b = nullptr;
    if (g->DSGetBufferID(hds, 0, &b) == GenTL::GC_ERR_SUCCESS)
      g->DSRevokeBuffer(hds, b, nullptr, nullptr);
  }
  nbuf = 0;
  buffer_capacity = 0;
  g->DSClose(hds);
  hds = nullptr;
}

void Camera::close() {
  stop();
  if (!impl_->opened) return;
  auto* og = impl_->owner->impl_.get();
  std::lock_guard<std::recursive_mutex> lk(og->mu);
  impl_->release_stream();
  impl_->features.detach();
  impl_->stream_features.detach();
  impl_->device_features.detach();
  impl_->interface_features.detach();
  if (impl_->g && impl_->hdev) impl_->g->DevClose(impl_->hdev);
  og->open_keys.erase(impl_->open_key);
  impl_->hdev = nullptr;
  impl_->hif = nullptr;
  impl_->hport = nullptr;
  impl_->g = nullptr;
  impl_->opened = false;
  impl_->info = {};
  impl_->stream_index = 0;
  impl_->stream_id.clear();
  impl_->open_key.clear();
}

void Camera::Impl::prepare_stream() {
  if (!opened || !g) throw Error("stream: camera not open");
  if (hds) return;

  uint32_t ns = 0;
  g->check(g->DevGetNumDataStreams(hdev, &ns), "DevGetNumDataStreams");
  if (ns == 0) throw Error("no data streams");
  char dsid[256] = {};
  size_t dsn = sizeof(dsid);
  if (stream_index >= ns) throw Error("selected data stream is unavailable");
  g->check(g->DevGetDataStreamID(hdev, stream_index, dsid, &dsn), "DevGetDataStreamID");
  stream_id = dsid;
  g->check(g->DevOpenDataStream(hdev, dsid, &hds), "DevOpenDataStream");
  try { load_remote_nodemap(stream_features, *g, hds); } catch (...) {}
  try_set_bool(stream_features, "AutoPacketSize", true);
  try_execute(stream_features, "ProbePacketSize");
  try_set_bool(stream_features, "EnableResend", true);
  try_set_int(stream_features, "MaxNumBuffer",
              static_cast<std::int64_t>(requested_buffer_count));

  snapshot_scan3d_geometry();

  const auto payload = payload_size();
  buffer_capacity = payload;

  size_t min_buf = requested_buffer_count;
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  size_t sz = sizeof(min_buf);
  if (g->DSGetInfo(hds, GenTL::STREAM_INFO_BUF_ANNOUNCE_MIN, &t, &min_buf, &sz) !=
      GenTL::GC_ERR_SUCCESS)
    min_buf = requested_buffer_count;
  if (min_buf < requested_buffer_count) min_buf = requested_buffer_count;
  auto max_buf = stream_features.find("MaxNumBuffer");
  if (max_buf.valid() && max_buf.is_readable()) {
    try {
      const auto cap = static_cast<std::size_t>(max_buf.get_int());
      if (cap && cap < min_buf) min_buf = cap;
    } catch (...) {
    }
  }

  nbuf = min_buf;
  for (size_t i = 0; i < nbuf; ++i) {
    GenTL::BUFFER_HANDLE b = nullptr;
    g->check(g->DSAllocAndAnnounceBuffer(hds, payload, nullptr, &b), "DSAllocAndAnnounceBuffer");
    g->check(g->DSQueueBuffer(hds, b), "DSQueueBuffer");
  }

}

std::size_t Camera::Impl::payload_size() {
  size_t payload = 0;
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  size_t sz = sizeof(payload);
  bool8_t defines = 0;
  size_t bsz = sizeof(defines);
  if (g->DSGetInfo(hds, GenTL::STREAM_INFO_DEFINES_PAYLOADSIZE, &t, &defines, &bsz) ==
          GenTL::GC_ERR_SUCCESS &&
      defines) {
    g->DSGetInfo(hds, GenTL::STREAM_INFO_PAYLOAD_SIZE, &t, &payload, &sz);
  }
  if (payload == 0) {
    auto p = features.find("PayloadSize");
    if (p.valid() && p.is_readable()) payload = static_cast<size_t>(p.get_int());
  }
  if (payload == 0) throw Error("stream payload size is unavailable");
  return payload;
}

void Camera::Impl::start_stream(uint64_t n_frames) {
  prepare_stream();
  if (acquiring) stop_acquisition();
  if (payload_size() > buffer_capacity) {
    release_stream();
    prepare_stream();
  }
  snapshot_scan3d_geometry();
  set_locked(1);
  try {
    g->check(g->GCRegisterEvent(hds, GenTL::EVENT_NEW_BUFFER, &hev), "GCRegisterEvent");
    g->check(g->DSStartAcquisition(hds, GenTL::ACQ_START_FLAGS_DEFAULT, n_frames),
             "DSStartAcquisition");
    acquiring = true;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(single_timeout_ms);
    while (true) {
      bool8_t grabbing = 0;
      GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
      size_t size = sizeof(grabbing);
      if (g->DSGetInfo(hds, GenTL::STREAM_INFO_IS_GRABBING, &type, &grabbing, &size)
          != GenTL::GC_ERR_SUCCESS || grabbing)
        break;
      if (std::chrono::steady_clock::now() >= deadline)
        throw Error("DSStartAcquisition: stream did not enter grabbing state");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto acquisition_start = features.find("AcquisitionStart");
    if (acquisition_start.valid()) acquisition_start.execute();
  } catch (...) {
    stop_acquisition();
    if (hev) {
      g->GCUnregisterEvent(hds, GenTL::EVENT_NEW_BUFFER);
      hev = nullptr;
    }
    try {
      set_locked(0);
    } catch (...) {
    }
    throw;
  }
}

Frame Camera::Impl::grab_one(uint64_t timeout_ms) {
  GenTL::EVENT_NEW_BUFFER_DATA data{};
  size_t size = sizeof(data);
  auto err = g->EventGetData(hev, &data, &size, timeout_ms);
  if (err == GenTL::GC_ERR_TIMEOUT) throw Error("grab timeout");
  if (err != GenTL::GC_ERR_SUCCESS) g->fail("EventGetData");
  if (!data.BufferHandle) throw Error("grab: null buffer handle");
  BufferRequeue requeue(g, hds, data.BufferHandle);

  Frame f;
  auto u64 = [&](GenTL::BUFFER_INFO_CMD cmd) {
    return buffer_info(g, hds, data.BufferHandle, cmd);
  };
  f.buffer_info.capacity = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_SIZE));
  f.buffer_info.size_filled = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_SIZE_FILLED));
  f.buffer_info.data_size = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_DATA_SIZE));
  f.buffer_info.image_offset = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_IMAGEOFFSET));
  f.buffer_info.y_padding = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_YPADDING));
  f.buffer_info.delivered_image_height = static_cast<std::uint32_t>(
      u64(GenTL::BUFFER_INFO_DELIVERED_IMAGEHEIGHT));
  f.buffer_info.incomplete = u64(GenTL::BUFFER_INFO_IS_INCOMPLETE) != 0;
  f.buffer_info.image_present = u64(GenTL::BUFFER_INFO_IMAGEPRESENT) != 0;
  f.buffer_info.contains_chunk_data = u64(GenTL::BUFFER_INFO_CONTAINS_CHUNKDATA) != 0;

  auto* base = reinterpret_cast<const std::uint8_t*>(static_cast<uintptr_t>(u64(GenTL::BUFFER_INFO_BASE)));
  const auto capacity = f.buffer_info.capacity;
  size_t filled = f.buffer_info.size_filled;
  if (filled == 0) filled = f.buffer_info.data_size;
  if (filled == 0) filled = capacity;
  if ((!base && filled) || filled > capacity
      || u64(GenTL::BUFFER_INFO_DATA_LARGER_THAN_BUFFER))
    throw Error("grab: buffer payload exceeds announced storage");
  f.width = static_cast<std::uint32_t>(u64(GenTL::BUFFER_INFO_WIDTH));
  f.height = static_cast<std::uint32_t>(u64(GenTL::BUFFER_INFO_HEIGHT));
  const auto delivered_height = f.buffer_info.delivered_image_height;
  if (delivered_height) f.height = delivered_height;
  f.pixel_format = u64(GenTL::BUFFER_INFO_PIXELFORMAT);
  f.payload_type = u64(GenTL::BUFFER_INFO_PAYLOADTYPE);
  f.format_namespace = u64(GenTL::BUFFER_INFO_PIXELFORMAT_NAMESPACE);
  f.pixel_endianness = static_cast<std::uint32_t>(u64(GenTL::BUFFER_INFO_PIXEL_ENDIANNESS));
  const auto integer_feature = [&](const char* name) -> std::uint64_t {
    auto node = features.find(name);
    if (!node.valid() || !node.is_readable()) return 0;
    try {
      return static_cast<std::uint64_t>(node.get_int());
    } catch (...) {
      return 0;
    }
  };
  if (!f.width) f.width = static_cast<std::uint32_t>(integer_feature("Width"));
  if (!f.height) f.height = static_cast<std::uint32_t>(integer_feature("Height"));
  if (!f.pixel_format) {
    auto node = features.find("PixelFormat");
    if (node.valid() && node.is_readable()) {
      try {
        f.pixel_format = static_cast<std::uint64_t>(node.get_enum_value());
      } catch (...) {
      }
    }
  }
  const auto bits_per_pixel = static_cast<std::size_t>((f.pixel_format >> 16U) & 0xffU);
  const auto padding = static_cast<std::size_t>(u64(GenTL::BUFFER_INFO_XPADDING));
  if (bits_per_pixel
      && static_cast<std::size_t>(f.width)
          <= ((std::numeric_limits<std::size_t>::max)() - 7U) / bits_per_pixel) {
    const auto row_bytes = (static_cast<std::size_t>(f.width) * bits_per_pixel + 7U) / 8U;
    if (padding <= (std::numeric_limits<std::size_t>::max)() - row_bytes)
      f.stride = row_bytes + padding;
  }
  if (f.payload_type == GenTL::PAYLOAD_TYPE_JPEG && !f.stride) f.stride = filled;
  if (f.buffer_info.incomplete && f.stride && filled > f.buffer_info.image_offset) {
    const auto usable = filled - f.buffer_info.image_offset;
    const auto rows = usable / f.stride;
    if (rows && rows < f.height) f.height = static_cast<std::uint32_t>(rows);
  }
  if (f.buffer_info.incomplete) {
    f.buffer_info.diagnostic = incomplete_error(g, hds, f.buffer_info, stream_features, f.width,
                                                f.height, f.pixel_format,
                                                u64(GenTL::BUFFER_INFO_FRAMEID));
  }
  f.frame_id = u64(GenTL::BUFFER_INFO_FRAMEID);
  f.timestamp = u64(GenTL::BUFFER_INFO_TIMESTAMP);
  f.timestamp_ns = u64(GenTL::BUFFER_INFO_TIMESTAMP_NS);
  f.stream_index = stream_index;
  f.stream_id = stream_id;
  f.scan3d_geometry = scan3d_geometry;
  if (base && filled && g->DSGetBufferChunkData && !features.empty()) {
    size_t chunk_count = 0;
    auto chunk_error = g->DSGetBufferChunkData(hds, data.BufferHandle, nullptr, &chunk_count);
    if ((chunk_error == GenTL::GC_ERR_SUCCESS || chunk_error == GenTL::GC_ERR_BUFFER_TOO_SMALL)
        && chunk_count) {
      std::vector<GenTL::SINGLE_CHUNK_DATA> chunks(chunk_count);
      if (g->DSGetBufferChunkData(hds, data.BufferHandle, chunks.data(), &chunk_count)
          == GenTL::GC_ERR_SUCCESS) {
        auto* map = static_cast<GenApi::CNodeMapRef*>(features.raw_nodemap());
        if (map && map->_Ptr) {
          std::vector<GenApi::SingleChunkData_t> generic_chunks;
          generic_chunks.reserve(chunk_count);
          for (std::size_t index = 0; index < chunk_count; ++index)
            generic_chunks.push_back({chunks[index].ChunkID, chunks[index].ChunkOffset,
                                      chunks[index].ChunkLength});
          try {
            GenApi::CChunkAdapterGeneric adapter(map->_Ptr);
            adapter.AttachBuffer(const_cast<std::uint8_t*>(base), generic_chunks.data(),
                                 static_cast<std::int64_t>(generic_chunks.size()));
            if (auto geometry = read_scan3d_geometry(features, "Chunk"))
              f.scan3d_geometry = std::move(geometry);
            adapter.DetachBuffer();
          } catch (...) {
          }
        }
      }
    }
  }
  std::uint32_t part_count = 0;
  if (g->DSGetNumBufferParts && g->DSGetBufferPartInfo &&
      g->DSGetNumBufferParts(hds, data.BufferHandle, &part_count) == GenTL::GC_ERR_SUCCESS) {
    f.parts.reserve(part_count);
    for (std::uint32_t index = 0; index < part_count; ++index) {
      Frame::Part part;
      auto info = [&](GenTL::BUFFER_PART_INFO_CMD cmd) {
        return part_info(g, hds, data.BufferHandle, index, cmd);
      };
      const auto* part_base = reinterpret_cast<const std::uint8_t*>(
          static_cast<uintptr_t>(info(GenTL::BUFFER_PART_INFO_BASE)));
      const auto part_size = static_cast<std::size_t>(info(GenTL::BUFFER_PART_INFO_DATA_SIZE));
      part.width = static_cast<std::uint32_t>(info(GenTL::BUFFER_PART_INFO_WIDTH));
      part.height = static_cast<std::uint32_t>(info(GenTL::BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT));
      if (!part.height)
        part.height = static_cast<std::uint32_t>(info(GenTL::BUFFER_PART_INFO_HEIGHT));
      part.data_type = info(GenTL::BUFFER_PART_INFO_DATA_TYPE);
      part.data_format = info(GenTL::BUFFER_PART_INFO_DATA_FORMAT);
      part.format_namespace = info(GenTL::BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE);
      part.source_id = info(GenTL::BUFFER_PART_INFO_SOURCE_ID);
      part.region_id = info(GenTL::BUFFER_PART_INFO_REGION_ID);
      part.purpose_id = info(GenTL::BUFFER_PART_INFO_DATA_PURPOSE_ID);
      const auto bits = static_cast<std::size_t>((part.data_format >> 16U) & 0xffU);
      const auto padding = static_cast<std::size_t>(info(GenTL::BUFFER_PART_INFO_XPADDING));
      if (bits && part.width <= ((std::numeric_limits<std::size_t>::max)() - 7U) / bits) {
        const auto packed = (static_cast<std::size_t>(part.width) * bits + 7U) / 8U;
        if (padding <= (std::numeric_limits<std::size_t>::max)() - packed)
          part.stride = packed + padding;
      }
      const auto base_address = reinterpret_cast<std::uintptr_t>(base);
      const auto part_address = reinterpret_cast<std::uintptr_t>(part_base);
      const bool contained = base && part_base && part_address >= base_address
          && part_address - base_address <= capacity
          && part_size <= capacity - (part_address - base_address);
      if (contained && part_size) part.payload.assign(part_base, part_base + part_size);
      if (!part.payload.empty()) f.parts.push_back(std::move(part));
    }
  }
  if (f.parts.empty() && base && filled) f.payload.assign(base, base + filled);
  const auto queue_error = g->DSQueueBuffer(hds, data.BufferHandle);
  requeue.release();
  if (queue_error != GenTL::GC_ERR_SUCCESS) g->fail("DSQueueBuffer");
  return f;
}

std::vector<Frame> Camera::single(std::size_t n) {
  if (!impl_->opened) throw Error("single: not open");
  if (impl_->live.load()) throw Error("single: live in progress");
  if (n == 0) throw Error("single: n == 0");
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  std::vector<Frame> out;
  out.reserve(n);
  try {
    impl_->start_stream(n);
    for (std::size_t i = 0; i < n; ++i) {
      impl_->try_trigger();
      out.push_back(impl_->grab_one(impl_->single_timeout_ms));
    }
  } catch (...) {
    impl_->stop_acquisition();
    throw;
  }
  impl_->stop_acquisition();
  return out;
}

void Camera::prepare() {
  if (!impl_->opened) throw Error("prepare: not open");
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  try {
    impl_->prepare_stream();
  } catch (...) {
    impl_->release_stream();
    throw;
  }
}

void Camera::live() {
  if (!impl_->opened) throw Error("live: not open");
  if (impl_->live.load()) return;
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  impl_->stop_live = false;
  try {
    impl_->start_stream(GENTL_INFINITE);
  } catch (...) {
    impl_->release_stream();
    throw;
  }
  impl_->live = true;
  try {
    impl_->live_thread = std::thread([this] {
      bool error_reported = false;
      const auto report_error = [&](const std::string_view message) {
        if (error_reported) return;
        error_reported = true;
        try {
          ErrorFn callback;
          {
            std::lock_guard<std::mutex> lk(impl_->callback_mu);
            callback = impl_->on_error;
          }
          if (callback) callback(message);
        } catch (...) {
        }
      };
      while (!impl_->stop_live.load()) {
        try {
          Frame f = impl_->grab_one(500);
          FrameFn callback;
          {
            std::lock_guard<std::mutex> lk(impl_->callback_mu);
            callback = impl_->on_frame;
          }
          if (callback) callback(f);
          error_reported = false;
        } catch (const Error& error) {
          if (impl_->stop_live.load()) break;
          if (std::string_view(error.what()) != "grab timeout") report_error(error.what());
        } catch (const std::exception& error) {
          report_error(error.what());
        } catch (...) {
          report_error("live frame callback failed");
        }
      }
      impl_->live = false;
    });
  } catch (...) {
    impl_->live = false;
    impl_->stop_acquisition();
    throw;
  }
}

void Camera::trigger() {
  if (!impl_->opened) throw Error("trigger: camera not open");
  if (!impl_->hds) throw Error("trigger: acquisition is not active");
  if (!impl_->try_trigger()) throw Error("trigger: TriggerSoftware is unavailable");
}

void Camera::stop() {
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  if (impl_->live_thread.joinable()
      && impl_->live_thread.get_id() == std::this_thread::get_id()) {
    impl_->stop_live = true;
    if (impl_->g && impl_->hev) impl_->g->EventKill(impl_->hev);
    return;
  }
  impl_->stop_acquisition();
}

}  // namespace onegen
