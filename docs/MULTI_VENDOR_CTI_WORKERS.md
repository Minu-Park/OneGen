# Multi-Vendor CTI Worker Architecture

## Status

This document defines the proposed OneGen runtime and UI contract. It is not
implemented yet. The design must remain vendor-neutral: no camera model,
manufacturer, transport, or PFNC-specific branch may be added to discovery,
selection, worker lifecycle, or session orchestration.

## Goals

- Present cameras rather than CTI files to the user.
- Support simultaneous cameras from multiple vendors.
- Support multiple independent OneGen sessions in Playground.
- Isolate incompatible CTI and vendor-runtime versions from each other.
- Load no CTI into the Playground process.
- Keep CTI discovery, selection, fallback, and diagnostics automatic.
- Preserve the existing OneGen GenTL/GenApi and GraphicsFrame boundaries.

## Package Layout

Every CTI used by the Playground OneGen plugin belongs to its independently
versioned plugin package:

```text
plugins/
└── onegen/
    └── current/
        ├── plugin.json
        ├── OneGenPlugin.{dll,so}
        ├── runtime/
        │   └── OneGen's GenApi runtime
        └── cti/
            ├── <producer-key-a>/
            │   ├── ProducerA.cti
            │   └── vendor runtime in its required relative layout
            ├── <producer-key-b-v1>/
            │   ├── ProducerB.cti
            │   └── vendor runtime
            └── <producer-key-b-v2>/
                ├── ProducerB.cti
                └── vendor runtime
```

The canonical path uses the lowercase package identifier:
`plugins/onegen/current/cti`. A producer directory must preserve the relative
layout required by that CTI's loader paths. Different versions must use
different producer directories even when their CTI filenames match.

The plugin package owns this complete tree. The host may copy and validate the
declared payload generically, but it must not maintain a vendor-specific CTI
file list. Only trusted CTIs distributed through or explicitly installed into
the OneGen package are eligible for automatic discovery.

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

1. The coordinator recursively inventories trusted `.cti` files below the
   package `cti` directory without loading them.
2. It starts an isolated probe worker for each CTI with a bounded timeout.
3. The worker loads its CTI, reports standard GenTL producer information, and
   enumerates interfaces and devices.
4. The coordinator receives copied producer and device records, never vendor
   pointers or GenApi objects.
5. Probe workers that are not selected for active cameras exit.

Each discovery record includes:

- CTI package-relative path;
- producer vendor, model or ID, and version when exposed by GenTL;
- transport-layer type;
- device ID, model, serial, MAC, IP, and display name when available;
- probe result, duration, and diagnostic error.

Failure of one CTI does not fail discovery through other CTIs.

## Device Identity and Producer Selection

The coordinator merges duplicate discoveries by the strongest available
vendor-neutral identity:

1. transport type plus serial number;
2. transport type plus MAC address;
3. transport type plus stable GenTL device ID.

Model or display name alone is never a device identity.

One device may retain several producer candidates internally. Automatic
selection uses this order:

1. the candidate that most recently opened this device successfully;
2. the highest reported compatible version;
3. deterministic package-relative path order.

If open fails, the coordinator tries the next candidate and records the
failure. The normal UI never asks the user to choose a CTI. Diagnostics may
show the selected producer and alternatives.

Only one candidate for a physical device becomes active. Two versions of the
same producer may be probed concurrently because they are process-isolated,
but they are never both used to open the same camera.

## OneGen Session UI

Creating `Device > OneGen Camera` creates one independent OneGen session.
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
├── Device
└── Interface
```

The module NodeMaps remain separate below those three branches so duplicate
node names cannot resolve against the wrong GenApi module.

Disconnect releases the camera and disables and clears the tree. It does not
close another OneGen session.

## Multiple Sessions

The plugin owns one process-wide coordinator, while every Playground OneGen
session owns one camera lease and one control widget. Repeating
`Device > OneGen Camera` creates another session against the same unified
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

## Backpressure Decision

The worker protocol must use a bounded frame queue. The exact policy remains
an approval boundary because it changes acquisition semantics:

- `Ordered`: preserve every admitted frame and apply producer backpressure;
- `LatestOnly`: discard stale queued display frames in favor of the newest;
- separate recording and display policies if both are required.

Implementation must not silently choose or change this policy.

## Diagnostics

Normal UI exposes camera state only. System Logs and an optional advanced
diagnostic view may include:

- CTI package-relative path and producer version;
- worker PID and lifecycle transitions;
- discovery and fallback decisions;
- duplicate-device merge decisions;
- GenTL error code and text;
- frame rejection reason and standard buffer metadata;
- worker crash, timeout, and restart result.

Absolute installation paths and CTI selection controls are not required in
the normal camera workflow.

## Implementation Sequence

1. Stage and validate `plugins/onegen/current/cti/<producer>/...`.
2. Add a standalone OneGen worker with probe and bounded timeout support.
3. Add coordinator inventory, isolated probing, device merging, and automatic
   producer selection.
4. Route connection and the three GenApi module trees through the worker
   protocol.
5. Route owned frame payloads through the worker protocol.
6. Support shared workers, per-session camera leases, failure isolation, and
   shutdown.
7. Remove in-process CTI loading from `OneGenPlugin`.

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
- Automatically accepting incomplete or undersized image buffers.
