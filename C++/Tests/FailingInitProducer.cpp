#include "Internal/Gentl.h"
#include <cstdlib>

#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

namespace GenTL {
// Mandatory symbols are exported, but only initialization may be called.
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetInfo( TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetLastError( GC_ERROR *piErrorCode, char *sErrText, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCInitLib( void )
{ return GC_ERR_ERROR; }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCCloseLib( void )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCReadPort( PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCWritePort( PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetPortInfo( PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCRegisterEvent( EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCUnregisterEvent( EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE EventGetData( EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE EventKill( EVENT_HANDLE hEvent )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLOpen( TL_HANDLE *phTL )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLClose( TL_HANDLE hTL )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetNumInterfaces( TL_HANDLE hTL, uint32_t *piNumIfaces )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceInfo( TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLGetInterfaceID( TL_HANDLE hTL, uint32_t iIndex, char *sID, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLOpenInterface( TL_HANDLE hTL, const char *sIfaceID, IF_HANDLE *phIface )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE TLUpdateInterfaceList( TL_HANDLE hTL, bool8_t *pbChanged, uint64_t iTimeout )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFClose( IF_HANDLE hIface )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetNumDevices( IF_HANDLE hIface, uint32_t *piNumDevices )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceID( IF_HANDLE hIface, uint32_t iIndex, char *sIDeviceID, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFUpdateDeviceList( IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFGetDeviceInfo( IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE IFOpenDevice( IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlag, DEV_HANDLE *phDevice )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetPort( DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDevice )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetNumDataStreams( DEV_HANDLE hDevice, uint32_t *piNumDataStreams )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DevGetDataStreamID( DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DevOpenDataStream( DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DevClose( DEV_HANDLE hDevice )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer( DS_HANDLE hDataStream, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSFlushQueue( DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSStartAcquisition( DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSStopAcquisition( DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetInfo( DS_HANDLE hDataStream, STREAM_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSClose( DS_HANDLE hDataStream )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSRevokeBuffer( DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **pBuffer, void **pPrivate )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSQueueBuffer( DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferInfo( DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE DSGetBufferID( DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetNumPortURLs( PORT_HANDLE hPort, uint32_t *piNumURLs )
{ std::abort(); }
extern "C" TEST_EXPORT GC_ERROR GC_CALLTYPE GCGetPortURLInfo( PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize )
{ std::abort(); }
} // namespace GenTL
