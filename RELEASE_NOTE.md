# Release Notes

## Unreleased

- Copy GenApi-declared selecting-feature state, including hidden/unreadable selectors, through node observations, worker responses and diagnostics without losing signed 64-bit precision. Report value/constraint observation errors explicitly; require matching worker/client protocol version 12.

- Preserve producer queue failures with stream identity, payload/announced capacity, buffer index/count and readable streaming/GenDC mode observations. Diagnostic failure never replaces the original backend error.

- Expose declared producer-scoped device discovery, connection, single-acquisition and Live tools on the source controller with connection-generation fences and explicit busy/failure outcomes. Consumer views observe copied controller state and share the existing Coordinator/Lease commands.
- Own device leases in the source controller; drain accepted external work before view/controller teardown, close channels explicitly, and reject RPCs through closed leases even after reconnect. Preserve connection/Live state when native close/stop is refused.
- Cover standard tool schema/invocation, GUI command parity, stale generations, single/Live exclusion, callback completion and failure recovery with isolated test peers.
## v0.2.0 (2026-10-08)

- Expose copied discovery phases and failure details independently of start admission. Preserve partial producer results, distinguish failed enumeration from successful empty inventories, and log scan transitions through the consumer adapter. Join stopping scans without allowing late completion to revive them; keep standalone discovery tests independent of consumer SDK headers.

- Report acquisition lifecycle to consumers and retain opt-in stopped-device feature snapshots at the current selector state; expose parameter requests and operation outcomes through diagnostics without changing capture policy.

- Use the shared Resources::Widgets search/favorites browser through a thin GenICam identity adapter. Keep existing favorite identities and consumer-selected stores; anchor the search surface independently of scrollbars. Remove implicit import between standalone and consumer favorite stores.

- Label multipart images using unique SFNC ComponentIDValue identities matched to GenTL data-purpose IDs; retain numbered fallbacks for unavailable or ambiguous identities.

- Number unidentified multipart images in delivered image order to distinguish sibling channels without assuming component semantics.

- Keep attached-buffer diagnostic node inventories consumer-configured; vendor chunk names do not participate in standard frame conversion. Preserve the SFNC CalibratedC versus RectifiedC XY contract.

- Offer opt-in Single capture dossiers with complete copied feature metadata, restored selector inventories, raw multipart/chunk bytes and attached-buffer chunk part diagnostics.

- Log selector-preserving component feature inventory on connection and first-frame part indices to support multipart channel identification.

- Report effective first-frame Scan3d mode, system, units, axis calibration and delivered chunk count for geometry diagnosis.

- Report every adapter rejection, discarded multipart/confidence part and missing frame consumer, including repeated failures, with camera/frame identity and layout/calibration diagnostics.

- Identify the first received/converted frame and multipart layout, including missing presentation consumers, to distinguish adapter rejection from acquisition failure.
- Accept declared GenTL 3D planar parts and report each rejected part's layout and calibration cause while preserving supported sibling resources.

- Publish scalar Coord3D parts that lack Scan3d calibration, including parts labeled as 2D images, as a native PixelGrid height map. Multi-axis payloads without calibration stay rejected.

- Accept exact consumer-owned CTI file lists, including an authoritative empty list. Share asynchronous startup discovery across later sessions and disconnected-control rescans; return completed empty results without another startup wait. Preserve standalone directory/environment discovery and protect active operations during reconfiguration.

- Keep search and Interface operations busy until their GUI completion callbacks are applied, including the initially queued search, to prevent premature reconfiguration.
- Show unsaved feature-row stars only on hover; keep saved favorites visible at all times. Center the visible star outline vertically in feature rows and favorite controls.

- Support consumer-requested GenTL path reconfiguration while disconnected, releasing idle workers and refreshing device inventories without restarting the consumer.

- Accept consumer-provided favorite storage and preserve standalone storage and unrelated settings.

- Use canonical temporary producer paths in worker regression fixtures, keeping pinned Interface identities consistent with discovery on symlinked temporary directories.

- Preserve leading producer NOTICE/INFO, DEBUG/TRACE and ERROR/FATAL severity fields without interpreting message-body text as a severity.
- Report Interface discovery failures alongside surviving Devices through worker protocol v10; retain Interface inventory, stop retrying failures as empty inventories, and retire failed idle discovery workers while preserving connected or opening sessions.

- Align feature-search clearing with the result count and favorites control; retain focus and filtering state when cleared.

- Declare the GenTL / GenApi subtitle and discovery, feature, and acquisition summary in package metadata.

- Preserve worker diagnostic severity, producer messages, and final partial lines; serialize complete stage records through a capturable standard diagnostic stream.

- Add a compact floating bottom feature search with the feature-header surface and a subtle shadow, name/category filtering, result counts, Ctrl+F and Escape shortcuts; omit surrounding gradient/blur.
- Share favorites by node-map/internal-name across devices; silently omit absent nodes while retaining the saved list. Toggle row stars or Ctrl+D and combine favorites-only browsing with search without rebuilding editors.
- Restore expansion and scrolling after filtering and let the last feature scroll above the search bubble.

- Let the outer panel own embedded page minimum sizes so tabs and status bars fit at the supported minimum window size.
- Select Data Streams asynchronously; keep stream changes and Live start disabled during Single while explicit triggers and disconnect remain available.
- Drain pending Single requests before releasing their camera lease and reject stream reconfiguration during active acquisition.

- Preserve Interface feature rows and editors across writes and rescans; safely track focus when a refreshed feature disappears.
- Keep the selected target during incomplete discovery, remove inventories for removed Producers, and restore Interface settings and Device choices after a failed native close.
- Prevent an old worker lease from releasing a camera that has reconnected through a replacement worker.
- Retain the current feature panel across discovery updates with the same producer-scoped target identity, including reordered lists and updated labels.
- Show target names without role prefixes or icons; distinguish Devices and Interfaces with full-name tooltips and accessible role labels.
- Preserve the empty feature tree for disconnected Interfaces and move the same panel beneath tabs only after connection succeeds.
- Keep camera and Interface selectors sized by their available layout width; long discovery labels no longer inflate their size hints or popup width.
- List all producer-scoped Interfaces and Devices explicitly; expose Interface settings and independent Device tabs without inferring hardware types or changing connections on tab selection.
- Share the board Interface in one producer worker, route camera sessions with protocol v9, and preserve sibling acquisition when a camera closes.
- Allow camera-less Interface settings/rescan, release temporary discovery handles, guard shared reconfiguration, and preserve producer-specific device IDs during fallback.
- Preserve the original single-camera control row and one status bar for direct cameras; show tabs only after an Interface connects and remove drained camera presentation sources when changing targets.
- Scope device identity to Producer/Interface/Device, preserve candidate InterfaceIDs, continue discovery after an individual Interface failure, and retire failed idle producer workers after an unsuccessful Interface open.
- Remove manufacturer-specific CTI discovery, the pylon-only defaults API, physical-board alias inference, and adapter requirements for Interface connection; defer physical resource arbitration to the Producer/SDK.
- Preserve opaque InterfaceIDs, including address-like strings; compare DeviceIDs exactly and obtain IP metadata only from verified GenTL SFNC selector features.
- Reuse shared device control sizing, selector grouping, and flat tab panes; preserve the common rounded feature-tree frame, direct-camera layout, and board header background.

## v0.1.1

- Read GenTL interface IDs, device IDs, and optional device strings with bounded retry when the producer reports a larger buffer; keep malformed optional metadata from hiding a discovered device.
- Preserve every supported GenTL multipart image and range as an independently identified resource grouped by standard source and region metadata.
- Collapse calibrated point-cloud output into the corresponding resource group and retain confidence-only identities.

- Collapse verbose producer stage boundaries into CTI initialization and interface-scan summaries with per-stage elapsed milliseconds.
- Keep the ten-second no-camera startup budget while probing every GenTL producer in one-second slices, returning as soon as that producer reports a camera.
- Include the port-read sequence, time since the last successful read, and producer `GCGetLastError` details when a GenTL port read fails; do not query stale producer errors for successful empty transfers.
- Forward worker `[GenICam]` stage lines through the host log and record LoadLibrary, GCInitLib, TLOpen, and enumerate boundaries so a hung producer probe can be located.
- Keep Probe replies long enough for sequential per-interface device lists, and skip loopback interfaces so a camera found on a later NIC is not discarded as a host timeout.
- Ignore feature-editor writes while the tree is rebuilt so a checkbox write cannot be undone by destroyed line-edit `editingFinished` signals.
- Refresh the feature tree in place after writes: update existing editors, add newly visible leaves, prune vanished rows, and keep the current item, focus, and top-visible viewport when only values change. Rebuild only when the camera root is missing or an editor cannot be reused.
- Edit integer and floating GenApi features with spin boxes so range, increment, and committed values match Camera/Gocator controls instead of free-form `QLineEdit` text. Integers keep the full signed 64-bit range.
- Run feature writes, post-write snapshots, and module restore on the worker pool so the GUI thread does not wait on NodeWrite or DescribeFeatures.
- Keep a dead producer worker from poisoning later errors: process exit is a short `Worker process exited`/`crashed` cause plus an exit-code log, not the accumulated stage dump. Drop that camera lease so reconnect is not `already in use`.
- Stop Live and session teardown must not throw into the host destructor; clear frame callbacks before joining the delivery thread so a closed session cannot force-crash the application.
- Keep the feature tree usable during Single: do not disable editors while waiting for a frame, run acquisition off the control loop so TriggerSoftware and other writes still reach the camera, keep the connection open when a frame never arrives, and suspend synchronous invalidation draining until the post-Single snapshot replaces queued changes.
- Keep Single and Live trigger-neutral: neither mode issues `TriggerSoftware` implicitly. Route an explicit Remote Device `TriggerSoftware` Execute action through the acquisition trigger operation without a post-command feature snapshot between trigger and buffer receive.
- Hide the top-bar Data Stream selector when the connected device exposes only one stream; retain it for actual multi-stream selection and keep Data Stream diagnostics in the feature tree.
- Accept a GenTL short port read on `GC_ERR_SUCCESS`: the port callback now treats a producer-reported partial transfer as valid for variable-length register regions, zero-filling the unread tail, and fails only on a producer error code or a zero-byte transfer. XML node-map body reads keep their exact-length check.
- Keep GenApi invalidation callbacks exception-safe so a dead producer channel or failed host notification cannot escape the callback boundary and terminate the worker process.

## v0.1.0

- Leave PixelGrid CalibratedC X/Y without a stored pitch; `xScale` remains UniformPhysical sampling only.
- Read C-only and explicit selector-free CalibratedC metadata without requiring unused axes; preserve calibration availability, output mode, coordinate system and stereo parameters through worker protocol v6.
- Decode unpacked C/AC/ABC8,16,32f/64f and packed 10/12-bit `p` layouts, including planar storage and both byte orders; apply RectifiedC physical XY sampling, assemble uniquely matched coordinate planes, and preserve native UncalibratedC previews.
- Reconstruct DisparityC with focal length and baseline, accept RectifiedAC like CalibratedAC, convert Spherical/Cylindrical samples to Cartesian, map meter/inch physical units to millimetres without rescaling PixelGrid X/Y, preserve Confidence1/1p, and byte-swap unpacked 2D images.
- Keep chunk Scan3d overlays from dropping stereo parameters; emit CalibratedABC_PointCloud coordinates from `physicalXAt`/`physicalYAt`/`physicalZAt` so unit conversion cannot split XYZ.
- Present unorganized calibrated ABC data as points without invented grid surfaces; align auxiliary fields and reject unsupported geometry modes and unsafe layouts.

- Separate single-image bytes from transport prefixes and chunk metadata; validate chunks and parts against delivered storage, retain owned raw chunks through worker protocol version 5, and reject non-image payloads in graphics conversion.
- Support all four documented Gen4 PixelFormat entries in single and multipart payloads, including native Confidence32f-only previews without fabricated depth calibration.
- Decode calibrated multipart Coord3D_C32f range and Confidence32f samples without integer truncation; preserve finite-sample masks and reject unsafe layouts and unknown endian identifiers.
- Keep graphics-adapter assertions enabled in Release test builds.

- Use the target platform's executable suffix for the packaged worker environment binding; verify it against the built worker in CTest.

- Isolate cameras sharing a CTI in separate retained workers, reusing the discovery worker for the first camera. Include model/serial in Live completion and asynchronous device-error logs.

- Include the connected camera model and serial in Single acquisition completion logs.

- Defer GUI-thread invalidation node reads during control/acquisition operations; retain the complete post-acquisition feature snapshot without blocking early frame painting. Extend opt-in timing to Single acquisition and stop stages.

- Retain successfully closed producer workers and their discovery cache across Disconnect/Connect; discard workers after failed Close and release retained workers at shutdown.

- Open discovered device IDs directly using their retained producer/interface association; refresh only for uncached queries or failed device opening, avoiding the recurring ten-second Connect scan.

- Add opt-in `ONEGEN_TRACE_CONNECT=1` stage timings for device opening, XML loading, stream preparation, feature snapshots, worker round trips, and GUI tree construction.

- Summarize discovery at start/completion and at most every five seconds; keep producer paths/results at Debug and group optional-producer unavailability at Info. Include camera, stream, and attempted feature-write details in control failures.

- Hide feature categories and module groups with no displayed children after access filtering.

- Remove periodic control-panel feature polling so refreshes do not interrupt item selection or editing.

- Separate local XML URL query metadata such as `?SchemaVersion=1.0.0` from the hexadecimal length before validation.

- Preserve semicolon-delimited local XML lengths and padded hexadecimal fields while retaining strict numeric and transfer bounds.

- Limit the control statusbar to Idle, Connected, and Live; route operation messages to application logs.

- Expose writable-only features, preserve signed 64-bit integers and double precision in numeric editors, and carry unit, representation, and increment metadata through worker protocol version 3.
- Try all bounded local/file/HTTP(S) XML URLs, retrieve variable-length URL and port names, and reject malformed lengths and short port reads. Use native WinHTTP on Windows and installed libcurl where available on other platforms.
- Add System/TL features, GenApi polling, and supported generic/invalidation producer events with bounded draining and node-map-owned adapter lifetimes.
- Add explicit bounded raw-register inspection/writes and selector-aware, verified GenApi feature-bag copy/restore; stop Live before restoring settings or writing raw registers.

- Report module XML/node-map load failures through the error callback, including worker errors before a camera controller is attached, while preserving optional-module fallback.

- Place remote-device feature categories directly below the model-and-serial camera root, alongside Data Stream, Device, Interface, and System/TL.

- Prefix control, discovery, and acquisition logs with `[GenICam]`.

- Keep the startup loading overlay until the first camera is found or ten seconds elapse; hand the same ongoing scan to the controls for remaining producer results.

- Keep the control statusbar limited to Idle, Connected, and Live; route discovery progress, operation results, and feature-write diagnostics to application logs.
- Open the control panel before hardware discovery completes; publish merged devices as each isolated producer finishes, without gating connection on slower producers. Deduplicate physical CTI file aliases before probing, without collapsing different producer binaries by filename.
- Remove the unused in-process host, vendor-specific dump tool, and uncalled public wrappers; refresh the feature tree only at acquisition and feature-write state boundaries.
- Refresh the selected stream after remote feature writes so payload changes apply to the first acquisition.
- Keep Windows workers in kill-on-close jobs so they cannot outlive the host.
- Decode feature module identifiers before their trees so MSVC argument evaluation cannot corrupt snapshots.
- Fold the GenApi feature tree into `QOneGenWidget`, matching Camera/Gocator control widgets.
- Align the module with Camera/Gocator: public headers sit in `C++/` next to sources, Qt UI is `Utility/Qt/QOneGenWidget`, Playground code stays under `Utility/PlaygroundAdapter`, and the EMVA GenICam cache is `genicam/` instead of `third_party`. Remove the unused viewer app and vendored `cti/` producers.
- Isolate every discovered CTI in its own Qt-free `OneGenWorker` process using a versioned binary stdin/stdout protocol; keep discovery vendor-neutral and remove the package `cti/` overlay and `ONEGEN_CTI_ROOT`.
- Resolve a CTI's sibling DLLs from its install directory on Windows when the host uses a restricted DLL search path.
- Remove the Software Trigger toolbar button; execute `TriggerSoftware` from the feature tree.
- Announce 10 stream buffers by default, raise a present `MaxNumBuffer`, and enable present `AutoPacketSize` / `EnableResend` before `ProbePacketSize` so GigE resend does not underrun the queue.
- Deliver incomplete frames with one diagnostic warning; reject a buffer only when it cannot form an image.
- Put GenTL buffer and stream counters, plus a present DataStream Statistic category, into incomplete-frame errors and refresh that tree after Single.
- Execute a present DataStream `ProbePacketSize` command when preparing the stream so GigE packet size can match the path before the first grab.
- Present the Playground device entry and session as GenICam.
- Log the system GenTL search path and inventoried producers.
- Keep GenTL interface handles after probe so Connect reuses the current GigE list instead of closing it and waiting another update.
- Open a camera from the producer's current device list instead of waiting another 1 s GigE update that can miss it.
- Keep discovery usable when one producer fails after another loads successfully.
- Reuse prepared GenTL streams and buffers across acquisitions, rebuilding them only after stream or feature changes.
- Keep operational state in the device status bar and route errors to System Logs.
- Recreate the GenTL new-buffer event for every acquisition so stale or killed notifications cannot corrupt the first frame of the next run.
- Decode GenTL `BOOL8` buffer metadata so incomplete and image-present diagnostics reflect the producer's actual flags.
- Expose the selected GenTL DataStream NodeMap with the other module trees, including producer transport controls and statistics when available.
- Wait for the standard GenTL stream-grabbing state before starting remote acquisition so the first packets cannot race receiver startup.
- Identify Single, Live start, Live stop, and Connect failures separately in System Logs.
- Refresh the feature access snapshot after both Live start and stop so editors follow the producer's current writable state.
- Refresh a feature editor after a successful write so the UI shows the value read back from the device.
- Size reusable acquisition buffers from GenTL or standard `PayloadSize` metadata instead of a fixed fallback, and remove producer-specific custom device-info probing.
- Recover interrupted GenICam downloads instead of reusing a corrupt cache.
- Reject invalid IPv4 octets in camera queries and device metadata.
- Support explicit runtime and GenTL paths with local GenICam SDK roots.
- Prevent local SDK configuration from scanning the filesystem root.
- Keep CMake paths correct when OneGen is added from another project.
- Document standalone build and runtime usage.
- Add the optional Playground PFNC image adapter, source controller, feature controls, and device plugin package.
- Deduplicate canonical default CTI paths before plugin discovery.
- Match Camera's device toolbar layout and open CTI producers through the host-themed file picker.
- Run CTI loading, discovery, connection, and acquisition controls outside the GUI thread.
- Reject incomplete GenTL buffers, always return acquired buffers to the producer, and preserve frame IDs.
- Display common PFNC Mono, RGB, RGBA, and 8/10/12/14/16-bit Bayer acquisitions in Playground.
- Clean up partial stream startup failures so acquisition can be retried without reconnecting.
- Surface live GenTL and unsupported-format errors in the device controls without per-frame log spam.
- Propagate a present remote `AcquisitionStart` failure instead of arming a stream that can only time out.
- Allow live callbacks to request stop without destroying the camera, serialize callback replacement, and clean up if camera creation fails.
- Discover trusted CTIs registered through the standard GenTL environment paths.
- Preserve GenTL multipart parts and convert calibrated PFNC Coord3D ABC32f/C16 with intensity and confidence to Playground RangeFrame.
- Read interface, device, and remote-device GenApi module maps so CTIs can expose frame-grabber and camera controls without SDK wrappers.
- Enumerate and select GenTL data streams, route their source index, and preserve nanosecond timestamps.
- Read generic per-buffer chunk metadata and prefer its SFNC Scan3d calibration over a pre-acquisition snapshot.
- Save and restore standard streamable GenApi feature configurations with optional readback verification.
- Preserve GenTL payload type, pixel-format namespace, and endianness; deliver standard JPEG payloads and reject unsupported big-endian multi-byte images.
- Fall back to the camera's GenApi Width, Height, and PixelFormat when a producer omits per-buffer image metadata.
- Show Remote Device, Device, and Interface controls together below a model-and-serial camera root.
- Report the exact frame rejection reason with PFNC, layout, payload size, and endianness.
- Preserve and report standard GenTL buffer metadata for incomplete-frame diagnosis.
- Document the process-isolated multi-vendor CTI Playground session contract.
