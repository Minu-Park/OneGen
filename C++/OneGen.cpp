#include "OneGen.h"

#include "Internal/Match.h"
#include "Internal/NodeMap.h"
#include "Internal/OneGenImpl.h"
#include "Internal/StageLog.h"

#include "OneGenError.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

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

bool loopback_interface(std::string_view id)
{
  return id.find("127.0.0.1") != std::string_view::npos
      || id.find("::1") != std::string_view::npos;
}

template <std::size_t InitialSize = 512, typename Read>
std::string read_gentl_string(Gentl& g, const char* where, Read read, bool optional = false) {
  std::array<char, InitialSize> local{};
  std::size_t size = local.size();
  auto result = read(local.data(), &size);
  if (result == GenTL::GC_ERR_BUFFER_TOO_SMALL) {
    if (size <= local.size() || size > 1024 * 1024) {
      if (optional) return {};
      throw Error(std::string(where) + ": invalid string size");
    }
    std::string value(size, '\0');
    const auto capacity = size;
    result = read(value.data(), &size);
    if (result == GenTL::GC_ERR_SUCCESS) {
      if (size == 0 || size > capacity || value[size - 1] != '\0') {
        if (optional) return {};
        throw Error(std::string(where) + ": invalid string response");
      }
      value.resize(size - 1);
      return value;
    }
  }
  if (optional && result != GenTL::GC_ERR_SUCCESS) return {};
  g.check(result, where);
  if (size == 0 || size > local.size() || local[size - 1] != '\0') {
    if (optional) return {};
    throw Error(std::string(where) + ": invalid string response");
  }
  return {local.data(), size - 1};
}

}  // namespace

void close_interfaces(Producer& producer) {
  for (auto& [id, hif] : producer.interfaces) {
    (void)id;
    if (hif) producer.api.IFClose(hif);
  }
  producer.interfaces.clear();
}

std::string gentl_string(Gentl& g, GenTL::IF_HANDLE hif, const char* devid,
                         GenTL::DEVICE_INFO_CMD cmd) {
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  auto value = read_gentl_string<1024>(g, "IFGetDeviceInfo", [&](char* buffer, size_t* size) {
    return g.IFGetDeviceInfo(hif, devid, cmd, &type, buffer, size);
  }, true);
  return type == GenTL::INFO_DATATYPE_STRING ? value : std::string{};
}

std::vector<FoundDevice> scan_devices(std::vector<std::unique_ptr<Producer>>& producers,
                                      uint64_t timeout_ms) {
  std::vector<FoundDevice> out;
  for (auto& prod : producers) {
    const auto first_device = out.size();
    auto& g = prod->api;
    bool8_t changed = 0;
    const auto transport_started = std::chrono::steady_clock::now();
    g.check(g.TLUpdateInterfaceList(g.tl, &changed, timeout_ms), "TLUpdateInterfaceList");
    uint32_t nif = 0;
    g.check(g.TLGetNumInterfaces(g.tl, &nif), "TLGetNumInterfaces");
    for (uint32_t i = 0; i < nif; ++i) {
      const auto ifid = read_gentl_string(g, "TLGetInterfaceID", [&](char* buffer, size_t* size) {
        return g.TLGetInterfaceID(g.tl, i, buffer, size);
      });
      if (loopback_interface(ifid)) {
        stage_log("worker skip loopback interface=%s", ifid.c_str());
        continue;
      }
      GenTL::IF_HANDLE hif = nullptr;
      if (const auto it = prod->interfaces.find(ifid); it != prod->interfaces.end()) {
        hif = it->second;
      } else {
        g.check(g.TLOpenInterface(g.tl, ifid.c_str(), &hif), "TLOpenInterface");
        prod->interfaces.emplace(ifid, hif);
      }
      bool8_t dch = 0;
      const auto update_started = std::chrono::steady_clock::now();
      g.check(g.IFUpdateDeviceList(hif, &dch, timeout_ms), "IFUpdateDeviceList");
      const auto update_finished = std::chrono::steady_clock::now();
      uint32_t nd = 0;
      g.check(g.IFGetNumDevices(hif, &nd), "IFGetNumDevices");
      std::vector<FoundDevice> batch;
      for (uint32_t d = 0; d < nd; ++d) {
        const auto devid = read_gentl_string(g, "IFGetDeviceID", [&](char* buffer, size_t* size) {
          return g.IFGetDeviceID(hif, d, buffer, size);
        });
        FoundDevice fd;
        fd.producer = prod.get();
        fd.if_id = ifid;
        fd.info.id = devid;
        fd.info.vendor = gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_VENDOR);
        fd.info.model = gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_MODEL);
        fd.info.serial = gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_SERIAL_NUMBER);
        fd.info.user_defined_name =
            gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_USER_DEFINED_NAME);
        fd.info.display_name = gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_DISPLAYNAME);
        fd.info.tl_type = gentl_string(g, hif, devid.c_str(), GenTL::DEVICE_INFO_TLTYPE);
        fd.info.cti_path = prod->path.string();
        if (auto ip = extract_ipv4(fd.info.id))
          fd.info.ip = *ip;
        else if (auto ip2 = extract_ipv4(fd.info.display_name))
          fd.info.ip = *ip2;
        else if (auto ip3 = extract_ipv4(fd.info.user_defined_name))
          fd.info.ip = *ip3;
        batch.push_back(std::move(fd));
      }
      if (!batch.empty()) {
        fill_ips_from_interface(g, hif, batch);
      }
      if (nd > 0)
        stage_log("interface scan completed: interface=%s cameras=%u update_ms=%lld total_ms=%lld",
                  ifid.c_str(), nd,
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                      update_finished - update_started).count()),
                  static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - update_started).count()));
      for (auto& fd : batch) out.push_back(std::move(fd));
    }
    if (out.size() > first_device)
      stage_log("producer scan completed: interfaces=%u cameras=%u timeout_ms=%llu elapsed_ms=%lld",
                nif, static_cast<unsigned>(out.size() - first_device),
                static_cast<unsigned long long>(timeout_ms),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - transport_started).count()));
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
  for (auto& p : impl_->producers) {
    close_interfaces(*p);
    p->api.unload();
  }
}

void OneGen::loadCti(const std::filesystem::path& cti) {
  const auto canon = std::filesystem::weakly_canonical(cti);
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  for (const auto& p : impl_->producers)
    if (p->path == canon) return;
  auto prod = std::make_unique<Producer>();
  prod->path = canon;
  prod->api.load(canon);
  impl_->producers.push_back(std::move(prod));
}

std::vector<DeviceInfo> OneGen::enumerate(std::chrono::milliseconds timeout) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (impl_->producers.empty()) throw Error("enumerate: no CTI loaded");
  auto found = scan_devices(impl_->producers, static_cast<uint64_t>(timeout.count()));
  impl_->discovered = found;
  std::vector<DeviceInfo> out;
  out.reserve(found.size());
  for (auto& f : found) out.push_back(std::move(f.info));
  return out;
}

Camera& OneGen::addCamera() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  impl_->cameras.push_back(std::unique_ptr<Camera>(new Camera(this)));
  return *impl_->cameras.back();
}

}  // namespace onegen
