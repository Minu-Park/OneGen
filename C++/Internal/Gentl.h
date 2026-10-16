#pragma once

#include "OneGenError.h"

#include <GenTL/GenTL.h>

#include <filesystem>
#include <string>

namespace onegen {

#define OG_GENTL_SYMS(X)                     \
  X(GCGetInfo)                               \
  X(GCGetLastError)                          \
  X(GCInitLib)                               \
  X(GCCloseLib)                              \
  X(GCReadPort)                              \
  X(GCWritePort)                             \
  X(GCGetPortInfo)                           \
  X(GCRegisterEvent)                         \
  X(GCUnregisterEvent)                       \
  X(EventGetData)                            \
  X(EventKill)                               \
  X(TLOpen)                                  \
  X(TLClose)                                 \
  X(TLGetNumInterfaces)                      \
  X(TLGetInterfaceInfo)                      \
  X(TLGetInterfaceID)                        \
  X(TLOpenInterface)                         \
  X(TLUpdateInterfaceList)                   \
  X(IFClose)                                 \
  X(IFGetNumDevices)                         \
  X(IFGetDeviceID)                           \
  X(IFUpdateDeviceList)                      \
  X(IFGetDeviceInfo)                         \
  X(IFOpenDevice)                            \
  X(DevGetPort)                              \
  X(DevGetNumDataStreams)                    \
  X(DevGetDataStreamID)                      \
  X(DevOpenDataStream)                       \
  X(DevClose)                                \
  X(DSAllocAndAnnounceBuffer)                \
  X(DSFlushQueue)                            \
  X(DSStartAcquisition)                      \
  X(DSStopAcquisition)                       \
  X(DSGetInfo)                               \
  X(DSClose)                                 \
  X(DSRevokeBuffer)                          \
  X(DSQueueBuffer)                           \
  X(DSGetBufferInfo)                         \
  X(DSGetBufferID)                           \
  X(GCGetNumPortURLs)                        \
  X(GCGetPortURLInfo)

struct Gentl {
  void* lib = nullptr;
  GenTL::TL_HANDLE tl = nullptr;

#define OG_DECL(name) GenTL::P##name name = nullptr;
  OG_GENTL_SYMS(OG_DECL)
#undef OG_DECL

  GenTL::PDSGetNumBufferParts DSGetNumBufferParts = nullptr;
  GenTL::PDSGetBufferPartInfo DSGetBufferPartInfo = nullptr;
  GenTL::PDSGetBufferChunkData DSGetBufferChunkData = nullptr;
  GenTL::PEventGetInfo EventGetInfo = nullptr;
  GenTL::PEventGetDataInfo EventGetDataInfo = nullptr;

  void load(const std::filesystem::path& cti);
  void close_tl();
  void unload();
  /** Reports the failing operation's return code independently of optional SDK detail. */
  [[noreturn]] void fail(const char* where, GenTL::GC_ERROR result) const;
  void check(GenTL::GC_ERROR err, const char* where) const;
};

}  // namespace onegen
