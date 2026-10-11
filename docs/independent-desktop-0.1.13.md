# RemoteOS 0.1.13: independent desktop and fresh-frame presentation

This diagnostic update adds a separate Linux application desktop and reduces
avoidable delay in the Atom client path. Performance on the actual HP remains
unqualified; selecting 60 fps sets a target, not a measured guarantee.

## Using the independent desktop

Extract the complete Linux host bundle and run `Start-RemoteOS-Host.sh` as your
normal logged-in user. The defaults are **Independent HP desktop**, **60 fps**
and **Trusted LAN / Tailscale · plain UDP media**. Select **1024×576** for the
panel identified in the supplied HP boot report; the HD variant is **1366×768**.
Press Start sharing and connect/pair on the HP with the displayed PIN.

The launcher starts a private, resizable Xvnc X server, Openbox window manager,
application launcher and D-Bus session. Xvnc supplies only the host-side X
framebuffer: RemoteOS captures it using MIT-SHM and sends MPEG-2 over RTP/UDP,
not VNC. There is no physical monitor extension, EVDI/uinput module, login
manager change, service installation or root requirement. It can coexist with
the host's KDE Wayland or Xorg desktop. Existing-monitor and KDE-extension
sharing remain selectable alternatives.

The independent session uses **the same Unix account and files**, with its own
XDG application configuration, cache and data directories. This is an
independent display/input context, not a separate OS account or security
boundary. D-Bus application activation occurs on its own bus. Known Firefox
and Chromium launchers receive separate browser profiles; other applications
using their own global singleton/IPC schemes still need qualification. Sandboxed
applications such as Snap/Flatpak may need additional portal integration.

Applications and Terminal are available in the top panel; Alt+Tab switches
windows. Save work before **Stop desktop · closes apps** or closing the host
app: both end the session and its applications. A client disconnection retains
the desktop while the sharing app remains running, permitting reconnects.
The host retains the same pairing identity across separate desktop restarts.

Connected HP screens determine native logical and encoded dimensions
automatically, even if Match host resolution is unchecked. The private virtual
framebuffer resizes and exposes logical monitor regions for independent
streams. Both monitors share the 40 Mbps budget. VGA changes replace only the
private layout; no physical host output is reconfigured. Openbox uses the
server's logical monitor information for its work areas.

Playback audio runs on an owned PipeWire/WirePlumber/PulseAudio-compatible
server with a virtual stereo sink. Hardware audio, Bluetooth and camera
monitors are disabled in that server; the host's local speakers/microphones
are not captured by the independent session. The MPEG-2 sender captures its
private `remoteos-output` monitor and sends Opus to the HP. Desktop apps use
that server through their private runtime directory and Pulse socket.

X11 TCP and VNC TCP listeners are disabled. X clients require a fresh private
MIT-MAGIC-COOKIE-1 credential. Xvnc requires a local VNC Unix socket, which is
confined to the session's 0700 runtime directory with mode 0600. The public
network surface remains RemoteOS's paired control and media endpoints.
Session startup chooses a free X display through `-displayfd`.

## Changes affecting latency

- Video presents fresh decoded frames immediately instead of waiting again
  on a playback clock. Audio retains clocked playback. RTP reordering keeps
  its 10 ms budget, and the decoded queue retains only one frame.
- If a framed MPEG-2 stream is more than 80 ms behind its receiver timeline,
  the client discards stale pictures and dependent P pictures until a fresh
  I picture arrives. It requests recovery through the authenticated control
  connection, at most about once per 150 ms. Reference chains are preserved;
  arbitrary P-frame dropping is not used to manufacture an FPS result.
- Xorg capture uses a reusable private MIT-SHM image when supported, with
  XGetImage fallback. Capture and input use separate X connections. XTEST
  input is flushed immediately without a synchronous acknowledgement per
  event; release-all remains synchronized.
- The appliance disables Wi-Fi power saving after link setup and reapplies
  it after driver/interface recovery when supported. This trades battery
  savings for interactive responsiveness.
- The power overlay includes measured displayed FPS, average decode time,
  receiver timeline delay, per-display video queue/drop/loss, host capture
  and encode rates, actual RTP payload rate, requested FPS and the latest
  adaptation decision. These are
  different measurements: none is complete input-to-photon latency. A video
  playout value of zero means no sink clock wait, not zero network delay.
- Network packet loss now lowers MPEG-2 bitrate before frame rate. The old
  controller reduced FPS while leaving bitrate unchanged, increasing picture
  size and potentially staying at 10 FPS. Decode/presentation pressure still
  lowers FPS, with native pixels preserved. Each display uses its own feedback;
  audio or VGA loss cannot throttle the built-in stream. New pipelines get two
  seconds to settle; six stable seconds permit a higher-FPS probe and twelve
  stable seconds a gradual bitrate recovery. Small loss below 1% does not block
  recovery. These thresholds do not prove that a particular Wi-Fi link is fast.
- Decoded FPS is measured before presentation, and the drop count includes a
  conservative count of pictures replaced by the one-frame decoded queue.
  Thus target 10 / displayed 5 is distinguishable from host encode 5, decoder
  5, or decoder 10 / display 5 rather than attributed to CPU percentage alone.

Native resolution is preserved. Host capture/conversion/compression and
packet pacing retain the 40 Mbps aggregate limit. The HP still performs
software MPEG-2 decode and XVideo YUV presentation; full GMA 950 hardware
decode is not claimed. Independent audio/video clocks prioritize responsive
desktop interaction and do not promise movie-style lip synchronization.

## Validation and boundaries

Private display/network fixtures exercise native resize, dual streams,
keyboard/pointer isolation, disconnect releases, no X11/VNC TCP listeners,
private audio and cleanup. Actual i386 executables use the image's libraries
for media regression checks on the builder. Image boot and cursor checks use
isolated QEMU/Xvfb. Release validation receipts bind results to binary/image
hashes. Builder throughput does not establish Atom or Wi-Fi performance.

The Linux bundle is built for the distribution recorded in `build.json`
(Ubuntu 26.04), including pinned TigerVNC 1.15.0 and Openbox 3.6.1 userspace
packages. System glibc, Python3, XKB/fonts and desktop applications remain
required. Ubuntu 24.04 needs a native build; Windows does not gain an
independent concurrent desktop from this release. Windows client editions
and host sharing APIs require a separately designed session implementation.
HP sustained dual-display/webcam throughput and on-device browser sign-in
remain hardware qualification tasks.
