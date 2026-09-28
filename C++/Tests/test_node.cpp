#include "Internal/NodeMap.h"
#include "Internal/Scan3dGeometry.h"
#include "OneGenError.h"

#include <cassert>
#include <memory>
#include <functional>
#include <iostream>
#include <string>
#include <cstring>
#include <limits>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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
    <Streamable>Yes</Streamable>
    <Value>1920</Value>
    <Min>1</Min>
    <Max>4096</Max>
    <Inc>1</Inc>
  </Integer>
  <Integer Name="Wide">
    <Value>9223372036854775806</Value>
    <Min>-9223372036854775808</Min>
    <Max>9223372036854775807</Max>
  </Integer>
  <Integer Name="WriteOnly">
    <ImposedAccessMode>WO</ImposedAccessMode>
    <Value>0</Value>
  </Integer>
  <Float Name="Gain">
    <Value>1</Value>
    <Min>0</Min>
    <Max>100</Max>
    <Inc>0.0000001</Inc>
    <Unit>dB</Unit>
    <Representation>Linear</Representation>
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
  <Integer Name="Locked"><Value>1</Value></Integer>
  <Command Name="Blocked">
    <pIsLocked>Locked</pIsLocked>
    <pValue>CmdVal</pValue>
    <CommandValue>1</CommandValue>
  </Command>
  <Register Name="Raw">
    <Address>4096</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Cachable>WriteThrough</Cachable>
    <PollingTime>100</PollingTime>
  </Register>
  <Port Name="Device"/>
  <Integer Name="EventValue">
    <pValue>EventRegister</pValue>
  </Integer>
  <IntReg Name="EventRegister">
    <Address>0</Address>
    <Length>4</Length>
    <AccessMode>RO</AccessMode>
    <pPort>EventPort</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="EventPort">
    <EventID>2A</EventID>
  </Port>
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

int main(int argc, char** argv) try {
  if (argc == 4 && std::string_view(argv[1]) == "--read-only-cti") {
    onegen::Gentl g;
    g.load(argv[2]);
    struct Cleanup { onegen::Gentl& g; ~Cleanup() { g.unload(); } } cleanup{g};
    bool8_t changed = 0;
    g.check(g.TLUpdateInterfaceList(g.tl, &changed, 1000), "TLUpdateInterfaceList");
    std::uint32_t count = 0;
    g.check(g.TLGetNumInterfaces(g.tl, &count), "TLGetNumInterfaces");
    for (std::uint32_t index = 0; index < count; ++index) {
      char id[65536]{};
      std::size_t size = sizeof(id);
      g.check(g.TLGetInterfaceID(g.tl, index, id, &size), "TLGetInterfaceID");
      GenTL::IF_HANDLE interface = nullptr;
      g.check(g.TLOpenInterface(g.tl, id, &interface), "TLOpenInterface");
      struct InterfaceCleanup { onegen::Gentl& g; GenTL::IF_HANDLE h; ~InterfaceCleanup() { g.IFClose(h); } } ic{g, interface};
      g.check(g.IFUpdateDeviceList(interface, &changed, 1000), "IFUpdateDeviceList");
      GenTL::DEV_HANDLE device = nullptr;
      if (g.IFOpenDevice(interface, argv[3], GenTL::DEVICE_ACCESS_READONLY, &device) != GenTL::GC_ERR_SUCCESS) continue;
      struct DeviceCleanup { onegen::Gentl& g; GenTL::DEV_HANDLE h; ~DeviceCleanup() { g.DevClose(h); } } dc{g, device};
      GenTL::PORT_HANDLE port = nullptr;
      g.check(g.DevGetPort(device, &port), "DevGetPort");
      onegen::NodeMap map;
      onegen::load_remote_nodemap(map, g, port);
      auto payload = map.find("PayloadSize");
      assert(payload.valid() && payload.is_readable());
      const auto bytes = payload.get_int();
      assert(bytes > 0);
      std::cout << "Physical device XML loaded; PayloadSize=" << bytes << " bytes (READONLY)\n";
      return 0;
    }
    throw onegen::Error("No matching device could be opened READONLY");
  }
  // A missing XML URL reports its module and preserves the original failure.
  onegen::Gentl producer;
  producer.GCGetNumPortURLs = [](GenTL::PORT_HANDLE, std::uint32_t* count) -> GenTL::GC_ERROR {
    *count = 0;
    return GenTL::GC_ERR_SUCCESS;
  };
  onegen::NodeMap missing;
  std::string diagnostic;
  assert(throws([&] {
    onegen::load_remote_nodemap(missing, producer, nullptr, "Interface",
        [&](std::string_view message) { diagnostic = message; });
  }));
  assert(diagnostic == "Interface XML/node map loading failed: device port has no XML URL");
  assert(missing.empty());
  assert(throws([&] {
    onegen::load_remote_nodemap(missing, producer, nullptr, "Device",
        [](std::string_view) { throw std::runtime_error("diagnostic callback failure"); });
  }));

  onegen::NodeMap map;
  onegen::load_string_nodemap(map, kXml);
  map.poll(250);
  map.deliver_event({0x78, 0x56, 0x34, 0x12}, 42);
  assert(map.find("EventValue").get_int() == 0x12345678);
  map.deliver_event({0x21, 0, 0, 0}, "2A");
  assert(map.find("EventValue").get_int() == 0x21);
  assert(throws([&] { map.poll(-1); }));

  auto width = map.find("Width");
  auto wide = map.find("Wide");
  onegen::apply_feature(wide, "int", "9223372036854775807");
  assert(wide.get_int() == INT64_MAX);
  onegen::apply_feature(wide, "int", "-9223372036854775808");
  assert(wide.get_int() == INT64_MIN);
  assert(throws([&] { onegen::apply_feature(wide, "int", "9223372036854775808"); }));
  assert(throws([&] { onegen::apply_feature(wide, "int", "1junk"); }));
  const auto write_only = onegen::snapshot(map.find("WriteOnly"));
  assert(write_only.writable && !write_only.readable);
  onegen::apply_feature(map.find("WriteOnly"), "int", "3");
  auto gain = map.find("Gain");
  const auto gain_metadata = onegen::snapshot(gain);
  assert(gain_metadata.unit == "dB" && gain_metadata.representation == "Linear");
  assert(gain_metadata.float_inc == 0.0000001);
  assert(!onegen::snapshot(map.find("Blocked")).writable);
  onegen::apply_feature(gain, "float", "1.2345678901234567");
  assert(gain.get_float() == 1.2345678901234567);
  assert(throws([&] { onegen::apply_feature(gain, "float", "nan"); }));
  assert(throws([&] { onegen::apply_feature(gain, "float", "1.2junk"); }));
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
  assert(pf.get_enum_value() == 1);
  pf.set("Mono8");
  assert(pf.value_text() == "Mono8");

  uid.set("hello");
  assert(uid.get_string() == "hello");

  assert(throws([&] { width.set(true); }));
  assert(throws([&] { pf.set(true); }));
  assert(throws([&] { start.set(1); }));
  assert(throws([&] { root.set("x"); }));
  assert(throws([&] { width.set(99999); }));
  start.execute();

  // The second, variable-length XML URL succeeds after the first URL fails.
  struct Port {
    bool fail_read = false;
    bool empty_read = false;
    bool short_read = false;
    std::uint8_t raw[4]{1, 2, 3, 4};
    std::string first = "invalid:unavailable";
    std::string second;
  } port;
  std::ostringstream length;
  length << std::hex << std::strlen(kXml);
  port.second = "local:" + std::string(2000, 'x') + ".xml;0;" + length.str();
  producer.GCGetNumPortURLs = [](GenTL::PORT_HANDLE, std::uint32_t* count) -> GenTL::GC_ERROR {
    *count = 2;
    return GenTL::GC_ERR_SUCCESS;
  };
  producer.GCGetPortURLInfo = [](GenTL::PORT_HANDLE handle, std::uint32_t index, GenTL::URL_INFO_CMD,
      GenTL::INFO_DATATYPE* type, void* destination, std::size_t* size) -> GenTL::GC_ERROR {
    auto& port = *static_cast<Port*>(handle);
    const auto& url = index == 0 ? port.first : port.second;
    *type = GenTL::INFO_DATATYPE_STRING;
    const auto required = url.size() + 1;
    if (!destination || *size < required) { *size = required; return GenTL::GC_ERR_BUFFER_TOO_SMALL; }
    std::memcpy(destination, url.c_str(), required);
    *size = required;
    return GenTL::GC_ERR_SUCCESS;
  };
  producer.GCGetPortInfo = [](GenTL::PORT_HANDLE, GenTL::PORT_INFO_CMD,
      GenTL::INFO_DATATYPE* type, void* destination, std::size_t* size) -> GenTL::GC_ERROR {
    *type = GenTL::INFO_DATATYPE_STRING;
    if (!destination || *size < 7) { *size = 7; return GenTL::GC_ERR_BUFFER_TOO_SMALL; }
    std::memcpy(destination, "Device", 7);
    *size = 7;
    return GenTL::GC_ERR_SUCCESS;
  };
  static unsigned last_error_calls = 0;
  producer.GCGetLastError = [](GenTL::GC_ERROR* code, char* text,
      std::size_t* size) -> GenTL::GC_ERROR {
    constexpr char message[] = "synthetic invalid handle";
    ++last_error_calls;
    *code = static_cast<GenTL::GC_ERROR>(GenTL::GC_ERR_INVALID_HANDLE);
    if (*size < sizeof(message)) {
      *size = sizeof(message);
      return GenTL::GC_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(text, message, sizeof(message));
    *size = sizeof(message);
    return GenTL::GC_ERR_SUCCESS;
  };
  producer.GCReadPort = [](GenTL::PORT_HANDLE handle, std::uint64_t address,
      void* bytes, std::size_t* size) -> GenTL::GC_ERROR {
    auto& port = *static_cast<Port*>(handle);
    if (address == 0 && *size == std::strlen(kXml)) {
      std::memcpy(bytes, kXml, *size);
      if (port.short_read) --*size;
      return GenTL::GC_ERR_SUCCESS;
    }
    if (address != 4096 || *size != 4) return GenTL::GC_ERR_INVALID_PARAMETER;
    if (port.fail_read) return GenTL::GC_ERR_INVALID_HANDLE;
    if (port.empty_read) { *size = 0; return GenTL::GC_ERR_SUCCESS; }
    std::memcpy(bytes, port.raw, 4);
    return GenTL::GC_ERR_SUCCESS;
  };
  producer.GCWritePort = [](GenTL::PORT_HANDLE handle, std::uint64_t address,
      const void* bytes, std::size_t* size) -> GenTL::GC_ERROR {
    if (address != 4096 || *size != 4) return GenTL::GC_ERR_INVALID_PARAMETER;
    std::memcpy(static_cast<Port*>(handle)->raw, bytes, 4);
    return GenTL::GC_ERR_SUCCESS;
  };
  onegen::NodeMap connected;
  onegen::load_remote_nodemap(connected, producer, &port);
  auto raw = connected.find("Raw");
  assert(raw.kind() == onegen::NodeKind::Register);
  assert((raw.read_register() == std::vector<std::uint8_t>{1, 2, 3, 4}));
  // Capture the real diagnostic and restore stderr before checking its contents.
#ifdef _WIN32
  const auto stderr_fd = _fileno(stderr);
  const auto saved_stderr = _dup(stderr_fd);
#else
  const auto stderr_fd = fileno(stderr);
  const auto saved_stderr = dup(stderr_fd);
#endif
  auto* diagnostic_file = std::tmpfile();
  assert(saved_stderr >= 0 && diagnostic_file);
  std::fflush(stderr);
#ifdef _WIN32
  assert(_dup2(_fileno(diagnostic_file), stderr_fd) == 0);
#else
  assert(dup2(fileno(diagnostic_file), stderr_fd) >= 0);
#endif
  port.fail_read = true;
  connected.invalidate("Raw");
  assert(throws([&] { raw.read_register(); }));
  assert(last_error_calls == 1);
  port.fail_read = false;
  port.empty_read = true;
  connected.invalidate("Raw");
  assert(throws([&] { raw.read_register(); }));
  assert(last_error_calls == 1);
  port.empty_read = false;
  std::fflush(stderr);
#ifdef _WIN32
  assert(_dup2(saved_stderr, stderr_fd) == 0);
  _close(saved_stderr);
#else
  assert(dup2(saved_stderr, stderr_fd) >= 0);
  close(saved_stderr);
#endif
  std::rewind(diagnostic_file);
  std::string log;
  char chunk[1024];
  while (const auto count = std::fread(chunk, 1, sizeof(chunk), diagnostic_file))
    log.append(chunk, count);
  std::fclose(diagnostic_file);
  assert(log.find("read_seq=2 since_last_success_ms=") != std::string::npos);
  assert(log.find("producer_error=\"synthetic invalid handle\"") != std::string::npos);
  assert(log.find("read_seq=3 since_last_success_ms=") != std::string::npos);
  assert(log.find("producer_error=\"\"") != std::string::npos);
  assert(throws([&] { raw.write_register({1, 2}); }));
  raw.write_register({5, 6, 7, 8});
  assert((raw.read_register() == std::vector<std::uint8_t>{5, 6, 7, 8}));
  connected.invalidate("Raw");
  assert(throws([&] { connected.invalidate("Missing"); }));
  connected.poll(250);
  port.raw[0] = 9;
  connected.poll(1000);
  assert(raw.read_register().front() == 9);
  auto connected_width = connected.find("Width");
  connected_width.set(123);
  const auto bag = connected.save_features();
  connected_width.set(456);
  connected.load_features(bag);
  assert(connected_width.get_int() == 123);
  assert(throws([&] { connected.load_features("not a feature bag"); }));
  port.short_read = true;
  onegen::NodeMap short_xml;
  assert(throws([&] { onegen::load_remote_nodemap(short_xml, producer, &port); }));
  port.short_read = false;
  // Preserve producer URL delimiters and padding without accepting numeric junk.
  for (const auto& suffix : {std::string(";"), std::string(";1.1;vendor-extension"),
                             std::string("?SchemaVersion=1.0.0")}) {
    port.second = "Local:///camera.xml; \t0X0 ; 0x" + length.str() + " \r\n" + suffix;
    onegen::NodeMap padded_xml;
    onegen::load_remote_nodemap(padded_xml, producer, &port);
    assert(padded_xml.find("Width").valid());
  }
  port.second = "local:camera.xml;0;" + length.str() + "junk;";
  assert(throws([&] { onegen::load_remote_nodemap(short_xml, producer, &port); }));
  port.second = "local:huge.xml;0;4000000";
  assert(throws([&] { onegen::load_remote_nodemap(short_xml, producer, &port); }));
  const auto file = std::filesystem::temp_directory_path() /
      ("onegen xml " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".xml");
  { std::ofstream output(file, std::ios::binary); output << kXml; }
  auto file_path = file.generic_string();
  for (std::size_t position = 0; (position = file_path.find(' ', position)) != std::string::npos; position += 3)
    file_path.replace(position, 1, "%20");
#ifdef _WIN32
  port.second = "file:///" + file_path;
#else
  port.second = "file://" + file_path;
#endif
  onegen::load_remote_nodemap(short_xml, producer, &port);
  assert(short_xml.find("Width").valid());
  std::filesystem::remove(file);

  // Real GenApi XML exercises missing/read-only/C-only selector calibration.
  const auto geometryMap = [](const std::string& mode, const std::string& selector,
                              const std::string& extra = "") {
    const std::string xml = R"(<RegisterDescription ModelName="Scan3dTest" VendorName="OneGen" StandardNameSpace="None" SchemaMajorVersion="1" SchemaMinorVersion="1" SchemaSubMinorVersion="0" MajorVersion="1" MinorVersion="0" SubMinorVersion="0" ProductGuid="B6B08EB8-B16D-4C62-BDA7-3A9698C9B27C" VersionGuid="B6B08EB8-B16D-4C62-BDA7-3A9698C9B27C" xmlns="http://www.genicam.org/GenApi/Version_1_1">
      <Category Name="Root"/>
      <String Name="Scan3dOutputMode"><Value>)" + mode + R"(</Value></String>
      <String Name="Scan3dDistanceUnit"><Value>Millimeter</Value></String>
      <Float Name="Scan3dCoordinateScale"><Value>0.5</Value></Float>
      <Float Name="Scan3dCoordinateOffset"><Value>-1</Value></Float>)" + selector + extra + "</RegisterDescription>";
    auto nodes = std::make_unique<onegen::NodeMap>();
    onegen::load_string_nodemap(*nodes, xml.c_str());
    return nodes;
  };
  auto scalar = geometryMap("CalibratedC", "");
  const auto cGeometry = onegen::read_scan3d_geometry(*scalar, "");
  assert(cGeometry && cGeometry->scale[2] == 0.5 && cGeometry->offset[2] == -1);
  assert(cGeometry->calibrated_axes == (std::array<bool, 3>{false, false, true}));
  auto ambiguous = geometryMap("RectifiedC", "");
  assert(!onegen::read_scan3d_geometry(*ambiguous, ""));
  const std::string cSelector = R"(<Enumeration Name="Scan3dCoordinateSelector"><EnumEntry Name="CoordinateC"><Value>2</Value></EnumEntry><Value>2</Value></Enumeration>)";
  auto onlyC = geometryMap("CalibratedC", cSelector);
  assert(onegen::read_scan3d_geometry(*onlyC, "")->calibrated_axes[2]);
  auto readOnlyC = geometryMap("CalibratedC", std::string(R"(<Enumeration Name="Scan3dCoordinateSelector"><ImposedAccessMode>RO</ImposedAccessMode><EnumEntry Name="CoordinateC"><Value>2</Value></EnumEntry><Value>2</Value></Enumeration>)"));
  assert(onegen::read_scan3d_geometry(*readOnlyC, "")->calibrated_axes[2]);
  const std::string xyzSelector = R"(<Enumeration Name="Scan3dCoordinateSelector"><EnumEntry Name="CoordinateA"><Value>0</Value></EnumEntry><EnumEntry Name="CoordinateB"><Value>1</Value></EnumEntry><EnumEntry Name="CoordinateC"><Value>2</Value></EnumEntry><Value>1</Value></Enumeration>)";
  auto rectified = geometryMap("RectifiedC", xyzSelector);
  const auto rectifiedGeometry = onegen::read_scan3d_geometry(*rectified, "");
  assert(rectifiedGeometry && rectifiedGeometry->calibrated_axes == (std::array<bool, 3>{true, true, true}));
  assert(rectified->find("Scan3dCoordinateSelector").get_enum() == "CoordinateB");
  auto missingInvalid = geometryMap("CalibratedC", "", R"(<Boolean Name="Scan3dInvalidDataFlag"><Value>1</Value></Boolean>)");
  assert(!onegen::read_scan3d_geometry(*missingInvalid, ""));
  auto nativeGeometryMap = geometryMap("UncalibratedC", "");
  assert(onegen::read_scan3d_geometry(*nativeGeometryMap, "")->output_mode == "UncalibratedC");
  auto disparityMap = geometryMap("DisparityC", "",
      R"(<Float Name="Scan3dFocalLength"><Value>12</Value></Float><Float Name="Scan3dBaseline"><Value>80</Value></Float>)");
  const auto disparityGeometry = onegen::read_scan3d_geometry(*disparityMap, "");
  assert(disparityGeometry && disparityGeometry->focal_length == 12 && disparityGeometry->baseline == 80);
  assert(disparityGeometry->calibrated_axes[2]);
  auto rectifiedAcMap = geometryMap("RectifiedAC", xyzSelector);
  const auto rectifiedAcGeometry = onegen::read_scan3d_geometry(*rectifiedAcMap, "");
  assert(rectifiedAcGeometry && rectifiedAcGeometry->output_mode == "RectifiedAC");
  assert(rectifiedAcGeometry->calibrated_axes == (std::array<bool, 3>{true, true, true}));

  std::string nativeOnlyXml = kXml;
  nativeOnlyXml.insert(nativeOnlyXml.find("</RegisterDescription>"),
      R"(<String Name="Scan3dOutputMode"><Value>UncalibratedC</Value></String>)");
  onegen::NodeMap nativeOnly;
  onegen::load_string_nodemap(nativeOnly, nativeOnlyXml.c_str());
  const auto nativeOnlyGeometry = onegen::read_scan3d_geometry(nativeOnly, "");
  assert(nativeOnlyGeometry && nativeOnlyGeometry->calibrated_axes == (std::array<bool, 3>{false, false, false}));

  std::string chunkXml = kXml;
  chunkXml.insert(chunkXml.find("</RegisterDescription>"),
      R"(<Float Name="ChunkScan3dCoordinateScale"><Value>2</Value></Float><Float Name="ChunkScan3dCoordinateOffset"><Value>7</Value></Float>)");
  onegen::NodeMap chunkNodes;
  onegen::load_string_nodemap(chunkNodes, chunkXml.c_str());
  auto baseline = *cGeometry;
  baseline.invalid_enabled[2] = true; baseline.invalid_value[2] = 0;
  const auto chunkGeometry = onegen::read_scan3d_geometry(chunkNodes, "Chunk", &baseline);
  assert(chunkGeometry && chunkGeometry->scale[2] == 2 && chunkGeometry->offset[2] == 7);
  assert(chunkGeometry->invalid_enabled[2] && chunkGeometry->invalid_value[2] == 0);
  assert(chunkGeometry->distance_unit == "Millimeter" && chunkGeometry->output_mode == "CalibratedC");
  auto stereoBaseline = *disparityGeometry;
  const auto stereoChunk = onegen::read_scan3d_geometry(chunkNodes, "Chunk", &stereoBaseline);
  assert(stereoChunk && stereoChunk->focal_length == 12 && stereoChunk->baseline == 80);
  assert(stereoChunk->scale[2] == 2);
  baseline.output_mode = "RectifiedC";
  assert(!onegen::read_scan3d_geometry(chunkNodes, "Chunk", &baseline));

  std::cout << "test_node ok\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
