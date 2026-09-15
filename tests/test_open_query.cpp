#include "match.hpp"
#include "onegen/error.hpp"
#include "onegen/onegen.hpp"

#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
  using onegen::DeviceInfo;
  using onegen::match_device;

  onegen::OneGen system;
  assert(!system.hasCti());
  const auto default_ctis = onegen::defaults::pylonCtis();
  assert(std::adjacent_find(default_ctis.begin(), default_ctis.end()) == default_ctis.end());
  const auto producer_ctis = onegen::defaults::producerCtis();
  assert(std::is_sorted(producer_ctis.begin(), producer_ctis.end()));
  assert(std::adjacent_find(producer_ctis.begin(), producer_ctis.end()) == producer_ctis.end());

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
  assert(std::find(environment_ctis.begin(), environment_ctis.end(),
                   std::filesystem::weakly_canonical(fake_cti)) != environment_ctis.end());
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
  a.id = "Basler-123";
  a.user_defined_name = "LineCam";
  a.ip = "192.168.0.157";
  a.serial = "123";
  a.display_name = "acA1920 (192.168.0.157)";
  a.model = "acA1920-40gm";
  ds.push_back(a);

  DeviceInfo b = a;
  b.id = "Other-9";
  b.user_defined_name = "Other";
  b.ip = "192.168.0.10";
  b.serial = "9";
  b.display_name = "other";
  b.model = "other-model";
  ds.push_back(b);

  assert(match_device(ds, "LineCam").id == "Basler-123");
  assert(match_device(ds, "192.168.0.157").id == "Basler-123");
  assert(match_device(ds, "123").id == "Basler-123");
  assert(match_device(ds, "acA1920-40gm").id == "Basler-123");
  assert(match_device(ds, "Basler-123").id == "Basler-123");
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
  mac.display_name = "acA1920-40gm";
  assert(match_device({mac}, "192.168.0.157").id == "003053AABBCC");

  threw = false;
  try {
    match_device({}, "192.168.0.157");
  } catch (const onegen::Error& e) {
    threw = std::string(e.what()).find("none enumerated") != std::string::npos;
  }
  assert(threw);

  std::cout << "test_open_query ok\n";
  return 0;
}
