# Native host startup, permissions and packet pacing — 0.1.20

This release addresses reports that the Mac application exits at launch and
that the Windows application only reports “host exit code 1” when sharing
starts. These reports do not establish a single cause on every machine.

The old CI checks ran the executable for three seconds and checked that the
process survived. They did not prove that Finder opened a visible Mac window
or that the host subprocess initialized identities, permissions and networking.

## Startup and permissions

The Mac delegate now has an explicit lifetime through the application run loop.
Startup exceptions show a readable error instead of an uncaught abort. A cache
directory that cannot be created does not abort portable runtime initialization.
Screen Recording and Accessibility consent is requested by the GUI on its main
thread after a user action. The helper only checks grants and reports missing
permissions. Its main Core Foundation run loop stays available while the Asio
server runs, allowing ScreenCaptureKit's main-queue callbacks to complete.
Capture permission revocation is reported through the existing session-error
path and releases remote input.

The Mac window includes permission checks, links to both privacy settings, and
an Open host log button. After granting Screen Recording, quit and reopen the
application. Ad-hoc signatures remain the default; no Developer ID credentials
or notarization are supplied. Do not bypass Gatekeeper or reset TCC globally.
A crash before the executable can run still requires the macOS crash report's
Termination Reason or missing-library line.

Windows no longer discards the helper's stdout and stderr. It captures them
through a pipe with an explicit inherited-handle list, shows structured sharing
errors, and retains a bounded private `last-host.log` under the user's state
directory. The host app includes Open host log. The ready status is displayed
only after the host reports an actual listener. The host manifest uses UTF-8;
TLS identities are read with filesystem paths and loaded from memory, avoiding
OpenSSL's separate narrow-path file loading.

Windows application mode checks the current interactive user's desktop access
rather than requiring the physical console session. Active RDP sessions are
eligible; Session 0, disconnected sessions, lock screens and inaccessible UAC
desktops receive explicit errors. The application does not elevate itself,
install a service, edit firewall rules or alter host security settings. Permit
the host executable on the intended private network when Windows Firewall asks.
Default ports are TCP 47010 (control), TCP 47012 (device resources), UDP 47009
(discovery) and UDP 47020–47022 (video/audio).

Linux retains its X11/Wayland consent and private independent-desktop paths.
The complete local suite covers those paths in isolated displays/network
namespaces. The existing 0.1.19 HP client uses the same protocol and does not
need a reflash for these host-only startup and pacing changes.

## Latency

Windows RTP streams use a private high-resolution waitable timer for packet
pacing instead of ordinary submillisecond `sleep_until` calls. This avoids
scheduler-tick rounding without global timer-resolution changes, busy spinning
or elevation. The existing wire-rate ceiling, separate audio reservation and
bounded video queues remain enforced. A pacing check measures 2,000 packet
schedule reservations at 32 Mb/s; it sends no network packets. Actual WLAN
latency, host capture, HP decoding and display scanout still require hardware
measurements.

ScreenCaptureKit keeps Apple's documented minimum queue depth of three. A
smaller unsupported value is not used as a latency shortcut. Trusted LAN and
Tailscale UDP media remains available; control and pairing remain authenticated.

## Validation and diagnostic limits

Native CI uses Tahoe (`macos-26` and `macos-26-intel`), opens the bundle through
Launch Services, and verifies a receipt from a visible setup window without
requesting privacy grants. With the SDK unavailable, both native packages run
MPEG-2 encode/decode, TLS initialization and packet pacing checks. A real host
process then generates its identities under a non-ASCII path, starts on
loopback, pairs over pinned TLS, and sends generated MPEG-2 over UDP. A separate
permission probe must either initialize desktop metadata or report an expected
permission denial; it never starts capture or injects input.

Synthetic mode explicitly skips physical desktop preparation and advertises no
input injection. Native CI is not evidence that a particular computer has
granted consent or that physical capture/input works. Read `last-host.log` for
the actual sharing error. Mac launch failures also appear in macOS Diagnostic
Reports if execution never reaches the setup window.

References: [Apple screen capture](https://developer.apple.com/documentation/screencapturekit),
[Apple capture queue minimum](https://developer.apple.com/documentation/screencapturekit/scstreamconfiguration/queuedepth),
[Windows UTF-8 manifest](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page),
[Windows desktop access](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openinputdesktop),
[GitHub macOS runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
