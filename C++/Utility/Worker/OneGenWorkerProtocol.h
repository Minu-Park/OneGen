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

inline constexpr std::uint32_t protocol_version = 13;
inline constexpr std::uint32_t maximum_message_size = 256U * 1024U * 1024U;

enum class Operation : std::uint8_t {
  Probe, Open, Close, ListStreams, SelectStream, DescribeFeatures, NodeRead, NodeWrite,
  GrabOne, StartLive, StopLive, Trigger, Shutdown, Frame, Error, Invalidate, FrameAck,
  PollFeatures, RegisterRead, RegisterWrite, SaveFeatures, LoadFeatures,
  ListInterfaces, InterfaceFeatures, InterfaceRead, InterfaceWrite, ReleaseInterfaces,
  ClaimInterface, ReleaseInterfaceClaim, OpenInterface, ProbeInterface, CloseInterface,
  ObserveNode, WriteNodeChecked,
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
  void read(void* destination, std::size_t size)
  {
    require(size);
    std::memcpy(destination, data_.data() + offset_, size);
    offset_ += size;
  }
  [[nodiscard]] bool empty() const noexcept { return offset_ == data_.size(); }
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
