# Client shell and authentication browser

The native client is `remoteos-client`, built with `REMOTEOS_WITH_GTK=ON`.
The independently launched browser is `remoteos-browser`, built with
`REMOTEOS_WITH_BROWSER=ON`; WebKitGTK API 4.1 and GTK3 development headers are
required. Streaming additionally requires `REMOTEOS_WITH_MEDIA=ON`.

For a development desktop, use:

```sh
remoteos-client --windowed --no-input --no-autoconnect --no-discovery --state-dir /tmp/remoteos-client
```

The diagnostic shell never reports a simulated desktop as connected. A build
without the media receiver refuses to start a desktop session. The browser
refuses to run as root.

## First boot setup

A fresh persistent state opens a full-screen guided setup automatically. The
appliance starts Xorg and the unprivileged client from init; no desktop login or
terminal command is required. Setup uses four numbered steps:

1. **Welcome:** explain the remote desktop and the host details needed for pairing.
2. **Network:** select Wi-Fi or use Ethernet, check connectivity, and open a
   captive-portal browser when needed. A direct LAN connection does not require
   a successful internet probe. If no networks appear, check the HP's physical
   wireless switch.
3. **Connection:** sign in to Tailscale through the local browser, or explicitly choose a
   direct connection. Continuing with Tailscale requires its daemon to report
   `Running`; opening a sign-in URL or closing the browser is not completion.
4. **Host pairing:** choose an automatically discovered or saved host. For a new
   host, compare the short verification value with its app and enter the six-digit
   PIN. Manual address/fingerprint entry remains in **Connect by IP address or
   hostname**. Streaming uses MPEG-2 with the host's 15/20/30/60 fps limit,
   initially 30 fps in the 0.1.12 app, with a 40 Mbps connection
   limit shared by both screens, audio and control. Host resolution matching
   is enabled; unsupported matching reports a specific error with recovery
   instructions. Clearing that option explicitly enables host-side scaling.

The native GTK adaptation uses Carbon's Gray 100 colors, IBM Plex Sans, an 8px
spacing grid, top-aligned labels, visible keyboard focus, a numbered progress
indicator and a persistent primary action. It follows Carbon's
[multistep form guidance](https://www.carbondesignsystem.com/building-blocks/core/patterns/forms).

The header includes a Wi-Fi signal/status icon. It distinguishes disconnected,
associated but waiting for an address, and connected states; its tooltip shows
SSID, address, RSSI and the separately measured Internet status. Ethernet has
its own connected icon. Selecting the indicator opens network setup.
Startup waits for an associated network with a usable address before automatic
host connection. After 20 seconds without one, **Connect this laptop** offers
**Set up connection** or **Continue offline**. A failed Internet probe or
captive-portal response also offers setup. Direct LAN use remains available.
Content scrolls on the smaller 1024×576 panel while navigation stays available.
Diagnostics and client power controls remain accessible throughout setup.
The application explicitly requests GTK's dark variant and styles popup menus;
it does not depend on a desktop user's preexisting theme preferences. The image
includes PNG symbolic icons, the base icon theme and MIME data for GTK controls.

The private `setup.json` file stores only a version, step, direct-connection
preference and completion flag. Interruptions resume at the saved step;
passwords and pairing codes are not stored there. Both local media-certificate
verification and the authenticated host's media-ready response are required
before setup is completed, with input handoff still required when enabled.
An in-progress setup remains incomplete if pairing alone succeeds or the media
connection fails. Existing paired profiles from the earlier diagnostic build
retain the normal tabbed interface and their automatic-connect preference.

## Runtime configuration

The appliance runs the shell as the `remoteos` user under Xorg. Grant that user
read access to `/dev/input/event*` through the appliance input group and access
to the `wpa_supplicant` control socket in `/run/wpa_supplicant`. Supplicant must
use `update_config=1` and a private configuration file on the persistent data
partition. DHCP remains the boot network service's responsibility. Ethernet
uses that same system network service.

The client discovers the Wi-Fi interface through `/sys/class/net/*/wireless`.
Scanning and configuration use Unix datagram control messages directly, without
an interpolated shell command. SSIDs are hex encoded. WPA2 PSKs are derived with
PBKDF2 before being sent to supplicant; passwords are never placed in process
arguments or log messages. Open and WPA2 Personal networks are supported;
WEP, EAP enterprise and OWE are rejected by the selector.

Put the deployment's controlled connectivity-check URL into the single-line
file `/etc/remoteos/connectivity-url`. The endpoint must respond with HTTP 204.
The client bounds the curl check to 12 seconds and 1 KiB. Redirects and other
HTTP responses offer a network sign-in flow. A network's sign-in URL can also be
entered directly. HTTP and HTTPS are supported; embedded URL credentials,
control characters, file URLs and script URLs are rejected.

Tailscale CLI commands explicitly select `/run/tailscale/tailscaled.sock`.
`tailscale status --json` supplies the displayed enrollment state. Before
enrollment, the authenticated local supervisor reapplies the fixed
`--operator=remoteos` preference as root; it accepts no caller-selected user,
socket or command. The unprivileged client then runs
`tailscale up --operator=remoteos --json --timeout=5s`; its returned `AuthURL`
opens on this laptop. The user session sets `USER=remoteos` and
`LOGNAME=remoteos`, including when the launcher uses `su -p`. Startup also
grants operator permission. Enrollment identity belongs to tailscaled, not
the browser. No password or auth key passes through the supervisor.

Remote host profiles are saved under `/data/remoteos/client/profile.json` with
private permissions, and TLS client identity under its `identity` directory.
The host address, verified certificate fingerprint and automatic-connect
preference persist. `/data/remoteos/client/hosts.json` retains up to 32 verified
hosts, with name, address, control port and certificate fingerprint. Existing
single-host profiles are migrated. Discovery refreshes every ten seconds in
local setup, using LAN broadcast/multicast and online Tailscale peer queries.
Unavailable saved hosts remain selectable; changed identities are flagged
without replacing their pins. PINs never persist in either file.

## Input and local controls

Local setup and the power overlay use Xorg's input drivers. Touchpads use
`xf86-input-libinput`, including tap-to-click, while keyboards and ordinary
USB/PS2 mice retain `xf86-input-evdev`. The image removes libinput's broad
upstream catch-all file and installs a touchpad-only `InputClass` override.
This closes a gap in the original evdev-only image: evdev can expose a
multitouch pad as XI2 dependent touch without converting its contacts into
ordinary pointer motion. The exact HP touchpad capabilities and its behavior
after this change still need the physical input inventory and a boot test.
`xinput --list --long` is included for device/class diagnostics; it does not
record keystrokes or a live input-event stream. Image creation checks both
input modules and their transitive shared-library dependencies for unresolved
symbols before packaging.

The evdev broker discovers devices at startup and every two seconds. Only
active remote sessions exclusively grab keyboard devices. Pointer devices stay
with Xorg/libinput so touchpad device units are processed once, with normal
acceleration, tap/click and scrolling. GTK root coordinates are mapped through
each screen's actual video viewport, including VGA letterboxing and encoded
downscaling. There is no fixed raw-touchpad multiplier. The
power key is always filtered from the host stream; a short press opens the
local client controls. Opening the controls, losing an input device, entering
setup, closing a session or encountering `SYN_DROPPED` releases host keys and
buttons. The power-button event is never sent to the host.

Remote-session keyboard and processed pointer events use the shared control
protocol. The root cursor is hidden through XFixes while forwarding, including
Intel's installed software cursor, and is restored for the power overlay,
setup, browser use and disconnection. Opening/resuming the overlay releases
held host keys/buttons. Touchpad behavior still needs a physical HP retest.
Firmware-intercepted keys and the hardware long-hold shutdown cannot be
redirected by this software.

The controls show CPU usage from `/proc/stat`, memory usage from
`/proc/meminfo`, battery status from sysfs, Wi-Fi signal, the connected host,
control round-trip latency, and the matched Tailscale peer route (direct, peer
relay, or DERP relay). Media status includes negotiated dimensions, measured
per-stream frame rate, RTP loss and actual queue occupancy. Idle or unknown
routes are identified explicitly. Restart and shutdown use fixed
JSON actions on `/run/remoteos/supervisor.sock`; the shell never invokes a root
shell or calls a passwordless arbitrary command.

## Browser isolation and authentication qualification

The sign-in browser is a separate process group. The client stops its session
before starting the browser, and terminates the browser process group before
returning to streaming. WebKit subprocess sandboxing is explicitly enabled;
the dangerous sandbox-disable environment variable is removed. Browser state
is ephemeral. The browser does not expose a JavaScript/native bridge, inject
scripts into sign-in pages, inspect credentials or spoof a user agent.

The browser supports an address bar, Back, Reload, regular HTTP(S) navigation,
popups and visible load failures. TLS failures cannot be bypassed. Downloads
and website access to devices are denied. Software rendering is selected for
the GMA 950 qualification image. A WebKit subprocess crash leaves a reloadable
browser window with an explanatory message.

Google authentication in this browser remains a physical-device acceptance
gate. A compiled browser, rendered page or working Tailscale CLI does not prove
Google accepts this user agent or that the laptop has enough memory to finish
MFA. The product must not claim that gate passed without the recorded test.

## Display qualification and known integration limits

The primary Xorg monitor supplies the internal display dimensions. One second
monitor is negotiated up to 1920×1080. Each has its own native window for media
presentation, while the host receives separate logical display identities.
The appliance Xorg layout should make the laptop panel primary.

Display hotplug suspends input and media, preserves the internal display
window, and renegotiates the monitor list over the authenticated control
session. Preserving the internal host monitor and video stream while adding or
removing only VGA is a v1 requirement that remains unimplemented. Connection
loss also tears down the host displays immediately; stable EDID values alone do
not preserve monitor presence or window positions during a short reconnect.
The host display transition and EVDI/IddCx behavior need hardware qualification
before release. The sidebar-free full-screen remote state and
opaque local controls must be checked with the Intel XVideo implementation on
the actual HP; the desktop preview cannot validate video-plane clipping.

Host setup selects either the default MPEG-2 Wi-Fi profile (30 fps initially,
60 fps available for testing), or
the wired raw-I420 profile (up to 60 fps on the built-in display). The external
display retains its independent MPEG-2 stream. At 1366×768 and 60 fps, raw
I420 requires 755.34 Mb/s before transport overhead, so it needs a qualified
Gigabit path. This is an explicit profile choice; selecting it does not prove
that the Atom, link or encrypted transport can sustain its target rate.

The host converts captured BGRA to final-size I420, packetizes it with
GStreamer's [RFC4175 raw-video RTP elements](https://gstreamer.freedesktop.org/documentation/rtp/rtpvrawpay.html)
and uses the negotiated plain UDP or authenticated DTLS-SRTP transport. Plain
media is intended for the trusted LAN/Tailscale deployment; control stays TLS
and Tailscale encryption remains. The receiver reassembles
raw YUV and presents it through XVideo without a software decoder, color
converter or scaler. Host capture staging buffers are reused; raw receive
buffers request a bounded 4 MiB socket budget. Reported RTP loss causes
incomplete raw frames to be discarded. Network decryption, packet reassembly,
memory copies and XVideo presentation still consume client resources.

Profiles adapt automatically after selection; custom numerical overrides are
not exposed. Volume uses ALSA Master through amixer; the control must exist on
the qualified sound card. A displayed target frame rate is distinct from the
measured frame rate.

### FFmpeg acceleration assessment

The current image already uses FFmpeg 6.1.5 through gst-libav 1.24.13:
`avenc_mpeg2video` encodes on the host and `avdec_mpeg2video` decodes on the
client. GStreamer retains RTP, optional DTLS-SRTP, timing and XVideo presentation.
The standalone `ffmpeg` command is omitted from the appliance; its codec
libraries are present. Replacing the pipeline with a subprocess does not
by itself add acceleration. See the [GStreamer decoder documentation](https://gstreamer.freedesktop.org/documentation/libav/avdec_mpeg2video.html).

The resolved target build enables x86 assembly, runtime CPU detection and
Atom-compatible MMX/SSE/SSE2/SSE3/SSSE3 code, with SSE4 and AVX disabled.
This is optimized CPU decoding. The build has no VA-API, QSV, VDPAU or other
hardware codec backend enabled. XVideo YUV presentation remains separate
from compressed-video decoding and needs qualification on the HP.

The 915/945 generation historically exposed MPEG-2 motion compensation through
[Intel XvMC](https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man4/intel.4.html),
but [FFmpeg removed XvMC in 5.1](https://www.ffmpeg.org/index.html).
The image's Intel Xorg package also disables XvMC. Testing that legacy path
would require a separate compatible decoder/driver integration and measured
single- and dual-display behavior; it is not a switch available in the current
receiver. Do not describe ordinary FFmpeg software decoding or XVideo scaling
as full hardware MPEG-2 decoding.

Further optimization should be measured after graphical startup works:

- Compare one and two FFmpeg decoder workers per stream on the physical Atom,
  including both displays, Opus and input. The current limit is two per stream;
  more workers are not assumed faster on one core with Hyper-Threading.
- Preserve the raw-I420 path's decoder-free client. Keep scaling and color
  conversion on the host, and evaluate combined conversion/scaling and host
  GPU processing before CPU readback. FFmpeg/libswscale is one benchmark
  candidate; the existing GStreamer conversion is not presumed slower.
- Probe host-specific MPEG-2 encoding support before selecting a hardware
  encoder. FFmpeg QSV/VA-API options require compatible hardware and drivers;
  support for a different codec does not establish MPEG-2 support. Keep the
  software encoder as the fallback and retain the authenticated transport.
- Record achieved fps, CPU use, dropped frames and end-to-end latency against
  the existing pipeline. Raw 1366×768 at 60 fps still requires 755.34 Mb/s
  before overhead, regardless of the host conversion implementation.

This assessment changes no runtime codec or image. FFmpeg tuning does not
address the photographed pre-streaming graphical-session failure; the saved
failure report remains necessary for that diagnosis.


## USB device controls

The local menu opens a USB device list from the privileged supervisor. The
list includes eligibility and a reason when forwarding is blocked. An
administrator must qualify models/serials in `/etc/remoteos/usb-policy.json`;
the graphical client cannot approve an otherwise blocked model or override
boot-disk, mounted-volume, HID, hub, or network-adapter exclusions.

Camera mode enforcement remains unimplemented: approving a USB ID does not
select MJPEG, restrict UVC modes or reject an over-budget stream. Fixed tunnel
rate limits are not a replacement for that qualification. The default approval
list is empty.

Forwarding requires an explicit user selection during an authenticated host
session. The host first issues a per-device, session-bound tunnel grant. The
client supervisor rechecks eligibility and binds that device, using only the
loopback USB/IP exporter. The reverse tunnel verifies the pinned host TLS
certificate before reporting readiness; only then does the host importer
attach. No raw USB/IP service is exposed through the public client interface.

For a normal detach, the host must eject/flush the device successfully before
the client stops its tunnel and restores its local driver. A mounted-volume
failure keeps the device attached and reports the reason. A failed import
cleans up its local export. Disconnect and browser transitions stop all device
relays; those abrupt transitions have physical-unplug semantics.

The client refreshes its supervisor export lease every five seconds. The
supervisor can recover exports after client crashes or expired leases. Input
is released locally when opening the menu; opening the USB list does not
interrupt already attached devices.

Input access is checked explicitly. The client stops the remote session if it
cannot read or exclusively grab an input device, including a hotplugged device,
rather than allowing that device's events to go silently to the local desktop.
Discovery includes standalone numeric keypads, media-key devices and WMI
keyboard events such as brightness keys. KEY_POWER stays local. Lid-switch
events and firmware-only Fn actions are not keyboard events. Linux hosts accept
the documented evdev keyboard ranges. Windows accepts its implemented evdev to
scan-code mapping; an unmapped key reports an explicit host error and ends the
session, so international, brightness and vendor keys require further
mapping/qualification before claiming complete keyboard support. The Windows
mapping includes F13–F24 and common media, browser and application-launch keys.

## Development verification performed

The native GTK and WebKit targets compile against the declared development
packages. Parser tests cover SSID escaping, supported Wi-Fi security modes,
and sign-in URL policy. Xvfb smoke tests render the network, host and controls
screens and exercise tab/menu navigation with input forwarding disabled.

The standalone browser was launched against a loopback HTTP test page. This
execution environment forbids the unprivileged user namespaces needed by
WebKit's bubblewrap sandbox, so page rendering is blocked here. Sandbox policy
was kept enabled. The appliance must create the configured user's private
writable `XDG_RUNTIME_DIR` and enable the required user namespace kernel
features. This limitation is separate from the unperformed Google enrollment
and real-HP memory/performance qualification.


The Diagnostics tab presents `/run/remoteos/hardware.txt` in a read-only,
scrollable inventory view. Refresh rereads the existing report without running
commands. Save writes a private copy to
`/data/remoteos/client/reports/hardware.txt` (or the selected state directory).
If `/run/remoteos/ephemeral-state` exists, the shell and local controls show that
settings and saved reports are temporary and will be lost after power off.

## Recording performance

The Diagnostics screen and the power overlay can start a thirty-minute
performance recording without a terminal. Resume the remote desktop after
starting the recording; the sampler continues in the background, with one
recording active at a time. The status panel shows its output file under the
private client state `reports/` directory. Each CSV joins hardware counters with
fresh application frame rate, negotiated profile, RTT, loss and queue metrics.
It does not record desktop pixels, keyboard input, credentials or host addresses.
Powering off interrupts recording; completed rows remain available. Copy the
file before powering off when the appliance reports temporary storage.
