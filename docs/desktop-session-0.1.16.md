# RemoteOS 0.1.16 — viewer geometry and Chrome in the dedicated desktop

Use the [HP image with Broadcom wl and b43 recovery](../output/releases/hp2140-desktop-wl-0.1.16/images/remoteos-hp2140.img.gz)
and the [Linux application host](../output/releases/desktop-host-linux-0.1.16.tar.gz).
This update addresses automatic viewer fullscreen and Chrome/Chromium page
loading inside the independent KDE Plasma desktop.

## Chrome and the private KDE session

A fresh Chrome profile reproduced the reported loading hang in a private KDE
desktop: Chrome stayed running, but a loopback HTTP server received no page
request. Chrome was waiting for KDE Wallet initialization. The private D-Bus
session still had the backing X server's display in its activation environment,
so the wallet dialog opened outside the visible nested KDE desktop.

The launcher now publishes the nested desktop's Wayland/Xwayland display and
desktop environment to its private session bus before desktop helpers start.
The update is confined to that bus; it does not import environment variables
into the logged-in user's systemd services.

A second fault was reproduced in the installed KWallet 6.23 compatibility
bridge. After successful encrypted-wallet creation, it discarded the returned
collection object and immediately searched a stale collection list. That
lookup failed, and the bridge returned without the asynchronous completion
signal Chrome was awaiting. Restarting Chrome could find the wallet later.

The Linux bundle includes a private compatibility bridge with the upstream
collection-ownership fix, retention of the newly created collection object,
completion reporting for failed open requests, and a private prompt handler
that returns an error when the host library supplies an unusable result. Its
service declarations
exist only in the dedicated desktop's temporary runtime directory. The
installed Secret Service still owns wallet encryption and setup/unlock.
Normal GPG support, Chrome's sandbox and its default encrypted password backend
remain enabled. Full corresponding source, patches, build information and
licenses accompany the private component.

Complete normal wallet setup/unlock when Chrome requests it. This desktop has
its own wallet and browser configuration. If the wallet window is behind
Chrome, select it in the taskbar or use Alt+Tab. No browser restart should be
needed after completing first-use setup.

The qualified flow covers successful encrypted setup, later password unlock
and cancellation of a fresh-wallet request. The tested KDE Secret Service 6.23
returns a malformed result on cancellation; libsecret then attempts to return
a null error and strands the request. The private prompt handler keeps normal
result-type validation and turns an unusable result into an explicit failure.
It does not convert a failed prompt into a successful wallet opening or replace
the installed encryption daemon.

Stop the old dedicated desktop and launch the new host package before testing.
Reconnecting the HP to a desktop that was already running does not recreate
that desktop's helper processes. Save remote work before stopping the desktop.
Existing private desktop settings and browser data are retained.

## Viewer geometry

The client reapplies the active monitor's native bounds when the remote session
becomes ready. Hidden setup content must not set the video page's minimum size.
The power overlay remains a separate local window, and remote input resumes
after it closes. Native encoded pixels and host logical display dimensions
remain independent of client window geometry.

The appliance runs Xorg without a window manager. A GTK fullscreen request alone
is not evidence that the viewer or its video surface fills the panel; validation
checks both surfaces' actual bounds and their origin. The explicit development
`--windowed` mode remains available.

## Installation and validation

Extract the host archive and run `Start-RemoteOS-Host.sh` as your normal user.
The bundle uses the recorded Ubuntu 26.04 ABI; Ubuntu 24.04 needs a native build.
Decompress the USB image if your disk-image writer requires it and write the
image to a 4 GB or larger flash drive. Whole-disk writing replaces its state
partition, so preserve needed settings and reports first.

The release's [validation summary](../output/releases/hp2140-desktop-wl-0.1.16/validation/validation-summary.json)
records exact artifact hashes, private desktop/browser checks, native and
32-bit client checks, and packaged-image/BIOS evidence. Browser validation
requires the first normal sandboxed Chrome process to fetch an HTTP page and
execute a JavaScript callback after encrypted-wallet creation, with its Safe
Storage item present in that wallet. The packaged helper must also work after
relocating the bundle to a path containing spaces. Private software-display
tests do not measure the HP's real Intel display path or wireless performance.
The kernel, Broadcom driver selection, MPEG-2/UDP performance profile and b43
recovery option are preserved from 0.1.15.

References: [D-Bus activation environment](https://dbus.freedesktop.org/doc/dbus-update-activation-environment.1.html),
[D-Bus runtime service directories](https://dbus.freedesktop.org/doc/dbus-daemon.1.html),
[KWallet upstream collection fix](https://github.com/KDE/kwallet/commit/4468244f880a3c3971ae5005078e83935c164ecf),
[Chrome's asynchronous wallet initialization](https://raw.githubusercontent.com/chromium/chromium/155.0.8059.39/components/os_crypt/async/browser/freedesktop_secret_key_provider.cc),
[GTK fullscreen semantics](https://docs.gtk.org/gtk3/method.Window.fullscreen.html),
[GTK stack size requests](https://docs.gtk.org/gtk3/method.Stack.set_hhomogeneous.html).
