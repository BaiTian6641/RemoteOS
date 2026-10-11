# RemoteOS application host

Open `remoteos-host-app` inside your normal logged-in desktop and choose
**Start sharing**. The application creates a private per-user TLS identity,
shows a verification value and generates a six-digit one-use PIN valid for
ten minutes. On the HP, select this computer in the host list, compare the
verification value on both screens, then enter its PIN. Manual IP/hostname
entry with the full verified SHA-256 fingerprint remains available. Paired client
identities persist; another code is needed only for a new client. Choose
**Stop sharing** or close the window to stop the host process and its listener.
Opening the window alone does not start sharing. Run one instance per user.

The appliance defaults to MPEG-2 under a **40 Mbps connection limit**. The
0.1.16 Linux host app starts with a **60 fps target** and provides 15/20/30/60 fps
choices. The 60 fps option requires testing on the physical HP. Native pixels remain unchanged; capture, resizing, YUV
conversion and encoding run on the host. Native monitor dimensions are sent
separately from the client's logical and encoded dimensions; pointer events
map back to the desktop. **Match host resolution** is enabled on the HP by
default: Xorg uses RandR modes or a host GPU transform to expose the client
desktop dimensions and restores the previous modes, transforms and layout on
disconnect. Windows tests/applies driver-supported modes and restores them;
unsupported modes are reported. A physical Wayland output must already match,
or be configured in the host's display settings. The standard portal does not
provide a desktop-resolution setter. Captured pixel dimensions and compositor
logical dimensions are checked separately; changing PipeWire caps is not
accepted as evidence of a resized desktop. Clearing **Match host resolution**
on the HP enables host-side video scaling for existing monitors and the generic
portal virtual source. **HP native virtual display** always requires exact
panel dimensions, including if matching was unchecked on an older client.
See [the motion update](motion-udp-0.1.15.md) for independent receive/decode
queues, immediate ordered-picture delivery with a 150 ms video repair deadline,
40 ms audio jitter, live video tuning and automatic bitrate recovery. The
[earlier UDP update](smooth-udp-0.1.12.md) describes asynchronous Wayland input,
pointer coalescing and plain/encrypted media choices. Older images remain compatible with negotiated legacy encrypted
media and their supported frame rates; the complete update requires both sides.

In the Linux host app, **Independent HP desktop** defaults to matching the user's
installed **KDE Plasma 6** environment in a private session that can coexist with
the local desktop. It copies appearance and primary-screen widgets/panels into
separate settings. **Minimal desktop · Openbox** remains an explicit option.
Other desktop environments are reported as unsupported by matching mode.
The independent desktop uses the same Unix account
and files, with separate display/input, configuration, session bus and playback
audio. Connected HP displays resize this session at native pixels. Save work
before stopping it: Stop desktop and window close end its applications.
See [the full-screen and Plasma update](fullscreen-plasma-0.1.14.md).
The [0.1.16 desktop update](desktop-session-0.1.16.md) corrects private D-Bus
helper activation so Chrome's KDE Wallet dialogs appear inside this desktop,
and the bundle supplies a private repaired KWallet 6.23 bridge for browser
initialization. The installed encrypted Secret Service remains in use; private
service declarations disappear when the desktop stops. Complete normal wallet
setup/unlock if requested, using Alt+Tab if its dialog is behind Chrome.
Stop the old desktop and
start the updated host to recreate its helper processes.

For sharing the current desktop, choose **Display source → Extend current KDE desktop · HP native pixels**,
**Share current monitors**, or **Virtual display · Wayland portal** before starting. Existing monitors requests
MONITOR only and permits up to two outputs. Virtual display requests VIRTUAL
only, with one output. This distinction is required by KDE's RemoteDesktop
backend: combining MONITOR and VIRTUAL selects its physical-monitor branch.
The permission popup can be a confirmation dialog without a screen chooser;
the source selection now lives in RemoteOS Host. The app shows actual captured
resolution after permission and the first frame succeed.

**KDE native mode (0.1.10):** select the HP panel size before Start sharing.
The default is **1024×576**, matching the tested laptop; **1366×768** is available
for the HD variant. This extends the current desktop and shares its input. A 1920×1080 single-output profile is also selectable for an
external display. The KDE adapter creates an actual virtual output at that
resolution with scale 1, then obtains the normal RemoteDesktop/ScreenCast grant.
It verifies the owned Wayland output mode, its granted PipeWire stream name and
serial, compositor geometry, and captured pixels. Only that verified stream
enters the capture pipeline. The client panel must match; an error tells the
user to stop sharing and select the correct size. Native mode keeps encoded
dimensions equal to the panel and adapts frame rate without reducing resolution.
One native virtual output is implemented in this release; two native virtual
displays remain an integration requirement.

The adapter declares its own executable's use of `zkde_screencast_unstable_v1`
in a hidden per-user `.desktop` entry and refreshes KDE's service cache with
`kbuildsycoca6`. It never changes KWin permission checks or uses private fake
input. This is a pinned KDE-specific protocol, not a portable portal extension.
KWin OpenGL compositing and a PipeWire policy manager are required. QPainter
is rejected before creating an output because Plasma 6.5's screencast plugin
requires an EGL display. Stop sharing closes the grant and removes only the
owned output; short client reconnects retain it.

The generic **Virtual display · Wayland portal** option still follows the
portal's chosen resolution. On Plasma 6.5 that remains **1920×1080**, and an HP
panel requires explicit host-side video scaling for that option. Use **HP native
virtual display** for exact HP-sized text. Native mode does not replace
ordinary logged-in-session consent, and does not add login/lock-screen access.

A missing VIRTUAL capability, an unexpected physical output returned for a
virtual request, or an unexpected output count fails before capture/listening;
the app never substitutes the user's physical screen for a virtual request.
Exact matching at the wrong portal resolution reports both actual and requested
sizes without renegotiating capture caps. The generic source can reconnect using explicitly selected scaling;
the sharing grant remains valid. A host with only one available source negotiates
one stream, including when the HP has a VGA screen attached.

The host caps the aggregate video encoder allocation at 80% of the negotiated
limit (32 Mbps at the default), with MPEG-2 VBV/maxrate control, no B-frames
and half-second GOPs. UDP datagrams are paced separately for each stream,
charging 128 extra bytes per packet for IP/tunnel framing. Media reservations
total at most 90% of the connection budget; playback has its own allocation
so video bursts cannot block it. Input/control retains the remaining margin.
This limits RemoteOS media, not unrelated applications on the host. Raw-I420
would exceed 40 Mbps and is rejected; its high-bandwidth transport remains
available only for explicit developer qualification profiles.

Discovery uses UDP 47009, local broadcast/multicast and unicast queries to
online Tailscale peer addresses. It advertises the host name, control port and
public fingerprint only while sharing, with nonce checking and bounded request
rates. PINs are never advertised. Saved identities are never replaced by
discovery. A changed identity is flagged and requires explicit verification.
Discovery uses IPv4; manual connections also support resolvable hostnames and
IPv6. Peer queries need the tailnet ACL/firewall to allow UDP 47009.

Default control uses TCP 47010; two video streams and playback audio use UDP
47020–47022. Connect directly over a reachable LAN or enroll both computers
in Tailscale independently. A host firewall may require allowing the
application or these ports. The build/launcher does not change firewall rules.
Only one controlling client is supported. Sharing is restricted to paired
client certificates. **Trusted LAN / Tailscale · plain UDP** removes application
media encryption, with peer address/port and per-session SSRC filters; these
are not cryptographic media authentication. Tailscale's own protection remains.
**Encrypted UDP · DTLS-SRTP** pins media certificates through the control
connection. Both retain TLS control and input. Losing control releases tracked keys/buttons. The window does
not bypass host login, authorization or locks.

## Linux / Ubuntu Wayland and Xorg

Requirements: C++17 compiler, CMake, Ninja, OpenSSL 3 development files, GTK3,
X11/XTEST/RandR/XFixes development files, GStreamer ≥1.24 development files
and runtime plugins. The necessary sender plugins include core/app, UDP,
RTP/RTP manager, DTLS/SRTP, libav MPEG-2, MPEG video parser, video conversion
and scaling, Opus, audio conversion and resampling. PipeWire's GStreamer
plugin and an accessible current-user PipeWire server supply playback audio.
Wayland capture also requires that plugin, a session D-Bus, `xdg-desktop-portal`
and a desktop backend exposing **both RemoteDesktop and ScreenCast**. GNOME
and KDE Plasma are the intended desktop integrations. A backend providing
only ScreenCast cannot forward keyboard/pointer input: the app reports that
requirement instead of silently using Xwayland or installing uinput.
The source sets `stream.capture.sink=true`; it requests playback, not a
microphone. If the PipeWire plugin is missing, the offer reports that audio
is unavailable and video remains usable. Uncheck playback audio to test a
video-only host.

Build on the machine/distribution that will run the application:

```sh
packaging/linux/build-desktop-host.sh
build-desktop/remoteos-host-app
```

For this repository's extracted development libraries, first source
`cmake/dev-env.sh` from the repository root. The build explicitly disables
service integrations. Wayland builds also need Wayland client/scanner and
PipeWire development packages (`libwayland-dev`, `libpipewire-0.3-dev`).
The application explicitly disables
EVDI linkage. Capture uses Xorg framebuffer readback and XFixes cursor
composition; XTEST provides normal user-session input. No EVDI, uinput,
module signing, display-manager setup, service installation or `sudo` is
needed. Xorg authorization still applies. Start this application from the
desktop session, where `DISPLAY` and `XAUTHORITY` identify the authorized
server. Monitor hotplug/resizing requires reconnecting the client. Closing or
restarting the X server ends capture.

On Wayland, choose the display source in RemoteOS Host, then **Start sharing**
and approve screen and remote-input access in the desktop's permission dialog.
The desktop may offer a chooser for existing outputs or only confirm access.
For a virtual request, the app requires exactly one virtual source. The
listener opens and the pairing code becomes usable after permission and
the first captured frames succeed. Canceling or denying permission leaves
sharing off. **Stop sharing** works while that dialog is pending and closes
the request. Permission must be granted again on each new Start sharing;
this version does not persist a portal restore token or bypass the desktop's
consent settings. The GTK window parents the dialog using an exported Wayland
window handle when the compositor supports it.

The host combines the documented
[RemoteDesktop portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)
and [ScreenCast portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
in one permission session. It uses the portal's restricted PipeWire descriptor
for capture and the `Notify*` portal methods for evdev keyboard/buttons,
absolute/relative pointer motion and scrolling. These are the documented
D-Bus input path; the app does not establish an EIS connection. An embedded
cursor is required. Native captured pixels and compositor coordinates remain
separate, including HiDPI scaling and screen placement. Newer portal stream
serials are preferred for targeting; older backends can supply node IDs.
Captured BGRA frames enter the existing host-side scaling/encoding path.
Queues keep one recent capture buffer per monitor and late frames are dropped.
MPEG-2 and explicit developer raw profiles use the same HP protocol.

The optional KDE native-output adapter creates its own HP-sized output before
the sharing grant, then captures only that verified, consented stream. See
the [0.1.10 native display update](native-display-0.1.10.md) for geometry,
stream identity, compositor requirements and validation boundaries.

A client disconnect releases held keys/buttons and stops frame delivery;
the grant remains active while the app is sharing, so a saved client can
reconnect. Stopping/closing the app destroys the pipelines, closes the
PipeWire connection and closes the portal session. Permission revocation,
portal restart, stream failure or an unexpected source resolution change stops sharing
and closes the listener. Start sharing again to choose the new source.
Desktop locks, secure prompts and reserved shortcuts remain subject to the
compositor's policies. The application does not supply a login/lock bypass.

Identity, configuration and paired clients live under
`$XDG_DATA_HOME/remoteos-host`, or `~/.local/share/remoteos-host`.
`--state-dir DIR` selects a separate profile for diagnostics.

## Windows 11 x64

The native launcher starts `remoteos-host.exe --desktop` as its current user.
DXGI Desktop Duplication captures existing outputs and the host composes the
cursor. `SendInput` forwards keyboard/pointer input. WASAPI loopback supplies
playback audio. Both executables use an `asInvoker`, `uiAccess=false` manifest;
the application build excludes the service executable and installs no
IddCx/VDD, USB/IP or kernel drivers.

Build in an x64 Visual Studio developer PowerShell with CMake, Ninja,
pkg-config/pkgconf, OpenSSL 3 and matching **MSVC x64** GStreamer runtime and
development SDKs ≥1.24. Use paths to the installed SDKs:

```powershell
packaging/windows/Build-DesktopHost.ps1 `
  -GStreamerRoot 'C:\gstreamer\1.0\msvc_x86_64' `
  -OpenSSLRoot 'C:\your-openssl-sdk' `
  -PkgConfigExe 'C:\your-pkgconf\pkgconf.exe'
```

Choose new output/build directories with the corresponding parameters when
needed. Open `output/desktop-host-windows/Start-RemoteOS-Host.cmd`, then choose
**Start sharing**. The script stages the two executables, runtime DLLs and
GStreamer plugins without installing a service or changing OS configuration.
The SDKs must actually include `avenc_mpeg2video`, RTP, DTLS/SRTP, Opus and
WASAPI; a successful C++ compile alone does not establish media availability.
Host identities live under `%LOCALAPPDATA%\RemoteOS\Host`.

Ordinary application permissions cannot inject into higher-integrity windows
or the UAC secure desktop. Ctrl+Alt+Delete, the login screen and unattended
startup require the separately installed privileged edition. See Microsoft's
[SendInput privilege restrictions](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).
The Windows backend and launcher cross-compile in this workspace; a full
MSVC/SDK package and capture/audio/input runtime still require validation on
Windows. This is an implemented application-host prototype, not a qualified
Windows release.

## Evidence and remaining qualification

`desktop-host-integration` runs on a private, cookie-authenticated Xvfb. It
checks actual framebuffer pixels, native capture dimensions, scaling from
640×480 to 320×240 on the sender, authenticated pairing, DTLS certificate
pins, raw60 negotiation, forwarding input, missing-second-monitor negotiation
and releasing held keys/buttons after a disconnect. It never captures the
developer's desktop. The Linux application's ELF dependency list contains
no libevdi. The service host's existing synthetic session and encryption tests
remain separate.

`wayland-host-integration` uses a **private portal contract fixture**, private
D-Bus and a real isolated PipeWire server with two generated video sources.
It checks source pixels and dimensions, two streams through one granted
descriptor, serial/node targeting, HiDPI input mapping, key/button/scroll
forwarding, input release, stopped delivery, reconnect and grant cleanup.
The fixture also verifies exact physical-monitor matching, rejects unsupported
physical resizing, requires MONITOR or VIRTUAL alone, checks fixed virtual
geometry and pointer endpoints, and rejects unsupported virtual resizing
without changing capture caps. Virtual requests that return physical screens
or multiple outputs are rejected without PipeWire access or a listener.
An actual host/client TLS/DTLS session verifies raw60 plus MPEG-2, source
scaling, display changes, persisted pairing and disconnect input release.
Cancel, denied input, an unsupported backend, missing embedded-cursor support,
too many monitors and invalid sizes open no listener. Revocation closes the
listener; Stop during a pending permission request closes it cleanly.
This fixture does not prove the real GNOME/Plasma permission dialog or capture
backend. Those desktops still require runtime qualification, especially
HiDPI, multi-monitor ordering, lock transitions and monitor hotplug.

`client-session-ui` connects two private desktops through the real GTK shell,
checks rendered cursor pixels through connection/overlay/disconnect, forwards
adjacent pointer coordinates and screen endpoints, and checks input release,
six-digit pairing, discovery and saved host persistence. `encrypted-media`
measures encrypted packet pacing under saturation and audio continuity while
the video allocation is full. These tests do not use physical input devices.

Actual host audio, real GNOME/Plasma portal behavior, mode changes on actual
graphics drivers, monitor hotplug, Windows runtime, HP presentation and sustained
dual-display MPEG-2 throughput are pending hardware qualification. Application
mode does not forward raw USB devices: that path requires host importer
drivers. The optional service edition retains that separate design.
