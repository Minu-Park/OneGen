# Release Notes

## Unreleased

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
