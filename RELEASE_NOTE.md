# Release Notes

## Unreleased

- Recover interrupted GenICam downloads instead of reusing a corrupt cache.
- Reject invalid IPv4 octets in camera queries and device metadata.
- Support explicit runtime and GenTL paths with local GenICam SDK roots.
- Prevent local SDK configuration from scanning the filesystem root.
- Keep CMake paths correct when OneGen is added from another project.
- Document standalone build and runtime usage.
- Add the optional Playground PFNC image adapter, source controller, feature controls, and device plugin package.
- Deduplicate canonical default CTI paths before plugin discovery.
- Match Camera's device toolbar layout and open CTI producers through the host-themed file picker.
- Keep discovery usable when one default CTI fails after another loads successfully.
- Run CTI loading, discovery, connection, and acquisition controls outside the GUI thread.
- Reject incomplete GenTL buffers, always return acquired buffers to the producer, and preserve frame IDs.
- Display common PFNC Mono, RGB, RGBA, and 8/10/12/14/16-bit Bayer acquisitions in Playground.
- Clean up partial stream startup failures so acquisition can be retried without reconnecting.
- Surface live GenTL and unsupported-format errors in the device controls without per-frame log spam.
- Propagate a present remote `AcquisitionStart` failure instead of arming a stream that can only time out.
- Allow live callbacks to request stop without tearing down their own worker, serialize callback replacement, and clean up if worker creation fails.
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
- Document the proposed process-isolated multi-vendor CTI package and session architecture.
