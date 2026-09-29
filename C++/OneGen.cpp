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
#include <iostream>
#include <string_view>

namespace onegen {
namespace {

std::string dotted_ipv4(std::uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%u.%u.%u.%u", (v >> 24) & 255u, (v >> 16) & 255u,
                (v >> 8) & 255u, v & 255u);
  return b;
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
  auto device_id = nm.find("DeviceID");
  if (!sel.valid() || !sel.is_readable() || !sel.is_writable()
      || !device_id.valid() || !device_id.is_readable()) return;
  std::int64_t previous;
  try { previous = sel.get_int(); } catch (...) { return; }
  // GenTL SFNC ties DeviceSelector to IFGetDeviceID's index; verify the selected ID.
  for (size_t i = 0; i < devs.size(); ++i) {
    try {
      sel.set_int(static_cast<std::int64_t>(i));
      if (device_id.get_string() != devs[i].info.id) continue;
      const auto address = ipn.get_int();
      if (address >= 0 && static_cast<std::uint64_t>(address) <= UINT32_MAX)
        devs[i].info.ip = dotted_ipv4(static_cast<std::uint32_t>(address));
    } catch (...) { }
  }
  try { sel.set_int(previous); } catch (...) { }

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

namespace {
Producer& producer_for(std::vector<std::unique_ptr<Producer>>& producers, const InterfaceInfo& info) {
  std::error_code error;
  const auto path = std::filesystem::weakly_canonical(info.cti_path, error);
  if (!error) for (auto& producer : producers) if (producer->path == path) return *producer;
  throw Error("Interface producer is unavailable: " + info.cti_path);
}
}

void close_interfaces(Producer& producer) {
  producer.interface_maps.clear();
  for (auto& [id, hif] : producer.interfaces) {
    (void)id;
    if (hif) producer.api.IFClose(hif);
  }
  producer.interfaces.clear();
  producer.claims.clear();
  producer.pinned.clear();
}

void close_interface(Producer& producer, const std::string& id) {
  if (producer.active_devices[id]) throw Error("Disconnect this interface's cameras before release.");
  if (const auto found = producer.interfaces.find(id); found != producer.interfaces.end()) {
    producer.interface_maps.erase(id);
    producer.api.check(producer.api.IFClose(found->second), "IFClose");
    producer.interfaces.erase(found);
    producer.claims.erase(id);
  }
  producer.pinned.erase(id);
}

InterfaceInfo interface_info(Producer& producer, const std::string& id) {
  auto& g = producer.api;
  InterfaceInfo info{id, producer.path.string()};
  const auto producer_string = [&](GenTL::TL_INFO_CMD command) {
    if (!g.GCGetInfo) return std::string{};
    GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
    auto value = read_gentl_string(g, "GCGetInfo", [&](char* data, size_t* size) {
      return g.GCGetInfo(command, &type, data, size);
    }, true);
    return type == GenTL::INFO_DATATYPE_STRING ? value : std::string{};
  };
  info.vendor = producer_string(GenTL::TL_INFO_VENDOR);
  if (g.TLGetInterfaceInfo) {
    const auto interface_string = [&](GenTL::INTERFACE_INFO_CMD command) {
      GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
      auto value = read_gentl_string(g, "TLGetInterfaceInfo", [&](char* data, size_t* size) {
        return g.TLGetInterfaceInfo(g.tl, id.c_str(), command, &type, data, size);
      }, true);
      return type == GenTL::INFO_DATATYPE_STRING ? value : std::string{};
    };
    info.display_name = interface_string(GenTL::INTERFACE_INFO_DISPLAYNAME);
    info.tl_type = interface_string(GenTL::INTERFACE_INFO_TLTYPE);
  }
  return info;
}

GenTL::IF_HANDLE ensure_interface(Producer& producer, const std::string& id) {
  if (const auto found = producer.interfaces.find(id); found != producer.interfaces.end()) return found->second;
  auto claim = producer.claim ? producer.claim(interface_info(producer, id)) : std::shared_ptr<void>{};
  GenTL::IF_HANDLE handle = nullptr;
  try { producer.api.check(producer.api.TLOpenInterface(producer.api.tl, id.c_str(), &handle), "TLOpenInterface"); }
  catch (const std::exception& error) {
    throw Error("interface=" + id + " cti=" + producer.path.string() + ": " + error.what());
  }
  producer.interfaces.emplace(id, handle);
  producer.claims[id] = std::move(claim);
  return handle;
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
    uint64_t timeout_ms, std::string_view selected_interface, std::string_view selected_producer,
    std::vector<std::string>* failures) {
  std::vector<FoundDevice> out;
  for (auto& prod : producers) {
    if (!selected_producer.empty() && prod->path.string() != selected_producer) continue;
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
      if (!selected_interface.empty() && ifid != selected_interface) continue;
      try {
      const auto hif = ensure_interface(*prod, ifid);
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
        fd.info.interface_id = ifid;
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
      } catch (const std::exception& error) {
        if (!selected_interface.empty()) throw;
        if (failures) failures->push_back("interface=" + ifid + " cti=" + prod->path.string()
                                         + ": " + error.what());
        stage_log_warning("interface search failed: cti=%s interface=%s: %s",
                          prod->path.string().c_str(), ifid.c_str(), error.what());
      }
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
  prod->claim = impl_->claim;
  impl_->producers.push_back(std::move(prod));
}

void OneGen::set_interface_claim_callback(InterfaceClaimFn claim) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  for (const auto& producer : impl_->producers)
    if (!producer->interfaces.empty()) throw Error("Set resource claims before interface discovery.");
  impl_->claim = claim;
  for (auto& producer : impl_->producers) {
    producer->claim = claim;
  }
}

std::vector<InterfaceInfo> OneGen::interfaces() const {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  std::vector<InterfaceInfo> result;
  for (auto& producer : impl_->producers) {
    auto& g = producer->api;
    bool8_t changed = 0;
    g.check(g.TLUpdateInterfaceList(g.tl, &changed, 1000), "TLUpdateInterfaceList");
    uint32_t count = 0;
    g.check(g.TLGetNumInterfaces(g.tl, &count), "TLGetNumInterfaces");
    for (uint32_t i = 0; i < count; ++i) {
      const auto id = read_gentl_string(g, "TLGetInterfaceID", [&](char* data, size_t* size) {
        return g.TLGetInterfaceID(g.tl, i, data, size);
      });
      result.push_back(interface_info(*producer, id));
    }
  }
  return result;
}

void OneGen::open_interface(std::string_view id) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  const auto inventory = interfaces();
  const auto match = std::find_if(inventory.begin(), inventory.end(),
      [&](const auto& item) { return item.id == id; });
  if (match == inventory.end()) throw Error("Interface is unavailable: " + std::string(id));
  if (std::count_if(inventory.begin(), inventory.end(), [&](const auto& item) { return item.id == id; }) != 1)
    throw Error("Interface ID is ambiguous across producers.");
  for (auto& producer : impl_->producers) if (producer->path.string() == match->cti_path) {
    ensure_interface(*producer, match->id);
    producer->pinned.insert(match->id);
  }
}

void OneGen::open_interface(const InterfaceInfo& info) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  auto& producer = producer_for(impl_->producers, info);
  ensure_interface(producer, info.id);
  producer.pinned.insert(info.id);
}

void OneGen::release_interface(std::string_view id) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  const auto key = std::string(id);
  if (std::count_if(impl_->producers.begin(), impl_->producers.end(),
      [&](const auto& producer) { return producer->interfaces.count(key); }) > 1)
    throw Error("Interface ID is ambiguous across producers.");
  for (auto& producer : impl_->producers) if (producer->interfaces.count(key)) close_interface(*producer, key);
}

void OneGen::release_interface(const InterfaceInfo& info) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  close_interface(producer_for(impl_->producers, info), info.id);
}

void OneGen::release_idle_interfaces() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  for (auto& producer : impl_->producers) {
    std::vector<std::string> idle;
    for (const auto& [id, handle] : producer->interfaces)
      if (!producer->pinned.count(id) && !producer->active_devices[id]) idle.push_back(id);
    for (const auto& id : idle) close_interface(*producer, id);
  }
}

std::vector<DeviceInfo> OneGen::enumerate_interface(std::string_view id, std::chrono::milliseconds timeout) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  const auto inventory = interfaces();
  const auto match = std::find_if(inventory.begin(), inventory.end(), [&](const auto& info) { return info.id == id; });
  if (match == inventory.end()) throw Error("Interface is unavailable: " + std::string(id));
  if (std::count_if(inventory.begin(), inventory.end(), [&](const auto& info) { return info.id == id; }) != 1)
    throw Error("Interface ID is ambiguous across producers; specify its producer.");
  return enumerate_interface(*match, timeout);
}

std::vector<DeviceInfo> OneGen::enumerate_interface(const InterfaceInfo& info, std::chrono::milliseconds timeout) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  auto& producer = producer_for(impl_->producers, info);
  auto found = scan_devices(impl_->producers, static_cast<uint64_t>(timeout.count()), info.id, producer.path.string());
  std::erase_if(impl_->discovered, [&](const auto& device) { return device.producer == &producer && device.if_id == info.id; });
  impl_->discovered.insert(impl_->discovered.end(), found.begin(), found.end());
  std::vector<DeviceInfo> result;
  for (auto& device : found) result.push_back(std::move(device.info));
  return result;
}

NodeMap& OneGen::interface_features(std::string_view id) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (std::count_if(impl_->producers.begin(), impl_->producers.end(),
      [&](const auto& producer) { return producer->interfaces.count(std::string(id)); }) > 1)
    throw Error("Interface ID is ambiguous across producers.");
  for (auto& producer : impl_->producers) {
    const auto found = producer->interfaces.find(std::string(id));
    if (found == producer->interfaces.end()) continue;
    return interface_features(InterfaceInfo{found->first, producer->path.string()});
  }
  throw Error("Interface is unavailable: " + std::string(id));
}

NodeMap& OneGen::interface_features(const InterfaceInfo& info) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  auto& producer = producer_for(impl_->producers, info);
  const auto handle = producer.interfaces.find(info.id);
  if (handle == producer.interfaces.end()) throw Error("Interface is unavailable: " + info.id);
  auto& map = producer.interface_maps[info.id];
  if (!map) {
    auto loaded = std::make_unique<NodeMap>();
    load_remote_nodemap(*loaded, producer.api, handle->second, "Interface");
    map = std::move(loaded);
  }
  return *map;
}

void OneGen::release_interfaces() {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  if (!impl_->open_keys.empty()) throw Error("Disconnect all cameras before interface reconfiguration.");
  impl_->discovered.clear();
  for (auto& producer : impl_->producers) {
    std::vector<std::string> ids;
    for (const auto& [id, handle] : producer->interfaces) ids.push_back(id);
    for (const auto& id : ids) close_interface(*producer, id);
  }
}

std::vector<DeviceInfo> OneGen::enumerate(std::chrono::milliseconds timeout) {
  std::vector<std::string> failures;
  return enumerate(timeout, failures);
}

std::vector<DeviceInfo> OneGen::enumerate(std::chrono::milliseconds timeout,
    std::vector<std::string>& failures) {
  std::lock_guard<std::recursive_mutex> lk(impl_->mu);
  failures.clear();
  if (impl_->producers.empty()) throw Error("enumerate: no CTI loaded");
  auto found = scan_devices(impl_->producers, static_cast<uint64_t>(timeout.count()), {}, {}, &failures);
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
