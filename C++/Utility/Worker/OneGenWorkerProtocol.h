#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace onegen::worker {

inline constexpr std::uint32_t protocol_version = 17;
inline constexpr std::uint32_t maximum_message_size = 256U * 1024U * 1024U;
inline constexpr std::uint32_t maximum_frame_ready_size = 64U * 1024U;
inline constexpr std::uint32_t maximum_metadata_components = 1024;
inline constexpr std::uint32_t maximum_metadata_string_bytes = 4096;
inline constexpr std::uint32_t maximum_metadata_diagnostic_bytes = 16384;

// FrameAck returns the single non-direct transport credit after the client has
// read the frame. It is not a consumer-completion signal. CaptureMark is a
// one-way worker message sent for every dequeued live buffer.
enum class Operation : std::uint8_t {
  Probe, Open, Close, ListStreams, SelectStream, DescribeFeatures, NodeRead, NodeWrite,
  GrabOne, StartLive, StopLive, Trigger, Shutdown, Frame, Error, Invalidate, FrameAck,
  PollFeatures, RegisterRead, RegisterWrite, SaveFeatures, LoadFeatures,
  ListInterfaces, InterfaceFeatures, InterfaceRead, InterfaceWrite, ReleaseInterfaces,
  ClaimInterface, ReleaseInterfaceClaim, OpenInterface, ProbeInterface, CloseInterface,
  ObserveNode, WriteNodeChecked,
  ProducerInactive,
  FramePoolAttach, FramePoolDetach, FrameReady, SlotRelease,
  CaptureMark,
};

/** @brief Host-created pool handshake; the worker validates bounds before mapping. */
struct FramePoolAttach {
  std::string region;
  std::uint64_t region_bytes = 0;
  std::uint32_t slot_count = 0;
  std::uint64_t slot_bytes = 0;
  std::uint64_t epoch = 0;
  std::uint64_t token = 0;
};

/** @brief Exact slot identity carried by a published direct frame. */
struct SlotIdentity {
  std::uint32_t session = 0;
  std::uint64_t epoch = 0;
  std::uint64_t token = 0;
  std::uint32_t slot = 0;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t used_bytes = 0;
};

/** @brief Derived host view; this struct is never encoded on the v17 wire. */
struct FrameDescriptor {
  std::uint32_t session = 0;
  std::uint64_t epoch = 0;
  std::uint64_t token = 0;
  std::uint32_t slot = 0;
  std::uint64_t generation = 0;
  std::uint64_t frame_id = 0;
  std::uint64_t sequence = 0;
  std::uint64_t used_bytes = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t stride = 0;
  std::uint64_t pixel_format = 0;
  std::uint64_t format_namespace = 0;
  std::uint32_t pixel_endianness = 0;
  std::uint64_t payload_type = 0;
  std::uint32_t stream_index = 0;
  std::string stream_id;
};

/** @brief Exact slot identity, never a bool-only acknowledgement. */
struct SlotRelease {
  std::uint64_t epoch = 0;
  std::uint64_t token = 0;
  std::uint32_t slot = 0;
  std::uint64_t generation = 0;
};

struct Message {
  Operation operation = Operation::Error;
  std::uint32_t id = 0;
  bool success = true;
  std::string error;
  std::vector<std::byte> payload;
  std::uint32_t session = 0;
};

class Writer {
public:
  template <class T> void scalar(T value)
  {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    bytes(&value, sizeof(value));
  }
  void bytes(const void* data, std::size_t size)
  {
    if (size > maximum_message_size || data_.size() > maximum_message_size - size)
      throw std::length_error("OneGen worker message is too large.");
    const auto* first = static_cast<const std::byte*>(data);
    data_.insert(data_.end(), first, first + size);
  }
  void string(std::string_view value)
  {
    scalar(static_cast<std::uint32_t>(value.size()));
    bytes(value.data(), value.size());
  }
  [[nodiscard]] std::vector<std::byte> take() { return std::move(data_); }
private:
  std::vector<std::byte> data_;
};

class Reader {
public:
  explicit Reader(const std::vector<std::byte>& data) : data_(data) {}
  template <class T> [[nodiscard]] T scalar()
  {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    T value;
    read(&value, sizeof(value));
    return value;
  }
  [[nodiscard]] std::string string()
  {
    const auto size = scalar<std::uint32_t>();
    require(size);
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
    offset_ += size;
    return value;
  }
  /** @brief Reads a string only after its declared length passes the metadata bound. */
  [[nodiscard]] std::string string(std::uint32_t maximum)
  {
    const auto size = scalar<std::uint32_t>();
    if (size > maximum) throw std::runtime_error("OneGen worker string exceeds its limit.");
    require(size);
    std::string value(reinterpret_cast<const char*>(data_.data() + offset_), size);
    offset_ += size;
    return value;
  }
  /** @brief Reads a strict wire bool and rejects every value except zero or one. */
  [[nodiscard]] bool boolean()
  {
    const auto value = scalar<std::uint8_t>();
    if (value > 1) throw std::runtime_error("OneGen worker boolean is not 0 or 1.");
    return value != 0;
  }
  void read(void* destination, std::size_t size)
  {
    require(size);
    std::memcpy(destination, data_.data() + offset_, size);
    offset_ += size;
  }
  [[nodiscard]] bool empty() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remainingSize() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::vector<std::byte> remaining()
  {
    std::vector<std::byte> value(data_.begin() + static_cast<std::ptrdiff_t>(offset_), data_.end());
    offset_ = data_.size();
    return value;
  }
private:
  void require(std::size_t size) const
  {
    if (offset_ > data_.size() || size > data_.size() - offset_)
      throw std::runtime_error("Truncated OneGen worker message.");
  }
  const std::vector<std::byte>& data_;
  std::size_t offset_ = 0;
};

inline void write(Writer& out, const FramePoolAttach& value)
{
  out.string(value.region);
  out.scalar(value.region_bytes);
  out.scalar(value.slot_count);
  out.scalar(value.slot_bytes);
  out.scalar(value.epoch);
  out.scalar(value.token);
}

inline FramePoolAttach readFramePoolAttach(Reader& in)
{
  FramePoolAttach value;
  value.region = in.string();
  value.region_bytes = in.scalar<std::uint64_t>();
  value.slot_count = in.scalar<std::uint32_t>();
  value.slot_bytes = in.scalar<std::uint64_t>();
  value.epoch = in.scalar<std::uint64_t>();
  value.token = in.scalar<std::uint64_t>();
  return value;
}

inline void write(Writer& out, const SlotIdentity& value)
{
  out.scalar(value.session);
  out.scalar(value.epoch);
  out.scalar(value.token);
  out.scalar(value.slot);
  out.scalar(value.generation);
  out.scalar(value.sequence);
  out.scalar(value.used_bytes);
}

inline SlotIdentity readSlotIdentity(Reader& in)
{
  SlotIdentity value;
  value.session = in.scalar<std::uint32_t>();
  value.epoch = in.scalar<std::uint64_t>();
  value.token = in.scalar<std::uint64_t>();
  value.slot = in.scalar<std::uint32_t>();
  value.generation = in.scalar<std::uint64_t>();
  value.sequence = in.scalar<std::uint64_t>();
  value.used_bytes = in.scalar<std::uint64_t>();
  return value;
}

inline void write(Writer& out, const SlotRelease& value)
{
  out.scalar(value.epoch);
  out.scalar(value.token);
  out.scalar(value.slot);
  out.scalar(value.generation);
}

inline SlotRelease readSlotRelease(Reader& in)
{
  SlotRelease value;
  value.epoch = in.scalar<std::uint64_t>();
  value.token = in.scalar<std::uint64_t>();
  value.slot = in.scalar<std::uint32_t>();
  value.generation = in.scalar<std::uint64_t>();
  return value;
}

inline std::array<std::byte, 4> frameSize(std::uint32_t size)
{
  return {std::byte(size >> 24), std::byte(size >> 16), std::byte(size >> 8), std::byte(size)};
}

inline std::uint32_t frameSize(const std::array<std::byte, 4>& bytes)
{
  return (std::to_integer<std::uint32_t>(bytes[0]) << 24)
      | (std::to_integer<std::uint32_t>(bytes[1]) << 16)
      | (std::to_integer<std::uint32_t>(bytes[2]) << 8)
      | std::to_integer<std::uint32_t>(bytes[3]);
}

inline std::vector<std::byte> encode(Message message)
{
  Writer out;
  out.scalar(protocol_version);
  out.scalar(message.operation);
  out.scalar(message.id);
  out.scalar(message.success);
  out.string(message.error);
  out.scalar(message.session);
  out.bytes(message.payload.data(), message.payload.size());
  return out.take();
}

inline Message decode(const std::vector<std::byte>& bytes)
{
  Reader in(bytes);
  if (in.scalar<std::uint32_t>() != protocol_version)
    throw std::runtime_error("Unsupported OneGen worker protocol version.");
  Message message;
  message.operation = in.scalar<Operation>();
  message.id = in.scalar<std::uint32_t>();
  message.success = in.scalar<bool>();
  message.error = in.string();
  message.session = in.scalar<std::uint32_t>();
  message.payload = in.remaining();
  return message;
}

} // namespace onegen::worker
