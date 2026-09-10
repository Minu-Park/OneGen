#include "onegen/camera.hpp"
#include "onegen/error.hpp"
#include "onegen/onegen.hpp"

#include "match.hpp"
#include "nodemap.hpp"
#include "onegen_impl.hpp"

#include <GenApi/GenApi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

namespace onegen {
namespace {

std::uint64_t buffer_info(Gentl* g, GenTL::DS_HANDLE hds, GenTL::BUFFER_HANDLE b,
                          GenTL::BUFFER_INFO_CMD cmd) {
  unsigned char raw[16] = {};
  size_t n = sizeof(raw);
  GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
  if (!g || g->DSGetBufferInfo(hds, b, cmd, &t, raw, &n) != GenTL::GC_ERR_SUCCESS) return 0;
  switch (t) {
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

}  // namespace

struct Camera::Impl {
  OneGen* owner = nullptr;
  DeviceInfo info;
  bool opened = false;
  std::atomic<bool> live{false};
  std::atomic<bool> stop_live{false};
  std::thread live_thread;
  FrameFn on_frame;

  Gentl* g = nullptr;
  GenTL::IF_HANDLE hif = nullptr;
  GenTL::DEV_HANDLE hdev = nullptr;
  GenTL::PORT_HANDLE hport = nullptr;
  GenTL::DS_HANDLE hds = nullptr;
  GenTL::EVENT_HANDLE hev = nullptr;
  size_t nbuf = 0;
  NodeMap features;
  std::mutex acq_mu;
  std::string open_key;

  void stop_stream();
  void start_stream(uint64_t n_frames);
  Frame grab_one(uint64_t timeout_ms);
  void cmd(const char* name);
  void set_locked(int64_t v);
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
NodeMap& Camera::features() { return impl_->features; }

Node Camera::feature(std::string_view name) {
  Node n = impl_->features.find(name);
  if (!n.valid()) throw Error("feature not found: " + std::string(name));
  return n;
}

void Camera::set_frame_callback(FrameFn cb) { impl_->on_frame = std::move(cb); }

void Camera::open(std::string_view query) {
  if (impl_->opened) throw Error("open: camera already open");
  auto* og = impl_->owner->impl_.get();
  std::lock_guard<std::recursive_mutex> lk(og->mu);
  if (og->producers.empty()) throw Error("open: no CTI loaded");

  auto found = scan_devices(og->producers, 1000);
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
  GenTL::IF_HANDLE hif = nullptr;
  g.check(g.TLOpenInterface(g.tl, hit->if_id.c_str(), &hif), "TLOpenInterface");
  bool8_t ch = 0;
  g.check(g.IFUpdateDeviceList(hif, &ch, 1000), "IFUpdateDeviceList");

  GenTL::DEVICE_ACCESS_FLAGS modes[] = {GenTL::DEVICE_ACCESS_EXCLUSIVE,
                                        GenTL::DEVICE_ACCESS_CONTROL};
  GenTL::DEV_HANDLE hdev = nullptr;
  GenTL::GC_ERROR err = GenTL::GC_ERR_ERROR;
  for (auto m : modes) {
    err = g.IFOpenDevice(hif, hit->info.id.c_str(), m, &hdev);
    if (err == GenTL::GC_ERR_SUCCESS) break;
  }
  if (err != GenTL::GC_ERR_SUCCESS) {
    g.IFClose(hif);
    g.fail("IFOpenDevice");
  }

  GenTL::PORT_HANDLE hport = nullptr;
  if (g.DevGetPort(hdev, &hport) != GenTL::GC_ERR_SUCCESS) {
    g.DevClose(hdev);
    g.IFClose(hif);
    g.fail("DevGetPort");
  }

  try {
    load_remote_nodemap(impl_->features, g, hport);
  } catch (...) {
    g.DevClose(hdev);
    g.IFClose(hif);
    throw;
  }

  impl_->g = &g;
  impl_->hif = hif;
  impl_->hdev = hdev;
  impl_->hport = hport;
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

void Camera::Impl::set_locked(int64_t v) {
  auto n = features.find("TLParamsLocked");
  if (n.valid() && n.is_writable()) n.set_int(v);
}

void Camera::Impl::stop_stream() {
  stop_live = true;
  if (g && hev) g->EventKill(hev);
  if (live_thread.joinable() && live_thread.get_id() != std::this_thread::get_id())
    live_thread.join();
  live = false;

  if (!g || !hds) return;
  try {
    cmd("AcquisitionStop");
  } catch (...) {
  }
  g->DSStopAcquisition(hds, GenTL::ACQ_STOP_FLAGS_DEFAULT);
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
  g->DSClose(hds);
  hds = nullptr;
  try {
    set_locked(0);
  } catch (...) {
  }
}

void Camera::close() {
  stop();
  if (!impl_->opened) return;
  auto* og = impl_->owner->impl_.get();
  std::lock_guard<std::recursive_mutex> lk(og->mu);
  impl_->features.detach();
  if (impl_->g && impl_->hdev) impl_->g->DevClose(impl_->hdev);
  if (impl_->g && impl_->hif) impl_->g->IFClose(impl_->hif);
  og->open_keys.erase(impl_->open_key);
  impl_->hdev = nullptr;
  impl_->hif = nullptr;
  impl_->hport = nullptr;
  impl_->g = nullptr;
  impl_->opened = false;
  impl_->info = {};
  impl_->open_key.clear();
}

void Camera::Impl::start_stream(uint64_t n_frames) {
  if (!opened || !g) throw Error("stream: camera not open");
  if (hds) stop_stream();

  uint32_t ns = 0;
  g->check(g->DevGetNumDataStreams(hdev, &ns), "DevGetNumDataStreams");
  if (ns == 0) throw Error("no data streams");
  char dsid[256] = {};
  size_t dsn = sizeof(dsid);
  g->check(g->DevGetDataStreamID(hdev, 0, dsid, &dsn), "DevGetDataStreamID");
  g->check(g->DevOpenDataStream(hdev, dsid, &hds), "DevOpenDataStream");

  set_locked(1);

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
  if (payload == 0) payload = 16 * 1024 * 1024;  // ponytail: ceiling unknown until DS reports

  size_t min_buf = 4;
  sz = sizeof(min_buf);
  if (g->DSGetInfo(hds, GenTL::STREAM_INFO_BUF_ANNOUNCE_MIN, &t, &min_buf, &sz) !=
      GenTL::GC_ERR_SUCCESS)
    min_buf = 4;
  if (min_buf < 4) min_buf = 4;

  nbuf = min_buf;
  for (size_t i = 0; i < nbuf; ++i) {
    GenTL::BUFFER_HANDLE b = nullptr;
    g->check(g->DSAllocAndAnnounceBuffer(hds, payload, nullptr, &b), "DSAllocAndAnnounceBuffer");
    g->check(g->DSQueueBuffer(hds, b), "DSQueueBuffer");
  }

  g->check(g->GCRegisterEvent(hds, GenTL::EVENT_NEW_BUFFER, &hev), "GCRegisterEvent");
  g->check(g->DSStartAcquisition(hds, GenTL::ACQ_START_FLAGS_DEFAULT, n_frames),
           "DSStartAcquisition");
  cmd("AcquisitionStart");
}

Frame Camera::Impl::grab_one(uint64_t timeout_ms) {
  GenTL::EVENT_NEW_BUFFER_DATA data{};
  size_t size = sizeof(data);
  auto err = g->EventGetData(hev, &data, &size, timeout_ms);
  if (err == GenTL::GC_ERR_TIMEOUT) throw Error("grab timeout");
  if (err != GenTL::GC_ERR_SUCCESS) g->fail("EventGetData");
  if (!data.BufferHandle) throw Error("grab: null buffer handle");

  Frame f;
  auto u64 = [&](GenTL::BUFFER_INFO_CMD cmd) {
    return buffer_info(g, hds, data.BufferHandle, cmd);
  };
  auto* base = reinterpret_cast<const std::uint8_t*>(static_cast<uintptr_t>(u64(GenTL::BUFFER_INFO_BASE)));
  size_t filled = static_cast<size_t>(u64(GenTL::BUFFER_INFO_SIZE_FILLED));
  if (filled == 0) filled = static_cast<size_t>(u64(GenTL::BUFFER_INFO_DATA_SIZE));
  if (filled == 0) filled = static_cast<size_t>(u64(GenTL::BUFFER_INFO_SIZE));
  f.width = static_cast<std::uint32_t>(u64(GenTL::BUFFER_INFO_WIDTH));
  f.height = static_cast<std::uint32_t>(u64(GenTL::BUFFER_INFO_HEIGHT));
  f.pixel_format = u64(GenTL::BUFFER_INFO_PIXELFORMAT);
  f.timestamp = u64(GenTL::BUFFER_INFO_TIMESTAMP);
  if (base && filled) f.payload.assign(base, base + filled);
  g->check(g->DSQueueBuffer(hds, data.BufferHandle), "DSQueueBuffer");
  return f;
}

std::vector<Frame> Camera::single(std::size_t n) {
  if (!impl_->opened) throw Error("single: not open");
  if (impl_->live.load()) throw Error("single: live in progress");
  if (n == 0) throw Error("single: n == 0");
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  impl_->start_stream(n);
  std::vector<Frame> out;
  out.reserve(n);
  try {
    for (std::size_t i = 0; i < n; ++i) out.push_back(impl_->grab_one(5000));
  } catch (...) {
    impl_->stop_stream();
    throw;
  }
  impl_->stop_stream();
  return out;
}

void Camera::live() {
  if (!impl_->opened) throw Error("live: not open");
  if (impl_->live.load()) return;
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  impl_->stop_live = false;
  impl_->start_stream(GENTL_INFINITE);
  impl_->live = true;
  impl_->live_thread = std::thread([this] {
    while (!impl_->stop_live.load()) {
      try {
        Frame f = impl_->grab_one(500);
        if (impl_->on_frame) impl_->on_frame(f);
      } catch (const Error&) {
        if (impl_->stop_live.load()) break;
      }
    }
  });
}

void Camera::stop() {
  std::lock_guard<std::mutex> lk(impl_->acq_mu);
  impl_->stop_stream();
}

}  // namespace onegen
