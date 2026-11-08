#pragma once

#include "Internal/CoordinateFormat.h"

#include <cstdint>
#include <limits>

namespace onegen {

enum class PixelPacking : std::uint8_t {
  Unknown,
  Unpacked,
  PackedLsb,
  Subsampled422,
  BiColor,
  Planar,
  NotHostShared,
};

struct PixelLayout {
  PixelPacking packing = PixelPacking::Unknown;
  std::uint16_t bits_per_pixel = 0;
  std::uint8_t planes = 1;
  std::uint8_t width_multiple = 1;
  bool host_shared = false;
  /** Legacy host-only values whose direct producer semantics are not proven. */
  bool legacy_host_only = false;
};

enum class RepresentVerdict : std::uint8_t {
  Ok,
  UnknownLayout,
  Planar,
  HostUnsupported,
  GeometryUnrepresentable,
  LayoutConstraint,
  PackedRowAlignment,
};

inline PixelLayout pixel_layout(std::uint64_t pfnc) noexcept
{
  const auto bits = static_cast<std::uint16_t>((pfnc >> 16U) & 0xffU);
  const auto entry = [bits](PixelPacking packing, bool host, std::uint8_t multiple = 1) {
    return PixelLayout{packing, bits, 1, multiple, host, false};
  };
  switch (static_cast<std::uint32_t>(pfnc)) {
    case 0x01080001:
    case 0x01080008:
    case 0x01080009:
    case 0x0108000a:
    case 0x0108000b:
    case 0x01100003:
    case 0x01100005:
    case 0x01100007:
    case 0x01100025:
    case 0x0110000c:
    case 0x0110000d:
    case 0x0110000e:
    case 0x0110000f:
    case 0x01100010:
    case 0x01100011:
    case 0x01100012:
    case 0x01100013:
    case 0x0110002e:
    case 0x0110002f:
    case 0x01100030:
    case 0x01100031:
    case 0x02180014:
    case 0x02180015:
    case 0x02200016:
    case 0x02200017:
    case 0x02300018:
    case 0x02300019:
    case 0x0230001a:
    case 0x0230001b:
    case 0x02300033:
      return entry(PixelPacking::Unpacked, true);
    case 0x0110002a:
    case 0x0110002b:
    case 0x0110002c:
    case 0x0110002d: {
      auto legacy = entry(PixelPacking::Unpacked, true);
      legacy.legacy_host_only = true;
      return legacy;
    }
    case 0x010a0046:
    case 0x010c0047:
    case 0x010a0052:
    case 0x010a0054:
    case 0x010a0056:
    case 0x010a0058:
    case 0x010c0053:
    case 0x010c0055:
    case 0x010c0057:
    case 0x010c0059:
      return entry(PixelPacking::PackedLsb, true);
    case 0x0210001f:
    case 0x02100032:
      return entry(PixelPacking::Subsampled422, true, 2);
    case 0x021000a5:
    case 0x021000a6:
    case 0x021400a8:
    case 0x021400aa:
    case 0x022000a7:
    case 0x022000a9:
    case 0x021800ac:
    case 0x021800ae:
    case 0x022000ab:
    case 0x022000ad:
      return entry(PixelPacking::BiColor, true, 2);
    case 0x02180021: {
      PixelLayout planar{PixelPacking::Planar, bits, 3, 1, false, false};
      return planar;
    }
    default:
      break;
  }
  const auto coordinate = coordinate_format(pfnc);
  if (coordinate.planar)
    return PixelLayout{PixelPacking::Planar, bits, static_cast<std::uint8_t>(coordinate.components),
                       1, false, false};
  if (coordinate.components)
    return PixelLayout{PixelPacking::NotHostShared, bits, static_cast<std::uint8_t>(coordinate.components),
                       1, false, false};
  switch (static_cast<std::uint32_t>(pfnc)) {
    case 0x01010037:
    case 0x010c0004:
    case 0x010c0006:
    case 0x010c0027:
    case 0x010c002b:
    case 0x020c001e:
    case 0x0220001d:
      return PixelLayout{PixelPacking::NotHostShared, bits, 1, 1, false, false};
    default:
      return PixelLayout{};
  }
}

namespace detail {

inline RepresentVerdict geometry_verdict(const PixelLayout& layout, std::uint32_t width,
                                         std::uint32_t height, std::uint64_t stride,
                                         std::uint64_t used_bytes) noexcept
{
  if (!width || !height || !stride) return RepresentVerdict::GeometryUnrepresentable;
  if (width > ((std::numeric_limits<std::uint64_t>::max)() - 7U) / layout.bits_per_pixel)
    return RepresentVerdict::GeometryUnrepresentable;
  const auto row_bytes = (static_cast<std::uint64_t>(width) * layout.bits_per_pixel + 7U) / 8U;
  if (!row_bytes || row_bytes > stride) return RepresentVerdict::GeometryUnrepresentable;
  if (stride > (std::numeric_limits<std::uint64_t>::max)() / height)
    return RepresentVerdict::GeometryUnrepresentable;
  if (stride * height != used_bytes) return RepresentVerdict::GeometryUnrepresentable;
  return RepresentVerdict::Ok;
}

}  // namespace detail

/** Legacy host-side acceptance: preserve the existing list and YUV422 width rule. */
inline RepresentVerdict host_shared_representable(std::uint64_t pfnc, std::uint32_t width,
                                                   std::uint32_t height, std::uint64_t stride,
                                                   std::uint64_t used_bytes) noexcept
{
  const auto layout = pixel_layout(pfnc);
  if (layout.packing == PixelPacking::Unknown) return RepresentVerdict::UnknownLayout;
  if (layout.packing == PixelPacking::Planar) return RepresentVerdict::Planar;
  if (!layout.host_shared) return RepresentVerdict::HostUnsupported;
  if (layout.packing == PixelPacking::Subsampled422 && (width % 2U))
    return RepresentVerdict::LayoutConstraint;
  return detail::geometry_verdict(layout, width, height, stride, used_bytes);
}

/**
 * Direct producer capability: same layout classes, but only proven PFNC meanings.
 * Legacy host-only values, unaligned packed rows and odd BiColor widths stay on
 * the same-pin owned path until producer/transport evidence is available.
 */
inline RepresentVerdict direct_representable(std::uint64_t pfnc, std::uint32_t width,
                                              std::uint32_t height, std::uint64_t stride,
                                              std::uint64_t used_bytes) noexcept
{
  const auto layout = pixel_layout(pfnc);
  if (layout.packing == PixelPacking::Unknown) return RepresentVerdict::UnknownLayout;
  if (layout.packing == PixelPacking::Planar) return RepresentVerdict::Planar;
  if (layout.legacy_host_only) return RepresentVerdict::UnknownLayout;
  if (!layout.host_shared) return RepresentVerdict::HostUnsupported;
  if (layout.packing == PixelPacking::PackedLsb) {
    if (width > (std::numeric_limits<std::uint64_t>::max)() / layout.bits_per_pixel)
      return RepresentVerdict::GeometryUnrepresentable;
    if ((static_cast<std::uint64_t>(width) * layout.bits_per_pixel) % 8U)
      return RepresentVerdict::PackedRowAlignment;
  }
  if (layout.width_multiple > 1 && (width % layout.width_multiple))
    return RepresentVerdict::LayoutConstraint;
  return detail::geometry_verdict(layout, width, height, stride, used_bytes);
}

}  // namespace onegen
