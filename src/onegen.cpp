#include "onegen/onegen.hpp"

#include "match.hpp"
#include "nodemap.hpp"
#include "onegen_impl.hpp"

#include "onegen/error.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace onegen {
namespace {

std::string dotted_ipv4(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%u.%u.%u.%u", (v >> 24) & 255u, (v >> 16) & 255u,
                (v >> 8) & 255u, v & 255u);
  return b;
}

std::uint32_t bswap32(std::uint32_t v) {
  return (v << 24) | ((v << 8) & 0xff0000u) | ((v >> 8) & 0xff00u) | (v >> 24);
}

bool plausible_ipv4(std::uint32_t v) {
  const unsigned a = (v >> 24) & 255u;
  return a != 0 && a < 224 && a != 127;
}

std::string ipv4_from_int(std::uint32_t v) {
  if (plausible_ipv4(v)) return dotted_ipv4(v);
  const std::uint32_t s = bswap32(v);
  if (plausible_ipv4(s)) return dotted_ipv4(s);
  return {};
}

// pylon ProducerGEV: DEVICE_INFO_BASLER_IPADDR at CUSTOM_ID+0 (then subnet/gateway/mac).
std::string gentl_custom_ip(Gentl& g, GenTL::IF_HANDLE hif, const char* devid) {
  for (int cmd = GenTL::DEVICE_INFO_CUSTOM_ID; cmd < GenTL::DEVICE_INFO_CUSTOM_ID + 4; ++cmd) {
    unsigned char buf[256] = {};
    size_t n = sizeof(buf);
    GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
    if (g.IFGetDeviceInfo(hif, devid, cmd, &type, buf, &n) != GenTL::GC_ERR_SUCCESS) continue;
    if (type == GenTL::INFO_DATATYPE_STRING) {
      if (auto ip = extract_ipv4(reinterpret_cast<char*>(buf))) return *ip;
    } else if (type == GenTL::INFO_DATATYPE_UINT32 || type == GenTL::INFO_DATATYPE_INT32 ||
               type == GenTL::INFO_DATATYPE_BUFFER) {
      std::uint32_t v = 0;
      if (n >= 4) std::memcpy(&v, buf, 4);
      if (auto s = ipv4_from_int(v); !s.empty()) return s;
    } else if (type == GenTL::INFO_DATATYPE_UINT64 || type == GenTL::INFO_DATATYPE_INT64) {
      std::uint64_t v = 0;
      std::memcpy(&v, buf, n < 8 ? n : 8);
      if (v <= 0xffffffffull) {
        if (auto s = ipv4_from_int(static_cast<std::uint32_t>(v)); !s.empty()) return s;
      }
    }
  }
  return {};
}

void fill_ips_from_interface(Gentl& g, GenTL::IF_HANDLE hif, std::vector<FoundDevice>& devs) {
  bool need = false;
  for (const auto& d : devs)
    if (d.info.ip.empty()) need = true;
  if (!need) return;

  NodeMap nm;
  try {
    load_remote_nodemap(nm, g, hif);
  } catch (...) {
    return;
  }
  auto ipn = nm.find("GevDeviceIPAddress");
  if (!ipn.valid() || !ipn.is_readable()) return;
  auto sel = nm.find("DeviceSelector");

  for (size_t i = 0; i < devs.size(); ++i) {
    if (!devs[i].info.ip.empty()) continue;
    if (sel.valid() && sel.is_writable()) {
      try {
        sel.set_int(static_cast<std::int64_t>(i));
      } catch (...) {
        continue;
      }
    }
    try {
      auto ip = ipv4_from_int(static_cast<std::uint32_t>(ipn.get_int()));
      if (!ip.empty()) devs[i].info.ip = ip;
    } catch (...) {
    }
  }
}

}  // namespace

std::string gentl_string(Gentl& g, GenTL::IF_HANDLE hif, const char* devid,
                         GenTL::DEVICE_INFO_CMD cmd) {
  char buf[1024] = {};
  size_t n = sizeof(buf);
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  if (g.IFGetDeviceInfo(hif, devid, cmd, &type, buf, &n) != GenTL::GC_ERR_SUCCESS) return {};
  if (type != GenTL::INFO_DATATYPE_STRING) return {};
  return buf;
}

std::vector<FoundDevice> scan_devices(std::vector<std::unique_ptr<Producer>>& producers,
                                      uint64_t timeout_ms) {
  std::vector<FoundDevice> out;
  for (auto& prod : producers) {
    auto& g = prod->api;
    bool8_t changed = 0;
    g.check(g.TLUpdateInterfaceList(g.tl, &changed, timeout_ms), "TLUpdateInterfaceList");
    uint32_t nif = 0;
    g.check(g.TLGetNumInterfaces(g.tl, &nif), "TLGetNumInterfaces");
    for (uint32_t i = 0; i < nif; ++i) {
      char ifid[512] = {};
      size_t ifn = sizeof(ifid);
      g.check(g.TLGetInterfaceID(g.tl, i, ifid, &ifn), "TLGetInterfaceID");
      GenTL::IF_HANDLE hif = nullptr;
      g.check(g.TLOpenInterface(g.tl, ifid, &hif), "TLOpenInterface");
      bool8_t dch = 0;
      g.check(g.IFUpdateDeviceList(hif, &dch, timeout_ms), "IFUpdateDeviceList");
      uint32_t nd = 0;
      g.check(g.IFGetNumDevices(hif, &nd), "IFGetNumDevices");
      std::vector<FoundDevice> batch;
      for (uint32_t d = 0; d < nd; ++d) {
        char devid[512] = {};
        size_t dn = sizeof(devid);
        g.check(g.IFGetDeviceID(hif, d, devid, &dn), "IFGetDeviceID");
        FoundDevice fd;
        fd.producer = prod.get();
        fd.if_id = ifid;
        fd.info.id = devid;
        fd.info.vendor = gentl_string(g, hif, devid, GenTL::DEVICE_INFO_VENDOR);
        fd.info.model = gentl_string(g, hif, devid, GenTL::DEVICE_INFO_MODEL);
        fd.info.serial = gentl_string(g, hif, devid, GenTL::DEVICE_INFO_SERIAL_NUMBER);
        fd.info.user_defined_name =
            gentl_string(g, hif, devid, GenTL::DEVICE_INFO_USER_DEFINED_NAME);
        fd.info.display_name = gentl_string(g, hif, devid, GenTL::DEVICE_INFO_DISPLAYNAME);
        fd.info.tl_type = gentl_string(g, hif, devid, GenTL::DEVICE_INFO_TLTYPE);
        fd.info.cti_path = prod->path.string();
        fd.info.ip = gentl_custom_ip(g, hif, devid);
        if (fd.info.ip.empty()) {
          if (auto ip = extract_ipv4(fd.info.id))
            fd.info.ip = *ip;
          else if (auto ip2 = extract_ipv4(fd.info.display_name))
            fd.info.ip = *ip2;
          else if (auto ip3 = extract_ipv4(fd.info.user_defined_name))
            fd.info.ip = *ip3;
        }
        batch.push_back(std::move(fd));
      }
      fill_ips_from_interface(g, hif, batch);
      for (auto& fd : batch) out.push_back(std::move(fd));
      g.IFClose(hif);
    }
  }
  return out;
}

OneGen::OneGen() : impl_(std::make_unique<Impl>()) {}
OneGen::~OneGen() {
  for (auto& c : impl_->cameras) {
    try {
      c->close();
    } catch (...) {
    }
  }
  for (auto& p : impl_->producers) p->api.unload();
}

void OneGen::loadCti(const std::filesystem::path& cti) {
  const auto canon = std::filesystem::weakly_canonical(cti);
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  for (const auto& p : impl_->producers)
    if (p->path == canon) throw Error("loadCti: already loaded: " + canon.string());
  auto prod = std::make_unique<Producer>();
  prod->path = canon;
  prod->api.load(canon);
  impl_->producers.push_back(std::move(prod));
}

void OneGen::unloadCti(const std::filesystem::path& cti) {
  const auto canon = std::filesystem::weakly_canonical(cti);
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  for (auto& c : impl_->cameras) {
    if (c->is_open() && c->info().cti_path == canon.string()) c->close();
  }
  auto it = std::find_if(impl_->producers.begin(), impl_->producers.end(),
                         [&](const auto& p) { return p->path == canon; });
  if (it == impl_->producers.end()) throw Error("unloadCti: not loaded: " + canon.string());
  (*it)->api.unload();
  impl_->producers.erase(it);
}

std::vector<DeviceInfo> OneGen::enumerate(std::chrono::milliseconds timeout) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (impl_->producers.empty()) throw Error("enumerate: no CTI loaded");
  auto found = scan_devices(impl_->producers, static_cast<uint64_t>(timeout.count()));
  std::vector<DeviceInfo> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.info));
  return out;
}

Camera& OneGen::addCamera() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  impl_->cameras.push_back(std::unique_ptr<Camera>(new Camera(this)));
  impl_->camera_view.clear();
  for (auto& c : impl_->cameras) impl_->camera_view.push_back(c.get());
  return *impl_->cameras.back();
}

void OneGen::removeCamera(Camera& cam) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  auto it = std::find_if(impl_->cameras.begin(), impl_->cameras.end(),
                         [&](const auto& p) { return p.get() == &cam; });
  if (it == impl_->cameras.end()) throw Error("removeCamera: not owned");
  try {
    (*it)->close();
  } catch (...) {
  }
  impl_->cameras.erase(it);
  impl_->camera_view.clear();
  for (auto& c : impl_->cameras) impl_->camera_view.push_back(c.get());
}

std::span<Camera* const> OneGen::cameras() const {
  return std::span<Camera* const>(impl_->camera_view.data(), impl_->camera_view.size());
}

}  // namespace onegen
