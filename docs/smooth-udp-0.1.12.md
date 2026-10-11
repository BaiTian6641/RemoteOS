# HP smooth UDP update — 0.1.12

Use the **0.1.12 Linux application host** and **0.1.12 HP image** together.
This update reduces avoidable waiting while preserving native panel pixels,
MPEG-2 host encoding and the shared **40 Mbps** connection ceiling. It does
not establish the physical HP's achievable frame rate or input-to-display
latency.

## Installation and starting settings

1. Stop the previous sharing application. Extract
   `desktop-host-linux-0.1.12.tar.gz` and open `Start-RemoteOS-Host.sh` as your
   normal logged-in desktop user. No kernel modules or services are installed.
2. Write the new `remoteos-hp2140.img` to a **4 GB or larger USB drive**. If
   downloading `.img.gz`, decompress it first when your writer requires a raw
   image. This is a whole-disk image: a full reflash replaces saved Wi-Fi,
   Tailscale and pairing state. The repository's `scripts/update-image.py`
   supports an offline signed update preserving the existing state partition
   when layouts match; it requires an explicit, unmounted target disk.
3. For KDE Plasma Wayland, choose **HP native virtual display · KDE Wayland**,
   and **1024×576** for the panel identified in this laptop's logs. Choose
   **1366×768** only for an HD-panel variant. Keep **Match host resolution**
   enabled on the HP.
4. Begin with **30 fps** and **Trusted LAN / Tailscale · plain UDP**. Start
   sharing and reconnect the HP. With the reported CPU headroom, test the
   **60 fps** option next. The host and encoded pictures remain at native
   panel resolution. Change settings while sharing is stopped.
5. Inspect the power-button overlay during scrolling and pointer movement:
   displayed FPS, late drops, total/busiest CPU, video queue, playout budget
   and control round trip. Keep the highest stable frame-rate setting.
   **20** and **15 fps** remain available if presentation falls behind.

The client requests up to 60 fps; the host selects its configured limit.
Adaptation lowers frame rate before considering encoded dimensions, and
native-display mode never shrinks the encoded text. A 60 fps setting is a
testable target rather than a guarantee. Raw I420 at 1366×768/60 still needs
about 755 Mb/s before transport overhead and cannot fit this Wi-Fi budget.
MPEG-2 remains the Wi-Fi profile.

## Changes that reduce waiting

- **Whole pictures survive RTP fragmentation.** New peers negotiate GStreamer's
  `rtpgstpay`/`rtpgstdepay` framing for complete MPEG-2 encoder buffers. The HP
  decoder receives each reassembled picture directly. The previous MPV
  depayloader emits fragments, requiring a parser that waits for the following
  picture boundary. This update removes that lookahead on the receiver.
  The codec remains MPEG-2 and the HP continues to use software slice decoding
  followed by XVideo YUV presentation.
- **The receiver starts with a 10 ms jitter budget**, rather than 40 ms, for
  newly negotiated sessions. Decoded-frame queues remain bounded. A smaller
  budget trades tolerance of network jitter for shorter playout delay.
- **Wayland input no longer waits for each portal reply.** Consent-scoped
  Notify calls are queued in D-Bus connection order without holding capture
  delivery until an acknowledgment arrives. Local connection/queue errors
  and permission revocation are still handled. Individual Notify calls no
  longer provide a remote acknowledgment or per-call error reply.
- **Stale absolute pointer positions are coalesced** in the TLS send queue.
  The newest adjacent waiting position replaces older ones, without replacing
  the in-flight packet or crossing clicks, keys, scroll or release boundaries.
  Relative motion retains every event.
- **Packet pacing uses scheduled packet-start times.** Scheduler wakeup
  lateness no longer adds another complete serialization interval to every
  packet. Catch-up is bounded to two additional packets; idle time cannot
  accumulate a large burst credit. Audio keeps its independent allocation.
- **The host starts at 30 fps** and provides an explicit 60 fps MPEG-2 option.
  The former 20 fps default itself imposed a 50 ms picture interval even
  when the decoder and network had spare capacity.

Host-side capture, color conversion, scaling when explicitly enabled,
compression and pacing remain on the host. The HP performs packet reception,
MPEG-2 decoding and YUV presentation. Merely replacing GStreamer with an
FFmpeg command would not remove those required client operations; the image
already uses FFmpeg codec libraries.

## Trusted-network and encrypted media modes

Video and audio already used UDP in earlier images. This update adds a
**plain RTP over UDP** option to remove the additional RemoteOS DTLS-SRTP
layer when using the requested trusted LAN or Tailscale environment. The
standalone service/configuration interface retains encrypted media as its
default; the application host exposes both choices and starts with the
trusted-network choice.

**TLS 1.3 pairing, stored host/client identities and reliable ordered input
remain in both modes.** The negotiated peer address, port and fresh per-stream
SSRC reject unrelated or stale sessions. Those filters do not cryptographically
authenticate or encrypt LAN media. On a direct LAN, anyone able to observe
or spoof that traffic may read or interfere with it. On Tailscale, its normal
WireGuard protection still applies; this choice removes duplicate application
media encryption rather than disabling Tailscale encryption. Choose
**Encrypted UDP · DTLS-SRTP** when that application protection is desired.

Plain media requires directly reachable negotiated UDP endpoints and does not
learn translated source ports. Routed LAN and Tailscale addresses are its
intended paths. Encrypted mode retains the existing authenticated DTLS port
learning. Both choices use host UDP 47020–47022 and client UDP 48020–48022;
control remains TCP 47010. Media is admitted only after paired control startup.

Older clients without the new transport/framing capabilities receive encrypted
MPV media with the former 40 ms jitter budget and at most their advertised
MPEG-2 frame rate. New clients clamp MPEG-2 requests to an older host's limit.
Use both 0.1.12 packages for the complete reduction in buffering.

The reported 50 Mbps path is a useful measured ceiling for this deployment,
not a universal Tailscale limit. The 40 Mbps RemoteOS ceiling keeps margin;
video encoder allocation remains at most 32 Mbps. A direct Tailscale route
usually performs better than a relay, but actual route, Wi-Fi jitter and
other network traffic still need measurement.

## Evidence and practical limits

Release receipts cover real socket negotiation, generated native-size pictures
at 30/60 fps, an isolated single MPEG-2 picture without a following frame,
independent Opus playback, stale-session rejection, retained encrypted paths,
packet pacing, and actual i386 appliance libraries. Private KWin/PipeWire tests
exercise native display size, host frame-rate selection, adaptation, PIN
pairing and saved reconnect. A deliberately delayed portal reply checks that
input queuing and capture continue. A 2,000-position input burst checks
coalescing and preserved button/key/release order. Private Xvfb and offline
BIOS tests inspect the packaged GUI, cursor and image.

These are builder and isolated software tests, **not measurements from the
HP's Atom, GMA 950, wireless card or screen**. Headless MPEG-2 timing tests
use generated frames and a discard sink; they do not measure XVideo scanout
or establish decoded image fidelity on the laptop. Overlay playout is clock
compensation and control RTT is a separate round trip. Neither includes the
complete input-to-photon path.

The actual i386-library test on the builder recorded these generated-frame
results. The first-picture interval includes test encoding, loopback transport
and presentation to the synchronized discard sink; it is not remote input
latency. Both native sizes used a 10 ms video playout budget and had no late
video drops during the two-second steady interval.

| Transport | Generated picture size | Target / measured FPS | Isolated first picture |
|---|---|---|---|
| Plain RTP/UDP | 1024×576 | 30 / 29.50 | 19.21 ms |
| Plain RTP/UDP | 1366×768 | 60 / 60.00 | 23.93 ms |
| DTLS-SRTP | 1024×576 | 30 / 29.50 | 13.44 ms |
| DTLS-SRTP | 1366×768 | 60 / 60.00 | 17.70 ms |

Short loopback runs do not establish a latency advantage for either transport.
The 25-test regression suite passed in one complete final run. The delayed
portal test queued 20 input calls in less than one measured millisecond while
replies waited 100 ms, and the TLS input burst reduced 2,000 absolute positions
to three current positions while preserving action order. Separate packaged
host sessions sustained approximately 30 and 60 fps using private software
KWin at the two native resolutions. Full test logs and receipts accompany the
artifacts.

Test an ordinary desktop session on the HP first, then scrolling/video and
a storage transfer. Prefer a reachable LAN address for a same-LAN comparison;
compare it with the Tailscale route when needed. Record the overlay values or
use the existing performance recorder. Full dual-display, webcam and sustained
Wi-Fi qualification remain required. This diagnostic update retains the
logged-in application-host access model; unattended login/lock access and
Windows release qualification remain separate work.

Primary references:

- [GStreamer frame-preserving RTP payloader](https://gstreamer.freedesktop.org/documentation/rtp/rtpgstpay.html)
- [GStreamer matching depayloader](https://gstreamer.freedesktop.org/documentation/rtp/rtpgstdepay.html)
- [Pinned MPV depayloader implementation](https://raw.githubusercontent.com/GStreamer/gstreamer/1.24.13/subprojects/gst-plugins-good/gst/rtp/gstrtpmpvdepay.c)
- [GIO queued D-Bus messages](https://docs.gtk.org/gio/method.DBusConnection.send_message.html)
- [RemoteDesktop portal input contract](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)
- [Tailscale connection paths](https://tailscale.com/docs/reference/device-connectivity)
- [Tailscale performance guidance](https://tailscale.com/docs/reference/best-practices/performance)
