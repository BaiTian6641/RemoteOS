# Host integration and qualification

For a host the user opens after logging in, use the
[application host](application-host.md). It shares existing displays without
new display/input/USB drivers. Linux application mode supports Wayland with
consented RemoteDesktop/ScreenCast portals and PipeWire, as well as Xorg/XTEST.
The privileged service integration documented
below is an optional edition for virtual displays and unattended access.

The native platform adapters implement frame capture and input. Their `unattended_qualified` capability deliberately remains false: this workspace has no Windows desktop or HP hardware on which to establish login, lock, UAC, driver, or performance qualification. Do not interpret a successful compile as passing those gates.

## Shared platform contract

`remoteos::platform::HostPlatform` accepts one or two logical `DisplayConfig` entries and emits owned BGRA `VideoFrame` buffers from a capture thread. Frames carry display IDs and monotonic capture times. The network/media layer is responsible for authentication, encryption, backpressure, frame scaling and connection loss. Keep its callback short; do not stop the backend from its own callback.

Input uses Linux evdev key codes on both platforms. Absolute pointer coordinates refer to the host's logical desktop, independent of encoded video dimensions. Call `release_all()` before overlay takeover, disconnect, session handoff, or input ownership changes. Only the local client handles the physical power key. The initial Windows mapping supports the standard PC keyboard, navigation and Windows keys, F13–F24, common playback/browser/application keys, volume keys and mouse side buttons. Vendor-specific and other unsupported codes fail explicitly.

Capture errors appear in capability diagnostics. A platform without its capture dependencies fails `start()` rather than pretending to stream. Provisioning kernel drivers, selecting physical GPUs and opening firewall ports are administrator installation steps.

## Ubuntu 24.04 / Xorg

Use Xfce, LightDM, `lightdm-gtk-greeter`, `xfce4-screensaver`, an Intel/AMD open driver, and the pinned EVDI 1.14.11 module built and signed for the exact running kernel. Ubuntu's packaged EVDI may be older. Compilation requires 1.14.11 headers; runtime checks require both libevdi and the loaded kernel module to report 1.14.11. The installer checks module version and running-kernel vermagic. Secure Boot remains enabled when deployed with a trusted module signature.

`packaging/linux/dependencies.lock.json` pins the upstream archive hash and source commit. Fetch the verified source without installing anything:

```sh
python3 packaging/linux/get-dependencies.py /absolute/path/to/remoteos-dependencies
```

Choose the exact Ubuntu host kernel that will be qualified, install its matching headers and libdrm development files on the build machine, then build into the source tree. These commands do not install or load the resulting module:

```sh
evdi_source=/absolute/path/to/remoteos-dependencies/evdi-1.14.11
qualified_kernel=REPLACE_WITH_EXACT_TARGET_KERNEL_RELEASE
make -C "$evdi_source/library"
make -C "$evdi_source/module" KVER="$qualified_kernel" KDIR="/lib/modules/$qualified_kernel/build"
cmake -S . -B build-host \
  -DREMOTEOS_WITH_MEDIA=ON \
  -DEVDI_INCLUDE_DIR="$evdi_source/library" \
  -DEVDI_LIBRARY="$evdi_source/library/libevdi.so.1.14.11"
cmake --build build-host
```

Distribution packaging must include that exact user library and install the module into the matching kernel's module directory. Sign `evdi.ko` using the target kernel's `scripts/sign-file` and a deployment key whose certificate is already trusted by the host; provisioning/enrolling that key is an administrator step. Verify `modinfo -F signer`, version and vermagic before loading the module. Record the tested kernel release, source hash, signing certificate fingerprint, GPU driver and qualification results. No host kernel or signing key is invented by this repository, and installing an older distribution package does not satisfy these checks.

Native behavior:

- Open up to two available EVDI DRM devices, advertise stable RemoteOS EDIDs at the requested logical modes and 60 Hz scanout, and request BGRA updates at at most the configured streamed fps.
- Use EVDI's event loop and registered buffers. Compose the cursor through EVDI, deliver full frames, and reject unexpected dimensions/pixel formats.
- Create an authenticated-session uinput device. Track pressed keys/buttons and release them on a normal stop. No input device exists before a session starts.
- Execute a fixed root-owned layout helper. It only changes administrator-mapped outputs whose EDIDs match the requested RemoteOS display IDs, then restores their prior state when stopping. It never guesses a physical output.

Install dependencies and build the host using the repository's CMake instructions. Then explicitly run:

```sh
sudo packaging/linux/install-host.sh /absolute/path/to/remoteos-host
```

The installer writes service, module, Xorg and LightDM configuration. It does not start the service or restart the display manager. Schedule the required maintenance reboot. Configure `/etc/remoteos/displays.json` with the actual `xrandr --props` output names and, where required, the EVDI sink and physical source provider IDs from `xrandr --listproviders`. Example after inspecting the qualified machine:

```json
{
  "outputs": { "0": "DVI-I-1-1", "1": "DVI-I-2-2" },
  "providers": [{ "sink": "1", "source": "0" }, { "sink": "2", "source": "0" }]
}
```

Those names are examples, not universal identifiers. The shipped configuration deliberately requires replacement. The service uses `DISPLAY=:0` and LightDM's `/var/run/lightdm/root/:0` authority; adjust them for the actual seat. Verify the selected outputs show the LightDM greeter and the existing session after login. Configure LightDM GTK Greeter's `active-monitors` for a remotely visible output on the qualified host. No autologin or lock bypass is configured.

After provisioning, explicitly enable/start the service. Identity state is under `/var/lib/remoteos`; transient pairing information is under `/run/remoteos`. Runtime output selection changes are temporary; the initial driver/module/Xorg setup persists until uninstalled.

```sh
sudo systemctl enable --now remoteos-host
sudo journalctl -u remoteos-host -f
```

Linux limitations requiring integration qualification: an existing Xorg server must expose both EVDI providers; provider topology varies by GPU/kernel; desktop manager restarts may require the session to reconnect and reapply layout. The backend does not claim arbitrary Wayland support.

The Linux sample config selects PipeWire playback audio. `audio::SessionSource` discovers seat0's active Xorg user through logind, excludes the greeter and tracks a generation across login/logout/user changes. A lock screen retains the same user identity. When audio is needed, it executes the installed root-owned `remoteos-audio-session` helper, which drops supplementary groups and switches to that user's primary GID and UID before connecting to `/run/user/UID/pipewire-0`. The helper verifies the PipeWire server's UID and transfers the connected descriptor through a private `SCM_RIGHTS` socket. The parent verifies the returned socket/UID, checks the session is still current and returns an owned descriptor. Setup times out rather than blocking indefinitely. No PipeWire access policy is weakened.

The host refreshes the active session every three seconds on a separate worker, rebuilds audio on generation changes, and acquires a fresh user-connected socket after media renegotiation. The GStreamer plugin duplicates that socket; the host retains its original until the pipeline stops and never reuses the old protocol connection. At initial acquisition, the greeter or an unavailable user PipeWire daemon produces a no-audio status while video remains available. A GStreamer audio error during an active stream currently stops the combined media engine and requires session recovery; seamless recovery across these transitions is not implemented. The source selects the playback sink monitor and never falls back to a microphone. Descriptor acquisition is tested against a fake UNIX server with the current user's credentials; actual PipeWire policy, playback capture and login/lock transitions still require qualification on Ubuntu. See the [PipeWire GStreamer connection implementation](https://github.com/PipeWire/pipewire/blob/1.0.5/src/gst/gstpipewirecore.c) for its descriptor ownership.

## Windows 11 x64

Build the platform backend with `d3d11`, `dxgi`, `user32`, `gdi32`, `advapi32` and `wtsapi32`; build the separate `remoteos-host-service` executable with `advapi32`, `wtsapi32` and `userenv`. The broker dynamically loads the operating system's `sas.dll`. MSVC or MinGW-w64 can compile the sources. Embed the supplied per-monitor DPI manifest in the host executable.

The broker runs as LocalSystem in session 0 and launches a LocalSystem host helper in the active console session. It supervises that process with a kill-on-close job, respawns after exit/session changes, and requests graceful shutdown using the SYSTEM-only event `Global\RemoteOS.Stop.<PID>`. The host executable observes that event to stop capture and release input; the broker allows five seconds before terminating an unresponsive helper. The implementation never changes UAC or locking policy.

The capture backend selects only administrator-mapped Virtual Display Driver outputs, applies their requested logical dimensions/positions, and restores their modes on clean exit. DXGI creates a capture device for each output's own adapter; frames are copied from staging textures, with the pointer composited when DXGI reports a separate visible cursor. Capture threads switch to the active desktop and retry duplication through access/session transitions. `SendInput` uses scan codes and the active virtual-desktop coordinate space.

For Ctrl+Alt+Delete, only the service invokes `SendSAS`. Its named pipe is local-only, SYSTEM-only and checks the caller PID against the supervised helper. The service checks the existing Windows **Disable or enable software Secure Attention Sequence** policy permits services. The installer does not weaken other policies. `SendSAS` provides no completion result; receipt of the operation must still be tested on the lock/login screens.

Signed dependencies are pinned in `packaging/windows/dependencies.lock.json`, including SHA-256 digests from their release metadata. `Get-Dependencies.ps1` downloads those exact artifacts, checks digests, validates the USB/IP executable's Authenticode signature and the VDD archive's catalog signatures. No driver is installed by the download script. Use the original signed driver installers; PnP must verify catalog membership and architecture. Never enable test signing or disable Secure Boot. Driver files have not been downloaded or signature-tested on Windows in this workspace.

Configure two SDR virtual monitors through the signed VDD distribution before starting RemoteOS. The backend requires `REMOTEOS_DISPLAY_0` and `REMOTEOS_DISPLAY_1`, for example `\\.\DISPLAY2` and `\\.\DISPLAY3`; these must match the actual VDD adapters. HDR/rotated outputs are not supported by this BGRA baseline.

The production installer requires Authenticode-signed RemoteOS binaries, copies them under Program Files, protects ProgramData state for SYSTEM/Administrators, creates the broker service and records service-specific display mappings. It leaves firewall changes, driver installation, GStreamer/runtime DLL provisioning and service start explicit:

```powershell
.\packaging\windows\Get-Dependencies.ps1 -Destination C:\RemoteOS-Dependencies
.\packaging\windows\Install-Host.ps1 -BuildDirectory C:\RemoteOS-Build `
    -Display0 '\\.\DISPLAY2' -Display1 '\\.\DISPLAY3'
Start-Service RemoteOSHost
```

Unsigned development builds can be compiled and inspected, but are not silently installed as a production SYSTEM service. A code-signing certificate and signed release runtime bundle are external release inputs.

`packaging/windows/vdd_settings.xml.example` supplies a two-monitor, 60 Hz configuration for the pinned driver. Back up the installed driver configuration and apply this during driver provisioning. It includes both HP panel variants and 1080p. The archive hash and DLL's x86-64 PE architecture were checked on Linux; catalog trust still requires Windows verification.

## USB helper boundary

Camera approval currently checks device identity only. MJPEG preference, UVC mode interception and rejection of over-budget camera modes are not implemented. The tunnel's fixed traffic limit does not qualify a webcam mode; keep cameras unapproved until those controls and physical throughput tests are complete.

`remoteos::devices` supplies a Linux sysfs policy and privileged USB/IP command wrappers. Approved VID/PID plus optional serial identify qualified devices. HID interfaces, hubs, network interfaces, the configured boot disk, mounted filesystems and swap backing devices cannot be exported. Storage remains denied until the supervisor supplies explicit boot-disk exclusions. Composite devices with any forbidden interface are denied as a whole. Selection is rechecked immediately before binding.

The importer connects only to `127.0.0.1` on an explicitly supplied tunnel port. Linux records the assigned VHCI port by matching the exact loopback endpoint and bus ID in USB/IP's root-only state records. Clean detach refuses mounted imported storage and flushes unmounted block devices before removing the VHCI connection. Helpers execute an absolute USB/IP binary with separate argv entries, validate bus IDs and bound command time. They never invoke a shell or launch stock `usbipd`, whose default listener is not restricted to loopback. The supervisor must stop local camera use and authorize the reverse tunnel before binding. Policy tests use fixtures and never touch actual USB hardware.

The asynchronous device relay uses a distinct mutual-TLS connection for each device. The client verifies the paired host fingerprint; the host verifies the paired client fingerprint, active session, bus ID, single-use grant token and authorization callback. It acknowledges authorization before connecting to the client's loopback exporter. The host importer endpoint listens only on loopback. Both directions use one 16 KiB backpressure buffer and a fixed 4 MiB/s rate ceiling (approximately 33.6 Mb/s before transport overhead); webcam qualification must fit that ceiling. There are at most eight active/prepared devices and eight pending handshakes. Unauthenticated setup expires after five seconds; authenticated setup and unused grants after thirty seconds. Device/session revocation closes relay sockets and importer listeners.

Every removed grant posts a notification containing its session, bus ID and unique token after closure. The host uses all three to discard stale callbacks, remove the matching device entry and clean its owned VHCI import. Individual relay failure and expiry therefore have a cleanup path even while the desktop control connection remains alive. Relay loss has physical-unplug semantics: the connection is already gone, so cleanup cannot promise an eject or flush. Normal user-requested detach still uses the separate eject/flush checks before closing the relay.

Windows USB forwarding remains unavailable pending a qualified mapping from imported VHCI ports to Windows device nodes for safe storage eject. The pinned signed Windows importer is a provisioning dependency; its presence does not imply this implementation safely detaches storage. Linux exporter/importer kernel attachment and actual camera/storage behavior still require hardware tests. Relay tests independently verify encrypted loopback transfer, pin failures, token rejection, authorization denial, session/device revocation and resource limits.

## Acceptance record

Automated checks performed during implementation: Linux backend compilation with and without EVDI enabled; Windows backend compilation and service link with MinGW-w64; Python syntax check for the layout helper; POSIX shell syntax check for the Linux installer; active-user audio descriptor handoff and credential validation tests; USB safety policy and encrypted relay tests. These checks run without installing drivers or changing this workspace host's desktop.

Before marking either platform qualified, record exact OS/kernel/driver versions, signed dependency digests, GPU and monitor mappings, then pass:

1. Cold boot into login, authenticate normally, logout, lock/unlock and reconnect while locked.
2. Windows UAC secure desktop and service-authorized Ctrl+Alt+Delete; Linux greeter, polkit and screensaver.
3. Hold modifiers and mouse buttons while opening the client overlay, losing the network and stopping/restarting the service; no stuck input may remain.
4. Two separate virtual displays at requested logical sizes; 30-minute media/audio/camera run with measured capture rate, CPU, memory and latency.
5. VGA unplug/replug and display-manager/desktop transitions; retain the unaffected monitor and restore previous output state on clean disconnect.
6. Signed-driver loading with normal platform protections, identity permissions, denied unknown peers and revocation.

The host OS service cannot capture BIOS, bootloader or full-disk-encryption unlock screens.

Primary implementation references: [EVDI API](https://github.com/DisplayLink/evdi/blob/v1.14.11/library/evdi_lib.h), [uinput](https://docs.kernel.org/input/uinput.html), [LightDM configuration](https://github.com/canonical/lightdm/blob/main/data/lightdm.conf), [DXGI duplication and secure desktop](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput), [SendSAS](https://learn.microsoft.com/en-us/windows/win32/api/sas/nf-sas-sendsas), [Virtual Display Driver releases](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23), [USB/IP importer release](https://github.com/vadimgrn/usbip-win2/releases/tag/v.0.9.8.1).
