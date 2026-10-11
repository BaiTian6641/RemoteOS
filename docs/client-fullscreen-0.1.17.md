# RemoteOS client 0.1.17 — separate fullscreen viewer

This update addresses the report that client 0.1.16 shows the remote desktop
inside setup controls or a window border. Update the HP USB image; keep the
existing Linux application host 0.1.16, including its dedicated-desktop browser
repair. The control protocol and host display negotiation are compatible.

## Install

Use `output/releases/hp2140-fullscreen-wl-0.1.17/images/remoteos-hp2140.img.gz`.
Decompress it to `.img`, or use an image writer that explicitly supports gzip,
then write the disk image to a 4 GB or larger USB drive. It is a legacy BIOS disk
image, rather than an ISO. A whole-drive write replaces the existing partitions,
including `REMOTEOS_STATE`; back up its settings and pairing identity first if
you want to preserve them. Boot the HP and confirm that Diagnostics reports
`0.1.17-diagnostic-fullscreen-wl`.

## Viewer behavior

The remote video drawable now belongs to a separate GTK toplevel. In appliance
mode that toplevel has no window decoration and occupies the detected panel's
native bounds. The complete setup toplevel is unmapped while connecting and
streaming, so its controls, labels and minimum size cannot consume viewer space.
Disconnect or a connection failure restores setup. The existing explicit
`--windowed` development mode continues to allow a normal managed window.

The power overlay is transient to the active viewer. Opening it releases held
host input and restores the local cursor. Resume hides the local cursor and
reasserts native viewer bounds; reconnect does the same. The separate VGA viewer
continues to use its associated monitor.

The pinned GStreamer XVideo sink caches its external window geometry. RemoteOS
disables the sink's X event/navigation handling because its input broker owns
keyboard and pointer events; with that option disabled, the sink selects no
window events. After media becomes ready or the drawable allocation changes,
the client synchronizes native layout and explicitly calls
`gst_video_overlay_expose()`. The pinned sink's expose implementation updates
the cached geometry before drawing. GStreamer documents this API in its
[VideoOverlay reference](https://gstreamer.freedesktop.org/documentation/video/gstvideooverlay.html).
Refresh is coalesced on the GTK idle queue and runs only after layout changes or
session resume. It adds no per-frame conversion, copying or polling.

Encoding, decoding, rate adaptation, UDP pacing, the 40 Mbps connection ceiling,
native host resolution, Broadcom `wl` selection and `b43` recovery are retained.
Source comparison verifies that only receiver window refresh was added to the
media implementation; codec and transport logic match the previous source.

## Evidence and limits

The release's `validation` directory records native regression tests and actual
32-bit GTK tests at 1024×576 and 1366×768 without a window manager, plus 1366×768
with Openbox. These check native video bounds despite oversized setup controls,
an unmapped setup window, a separate borderless viewer, cursor ownership, native
pointer coordinates, held-input release, overlay resume, disconnect and reconnect.
The target tests connect to the existing 0.1.16 application host package.

A focused VideoOverlay test verifies explicit expose dispatch, rejection of
non-overlay sinks and safe receiver teardown. A production-client cursor probe,
offline BIOS boot, direct disk-content audit and image signature/checksum checks
bind the remaining evidence to the delivered image.

Xvfb has no Intel XVideo ports, so its session fixture uses a headless video sink.
QEMU checks boot and Intel driver linkage, not the real laptop's graphics. These
checks do not reproduce or qualify the reported physical HP rendering problem.
After booting 0.1.17, verify that a connected desktop reaches all panel edges,
the power overlay resumes to the same bounds, and disconnect/reconnect preserves
that behavior. Physical HP confirmation remains required.
