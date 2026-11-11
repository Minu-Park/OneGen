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

Probe responses in protocol v16 carry both Devices and producer-scoped Interface
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
consumer exception keeps the existing delivery/continued-delivery behavior
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
producer worker and its Device channels share that worker through protocol v16.
Standalone Device connections retain the existing worker-isolation policy.

- Duplicate local opens of the same complete Device identity are rejected.
- Distinct producers reporting equal InterfaceIDs remain distinct targets.
- Discovery releases temporary Interfaces while retaining active or pinned ones.
- Physical ownership across producers and SDKs follows native resource arbitration.
  Automatic board-serial matching and cross-module software claims are removed;
  the consumer explicitly chose native arbitration. No proactive cross-producer
  physical equivalence guarantee is claimed.

## Acquisition

Live frames use the worker `Camera::live()` callback. Acquisition never waits
for the consumer: a non-direct frame needs the single worker/client transport
credit, and a frame that arrives while the credit is held is dropped before
serialization and counted. The client returns the credit when its delivery
thread reads the queued frame, before the consumer callback. `LatestOnly` and
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
per-frame record, allocation or counter update is added, and delivery behavior
is unchanged. Host and adapter records are
Debug records and are discarded when Debug capture is off; the worker record is
controlled by the variable because the worker cannot observe the consumer's
toggle.

- Worker: `serialize_ms` (Frame copy into the protocol Writer), `encode_ms`
  (protocol envelope), `write_ms` (stdout write and flush), `transport_dropped`
  (non-direct drops when the transport credit was held).
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

## Live telemetry and consumer Debug

The consumer's Debug switch is observed through the `diagnostics.OneGen` Qt
logging category: Debug enables its debug output, and there is no other
dependency on the consumer's logging implementation. The panel re-reads the
category on each one-second telemetry tick, so a toggle during acquisition
takes effect without restarting it.

- Debug only: the `fps` and `failed` statusbar bubbles, the five-second
  `[GenICam] diagnostics:` line, the per-run first-frame detail
  (`first frame received/chunk count/part/converted`, effective Scan3d) and the
  component node inventory taken when a lease is bound. Counters and FPS
  sampling keep running while Debug is off, so values are current when it is
  enabled.
- Always retained: the per-run `[GenICam] live diagnostics:` stop summary
  (received, delivered, failed, incomplete) and every warning or error,
  including incomplete-frame and rejected-frame records.

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
- **Slot states:** `free -> reserved -> ready -> released` with an exact
  `(poolEpoch, session, slot, slotGeneration)` identity. A slot is reusable
  only after a matching release. Duplicate, stale and unknown releases are
  ignored and diagnosable in Debug.
- **Publication:** the worker copies the GenTL image into the reserved slot
  while the borrowed buffer is pinned, then publishes only a small
  `FrameReady` descriptor. No GenTL pointer, writable mapping or 127 MB body
  crosses the stdout protocol on the optimized path.
- **Backpressure:** the slot ring is the bound. When no slot is free, the
  worker dequeues and requeues one frame without a direct-slot copy (chunk and
  multipart layouts still extract their metadata) and increments
  `slot_exhausted` for that dropped frame; it never waits for the consumer.
  The non-direct stdout path keeps one transport credit and counts
  `transport_dropped` before serialization when the credit is held. Ordered and
  Strict policy remain unchanged; LatestOnly stays opt-in with explicit loss
  accounting. No source admission wait remains in the worker.
- **Copy target:** direct GenTL -> SHM for eligible single-image payloads; one
  intentional image copy. Unsupported layouts use the existing owned payload
  path explicitly and are counted; the intermediate `Frame.payload -> SHM`
  milestone is not a permanent product path.
- **Lifetime:** callback completion is not proof that the last consumer
  released the frame. The host storage owner enqueues `SlotRelease` from its
  destructor without touching a QProcess or an application mutex in an
  arbitrary thread. A worker reservation also retains the owning pool and its
  mapping until publish/cancel completes, so detaching the sink and dropping
  every channel/sink pool reference during a pinned acquisition cannot unmap
  the reservation destination. Malformed diagnostics are static strings, not
  pointers into an expiring exception object.
- **Failure policy:** descriptors and releases are validated before use
  (bounds, stride, layout, epoch, generation, duplicate slot). Malformed input
  fails closed for that frame and does not advance the accepted generation or
  sequence; worker/session epochs are invalidated before stale work can be
  delivered; all waits wake on stop, close, exit and reconnect. No silent loss:
  source, admitted, delivered, rejected and policy-dropped counts stay separate.
- **Capacity:** when the selected payload does not fit a slot, the reservation
  is cancelled and the pinned buffer is requeued once before any owned delivery.
  The worker reports one Error per failed frame with
  `direct-shm capacity failure: session=... frame=... required=... slot_bytes=...
  consecutive=... total=...`; the counter advances per consecutive failure and
  resets after a successful publication. Acquisition does not resize, regrab or
  silently fall back for this case.
- **Metadata limits:** a shared `FrameReady` body is bounded to 64 KiB, with at
  most 1024 components, 4096 bytes per component name and 16 KiB of diagnostic
  text. A frame that exceeds a limit falls back to the owned path with
  `MetadataTooLarge` rather than truncating or synthesizing values.
- **Protocol:** worker protocol v16 carries the pool attach/detach,
  metadata-complete `FrameReady` and `SlotRelease` messages. A v15 peer is
  rejected with `Unsupported OneGen worker protocol version`; the request is
  not left pending. No compatibility shim is added before v1.0.0.

The production worker now implements the direct path while keeping the owned
path as the default. Pool attach is accepted only when
`ONEGEN_DIRECT_SHM_TEST=1`; otherwise it is rejected with
`direct shared-memory test mode is disabled`. With a pool attached,
`Camera::live()` runs reserve -> direct-or-owned conversion ->
publish/cancel. Direct eligibility comes from one shared layout table and is
not a format-name whitelist. Unpacked, byte-aligned packed-LSB, even-width
YUV422, even-width BiColor and other declared interleaved layouts are eligible;
planar and unknown layouts use the same-pin owned fallback. Legacy host-only
values `0x0110002A`-`0x0110002D` remain accepted by the host for compatibility
but direct conversion reports `UnknownLayout` until producer semantics are
proven. Unaligned packed rows report `PackedRowAlignment`; odd BiColor and
YUV422 widths report `LayoutConstraint`. Eligible images copy once from the
pinned GenTL buffer into the reserved slot and publish complete owned metadata
(width, height, stride, payload type, PFNC format, namespace, endianness,
FrameID, timestamps, stream identity, components, BufferInfo and diagnostic).
JPEG, planar, multipart, chunk-bearing, non-image and unsupported layouts take
the same-pin owned fallback; incomplete buffers keep the owned path's truncated
height and diagnostic. `GrabOne` always uses the owned path.

Host validation remains the legacy acceptance set: it still accepts the four
legacy values, uses ceiling byte rows for packed formats, rejects odd YUV422
widths, and does not add blanket BiColor or packed-alignment rejection. Direct
capability is checked separately, so a host-accepted frame can still take the
owned fallback when direct transport semantics are unproven. Real bounds,
overflow, null-storage and image-extent violations fail closed; an eligible
frame whose image does not fit the slot reports capacity per occurrence and
never falls back, drops, regrabs or resizes.

`WorkerFramePool` uses `free/reserved/ready/retired` slots, generation-checked
releases, and an epoch/token pair per attach. `WorkerDirectSink` encodes the
retained frame metadata itself, maps encode failure to `free`, send failure to
`retired`, and distinguishes stale, duplicate and unknown releases. Detach,
stop and shutdown wake blocked reservations. Host leases retain the mapping and
slot generation until the last consumer reference dies.

`test_worker_frame_pool` covers the pool/sink state machine. `test_pixel_layout`
verifies the legacy host list and the separate direct verdicts, including legacy
values, unaligned packed rows, odd BiColor, odd YUV422, planar and unknown
formats. `test_worker` covers host-side attach/detach, budget, malformed
descriptors, fencing and retained mappings with a fake peer, plus a separate
`LayoutClassBoard` regression that accepts RGB8, YUV422, Mono10p, Mono16
endianness, legacy `0x0110002A`, unaligned packed and odd BiColor descriptors
while rejecting planar metadata and recovering on the next valid frame.
`test_worker_direct_shm` adds a minimal synthetic GenTL CTI
(`Tests/SyntheticGenTL.cpp`) and exercises the production worker through the
real host `Connection`: owned parity for M0-M7 with byte/metadata comparison,
opt-in-off rejection, M8 incomplete, M9 direct Mono10 plus
chunk/multipart/JPEG fallback, M10 oversized metadata fallback, six direct
layout-class parity cases, five layout fallbacks, RGB8 capacity, five
consecutive capacity errors and recovery, capacity counter reset after publish,
and Close/reopen with a retained lease. The synthetic CTI is a test fixture,
not evidence for physical camera behavior. `T4 M7` scan3d metadata remains
limited by the synthetic XML; the in-process T2 suite covers scan3d
source-value preservation.

Physical acceptance used the actual Basler pylon CXP producer and
`boA13440-19ccCST`/`iF-CXP12-P` pair. Owned and direct acquisitions reported
the same `13376x9528` BayerRG8 geometry, stride, namespace, endianness, stream,
BufferInfo values and nonzero timestamp/timestamp_ns; the direct descriptor
carried `used_bytes=127446528`, while `metadata.payload` is empty by the v16
contract. A temporary static `TestPattern=Testimage1` acquisition produced
identical owned/direct bytes (same checksum) and was restored to the original
`Off` value with readback verification. This is acquisition correctness
evidence only, not a performance or default-enablement claim.

## Non-Goals

- Implementing vendor discovery protocols outside GenTL.
- Inferring physical aliases or hardware classes from manufacturer, transport, or ID strings.
- Adding camera-model-specific discovery or lifecycle code.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
