# Native HP virtual desktop — Linux host 0.1.10

The KDE Wayland host can now create the desktop at the HP panel's actual
resolution. The desktop, capture and MPEG-2 frame have identical dimensions,
with 100% compositor scale. This preserves the raster size of text.

The hardware report for this laptop identifies **1024×576**. **1366×768** is
also supported for the HP 2140 HD-panel variant. Select the size reported by
the HP; these are distinct panels, not interchangeable resolution presets.

## Use the update

1. Stop the old host application and extract `desktop-host-linux-0.1.10.tar.gz`.
2. Run `Start-RemoteOS-Host.sh` as your ordinary desktop user.
3. Choose **HP native virtual display · KDE Wayland** under **Display source**.
   This is the default when the app starts in KDE Wayland.
4. Choose **1024 × 576 · HP 2140 standard panel** for the tested laptop,
   or **1366 × 768 · HP 2140 HD panel** for that panel variant.
5. Choose **Start sharing** and approve screen, keyboard and pointer access
   in KDE's permission dialog. The app shows the actual native capture size.
6. On the HP, keep **Match the host desktop to this laptop's screen resolution**
   enabled under **Display and startup options**, then connect to the host.

The existing HP **0.1.8 image** is compatible. This update changes the host;
reflashing the HP is unnecessary. Existing per-user host identities and paired
clients are reused. The client source has updated matching help text, but the
published 0.1.8 image is unchanged.

Native mode rejects a panel mismatch even if an older client unchecked
matching. Stop sharing, change the host's panel selection and start again.
The generic **Virtual display · Wayland portal** option retains the portal's
chosen desktop size, which is 1920×1080 on Plasma 6.5. Choose the native option
for HP-sized text.

## Implementation and boundaries

The host uses the pinned KDE virtual-output protocol to create its own output
at the chosen size and scale 1. It registers its own executable in a hidden
per-user `.desktop` entry with KDE's interface declaration, refreshes the
service cache using `kbuildsycoca6`, and allows for KService's cache refresh
delay. KWin's normal permission checks remain enabled.

KDE's RemoteDesktop portal grants screen and keyboard/pointer access. Its
monitor branch returns all selected desktop streams; RemoteOS inspects only
the granted node metadata on a separate restricted PipeWire connection. The
owned output's unique name, actual mode, PipeWire media name and object serial,
portal logical geometry, and captured pixels must all match. Only that owned
stream enters a video capture pipeline. An unknown, ambiguous, wrong-size or
wrong-serial stream is rejected without a physical-monitor fallback.

Input uses the standard portal `Notify*` methods and the verified stream node;
the compositor accounts for the virtual monitor's position. Native capture
keeps the HP's pointer endpoints unchanged. Disconnect releases held keys and
buttons. The output survives short reconnects while the host app is sharing.
Stopping the app closes the grant and removes only the owned virtual output.
The host caches at most one latest capture per stream and sends it at the
negotiated cadence, so an idle desktop is immediately visible after reconnect
without waiting for another compositor repaint. Capture queues remain bounded.

The native profile retains encoded resolution under pressure, reducing frame
rate within supported MPEG-2 rates and recovering it when conditions improve.
It does not silently shrink video to reduce decoding or network cost. The
existing 40 Mbps connection limit, reserved audio/input budget, host encoding,
bounded queues and packet pacing remain in force. Native pixels do not make
MPEG-2 lossless, and real HP throughput remains a measured acceptance result.

Requirements: KDE KWin Wayland with the native screencast interface, OpenGL
compositing, `kbuildsycoca6`, RemoteDesktop/ScreenCast portals, PipeWire and a
running policy manager such as WirePlumber. QPainter is rejected before output
creation; Plasma 6.5's screencast implementation expects an EGL display. No
kernel module, compositor extension, root service, private input API or
permission-check override is installed.

This release implements **one native virtual monitor**. A 1920×1080 single
output can be selected for an external-only profile; it does not establish
simultaneous native LCD plus VGA support. Existing-monitor sharing still
supports up to two sources. Native mode shares an ordinary logged-in session;
it does not add access to login, lock or privileged secure desktops.

The provided bundle is built for the recorded Ubuntu 26.04 development system.
Build locally on Ubuntu 24.04 using the application host guide. The KDE-specific
protocol is not a portable standard and requires requalification after relevant
compositor changes.

## Evidence

`kde-native-display-integration` runs a **real KWin 6.5.5 compositor** with
software OpenGL and private Wayland, D-Bus and PipeWire instances. Private
WirePlumber runs its policy-only profile; no hardware Device objects are
created. Both HP panel sizes are verified against actual `wl_output` mode and
scale events, actual KWin PipeWire frame caps and frame bytes. A private portal
fixture grants the native stream alongside decoy monitor streams.

The test checks one-source selection, exclusion of the decoys, exact pointer
coordinates, held-input release, mismatch rejection, bad stream identity and
geometry rejection, and output removal after success and failure. Core tests
verify frame-rate adaptation and recovery without reducing native resolution.
Both panel sizes also pass a full application-host session through pairing,
TLS/DTLS-SRTP, native MPEG-2 reception, renegotiation and saved-identity
reconnect under a 40 Mbps budget. The owned virtual display survives reconnect.
The remaining suite covers encrypted MPEG-2/audio/raw transport, client cursor
ownership, discovery, pairing, saved identity, Xorg matching and existing
portal cancellation/revocation behavior.

The real KDE permission dialog and actual compositor input injection are not
exercised by the private portal fixture. An end-to-end run on the user's normal
KDE desktop and physical HP is still required; these tests do not prove sustained
HP Wi-Fi performance or simultaneous VGA streaming.

Primary references:

- [Pinned KDE virtual-output protocol](https://raw.githubusercontent.com/KDE/plasma-wayland-protocols/v1.19.0/src/protocols/zkde-screencast-unstable-v1.xml)
- [KWin native virtual-output creation](https://raw.githubusercontent.com/KDE/kwin/v6.5.5/src/plugins/screencast/screencastmanager.cpp)
- [KWin application interface declaration](https://raw.githubusercontent.com/KDE/kwin/v6.5.5/src/utils/serviceutils.h)
- [KDE Plasma 6.5 RemoteDesktop implementation](https://raw.githubusercontent.com/KDE/xdg-desktop-portal-kde/v6.5.5/src/remotedesktop.cpp)
- [RemoteDesktop portal interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)
