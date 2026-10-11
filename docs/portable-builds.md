# Portable RemoteOS builds and relative mouse — 0.1.19

The client adds **Relative mouse (Blender / 3D)** in Client controls. On the HP,
short-press the power button, select this option, then Resume. In the Linux
desktop client, use Ctrl+Alt+Home to open those controls. The selection is saved
with the connection profile. Absolute positioning remains the initial default.

Relative mode uses XInput 2.1 processed relative deltas, including Xorg/libinput
touchpad processing. It does not interpret raw touchpad hardware coordinates.
Mouse buttons and scroll retain their ordered control messages. Movement does
not stop at the client screen edge, and host cursor warps do not produce an
absolute-position jump. The overlay, disconnect and desktop-client focus loss
release capture and all held host input. No new video processing is added.
Touchscreens and absolute tablet valuators are excluded from relative mode.
The Linux client needs X11 or XWayland; native Wayland pointer locking is a
separate future client backend. Wayland **hosts** use the existing portal
relative-pointer injection API.

## GitHub Actions

`.github/workflows/portable.yml` builds on push to `main`, pull requests,
version tags and manual dispatch. The workflow contains:

| Artifact | Build and portability verification | How to use |
|---|---|---|
| Windows x64 portable ZIP | Native MSVC 2022 build; bundled GStreamer/OpenSSL and app-local Microsoft CRT; PE dependency audit; generated MPEG-2 encode/decode; GUI startup with SDK unavailable | Extract the entire folder and open `bin/remoteos-host-app.exe`, or the top-level launcher |
| macOS universal `.app.zip` | Native Apple SDK; arm64 and x86_64 in every Mach-O; relocated framework and plugins; code-signature audit; runtime checks on Apple Silicon and Intel runners | Extract, move the app wherever desired, then open it |
| Linux x86_64 client AppImage | Ubuntu 24.04 baseline; bundled libraries/plugins; relative-input and fullscreen tests; executable extraction/startup | Mark executable and run it; `--appimage-extract-and-run` is supported when FUSE is unavailable |

Each job uploads the actual executable package and a SHA-256 checksum. macOS
ZIP creation uses `ditto` to retain bundle symlinks and signatures. Windows
packages require Windows 11 x64; the Mac application targets macOS 13 or newer.
Build artifacts remain available from the workflow run for 30 days. A version
tag creates a **draft GitHub Release** containing verified job artifacts, ready
for review and publication. Normal branch builds do not publish a release.

Actions are pinned to full commit IDs. Official GStreamer 1.28.7 runtime and
development installers are pinned by URL and SHA-256 in
`packaging/native-sdk.lock.json`. Their OpenSSL SDK is used for both native
hosts. Runtime redistribution licenses ship inside each package. CI installs
SDKs on disposable runners only; end users do not install GStreamer, OpenSSL,
drivers or a host service. Source/build images, private identities and signing
keys are excluded from Git.

## Native host scope

Keep the host app open while sharing. Pair through the client host list and PIN
after comparing the host fingerprint. Windows uses Desktop Duplication and
SendInput; macOS uses ScreenCaptureKit and Accessibility events. Both use the
existing authenticated RemoteOS protocol and MPEG-2/UDP media path. Windows
supports WASAPI playback audio; the current macOS backend does not yet capture
playback audio. These application backends share existing displays. Linux's
independent desktop is not automatically available on Windows or macOS.

The macOS app needs Screen Recording and Accessibility permission in System
Settings. Restart after granting Screen Recording. On the client, disable
automatic host resolution matching if the Mac cannot supply that panel mode.
Normal host applications do not control login, locked sessions or privileged
secure desktops. Physical host permission/capture/input testing remains
necessary even after CI compile and package tests pass.

CI uses ad-hoc macOS signatures and does not claim Apple notarization or a
Developer ID signature. Gatekeeper may require explicit approval of the
downloaded application. Do not disable Gatekeeper or other host protections.
For distribution with a Developer ID certificate, supply
`REMOTEOS_MAC_SIGN_IDENTITY` to the local build script, then notarize and staple
the bundle with your Apple developer credentials. No credentials are required
or embedded in the default CI builds.

## Local native builds

Windows: use PowerShell 7 in the x64 Visual Studio developer environment,
install the pinned official GStreamer SDK with development components, then
run `packaging/windows/Build-DesktopHost.ps1 -GStreamerRoot PATH`. It stages a
fresh portable folder. Run `verify-portable.ps1 -Package FOLDER` with the SDK
unavailable before distributing it.

Mac: install the pinned GStreamer runtime and development `.pkg` files and
Xcode command-line tools, ensure CMake/Ninja/pkg-config are available, then run
`bash packaging/macos/build-host.sh`. The script creates a universal app,
bundles its runtime, relocates dependencies and signs nested binaries.

`remoteos-runtime-check` uses generated video only. It verifies required plugin
factories, twelve MPEG-2 encode/decode frames and TLS 1.3 initialization. It
never captures a desktop, reads physical input, opens a camera or requests
screen-sharing permission.

Primary references: [GStreamer SDKs](https://gstreamer.freedesktop.org/download/),
[GitHub runner architectures](https://docs.github.com/en/actions/reference/runners/github-hosted-runners),
[Microsoft runtime redistribution](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).

## HP diagnostic image and local evidence

The local 0.1.19 HP image is `output/releases/hp2140-relative-mouse-wl-0.1.19/images/remoteos-hp2140.img.gz`.
It retains the Broadcom wl profile and b43 recovery option. The existing Linux
application host 0.1.18 accepts the relative-motion protocol; upgrading that
host is not required to enable the new client mouse mode.

The local general client is `output/releases/RemoteOS-Client-0.1.19-x86_64.AppImage`.
Its Ubuntu 24.04 dependency baseline has a maximum required GLIBC symbol of
2.38. The package opens setup on a private X11 display with the development
SDK environment removed. All 1,146 staged asset/library/license hashes were
verified before distribution.

Validation includes 32 passing native regression tests; three real ELF32 GTK
session cases against the packaged Linux host (1024×576, 1366×768 and managed
1366×768); continuous relative movement at the client edge; host recentering;
held middle-button release; absolute-pointer recovery; rendered setup/overlay
cursor checks; signed-image and gzip verification; a direct audit of 59 ELF32
components; and an offline BIOS boot using an N270 CPU model and 2 GB RAM.
QEMU establishes startup and Intel driver loading, not physical HP rendering
or performance. Real Blender dragging on the HP remains a hardware check.

The detailed local record is
`output/releases/hp2140-relative-mouse-wl-0.1.19/validation/validation-summary.json`.
GitHub records native Windows and macOS build/package checks in the workflow
run for each source commit. These executable-package checks are distinct from
physical ScreenCaptureKit/Desktop Duplication and permission qualification.
