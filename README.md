# OneGen

OneGen is a C++20 GenTL consumer library. It loads CTI producers at runtime,
enumerates cameras, acquires owned frame payloads, and exposes GenApi features.

## Build

Requirements: CMake 3.24+, a C++20 compiler, and network access for the pinned
EMVA GenICam package cached under `genicam/`. For extracted local packages, set
`ONEGEN_GENICAM_ROOT`, `ONEGEN_GENICAM_RUNTIME_ROOT`, and `ONEGEN_GENTL_INCLUDE`.
Qt 6 Widgets is optional and enables `OneGen::QtWidget`.

```sh
cmake -S . -B build
cmake --build build
cmake --build build --target check
```

## Runtime

Pass a vendor CTI file to `OneGen::loadCti()`, then enumerate devices or open a
camera by ID, user-defined name, IP address, serial number, display name, or
model. Loading a CTI executes vendor code, so only use trusted producers.
The Playground plugin discovers `.cti` entries from `GENICAM_GENTL64_PATH` or
`GENICAM_GENTL32_PATH` and documented vendor locations. Discovery starts one
`OneGenWorker` process per CTI; the first camera adopts that worker, and additional
cameras use separate workers so Open cannot close another camera. Successfully
closed camera workers retain their discovery caches for reconnect. Each worker preserves the producer install directory for
relative dependencies while isolating incompatible vendor runtimes. The worker
uses a versioned binary stdin/stdout protocol and has no Qt dependency. Do not
copy a CTI without its producer tree.

GenTL 1.5 multipart buffers are retained without a vendor SDK. The Playground
adapter assembles standard PFNC `Coord3D_ABC32f` and `Coord3D_C16` range data
with matching mono intensity and confidence parts. SFNC coordinate scale,
offset, invalid-value, and distance-unit features are snapshotted before
acquisition; missing calibration is rejected instead of guessed.

GenDC parsing, disparity calibration, vendor motion/trigger recipes, and
proprietary pixel formats are not GenTL services and still require a documented
standard payload or an explicit device-family profile before removing their
SDK paths.

## Vendor SDK replacement map

| Existing module capability | GenICam replacement in OneGen | Status |
| --- | --- | --- |
| Camera discovery, connection, feature tree, Single/Live/Stop | GenTL system/interface/device/stream plus remote GenApi | Implemented |
| Camera 2D image delivery | PFNC image payload, standard GenTL JPEG payload, and owned buffer copy | Implemented; big-endian multi-byte pixels fail safely |
| Camera multipart XYZ, intensity, confidence | GenTL parts plus PFNC and SFNC Scan3d metadata | Implemented for ABC32f/C16 |
| Camera GenDC and stereo disparity reconstruction | GenDC descriptor parsing and a documented calibration profile | Pending payload evidence |
| Framegrabber board/applet/camera feature access | GenTL interface/device/remote-device module node maps | Implemented when exposed by the CTI |
| Framegrabber DMA selection and source routing | GenTL data-stream enumeration and selection | Implemented one active stream at a time |
| Framegrabber applet/configuration files | CTI-owned GenApi features and standard feature persistence | Feature bags implemented with selector-aware verification; native applet loaders remain producer-owned |
| Gocator discovery/configuration/acquisition | A vendor GenTL producer exposing equivalent devices and features | Blocked: current module uses GoPxL/GDP and includes no CTI |
| Gocator uniform/profile/surface messages | Standard PFNC multipart range or a documented GenDC layout | Pending producer contract |
| Heliotis discovery, features, trigger, multipart acquisition | Diaphus CTI, GenApi, software trigger, GenTL parts, and generic chunk metadata | Implemented generic path; hardware validation pending |
| Heliotis stage/profile commands | GenApi nodes exported by Diaphus | Available through the feature tree; profile automation pending hardware validation |

## Feature access and XML loading

The feature tree exposes Remote Device, Data Stream, Device, Interface, and
System/TL node maps. Remote-device categories appear directly under the
`model (serial)` root. Visible readable or writable features are shown;
writable-only values remain blank/indeterminate until explicitly supplied.
Integer editors preserve signed 64-bit decimal values. Float editors preserve
double precision; node range, increment, unit, and preferred representation are
available in their tooltip. GenApi verifies numeric writes and constraints.

XML retrieval tries up to 16 advertised URLs, with variable-length URL and port
names. Local and file XML/ZIP reads are limited to 32 MiB and checked for short
reads. HTTP(S) retrieval rejects redirects, empty responses, non-200 status, and
transfers above 32 MiB. Windows uses WinHTTP with bounded phase timeouts and a
15-second deadline checked between reads; other platforms use installed libcurl
when found at configure time, with a 15-second total timeout. HTTPS certificate
verification stays enabled. Without libcurl, HTTP URLs report an explicit error
and subsequent URLs are still tried. The transfer limit does not bound ZIP
expansion inside GenApi. File URL paths decode percent escapes and use UTF-8 filesystem paths.

Set `ONEGEN_TRACE_CONNECT=1` before starting the consumer to enable monotonic
Connect stage timings. The worker reports device refresh/open, per-module XML,
stream preparation, and feature snapshots; the Qt controls report worker round
trips and GUI tree construction. Worker timing lines are forwarded to system logs
with the CTI path. Timings are disabled by default.

Controls do not periodically poll or refresh the feature tree. Operation-boundary
refreshes and queued GenApi invalidations remain. The explicit worker polling API
can advance GenApi-declared intervals and drain events; the control panel no longer
invokes it on a timer. Producers that
support `EVENT_FEATURE_INVALIDATE`, `EVENT_MODULE`, or `EVENT_REMOTE_DEVICE`
are handled with bounded event sizes and at most 32 drained messages per channel
per poll. Numeric and string generic event IDs are supported. Device event
notification features are not enabled automatically; producer compatibility and
real notification configuration still require device validation. Automatic
`EVENT_FEATURE_CHANGE` cache injection and `EVENT_ERROR` decoding are not provided.

Raw Register nodes require explicit inspection; writes require an explicit opt-in,
exact hexadecimal byte length, write access, and stopped Live. Raw access is
bounded to 64 KiB and registers are not read eagerly during tree snapshots.
The feature-tree context menu copies/restores the selected module's GenApi feature
bag through the clipboard. Restoration uses selector state and verifies values.
A failed restore can have applied earlier entries; it is not transactional.
The worker and client must both use protocol version 3.

OneGen intentionally does not emulate frame-grabber applet loaders, GoPxL REST/GDP,
or C4Hdl calls. A CTI must expose the equivalent operation through GenTL/GenApi;
otherwise that operation is not portable GenICam functionality.

```cpp
#include "OneGen.h"
#include <iostream>

onegen::OneGen gentl;
gentl.loadCti("/path/to/Producer.cti");
for (const auto& device : gentl.enumerate())
  std::cout << device.id << '\n';
```

The repository follows the Camera/Gocator layout: public headers live next to
sources in `C++/`, internals in `C++/Internal/`, `QOneGenWidget` in
`C++/Utility/Qt/`, and the Playground plugin under
`C++/Utility/PlaygroundAdapter/`. Isolated CTI workers stay in
`C++/Utility/Worker/`. The EMVA GenICam fetch cache is `genicam/`; vendor `.cti`
files are not stored in the module.

Playground composes the optional `OneGen::QtWidget`, `OneGen::PlaygroundAdapter`,
and `OneGenPlugin` targets while OneGen keeps its implementation and package
identity independent.

The Playground session contract is documented in
[Multi-Vendor CTI Runtime](docs/MULTI_VENDOR_CTI_WORKERS.md).
