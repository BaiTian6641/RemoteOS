# HP Wi-Fi latency update — 0.1.11

Use the **0.1.11 Linux host bundle** and **0.1.11 HP image** together for the
complete update. The host remains compatible with the HP 0.1.8 image: its new
frame-rate selector and corrected handling of older feedback also apply to
that image. Client decoder, timing and diagnostic changes require the new image.

The downloadable image is `remoteos-hp2140.img.gz` (about 179 MiB), expanding
to a 2.15 GB whole-disk image for a 4 GB or larger USB drive. Decompress it
before using a writer that requires `.img`. A full reflash replaces saved
Wi-Fi, Tailscale and pairing state. The repository's `scripts/update-image.py`
can preserve the existing state partition in an offline signed update when
the disk layout matches; it requires an explicit, unmounted target disk.

## Start with these settings

1. Stop the old host application, extract `desktop-host-linux-0.1.11.tar.gz`,
   and open `Start-RemoteOS-Host.sh` as the normal desktop user.
2. Choose **HP native virtual display · KDE Wayland** and **1024×576** for
   the panel identified in this laptop's hardware report. The separate
   **1366×768** preset is for an HD-panel variant.
3. Leave **Video frame-rate limit** at **20 fps**, then start sharing and
   reconnect the HP. Keep **Match host resolution** enabled on the HP.
4. Short-press the HP power button to inspect displayed FPS, late drops,
   CPU/busiest thread, media queue and control round-trip time. Try **15 fps**
   if decoding still falls behind. Try **30 fps** after the 20 fps profile
   has stable presentation and CPU headroom. Changing the limit requires
   stopping and starting sharing.

The limit changes pictures per second while keeping the host desktop, encoded
frame and client panel at the same native size. It is a starting configuration,
not a qualified HP performance result. The host and client still share a
40 Mbps ceiling, with reservations for audio/input and network overhead.
Actual MPEG-2 traffic can be much lower. Raw 60 fps remains the explicitly
selected high-bandwidth developer profile; this selector applies to MPEG-2.

## Changes

- The host sends complete encoder pictures directly to the MPEG-2 RTP payloader.
  Its redundant elementary-stream parser previously waited for the following
  picture boundary. The receiver retains its parser to assemble RTP fragments.
- MPEG-2 encoding signals low delay, uses no B pictures and selects slice
  threading. The HP decoder uses two slice workers instead of allowing whole
  frame threading. FFmpeg's existing Atom-compatible assembly optimizations
  remain enabled. The client does no RGB conversion or video scaling before
  its XVideo sink.
- Host capture staging and client decoded-frame queues retain at most one
  waiting frame each. Capture cadence tolerates a small early arrival so that
  two independent rate gates do not discard alternate fresh frames.
- Media pipelines process dynamic `LATENCY` messages, measure actual sink
  presentation, and report late drops and configured playout latency. Queued
  or decoded frames no longer count as displayed FPS. QoS can drop late
  presentation while the MPEG-2 decoder preserves reference-picture decoding.
- Video adaptation uses video queues only. Previously the client included its
  audio playback queue in the video-pressure metric, which could lower video
  FPS during ordinary audio buffering. With older clients, the host uses
  CPU/loss feedback and disregards that ambiguous aggregate queue.
- Client feedback also reports the busiest logical CPU and late presentation.
  A saturated worker on the N270 can be hidden by a 50% aggregate CPU reading.
  Native-resolution profiles adapt frame rate without shrinking text.
- Opus keeps packet-loss concealment and disables receiver FEC lookahead.
  The pinned decoder reserves 120 ms with FEC enabled; removing this lookahead
  reduces playback latency at the cost of relying on concealment for lost
  audio packets.
- The Linux host is compiled with compiler optimization enabled.

The overlay's **playout** value is the media pipeline's clock compensation.
**Control round trip** is measured over TLS. Neither is an end-to-end
input-to-display measurement; capture, encoding and network transit also take
time. The release tests do not establish the HP's achievable FPS or latency.

## Validation and remaining measurement

The release evidence includes native regression tests and actual i386
MPEG-2/RTP/Opus/DTLS-SRTP execution using the appliance's libraries. It checks
steady presentation cadence, bounded media timing, authentication, aggregate
packet pacing, independent audio under video saturation, dual-stream handling,
native dimensions, cursor/input ownership, pairing and reconnect. Software
KWin and Xvfb run privately; a separate network namespace prevents collisions
with a currently running host. Offline BIOS and packaged-image checks validate
the boot artifact. Their receipts accompany the image and host bundle.

For the physical HP, record a two-minute ordinary desktop session over the
same 5 GHz Wi-Fi. Compare target/displayed FPS, late drops, total/busiest CPU,
queue time and control RTT. Repeat during scrolling or video. A rising RTT
suggests network queuing; high CPU or late drops with a low RTT suggests local
decode/presentation pressure. These are diagnostic indications, not proof of
a single cause. Sustained dual-display, webcam and real Wi-Fi performance
remain part of hardware qualification.

Primary implementation references:

- [GStreamer latency handling](https://gstreamer.freedesktop.org/documentation/additional/design/latency.html)
- [GStreamer sink presentation and QoS](https://gstreamer.freedesktop.org/documentation/base/gstbasesink.html)
- [FFmpeg MPEG-2 decoder used by the appliance](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n6.1.5/libavcodec/mpeg12dec.c)
- [Pinned GStreamer libav decoder timing](https://raw.githubusercontent.com/GStreamer/gstreamer/1.24.13/subprojects/gst-libav/ext/libav/gstavviddec.c)
- [Pinned Opus decoder FEC latency](https://raw.githubusercontent.com/GStreamer/gstreamer/1.24.13/subprojects/gst-plugins-base/ext/opus/gstopusdec.c)
