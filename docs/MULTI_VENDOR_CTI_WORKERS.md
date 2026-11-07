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
  onegen/
    current/
      plugin.json
      OneGenPlugin.{dll,so}
      OneGenWorker{.exe}
      runtime/
        OneGen's GenApi runtime
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

Probe responses in protocol v15 carry both Devices and producer-scoped Interface
failure details. An individual Interface failure preserves other Interfaces and
their Devices, appears in the cumulative result, and is not retried as an empty
inventory. Successful empty inventories retain the initial-discovery budget.
An idle worker with a failed probe is retired before publishing its result so
native resources retained during failed initialization are released. Connected
or opening Interface/Device sessions prevent retirement of their worker.

Worker maps use one canonical producer path identity, so equivalent forward-
and backslash spellings of the same CTI share the retained discovery worker.
If that discovery-only worker rejects a Device open with an Interface/resource-
busy failure, the coordinator retires it once per CTI for that open call and
retries with a fresh process. It never retires while discovery is running or
while a worker request is active; the connection enters a retiring state that
rejects new requests before close. Active-request drain happens outside the
coordinator map lock with a two-second deadline; Shutdown remains admitted
during retirement, and a timed-out drain force-closes the worker. Validation,
identity and timeout failures do not replace the worker. A pinned Interface,
active Device, or live lease also prevents retirement and retry.

Idle search reset snapshots the retained workers, reserves every snapshotted
producer lifecycle, and revalidates that no Device claim, Interface session or
new producer worker appeared before it transfers map ownership. A failed final
check restores the reservations and leaves the workers in place; retained
workers drain active requests before close, and worker shutdown runs outside
the coordinator lock. Other producers are not serialized by a reset.

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
producer worker and its Device channels share that worker through protocol v15.
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

## Frame transport diagnostics

Set `ONEGEN_TRACE_FRAMES=1` in the environment before starting Playground and
enable Debug capture. The worker inherits the variable. Records use the
producer FrameID as `frame`, so worker, host and adapter stages correlate; a
producer that reports FrameID 0 remains uncorrelated. The flag is read once per
process; leave it unset for normal operation. With the variable unset, no
per-frame record, allocation or counter update is added, and
delivery/acknowledgement behavior is unchanged. Host and adapter records are
Debug records and are discarded when Debug capture is off; the worker record is
controlled by the variable because the worker cannot observe the consumer's
toggle.

- Worker: `serialize_ms` (Frame copy into the protocol Writer), `encode_ms`
  (protocol envelope), `write_ms` (stdout write and flush), `ack_wait_ms`
  (delivery acknowledgement wait).
- Host: `read_ms` (QProcess read plus `QByteArray` append, attributed to the
  first Frame parsed from that read), `extract_ms` / `decode_ms`
  (`takeMessage` body handling and decode), `payload_ms` (`readFrame` payload
  copy), `queue_ms` (message to delivery thread), `callback_ms` (delivery
  callback).
- Adapter: `conversion_ms` (`oneGenGraphicsFrame`), `consumer_ms`
  (SessionFrame admission).

Owned live-path copies are: GenTL buffer -> `Frame.payload`, `Frame` ->
protocol Writer, Writer -> encoded message, stdout -> `QByteArray`, message
decode, `readFrame`, delivery queue handoff, and adapter
`make_shared<vector>` storage. The intermediate SHM milestone removes the
stdout, decode and adapter copies; the final milestone retains only the
GenTL -> SHM copy.

The manual 127 MB synthetic baseline uses the test executable as its own fake
worker. Run it from the OneGen build directory with three or more frames:

```sh
ONEGEN_TRACE_FRAMES=1 test_worker --profile-large-frames 3
```

The fake peer emits 13376x9528 Mono8 frames (127,446,528 bytes each) with
sequential FrameIDs. This mode is not part of the default CTest suite and does
not replace physical camera acceptance.

The physical live profile uses the real worker and an explicitly selected
producer directory and device:

```sh
ONEGEN_TRACE_FRAMES=1 test_worker --profile-live <OneGenWorker> <cti-directory> <device-id> 20
```

`--profile-discovery <OneGenWorker> <cti-directory>` prints the producer-scoped
device IDs. The live profile records worker and host stages; presentation and
script stages remain Playground-owned measurements.

Interface and device lifecycle detail records are Debug records. `IFOpen`,
`IFClose`, `IFCloseAll`, `pin`, `idle-release`, `DeviceOpen` and `DeviceClose`
include retained `opened`, `pinned` and `activeDevices` counts with the CTI
path.

## Bounded frame transport contract (approved)

This contract was approved on 2026-11-07 for the R1/R3 implementation and
remains experimental until the physical acceptance gate in #348 passes.

- **Pool ownership:** the host creates the OS-backed region and maps it
  read-only; the worker opens the unguessable region name and maps it
  read/write. A worker crash cannot unmap bytes that consumer leases still
  reference. The host releases the old mapping only after the last lease dies.
- **Budget:** three slots of the selected payload size per session. For
  `13376x9528` BayerRG8 this is `3 * 127,446,528 = 382,339,584` bytes. Retired
  but referenced pools remain in the accounting; allocation fails explicitly
  instead of growing without bound.
- **Slot states:** `free -> reserved -> filling -> ready -> released` with an
  exact `(poolEpoch, session, slot, slotGeneration)` identity. A slot is
  reusable only after a matching release. Duplicate, stale and unknown
  releases are ignored and diagnosable in Debug.
- **Publication:** the worker copies the GenTL image into the reserved slot
  while the borrowed buffer is pinned, then publishes only a small
  `FrameReady` descriptor. No GenTL pointer, writable mapping or 127 MB body
  crosses the stdout protocol on the optimized path.
- **Backpressure:** frame and byte credits are bounded per session. When no
  slot is free, acquisition paces at the single documented source admission
  boundary. Ordered and Strict policy remain unchanged; LatestOnly stays
  opt-in with explicit loss accounting. No new wait is stacked on the existing
  acknowledgement, Framegrabber permit or SessionFrame admission.
- **Copy target:** direct GenTL -> SHM for eligible single-image payloads; one
  intentional image copy. Unsupported layouts use the existing owned payload
  path explicitly and are counted; the intermediate `Frame.payload -> SHM`
  milestone is not a permanent product path.
- **Lifetime:** callback completion is not proof that the last consumer
  released the frame. The host storage owner enqueues `SlotRelease` from its
  destructor without touching a QProcess or an application mutex in an
  arbitrary thread.
- **Failure policy:** descriptors and releases are validated before use
  (bounds, stride, layout, epoch, generation, duplicate slot). Malformed input
  fails closed for that frame; worker/session epochs are invalidated before
  stale work can be delivered; all waits wake on stop, close, exit and
  reconnect. No silent loss: source, admitted, delivered, rejected and
  policy-dropped counts stay separate.
- **Protocol:** worker protocol v15 carries the pool attach/detach,
  `FrameReady` and `SlotRelease` messages. Mismatched peers are rejected; no
  compatibility shim is added before v1.0.0.

R1 currently implements the host client and the fake test peer: ordered pool
attach/detach transitions, a per-session three-slot byte budget that keeps
retired lease-backed mappings charged, PFNC storage-size row/stride validation,
exact releases, detach callback fencing (every external detach drains the
retiring epoch, queued frames are rejected, and a callback may detach its own
pool), malformed-frame rejection and retained-mapping safety are covered by
`test_worker`. The production worker
rejects pool attach until the direct GenTL-to-SHM acquisition path lands, so
the optimized transport is not enabled and the existing owned-payload path
remains the live default.

## Non-Goals

- Implementing vendor discovery protocols outside GenTL.
- Inferring physical aliases or hardware classes from manufacturer, transport, or ID strings.
- Adding camera-model-specific discovery or lifecycle code.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
