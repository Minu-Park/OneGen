#include "nodemap.hpp"
#include "onegen/error.hpp"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>

static const char kXml[] = R"XML(<?xml version="1.0" encoding="utf-8"?>
<RegisterDescription
    ModelName="TestModel"
    VendorName="OneGen"
    StandardNameSpace="None"
    SchemaMajorVersion="1"
    SchemaMinorVersion="1"
    SchemaSubMinorVersion="0"
    MajorVersion="1"
    MinorVersion="0"
    SubMinorVersion="0"
    ProductGuid="2D932CC6-EB68-4df9-8D9D-7D2DA4A0D108"
    VersionGuid="02A80B24-C52C-45f6-B295-72B2167D71A9"
    xmlns="http://www.genicam.org/GenApi/Version_1_1">
  <Category Name="Root">
    <pFeature>Width</pFeature>
    <pFeature>Gain</pFeature>
    <pFeature>ReverseX</pFeature>
    <pFeature>PixelFormat</pFeature>
    <pFeature>DeviceUserID</pFeature>
    <pFeature>Start</pFeature>
  </Category>
  <Integer Name="Width">
    <Value>1920</Value>
    <Min>1</Min>
    <Max>4096</Max>
    <Inc>1</Inc>
  </Integer>
  <Float Name="Gain">
    <Value>1</Value>
    <Min>0</Min>
    <Max>100</Max>
  </Float>
  <Boolean Name="ReverseX">
    <Value>0</Value>
  </Boolean>
  <Enumeration Name="PixelFormat">
    <EnumEntry Name="Mono8">
      <Value>0</Value>
    </EnumEntry>
    <EnumEntry Name="BayerRG8">
      <Value>1</Value>
    </EnumEntry>
    <Value>0</Value>
  </Enumeration>
  <String Name="DeviceUserID">
    <Value>cam</Value>
  </String>
  <Integer Name="CmdVal">
    <Value>0</Value>
  </Integer>
  <Command Name="Start">
    <pValue>CmdVal</pValue>
    <CommandValue>1</CommandValue>
  </Command>
</RegisterDescription>
)XML";

static bool throws(const std::function<void()>& fn) {
  try {
    fn();
    return false;
  } catch (const onegen::Error&) {
    return true;
  }
}

int main() {
  onegen::NodeMap map;
  onegen::load_string_nodemap(map, kXml);

  auto width = map.find("Width");
  auto gain = map.find("Gain");
  auto rev = map.find("ReverseX");
  auto pf = map.find("PixelFormat");
  auto uid = map.find("DeviceUserID");
  auto start = map.find("Start");
  auto root = map.find("Root");
  assert(width.valid() && gain.valid() && rev.valid() && pf.valid());
  assert(uid.valid() && start.valid() && root.valid());
  assert(width.kind() == onegen::NodeKind::Integer);
  assert(gain.kind() == onegen::NodeKind::Float);
  assert(rev.kind() == onegen::NodeKind::Boolean);
  assert(pf.kind() == onegen::NodeKind::Enumeration);
  assert(start.kind() == onegen::NodeKind::Command);
  assert(root.kind() == onegen::NodeKind::Category);

  width.set(100);
  assert(width.get_int() == 100);
  width.set(10.9);
  assert(width.get_int() == 10);

  gain.set(12);
  assert(gain.get_float() == 12.0);
  gain.set(3.5);
  assert(gain.get_float() == 3.5);

  rev.set(true);
  assert(rev.get_bool());
  rev.set(0);
  assert(!rev.get_bool());
  rev.set("1");
  assert(rev.get_bool());

  pf.set("BayerRG8");
  assert(pf.get_enum() == "BayerRG8");
  pf.set("Mono8");
  assert(pf.get() == "Mono8");

  uid.set("hello");
  assert(uid.get_string() == "hello");

  assert(throws([&] { width.set(true); }));
  assert(throws([&] { pf.set(true); }));
  assert(throws([&] { start.set(1); }));
  assert(throws([&] { root.set("x"); }));
  assert(throws([&] { width.set(99999); }));
  start.execute();

  std::cout << "test_node ok\n";
  return 0;
}
