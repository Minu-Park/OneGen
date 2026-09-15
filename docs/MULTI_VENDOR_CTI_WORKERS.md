# Multi-Vendor CTI Worker Architecture

## Status

This document defines the target OneGen runtime and UI contract. Isolated
workers, coordinator merging, exclusive camera leases, and Playground session
routing are implemented. Discovery uses the standard GenTL search path and
documented vendor locations; workers load those producers in place. Feature
trees and owned frames cross the worker protocol as copied values; Playground
does not load a CTI.
The design must remain vendor-neutral: no camera model, manufacturer,
transport, or PFNC-specific branch may be added to discovery, selection, worker
lifecycle, or session orchestration.

## Goals

- Present cameras rather than CTI files to the user.
- Support simultaneous cameras from multiple vendors.
- Support multiple independent OneGen sessions in Playground.
- Isolate incompatible CTI and vendor-runtime versions from each other.
- Load no CTI into the Playground process.
- Keep CTI discovery, selection, fallback, and diagnostics automatic.
- Preserve the existing OneGen GenTL/GenApi and GraphicsFrame boundaries.

## Package Layout

The plugin package owns the worker and OneGen GenApi runtime. It does not ship
vendor CTI files. Workers `dlopen` producers from the vendor install in place
so `@loader_path` / `@rpath` keep their original relative layout.

```text
plugins/
└── onegen/
    └── current/
        ├── plugin.json
        ├── OneGenPlugin.{dll,so}
        ├── OneGenWorker
        ├── runtime/
        │   └── OneGen's GenApi runtime
        └── cti/                  # optional extra producers, full vendor layout only
```

`ONEGEN_CTI_ROOT` still names the optional package `cti` directory. Automatic
discovery uses the standard GenTL search path, documented vendor locations, and
that optional overlay. Copied `.cti` files without their sibling vendor
libraries are not eligible.

## Process Boundary

Playground and `OneGenPlugin` never call `dlopen`, `LoadLibrary`, `GCInitLib`,
or another CTI entry point. A separate worker process owns each loaded CTI:

```text
Playground
└── OneGenPlugin
    └── OneGenCoordinator
        ├── Worker: producer A
        ├── Worker: producer B v1
        └── Worker: producer B v2
```

Each worker loads exactly one CTI path. Multiple cameras exposed by the same
selected CTI may share that worker. CTIs in different directories never share
a process, including two versions from the same vendor. This prevents RTTI,
symbol, DLL/dylib, environment, and process-global SDK state from crossing a
producer boundary.

A worker crash disconnects only cameras owned by that worker. It must not
terminate Playground or another producer worker.

## Discovery

1. The coordinator inventories `.cti` files from `GENICAM_GENTL64_PATH` /
   `GENICAM_GENTL32_PATH`, documented vendor locations, and optional package
   `cti/` overlay, without loading them into Playground.
2. It starts an isolated probe worker for each CTI concurrently. Each GenTL
   list update waits up to 10 s; one producer cannot stall the rest.
3. The worker loads that CTI from its vendor path, reports standard GenTL
   producer information, and enumerates interfaces and devices. It keeps those
   GenTL interface handles open so a later open does not wait another list
   update.
4. The coordinator receives copied producer and device records, never vendor
   pointers or GenApi objects.
5. Probe workers that are not selected for active cameras exit.

Each discovery record includes:

- absolute CTI path in the vendor or overlay tree;
- producer vendor, model or ID, and version when exposed by GenTL;
- transport-layer type;
- device ID, model, serial, IP, and display name when available;
- probe result, duration, and diagnostic error.

Failure of one CTI does not fail discovery through other CTIs.

## Device Identity and Producer Selection

The coordinator merges duplicate discoveries by the strongest available
vendor-neutral identity:

1. transport type plus serial number;
2. transport type plus IP address;
3. transport type plus stable GenTL device ID.

Model or display name alone is never a device identity.

One device may retain several producer candidates internally. Automatic
selection uses this order:

1. the candidate that most recently opened this device successfully;
2. the highest reported compatible version;
3. deterministic CTI path order.

If open fails, the coordinator tries the next candidate and records the
failure. The normal UI never asks the user to choose a CTI. Diagnostics may
show the selected producer and alternatives.

Only one candidate for a physical device becomes active. Two versions of the
same producer may be probed concurrently because they are process-isolated,
but they are never both used to open the same camera.

## OneGen Session UI

Creating `Device > GenICam` creates one independent OneGen session.
Before connection, the session shows only the unified camera selector,
Refresh, and Connect controls. The Feature Tree is empty and disabled.

```text
[ Camera ▼ ] [ Refresh ] [ Connect ]
------------------------------------
Feature Tree (disabled)
```

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

The module NodeMaps remain separate below those four branches so duplicate
node names cannot resolve against the wrong GenApi module.

Disconnect releases the camera and disables and clears the tree. It does not
close another OneGen session.

## Multiple Sessions

The plugin owns one process-wide coordinator, while every Playground OneGen
session owns one camera lease and one control widget. Repeating
`Device > GenICam` creates another session against the same unified
discovery snapshot.

- Sessions may connect cameras from the same CTI worker.
- Sessions may connect cameras from different vendor workers.
- A physical camera may have only one active session lease.
- Attempting a second lease reports that the camera is already in use.
- Closing a session releases only its lease.
- A worker exits after its last camera lease is released and no probe is using
  it.
- Plugin shutdown closes all workers after all sessions have been destroyed.

## Worker Protocol

Control messages use a versioned, length-prefixed local protocol over
`QLocalSocket`. Required operations are:

- probe and enumerate;
- open and close device;
- list data streams;
- read the Interface, Device, and Remote Device feature trees;
- read, write, execute, and invalidate GenApi nodes;
- single acquisition, live start, software trigger, and stop;
- worker health and orderly shutdown.

Messages use copied scalar, string, list, and binary values. Native handles,
GenApi objects, pointers, and vendor objects never cross the process boundary.

Frame messages retain owned payload bytes plus the existing OneGen PFNC,
multipart, stream, timestamp, and standard GenTL buffer diagnostics. The
Playground adapter continues converting those values to `GraphicsFrame`.
The binary payload length follows the protocol and process address space; it
is not capped to a camera-resolution assumption.

Feature invalidations are not serviced as synchronous GUI-thread RPCs during
live acquisition. Live start and stop each collect one worker-side feature
snapshot after the state transition and replace the UI snapshot once, so every
editor follows the producer's current access mode without per-node RPC bursts.

The selected data stream and announced buffers remain prepared across
acquisitions. Each acquisition owns a fresh new-buffer event so a completed or
killed event cannot leak a stale buffer notification into the next run. After
the DataStream NodeMap is attached, OneGen enables a present writable
`AutoPacketSize` / `EnableResend` pair, executes a present writable
`ProbePacketSize` command, and raises a present writable `MaxNumBuffer` to the
requested announce count so GigE resend has a queue. Missing nodes are ignored;
no camera model, manufacturer, or transport branch selects this path.
Ten buffers are announced by default, or the producer's
`STREAM_INFO_BUF_ANNOUNCE_MIN` / `MaxNumBuffer` cap when that is larger or
smaller. Before acquisition, OneGen reads the producer's stream payload size
(falling back to the standard `PayloadSize` node) and reallocates only when the
existing buffers are too small. No pixel format, resolution, camera model, or
manufacturer list controls this decision.
A producer may mark a buffer incomplete while still delivering image bytes.
OneGen keeps that frame, falls back to remote Width/Height/PixelFormat when
per-buffer layout is missing, and shortens height to the filled rows when the
tail is missing. System Logs get one incomplete diagnostic per session. The
buffer is rejected only when it cannot form an image at all.
After starting the DataStream, OneGen waits for the standard
`STREAM_INFO_IS_GRABBING` state before issuing the remote `AcquisitionStart`,
so camera packets cannot race receiver startup.

## Backpressure Decision

Live frames use **Ordered ack depth 1**: the worker waits for a host `frameAck`
before sending the next live frame. This matches the in-process
`Camera::live()` contract (grab, deliver, then grab again). `LatestOnly` and
split display-versus-record policies remain an approval boundary and are out of
scope.

Both ends of the local socket are event-driven. Socket readiness and queued
writes wake their owning event loops directly; acquisition latency and frame
rate do not depend on a polling interval.

The worker protocol must keep that queue bounded. Changing the policy is an
approval boundary because it changes acquisition semantics:

- `Ordered`: preserve every admitted frame and apply producer backpressure;
- `LatestOnly`: discard stale queued display frames in favor of the newest;
- separate recording and display policies if both are required.

Implementation must not silently choose or change this policy.

## Diagnostics

Normal UI exposes camera state only. System Logs and an optional advanced
diagnostic view may include:

- CTI path and producer version;
- worker PID and lifecycle transitions;
- discovery and fallback decisions;
- duplicate-device merge decisions;
- GenTL error code and text;
- frame rejection reason and standard buffer metadata;
- incomplete-frame GenTL buffer facts, stream counters, and a present DataStream Statistic category;
- worker crash, timeout, and restart result.

Absolute installation paths and CTI selection controls are not required in
the normal camera workflow.

## Implementation Sequence

1. Discover producers from the standard GenTL search path and documented vendor
   locations. Optional package `cti/` overlay may add extra producers that keep
   their vendor-relative library layout. Do not copy a `.cti` out of its
   install tree.
2. Add a standalone OneGen worker with probe and bounded timeout support.
   `OneGenWorker` now owns one command-line CTI, exchanges versioned
   length-prefixed JSON over a local socket, and is killed by the probe client
   when its total deadline expires.
3. Add coordinator inventory, isolated probing, device merging, and automatic
   producer selection. The coordinator now inventories the package root,
   probes each CTI with a disposable worker, and selects duplicates by stable
   identity and deterministic package path. Successful-open and producer-version
   preferences remain part of the connection phase.
4. Route connection and the three GenApi module trees through the worker
   protocol. The session Feature Tree binds copied `FeatureNode` snapshots and
   reads/writes nodes over RPC.
5. Route owned frame payloads through the worker protocol as JSON metadata plus
   trailing raw bytes (`binarySize`).
6. Support shared workers, per-session camera leases, failure isolation, and
   shutdown. Duplicate physical cameras merge by transport plus serial, IP, or
   stable GenTL device ID; only one lease is active per identity.
7. Remove in-process CTI loading from `OneGenPlugin`. The plugin owns the
   coordinator only; `OneGenWorker` is the sole process that `dlopen`s a CTI.

Each phase must keep the standalone OneGen checks, adapter tests, parent plugin
build, and `git diff --check` passing. Multi-vendor acceptance requires at
least two CTIs active concurrently, two OneGen sessions, duplicate-device
deduplication, one worker failure without host failure, and verification that
no CTI or vendor runtime is loaded into the Playground process.

## Non-Goals

- Implementing vendor discovery protocols outside GenTL.
- Selecting a CTI from the normal camera UI.
- Adding camera-model-specific discovery or lifecycle code.
- Loading two CTIs into one worker.
- Passing vendor objects across the process boundary.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
