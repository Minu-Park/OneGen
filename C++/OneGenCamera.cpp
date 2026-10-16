#include "OneGenCamera.h"
#include "OneGenError.h"
#include "OneGen.h"

#include "Internal/Match.h"
#include "Internal/ConnectTiming.h"
#include "Internal/NodeMap.h"
#include "Internal/OneGenImpl.h"
#include "Internal/FramePayload.h"
#include "Internal/CoordinateFormat.h"
#include "Internal/Scan3dGeometry.h"

#include <GenApi/GenApi.h>
#include <GenApi/ChunkAdapterGeneric.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <set>
#include <thread>

namespace onegen {
namespace {

constexpr std::size_t acquisition_buffer_count = 10;
constexpr std::uint64_t acquisition_timeout_ms = 10000;

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
  NodeMap system_features;
  std::mutex acq_mu;
  std::string open_key;
  std::uint32_t stream_index = 0;
  std::string stream_id;
  std::optional<Frame::Scan3dGeometry> scan3d_geometry;
  std::vector<Frame::ComponentIdentity> components;
  std::size_t buffer_capacity = 0;
  bool acquiring = false;
  struct FeatureEvent {
    GenTL::EVENT_HANDLE event;
    GenTL::PORT_HANDLE module;
    NodeMap* map;
    GenTL::EVENT_TYPE type;
    std::vector<std::uint8_t> bytes;
  };
  std::vector<FeatureEvent> feature_events;
  std::set<std::pair<GenTL::PORT_HANDLE, GenTL::EVENT_TYPE>> attempted_events;
  /** Unregisters feature events before their producer module is closed. */
  void clear_feature_events(GenTL::PORT_HANDLE module = nullptr) {
    for (auto it = feature_events.begin(); it != feature_events.end();) {
      if (!module || it->module == module) {
        g->GCUnregisterEvent(it->module, it->type);
        it = feature_events.erase(it);
      } else ++it;
    }
    for (auto it = attempted_events.begin(); it != attempted_events.end();)
      if (!module || it->first == module) it = attempted_events.erase(it);
      else ++it;
  }

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
  /** Loads one module while forwarding diagnostics through the installed error callback. */
  void load_module(NodeMap& map, Gentl& gentl, GenTL::PORT_HANDLE port,
                   std::string_view module) {
    load_remote_nodemap(map, gentl, port, module, [this](std::string_view message) {
      ErrorFn callback;
      { std::lock_guard lock(callback_mu); callback = on_error; }
      if (callback) callback(message);
    });
  }
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
bool Camera::is_acquiring() const {
  std::lock_guard<std::mutex> lock(impl_->acq_mu);
  return impl_->acquiring;
}
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
    case Module::System: return impl_->system_features;
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
  std::lock_guard<std::mutex> lock(impl_->acq_mu);
  if (impl_->live || impl_->acquiring) throw Error("select_data_stream: acquisition is active");
  const auto streams = data_streams();
  if (index >= streams.size()) throw Error("select_data_stream: index out of range");
  if (index == impl_->stream_index) return;
  impl_->release_stream();
  impl_->stream_index = index;
  impl_->stream_id = streams[index].id;
}

std::uint32_t Camera::selected_data_stream() const {
  std::lock_guard<std::mutex> lock(impl_->acq_mu);
  return impl_->stream_index;
}

void Camera::set_frame_callback(FrameFn cb) {
  std::lock_guard<std::mutex> lk(impl_->callback_mu);
  impl_->on_frame = std::move(cb);
}

void Camera::poll_features(std::int64_t elapsed_ms) {
  if (!impl_->opened) return;
  for (auto* map : {&impl_->features, &impl_->stream_features, &impl_->device_features,
                   &impl_->interface_features, &impl_->system_features}) map->poll(elapsed_ms);
  auto& g = *impl_->g;
  if (!g.EventGetInfo || !g.EventGetDataInfo) return;
  const auto register_event = [&](GenTL::PORT_HANDLE module, NodeMap& map, GenTL::EVENT_TYPE type) {
    if (!module || map.empty() || !impl_->attempted_events.emplace(module, type).second) return;
    GenTL::EVENT_HANDLE event = nullptr;
    if (g.GCRegisterEvent(module, type, &event) != GenTL::GC_ERR_SUCCESS) return;
    std::size_t maximum = 0, size = sizeof(maximum);
    GenTL::INFO_DATATYPE datatype = GenTL::INFO_DATATYPE_UNKNOWN;
    if (g.EventGetInfo(event, GenTL::EVENT_SIZE_MAX, &datatype, &maximum, &size) != GenTL::GC_ERR_SUCCESS
        || !maximum || maximum > 1024U * 1024U) {
      g.GCUnregisterEvent(module, type);
      return;
    }
    impl_->feature_events.push_back({event, module, &map, type, std::vector<std::uint8_t>(maximum)});
  };
  for (const auto& entry : {std::pair{g.tl, &impl_->system_features},
                           std::pair{impl_->hif, &impl_->interface_features},
                           std::pair{impl_->hdev, &impl_->device_features},
                           std::pair{impl_->hds, &impl_->stream_features}}) {
    register_event(entry.first, *entry.second, GenTL::EVENT_FEATURE_INVALIDATE);
    register_event(entry.first, *entry.second, GenTL::EVENT_MODULE);
  }
  register_event(impl_->hdev, impl_->features, GenTL::EVENT_REMOTE_DEVICE);
  for (auto& event : impl_->feature_events) {
    // Bound each drain so producer event floods cannot starve acquisition commands.
    for (int count = 0; count < 32; ++count) {
      std::size_t size = event.bytes.size();
      const auto received = g.EventGetData(event.event, event.bytes.data(), &size, 0);
      if (received == GenTL::GC_ERR_TIMEOUT) break;
      g.check(received, "EventGetData(feature)");
      if (size > event.bytes.size()) throw Error("Feature event exceeds registered size");
      GenTL::INFO_DATATYPE datatype = GenTL::INFO_DATATYPE_UNKNOWN;
      if (event.type == GenTL::EVENT_FEATURE_INVALIDATE) {
        std::vector<char> name(65537, 0);
        std::size_t name_size = name.size() - 1;
        g.check(g.EventGetDataInfo(event.event, event.bytes.data(), size, GenTL::EVENT_DATA_ID,
                                  &datatype, name.data(), &name_size), "EventGetDataInfo(feature name)");
        if (datatype != GenTL::INFO_DATATYPE_STRING || name_size > 65536)
          throw Error("Feature invalidation event has an invalid identifier");
        // A single dead-channel node must not abort the whole drain or unwind
        // through the producer event frame; skip it and keep draining.
        try { event.map->invalidate(name.data()); } catch (...) {}
      } else {
        std::uint64_t id = 0;
        std::size_t id_size = sizeof(id);
        const auto id_result = g.EventGetDataInfo(event.event, event.bytes.data(), size, GenTL::EVENT_DATA_NUMID,
                                                &datatype, &id, &id_size);
        const bool numeric_id = id_result == GenTL::GC_ERR_SUCCESS
            && datatype == GenTL::INFO_DATATYPE_UINT64 && id_size == sizeof(id);
        std::string string_id;
        if (!numeric_id) {
          std::vector<char> name(65537, 0);
          std::size_t name_size = name.size() - 1;
          g.check(g.EventGetDataInfo(event.event, event.bytes.data(), size, GenTL::EVENT_DATA_ID,
                                    &datatype, name.data(), &name_size), "EventGetDataInfo(event string ID)");
          if (datatype != GenTL::INFO_DATATYPE_STRING || name_size > 65536)
            throw Error("Generic feature event has an invalid identifier");
          string_id = name.data();
        }
        std::size_t payload_size = 0;
        const auto payload_query = g.EventGetDataInfo(event.event, event.bytes.data(), size, GenTL::EVENT_DATA_VALUE,
                                                     &datatype, nullptr, &payload_size);
        if (payload_query != GenTL::GC_ERR_SUCCESS && payload_query != GenTL::GC_ERR_BUFFER_TOO_SMALL)
          g.check(payload_query, "EventGetDataInfo(event payload size)");
        if (payload_size > 1024U * 1024U) throw Error("Generic feature event payload exceeds 1 MiB");
        std::vector<std::uint8_t> payload(payload_size + 1);
        const auto capacity = payload_size;
        g.check(g.EventGetDataInfo(event.event, event.bytes.data(), size, GenTL::EVENT_DATA_VALUE,
                                  &datatype, payload.data(), &payload_size), "EventGetDataInfo(event payload)");
        if (payload_size > capacity) throw Error("Generic feature event payload is too large");
        payload.resize(payload_size);
        try {
          if (numeric_id) event.map->deliver_event(payload, id);
          else event.map->deliver_event(payload, string_id);
        } catch (...) {}
      }
    }
  }
}
void Camera::set_error_callback(ErrorFn cb) {
  std::lock_guard<std::mutex> lk(impl_->callback_mu);
  impl_->on_error = std::move(cb);
}

void Camera::open(std::string_view query) {
  open_impl(query, {}, {});
}

void Camera::open(const DeviceInfo& device) {
  open_impl(device.id, device.cti_path, device.interface_id);
}

void Camera::open_impl(std::string_view query, std::string_view producer, std::string_view interface_id) {
  ConnectTiming timing("device-open");
  if (impl_->opened) throw Error("open: camera already open");
  auto* og = impl_->owner->impl_.get();
  std::lock_guard<std::recursive_mutex> lk(og->mu);
  if (og->producers.empty()) throw Error("open: no CTI loaded");

  // Open an enumerated ID directly; zero-timeout refresh can discard producer results.
  auto found = og->discovered;
  const auto in_scope = [&](const auto& device) {
    return (producer.empty() || device.info.cti_path == producer)
        && (interface_id.empty() || device.if_id == interface_id);
  };
  std::erase_if(found, [&](const auto& device) { return !in_scope(device); });
  if (std::none_of(found.begin(), found.end(), [&](const auto& device) { return device.info.id == query; })) {
    found = scan_devices(og->producers, 10000, interface_id, producer);
    std::erase_if(og->discovered, in_scope);
    og->discovered.insert(og->discovered.end(), found.begin(), found.end());
  }
  timing.mark("discovery-result-resolution");
  std::vector<DeviceInfo> infos;
  infos.reserve(found.size());
  for (auto& f : found) infos.push_back(f.info);
  const DeviceInfo& match = match_device(infos, query);

  FoundDevice* hit = nullptr;
  for (auto& f : found)
    if (f.info.id == match.id && f.info.cti_path == match.cti_path && f.if_id == match.interface_id) hit = &f;
  if (!hit) throw Error("open: match lost");

  const std::string key = hit->info.cti_path + '\0' + hit->if_id + '\0' + hit->info.id;
  if (og->open_keys.count(key)) throw Error("open: device already open: " + hit->info.id);

  auto& g = hit->producer->api;
  GenTL::IF_HANDLE hif = ensure_interface(*hit->producer, hit->if_id);

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
  if (err != GenTL::GC_ERR_SUCCESS) g.fail("IFOpenDevice", err);
  timing.mark("IFOpenDevice");

  try {
    try { impl_->load_module(impl_->system_features, g, g.tl, "System/TL"); } catch (...) {}
    timing.mark("System/TL-XML");
    try { impl_->load_module(impl_->interface_features, g, hif, "Interface"); } catch (...) {}
    timing.mark("Interface-XML");
    try { impl_->load_module(impl_->device_features, g, hdev, "Device"); } catch (...) {}
    timing.mark("Device-XML");
    GenTL::PORT_HANDLE hport = nullptr;
    if (g.DevGetPort(hdev, &hport) == GenTL::GC_ERR_SUCCESS) {
      try {
        impl_->load_module(impl_->features, g, hport, "Remote Device");
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
    impl_->system_features.detach();
    g.DevClose(hdev);
    throw;
  }

  impl_->g = &g;
  timing.mark("Remote-Device-XML");
  impl_->hif = hif;
  impl_->hdev = hdev;
  impl_->info = hit->info;
  impl_->open_key = key;
  impl_->opened = true;
  og->open_keys.insert(key);
  ++hit->producer->active_devices[hit->if_id];
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
  components.clear();
  auto selector = features.find("ComponentSelector");
  auto componentId = features.find("ComponentIDValue");
  if (!selector.is_readable() || !selector.is_writable()) return;
  const auto previous = selector.get_enum();
  try {
    for (const auto& name : selector.enum_entries()) {
      try {
        selector.set_enum(name);
        if (componentId.is_readable()) {
          const auto id = componentId.get_int();
          if (id >= 0) components.push_back({static_cast<std::uint64_t>(id), name});
        }
      } catch (...) { /* Unsupported entries do not identify a component. */ }
    }
  } catch (...) { components.clear(); }
  try {
    selector.set_enum(previous);
    if (selector.get_enum() != previous) throw Error("ComponentSelector restore mismatch");
  }
  catch (...) { throw Error("ComponentSelector could not be restored"); }
}

void Camera::Impl::stop_acquisition() {
  ConnectTiming timing("acquisition-stop");
  stop_live = true;
  if (g && hev) g->EventKill(hev);
  if (live_thread.joinable() && live_thread.get_id() != std::this_thread::get_id())
    live_thread.join();
  live = false;

  if (!g || !hds || !acquiring) return;
  try {
    cmd("AcquisitionStop");
  } catch (...) {
  }
  g->DSStopAcquisition(hds, GenTL::ACQ_STOP_FLAGS_DEFAULT);
  timing.mark("AcquisitionStop-and-DSStopAcquisition");
  g->DSFlushQueue(hds, GenTL::ACQ_QUEUE_ALL_TO_INPUT);
  timing.mark("DSFlushQueue");
  if (hev) {
    g->GCUnregisterEvent(hds, GenTL::EVENT_NEW_BUFFER);
    hev = nullptr;
  }
  acquiring = false;
  timing.mark("event-unregister");
  try {
    set_locked(0);
  } catch (...) {
  }
  timing.mark("unlock-parameters");
}

void Camera::Impl::release_stream() {
  stop_acquisition();
  if (!g || !hds) return;
  clear_feature_events(hds);
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
  impl_->clear_feature_events();
  impl_->features.detach();
  impl_->stream_features.detach();
  impl_->device_features.detach();
  impl_->interface_features.detach();
  impl_->system_features.detach();
  if (impl_->g && impl_->hdev) impl_->g->DevClose(impl_->hdev);
  og->open_keys.erase(impl_->open_key);
  for (auto& producer : og->producers) if (producer->path.string() == impl_->info.cti_path) {
    auto& count = producer->active_devices[impl_->info.interface_id];
    if (count) --count;
  }
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
  ConnectTiming timing("stream-prepare");
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
  timing.mark("DevOpenDataStream");
  try { load_module(stream_features, *g, hds, "Data Stream"); } catch (...) {}
  timing.mark("Data-Stream-XML");
  try_set_bool(stream_features, "AutoPacketSize", true);
  try_execute(stream_features, "ProbePacketSize");
  timing.mark("AutoPacketSize-and-ProbePacketSize");
  try_set_bool(stream_features, "EnableResend", true);
  try_set_int(stream_features, "MaxNumBuffer",
              static_cast<std::int64_t>(acquisition_buffer_count));

  snapshot_scan3d_geometry();

  const auto payload = payload_size();
  buffer_capacity = payload;
  timing.mark("stream-settings-geometry-payload");

  size_t min_buf = acquisition_buffer_count;
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  size_t sz = sizeof(min_buf);
  if (g->DSGetInfo(hds, GenTL::STREAM_INFO_BUF_ANNOUNCE_MIN, &t, &min_buf, &sz) !=
      GenTL::GC_ERR_SUCCESS)
    min_buf = acquisition_buffer_count;
  if (min_buf < acquisition_buffer_count) min_buf = acquisition_buffer_count;
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
    try { g->check(g->DSQueueBuffer(hds, b), "DSQueueBuffer"); }
    catch (const Error& error) {
      // Preserve the producer error before any diagnostic query replaces its last-error slot.
      const auto observed_mode = [this](const char* name) {
        try {
          const auto mode = features.find(name);
          if (mode.valid() && mode.kind() == NodeKind::Enumeration && mode.is_readable())
            return mode.get_enum();
        } catch (...) { /* Observation must not replace the queue failure. */ }
        return std::string("unknown");
      };
      throw Error(std::string(error.what()) + " payload=" + std::to_string(payload) + " index=" + std::to_string(i)
                  + " count=" + std::to_string(nbuf) + " announced=" + std::to_string(buffer_info(g, hds, b, GenTL::BUFFER_INFO_SIZE))
                  + " stream=" + stream_id + " streamingMode=" + observed_mode("GevStreamingMode")
                  + " genDC=" + observed_mode("GenDCStreamingStatus"));
    }
  }
  timing.mark("buffer-allocation-and-queue");

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
        + std::chrono::milliseconds(acquisition_timeout_ms);
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
  if (err != GenTL::GC_ERR_SUCCESS) g->fail("EventGetData", err);
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
  if (filled == 0 && (f.buffer_info.contains_chunk_data || f.buffer_info.incomplete))
    throw Error("grab: delivered byte count is unavailable for chunk or incomplete data");
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
  const auto layout = coordinate_format(f.pixel_format);
  const auto bits_per_pixel = layout.planar ? layout.sample_bits
      : static_cast<std::size_t>((f.pixel_format >> 16U) & 0xffU);
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
  f.components = components;
  if (base && filled && g->DSGetBufferChunkData) {
    size_t chunk_count = 0;
    auto chunk_error = g->DSGetBufferChunkData(hds, data.BufferHandle, nullptr, &chunk_count);
    if ((chunk_error == GenTL::GC_ERR_SUCCESS || chunk_error == GenTL::GC_ERR_BUFFER_TOO_SMALL)
        && chunk_count) {
      if (chunk_count > filled) throw Error("grab: chunk count exceeds delivered storage");
      std::vector<GenTL::SINGLE_CHUNK_DATA> chunks(chunk_count);
      const auto chunk_capacity = chunk_count;
      if (g->DSGetBufferChunkData(hds, data.BufferHandle, chunks.data(), &chunk_count)
          == GenTL::GC_ERR_SUCCESS) {
        if (chunk_count > chunk_capacity) throw Error("grab: invalid returned chunk count");
        for (std::size_t index = 0; index < chunk_count; ++index) {
          const auto& chunk = chunks[index];
          if (chunk.ChunkOffset > filled || chunk.ChunkLength > filled - chunk.ChunkOffset)
            throw Error("grab: chunk exceeds delivered data");
          Frame::Chunk owned;
          owned.id = chunk.ChunkID;
          owned.offset = chunk.ChunkOffset;
          owned.payload.assign(base + chunk.ChunkOffset,
                               base + chunk.ChunkOffset + chunk.ChunkLength);
          f.chunks.push_back(std::move(owned));
        }
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
            if (auto geometry = read_scan3d_geometry(features, "Chunk",
                    f.scan3d_geometry ? &*f.scan3d_geometry : nullptr)) {
              if (f.scan3d_geometry
                  && geometry->distance_unit == f.scan3d_geometry->distance_unit
                  && geometry->output_mode == f.scan3d_geometry->output_mode
                  && geometry->coordinate_system == f.scan3d_geometry->coordinate_system) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                  if (!geometry->calibrated_axes[axis] && f.scan3d_geometry->calibrated_axes[axis]) {
                    geometry->scale[axis] = f.scan3d_geometry->scale[axis];
                    geometry->offset[axis] = f.scan3d_geometry->offset[axis];
                    geometry->invalid_enabled[axis] = f.scan3d_geometry->invalid_enabled[axis];
                    geometry->invalid_value[axis] = f.scan3d_geometry->invalid_value[axis];
                    geometry->calibrated_axes[axis] = true;
                  }
                }
              }
              f.scan3d_geometry = std::move(geometry);
            }
            // Harness-only diagnostics must read chunk nodes while the buffer is attached.
            if (std::getenv("ONEGEN_SINGLE_DIAGNOSTICS_DIR")) {
              const auto report = [&](const char* name, std::int64_t part) {
                try {
                  auto node = features.find(name);
                  if (!node.is_readable()) {
                    std::cerr << "[INFO] chunk metadata: part=" << part << " node=" << name << " readable=false\n";
                    return;
                  }
                  std::string value;
                  switch (node.kind()) {
                    case NodeKind::Integer: value = std::to_string(node.get_int()); break;
                    case NodeKind::Float: { std::ostringstream out; out.precision(17); out << node.get_float(); value = out.str(); break; }
                    case NodeKind::Boolean: value = node.get_bool() ? "true" : "false"; break;
                    case NodeKind::Enumeration: value = node.get_enum(); break;
                    case NodeKind::String: value = node.get_string(); break;
                    default: value = "unsupported-value-kind"; break;
                  }
                  std::cerr << "[INFO] chunk metadata: frame=" << f.frame_id << " part=" << part
                            << " node=" << name << " value=" << value << "\n";
                } catch (const std::exception& e) {
                  std::cerr << "[WARNING] chunk metadata read failed: node=" << name << " reason=" << e.what() << "\n";
                }
              };
              // The consumer supplies vendor diagnostics; acquisition never interprets them.
              const auto reportList = [&](const char* variable, std::int64_t part) {
                const auto* configured = std::getenv(variable);
                if (!configured) return;
                std::istringstream names(configured);
                std::string name;
                while (std::getline(names, name, ','))
                  if (!name.empty()) report(name.c_str(), part);
              };
              reportList("ONEGEN_DIAGNOSTIC_CHUNK_NODES", -1);
              const auto* selectorName = std::getenv("ONEGEN_DIAGNOSTIC_CHUNK_SELECTOR");
              const auto* countName = std::getenv("ONEGEN_DIAGNOSTIC_CHUNK_COUNT");
              auto selector = features.find(selectorName ? selectorName : "");
              auto count = features.find(countName ? countName : "");
              if (selector.is_readable() && selector.is_writable() && count.is_readable()
                  && selector.kind() == NodeKind::Integer && count.kind() == NodeKind::Integer) {
                const auto original = selector.get_int();
                try {
                  const auto parts = count.get_int();
                  if (parts < 0 || parts > 256) throw Error("Diagnostic chunk part count is outside supported bounds");
                  for (std::int64_t part = 0; part < parts; ++part) {
                    selector.set_int(part);
                    reportList("ONEGEN_DIAGNOSTIC_SELECTED_CHUNK_NODES", part);
                  }
                } catch (...) {
                  selector.set_int(original);
                  throw;
                }
                selector.set_int(original);
              }
            }
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
    if (part_count > filled) throw Error("grab: part count exceeds delivered storage");
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
      const auto layout = coordinate_format(part.data_format);
      const auto bits = layout.planar ? layout.sample_bits
          : static_cast<std::size_t>((part.data_format >> 16U) & 0xffU);
      const auto padding = static_cast<std::size_t>(info(GenTL::BUFFER_PART_INFO_XPADDING));
      if (bits && part.width <= ((std::numeric_limits<std::size_t>::max)() - 7U) / bits) {
        const auto packed = (static_cast<std::size_t>(part.width) * bits + 7U) / 8U;
        if (padding <= (std::numeric_limits<std::size_t>::max)() - packed)
          part.stride = packed + padding;
      }
      const auto base_address = reinterpret_cast<std::uintptr_t>(base);
      const auto part_address = reinterpret_cast<std::uintptr_t>(part_base);
      const bool contained = base && part_base && part_address >= base_address
          && part_address - base_address <= filled
          && part_size <= filled - (part_address - base_address);
      if (!contained && part_size) throw Error("grab: part exceeds delivered data");
      if (contained && part_size) part.payload.assign(part_base, part_base + part_size);
      if (!part.payload.empty()) f.parts.push_back(std::move(part));
    }
  }
  if (f.parts.empty()) copy_single_payload(f, base, filled);
  const auto queue_error = g->DSQueueBuffer(hds, data.BufferHandle);
  requeue.release();
  if (queue_error != GenTL::GC_ERR_SUCCESS) g->fail("DSQueueBuffer", queue_error);
  return f;
}

std::vector<Frame> Camera::single(std::size_t n) {
  ConnectTiming timing("single");
  if (!impl_->opened) throw Error("single: not open");
  if (impl_->live.load()) throw Error("single: live in progress");
  if (n == 0) throw Error("single: n == 0");
  {
    std::lock_guard<std::mutex> lk(impl_->acq_mu);
    if (impl_->acquiring) throw Error("single: acquisition is active");
    impl_->start_stream(n);
  }
  timing.mark("stream-start");
  std::vector<Frame> out;
  out.reserve(n);
  std::exception_ptr error;
  try {
    for (std::size_t i = 0; i < n; ++i) {
      out.push_back(impl_->grab_one(acquisition_timeout_ms));
      timing.mark("frame-received-and-copied");
    }
  } catch (...) {
    error = std::current_exception();
  }
  {
    std::lock_guard<std::mutex> lk(impl_->acq_mu);
    impl_->stop_acquisition();
  }
  timing.mark("stop-before-return");
  if (error) std::rethrow_exception(error);
  return out;
}

void Camera::prepare() {
  if (!impl_->opened) throw Error("prepare: not open");
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  if (impl_->live || impl_->acquiring) throw Error("prepare: acquisition is active");
  try {
    impl_->release_stream();
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
  if (impl_->acquiring) throw Error("live: acquisition is active");
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
