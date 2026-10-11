# HP Mini 2140 qualification record

**Status: UNQUALIFIED.** The repository contains an appliance build definition and diagnostics. A successful native build or a QEMU boot does not establish laptop performance, webcam compatibility, Google enrollment, Windows secure-desktop operation, or EVDI integration. Record each result against the exact image SHA-256, host version and hardware IDs.

### Enrollment permissions and startup network UX — 0.1.6 diagnostic

The user reports `Access denied: checkprefs access denied` during Tailscale
enrollment. Release 0.1.6 grants the fixed `remoteos` operator on the explicit
daemon socket at boot and immediately before enrollment through the restricted
supervisor. The unprivileged session now sets both `USER` and `LOGNAME` to
`remoteos`; `su -p` otherwise preserves the root launcher's environment. The
previous grant was ineffective at enrollment; the preserved environment is a
separate defect, not a proven complete explanation for the reported denial.

The final-image offline QEMU test reproduces the exact denial using UID 600,
invokes the real supervisor from that UID and proves preference writes then
succeed. Enrollment subsequently times out with no access denial because the
VM deliberately has no network. This establishes the permission repair path,
not Wi-Fi or Google/Workspace enrollment on the HP.

The final i386 setup client was rendered at 1024×576 on a private Xvfb. Its
Wi-Fi indicator and opaque Carbon connection prompt fit the screen. Startup
waits for network association/carrier plus an address, offering setup or offline
use after 20 seconds without connectivity. The modal cancels pending remote
connections so their callbacks cannot take input behind it. Internet status is
measured separately; direct LAN use can continue offline. Actual captive-portal
and radio behavior still need the HP.

The application host is a separate edition for an already logged-in desktop.
Linux capture/input, both packaged raw60 and MPEG-2 streams with host scaling,
and window start/stop/restart/close checks pass on an authorized private test
display. Its runtime has no EVDI dependency or kernel module. That bundle was
built on Ubuntu 26.04; Ubuntu 24.04 builds/run and host playback need physical
validation. The Windows launcher/backend/manifest compile; a full SDK-linked
Windows package and runtime remain unqualified. Application hosting shares
existing monitors and does not provide raw USB importing or login/UAC access.

The 0.1.5 cursor fix below is retained. Its final i386 helper/client still draw
228 cursor pixels at two positions on Xvfb; physical Intel rendering and
sustained 768p60 streaming remain pending.

### Invisible cursor confirmed — 0.1.5 diagnostic

The latest physical report identifies `0.1.4-diagnostic-cursor` and records
`Software cursor enabled (SWCursor)`. The Synaptics touchpad is an XInput
pointer using libinput, and the user confirms movement/clicks work while every
cursor shape remains invisible. The fallback was selected but did not resolve
visibility. This rules out an ignored `SWCursor` configuration as the remaining
problem; it does not establish the precise i915/SNA rendering failure.

The report also identifies the internal 1024×576 display and Intel textured
XVideo adaptor with 16 ports. This confirms availability, not concurrent stream
performance. `wlan0 wifi ready` means the control service is ready, not that
association, DHCP or Internet access has been qualified.

The minimal X session had no desktop/window manager to initialize a themed
default pointer. The actual previous client inherited a 16×16 core font cursor
on the welcome screen. Release 0.1.5 explicitly loads Adwaita's 24-pixel
`left_ptr` on the root window and exports the theme/size to the client and
browser. This changes the software sprite's source: on an ARGB-capable X
server, Xorg uses the Render composite path rather than monochrome
`PushPixels`. A font-cursor drawing defect remains a hypothesis, not a proven
cause on this HP. SNA, KMS, XVideo and the software cursor selection remain
enabled.

The new `remoteos-cursor-info` diagnostic reports current cursor name, size,
position, hotspot, visible-pixel count and light/dark contrast. It does not
record input events. The hardware report includes it under `X11 cursor image`.
The isolated framebuffer probe compares actual Xvfb pixels before and after
hiding the cursor; metadata alone is insufficient evidence of presentation.
Physical HP visibility, entry-field cursors, overlay and VGA hotplug still
require a boot test against the 0.1.5 image hash.

### Attempted software-cursor fallback — 0.1.4

The user now reports working pointer input with an invisible cursor. An audit
found no pointer-hiding calls or `-nocursor` option, and the packaged cursor
images contain visible pixels. The actual i386 client submits a nontransparent
cursor on an isolated Xvfb display. These observations prompted a hardware
cursor presentation hypothesis, which the subsequent physical 0.1.4 test did
not confirm: the selected software fallback was also invisible.

Release 0.1.4 adds a narrowly scoped downstream `SWCursor` option to the pinned
Intel SNA driver. The stock SNA driver does not implement this option, so an
Xorg configuration line alone would have no effect. The patch keeps Xorg's
software sprite initialization and skips only registration of the hardware
cursor wrapper. The HP configuration opts in, while SNA rendering, KMS and
textured XVideo remain enabled. The build guard rejects a stale Intel module
that lacks the new capability. Initialization, reload and cleanup paths have
source-level regression coverage, but physical cursor visibility, VGA hotplug
and presentation performance remain laptop tests.

The Xorg report should contain `Software cursor enabled (SWCursor)`. Cursor
messages are now retained in the hardware inventory. If visibility remains
broken, collect the refreshed `reports/latest-boot-hardware.txt`; it can show
whether the patched driver actually selected the fallback. The libinput and
Wi-Fi startup repairs from 0.1.3 remain included. Pointer input working is not
evidence that Wi-Fi association or sustained streaming has been qualified.

### Follow-up setup report — input and Wi-Fi repair

After the graphics repair, the user reports that keyboard navigation works in
setup, while pointer movement, hover and clicks do not. Wi-Fi setup reports
`Cannot access Wi-Fi control socket: No such file or directory`.

The shipped Wi-Fi daemon reproduces a definite build/startup mismatch: the
worker requests `-s`, but `CONFIG_DEBUG_SYSLOG` was disabled. The old binary
prints usage and exits before creating the socket, even returning exit code
zero. Release 0.1.3 explicitly enables the feature and checks the installed
binary for both syslog and Unix control support before image creation.
`scripts/probe-wifi-control.py` runs the actual i386 daemon with a private
global Unix socket and verifies a `PING`/`PONG` exchange without using a network
interface. This isolates the startup/control repair; radio association and
DHCP still require the HP. BusyBox `logread` exposes the existing bounded RAM
syslog ring. The worker saves one private, filtered, bounded
`/state/reports/latest-wifi-failure.txt` per service-failure outage.

For touchpads, the evdev-only image has a concrete coverage gap: multitouch
touchpads produce dependent-touch events without normal pointer emulation.
Release 0.1.3 uses libinput specifically for touchpads, with tap-to-click,
while retaining evdev for keyboards and ordinary mice. The supplied earlier
inventory did not record this HP's touchpad capabilities, so this is not yet
proof of its physical failure's cause. No speculative Intel cursor option or
kernel PS/2 workaround is applied. The new hardware report includes input
devices, udev classification, kernel input initialization, Xorg input driver
selection and `xinput --list --long`, without recording input events. If pointer
input remains absent, collect the refreshed `latest-boot-hardware.txt`; if the
Wi-Fi service still exits, also collect `latest-wifi-failure.txt` when present.

### Physical boot observation — 2026-10-08

The supplied [hardware inventory](latest-boot-hardware.txt) identifies release
`0.1.1-diagnostic-setup`, an Atom N270, approximately 2 GB RAM, Intel 945GSE
graphics using i915, and a connected **1024×576** LVDS panel. VGA was disconnected.
This is evidence of Linux userspace boot on the laptop; graphical setup and
streaming remain unqualified. The exact flashed image hash was not supplied.

The [graphical failure report](latest-graphical-failure.txt) identifies the
blocking error: Xorg cannot load `intel_drv.so` because `vbeFree` is undefined,
then reports no drivers and no screens. The kernel DRM device exists; this is
not evidence of a missing i915 kernel driver. The built Intel module included
legacy i810/i815 userspace modesetting code that refers to separately loaded
VBE/VGA helpers. Full RELRO enables immediate binding, so those references
prevent the entire module from loading before the helpers can be requested.
Build the Intel driver with its supported `--enable-kms-only` option for this
i915/KMS appliance, preserving SNA/UXA and full RELRO. Check plugin symbol
resolution as well as ordinary ELF shared-library dependencies.

The ACPI `AE_AML_BUFFER_LIMIT` message and missing ALSA UCM configuration in the
boot photograph precede this failure. Startup continued after them; ALSA
reported initialization with generic defaults. They are not the reported
Intel driver load error.

Source inspection also found that inherited `umask 077` created `/run/user`
root-only while only `/run/user/600` received client permissions. The startup
script now explicitly sets the parent to root:root mode 0755 and the private
child to 600:600 mode 0700, with a restrictive-umask traversal regression.
This is a separate confirmed defect included in the 0.1.2 boot repair.

The inventory confirms BCM4322 `14e4:432b`, N-PHY revision 4, firmware 666.2
loading, and unblocked Wi-Fi radio state. Its startup snapshot still says
`waiting-for-supplicant`; association, DHCP and throughput are unverified.
The Chicony CNF8031 webcam enumerates **YUYV only**, at resolutions from
160×120 through 640×480 and rates 5/10/15/20/22 fps. No MJPEG mode is listed.
The separate current-format query's 25 fps conflicts with those enumerated
intervals; select an enumerated mode and measure capture before qualification.
VGA webcam at 22 fps requires 108.1 Mb/s of pixel payload before USB/IP/TLS
overhead. A 320×240 at 15 fps candidate requires 18.4 Mb/s and is not yet a
qualified combined Wi-Fi/media profile.

Factory Wi-Fi options, verified driver/firmware packaging, support limits and the terminal-free startup sequence are documented in [HP 2140 wireless and first boot](hp2140-wireless.md). A Broadcom 4322AGN product label does not establish 802.11n throughput under b43.

The HP family has both 1024×576 and 1366×768 panels. Read the connector modes rather than treating the proposed resolution as detected hardware. MPEG-2 decoding is software decoding with XVideo presentation; this release does not claim full GMA 950 hardware decode.

The additional raw I420 profile moves color conversion to the host and removes MPEG-2 decoding on the client. It still requires packet reception, frame assembly and presentation. A 1366×768 stream at 60 fps carries about 755 Mb/s of image samples before RTP/UDP/IP, encryption and Ethernet overhead. Qualify this separately on wired Gigabit Ethernet; Wi-Fi performance and dual-monitor operation cannot be inferred from codec removal. Record the actual selected mode and sustained delivered frames.

## Build and inspect

On a Linux build machine with GCC/G++, GNU make, binutils, patch, Perl, Python 3, CMake, curl, rsync, cpio, bison, flex, unzip, file, bc, and development headers:

```sh
./scripts/build-image.sh check-config
./scripts/build-image.sh build
./scripts/build-image.sh legal-info
```

The default HTTP 204 connectivity probe is `http://connectivitycheck.gstatic.com/generate_204`; replace `/etc/remoteos/connectivity-url` in the overlay with an operator-controlled endpoint before a fleet release.

The first command downloads Buildroot 2025.02.18, verifies its pinned SHA-256, applies the checked-in WebKitGTK 2.54.1, Go 1.27.2 and Tailscale 1.104.1 backports, and rejects any explicit Kconfig selection lost to an unmet dependency. Outputs live in `output/hp2140`; temporary files and compiler caches also stay under that directory so restricted host paths do not break bootstrap. The wrapper locks the output directory to prevent overlapping builds. `REMOTEOS_JOBS` defaults to 4; WebKitGTK and the internal C++ toolchain require significant host memory and build time. `REMOTEOS_CACHE_DIR` and `REMOTEOS_OUTPUT_DIR` override storage locations. Run `configure` explicitly after changing the defconfig; `build` preserves an existing configuration.

After a complete build, inspect `output/hp2140/images/remoteos-hp2140.img` and `remoteos-buildroot.config`. The image uses BIOS GRUB, a 1.5 GiB read-only SquashFS partition, and a 512 MiB ext4 state partition. Its complete disk layout is 2,049 MiB, so use a USB drive of at least 4 GB; the compressed root filesystem payload is much smaller. The state filesystem is pre-created in the image; the boot path never formats an attached disk. Startup derives the state partition from the disk containing the actually mounted root partition and checks its label; it never searches other disks by a shared state label. If it cannot mount that state partition, startup uses temporary RAM state and marks `/run/remoteos/ephemeral-state`.

Images intentionally have a fixed MBR signature/PARTUUID for reproducible builds. Two RemoteOS clones attached at boot can make the kernel's root PARTUUID selection ambiguous. Connect only one RemoteOS boot disk during qualification. Perform installation/recovery from generic Linux media rather than attaching a second booted RemoteOS clone. The image boots into the diagnostic client; it does not contain an automatic installer UI.

`legal-info` collects third-party license manifests and redistributable sources. The RemoteOS local package deliberately skips generic automatic source archiving because the working repository contains build outputs and private provisioning caches; bundle reviewed project source separately after choosing its release/license policy. This setting does not assign a software license.

**Firmware distribution:** the diagnostic defconfig includes Buildroot's `b43-firmware` package for Broadcom adapters. Its upstream metadata marks it proprietary and non-redistributable. This private qualification image must not become a public release image until firmware redistribution rights or user-supplied installation are resolved. `linux-firmware`'s `brcm` files alone do not supply b43 firmware. Record the PCI device/driver actually used; unsupported adapters remain a feasibility failure.

`scripts/check-target-media.sh` runs the built i386 GStreamer binaries with only the image’s libraries and plugins on a Linux builder that supports 32-bit ELF execution. It checks all required factories and ten generated 1366×768 frames through MPEG-2 and raw-I420 RTP round trips into discard sinks. Evidence is saved under `output/hp2140/validation/target-media`; these checks establish library compatibility, not laptop throughput or XVideo presentation.

A local emulation smoke test, after image construction, is:

```sh
qemu-system-i386 -m 2048 -drive file=output/hp2140/images/remoteos-hp2140.img,format=raw,if=ide,snapshot=on -nic user,model=e1000
```

For reproducible BIOS boot evidence, `scripts/qemu-smoke.py IMAGE --output NEW_EVIDENCE_DIRECTORY --require-intel-ddx` boots with snapshot-only writes, an N270 CPU model, 2 GiB RAM and no network; it selects the serial diagnostic GRUB entry and records screenshots, a serial log and a hash-bound receipt. It requires the expected kernel, read-only SquashFS root, init milestones and successful Intel module load, rejecting undefined-symbol or Intel module-load failures. Menu timing can be adjusted with `--menu-delay`.

QEMU can validate Intel module loading and dynamic linking, but its emulated VGA cannot validate Intel hardware initialization or presentation. It can therefore report “no screens found” after the module loads successfully. This is distinct from the HP's presentation test. Use the verbose boot entry for diagnostics, or the serial diagnostic entry with `-serial stdio` to record kernel and init output at 115200 baud. A BIOS smoke test must boot the complete disk image through GRUB; direct-kernel tests do not establish bootloader correctness.

## Signed offline installation and recovery

`sign-image.sh` creates a detached SHA-256/RSA or ECDSA signature using an externally supplied private key. No release key ships in source. Obtain the public key from a trusted channel independently of the downloaded image. Example commands with placeholder paths:

```sh
./scripts/sign-image.sh output/hp2140/images/remoteos-hp2140.img /secure/release-key.pem /exports/remoteos-hp2140.img.sig
./scripts/verify-image.sh output/hp2140/images/remoteos-hp2140.img /exports/remoteos-hp2140.img.sig /trusted/release-public.pem
```

`scripts/install-image.py` is an **offline whole-disk** installer. It requires the signed image, trusted key, an explicit whole-disk target, and `--confirm-erase` repeating that canonical target path. It refuses mounted disks, partitions, device-mapper targets, and files, stages and verifies a private snapshot, and opens the target exclusively before writing. No install is triggered by boot, build, network input, or these documents. Run it only on another machine or recovery environment after backing up the target. It replaces all prior partitions, including saved Wi-Fi/Tailscale/pairing state.

`scripts/update-image.py` uses the same signed-image verification model with `--confirm-update`. It requires the existing RemoteOS disk's root/state partition entries to match the new image exactly and validates the state filesystem label before replacing only the boot/root prefix. The existing state partition is preserved. Layout changes require an explicit migration. This is an offline update performed from another OS or recovery environment; automatic A/B updating and rollback are not implemented, and interruption during a write requires recovery media.

## Collect the laptop evidence

On the laptop, the boot service atomically publishes `/run/remoteos/hardware.txt` and, when persistent state is available, one latest snapshot at `/state/reports/latest-boot-hardware.txt`. Each snapshot is capped at 512 KiB; graphical startup refreshes it with display details. If Xorg cannot start on the HP, the graphical launcher retries three consecutive early exits and prints a recovery message plus the last 40 captured lines, capped at 8 KiB. With the exact kernel command-line token `remoteos.diagnostic=1`, the console instead receives the complete captured prefix, capped at 64 KiB. Boot generic Linux recovery media and retrieve the state partition’s `reports/latest-boot-hardware.txt` and, if present, `reports/latest-graphical-failure.txt` files. The latter is private and limited to a status line plus a 64 KiB log prefix; review it before sharing. Do not copy the entire state partition: it also holds Wi-Fi, Tailscale and paired-host credentials. Run `remoteos-hardware-report` again from the graphical diagnostic environment with `DISPLAY=:0` and the authorized X cookie to include `xrandr` and `xvinfo`. Reports contain hardware IDs and local topology but omit credentials and browser data.

An unprivileged connector watcher enables VGA on plug and removes its CRTC on unplug. It selects an advertised progressive mode up to 1920×1080 and retains the internal panel's native mode. The local Xorg framebuffer stacks VGA below the panel, keeping the two target outputs within 1920×1848. This avoids an unnecessarily wide framebuffer on the old Intel presentation path; the host still arranges its logical monitors side by side and the client places each video window using the actual local monitor geometry. Validate both XVideo ports on the laptop, including overlay behavior after hotplug.

Record the following before testing a host:

| Item | Required evidence |
|---|---|
| CPU/RAM | Atom N270-compatible 32-bit instruction set, 2 GB physical RAM |
| LCD/VGA | Connector modes, built-in native resolution, external 1920×1080 EDID timing |
| XVideo | Intel textured adaptor and at least two usable ports; two windows presented concurrently |
| Wi-Fi | PCI ID, driver, PHY/core revision, firmware files loaded, hardware/software rfkill state, open and WPA2-Personal association, DHCP, DNS; start with radio off and confirm switch-on recovery without reboot |
| Camera | USB ID and every `v4l2-ctl --list-formats-ext` mode; prefer native MJPEG |
| Sound | ALSA card/device, speaker test, remote Opus playback |
| Persistence | Network profile and machine/Tailscale/host identities survive reboot |
| Isolation | Root remains read-only; shell/browser UID is 600; browser sandbox remains enabled |

Start the thirty-minute combined test with **Record 30 min performance** on the client’s Diagnostics tab, or **Record 30 min** in the power overlay during a session. Resume the session while the sampler runs in the background; the client allows one recording at a time and saves it privately under `/data/remoteos/client/reports/`. A technician with a diagnostic shell can instead run `remoteos-sample 1800 /state/home/qualification.csv`. It records actual CPU utilization, available memory, aggregate byte counters from interfaces backed by physical devices (Wi-Fi/Ethernet/USB NICs), and maximum thermal sensor temperature once per second, then appends the client’s codec, resolution, target/achieved fps, control RTT, RTP loss, media queue delay and exported-device count. Virtual interfaces such as `tailscale0` are excluded from the hardware network totals to avoid counting encapsulated traffic twice. Only complete, correctly formatted application records from the past five seconds are accepted; missing or stale records have `metrics_fresh=0` and blank application fields. Record the Tailscale route type separately. These application measurements and hardware counters are observations, not a claim that the target profile is qualified. Copy evidence to another machine before recovery/reinstallation.

## Blocking acceptance matrix

| Gate | Procedure and acceptance |
|---|---|
| Browser | Captive portal on local display; Google/Workspace Tailscale enrollment including MFA; normal TLS validation; no user-agent spoofing; browser processes terminate before streaming |
| Two displays | Built-in native logical mode plus 1920×1080 VGA logical mode, independent MPEG-2 streams; measure sustainable fps and decode CPU; describe any adaptation, never claim 1080p30 from VGA capability alone |
| Combined load | Thirty minutes with both streams, sound and a qualified webcam mode; then storage transfer; bounded latency and no stuck input or sustained audio interruption |
| Input ownership | Hold Shift/Ctrl/mouse button, short-press power, operate overlay, resume; host receives releases and clean input; hardware long-press cutoff is distinct |
| Recovery | Wi-Fi loss, DNS failure, Tailscale revocation, changed host identity, host restart, VGA unplug/replug; no unauthenticated fallback; visible reason and recoverable state |
| Camera bandwidth | Use actual USB throughput. Raw 640×480 YUYV at 30 fps is 147.456 Mb/s before overhead and is not a Wi-Fi baseline. Reject over-budget settings |
| Windows | Cold boot to login, login/logout, lock/unlock, UAC secure desktop, SAS and reconnect while locked with signed dependencies and ordinary security enabled |
| Linux | Qualified Ubuntu/Xorg/LightDM/Xfce versions; cold boot greeter, PAM login, lock, polkit, X restart and EVDI module loading under the chosen Secure Boot policy |
| Storage | Explicit host ownership; never client boot disk/network/HID/hubs; flush/eject before normal detach; abrupt failure documented as unplug semantics |

A failed gate remains **FAIL/BLOCKED**, with logs and a concrete next test. Report the highest stable measured profile, rather than promoting a requested target to a supported profile.

## Source baselines

- Buildroot 2025.02.18 archive SHA-256: `e38ad1df6ea0479fff6419a87a64535d02131674d463be154a763ef725b55321` from the [official release](https://buildroot.org/downloads/).
- Linux 6.12.109 and package hashes come from that pinned Buildroot release; GCC 14 and dynamic glibc target the 32-bit Atom.
- Tailscale 1.104.1 uses Go 1.27.2 (its upstream module requires at least Go 1.27.1). The source/archive hashes and changed license hash are pinned in the bootstrap patches. Its vendored archive is generated with Buildroot's normal `go-post-process`, with dependencies verified against upstream `go.sum`. ACME, SSH, Serve/Funnel, web client, route/exit-node advertisement, app connectors, BIRD, Drive, Taildrop and TAP are omitted using upstream-supported feature tags, validated through its feature dependency tool. Device state and Tailnet Lock data use explicit persistent `--state` and `--statedir` paths. [Tailscale release notes](https://tailscale.com/changelog), [security bulletins](https://tailscale.com/security-bulletins), [Go release checksums](https://go.dev/dl/).
- [WebKitGTK 2.54.1 source sums](https://webkitgtk.org/releases/webkitgtk-2.54.1.tar.xz.sums): `ea0bbb02dbdbc596874a4e7ad35b66645b3e0a232bd0e4081de5ed92eb0a397d`. The backport enables Skia, adds its direct dependencies, disables unsupported 32-bit JIT/WebAssembly and removes an already-integrated 2.52 patch. Source patches restore missing disabled-libpas include guards for the i386 system-malloc build, use explicit ANGLE rounding when floating-point expressions have excess precision, and include an unconditional NowPlayingInfo field in its IPC serialization description. Type, size and ordering checks remain enabled. The rounding fix follows the [GCC 13 C++ porting guidance](https://gcc.gnu.org/gcc-13/porting_to.html). The full i386 cross-build succeeds with these patches. Real browser startup, memory use and Google sign-in on the HP remain blocking qualification tests.
- The USB/IP tools package patches `usbipd` to bind only `127.0.0.1`; IPv6-only invocation fails closed. The daemon is not started by init. The default `/etc/remoteos/usb-policy.json` has empty approved/protected-device lists; no device is approved until qualification and explicit image provisioning. The post-build hook verifies the installed daemon and source policy before creating the supervisor capability marker. Export policy and the authenticated relay must authorize a device before launch/bind.
- GStreamer 1.24.13 uses a package patch that enforces a DTLS 1.2 minimum and fails closed if the OpenSSL policy cannot be set. The upstream archive hash remains checked by Buildroot.
- The Intel Xorg package uses upstream `--enable-kms-only`, retaining SNA/UXA and full RELRO while excluding the unrelated i810/i815 user-mode-setting backend. The first real HP boot exposed that backend's unresolved `vbeFree` dependency during immediate driver binding. `scripts/check-xorg-driver-symbols.py` now checks every strong undefined Intel-driver symbol against Xorg and their transitive shared-library dependencies before image creation; optional modules that would load later cannot satisfy this check. An existing build must use `xdriver_xf86-video-intel-dirclean` followed by `xdriver_xf86-video-intel`; reconfigure alone can retain the old legacy convenience archive. This resolves the demonstrated module-linkage failure, but successful physical display startup still requires another HP boot.
- IBM Plex Sans 6.4.0 is pinned to upstream commit `383c681f015ed2626e919ec4e3cca16ccc204e9d` with per-file SHA-256 checks and its OFL license included.
- [Google OAuth policy](https://developers.google.com/identity/protocols/oauth2/policies) may reject an embedded or unsupported user agent; a sandboxed WebKitGTK process alone does not prove Google accepts the browser.
