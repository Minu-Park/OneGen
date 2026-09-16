# Multi-Vendor CTI Runtime

## Status

This document defines the OneGen Playground runtime and UI contract. The plugin
starts one generic worker process for each producer found on the standard GenTL
search path and documented vendor locations, merges duplicate cameras, and
holds exclusive opens. Feature trees bind live GenApi module maps. Owned frames
convert to `GraphicsFrame` inside the plugin.

The design must remain vendor-neutral: no camera model, manufacturer,
transport, or PFNC-specific branch may be added to discovery, selection, or
session orchestration.

Each worker loads exactly one CTI. This prevents same-named but ABI-incompatible
GenApi and logging DLLs from different vendors from sharing one Windows process.
The worker is C++20 plus OneGen only; Qt remains in the host-side client.

## Goals

- Present cameras rather than CTI files to the user.
- Support simultaneous cameras from producers the system GenTL path can load
  together.
- Support multiple independent OneGen sessions in Playground.
- Keep CTI discovery, selection, fallback, and diagnostics automatic.
- Preserve the existing OneGen GenTL/GenApi and GraphicsFrame boundaries.

## Package Layout

The plugin package owns OneGen GenApi runtime. It does not ship vendor CTI
files and does not own a package `cti/` overlay.

```text
plugins/
└── onegen/
    └── current/
        ├── plugin.json
        ├── OneGenPlugin.{dll,so}
        ├── OneGenWorker{.exe}
        └── runtime/
            └── OneGen's GenApi runtime
```

Automatic discovery uses the standard GenTL search path and documented vendor
locations. Copied `.cti` files without their sibling vendor libraries are not
eligible.

## Discovery

1. The plugin inventories `.cti` files from `GENICAM_GENTL64_PATH` /
   `GENICAM_GENTL32_PATH` and documented vendor locations.
   Canonical path duplicates and aliases of the same physical file are removed.
   Identical filenames, versions, or copied bytes do not prove equivalent sibling
   DLL and configuration environments; distinct installed producers remain candidates.
2. It starts one worker for each path. That worker calls `OneGen::loadCti()`;
   a failed producer is logged and skipped without failing other CTIs.
3. The workers report devices from every successfully loaded
   producer. Duplicate physical cameras merge by the strongest available
   vendor-neutral identity:
   1. transport type plus serial number;
   2. transport type plus IP address;
   3. transport type plus stable GenTL device ID.

Model or display name alone is never a device identity. The normal UI never
asks the user to choose a CTI.

The plugin preflight validates the worker package and returns value-only session
data immediately. After GUI-thread construction, the control panel schedules
discovery on a separate future. Each completed producer publishes a cumulative
merged device list on the GUI thread. Connection and acquisition do not wait for
unrelated producers. Refresh remains unavailable until its discovery finishes.
Discovery runs are serialized per coordinator; process isolation does not confer
exclusive ownership of hardware shared by different producer installations.
Shutdown suppresses pending GUI updates and joins discovery before teardown.

## OneGen Session UI

Creating `Device > GenICam` creates one independent OneGen session.
Before connection, the session shows only the unified camera selector,
Refresh, and Connect controls. The Feature Tree is empty and disabled.

After connection, the camera selector is locked and the Feature Tree is
enabled. Its root is the model plus serial number, falling back to the stable
device ID only when either field is unavailable:

```text
Camera Model (Serial)
├── Remote Device
├── Data Stream
├── Device
└── Interface
```

Disconnect releases the camera and disables and clears the tree. It does not
close another OneGen session.

## Multiple Sessions

The plugin owns one coordinator and one worker connection per CTI. Every
Playground OneGen session owns one remote camera lease and one control widget.

- Sessions may connect cameras exposed by the same loaded producer.
- A physical camera may have only one active open.
- Attempting a second open reports that the camera is already in use.
- Closing a session releases only its camera.

## Acquisition

Live frames use the worker `Camera::live()` callback and a one-frame
acknowledgement: grab, deliver, acknowledge, then grab again. `LatestOnly` and
split display-versus-record policies remain an approval boundary.

The selected data stream and announced buffers remain prepared across
acquisitions. Ten buffers are announced by default, or the producer's
`STREAM_INFO_BUF_ANNOUNCE_MIN` / `MaxNumBuffer` cap when that is larger or
smaller. A producer may mark a buffer incomplete while still delivering image
bytes. OneGen keeps that frame and rejects it only when it cannot form an
image.

## Non-Goals

- Implementing vendor discovery protocols outside GenTL.
- Selecting a CTI from the normal camera UI.
- Adding camera-model-specific discovery or lifecycle code.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
