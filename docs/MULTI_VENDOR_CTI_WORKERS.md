# Multi-Vendor CTI Runtime

## Status

This document defines the OneGen Playground runtime and UI contract. The plugin
starts one generic worker process for each producer found on the standard GenTL
search path, merges only identical scoped Device records, and
maintains independent Device leases. Feature trees bind live GenApi module maps. Owned frames
convert to `GraphicsFrame` inside the plugin.

The design must remain vendor-neutral: no camera model, manufacturer,
transport, or PFNC-specific branch may be added to discovery, selection, or
session orchestration.

Each worker loads exactly one CTI. This prevents same-named but ABI-incompatible
GenApi and logging DLLs from different vendors from sharing one Windows process.
The worker is C++20 plus OneGen only; Qt remains in the host-side client.

## Goals

- Explicitly distinguish every enumerated Interface and Device.
- Support simultaneous cameras from producers the system GenTL path can load
  together.
- Support multiple independent OneGen sessions in Playground.
- Discover CTIs automatically; connect only the explicitly selected producer-scoped target.
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

Automatic discovery uses only the architecture-specific standard GenTL search path. Copied `.cti` files without their sibling vendor libraries are not
eligible.

## Discovery

1. The plugin inventories `.cti` files from `GENICAM_GENTL64_PATH` /
   `GENICAM_GENTL32_PATH`. There are no manufacturer default paths or filenames.
   Canonical path duplicates and aliases of the same physical file are removed.
   Identical filenames, versions, or copied bytes do not prove equivalent sibling
   DLL and configuration environments; distinct installed producers remain candidates.
2. It starts one worker for each path. That worker calls `OneGen::loadCti()`;
   a failed producer is logged and skipped without failing other CTIs.
3. Workers report standard Interface and Device records from every loaded producer.
   Interface identity is `(CTI path, InterfaceID)` and Device identity is
   `(CTI path, InterfaceID, DeviceID)`. IDs are opaque and case-sensitive.
   Serial numbers, IP addresses, transport types, model names, and equal CTI
   filenames never establish cross-producer physical equivalence.

Default discovery uses GENICAM_GENTL32_PATH or GENICAM_GENTL64_PATH. A consumer
may supply an explicit CTI search directory. IP metadata comes from standard
GenTL SFNC selector features, not address-shaped IDs or labels. Every Interface,
including network and camera-less Interfaces, remains selectable. Producer
paths in selector tooltips distinguish targets with equal display names.

The plugin preflight validates the worker package and returns value-only session
data immediately. After GUI-thread construction, the control panel schedules
discovery on a separate future. Each completed producer publishes a cumulative
merged device list on the GUI thread. Connection and acquisition do not wait for
unrelated producers. Refresh remains unavailable until its discovery finishes.
Discovery runs are serialized per coordinator; process isolation does not confer
exclusive ownership of hardware shared by different producer installations.
Shutdown suppresses pending GUI updates and joins discovery before teardown.

Probe responses in protocol v10 carry both Devices and producer-scoped Interface
failure details. An individual Interface failure preserves other Interfaces and
their Devices, appears in the cumulative result, and is not retried as an empty
inventory. Successful empty inventories retain the initial-discovery budget.
An idle worker with a failed probe is retired before publishing its result so
native resources retained during failed initialization are released. Connected
or opening Interface/Device sessions prevent retirement of their worker.

Worker diagnostics preserve OneGen severity tags and leading producer severity
fields, including timestamped `NOTICE:` and `ERROR:` records. Unclassified
producer output retains the warning fallback; message-body keywords do not
change severity. Vendor fatal diagnostics are forwarded as critical messages
without terminating the consumer.

Worker C++ failures retain their operation/request/session context on diagnostic
stderr; binary protocol stdout remains separate. Standard request and single-acquisition
exceptions return failed replies and log at INFO; unknown request exceptions retain
WARN severity. Fatal worker startup/IO failures
return a failed process or close the connection and wake pending requests. Unknown
C++ exceptions follow the same failure boundary as standard exceptions. A frame
consumer exception keeps the existing acknowledgement/continued-delivery behavior
and logs its detail, session and CTI; error/invalidation callback failures close
the connection rather than escaping the IO thread. Startup errors include worker
and CTI paths. Exit diagnostics include decimal and hexadecimal process codes and
retain the final tagged warning/error record. These guards do not recover native
access violations, CRT assertions or process fail-fast; those remain isolated
worker/process failures. No automatic retry or producer ownership change is added.

`test_worker_errors` injects startup, invalid inactive-state, worker argument and
standard/unknown callback failures. It verifies explicit nonzero exits, diagnostic
context, clean protocol stdout, request wake-up and continued frame delivery.

## OneGen Session UI

Creating `Device > GenICam` creates one independent OneGen session.
Device selection preserves the original single-camera row and an empty feature
view until the Device opens. Interface selection shows the selector and its
connection action without constructing a Device panel. Opening an Interface
requires neither a manufacturer adapter nor an attached Device.

After Interface connection, the selector is locked and the panel shows an
Interface settings tab plus one tab per discovered Device. Each Device connects,
disconnects, and acquires independently; switching tabs never changes connection
state. Camera-less Interfaces expose settings and rescan with acquisition
unavailable. Interface and Device panels share Resources control and tree styling.

Closing one Device releases only its DataStream and Device. Interface disconnect
drains all child operations before closing the Interface. Reconfiguration rejects
affected active or opening Device leases. Successfully closed workers are retained
for reconnect; shutdown releases them.

## Multiple Sessions

The plugin owns one coordinator. An explicitly connected Interface pins one
producer worker and its Device channels share that worker through protocol v10.
Standalone Device connections retain the existing worker-isolation policy.

- Duplicate local opens of the same complete Device identity are rejected.
- Distinct producers reporting equal InterfaceIDs remain distinct targets.
- Discovery releases temporary Interfaces while retaining active or pinned ones.
- Physical ownership across producers and SDKs follows native resource arbitration.
  Automatic board-serial matching and cross-module software claims are removed;
  the consumer explicitly chose native arbitration. No proactive cross-producer
  physical equivalence guarantee is claimed.

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
- Inferring physical aliases or hardware classes from manufacturer, transport, or ID strings.
- Adding camera-model-specific discovery or lifecycle code.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
