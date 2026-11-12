# In-Process GenTL Producers

## Status

This document defines the OneGen Playground runtime and UI contract. The plugin
loads every GenTL producer (CTI) found on the standard search path into the
consumer process, merges only identical scoped Device records, and maintains
independent Device leases. Feature trees bind live GenApi module maps. Frames
convert to `GraphicsFrame` inside the plugin.

The design must remain vendor-neutral: no camera model, manufacturer,
transport, or PFNC-specific branch may be added to discovery, selection, or
session orchestration.

Earlier releases ran each CTI in its own worker process. That boundary was
removed (Playground #413) after measurement showed that frame loss came from
slot sizing rather than process isolation, and that producer libraries rarely
collide. The trade-offs are stated below under [Crash policy](#crash-policy) and
[Library conflicts](#library-conflicts).

## Goals

- Explicitly distinguish every enumerated Interface and Device.
- Support simultaneous cameras from producers that can share one process.
- Support multiple independent OneGen sessions in Playground.
- Discover CTIs automatically; connect only the explicitly selected producer-scoped target.
- Preserve the existing OneGen GenTL/GenApi and GraphicsFrame boundaries.
- Deliver frames with one copy and no consumer wait on the acquisition thread.

## Package Layout

The plugin package owns OneGen's GenApi runtime. It does not ship vendor CTI
files and does not own a package `cti/` overlay.

```text
plugins/
  onegen/
    current/
      plugin.json
      OneGenPlugin.{dll,so}
      runtime/
        OneGen's GenApi runtime
```

OneGen's GenApi libraries use unbranded names (for example
`GenApi_MD_VC142_v3_5.dll`), so they do not collide with vendor-branded GenApi
builds that producers load.

## Producer lifecycle

`onegen::producer::Coordinator` owns one OneGen system per canonical CTI path.
Each producer has one lock that serializes control and feature calls: discovery,
interface and node-map access, stream configuration and acquisition control.
Blocking Single and Live acquisition run outside that lock so stop can interrupt them.
A process-wide load lock spans conflict inspection, directory registration, library
loading and initialization, including TLOpen, even when discovery probes run in parallel.
Acquisition runs on the camera's own thread. Equivalent forward- and backslash
spellings of a CTI path share one producer.

- A producer that fails to load or initialize is **inactive**. Its error is
  reported by discovery and every later request. Only a new explicit discovery
  replaces an inactive producer, and only while no lease uses it.
- `reset_idle_search()` closes every producer when no discovery, Interface
  session or Device lease is active. `set_producer_files()` accepts a new
  file list only while no producer is loaded.
- Producer libraries stay mapped until process exit. Closing a producer calls
  `TLClose` and `GCCloseLib`; it never calls `FreeLibrary` or `dlclose`,
  because a vendor runtime may still own threads, callbacks or process-global
  state. Loading the same canonical CTI again reuses the mapped module.

## Consumer hardware claims

The adapter passes the consumer resource-claim callback to the in-process
Coordinator. For a Siso InterfaceID containing `SN_<serial>`, it claims
`siso-board:<normalized serial>` before TLOpenInterface. A denied claim leaves
the Interface closed and reports the owning package; the Interface lifetime
retains the claim. Other interfaces do not acquire this Siso-specific claim.
This requires the ABI 7.0 consumer resource-claim contract. It does not infer
physical equivalence from unrelated device serial numbers or IP addresses.

## Library conflicts

Windows resolves a name-only import to an already loaded module with the same
base name, and other platforms behave similarly for already loaded sonames.
Two producers, or a producer and another device plugin, that ship different
builds of a same-named library would therefore share one of them silently.

Before loading a CTI, OneGen scans the CTI directory (or, when that directory is
named `bin`, the producer root above it, including sibling `lib` and `dll`
trees) and compares every shipped library with the module of the same base name
already loaded in the process. The CTI is refused before the loader runs any of
its code when a same-named module is loaded from a different file with different
contents. The refusal names each conflicting library and the loaded path. The
producer stays inactive until Playground restarts without the owner of those
libraries.

Not conflicts: identical contents at another path, compiler runtime
redistributables (`msvcp*`, `vcruntime*`, `ucrtbase`, `libstdc++`, and similar),
and modules loaded from the Windows directory. OneGen cannot see the reverse
direction, a plugin activated after the producer that binds to its libraries;
the consumer must provide a package-activation check for this direction
([#413](https://github.com/Minu-Park/Playground/issues/413) WP4).

### Concurrent consumer loaders

The load lock serializes OneGen loader calls only. It is not shared with the
consumer's plugin manager or other code that loads native libraries. A consumer
can activate a package while asynchronous OneGen discovery is running. If that
loader maps a conflicting library after OneGen's inspection but before its
library load, the producer can bind to that library without a pre-load refusal.
`Gentl::load` currently has no post-load conflict inspection. The consumer's own
post-load package check does not make both loader paths one atomic transaction.

Consequently, the guard detects conflicts visible at inspection and serializes
OneGen-to-OneGen loads; it does not guarantee conflict exclusion against a
concurrent independent loader. Sequential activation-order tests and concurrent
OneGen discovery tests do not establish that stronger guarantee. A native fault
can occur before any post-load check, so a later inspection alone would not
provide complete protection either. Safe exclusion would require a shared
consumer/module load-coordination contract covering all participating loaders;
this is a separate integration decision, not a library-name or device allowlist.

### Import closure (Windows)

The consumer process removes `PATH` from its DLL search, but producers often
depend on vendor runtimes that a standalone process finds only through `PATH`.
Some of them are delay-loaded: the loader resolves them on first use, from a
producer thread, and an unresolved delay-load import terminates the process
(`0xC06D007E`). pylon `ProducerCXP.cti` delay-loads
`GCBase_MD_VC142_v3_5_Basler_pylon_v1.dll` from `pylon\Runtime\x64` this way.

Before loading a CTI, OneGen therefore follows the static and delay-load import
tables of the CTI and of every library it resolves. Each import resolves as a
standalone producer process would find it: the CTI directory, then the Windows
system directories, then each `PATH` entry. The result decides the load:

- An import that no location provides, that the process has not already loaded
  and that is not in the consumer's application directory refuses the CTI and
  names the library and its importer.
- A resolved library that differs from an already loaded module with the same
  name, or from a same-named file in the consumer's application directory or in
  another directory the producer needs, is a conflict. Delay loads search all
  of those in an unspecified order, so any of them could win.
- Otherwise, every non-system directory in the closure is added to the process
  DLL search with `AddDllDirectory` and stays registered until process exit,
  like the producer libraries themselves. Later loads in the process may also
  resolve from these directories, which widens the reverse-direction exposure
  described above.

Other platforms use only the directory scan above. Measured examples
(2026-11-11, Windows): pylon `ProducerCXP.cti`
ships 19 libraries that differ from the Framegrabber plugin's same-named Silicon
Software and pylon GenApi builds, so the two cannot share a process. Euresys,
Heliotis, Blaze, GEV, U3V and StereoMini producers had no conflicts.

## Live pool limits

Live requires a readable nonzero payload and at least 10 slots within the 2 GiB
per-source budget, with 12 slots preferred. The maximum payload is 214,748,364
bytes. An unavailable payload, an over-budget plan or an allocation failure
refuses Live with an explicit error before acquisition starts. Exhausting an
allocated pool drops and counts frames; it does not wait on the acquisition thread.

## Crash policy

A native fault inside a producer (access violation, CRT assertion or
fail-fast) terminates Playground. OneGen converts every GenTL error code and
C++ exception into an explicit operation failure, but it cannot recover from
native faults in vendor code. This matches the other device plugins, which load
vendor SDKs in the consumer process.

## Discovery

1. The plugin inventories `.cti` files from `GENICAM_GENTL64_PATH` /
   `GENICAM_GENTL32_PATH`, or from the exact file list a consumer supplies.
   There are no manufacturer default paths or filenames. Canonical path
   duplicates and aliases of the same physical file are removed. Identical
   filenames, versions, or copied bytes do not prove equivalent sibling library
   and configuration environments; distinct installed producers remain
   candidates.
2. Producers are probed in parallel, one thread per CTI. A failed producer is
   reported without failing other CTIs.
3. Interface identity is `(CTI path, InterfaceID)` and Device identity is
   `(CTI path, InterfaceID, DeviceID)`. IDs are opaque and case-sensitive.
   Serial numbers, IP addresses, transport types, model names, and equal CTI
   filenames never establish cross-producer physical equivalence.

A consumer may supply an explicit CTI search directory. IP metadata comes from
standard GenTL SFNC selector features, not address-shaped IDs or labels. Every
Interface, including network and camera-less Interfaces, remains selectable.
Producer paths in selector tooltips distinguish targets with equal display
names.

The plugin returns value-only session data immediately. After GUI-thread
construction, the control panel schedules discovery on a separate future. Each
completed producer publishes a cumulative merged device list on the GUI thread;
a snapshot is also published every five seconds while producers are pending.
Connection and acquisition do not wait for unrelated producers. Discovery runs
are serialized per coordinator. Shutdown suppresses pending GUI updates and
joins discovery before teardown.

An individual Interface failure preserves other Interfaces and their Devices,
appears in the cumulative result, and is not retried as an empty inventory.
Successful empty inventories retain the initial-discovery budget, scanned in
one-second slices. Temporary discovery interfaces are released after each
probe; pinned Interfaces and active Devices stay open.

Records that producers and OneGen emit on a discovery thread are reported at
Debug; the scan owner reports each producer outcome and the original failure
reason at INFO. OneGen diagnostics reach the consumer through the Qt log sink
that the plugin installs (`onegen::set_log_sink`) with their OneGen severity.
Output that a vendor library writes directly to the process streams is not
captured or reclassified.

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

Closing one Device releases only its DataStream and Device and returns its
camera to the producer for reuse. Interface disconnect drains all child
operations before closing the Interface. Reconfiguration rejects affected active
Device leases.

## Multiple Sessions

The plugin owns one coordinator.

- Duplicate local opens of the same complete Device identity are rejected.
- Distinct producers reporting equal InterfaceIDs remain distinct targets.
- A Device opened on an explicitly connected Interface shares that Interface's
  maps; its Interface and System modules are read through the Interface tab only.
- Physical ownership across producers and plugins follows native resource
  arbitration. A host-owned hardware claim is tracked in Playground #414.

## Acquisition

### Frame pool

Live frames use one copy from the pinned GenTL buffer into a slot of a
plugin-owned frame pool:

- **Slot plan:** before each Live start the controller reads the stream payload
  size (the producer-defined size, else `PayloadSize`) and plans 12 slots, the
  host's worst-case concurrent leases (9 with Script on and the default six
  presentation leases) plus headroom. When 12 slots exceed the 2 GiB per-source
  budget, the plan uses fewer slots down to 10, the minimum that still exceeds
  the host bound. A payload that cannot fit 10 slots is refused at Live start
  with an explicit error. The pool is reused while the payload size is
  unchanged. For `13376x9528` BayerRG8 the pool is
  `12 * 127,446,528 = 1,529,358,336` bytes.
- **Memory:** every slot is allocated and committed when the pool is created,
  so allocation failure surfaces before acquisition starts.
- **Publication:** the acquisition thread reserves a free slot, copies the image
  while the GenTL buffer is pinned, requeues the buffer exactly once, and hands
  a `FrameLease` to the controller. The lease is an immutable alias for the
  `GraphicsFrame` image storage; the last reference returns the slot to the
  pool from any thread. Leases keep the pool memory alive after the pool is
  detached.
- **Backpressure:** the slot pool is the bound. When no slot is free, the
  acquisition loop dequeues and requeues one frame without a copy and counts it
  as exhausted; it never waits for the consumer. Exhausted frames are added to
  the source's `failed` count in `source.diagnostics`, logged on the first and
  every hundredth drop, and included as `dropped` in the stop summary. The
  controller reports the slot count through `producerCapabilities()` so the
  host's producer-pool check can warn when the pool does not exceed its bound.
- **Fallback:** JPEG, planar, multipart, chunk-bearing, non-image and
  unsupported layouts take the same-pin owned path and are delivered as owned
  frames. Direct eligibility comes from one shared PFNC layout table, not a
  format-name whitelist.
- **Capacity:** an eligible image that does not fit its slot is not delivered;
  its buffer is requeued and the failure is reported on the first and every
  hundredth occurrence. Restart acquisition after a payload change so the pool
  is planned again.

`GrabOne` always uses the owned path. Without an attached pool, Live also uses
the owned path.

### Streams and buffers

The selected data stream and announced buffers remain prepared across
acquisitions. Ten buffers are announced by default, or the producer's
`STREAM_INFO_BUF_ANNOUNCE_MIN` / `MaxNumBuffer` cap when that is larger or
smaller. A producer may mark a buffer incomplete while still delivering image
bytes. OneGen keeps that frame and rejects it only when it cannot form an
image.

## Frame transport diagnostics

Set `ONEGEN_TRACE_FRAMES=1` in the environment before starting Playground and
enable Debug capture. Owned frames then log `conversion_ms`
(`oneGenGraphicsFrame`) and `consumer_ms` (SessionFrame admission) with the
producer FrameID. The flag is read once per process; with it unset, no
per-frame record or counter is added.

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
  (`first frame received/chunk count/part/converted`, effective Scan3d), the
  frame pool plan and the component node inventory taken when a lease is
  bound. Counters and FPS sampling keep running while Debug is off, so values
  are current when it is enabled.
- Always retained: the per-run `[GenICam] live diagnostics:` stop summary
  (received, delivered, failed, incomplete, dropped) and every warning or error,
  including incomplete-frame, rejected-frame and slot-exhaustion records.

## Validation

- `test_producer` drives the coordinator against the synthetic CTI
  (`Tests/SyntheticGenTL.cpp`) and a failing producer: discovery with an
  inactive producer, duplicate-open refusal, checked writes, pooled Live with
  held leases, exhaustion without blocking, owned Live, Single, close/reopen and
  idle reset.
- `test_frame_pool` covers reserve/publish/cancel/release accounting, leases
  outliving the pool owner, and the slot plan.
- `test_module_guard` covers conflict refusal before load, identical contents,
  runtime redistributables and unrelated names.
- `test_logging` covers stage serialization, the Qt sink and discovery-scope
  severity.
- `test_plugin_discovery` loads the plugin with an in-process synthetic
  producer and an unloadable CTI.

The synthetic CTI is a test fixture, not evidence for physical camera behavior.
Physical acceptance is tracked in Playground #413.

## Non-Goals

- Implementing vendor discovery protocols outside GenTL.
- Inferring physical aliases or hardware classes from manufacturer, transport, or ID strings.
- Adding camera-model-specific discovery or lifecycle code.
- Automatically accepting incomplete buffers as healthy frames without a diagnostic.
- Recovering from native faults inside producer code.
