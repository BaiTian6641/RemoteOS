# RemoteOS Linux host 0.1.9 — explicit virtual display selection

This host-only update works with the HP 0.1.8 image. In the host application,
choose **Display source → Virtual display · Wayland portal**, then **Start
sharing** and approve screen, keyboard and pointer access. Pair or reconnect
the HP using its saved host entry. Existing monitor sharing remains available.

The old request combined MONITOR and VIRTUAL. KDE Plasma's RemoteDesktop portal
checks for VIRTUAL alone; the combined request instead shares physical monitors.
The application now requests exactly the selected source type and places the
source control before network settings. The KDE permission popup may still be
a confirmation dialog: the virtual-display choice is in RemoteOS Host.

Plasma 6.5 creates a **1920×1080** virtual desktop through this portal. To use it
with the HP's native 1024×576 or 1366×768 panel, clear **Match the host desktop to this laptop’s screen resolution**
in the HP’s connection options. Capture, scaling, MPEG-2 compression and packet pacing remain on the
host under the existing 40 Mbps connection budget. The host desktop stays
1920×1080; this is an independent virtual desktop with scaled video, not an
HP-sized host desktop. The app shows the actual capture resolution while ready.

Exact HP-sized virtual desktops and two virtual displays require additional
compositor integration. This release uses standard portals and installs no
kernel driver, service or compositor extension. PipeWire caps are not used to
pretend a logical desktop changed size. Wrong-size exact matching explains the
actual and requested sizes and preserves the sharing grant for another attempt.

Virtual requests that are unsupported, return physical screens, or return more
than one output are rejected before PipeWire capture and the network listener.
Normal existing-source requests use MONITOR alone even when the portal advertises
VIRTUAL. Stop sharing closes the permission session and its virtual output.

Validation uses private D-Bus, generated PipeWire sources and isolated Xvfb
windows. It checks exact source flags, one-source enforcement, no physical
fallback, fixed-size errors, actual geometry, pointer endpoint mapping, input
release, permission revocation, host-side encrypted streaming and app lifecycle.
These fixtures validate the protocol contract; an interactive run on the user's
KDE session and the physical HP remains a release qualification test.

Primary references:

- [KDE Plasma 6.5 RemoteDesktop implementation](https://raw.githubusercontent.com/KDE/xdg-desktop-portal-kde/Plasma/6.5/src/remotedesktop.cpp)
- [ScreenCast portal interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- [RemoteDesktop portal interface](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)

The supplied native Linux bundle targets Ubuntu 26.04 development systems.
Build locally for Ubuntu 24.04 using the application host guide; a 26.04 binary
bundle is not claimed to run on 24.04.
