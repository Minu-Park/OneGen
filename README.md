# OneGen

OneGen is a C++20 GenTL consumer library. It loads CTI producers at runtime,
enumerates cameras, acquires owned frame payloads, and exposes GenApi features.

## Build

Requirements: CMake 3.24+, a C++20 compiler, and network access for the pinned
EMVA GenICam package. For extracted local packages, set `ONEGEN_GENICAM_ROOT`,
`ONEGEN_GENICAM_RUNTIME_ROOT`, and `ONEGEN_GENTL_INCLUDE`. Qt 6 Widgets is
optional and only enables `onegen-viewer`.

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
`GENICAM_GENTL32_PATH`, documented vendor locations, and an optional package
overlay. Isolated workers load those files from the vendor install so their
relative library paths remain valid. Do not copy a CTI without its producer
tree.

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
| Framegrabber applet/configuration files | CTI-owned GenApi features and standard feature persistence | Implemented when exposed by the CTI |
| Gocator discovery/configuration/acquisition | A vendor GenTL producer exposing equivalent devices and features | Blocked: current module uses GoPxL/GDP and includes no CTI |
| Gocator uniform/profile/surface messages | Standard PFNC multipart range or a documented GenDC layout | Pending producer contract |
| Heliotis discovery, features, trigger, multipart acquisition | Diaphus CTI, GenApi, software trigger, GenTL parts, and generic chunk metadata | Implemented generic path; hardware validation pending |
| Heliotis stage/profile commands | GenApi nodes exported by Diaphus | Available through the feature tree; profile automation pending hardware validation |

OneGen intentionally does not emulate frame-grabber applet loaders, GoPxL REST/GDP,
or C4Hdl calls. A CTI must expose the equivalent operation through GenTL/GenApi;
otherwise that operation is not portable GenICam functionality.

```cpp
#include <onegen/onegen.hpp>
#include <iostream>

onegen::OneGen gentl;
gentl.loadCti("/path/to/Producer.cti");
for (const auto& device : gentl.enumerate())
  std::cout << device.id << '\n';
```

Playground composes the optional `OneGen::PlaygroundAdapter` and `OneGenPlugin`
targets while OneGen keeps its implementation and package identity independent.

The proposed process-isolated multi-vendor CTI package and session contract is
documented in [Multi-Vendor CTI Worker Architecture](docs/MULTI_VENDOR_CTI_WORKERS.md).
