# RemoteOS 0.1.18 — power, userspace devices and portable client

This is a diagnostic release building on the user-confirmed smooth HP 0.1.17
fullscreen path. It preserves the Intel XVideo presentation path, Broadcom wl
profile, native panel detection and the Linux independent-desktop integration.
Hardware power/camera/SD qualification remains required. Windows and macOS
native binaries are not provided or qualified by this Linux build.

## Deliverables and platform status

- `output/releases/hp2140-power-devices-wl-0.1.18/images/remoteos-hp2140.img.gz`:
  BIOS USB/disk image for the HP, signed with the existing diagnostic key.
  Decompress before writing with a tool that requires raw `.img` files.
  Flashing a whole-device image erases that device. Keep your existing state
  backup if pairing and network profiles need to survive a reflash.
- `output/releases/desktop-host-linux-0.1.18.tar.gz`: ordinary-user Linux host
  bundle for the current Ubuntu 26.04 x64 ABI. Open `Start-RemoteOS-Host.sh`.
  It retains the qualified private KWallet bridge for the independent KDE
  desktop. No host service, display driver or input module is installed.
- `output/releases/RemoteOS-Client-0.1.18-x86_64.AppImage`: general Linux client,
  built with Ubuntu 24.04 libraries; targets Ubuntu 24.04 or newer x86_64 systems.
  X11 is supported directly; Wayland requires an available XWayland server.
  This is not a native Wayland client. Audio uses PulseAudio or PipeWire Pulse.
- `output/releases/RemoteOS-0.1.18-host-source.tar.gz`: reviewed source and native
  build routes for Windows and macOS, plus the common core. A build route is not
  evidence of a working native package.

Windows retains the existing DXGI/SendInput userspace backend and native window;
this release adds 15/20/30/60 fps, trusted/encrypted UDP controls, and device URL
status. Build on Windows with matching x64 MSVC GStreamer/OpenSSL SDKs using
`packaging/windows/Build-DesktopHost.ps1`. Native compilation, driver-free
installation, secure-prompt limitations and application compatibility still
need verification. Ordinary-user hosting does not provide UAC/login control.

The new macOS source uses ScreenCaptureKit and CGEvent input, with an AppKit
launcher. Build on macOS 13+ using `packaging/macos/build-host.sh` and development
SDKs. Screen Recording and Accessibility require macOS approval. The resulting
app currently depends on installed SDK libraries; it is not a portable signed
and notarized release. It shares existing displays; matching arbitrary client
resolutions and independent desktops are not implemented. Disable “Match host
resolution” when trying this backend. **Playback audio is also not implemented
on macOS.** This backend has not been compiled with an Apple SDK here.

## HP power behavior

- At 15% while discharging, show a low-battery notice in the power overlay.
- At 5%, show a critical warning. Warnings are latched per discharge cycle;
  charging or recovery above 20% rearms them.
- ACPI lid-close input is handled locally. Release host input, stop the remote
  session and forwarding, unmount owned SD volumes, then suspend to RAM.
- The restricted root supervisor selects `deep` sleep when the kernel offers
  it and writes `mem` to `/sys/power/state`. It never accepts a caller-provided
  sysfs path. Suspend errors remain visible locally.
- On resume, reconnect the previous active host. This depends on real firmware,
  Wi-Fi resume and network recovery; it must be tested on the HP.
- “Suspend client” is also available in the power overlay. The AppImage uses
  the desktop's logind suspend request rather than the appliance supervisor.

Policy tests use ordinary temporary files. No real suspend, power-off, camera
capture or SD mount was performed on the development machine.

## Webcam forwarding and a selectable host camera

Open the HP power overlay, select **Webcam and SD files**, then **Start webcam**.
The camera starts only after that action and stops on disconnect, setup/browser
mode and suspend. Stop webcam explicitly when finished. It does not restart
silently after reconnect.

The hardware report identifies the Chicony CNF8031 and lists YUYV only. The
initial profile is its supported **320×240, 5 fps YUYV** mode, roughly 6.15 Mb/s
before TLS/HTTP overhead. The HP captures and copies those frames; it does not
JPEG-encode them. The host converts them and encodes JPEG, supplying a loopback
MJPEG endpoint and an HTML camera page. Input/desktop video remain on their
separate existing channels, with a 12 Mb/s video-wire reservation while device
sharing is enabled. Joint Wi-Fi throughput still requires HP qualification.

To make it selectable in Zoom, Chrome or other host apps:

1. Install an appropriate maintained OBS build on the host.
2. Copy the camera page address displayed in the **host app**. It begins with
   `http://127.0.0.1:` and a random capability path; it is local to that host.
3. Add an OBS **Browser Source** using that address, at 320×240. Keep the source
   active; if using “Shutdown source when not visible”, do not hide its scene.
4. Start OBS **Virtual Camera**. Select **OBS Virtual Camera** in the target app.

A camera preview alone does not create a selectable OS camera. OBS is the
actual virtual-camera provider for this initial implementation. Windows uses
its supplied component. macOS may require approval of its camera extension.
Linux needs the distro's separately packaged `v4l2loopback` module (commonly
provisioned with DKMS), not a full kernel rebuild. Secure Boot module-signing
requirements still apply. RemoteOS does not install that module automatically.

Transport and host JPEG conversion pass synthetic-frame tests. Selection in
Zoom/Chrome and physical CNF8031 behavior are not qualified yet. There is no
microphone forwarding and no general camera-mode negotiation in this release;
the client uses `/dev/video0` and fails visibly if the mode cannot be delivered.

Primary references: [OBS virtual-camera guide](https://obsproject.com/kb/virtual-camera-guide),
[OBS camera troubleshooting](https://obsproject.com/kb/virtual-camera-troubleshooting),
[OBS Browser Source](https://obsproject.com/kb/browser-source).

## SD shared files

The HP retains ownership and mounts the card. No block device is imported by
host drivers. Use **Detect SD card**, select the desired partition and press
**Share SD files**. Supported appliance filesystems are FAT/FAT32 and exFAT.
Read-only is the default; explicitly enable uploads/deletion if desired.

The host app displays a loopback address. Open it in a browser to browse and
download files, or in a compatible WebDAV file manager for writes. The minimal
server supports OPTIONS, GET, HEAD, PROPFIND, PUT, MKCOL and DELETE; it does not
implement MOVE or LOCK. OS file-manager interoperability is not yet qualified.
Uploads are limited to 256 MiB. Files larger than the target's `size_t` range
are refused; on the HP this limits downloads to less than 4 GiB.

SD traffic is limited to 512 KiB/s per direction, about 4.2 Mb/s, separate from
input/audio. Transfers use bounded buffers. Uploaded files are staged and
synced before replacing the destination; an interrupted upload preserves the
previous file. Traversal, symlink escapes and externally hard-linked files are
refused. Directory listings are bounded to 1000 entries.

The privileged mount helper accepts only identified SD devices. It protects the
entire root/state disk using sysfs device identities and mountinfo, including
`/dev/root` aliases; it refuses already mounted devices. Mount flags include
nodev, nosuid and noexec. **Stop sharing** closes the relay and unmounts owned
cards. Wait for safe-unmount confirmation before removing the card. Network
loss does not automatically eject a physically attached card.

In the AppImage, choose an already mounted SD folder with the file picker. The
system desktop owns mounting/unmounting; RemoteOS does not require root.

## Encryption and network ports

Control, pairing, input and userspace-device tunnels retain TLS 1.3 and exact
certificate pinning. On x86 clients with AES and PCLMUL acceleration, AES-128-GCM
is preferred. Otherwise ChaCha20-Poly1305 is preferred, which covers the Atom
N270. The host honors the client's ordering. Idle TLS buffers are released.
Negotiated cipher is visible in the HP status overlay.

Trusted LAN/Tailscale video and audio preserve the existing plain UDP profile;
no additional TLS is added to media packets. Tailscale itself still encrypts
its traffic. No Atom encryption speedup percentage is claimed: the cipher policy
and actual handshakes were tested, not the laptop's CPU time.

Normal host app defaults: TCP 47010 for control, **TCP 47012 for device TLS**,
UDP 47009 for discovery and UDP 47020–47022 for media. If the control port changes,
the app chooses control+2 for devices (control−2 near the upper port boundary).
Custom JSON can override `resource_port`. Raw HTTP camera/file endpoints bind
only to host/client loopback. Reverse TLS means no inbound laptop port is needed.
Resource listeners start after desktop sharing permission/preparation succeeds.

## General client use

```sh
chmod +x RemoteOS-Client-0.1.18-x86_64.AppImage
./RemoteOS-Client-0.1.18-x86_64.AppImage
```

Configure networking and Tailscale using the system's normal tools; the general
client opens directly at host selection. State is stored under
`$XDG_DATA_HOME/remoteos-client` or `~/.local/share/remoteos-client`. It uses a
normal window by default. Pass `--fullscreen` for fullscreen mode.

Keyboard and pointer events are forwarded only while the viewer owns focus.
**Ctrl+Alt+Home** opens the local overlay. Losing focus releases held host input.
The application does not open evdev devices or hide the system's root cursor.
It uses ximagesink presentation rather than requiring Intel-specific XVideo.

If FUSE mounting is unavailable:

```sh
./RemoteOS-Client-0.1.18-x86_64.AppImage --appimage-extract-and-run
```

The runtime is pinned to AppImage type2-runtime 20251108 and verified against
the upstream GitHub asset digest. Runtime library origins, hashes, Ubuntu
package versions and license notices are included in the AppDir manifest.
Building on the oldest intended baseline follows the
[AppImage portability guidance](https://docs.appimage.org/reference/best-practices.html).

## Qualification required before wider release

On the HP: low/critical battery notices, held-key lid close, Wi-Fi resume,
physical camera with both desktop motion and SD transfers, card removal,
safe-unmount errors and sustained temperature/CPU/latency measurements.
On each native host: capture/input permissions, camera selection in real apps,
file-manager WebDAV behavior and disconnect cleanup. Windows/macOS SDK builds,
macOS audio and portable native packaging are still outstanding work.
