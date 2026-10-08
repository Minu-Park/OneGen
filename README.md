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
`GENICAM_GENTL32_PATH`, without vendor-specific paths or filenames. Discovery starts one
`OneGenWorker` process per CTI; the first camera adopts that worker, and additional
cameras use separate workers so Open cannot close another camera. Successfully
closed camera workers retain their discovery caches for reconnect. Each worker preserves the producer install directory for
relative dependencies while isolating incompatible vendor runtimes. The worker
uses a versioned binary stdin/stdout protocol and has no Qt dependency. Do not
copy a CTI without its producer tree.

The selector explicitly distinguishes every enumerated Interface from Device entries.
InterfaceIDs are opaque and producer-scoped; DeviceIDs are Interface-scoped.
Opening an Interface requires no manufacturer adapter or attached Device. Connected
Interfaces share one worker with their independent Device/DataStream sessions.
Physical-board equivalence across producers or SDKs is not inferred; native
Producer/SDK resource arbitration determines whether a connection is available.

The optional consumer adapter's `OneGenSourceController` owns Device leases and
declares `device.targets`, `device.connection`, `device.discover`, `device.connect`,
`device.disconnect`, `device.single`, `device.live-start` and `device.live-stop`.
Consumers discover schemas through `IToolProvider`; views borrow lease references
and use the same module commands. A target retains producer, Interface and Device
identity; each connection gets a fresh UUID. Pending GUI/API/discovery or Interface
work is refused as Busy, and accepted external work drains before teardown.
Explicit close and Live stop retain state on backend refusal. Closed leases reject
RPCs even when their worker is reused. Single completes after frame conversion;
consumer presentation and artifact export remain separate authorities.

The source controller also implements the consumer's optional
`IDeviceParameterControl` contract. Existing `Lease::describe_features`,
`read_node` and `write_node` provide typed inventory, reads, writes and commands
for `RemoteDevice`, single-stream `DataStream` and `Device` scopes. Multi-stream
DataStream parameters are refused because the existing Lease cannot expose the
selected stream identity needed to fence GUI selection changes. Connection UUIDs, current
transitive selector values and query-observed revisions qualify targets. Integer
values remain signed 64-bit; SDK readback may differ from a requested value.
After the existing setter completes preparation, the adapter reads again through
the existing getter. Failed post-write observation retains an applied effect
and explicitly unavailable readback.
Unreadable values are absent and incomplete selector observations refuse writes.
GUI operations and external requests share control admission; raw-register modal
inspection also holds that admission. Writes never select a context or stop Live
implicitly. TriggerSoftware retains the existing trigger/receive path.
Read and write are separate existing worker calls: revisions detect observed
changes, not unobserved changes that return to the same state, and do not provide
an atomic fence against producer/device changes between calls. Interface/System
parameters, raw-register API writes, multiple-connection lifecycle control and
cancellation of pending Single remain outside this contract.

GenTL 1.5 multipart buffers are retained without a vendor SDK. The Playground
adapter assembles standard PFNC `Coord3D_ABC32f` and `Coord3D_C16` range data
with matching mono intensity and confidence parts. SFNC coordinate scale,
offset, invalid-value, and distance-unit features are snapshotted before
acquisition; missing calibration is rejected instead of guessed.

GenDC parsing, vendor motion/trigger recipes, and proprietary pixel formats are
not GenTL services and still require a documented standard payload or an explicit
device-family profile before removing their SDK paths.

## Vendor SDK replacement map

| Existing module capability | GenICam replacement in OneGen | Status |
| --- | --- | --- |
| Camera discovery, connection, feature tree, Single/Live/Stop | GenTL system/interface/device/stream plus remote GenApi | Implemented |
| Camera 2D image delivery | PFNC image payload, standard GenTL JPEG payload, and owned buffer copy | Implemented; unpacked multi-byte pixels convert both byte orders; packed `p` stays LSB-first |
| Camera multipart XYZ, intensity, confidence | GenTL parts plus PFNC and SFNC Scan3d metadata | Implemented for unpacked/packed C/AC/ABC, 64f, disparity, RectifiedAC, and spherical/cylindrical reconstruction |
| Camera stereo DisparityC | SFNC baseline, focal length, and optional principal-point reconstruction | Implemented; hardware pending |
| Camera GenDC | GenDC descriptor parsing | Pending payload evidence |
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
The worker and client must both use protocol version 12.

Copied `FeatureNode` observations include direct GenApi selecting features,
including hidden or unreadable selectors. Selector records are shallow copies;
their readable flags and observation errors determine whether a value is known.
Integer values retain signed 64-bit precision through the worker protocol and
diagnostic JSON. Value/constraint observation failures are explicit rather than
successful default values. These observations do not change selectors or provide
an atomic write-validation/acquisition fence; consumer feature commands must
revalidate connection and selector context at their owning execution boundary.

Single-image payloads are copied from the producer's image offset using the
declared row stride and delivered height; prefix bytes, Y padding and trailing
chunk metadata are excluded. Available chunk descriptors are validated against
delivered bytes and retained in `Frame::chunks` as owned ID/offset/raw-byte data,
including through worker IPC. Chunk-only and other non-image payloads retain
their raw bytes but are not interpreted as images by the graphics adapter.
Chunk geometry is read while attached to the GenApi node map; general decoded
chunk values are not exposed by this raw-byte contract. Compressed images with
attached chunks require a known image boundary. Physical producer compatibility
still requires captured-buffer validation.

3D conversion supports unpacked PFNC C, AC and ABC at 8/16-bit integer and 32/64-bit
float depth, plus packed 10/12-bit `p` layouts. AC/ABC support interleaved and planar
storage in either byte order. CalibratedC needs only C calibration, including a readable
C-only selector or selector-free scalar calibration identified by an explicit CalibratedC
mode. RectifiedC requires all three calibrated axes and applies physical XY sampling.
CalibratedAC reconstructs B from the declared uniform row scale/offset. ABC grid
payloads retain explicit XYZ; CalibratedABC_PointCloud produces only unorganized
points from the RangeFrame world domain so meter/inch conversion cannot split Z
from XY, with invalid points removed and auxiliary fields kept aligned. Separate
A/B/C planes must have unique matching source/region IDs, dimensions and namespace.
Auxiliary fields must match the selected range source/region. UncalibratedC is a
native scalar preview with unspecified physical XY, not calibrated geometry.
DisparityC reconstructs Z as baseline*focal/disparity when those nodes exist;
principal-point projection is applied when U/V are present. RectifiedAC uses the
same A/C samples and uniform B row reconstruction as CalibratedAC. Spherical and
Cylindrical ABC/AC/RectifiedC samples are converted to Cartesian using radians after
scale/offset. Meter and inch distance units convert physical axes to millimetres
and leave PixelGrid X/Y as pixel indices. Confidence1 and Confidence1p are preserved
as 0/1 auxiliaries. Calibration availability, output mode, coordinate system and
stereo parameters survive worker IPC.
Endian identifier 0 retains the legacy native-host interpretation; explicit
little/big endian identifiers use byte-order conversion for unpacked multi-byte
coordinates and 2D images. Packed `p` formats stay LSB-first.
Known encoder-dependent Linescan modes, Scan3d transform matrices, GenDC containers,
multiple simultaneous range presentations and per-part calibration selector contexts
are not implemented. They require their own metadata and presentation contracts;
no vendor-specific acquisition bypass or invented physical calibration is used.

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

## Qt feature browsing

`OneGenFeatureTree` derives from the Qt-only `SearchableTreeWidget` in
`Resources::Widgets`. Enable the widget target only after the consumer has made
that target available (for example with `add_subdirectory(path/to/Resources)`).
No theme installation is required; GenTL/core-only builds do not depend on it.
OneGen provides internal node search text, snapshot row identities and the
unchanged node-map/internal-name favorite identities. It uses the consumer's
explicit settings file/key or `OneGenFeatures.ini` for standalone widgets;
these stores remain independent and no legacy list is imported implicitly.
