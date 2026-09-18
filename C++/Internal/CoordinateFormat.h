#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace onegen {

/** PFNC coordinate layout. Packed `p` formats use LSB-first little-endian bits. */
struct CoordinateFormat {
  unsigned components = 0;
  unsigned sample_bits = 0;
  bool planar = false;
  bool floating = false;
  bool packed = false;
};

/** Bytes needed to store one packed or unpacked coordinate row of one plane. */
inline std::size_t coordinate_row_bytes(const CoordinateFormat& layout, std::uint32_t width) noexcept
{
  const auto comps = layout.planar ? 1U : layout.components;
  if (!layout.sample_bits || !comps || !width
      || width > (std::numeric_limits<std::uint64_t>::max)() / comps / layout.sample_bits)
    return 0;
  return static_cast<std::size_t>((std::uint64_t{width} * comps * layout.sample_bits + 7U) / 8U);
}

/** Read `width` consecutive LSB-first bits starting at `bit`. */
inline std::uint64_t packed_bits(const std::uint8_t* data, std::size_t bytes,
                                 std::uint64_t bit, unsigned width) noexcept
{
  std::uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) {
    const auto pos = bit + i;
    const auto index = static_cast<std::size_t>(pos / 8);
    if (index >= bytes) return 0;
    if (data[index] & static_cast<std::uint8_t>(1U << (pos % 8)))
      value |= (std::uint64_t{1} << i);
  }
  return value;
}

/** Return the supported C/AC/ABC layout, or an empty layout for other PFNC formats. */
inline CoordinateFormat coordinate_format(std::uint64_t format) noexcept
{
  switch (format) {
    case 0x010800b1: return {1, 8, false, false, false};
    case 0x011000b8: return {1, 16, false, false, false};
    case 0x012000bf: return {1, 32, false, true, false};
    case 0x01400128: return {1, 64, false, true, false};
    case 0x010a00d7: return {1, 10, false, false, true};
    case 0x010c00da: return {1, 12, false, false, true};
    case 0x021800b2: return {3, 8, false, false, false};
    case 0x021800b3: return {3, 8, true, false, false};
    case 0x023000b9: return {3, 16, false, false, false};
    case 0x023000ba: return {3, 16, true, false, false};
    case 0x026000c0: return {3, 32, false, true, false};
    case 0x026000c1: return {3, 32, true, true, false};
    case 0x02c00129: return {3, 64, false, true, false};
    case 0x02c0012a: return {3, 64, true, true, false};
    case 0x021e00db: return {3, 10, false, false, true};
    case 0x021e00dc: return {3, 10, true, false, true};
    case 0x022400de: return {3, 12, false, false, true};
    case 0x022400df: return {3, 12, true, false, true};
    case 0x021000b4: return {2, 8, false, false, false};
    case 0x021000b5: return {2, 8, true, false, false};
    case 0x022000bb: return {2, 16, false, false, false};
    case 0x022000bc: return {2, 16, true, false, false};
    case 0x024000c2: return {2, 32, false, true, false};
    case 0x024000c3: return {2, 32, true, true, false};
    case 0x0280012b: return {2, 64, false, true, false};
    case 0x0280012c: return {2, 64, true, true, false};
    case 0x021400f0: return {2, 10, false, false, true};
    case 0x021400f1: return {2, 10, true, false, true};
    case 0x021800f2: return {2, 12, false, false, true};
    case 0x021800f3: return {2, 12, true, false, true};
    default: return {};
  }
}

} // namespace onegen
