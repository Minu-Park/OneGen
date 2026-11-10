#include "Internal/Gentl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace {

using namespace GenTL;

enum class Scenario {
  Default,
  M0,
  M1,
  M2,
  M3,
  M4,
  M5,
  M6,
  M7,
  M8,
  M9,
  M10,
  M12,
  M13,
  M14,
};

Scenario loadScenario() {
  const char* value = std::getenv("ONEGEN_SYNTHETIC_SCENARIO");
  if (!value) return Scenario::Default;
  const std::string_view name(value);
  if (name == "M0") return Scenario::M0;
  if (name == "M1") return Scenario::M1;
  if (name == "M2") return Scenario::M2;
  if (name == "M3") return Scenario::M3;
  if (name == "M4") return Scenario::M4;
  if (name == "M5") return Scenario::M5;
  if (name == "M6") return Scenario::M6;
  if (name == "M7") return Scenario::M7;
  if (name == "M8") return Scenario::M8;
  if (name == "M9") return Scenario::M9;
  if (name == "M10") return Scenario::M10;
  if (name == "M12") return Scenario::M12;
  if (name == "M13") return Scenario::M13;
  if (name == "M14") return Scenario::M14;
  return Scenario::Default;
}

const Scenario g_scenario = loadScenario();

struct FormatOverride {
  bool enabled = false;
  std::uint64_t format = 0;
  std::uint32_t width = 64;
  std::uint32_t height = 4;
  std::size_t x_padding = 0;
  std::size_t image_offset = 0;
  std::size_t trailing = 0;
  std::int32_t endianness = 0;
};

std::uint64_t parseUnsigned(const char* value, std::uint64_t fallback) {
  if (!value || !*value) return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 0);
  return end == value ? fallback : static_cast<std::uint64_t>(parsed);
}

FormatOverride loadFormatOverride() {
  FormatOverride result;
  const char* format = std::getenv("ONEGEN_SYNTHETIC_FORMAT");
  if (!format) return result;
  result.enabled = true;
  result.format = parseUnsigned(format, 0x01080001);
  result.width = static_cast<std::uint32_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_WIDTH"), result.width));
  result.height = static_cast<std::uint32_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_HEIGHT"), result.height));
  result.x_padding = static_cast<std::size_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_XPAD"), result.x_padding));
  result.image_offset = static_cast<std::size_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_OFFSET"), result.image_offset));
  result.trailing = static_cast<std::size_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_TRAILING"), result.trailing));
  result.endianness = static_cast<std::int32_t>(parseUnsigned(
      std::getenv("ONEGEN_SYNTHETIC_ENDIAN"), static_cast<std::uint64_t>(result.endianness)));
  return result;
}

const FormatOverride g_format_override = loadFormatOverride();

constexpr std::uint64_t mono8 = 0x01080001;
constexpr std::uint64_t bayer_rg8 = 0x01080009;
constexpr std::uint64_t mono10 = 0x01100003;

constexpr std::uint64_t width_register = 0x1000;
constexpr std::uint64_t height_register = 0x1004;
constexpr std::uint64_t pixel_format_register = 0x1008;
constexpr std::uint64_t component_selector_register = 0x1010;
constexpr std::uint64_t component_id_register = 0x1014;
constexpr std::uint64_t payload_size_register = 0x1018;
constexpr std::uint64_t trigger_register = 0x1100;
constexpr std::uint64_t acquisition_start_register = 0x1104;
constexpr std::uint64_t acquisition_stop_register = 0x1108;

struct ChunkData {
  std::uint64_t id = 0;
  std::size_t offset = 0;
  std::size_t length = 0;
};

struct PartData {
  std::size_t offset = 0;
  std::size_t size = 0;
  std::uint64_t data_format = mono8;
  std::uint64_t format_namespace = 0;
  std::uint64_t source_id = 10;
  std::uint64_t region_id = 20;
  std::uint64_t purpose_id = 30;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct BufferData {
  std::vector<std::uint8_t> storage;
  void* user = nullptr;
  bool queued = false;
  bool ready = false;
  bool acquiring = false;
  bool revoked = false;

  std::size_t capacity = 0;
  std::size_t size_filled = 0;
  std::size_t data_size = 0;
  std::size_t image_offset = 0;
  std::size_t x_padding = 0;
  std::size_t y_padding = 0;
  std::size_t delivered_height = 0;
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t pixel_format = mono8;
  std::uint64_t payload_type = PAYLOAD_TYPE_IMAGE;
  std::uint64_t format_namespace = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t timestamp = 0;
  std::uint64_t timestamp_ns = 0;
  std::int32_t pixel_endianness = 0;
  bool incomplete = false;
  bool image_present = true;
  bool contains_chunk = false;
  bool data_larger_than_buffer = false;
  std::vector<ChunkData> chunks;
  std::vector<PartData> parts;
};

struct EventData {
  EVENTSRC_HANDLE source = nullptr;
  EVENT_TYPE type = EVENT_NEW_BUFFER;
  bool killed = false;
};

struct StreamData {
  std::string id = "stream0";
  bool grabbing = false;
  bool finite = false;
  std::uint64_t remaining = 0;
  std::uint64_t frame_counter = 0;
  std::uint64_t trigger_pending = 0;
  std::uint64_t delivered = 0;
  std::uint64_t underrun = 0;
  std::uint64_t started = 0;
  std::uint64_t announced = 0;
  std::vector<std::unique_ptr<BufferData>> buffers;
  std::deque<BufferData*> input;
  EventData* new_buffer_event = nullptr;
};

struct DeviceData {
  bool open = false;
  StreamData stream;
};

struct TlHandle {
  int tag = 0;
};
struct IfHandle {
  int tag = 0;
};
struct DevHandle {
  int tag = 0;
};
struct DsHandle {
  int tag = 0;
};
struct PortHandle {
  int tag = 0;
};

std::mutex g_mutex;
std::condition_variable g_condition;
DeviceData g_device;
std::string g_xml;
std::uint32_t g_component_selector = 0;
std::vector<EventData*> g_events;
GC_ERROR g_last_error = GC_ERR_SUCCESS;
std::string g_last_error_text;

void setLastError(GC_ERROR code, std::string_view text) {
  g_last_error = code;
  g_last_error_text.assign(text);
}

bool valid(void* handle) {
  return handle != nullptr;
}

bool8_t b8(bool value) {
  return value ? bool8_t{1} : bool8_t{0};
}

template <class T>
GC_ERROR give(INFO_DATATYPE* type, void* buffer, std::size_t* size,
              INFO_DATATYPE data_type, const T& value) {
  if (type) *type = data_type;
  if (!size) return GC_ERR_INVALID_PARAMETER;
  if (!buffer) {
    *size = sizeof(T);
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  if (*size < sizeof(T)) {
    *size = sizeof(T);
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(buffer, &value, sizeof(T));
  *size = sizeof(T);
  return GC_ERR_SUCCESS;
}

GC_ERROR giveString(INFO_DATATYPE* type, void* buffer, std::size_t* size,
                    std::string_view value) {
  if (type) *type = INFO_DATATYPE_STRING;
  if (!size) return GC_ERR_INVALID_PARAMETER;
  const auto required = value.size() + 1;
  if (!buffer) {
    *size = required;
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  if (*size < required) {
    *size = required;
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  std::memcpy(buffer, value.data(), value.size());
  static_cast<char*>(buffer)[value.size()] = '\0';
  *size = required;
  return GC_ERR_SUCCESS;
}

std::string hex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}

std::string makeXml() {
  std::ostringstream out;
  out << R"XML(<?xml version="1.0" encoding="utf-8"?>
<RegisterDescription
    ModelName="SyntheticGenTL"
    VendorName="OneGen"
    StandardNameSpace="None"
    SchemaMajorVersion="1"
    SchemaMinorVersion="1"
    SchemaSubMinorVersion="0"
    MajorVersion="1"
    MinorVersion="0"
    SubMinorVersion="0"
    ProductGuid="7C3C3C6E-5B5A-4D7D-9C55-2B1D7D9A3F10"
    VersionGuid="BD5E8E76-51B7-4A57-8A54-3E1F5C3F5A22"
    xmlns="http://www.genicam.org/GenApi/Version_1_1">
  <Category Name="Root">
    <pFeature>Width</pFeature>
    <pFeature>Height</pFeature>
    <pFeature>PixelFormat</pFeature>
    <pFeature>TriggerSoftware</pFeature>
    <pFeature>ComponentSelector</pFeature>
    <pFeature>ComponentIDValue</pFeature>
  </Category>
  <IntReg Name="WidthReg">
    <Address>4096</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Integer Name="Width">
    <pValue>WidthReg</pValue>
    <Min>1</Min>
    <Max>16777215</Max>
    <Inc>1</Inc>
  </Integer>
  <IntReg Name="HeightReg">
    <Address>4100</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Integer Name="Height">
    <pValue>HeightReg</pValue>
    <Min>1</Min>
    <Max>16777215</Max>
    <Inc>1</Inc>
  </Integer>
  <IntReg Name="PixelFormatReg">
    <Address>4104</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Enumeration Name="PixelFormat">
    <EnumEntry Name="Mono8"><Value>17301505</Value></EnumEntry>
    <EnumEntry Name="BayerRG8"><Value>17301513</Value></EnumEntry>
    <EnumEntry Name="Mono10"><Value>17825795</Value></EnumEntry>
    <pValue>PixelFormatReg</pValue>
  </Enumeration>
  <IntReg Name="ComponentSelectorReg">
    <Address>4112</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Enumeration Name="ComponentSelector">
    <EnumEntry Name="Intensity"><Value>0</Value></EnumEntry>
    <EnumEntry Name="Depth"><Value>1</Value></EnumEntry>
)XML";
  if (g_scenario == Scenario::M10) {
    out << "    <EnumEntry Name=\"HugeComponent_";
    out << std::string(5000, 'H');
    out << "\"><Value>2</Value></EnumEntry>\n";
  }
  out << R"XML(    <pValue>ComponentSelectorReg</pValue>
  </Enumeration>
  <IntReg Name="ComponentIDValueReg">
    <Address>4116</Address>
    <Length>4</Length>
    <AccessMode>RO</AccessMode>
    <pPort>Device</pPort>
    <Sign>Signed</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Integer Name="ComponentIDValue">
    <pValue>ComponentIDValueReg</pValue>
  </Integer>
  <IntReg Name="TriggerSoftwareReg">
    <Address>4352</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Command Name="TriggerSoftware">
    <pValue>TriggerSoftwareReg</pValue>
    <CommandValue>1</CommandValue>
  </Command>
  <IntReg Name="AcquisitionStartReg">
    <Address>4356</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Command Name="AcquisitionStart">
    <pValue>AcquisitionStartReg</pValue>
    <CommandValue>1</CommandValue>
  </Command>
  <IntReg Name="AcquisitionStopReg">
    <Address>4360</Address>
    <Length>4</Length>
    <AccessMode>RW</AccessMode>
    <pPort>Device</pPort>
    <Sign>Unsigned</Sign>
    <Endianess>LittleEndian</Endianess>
  </IntReg>
  <Command Name="AcquisitionStop">
    <pValue>AcquisitionStopReg</pValue>
    <CommandValue>1</CommandValue>
  </Command>
  <Port Name="Device"/>
</RegisterDescription>
)XML";
  return out.str();
}

void resetFrame(BufferData& buffer) {
  buffer.size_filled = 0;
  buffer.data_size = 0;
  buffer.image_offset = 0;
  buffer.x_padding = 0;
  buffer.y_padding = 0;
  buffer.delivered_height = 0;
  buffer.width = 0;
  buffer.height = 0;
  buffer.pixel_format = mono8;
  buffer.payload_type = PAYLOAD_TYPE_IMAGE;
  buffer.format_namespace = 0;
  buffer.pixel_endianness = 0;
  buffer.incomplete = false;
  buffer.image_present = true;
  buffer.contains_chunk = false;
  buffer.data_larger_than_buffer = false;
  buffer.chunks.clear();
  buffer.parts.clear();
  buffer.frame_id = 0;
  buffer.timestamp = 0;
  buffer.timestamp_ns = 0;
}

void fillPattern(BufferData& buffer, std::uint64_t frame) {
  for (std::size_t index = 0; index < buffer.storage.size(); ++index)
    buffer.storage[index] = static_cast<std::uint8_t>((frame * 37 + index * 13) & 0xff);
}

void fillFrame(BufferData& buffer, std::uint64_t frame) {
  resetFrame(buffer);
  buffer.frame_id = frame;
  buffer.timestamp = 1000 + frame * 7;
  buffer.timestamp_ns = 2000 + frame * 9;

  if (g_format_override.enabled) {
    const auto bits = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, (g_format_override.format >> 16U) & 0xffU));
    const auto planes = g_format_override.format == 0x02180021 ? 3U : 1U;
    const auto per_plane_bits = std::max(1U, bits / planes);
    const auto row_bytes = (static_cast<std::uint64_t>(g_format_override.width) * per_plane_bits + 7U) / 8U
        + g_format_override.x_padding;
    const auto filled = g_format_override.image_offset + row_bytes * g_format_override.height * planes
        + g_format_override.trailing;
    buffer.width = g_format_override.width;
    buffer.height = g_format_override.height;
    buffer.pixel_format = g_format_override.format;
    buffer.payload_type = PAYLOAD_TYPE_IMAGE;
    buffer.x_padding = g_format_override.x_padding;
    buffer.image_offset = g_format_override.image_offset;
    buffer.size_filled = filled;
    buffer.data_size = filled;
    buffer.capacity = filled + 64;
    buffer.pixel_endianness = g_format_override.endianness;
    buffer.storage.assign(buffer.capacity, 0);
    fillPattern(buffer, frame);
    return;
  }

  const auto base = [&](std::uint64_t width, std::uint64_t height,
                        std::uint64_t format, std::uint64_t payload,
                        std::size_t filled, std::size_t capacity) {
    buffer.width = width;
    buffer.height = height;
    buffer.pixel_format = format;
    buffer.payload_type = payload;
    buffer.size_filled = filled;
    buffer.data_size = filled;
    buffer.capacity = capacity;
    buffer.storage.assign(capacity, 0);
    fillPattern(buffer, frame);
  };

  switch (g_scenario) {
    case Scenario::M1:
      base(64, 4, bayer_rg8, PAYLOAD_TYPE_IMAGE, 268, 300);
      buffer.x_padding = 3;
      buffer.data_size = 0;
      break;
    case Scenario::M2:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 272, 300);
      buffer.image_offset = 16;
      buffer.y_padding = 2;
      break;
    case Scenario::M3:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      buffer.delivered_height = 0;
      break;
    case Scenario::M4:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      buffer.delivered_height = 4;
      break;
    case Scenario::M5:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      buffer.image_present = (frame % 2) != 0;
      break;
    case Scenario::M8:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      buffer.incomplete = true;
      buffer.delivered_height = 2;
      break;
    case Scenario::M12:
      if (frame % 2) base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      else base(64, 8, mono8, PAYLOAD_TYPE_IMAGE, 512, 512);
      break;
    case Scenario::M13:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      if (frame <= 2) buffer.image_offset = buffer.size_filled + 1;
      break;
    case Scenario::M14:
      if (frame == 1) {
        base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      } else {
        base(1, 1, mono8, PAYLOAD_TYPE_IMAGE, 1, 1);
        buffer.image_offset = buffer.size_filled + 1;
      }
      break;
    case Scenario::M9: {
      const auto variant = frame % 4;
      if (variant == 0) {
        base(64, 4, mono10, PAYLOAD_TYPE_IMAGE, 512, 512);
      } else if (variant == 1) {
        base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 264, 264);
        buffer.contains_chunk = true;
        buffer.chunks.push_back({17, 256, 4});
        buffer.chunks.push_back({18, 260, 4});
      } else if (variant == 2) {
        base(32, 4, mono8, PAYLOAD_TYPE_MULTI_PART, 256, 256);
        PartData first;
        first.offset = 0;
        first.size = 128;
        first.width = 32;
        first.height = 4;
        PartData second;
        second.offset = 128;
        second.size = 128;
        second.width = 32;
        second.height = 4;
        second.source_id = 11;
        second.region_id = 21;
        second.purpose_id = 31;
        buffer.parts.push_back(first);
        buffer.parts.push_back(second);
        buffer.image_present = false;
      } else {
        base(8, 8, mono8, PAYLOAD_TYPE_JPEG, 64, 64);
        buffer.image_present = false;
        buffer.pixel_endianness = 0;
      }
      break;
    }
    default:
      base(64, 4, mono8, PAYLOAD_TYPE_IMAGE, 256, 256);
      break;
  }
}

bool acquireReady(const StreamData& stream) {
  if (!stream.grabbing) return false;
  if (stream.finite && stream.remaining == 0) return false;
  return !stream.input.empty();
}

GC_ERROR takeBuffer(std::unique_lock<std::mutex>& lock, EventData* event,
                    void* data, std::size_t* size, std::uint64_t timeout) {
  if (!event) return GC_ERR_INVALID_HANDLE;
  if (event->type != EVENT_NEW_BUFFER) {
    if (size) *size = 0;
    return GC_ERR_TIMEOUT;
  }
  if (event->killed) return GC_ERR_ABORT;

  auto& stream = g_device.stream;
  const auto ready = [&] { return event->killed || acquireReady(stream); };
  const auto deadline = timeout == GENTL_INFINITE
      ? std::chrono::steady_clock::time_point::max()
      : std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
  while (!ready()) {
    if (timeout == 0) {
      if (size) *size = 0;
      return GC_ERR_TIMEOUT;
    }
    if (deadline == std::chrono::steady_clock::time_point::max()) {
      g_condition.wait(lock);
    } else if (g_condition.wait_until(lock, deadline) == std::cv_status::timeout) {
      break;
    }
  }
  if (event->killed) return GC_ERR_ABORT;
  if (!acquireReady(stream)) {
    if (size) *size = 0;
    return GC_ERR_TIMEOUT;
  }

  BufferData* buffer = stream.input.front();
  stream.input.pop_front();
  ++stream.frame_counter;
  fillFrame(*buffer, stream.frame_counter);
  if (stream.finite) --stream.remaining;
  ++stream.delivered;
  ++stream.started;
  buffer->queued = false;
  buffer->ready = true;

  EVENT_NEW_BUFFER_DATA event_data{};
  event_data.BufferHandle = buffer;
  event_data.pUserPointer = buffer->user;
  if (g_scenario == Scenario::M9) {
    // Pace the M9 format matrix for the one-frame non-direct transport credit:
    // the worker drops a fallback frame when the host has not read the prior one.
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return give(nullptr, data, size, INFO_DATATYPE_UNKNOWN, event_data);
}

GC_ERROR readRegister(std::uint64_t address, void* buffer, std::size_t* size) {
  if (!size) return GC_ERR_INVALID_PARAMETER;
  if (!buffer) {
    *size = 4;
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  if (*size < 4) {
    *size = 4;
    return GC_ERR_BUFFER_TOO_SMALL;
  }
  std::uint32_t value = 0;
  switch (address) {
    case width_register: value = 64; break;
    case height_register: value = 4; break;
    case pixel_format_register: value = static_cast<std::uint32_t>(mono8); break;
    case component_selector_register: value = g_component_selector; break;
    case component_id_register:
      value = g_component_selector == 0 ? 7 : (g_component_selector == 1 ? 9 : 11);
      break;
    case payload_size_register: value = 64 * 4; break;
    case trigger_register:
    case acquisition_start_register:
    case acquisition_stop_register:
      value = 0;
      break;
    default:
      return GC_ERR_INVALID_ADDRESS;
  }
  std::memcpy(buffer, &value, sizeof(value));
  *size = sizeof(value);
  return GC_ERR_SUCCESS;
}

GC_ERROR writeRegister(std::uint64_t address, const void* buffer, std::size_t* size) {
  if (!buffer || !size || *size != 4) return GC_ERR_INVALID_PARAMETER;
  std::uint32_t value = 0;
  std::memcpy(&value, buffer, sizeof(value));
  switch (address) {
    case component_selector_register:
      g_component_selector = value;
      return GC_ERR_SUCCESS;
    case trigger_register:
      if (g_device.stream.grabbing && !g_device.stream.finite) {
        ++g_device.stream.trigger_pending;
        g_condition.notify_all();
      }
      return GC_ERR_SUCCESS;
    case acquisition_start_register:
    case acquisition_stop_register:
      return GC_ERR_SUCCESS;
    case width_register:
    case height_register:
    case pixel_format_register:
      return GC_ERR_SUCCESS;
    default:
      return GC_ERR_INVALID_ADDRESS;
  }
}

std::string portUrl() {
  return "local:synthetic.xml;0;" + hex(g_xml.size());
}

}  // namespace

extern "C" {

TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetInfo(TL_INFO_CMD command, INFO_DATATYPE* type,
                                            void* buffer, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  switch (command) {
    case TL_INFO_ID: return giveString(type, buffer, size, "SyntheticGenTL");
    case TL_INFO_VENDOR: return giveString(type, buffer, size, "OneGen");
    case TL_INFO_MODEL: return giveString(type, buffer, size, "SyntheticGenTL");
    case TL_INFO_VERSION: return giveString(type, buffer, size, "1.0.0");
    case TL_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    case TL_INFO_NAME: return giveString(type, buffer, size, "SyntheticGenTL.cti");
    case TL_INFO_PATHNAME: return giveString(type, buffer, size, "SyntheticGenTL.cti");
    case TL_INFO_DISPLAYNAME: return giveString(type, buffer, size, "SyntheticGenTL");
    case TL_INFO_CHAR_ENCODING: return give(type, buffer, size, INFO_DATATYPE_INT32, std::int32_t{0});
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetLastError(GC_ERROR* code, char* text, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (code) *code = g_last_error;
  return giveString(nullptr, text, size, g_last_error_text);
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCInitLib(void) {
  std::lock_guard lock(g_mutex);
  g_xml = makeXml();
  g_last_error = GC_ERR_SUCCESS;
  g_last_error_text.clear();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCCloseLib(void) {
  std::lock_guard lock(g_mutex);
  for (auto* event : g_events) delete event;
  g_events.clear();
  g_device = DeviceData{};
  g_xml.clear();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE port, std::uint64_t address,
                                             void* buffer, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!valid(port)) return GC_ERR_INVALID_HANDLE;
  if (address == 0) {
    if (!size) return GC_ERR_INVALID_PARAMETER;
    const auto copied = std::min(*size, g_xml.size());
    if (!buffer) {
      *size = g_xml.size();
      return GC_ERR_BUFFER_TOO_SMALL;
    }
    if (*size < g_xml.size()) {
      *size = g_xml.size();
      return GC_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, g_xml.data(), copied);
    *size = copied;
    return GC_ERR_SUCCESS;
  }
  return readRegister(address, buffer, size);
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE port, std::uint64_t address,
                                              const void* buffer, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!valid(port)) return GC_ERR_INVALID_HANDLE;
  return writeRegister(address, buffer, size);
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE port, PORT_INFO_CMD command,
                                                INFO_DATATYPE* type, void* buffer,
                                                std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!valid(port)) return GC_ERR_INVALID_HANDLE;
  switch (command) {
    case PORT_INFO_ID: return giveString(type, buffer, size, "SyntheticPort");
    case PORT_INFO_VENDOR: return giveString(type, buffer, size, "OneGen");
    case PORT_INFO_MODEL: return giveString(type, buffer, size, "Synthetic");
    case PORT_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    case PORT_INFO_MODULE: return giveString(type, buffer, size, "Device");
    case PORT_INFO_PORTNAME: return giveString(type, buffer, size, "Device");
    case PORT_INFO_LITTLE_ENDIAN: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{1});
    case PORT_INFO_BIG_ENDIAN: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{0});
    case PORT_INFO_ACCESS_READ: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{1});
    case PORT_INFO_ACCESS_WRITE: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{1});
    case PORT_INFO_ACCESS_NA: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{0});
    case PORT_INFO_ACCESS_NI: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{0});
    case PORT_INFO_VERSION: return giveString(type, buffer, size, "1.0");
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE port, std::uint32_t* count) {
  std::lock_guard lock(g_mutex);
  if (!valid(port) || !count) return GC_ERR_INVALID_PARAMETER;
  *count = 1;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetPortURLInfo(PORT_HANDLE port, std::uint32_t index,
                                                   URL_INFO_CMD command, INFO_DATATYPE* type,
                                                   void* buffer, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!valid(port)) return GC_ERR_INVALID_HANDLE;
  if (index != 0) return GC_ERR_INVALID_INDEX;
  if (command != URL_INFO_URL) return GC_ERR_INVALID_PARAMETER;
  return giveString(type, buffer, size, portUrl());
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE source, EVENT_TYPE type,
                                                  EVENT_HANDLE* event) {
  std::lock_guard lock(g_mutex);
  if (!valid(source) || !event) return GC_ERR_INVALID_PARAMETER;
  for (auto* existing : g_events) {
    if (existing->source == source && existing->type == type) return GC_ERR_RESOURCE_IN_USE;
  }
  auto* created = new EventData;
  created->source = source;
  created->type = type;
  g_events.push_back(created);
  if (type == EVENT_NEW_BUFFER) {
    if (g_device.stream.new_buffer_event) {
      g_events.pop_back();
      delete created;
      return GC_ERR_RESOURCE_IN_USE;
    }
    g_device.stream.new_buffer_event = created;
  }
  *event = created;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE GCUnregisterEvent(EVENTSRC_HANDLE source, EVENT_TYPE type) {
  std::lock_guard lock(g_mutex);
  if (!valid(source)) return GC_ERR_INVALID_HANDLE;
  for (auto it = g_events.begin(); it != g_events.end(); ++it) {
    if ((*it)->source != source || (*it)->type != type) continue;
    if (g_device.stream.new_buffer_event == *it)
      g_device.stream.new_buffer_event = nullptr;
    delete *it;
    g_events.erase(it);
    g_condition.notify_all();
    return GC_ERR_SUCCESS;
  }
  return GC_ERR_INVALID_ID;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE event, void* buffer,
                                               std::size_t* size, std::uint64_t timeout) {
  std::unique_lock lock(g_mutex);
  return takeBuffer(lock, static_cast<EventData*>(event), buffer, size, timeout);
}

TEST_EXPORT GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE event) {
  std::lock_guard lock(g_mutex);
  auto* value = static_cast<EventData*>(event);
  if (!value) return GC_ERR_INVALID_HANDLE;
  value->killed = true;
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE event, EVENT_INFO_CMD command,
                                               INFO_DATATYPE* type, void* buffer,
                                               std::size_t* size) {
  std::lock_guard lock(g_mutex);
  auto* value = static_cast<EventData*>(event);
  if (!value) return GC_ERR_INVALID_HANDLE;
  switch (command) {
    case EVENT_EVENT_TYPE: return give(type, buffer, size, INFO_DATATYPE_INT32, std::int32_t{value->type});
    case EVENT_NUM_IN_QUEUE: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case EVENT_NUM_FIRED: return give(type, buffer, size, INFO_DATATYPE_UINT64, std::uint64_t{0});
    case EVENT_SIZE_MAX:
      return give(type, buffer, size, INFO_DATATYPE_SIZET,
                  value->type == EVENT_NEW_BUFFER ? sizeof(EVENT_NEW_BUFFER_DATA) : std::size_t{64});
    case EVENT_INFO_DATA_SIZE_MAX: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{0});
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE EventGetDataInfo(EVENT_HANDLE event, const void*,
                                                   std::size_t, EVENT_DATA_INFO_CMD,
                                                   INFO_DATATYPE*, void*, std::size_t*) {
  std::lock_guard lock(g_mutex);
  if (!event) return GC_ERR_INVALID_HANDLE;
  return GC_ERR_NOT_IMPLEMENTED;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE event) {
  std::lock_guard lock(g_mutex);
  return event ? GC_ERR_SUCCESS : GC_ERR_INVALID_HANDLE;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLOpen(TL_HANDLE* tl) {
  std::lock_guard lock(g_mutex);
  if (!tl) return GC_ERR_INVALID_PARAMETER;
  *tl = new TlHandle;
  g_device = DeviceData{};
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLClose(TL_HANDLE tl) {
  std::lock_guard lock(g_mutex);
  if (!tl) return GC_ERR_INVALID_HANDLE;
  delete static_cast<TlHandle*>(tl);
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetInfo(TL_HANDLE tl, TL_INFO_CMD command,
                                            INFO_DATATYPE* type, void* buffer,
                                            std::size_t* size) {
  {
    std::lock_guard lock(g_mutex);
    if (!tl) return GC_ERR_INVALID_HANDLE;
  }
  return GCGetInfo(command, type, buffer, size);
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLUpdateInterfaceList(TL_HANDLE tl, bool8_t* changed,
                                                        std::uint64_t) {
  std::lock_guard lock(g_mutex);
  if (!tl || !changed) return GC_ERR_INVALID_PARAMETER;
  *changed = 0;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetNumInterfaces(TL_HANDLE tl, std::uint32_t* count) {
  std::lock_guard lock(g_mutex);
  if (!tl || !count) return GC_ERR_INVALID_PARAMETER;
  *count = 1;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceID(TL_HANDLE tl, std::uint32_t index,
                                                   char* id, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!tl) return GC_ERR_INVALID_HANDLE;
  if (index != 0) return GC_ERR_INVALID_INDEX;
  return giveString(nullptr, id, size, "SyntheticBoard");
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceInfo(TL_HANDLE tl, const char* id,
                                                     INTERFACE_INFO_CMD command,
                                                     INFO_DATATYPE* type, void* buffer,
                                                     std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!tl) return GC_ERR_INVALID_HANDLE;
  if (!id || std::string_view(id) != "SyntheticBoard") return GC_ERR_INVALID_ID;
  switch (command) {
    case INTERFACE_INFO_ID: return giveString(type, buffer, size, "SyntheticBoard");
    case INTERFACE_INFO_DISPLAYNAME: return giveString(type, buffer, size, "Synthetic Board");
    case INTERFACE_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE TLOpenInterface(TL_HANDLE tl, const char* id,
                                                  IF_HANDLE* iface) {
  std::lock_guard lock(g_mutex);
  if (!tl || !iface) return GC_ERR_INVALID_PARAMETER;
  if (!id || std::string_view(id) != "SyntheticBoard") return GC_ERR_INVALID_ID;
  *iface = new IfHandle;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFClose(IF_HANDLE iface) {
  std::lock_guard lock(g_mutex);
  if (!iface) return GC_ERR_INVALID_HANDLE;
  delete static_cast<IfHandle*>(iface);
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetInfo(IF_HANDLE iface, INTERFACE_INFO_CMD command,
                                            INFO_DATATYPE* type, void* buffer,
                                            std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!iface) return GC_ERR_INVALID_HANDLE;
  switch (command) {
    case INTERFACE_INFO_ID: return giveString(type, buffer, size, "SyntheticBoard");
    case INTERFACE_INFO_DISPLAYNAME: return giveString(type, buffer, size, "Synthetic Board");
    case INTERFACE_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFUpdateDeviceList(IF_HANDLE iface, bool8_t* changed,
                                                     std::uint64_t) {
  std::lock_guard lock(g_mutex);
  if (!iface || !changed) return GC_ERR_INVALID_PARAMETER;
  *changed = 0;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetNumDevices(IF_HANDLE iface, std::uint32_t* count) {
  std::lock_guard lock(g_mutex);
  if (!iface || !count) return GC_ERR_INVALID_PARAMETER;
  *count = 1;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceID(IF_HANDLE iface, std::uint32_t index,
                                                char* id, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!iface) return GC_ERR_INVALID_HANDLE;
  if (index != 0) return GC_ERR_INVALID_INDEX;
  return giveString(nullptr, id, size, "SyntheticCam0");
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceInfo(IF_HANDLE iface, const char* id,
                                                  DEVICE_INFO_CMD command,
                                                  INFO_DATATYPE* type, void* buffer,
                                                  std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!iface) return GC_ERR_INVALID_HANDLE;
  if (!id || std::string_view(id) != "SyntheticCam0") return GC_ERR_INVALID_ID;
  switch (command) {
    case DEVICE_INFO_ID: return giveString(type, buffer, size, "SyntheticCam0");
    case DEVICE_INFO_VENDOR: return giveString(type, buffer, size, "OneGen");
    case DEVICE_INFO_MODEL: return giveString(type, buffer, size, "Synthetic Camera");
    case DEVICE_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    case DEVICE_INFO_DISPLAYNAME: return giveString(type, buffer, size, "Synthetic Camera (SyntheticCam0)");
    case DEVICE_INFO_USER_DEFINED_NAME: return giveString(type, buffer, size, "");
    case DEVICE_INFO_SERIAL_NUMBER: return giveString(type, buffer, size, "SYNTH-0001");
    case DEVICE_INFO_VERSION: return giveString(type, buffer, size, "1.0.0");
    case DEVICE_INFO_TIMESTAMP_FREQUENCY: return give(type, buffer, size, INFO_DATATYPE_UINT64, std::uint64_t{1000000000});
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE IFOpenDevice(IF_HANDLE iface, const char* id,
                                               DEVICE_ACCESS_FLAGS,
                                               DEV_HANDLE* device) {
  std::lock_guard lock(g_mutex);
  if (!iface || !device) return GC_ERR_INVALID_PARAMETER;
  if (!id || std::string_view(id) != "SyntheticCam0") return GC_ERR_INVALID_ID;
  if (g_device.open) return GC_ERR_RESOURCE_IN_USE;
  g_device.open = true;
  *device = new DevHandle;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetPort(DEV_HANDLE device, PORT_HANDLE* port) {
  std::lock_guard lock(g_mutex);
  if (!device || !port) return GC_ERR_INVALID_PARAMETER;
  *port = new PortHandle;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetNumDataStreams(DEV_HANDLE device, std::uint32_t* count) {
  std::lock_guard lock(g_mutex);
  if (!device || !count) return GC_ERR_INVALID_PARAMETER;
  *count = 1;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE device, std::uint32_t index,
                                                     char* id, std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!device) return GC_ERR_INVALID_HANDLE;
  if (index != 0) return GC_ERR_INVALID_INDEX;
  return giveString(nullptr, id, size, "stream0");
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevOpenDataStream(DEV_HANDLE device, const char* id,
                                                    DS_HANDLE* stream) {
  std::lock_guard lock(g_mutex);
  if (!device || !stream) return GC_ERR_INVALID_PARAMETER;
  if (!id || std::string_view(id) != "stream0") return GC_ERR_INVALID_ID;
  g_device.stream = StreamData{};
  *stream = new DsHandle;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetInfo(DEV_HANDLE device, DEVICE_INFO_CMD,
                                             INFO_DATATYPE*, void*, std::size_t*) {
  std::lock_guard lock(g_mutex);
  return device ? GC_ERR_NOT_IMPLEMENTED : GC_ERR_INVALID_HANDLE;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DevClose(DEV_HANDLE device) {
  std::lock_guard lock(g_mutex);
  if (!device) return GC_ERR_INVALID_HANDLE;
  delete static_cast<DevHandle*>(device);
  g_device.open = false;
  g_device.stream = StreamData{};
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer(DS_HANDLE stream, std::size_t size,
                                                           void* user, BUFFER_HANDLE* buffer) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  if (!g_device.open) return GC_ERR_INVALID_HANDLE;
  auto value = std::make_unique<BufferData>();
  value->storage.assign(size, 0);
  value->capacity = size;
  value->user = user;
  auto* handle = value.get();
  g_device.stream.buffers.push_back(std::move(value));
  ++g_device.stream.announced;
  *buffer = handle;
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE stream, BUFFER_HANDLE buffer) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  auto* value = static_cast<BufferData*>(buffer);
  if (value->revoked) return GC_ERR_INVALID_BUFFER;
  if (value->queued) return GC_ERR_SUCCESS;
  value->queued = true;
  value->ready = false;
  value->size_filled = 0;
  value->data_size = 0;
  g_device.stream.input.push_back(value);
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSFlushQueue(DS_HANDLE stream, ACQ_QUEUE_TYPE operation) {
  std::lock_guard lock(g_mutex);
  if (!stream) return GC_ERR_INVALID_HANDLE;
  if (operation == ACQ_QUEUE_ALL_TO_INPUT || operation == ACQ_QUEUE_UNQUEUED_TO_INPUT) {
    for (auto& buffer : g_device.stream.buffers) {
      if (buffer->revoked) continue;
      buffer->ready = false;
      buffer->queued = true;
      if (std::find(g_device.stream.input.begin(), g_device.stream.input.end(), buffer.get())
          == g_device.stream.input.end())
        g_device.stream.input.push_back(buffer.get());
    }
  } else if (operation == ACQ_QUEUE_ALL_DISCARD || operation == ACQ_QUEUE_OUTPUT_DISCARD) {
    g_device.stream.input.clear();
    for (auto& buffer : g_device.stream.buffers) {
      buffer->queued = false;
      buffer->ready = false;
    }
  } else if (operation == ACQ_QUEUE_INPUT_TO_OUTPUT) {
    for (auto& buffer : g_device.stream.buffers) {
      buffer->queued = false;
      buffer->ready = true;
    }
    g_device.stream.input.clear();
  } else {
    return GC_ERR_INVALID_PARAMETER;
  }
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSStartAcquisition(DS_HANDLE stream, ACQ_START_FLAGS,
                                                     std::uint64_t count) {
  std::lock_guard lock(g_mutex);
  if (!stream) return GC_ERR_INVALID_HANDLE;
  auto& state = g_device.stream;
  state.grabbing = true;
  state.finite = count != GENTL_INFINITE;
  state.remaining = state.finite ? count : 0;
  state.frame_counter = 0;
  state.trigger_pending = 0;
  state.delivered = 0;
  state.underrun = 0;
  state.started = 0;
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSStopAcquisition(DS_HANDLE stream, ACQ_STOP_FLAGS) {
  std::lock_guard lock(g_mutex);
  if (!stream) return GC_ERR_INVALID_HANDLE;
  g_device.stream.grabbing = false;
  g_device.stream.finite = false;
  g_device.stream.remaining = 0;
  g_device.stream.trigger_pending = 0;
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE stream, STREAM_INFO_CMD command,
                                            INFO_DATATYPE* type, void* buffer,
                                            std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!stream) return GC_ERR_INVALID_HANDLE;
  const auto& state = g_device.stream;
  switch (command) {
    case STREAM_INFO_ID: return giveString(type, buffer, size, state.id);
    case STREAM_INFO_NUM_DELIVERED: return give(type, buffer, size, INFO_DATATYPE_UINT64, state.delivered);
    case STREAM_INFO_NUM_UNDERRUN: return give(type, buffer, size, INFO_DATATYPE_UINT64, state.underrun);
    case STREAM_INFO_NUM_ANNOUNCED: return give(type, buffer, size, INFO_DATATYPE_SIZET, state.announced);
    case STREAM_INFO_NUM_QUEUED: return give(type, buffer, size, INFO_DATATYPE_SIZET, state.input.size());
    case STREAM_INFO_NUM_AWAIT_DELIVERY:
      return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case STREAM_INFO_NUM_STARTED: return give(type, buffer, size, INFO_DATATYPE_UINT64, state.started);
    case STREAM_INFO_PAYLOAD_SIZE: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{512});
    case STREAM_INFO_IS_GRABBING:
      return give(type, buffer, size, INFO_DATATYPE_BOOL8, b8(state.grabbing));
    case STREAM_INFO_DEFINES_PAYLOADSIZE: return give(type, buffer, size, INFO_DATATYPE_BOOL8, bool8_t{1});
    case STREAM_INFO_TLTYPE: return giveString(type, buffer, size, "Synthetic");
    case STREAM_INFO_NUM_CHUNKS_MAX: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case STREAM_INFO_BUF_ANNOUNCE_MIN: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{1});
    case STREAM_INFO_BUF_ALIGNMENT: return give(type, buffer, size, INFO_DATATYPE_SIZET, std::size_t{1});
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE stream, std::uint32_t index,
                                                BUFFER_HANDLE* buffer) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  std::uint32_t position = 0;
  for (auto& candidate : g_device.stream.buffers) {
    if (candidate->revoked) continue;
    if (position++ == index) {
      *buffer = candidate.get();
      return GC_ERR_SUCCESS;
    }
  }
  return GC_ERR_INVALID_INDEX;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE stream) {
  std::lock_guard lock(g_mutex);
  if (!stream) return GC_ERR_INVALID_HANDLE;
  delete static_cast<DsHandle*>(stream);
  g_device.stream = StreamData{};
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE stream, BUFFER_HANDLE buffer,
                                                 void** base, void** user) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  auto* value = static_cast<BufferData*>(buffer);
  if (base) *base = value->storage.data();
  if (user) *user = value->user;
  value->revoked = true;
  value->queued = false;
  value->ready = false;
  g_device.stream.input.erase(
      std::remove(g_device.stream.input.begin(), g_device.stream.input.end(), value),
      g_device.stream.input.end());
  g_device.stream.buffers.erase(
      std::remove_if(g_device.stream.buffers.begin(), g_device.stream.buffers.end(),
                     [value](const auto& item) { return item.get() == value; }),
      g_device.stream.buffers.end());
  g_condition.notify_all();
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferInfo(DS_HANDLE stream, BUFFER_HANDLE buffer,
                                                  BUFFER_INFO_CMD command,
                                                  INFO_DATATYPE* type, void* out,
                                                  std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  auto& value = *static_cast<BufferData*>(buffer);
  switch (command) {
    case BUFFER_INFO_BASE: return give(type, out, size, INFO_DATATYPE_PTR, static_cast<void*>(value.storage.data()));
    case BUFFER_INFO_SIZE: return give(type, out, size, INFO_DATATYPE_SIZET, value.capacity);
    case BUFFER_INFO_USER_PTR: return give(type, out, size, INFO_DATATYPE_PTR, value.user);
    case BUFFER_INFO_TIMESTAMP: return give(type, out, size, INFO_DATATYPE_UINT64, value.timestamp);
    case BUFFER_INFO_NEW_DATA: return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.ready));
    case BUFFER_INFO_IS_QUEUED: return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.queued));
    case BUFFER_INFO_IS_ACQUIRING: return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.acquiring));
    case BUFFER_INFO_IS_INCOMPLETE: return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.incomplete));
    case BUFFER_INFO_TLTYPE: return giveString(type, out, size, "Synthetic");
    case BUFFER_INFO_SIZE_FILLED: return give(type, out, size, INFO_DATATYPE_SIZET, value.size_filled);
    case BUFFER_INFO_WIDTH: return give(type, out, size, INFO_DATATYPE_SIZET, static_cast<std::size_t>(value.width));
    case BUFFER_INFO_HEIGHT: return give(type, out, size, INFO_DATATYPE_SIZET, static_cast<std::size_t>(value.height));
    case BUFFER_INFO_XOFFSET: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case BUFFER_INFO_YOFFSET: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case BUFFER_INFO_XPADDING: return give(type, out, size, INFO_DATATYPE_SIZET, value.x_padding);
    case BUFFER_INFO_YPADDING: return give(type, out, size, INFO_DATATYPE_SIZET, value.y_padding);
    case BUFFER_INFO_FRAMEID: return give(type, out, size, INFO_DATATYPE_UINT64, value.frame_id);
    case BUFFER_INFO_IMAGEPRESENT: return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.image_present));
    case BUFFER_INFO_IMAGEOFFSET: return give(type, out, size, INFO_DATATYPE_SIZET, value.image_offset);
    case BUFFER_INFO_PAYLOADTYPE: return give(type, out, size, INFO_DATATYPE_SIZET, static_cast<std::size_t>(value.payload_type));
    case BUFFER_INFO_PIXELFORMAT: return give(type, out, size, INFO_DATATYPE_UINT64, value.pixel_format);
    case BUFFER_INFO_PIXELFORMAT_NAMESPACE: return give(type, out, size, INFO_DATATYPE_UINT64, value.format_namespace);
    case BUFFER_INFO_DELIVERED_IMAGEHEIGHT: return give(type, out, size, INFO_DATATYPE_SIZET, value.delivered_height);
    case BUFFER_INFO_PIXEL_ENDIANNESS: return give(type, out, size, INFO_DATATYPE_INT32, value.pixel_endianness);
    case BUFFER_INFO_DATA_SIZE: return give(type, out, size, INFO_DATATYPE_SIZET, value.data_size);
    case BUFFER_INFO_TIMESTAMP_NS: return give(type, out, size, INFO_DATATYPE_UINT64, value.timestamp_ns);
    case BUFFER_INFO_DATA_LARGER_THAN_BUFFER:
      return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.data_larger_than_buffer));
    case BUFFER_INFO_CONTAINS_CHUNKDATA:
      return give(type, out, size, INFO_DATATYPE_BOOL8, b8(value.contains_chunk));
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE stream, BUFFER_HANDLE buffer,
                                                       SINGLE_CHUNK_DATA* chunks,
                                                       std::size_t* count) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer || !count) return GC_ERR_INVALID_PARAMETER;
  const auto& value = *static_cast<BufferData*>(buffer);
  if (!chunks) {
    *count = value.chunks.size();
    return value.chunks.empty() ? GC_ERR_SUCCESS : GC_ERR_BUFFER_TOO_SMALL;
  }
  const auto copied = std::min(*count, value.chunks.size());
  for (std::size_t index = 0; index < copied; ++index) {
    chunks[index].ChunkID = value.chunks[index].id;
    chunks[index].ChunkOffset = static_cast<ptrdiff_t>(value.chunks[index].offset);
    chunks[index].ChunkLength = value.chunks[index].length;
  }
  *count = copied;
  return copied == value.chunks.size() ? GC_ERR_SUCCESS : GC_ERR_BUFFER_TOO_SMALL;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetNumBufferParts(DS_HANDLE stream, BUFFER_HANDLE buffer,
                                                      std::uint32_t* count) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer || !count) return GC_ERR_INVALID_PARAMETER;
  *count = static_cast<std::uint32_t>(static_cast<BufferData*>(buffer)->parts.size());
  return GC_ERR_SUCCESS;
}

TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferPartInfo(DS_HANDLE stream, BUFFER_HANDLE buffer,
                                                      std::uint32_t index,
                                                      BUFFER_PART_INFO_CMD command,
                                                      INFO_DATATYPE* type, void* out,
                                                      std::size_t* size) {
  std::lock_guard lock(g_mutex);
  if (!stream || !buffer) return GC_ERR_INVALID_PARAMETER;
  auto& value = *static_cast<BufferData*>(buffer);
  if (index >= value.parts.size()) return GC_ERR_INVALID_ID;
  const auto& part = value.parts[index];
  switch (command) {
    case BUFFER_PART_INFO_BASE:
      return give(type, out, size, INFO_DATATYPE_PTR,
                  static_cast<void*>(value.storage.data() + part.offset));
    case BUFFER_PART_INFO_DATA_SIZE: return give(type, out, size, INFO_DATATYPE_SIZET, part.size);
    case BUFFER_PART_INFO_DATA_TYPE: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{PART_DATATYPE_2D_IMAGE});
    case BUFFER_PART_INFO_DATA_FORMAT: return give(type, out, size, INFO_DATATYPE_UINT64, part.data_format);
    case BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE:
      return give(type, out, size, INFO_DATATYPE_UINT64, part.format_namespace);
    case BUFFER_PART_INFO_WIDTH: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{part.width});
    case BUFFER_PART_INFO_HEIGHT: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{part.height});
    case BUFFER_PART_INFO_XOFFSET: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case BUFFER_PART_INFO_YOFFSET: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case BUFFER_PART_INFO_XPADDING: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{0});
    case BUFFER_PART_INFO_SOURCE_ID: return give(type, out, size, INFO_DATATYPE_UINT64, part.source_id);
    case BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT: return give(type, out, size, INFO_DATATYPE_SIZET, std::size_t{part.height});
    case BUFFER_PART_INFO_REGION_ID: return give(type, out, size, INFO_DATATYPE_UINT64, part.region_id);
    case BUFFER_PART_INFO_DATA_PURPOSE_ID: return give(type, out, size, INFO_DATATYPE_UINT64, part.purpose_id);
    default: return GC_ERR_INVALID_PARAMETER;
  }
}

}  // extern "C"
