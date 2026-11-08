#include "Internal/PixelLayout.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using onegen::RepresentVerdict;
using onegen::host_shared_representable;
using onegen::direct_representable;
using onegen::pixel_layout;

struct Case {
  std::uint64_t format;
  std::uint32_t width;
  std::uint32_t height;
  std::uint64_t stride;
};

std::uint64_t row_bytes(std::uint64_t format, std::uint32_t width)
{
  const auto bits = (format >> 16U) & 0xffU;
  return (static_cast<std::uint64_t>(width) * bits + 7U) / 8U;
}

Case make(std::uint64_t format, std::uint32_t width, std::uint32_t height,
          std::uint64_t padding = 0)
{
  return {format, width, height, row_bytes(format, width) + padding};
}

void same(Case value, RepresentVerdict host, RepresentVerdict direct)
{
  const auto used = value.stride * value.height;
  assert(host_shared_representable(value.format, value.width, value.height, value.stride, used)
         == host);
  assert(direct_representable(value.format, value.width, value.height, value.stride, used)
         == direct);
}

}  // namespace

int main() try {
  const std::vector<std::uint64_t> host_formats = {
      0x01080001, 0x01080008, 0x01080009, 0x0108000a, 0x0108000b,
      0x01100003, 0x01100005, 0x01100007, 0x01100025,
      0x0110000c, 0x0110000d, 0x0110000e, 0x0110000f,
      0x01100010, 0x01100011, 0x01100012, 0x01100013,
      0x0110002e, 0x0110002f, 0x01100030, 0x01100031,
      0x0210001f, 0x02100032,
      0x010a0046, 0x010c0047, 0x010a0052, 0x010a0054, 0x010a0056, 0x010a0058,
      0x010c0053, 0x010c0055, 0x010c0057, 0x010c0059,
      0x021000a5, 0x021000a6, 0x021400a8, 0x021400aa, 0x022000a7, 0x022000a9,
      0x021800ac, 0x021800ae, 0x022000ab, 0x022000ad,
      0x02180014, 0x02180015, 0x02200016, 0x02200017,
      0x02300018, 0x02300019, 0x0230001a, 0x0230001b, 0x02300033,
  };
  for (const auto format : host_formats) {
    const auto layout = pixel_layout(format);
    assert(layout.host_shared);
    assert(layout.bits_per_pixel == ((format >> 16U) & 0xffU));
    const auto sample = make(format, 4, 2);
    assert(host_shared_representable(format, sample.width, sample.height, sample.stride,
                                     sample.stride * sample.height)
           == RepresentVerdict::Ok);
  }
  for (const auto format : {0x0110002a, 0x0110002b, 0x0110002c, 0x0110002d}) {
    const auto sample = make(format, 3, 2);
    assert(host_shared_representable(format, sample.width, sample.height, sample.stride,
                                     sample.stride * sample.height)
           == RepresentVerdict::Ok);
    assert(direct_representable(format, sample.width, sample.height, sample.stride,
                                sample.stride * sample.height)
           == RepresentVerdict::UnknownLayout);
  }

  same(make(0x01100003, 4, 2, 2), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x010a0046, 64, 2), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x010c0059, 2, 2), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x02180014, 5, 2, 1), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x02180015, 5, 2, 1), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x02100032, 4, 2), RepresentVerdict::Ok, RepresentVerdict::Ok);
  same(make(0x021000a5, 4, 2), RepresentVerdict::Ok, RepresentVerdict::Ok);

  same(make(0x010a0046, 3, 2), RepresentVerdict::Ok, RepresentVerdict::PackedRowAlignment);
  same(make(0x02100032, 3, 2), RepresentVerdict::LayoutConstraint, RepresentVerdict::LayoutConstraint);
  same(make(0x021000a5, 3, 2), RepresentVerdict::Ok, RepresentVerdict::LayoutConstraint);

  same(make(0x02180021, 4, 2), RepresentVerdict::Planar, RepresentVerdict::Planar);
  same(make(0x026000c1, 2, 2), RepresentVerdict::Planar, RepresentVerdict::Planar);
  same(make(0x010800b1, 4, 2), RepresentVerdict::HostUnsupported, RepresentVerdict::HostUnsupported);
  same(make(0x0220001d, 4, 2), RepresentVerdict::HostUnsupported, RepresentVerdict::HostUnsupported);
  same(make(0x010c0006, 4, 2), RepresentVerdict::HostUnsupported, RepresentVerdict::HostUnsupported);
  same(make(0x020c001e, 4, 2), RepresentVerdict::HostUnsupported, RepresentVerdict::HostUnsupported);
  same(make(0x01010037, 8, 2), RepresentVerdict::HostUnsupported, RepresentVerdict::HostUnsupported);
  same(make(0x01100004, 4, 2), RepresentVerdict::UnknownLayout, RepresentVerdict::UnknownLayout);
  same(make(0x81080001, 4, 2), RepresentVerdict::UnknownLayout, RepresentVerdict::UnknownLayout);

  {
    const auto format = 0x01080001;
    assert(host_shared_representable(format, 4, 2, 3, 6) == RepresentVerdict::GeometryUnrepresentable);
    assert(direct_representable(format, 4, 2, 3, 6) == RepresentVerdict::GeometryUnrepresentable);
    assert(host_shared_representable(format, 4, 2, 4, 7) == RepresentVerdict::GeometryUnrepresentable);
    assert(host_shared_representable(format, 0, 2, 4, 8) == RepresentVerdict::GeometryUnrepresentable);
    assert(direct_representable(format, 0, 2, 4, 8) == RepresentVerdict::GeometryUnrepresentable);
    assert(host_shared_representable(format, 0xffffffffU, 2, 8, 16)
           == RepresentVerdict::GeometryUnrepresentable);
    assert(host_shared_representable(format, 4, 2, std::uint64_t{1} << 63, 0)
           == RepresentVerdict::GeometryUnrepresentable);
    assert(direct_representable(format, 4, 2, std::uint64_t{1} << 63, 0)
           == RepresentVerdict::GeometryUnrepresentable);
  }

  std::cout << "test_pixel_layout ok: " << host_formats.size() << " legacy host formats\n";
  return 0;
} catch (...) {
  return 1;
}
