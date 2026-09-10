#include "gentl.hpp"

#include "onegen/error.hpp"
#include "onegen/onegen.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

const char* dtype(int t) {
  switch (t) {
    case 1:
      return "STRING";
    case 5:
      return "INT32";
    case 6:
      return "UINT32";
    case 7:
      return "INT64";
    case 8:
      return "UINT64";
    case 13:
      return "BUFFER";
    default:
      return "OTHER";
  }
}

void dump_cmd(onegen::Gentl& g, GenTL::IF_HANDLE hif, const char* devid, int cmd) {
  unsigned char buf[512] = {};
  size_t n = sizeof(buf);
  GenTL::INFO_DATATYPE type = GenTL::INFO_DATATYPE_UNKNOWN;
  auto err = g.IFGetDeviceInfo(hif, devid, cmd, &type, buf, &n);
  if (err != GenTL::GC_ERR_SUCCESS) return;
  std::printf("  cmd=%d type=%s(%d) size=%zu", cmd, dtype(type), type, n);
  if (type == GenTL::INFO_DATATYPE_STRING) {
    std::printf(" str=\"%s\"", buf);
  } else {
    std::printf(" hex=");
    for (size_t i = 0; i < n && i < 16; ++i) std::printf("%02x", buf[i]);
    if (n == 4) {
      std::uint32_t v = 0;
      std::memcpy(&v, buf, 4);
      std::printf(" u32=%u 0x%08x", v, v);
    }
    if (n == 8) {
      std::uint64_t v = 0;
      std::memcpy(&v, buf, 8);
      std::printf(" u64=%llu", static_cast<unsigned long long>(v));
    }
  }
  std::printf("\n");
}

}  // namespace

int main() {
  const char* cti =
      "/Library/Frameworks/pylon.framework/Libraries/gentlproducer/gtl/ProducerGEV.cti";
  onegen::Gentl g;
  g.load(cti);
  bool8_t changed = 0;
  g.check(g.TLUpdateInterfaceList(g.tl, &changed, 3000), "TLUpdateInterfaceList");
  uint32_t nif = 0;
  g.check(g.TLGetNumInterfaces(g.tl, &nif), "TLGetNumInterfaces");
  std::printf("interfaces=%u\n", nif);
  for (uint32_t i = 0; i < nif; ++i) {
    char ifid[512] = {};
    size_t ifn = sizeof(ifid);
    g.check(g.TLGetInterfaceID(g.tl, i, ifid, &ifn), "TLGetInterfaceID");
    GenTL::IF_HANDLE hif = nullptr;
    g.check(g.TLOpenInterface(g.tl, ifid, &hif), "TLOpenInterface");
    bool8_t dch = 0;
    g.check(g.IFUpdateDeviceList(hif, &dch, 3000), "IFUpdateDeviceList");
    uint32_t nd = 0;
    g.check(g.IFGetNumDevices(hif, &nd), "IFGetNumDevices");
    std::printf("if[%u] id=%s devices=%u\n", i, ifid, nd);

    uint32_t nurl = 0;
    auto uerr = g.GCGetNumPortURLs(hif, &nurl);
    std::printf("  IF port URLs err=%d n=%u\n", uerr, nurl);
    if (uerr == GenTL::GC_ERR_SUCCESS && nurl) {
      char url[1024] = {};
      size_t us = sizeof(url);
      GenTL::INFO_DATATYPE t = GenTL::INFO_DATATYPE_UNKNOWN;
      if (g.GCGetPortURLInfo(hif, 0, GenTL::URL_INFO_URL, &t, url, &us) == GenTL::GC_ERR_SUCCESS)
        std::printf("  URL=%s\n", url);
    }

    for (uint32_t d = 0; d < nd; ++d) {
      char devid[512] = {};
      size_t dn = sizeof(devid);
      g.check(g.IFGetDeviceID(hif, d, devid, &dn), "IFGetDeviceID");
      std::printf("  device[%u] id=%s\n", d, devid);
      for (int cmd = 0; cmd <= 15; ++cmd) dump_cmd(g, hif, devid, cmd);
      for (int cmd = 1000; cmd <= 1020; ++cmd) dump_cmd(g, hif, devid, cmd);
    }
    g.IFClose(hif);
  }
  g.unload();

  onegen::OneGen og;
  og.loadCti(cti);
  for (const auto& d : og.enumerate(std::chrono::milliseconds{3000})) {
    std::printf("enum id=%s ip=%s serial=%s name=%s\n", d.id.c_str(), d.ip.c_str(),
                d.serial.c_str(), d.display_name.c_str());
  }
  auto& cam = og.addCamera();
  cam.open("192.168.0.157");
  auto r = cam.features().root();
  std::printf("root=%s\n", r.display_name().c_str());
  auto kids = r.children();
  std::printf("children=%zu\n", kids.size());
  for (const auto& ch : kids) {
    std::printf("  %s (%s)\n", ch.name().c_str(), ch.display_name().c_str());
  }
  auto frames = cam.single(1);
  if (frames.empty()) {
    std::printf("single: no frames\n");
  } else {
    const auto& f = frames.back();
    std::printf("single %ux%u pxfmt=%llu bytes=%zu ts=%llu\n", f.width, f.height,
                static_cast<unsigned long long>(f.pixel_format), f.payload.size(),
                static_cast<unsigned long long>(f.timestamp));
  }
  cam.close();
  return 0;
}
