#include "match.hpp"
#include "onegen/error.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using onegen::DeviceInfo;
  using onegen::match_device;

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
