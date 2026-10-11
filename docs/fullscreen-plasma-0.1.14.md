# RemoteOS 0.1.14 — full-screen presentation and matching KDE desktop

This diagnostic build addresses the reported change from approximately 30 FPS
in a small RemoteOS viewer to approximately 5 FPS and 66% drops in full screen.
It also replaces the default minimal independent desktop with a separate KDE
Plasma desktop when the host user is logged into KDE.

## Install both components

Write `images/remoteos-hp2140.img` to a USB drive, or decompress the `.img.gz`
first. It is a legacy BIOS disk image with a read-only SquashFS root and a state
partition. Use a 4 GB or larger USB drive. Whole-disk writing replaces the drive's
contents, including an existing RemoteOS state partition; back up settings first.

Extract the matching `desktop-host-linux-0.1.14.tar.gz` and run
`Start-RemoteOS-Host.sh` as the logged-in user. In the host window choose:

1. **Independent HP desktop · use host simultaneously**.
2. **Match logged-in desktop · KDE Plasma** (the default).
3. The actual HP panel size: **1024×576**, or **1366×768** for the HD model.
4. **60 FPS** and **Trusted LAN / Tailscale · plain UDP media**.
5. **Start sharing**, then connect the HP using its saved pairing or the host PIN.

The supplied hardware report identifies the particular HP panel as 1024×576.
Connecting the client resizes the independent desktop to its detected panel and
VGA sizes, with one encoded stream per display and no text scaling.

## Full-screen changes

The appliance keeps Intel SNA acceleration, textured XVideo presentation and
the cursor fix. Its Xorg configuration sets `SwapbuffersWait=false`. In the
pinned Intel driver this initializes each textured port's `XV_SYNC_TO_VBLANK`
to zero, preventing the GPU scanline wait in that path. This prioritizes
immediate presentation and may allow visible tearing. The framebuffer remains
at its native display timing.

Video presentation has `sync=false`, `qos=false` and unlimited clock lateness.
The MPEG-2 decoder also has clock QoS disabled. The existing bounded jitter
buffer, one-frame decoded queue and reference-aware stale-picture recovery
still prevent delayed frames accumulating. Audio retains its playback clock.

No permanent 30 FPS cap is added. The host advertises its selected FPS ceiling
accurately. Its window shows requested FPS, effective sending FPS and the
adaptation reason. The client status adds the output sink/adapter and separate
totals for display-queue replacement, decoder discards and stale recovery.
Decoded and displayed FPS remain separate measured rates.

The full-screen report suggests a presentation bottleneck; these changes address
the identified waits and decoder feedback, but the physical HP must confirm the
root cause and improved performance. A successful 60 FPS builder test is not an
HP performance result. Native pixels, a 40 Mbps connection limit, host encoding
and MPEG-2 over UDP remain the defaults.

## Independent KDE Plasma

The launcher starts installed KDE components inside a private display, Wayland
compositor and D-Bus session. A private userspace X server supplies capture and
input to the nested compositor, including native Wayland apps. No kernel module,
system service, login-manager reconfiguration or real monitor/input-device
modification is involved. Two independent nested outputs follow the HP panel and
VGA geometry at scale 1. The second output is disabled when VGA is absent.

The private X server's presentation clock stays active at 60 Hz. Without this,
TigerVNC idles its Present clock at 1 Hz because direct X11 capture does not
attach a VNC viewer. A passive connection to the protected local socket keeps
the clock active; it never requests VNC frames or sends input. RemoteOS still
captures pixels directly and performs a single MPEG-2 encode per stream.

On first launch it copies KDE appearance and the primary screen's widget/panel
settings into `independent-desktop/config` below the host state directory. It
keeps those remote settings for later sessions. Installed user themes and widgets
remain available through a data search path. It starts the real Plasma shell and
KDE session services needed for desktop/app integration, rather than the small
RemoteOS application launcher. The usual full login startup and global autostart
sequence are not run, since those can redirect an already active user's session.

The two desktops use the same Unix account and home files. Display, keyboard,
pointer, session bus, application settings and playback audio are separate.
Disconnect retains the remote desktop while the host app stays open. **Stop
desktop** or closing the host window terminates its apps, including detached
children; save remote work first. Local applications are outside that supervision.

Matching mode currently supports installed **KDE Plasma 6**, including Wayland
hosts. It requires `kwin_wayland`, `plasmashell`, `kded6` and `kscreen-doctor`.
Missing components or a different desktop are reported explicitly. **Minimal
desktop · Openbox** is an optional, explicit choice. GNOME/Xfce matching, separate
Unix-user provisioning and a Windows independent session are not implemented by
this update. The Linux bundle still requires the builder's Ubuntu 26.04 runtime
compatibility; this is not a cross-distribution installer.

## Validation and the next HP measurement

The release's `validation` directory binds checks to executable and image hashes.
It contains native regression results, i386 media tests using the target libraries,
a BIOS boot probe, cursor rendering checks and an image-content audit. The real
KDE integration checks native resize, VGA enable/disable, two actual Plasma
desktops, a panel, maximization and mouse/keyboard input in a native Wayland app,
separate local input, 60/30 FPS capture/encode/decode, stream-specific adaptation
and cleanup. Display performance on the builder uses headless video sinks.
The KDE test also measures an animated native Wayland app's redraws, verifies
clicks beyond its former window size, and confirms zero VNC framebuffer updates.

After booting this image, compare windowed and full-screen viewer performance at
the same native stream size. In the power-button status overlay, record:

- Requested/effective/decoded/displayed FPS and the adaptation reason.
- XVideo adapter, decode time and receiver timeline delay.
- Display-queue, decoder and recovery discard totals over the same interval.
- CPU/busiest-thread usage, packet loss and control RTT.

The hardware report now includes the Intel presentation configuration. Check
`SwapbuffersWait=false` and the textured port's current `XV_SYNC_TO_VBLANK=0` in
`xvinfo`. A live HP test is required before claiming reliable native full-screen
30 or 60 FPS.

Primary implementation references: [Intel driver documentation](https://cgit.freedesktop.org/xorg/driver/xf86-video-intel/tree/man/intel.man),
[GStreamer BaseSink](https://gstreamer.freedesktop.org/documentation/base/gstbasesink.html),
[KWin's userspace X11 backend](https://invent.kde.org/plasma/kwin/-/blob/v6.5.5/src/backends/x11/x11_windowed_backend.cpp),
[TigerVNC presentation clock](https://github.com/TigerVNC/tigervnc/blob/v1.15.0/common/rfb/VNCServerST.cxx).
