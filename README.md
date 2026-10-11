Portable Windows and macOS hosts, plus the Linux client AppImage, are built by [GitHub Actions](https://github.com/BaiTian6641/RemoteOS/actions/workflows/portable.yml). Download the executable packages from the latest successful run.

Relative mouse and portable Windows/macOS build workflows are documented in [docs/portable-builds.md](docs/portable-builds.md). Use Client controls → Relative mouse (Blender / 3D) to capture movement; open the controls to release it.

The local 0.1.19 HP image is `output/releases/hp2140-relative-mouse-wl-0.1.19/images/remoteos-hp2140.img.gz`. Relative mouse capture is also available in the Linux client AppImage from GitHub Actions. The existing Linux application host 0.1.18 accepts this input protocol.

The 0.1.18 diagnostic power/devices release is documented in [docs/power-devices-0.1.18.md](docs/power-devices-0.1.18.md). Native package startup checks run in CI; physical Windows/macOS capture and permission qualification remain separate hardware tests.

# RemoteOS — HP Mini 2140 appliance prototype

RemoteOS is a locally booted, 32-bit Buildroot appliance that connects to a
remote desktop host. This repository implements the diagnostic system and
working client, protocol, media and host integration components. **It is not
a qualified RemoteOS v1 release.** Google sign-in on the actual laptop,
dual-display decoding, combined Wi-Fi/webcam performance and unattended host
transitions still require the physical acceptance tests.

The first deployment target is the hardware/browser diagnostic image described
in [the qualification guide](docs/hardware-qualification.md). Build output is
under `output/hp2140/images/`; a build definition or native test pass is not
evidence that an image booted on an HP 2140.

## Separate fullscreen viewer — client 0.1.17

Use the [HP USB image with wl and b43 recovery](output/releases/hp2140-fullscreen-wl-0.1.17/images/remoteos-hp2140.img.gz)
with the existing [Linux application host 0.1.16](output/releases/desktop-host-linux-0.1.16.tar.gz).
The viewer now owns a separate borderless window at native panel size, and the
entire setup window is unmapped during the session. The client explicitly
refreshes XVideo's cached drawing geometry after layout changes. Overlay resume
and reconnect retain full-panel bounds. This adds no per-frame processing.

See the [client update guide](docs/client-fullscreen-0.1.17.md) and
[validation record](output/releases/hp2140-fullscreen-wl-0.1.17/validation/validation-summary.json).
Real HP rendering confirmation remains required.

## Chrome in the dedicated desktop — host 0.1.16

Use the [HP USB image with wl and b43 recovery](output/releases/hp2140-desktop-wl-0.1.16/images/remoteos-hp2140.img.gz)
with the [Linux application host](output/releases/desktop-host-linux-0.1.16.tar.gz).
The viewer reapplies native panel geometry when a remote session becomes ready,
and hidden setup content no longer determines the video page's minimum size.
The local power overlay retains cursor and input ownership.

Chrome's loading hang was reproduced in a private KDE session: a KDE Wallet
dialog opened on the hidden backing display, so the browser waited for an
inaccessible prompt. The new launcher updates its private D-Bus activation
environment before desktop helpers start, placing their dialogs in the visible
dedicated desktop. A second KWallet 6.23 fault discarded a newly created wallet
object, then failed immediate lookup and left Chrome awaiting completion. The
host bundle includes a private bridge repair with source and licenses. Normal
Chrome sandboxing and encrypted wallet setup/unlock remain enabled.
Stop the old dedicated desktop and launch the new host package to
apply this change; reconnecting alone retains old helper processes.

See the [update guide](docs/desktop-session-0.1.16.md) and
[validation record](output/releases/hp2140-desktop-wl-0.1.16/validation/validation-summary.json).
The previous wl/b43 selection and MPEG-2/UDP performance profile are retained.

## Motion, receive scheduling and quality — 0.1.15

For the identified **BCM4322 (`14e4:432b`)**, an optional
[Broadcom `wl` diagnostic image](output/releases/hp2140-motion-wl-0.1.15/images/remoteos-hp2140.img.gz)
uses the same performance update and adds a boot-menu **b43 recovery** entry.
Its 32-bit Broadcom STA module is pinned to Debian's patched
`6.30.223.271-32` package and this image's Linux `6.12.109` kernel. The driver
is selected before device coldplug; it does not force-unload a bound driver.
See the [driver guide](buildroot/package/remoteos-broadcom-sta/README.md) and
[driver-image validation](output/releases/hp2140-motion-wl-0.1.15/validation/validation-summary.json).
The build and recovery checks do not establish actual HP wireless throughput.

Use both [the HP USB image](output/releases/hp2140-motion-udp-0.1.15/images/remoteos-hp2140.img.gz)
and [the Linux application host](output/releases/desktop-host-linux-0.1.15.tar.gz).
This diagnostic update preserves native display pixels, the independent KDE
session, selectable 60 fps ceiling, trusted-network UDP media and TLS control.
It addresses frame/late drops; the reported 20% was not measured packet loss.

- RTP reassembly now drains independently of MPEG-2 decoding. A bounded queue
  holds at most three complete compressed pictures, with reference-aware
  I-picture recovery after dropping old pictures. Video jitterbuffer packet
  eviction is disabled, and MPEG-2 requests a 512 KiB socket burst buffer.
  RTP packet allocation queries terminate before the jitterbuffer to avoid
  blocking reception while waiting for a reordered packet. A 150 ms video
  repair deadline accommodates paced picture fragments; ordered pictures
  start immediately and the sink does not wait on a playout clock. Audio keeps
  its separate 40 ms jitter budget.
- Automatic MPEG-2 profiles start at up to 12 Mbps and probe quality up to
  24 Mbps per display when delivery and decoding remain stable. Multi-display
  ceilings share 70% of the connection limit by pixel area. The 40 Mbps setting
  remains a ceiling including transport headroom, not a minimum traffic rate.
  Explicit/mixed raw profiles retain their requested budgets.
- Host MPEG-2 encoding uses rate-distortion block decisions. Trellis
  quantization is disabled because it caused expensive startup frames during
  live quality changes in the motion stress test. No B frames or
  additional decoder references are introduced. A 100 ms encoder VBV bounds
  picture bursts. A one-picture output queue overlaps encoding with paced
  transmission without dropping reference pictures. Packet pacing uses the
  reserved wire share independently of the average codec bitrate.
- Receiver decode/presentation pressure reduces frame rate without cutting
  the bitrate. Packet deadline loss reduces bitrate. Sustained stability and
  measured decode time govern recovery, avoiding repeated 30/50 fps probes.
- Modern peers tune only the video encoder at a complete-picture boundary.
  Transport, audio, held input and logical display identities stay active.
  Each new encoder starts with fresh sequence headers and an I picture.
- The power overlay separates packet loss, presentation drops and dependent
  picture recovery. Qualification recordings include decoder time, received
  Mbps, CPU topology/frequency, UDP overflows and wireless retry/rate snapshots.

The N270 has one physical core with two SMT logical CPUs; the overlay now
labels its per-CPU figure accurately. The supplied HP log identifies BCM4322
with `b43`. [Linux Wireless documents no 802.11n HT/aggregation support in that driver](https://wireless.docs.kernel.org/en/latest/en/users/drivers/b43.html),
so 5 GHz alone does not establish high WLAN throughput. The new recording
captures link rates and retries without recording Wi-Fi credentials.

MPEG-2 remains the default after comparative moving-image tests. H.264
fastdecode improved quality per bit but used more decoder CPU; whole-frame
JPEG showed large content-dependent bandwidth peaks. These builder
comparisons do not establish Atom performance. See the release's
[validation record and codec evidence](output/releases/hp2140-motion-udp-0.1.15/validation/validation-summary.json)
for measurements and limitations. Real HP full-screen motion tests remain
required; 60 fps is a requested ceiling rather than a guaranteed result.

Write the decompressed `.img` with your normal disk-image writer to a 4 GB or
larger flash drive. A whole-disk reflash replaces the state partition, so save
needed profiles and reports first. Run `Start-RemoteOS-Host.sh` from the new
host bundle and use the power overlay's recording action while reproducing
motion for at least 60 seconds. Send the CSV and its `.start.txt` / `.end.txt`
reports to distinguish decoder pressure, RTP lateness and actual socket or
wireless loss. The Linux bundle retains the recorded Ubuntu 26.04 ABI;
Ubuntu 24.04 requires a native host build.

## Full-screen presentation and matching KDE desktop — 0.1.14

Use [the HP USB image](output/releases/hp2140-fullscreen-plasma-0.1.14/images/remoteos-hp2140.img.gz)
with [the Linux application host](output/releases/desktop-host-linux-0.1.14.tar.gz).
The client disables the Intel textured-video scanline wait and clock-based
video QoS while retaining SNA/XVideo acceleration, native pixels, bounded
queues and audio timing. The host defaults to a selectable **60 fps target**,
MPEG-2, a **40 Mbps** limit and plain UDP media with TLS control/input.

**Independent HP desktop** now starts installed KDE Plasma 6 in a private
session matching the logged-in user's appearance and primary-screen panel
settings. Display/input, configuration, session bus and audio are separate;
the Unix account and home files remain shared. Its private presentation clock
stays active without requesting or encoding VNC framebuffer updates.
Openbox remains an explicit minimal option. Stop desktop or closing the host
app closes remote applications.

All **27 native regressions** passed. Packaged-host tests also verify an
animated native Wayland app, input across the resized window, two native
60/30 fps streams, private audio and detached-application cleanup. The image
passes BIOS boot, i386 media, cursor-rendering and disk-content checks. Builder
tests do not establish full-screen performance on the physical Atom/GMA 950.
The Linux bundle uses the recorded Ubuntu 26.04 ABI and requires installed KDE
components; build natively for Ubuntu 24.04.

See [installation, implementation and HP measurement instructions](docs/fullscreen-plasma-0.1.14.md)
and [the validation record](output/releases/hp2140-fullscreen-plasma-0.1.14/validation/validation-summary.json).
Decompress the image for a writer requiring `.img`; use a 4 GB or larger USB
drive and back up existing state before a whole-disk reflash. Immediate video
presentation can tear; 60 fps remains a target to qualify on the HP.

## Previous HP smooth UDP update — 0.1.12

Use both new packages: [Linux application host 0.1.12](output/releases/desktop-host-linux-0.1.12.tar.gz)
and [HP USB image 0.1.12](output/releases/hp2140-smooth-udp-0.1.12/images/remoteos-hp2140.img.gz).
The host defaults to **30 fps** with an optional **60 fps MPEG-2** limit,
retains native pixels and the **40 Mbps** budget, and offers trusted-network
plain UDP or encrypted UDP media. Pairing and control/input retain TLS.
Frame-preserving RTP removes receiver picture-boundary lookahead, jitter starts
at 10 ms, packet pacing compensates bounded scheduler lateness, and Wayland
input no longer waits for each portal acknowledgment. Adjacent waiting absolute
pointer positions are coalesced without crossing input-action boundaries.
See [settings, transport behavior and validation limits](docs/smooth-udp-0.1.12.md).
Builder tests do not establish smoothness or 60 fps on the physical HP.
Decompress `.img.gz` before using a writer that needs `.img`; a whole-disk
reflash replaces saved state.

## Previous HP Wi-Fi latency update — 0.1.11

The new host starts with a selectable **20 fps MPEG-2 limit**, keeps native
pixels, removes redundant pre-transmission parsing and ignores ambiguous
audio-inclusive queue feedback from older clients. The new HP image uses
slice decoding, processes media timing changes, limits waiting decoded frames,
and reports displayed FPS, late drops and the busiest CPU thread. Video
adaptation no longer treats ordinary audio buffering as video overload.
See [the update and settings](docs/low-latency-0.1.11.md). Physical HP
performance still needs measurement; 20 fps is the starting preset.

Downloads: [Linux application host 0.1.11](output/releases/desktop-host-linux-0.1.11.tar.gz)
and [HP USB image 0.1.11](output/releases/hp2140-low-latency-0.1.11/images/remoteos-hp2140.img.gz).
The image is gzip-compressed; decompress it before using an image writer that
requires a raw `.img`. Writing the whole image replaces the USB's saved state.

## Previous native HP virtual desktop — 0.1.10

On KDE Plasma Wayland, the host app now defaults to **HP native virtual display**.
Choose **1024×576** for the panel identified in this laptop's hardware report,
or **1366×768** for an HD-panel variant. KWin creates a separate desktop at
that exact size and 100% scale; captured and encoded pixels retain the same
dimensions. Keep **Match host resolution** enabled on the HP. Under load,
native mode reduces frame rate without shrinking text. MPEG-2 encoding and
the existing 40 Mbps budget remain on the host.

This KDE-specific userspace adapter owns only its virtual output, requires
the desktop's normal input/screen permission grant, and captures only the
verified RemoteOS stream. Stop sharing removes that output. No kernel module
or service is installed. It requires KWin OpenGL compositing, `kbuildsycoca6`
and a running PipeWire policy manager. A real isolated KWin test verifies both
panel sizes; the real KDE permission dialog and HP end-to-end performance
still need qualification. See [the update](docs/native-display-0.1.10.md) and
[the Linux bundle](output/releases/desktop-host-linux-0.1.10.tar.gz).
The HP 0.1.8 image below remains compatible; this update changes the host only.

## Built diagnostic artifact — 0.1.8 remote session

This update hides the local cursor during streaming, forwards processed Xorg
pointer coordinates instead of raw touchpad deltas, and adds automatic host
discovery, a saved host list and six-digit PIN pairing with host verification.
The host performs capture, resizing, color conversion, MPEG-2 encoding and
encrypted packet pacing within a shared 40 Mbps connection budget. Host
resolution matching is enabled; Xorg and Windows restore prior display settings
on disconnect. Wayland exact matching uses a consented virtual output when the
backend supports it, or an already matching physical output. Unsupported
matching reports an explicit error; the client can enable existing-monitor
sharing with host-side scaling instead. See [the session update](docs/remote-session-0.1.8.md).

The network indicator, automatic setup prompt, fixed Tailscale operator and
authenticated permission repair remain. Intel software-cursor configuration,
themed cursor initialization, Wi-Fi services and libinput touchpad handling
are retained.

| Artifact | Purpose |
|---|---|
| [Compressed image](output/releases/hp2140-remote-session-0.1.8/images/remoteos-hp2140.img.gz) | Approximately 179 MiB; decompress before writing |
| [Raw image](output/releases/hp2140-remote-session-0.1.8/images/remoteos-hp2140.img) | 2,049 MiB layout; use a USB drive at least 4 GB |
| [Raw checksum](output/releases/hp2140-remote-session-0.1.8/images/remoteos-hp2140.img.sha256) and [compressed checksum](output/releases/hp2140-remote-session-0.1.8/images/remoteos-hp2140.img.gz.sha256) | SHA-256 integrity checks |
| [Signature](output/releases/hp2140-remote-session-0.1.8/images/remoteos-hp2140.img.sig) and [public key](output/releases/hp2140-remote-session-0.1.8/images/diagnostic-public-key.pem) | Local diagnostic signature covering the raw image |
| [Build and qualification record](output/releases/hp2140-remote-session-0.1.8/images/diagnostic-build.json) | Exact hashes, evidence and remaining physical gates |
| [Linux application host with Wayland](output/releases/desktop-host-linux-0.1.8.tar.gz) | Open the included `Start-RemoteOS-Host.sh`; Wayland portals or Xorg; native Ubuntu 26.04 development bundle |
| [Application host guide](docs/application-host.md) | Build on Ubuntu 24.04 or create a Windows MSVC/SDK package |

Raw image SHA-256:

```text
0dcd33da0fc0810b12f6359c98105f1a4838a8b3184eb2f16e805c8527910db9
```

Decompress and flash the raw image to the whole USB drive, replacing its existing
partitions/settings; boot in legacy BIOS mode. Previous release bundles remain
available. The final image passed offline BIOS/GRUB boot and Intel module load,
exact packaged-file/ELF inspection, signature verification, and actual i386 setup
and cursor rendering on private Xvfb. Twenty-two native regression suites
passed, including a connected GTK session that checks rendered cursor pixels,
exact pointer endpoints, discovery/PIN pairing, saved-host persistence and input
release in the overlay. Encrypted-media saturation tests measure packet pacing
and audio continuity. The earlier 0.1.6 offline permission proof remains evidence
for the retained Tailscale repair; it is not a new on-device sign-in test.
**Google sign-in and physical HP presentation/performance remain unqualified.**

The application host runs while the user keeps its window open and chooses
Start sharing. It shares existing monitors and uses ordinary desktop input
APIs; no new display/input/USB driver or service installation is needed. The
Linux application supports Wayland through the RemoteDesktop/ScreenCast
portals and PipeWire, alongside Xorg/XTEST. Wayland needs the desktop's visible
permission grant for screen, keyboard and pointer access. Its private portal
fixture exercises two real PipeWire streams, HiDPI input, encrypted streaming,
reconnect, cancellation and revocation. The Xorg private-desktop capture and
window lifecycle checks remain separate. Real GNOME/Plasma runtime behavior
and sustained throughput still need qualification. Build locally for Ubuntu 24.04; the provided runtime was
built on Ubuntu 26.04. The Windows backend, launcher and manifest cross-compile;
full Windows SDK linking and runtime qualification are pending.

The latest HP inventory identifies a **1024×576 panel**, **BCM4322 Wi-Fi** and a
**Chicony CNF8031 webcam with YUYV only**. Wi-Fi control readiness does not prove
association or Internet access. See [wireless limitations](docs/hp2140-wireless.md),
[first-boot behavior](docs/client.md#first-boot-setup), and the
[qualification guide](docs/hardware-qualification.md).

## Implemented components

| Component | Implementation | Validation boundary |
|---|---|---|
| Appliance | Buildroot 2025.02.18 external tree, Atom i386/glibc, Linux 6.12.109, BIOS GRUB, SquashFS root and ext4 state | HP keyboard/pointer input and Wi-Fi control service observed; 0.1.6 enrollment permissions verified offline; physical cursor, Wi-Fi and Google sign-in await retest |
| Setup shell | Four-step first-run wizard, GTK3 Carbon token styling, resumable setup, Wi-Fi profiles, captive-portal flow, local Tailscale enrollment, pinned host pairing, automatic reconnect | Native backend tests; actual i386 client exercised at both panel sizes on Xvfb |
| Browser | Separate sandboxed WebKitGTK process, normal navigation/popups, certificate errors, ephemeral profile | WebKitGTK 2.54.1 and browser fully cross-built for i386; Google authentication is unqualified |
| Control | C++17, Asio, OpenSSL TLS 1.3, one-use pairing code, stored certificate pins, bounded framing/queues, release-all input | Real socket tests cover pairing, reconnect, unknown/changed identity and malformed framing |
| Media | MPEG-2 with selectable 15/20/30/60 fps limits, 40 Mbps budget, host encoding/pacing, Opus, frame-preserving RTP, plain or DTLS-SRTP UDP | Isolated native 30/60, single-picture, pacing/audio and i386 tests; physical HP XVideo and sustained throughput pending; raw60 remains an explicit developer profile |
| Input/overlay | evdev keyboard ownership, processed Xorg pointer forwarding, local power action, input release and cursor hiding/restoration | Actual private GTK session and rendered-cursor tests; physical HP Fn/power/touchpad behavior pending |
| Application host | Linux Wayland portals/PipeWire or Xorg/XTEST; Windows DXGI/SendInput; resolution matching, discovery, one-use numeric PIN and per-user identities | Private portal/PipeWire and Xorg integration tests; real graphics-driver modes, GNOME/Plasma and Windows runtime qualification pending |
| Linux service host | EVDI frame capture/virtual displays, uinput, RandR helper, LightDM/Xfce packaging | Compiled against EVDI; no greeter/lock/driver qualification on a real host |
| Windows service host | DXGI capture, virtual-display mapping, privileged input, session broker and service-mediated SAS | Platform/service cross-compilation; complete signed installer/runtime qualification pending |
| USB | Per-device reverse TLS relay, bounded buffers, explicit client device policy, Linux USB/IP importer, lease cleanup | TLS relay and sysfs fixture tests; physical device support unqualified, Windows safe import/eject unfinished |
| Image tools | Pinned downloads, configuration validation, detached image signatures, offline installer and state-preserving updater | Signature/tamper, layout, state preservation and refusal-path tests; release signing key supplied by operator |

## Build and test the native prototype

Use a Linux development machine with CMake, Ninja, a C++17 compiler, OpenSSL 3
headers, GTK3, WebKitGTK 4.1 headers, GStreamer ≥1.24 and its development
headers, plus X11/XTEST/RandR/XFixes, Wayland and PipeWire development files for the application host. Runtime elements are listed by `remoteosctl --media-capabilities`.
The optional Linux service edition additionally needs matching EVDI headers/library and the
qualified kernel module. Use `REMOTEOS_WITH_SERVICE_HOST=OFF` for the application-only build. GStreamer needs libav MPEG-2, RTP/RTCP, DTLS-SRTP,
Opus, XVideo and ALSA plugins; a Linux playback source needs PipeWire.

```sh
cmake -S . -B build -G Ninja \
  -DREMOTEOS_WITH_GTK=ON \
  -DREMOTEOS_WITH_BROWSER=ON \
  -DREMOTEOS_WITH_MEDIA=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

The optional `cmake/bootstrap-dev.py` extracts Ubuntu development packages into
`.deps` without installing packages or changing the machine. It uses the
machine's configured apt repositories; this is a native development convenience,
not the reproducible appliance dependency lock. After running it, source
`cmake/dev-env.sh` **from the repository root** before configuration and testing.
The checked native environment has WebKitGTK 2.52.6; the separately pinned
appliance build targets the requested 2.54.1.

The Linux capture SDK is pinned to EVDI 1.14.11. The development bootstrap
verifies its source archive and builds only its **userspace** library. For a
manual setup, fetch into a fresh destination with
`python3 packaging/linux/get-dependencies.py .deps` and run
`make -C .deps/evdi-1.14.11/library`. Use that directory for
`EVDI_INCLUDE_DIR` and its `libevdi.so.1.14.11` for `EVDI_LIBRARY` when replacing
an existing CMake cache that selected a different system SDK.
This does not install or load an EVDI kernel module. See the host guide before
provisioning the actual Ubuntu host kernel.

A protocol-only build works without GTK, WebKit or GStreamer: omit all three
`REMOTEOS_WITH_*` options. Media APIs then report unavailable and refuse a
streaming session. No fake desktop is substituted.

Run a local shell preview without taking ownership of this computer's input:

```sh
REMOTEOS_ASSET_DIR="$PWD/assets" build/remoteos-client \
  --windowed --no-input --no-autoconnect --no-discovery --state-dir /tmp/remoteos-client
```

To exercise pairing independently of capture, create identities and run the
host in a separate terminal:

```sh
build/remoteos-host --identity /tmp/remoteos-host --init
build/remoteos-host --identity /tmp/remoteos-host \
  --pair-code-file /tmp/remoteos-host/pairing-code
```

The initialization command prints the host fingerprint and private pairing-code
file path. Verify that fingerprint at the host, then use the printed value:

```sh
build/remoteosctl --host 127.0.0.1 --identity /tmp/remoteos-client-identity \
  --fingerprint VERIFIED_SHA256 \
  --pair-code-file /tmp/remoteos-host/pairing-code
```

The first successful pairing consumes the code file. Future connections use
the saved identity and pin. Pairing expires ten minutes after host startup and
closes after five failed attempts. Generate a new code and restart the service
to authorize another client. A normal service restart works after the old code
has been consumed. For a configured host, `--config host.json --init` writes
the code to that configuration's `pairing_code_file` path.

`--test-source` explicitly selects generated video for development; it does
not create or control a real host desktop. Synthetic audio additionally needs
`"audio_source":"test"` in the host configuration. The encrypted-media test
uses headless sinks, so a successful test does not validate XVideo adapters.

## Streaming profiles

The host-selection screen provides two profiles and saves the selected one:

| Profile | Laptop panel | Independent VGA stream |
|---|---|---|
| Wi-Fi / lower bandwidth | MPEG-2, target 30 fps | MPEG-2, target 30 fps |
| Wired performance | Raw, uncompressed I420/YUV420, target 60 fps at detected panel resolution | MPEG-2, target 30 fps |

The raw path converts and scales host frames on the host, sends them as RFC 4175
RTP inside the existing authenticated DTLS-SRTP transport, and presents the
depacketized YUV frames through XVideo. The client does not run a video codec
decoder or CPU color converter for that stream. YUV420 subsamples chroma; this
is uncompressed video, not lossless RGB desktop transport.

At 1366×768×60, raw YUV420 payload alone is **755.3 Mb/s** (94.4 MB/s), before
RTP/UDP/IP, authentication, encryption and the separate VGA/audio/device traffic.
It targets Gigabit Ethernet; it is not a feasible 60 fps profile for the original
Wi-Fi adapter. RGB24 would require 1.51 Gb/s before overhead and is not selected.
Receiving packets, decrypting, copying and presenting still consume HP resources.
The adaptive controller reduces frame rate and then host-produced stream
dimensions while preserving logical monitor geometry. **60 fps is a target,
not a measured HP result.** Select the MPEG-2 profile when using Wi-Fi or a
bandwidth-limited Tailscale route.

## Build the diagnostic USB/disk image

```sh
./scripts/build-image.sh check-config
REMOTEOS_JOBS=8 ./scripts/build-image.sh build
```

The bootstrap script downloads the SHA-pinned Buildroot release and applies the
checked WebKit package backport. Product changes remain in `buildroot/` as a
`BR2_EXTERNAL` tree. The full browser/toolchain build needs substantial disk,
memory and time; incremental builds reuse `output/hp2140`. The build uses a
lock to avoid two writers in the same output directory.

Follow [hardware qualification](docs/hardware-qualification.md) for inventory,
sampling and installation. No command in the normal build writes a physical
disk. `scripts/install-image.py` requires a signed image, a trusted public key,
an explicit whole-disk target and the repeated erase target. **It replaces all
partitions, including saved client state.** For an existing installation,
`scripts/update-image.py` verifies the signed image and identical supported
partition layout, then replaces only the OS/boot prefix while preserving the
state partition. Run it from separate Linux recovery media with every target
partition unmounted. It requires `--confirm-update` to repeat the exact target
path and refuses layout changes. Neither tool has been run against a physical
disk here. Automatic A/B updates and interruption-safe update recovery remain
later work.

The default USB approval list is empty. USB storage requires an explicit client
boot-disk exclusion before it can be exported. Browser sandboxing and normal
host driver/security protections must remain enabled during qualification.

## Host deployment and remaining release gates

See [host integration](docs/hosts.md), [client behavior](docs/client.md) and
[protocol architecture](docs/architecture.md). Setup scripts are supplied for
Ubuntu 24.04 Xorg/Xfce/LightDM and Windows 11 x64; they are not evidence that
driver installation, secure desktop capture or login transitions have passed.
The Windows production installer requires signed RemoteOS binaries and signed
dependencies. The implementation does not disable Secure Boot, UAC or locking.

Release blockers include the actual HP acceptance matrix, Google/Workspace MFA
in the maintained on-device browser, reliable XVideo presentation on both
outputs, signed host runtime bundles, unattended host transition testing,
qualified webcam bandwidth/mode enforcement and Windows USB safe detach.
VGA hotplug currently renegotiates both
media pipelines; uninterrupted preservation of the unaffected stream and host
monitor across that transition remains work before v1. A lost control
connection currently removes the host displays immediately, so retaining them
through short reconnects is also unfinished. Midstream audio errors require
session recovery; seamless audio transition recovery remains unqualified.

Source and license provenance is recorded in [third-party notices](third_party/NOTICE.md),
the Buildroot recipes and the Windows dependency lock. No device passwords,
paired private keys or release signing keys are checked into source.
