#include "Internal/Match.h"
#include "Internal/OneGenImpl.h"
#include "OneGenError.h"
#include "OneGen.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

const std::string interface_id = std::string(600, 'I') + "127.0.0.1::1";
const std::string device_id = std::string(600, 'D') + "192.168.0.7";
const std::string device_vendor(1500, 'V');

GenTL::GC_ERROR copy_string(const std::string& value, char* buffer, size_t* size) {
  const auto required = value.size() + 1;
  if (*size < required) { *size = required; return GenTL::GC_ERR_BUFFER_TOO_SMALL; }
  std::memcpy(buffer, value.c_str(), required);
  *size = required;
  return GenTL::GC_ERR_SUCCESS;
}

GenTL::GC_ERROR GC_CALLTYPE interface_count(GenTL::TL_HANDLE, uint32_t* count) {
  *count = 1;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE two_interfaces(GenTL::TL_HANDLE, uint32_t* count) {
  *count = 2;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE two_interface_ids(GenTL::TL_HANDLE, uint32_t index, char* buffer, size_t* size) {
  return copy_string(index == 0 ? "busy-interface" : interface_id, buffer, size);
}
GenTL::GC_ERROR GC_CALLTYPE device_count(GenTL::IF_HANDLE, uint32_t* count) {
  *count = 1;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE no_port_urls(GenTL::PORT_HANDLE, uint32_t* count) {
  *count = 0;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE update_interfaces(GenTL::TL_HANDLE, bool8_t* changed, uint64_t) {
  *changed = 0;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE update_devices(GenTL::IF_HANDLE, bool8_t* changed, uint64_t) {
  *changed = 0;
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE get_interface_id(GenTL::TL_HANDLE, uint32_t, char* buffer, size_t* size) {
  return copy_string(interface_id, buffer, size);
}
GenTL::GC_ERROR GC_CALLTYPE open_interface(GenTL::TL_HANDLE, const char* id, GenTL::IF_HANDLE* handle) {
  assert(id == interface_id);
  *handle = reinterpret_cast<GenTL::IF_HANDLE>(1);
  return GenTL::GC_ERR_SUCCESS;
}
GenTL::GC_ERROR GC_CALLTYPE get_device_id(GenTL::IF_HANDLE, uint32_t, char* buffer, size_t* size) {
  return copy_string(device_id, buffer, size);
}
GenTL::GC_ERROR GC_CALLTYPE get_device_info(GenTL::IF_HANDLE, const char* id,
    GenTL::DEVICE_INFO_CMD command, GenTL::INFO_DATATYPE* type, void* buffer, size_t* size) {
  assert(id == device_id);
  if (command == GenTL::DEVICE_INFO_DISPLAYNAME) {
    *type = GenTL::INFO_DATATYPE_UINT32;
    if (*size < sizeof(uint32_t)) return GenTL::GC_ERR_BUFFER_TOO_SMALL;
    std::memset(buffer, 0xff, sizeof(uint32_t));
    *size = sizeof(uint32_t);
    return GenTL::GC_ERR_SUCCESS;
  }
  *type = GenTL::INFO_DATATYPE_STRING;
  return copy_string(command == GenTL::DEVICE_INFO_VENDOR ? device_vendor : interface_id,
                     static_cast<char*>(buffer), size);
}

}  // namespace

int main() {
  using onegen::DeviceInfo;
  using onegen::match_device;

  onegen::OneGen system;
  const auto producer_ctis = onegen::defaults::producerCtis();
  assert(std::is_sorted(producer_ctis.begin(), producer_ctis.end()));
  assert(std::adjacent_find(producer_ctis.begin(), producer_ctis.end()) == producer_ctis.end());

  auto producer = std::make_unique<onegen::Producer>();
  producer->api.TLUpdateInterfaceList = update_interfaces;
  producer->api.TLGetNumInterfaces = interface_count;
  producer->api.TLGetInterfaceID = get_interface_id;
  producer->api.TLOpenInterface = open_interface;
  producer->api.IFUpdateDeviceList = update_devices;
  producer->api.IFGetNumDevices = device_count;
  producer->api.IFGetDeviceID = get_device_id;
  producer->api.IFGetDeviceInfo = get_device_info;
  producer->api.GCGetNumPortURLs = no_port_urls;
  std::vector<std::unique_ptr<onegen::Producer>> fake_producers;
  fake_producers.push_back(std::move(producer));
  const auto found = onegen::scan_devices(fake_producers, 0);
  assert(found.size() == 1);
  assert(found.front().if_id == interface_id);
  assert(found.front().info.id == device_id);
  assert(found.front().info.vendor == device_vendor);
  assert(found.front().info.display_name.empty());
  assert(found.front().info.ip.empty()); // An IP-looking DeviceID is still an opaque ID.

  // One busy interface must not suppress a different available interface.
  auto& fake = *fake_producers[0];
  fake.interfaces.clear();
  fake.api.TLGetNumInterfaces = two_interfaces;
  fake.api.TLGetInterfaceID = two_interface_ids;
  int claims = 0;
  fake.claim = [&](const onegen::InterfaceInfo& info) -> std::shared_ptr<void> {
    ++claims;
    if (info.id == "busy-interface") throw onegen::Error("Interface resource busy: another owner");
    return {};
  };
  const auto partial = onegen::scan_devices(fake_producers, 0);
  assert(claims == 2 && partial.size() == 1 && partial.front().info.id == device_id);
  bool selected_busy_failed = false;
  try { onegen::scan_devices(fake_producers, 0, "busy-interface"); }
  catch (const onegen::Error&) { selected_busy_failed = true; }
  assert(selected_busy_failed);

  fake.path = std::filesystem::weakly_canonical("producer-a.cti");
  auto another = std::make_unique<onegen::Producer>();
  another->path = std::filesystem::weakly_canonical("producer-b.cti");
  another->api.TLUpdateInterfaceList = update_interfaces;
  another->api.TLGetNumInterfaces = interface_count;
  another->api.TLGetInterfaceID = get_interface_id;
  another->api.TLOpenInterface = open_interface;
  another->api.IFUpdateDeviceList = update_devices;
  another->api.IFGetNumDevices = device_count;
  another->api.IFGetDeviceID = get_device_id;
  another->api.IFGetDeviceInfo = get_device_info;
  another->api.GCGetNumPortURLs = no_port_urls;
  fake_producers.push_back(std::move(another));
  const auto scoped = onegen::scan_devices(fake_producers, 0, interface_id, fake_producers[1]->path.string());
  assert(scoped.size() == 1 && scoped.front().producer == fake_producers[1].get());
  // A failed native close must retain the consumer's physical resource claim.
  int released = 0;
  fake.claims[interface_id] = std::shared_ptr<void>(reinterpret_cast<void*>(1), [&](void*) { ++released; });
  fake.pinned.insert(interface_id);
  fake.api.IFClose = [](GenTL::IF_HANDLE) -> GenTL::GC_ERROR { return GenTL::GC_ERR_ERROR; };
  bool close_failed = false;
  try { onegen::close_interface(fake, interface_id); } catch (const onegen::Error&) { close_failed = true; }
  assert(close_failed && released == 0 && fake.interfaces.count(interface_id) == 1);
  fake.api.IFClose = [](GenTL::IF_HANDLE) -> GenTL::GC_ERROR { return GenTL::GC_ERR_SUCCESS; };
  onegen::close_interface(fake, interface_id);
  assert(released == 1 && !fake.interfaces.count(interface_id) && !fake.pinned.count(interface_id));

  const auto temp = std::filesystem::temp_directory_path()
      / ("onegen-cti-test-" + std::to_string(reinterpret_cast<std::uintptr_t>(&system)));
  std::filesystem::create_directories(temp);
  const auto fake_cti = temp / "Producer.cti";
  std::ofstream(fake_cti).put('\0');
#ifdef _WIN32
  char* old_path = nullptr;
  std::size_t old_path_size = 0;
  _dupenv_s(&old_path, &old_path_size, "GENICAM_GENTL64_PATH");
  _putenv_s("GENICAM_GENTL64_PATH", temp.string().c_str());
#else
  const char* old_value = std::getenv("GENICAM_GENTL64_PATH");
  const std::string old_path = old_value ? old_value : "";
  setenv("GENICAM_GENTL64_PATH", temp.c_str(), 1);
#endif
  const auto environment_ctis = onegen::defaults::producerCtis();
  assert(environment_ctis.size() == 1); // No implicit vendor installation fallback.
  assert(std::find(environment_ctis.begin(), environment_ctis.end(),
                   std::filesystem::weakly_canonical(fake_cti)) != environment_ctis.end());
  const auto nested_cti = temp / "vendor" / "nested" / "Custom.CTI";
  std::filesystem::create_directories(nested_cti.parent_path());
  std::ofstream(nested_cti).put('\0');
  const auto nested_ctis = onegen::defaults::producerCtis(temp);
  assert(std::find(nested_ctis.begin(), nested_ctis.end(),
                   std::filesystem::weakly_canonical(nested_cti)) != nested_ctis.end());
  std::error_code link_error;
  std::filesystem::create_hard_link(fake_cti, temp / "Alias.cti", link_error);
  if (!link_error) {
    const auto aliases = onegen::defaults::producerCtis(temp);
    assert(aliases.size() == nested_ctis.size());
  }
#ifdef _WIN32
  _putenv_s("GENICAM_GENTL64_PATH", old_path ? old_path : "");
  free(old_path);
#else
  if (old_value) setenv("GENICAM_GENTL64_PATH", old_path.c_str(), 1);
  else unsetenv("GENICAM_GENTL64_PATH");
#endif
  std::filesystem::remove_all(temp);

  std::vector<DeviceInfo> ds;
  DeviceInfo a;
  a.id = "Device-123";
  a.user_defined_name = "LineCam";
  a.ip = "192.168.0.157";
  a.serial = "123";
  a.display_name = "Generic camera (192.168.0.157)";
  a.model = "GenericModel";
  ds.push_back(a);

  DeviceInfo b = a;
  b.id = "Other-9";
  b.user_defined_name = "Other";
  b.ip = "192.168.0.10";
  b.serial = "9";
  b.display_name = "other";
  b.model = "other-model";
  ds.push_back(b);

  assert(match_device(ds, "LineCam").id == "Device-123");
  assert(match_device(ds, "192.168.0.157").id == "Device-123");
  assert(match_device(ds, "123").id == "Device-123");
  assert(match_device(ds, "GenericModel").id == "Device-123");
  assert(match_device(ds, "Device-123").id == "Device-123");
  auto case_variant = a;
  case_variant.id = "device-123";
  assert(match_device({a, case_variant}, "Device-123").id == "Device-123");
  assert(match_device({a, case_variant}, "device-123").id == "device-123");
  auto misleading = a;
  misleading.id = "Device-192.168.1.42";
  misleading.ip.clear();
  bool inferred_ip = false;
  try { match_device({misleading}, "192.168.1.42"); inferred_ip = true; } catch (const onegen::Error&) { }
  assert(!inferred_ip);
  assert(!onegen::extract_ipv4("999.999.999.999"));
  assert(!onegen::extract_ipv4("prefix1192.168.0.1"));

  bool threw = false;
  try {
    match_device(ds, "nope");
  } catch (const onegen::Error&) {
    threw = true;
  }
  assert(threw);

  DeviceInfo mac;
  mac.id = "003053AABBCC";
  mac.ip = "192.168.0.157";
  mac.serial = "21734321";
  mac.display_name = "GenericModel";
  assert(match_device({mac}, "192.168.0.157").id == "003053AABBCC");

  threw = false;
  try {
    match_device({}, "192.168.0.157");
  } catch (const onegen::Error& e) {
    threw = std::string(e.what()).find("none enumerated") != std::string::npos;
  }
  assert(threw);

  system.addCamera();

  std::cout << "test_open_query ok\n";
  return 0;
}
