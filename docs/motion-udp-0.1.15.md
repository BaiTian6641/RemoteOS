# RemoteOS 0.1.15 — motion quality and receive scheduling

Install the matching HP image and Linux host bundle together. This is a
diagnostic performance update; sustained full-screen performance on the real
Atom N270 remains unqualified. Keep the independent KDE desktop, native panel
resolution, 60 FPS requested ceiling, 40 Mbps connection limit and trusted-LAN
UDP media selected. TLS continues to protect pairing, control and input.

## Changes that address the reported symptoms

The reported 20% was frame/late drops, not measured packet loss. The old
automatic profile was limited by its initial codec budget and did not use
available bandwidth to recover motion quality. Automatic MPEG-2 now starts at
up to 12 Mbps and probes up to 24 Mbps per display after stable intervals.
Multiple displays share 70% of the connection budget by pixel area. The limit
is a ceiling: static content need not fill it, and genuine delivery loss can
reduce the bitrate. Native pixels remain unchanged.

RTP packet reception previously waited on downstream allocation queries while
the jitterbuffer was waiting for a reordered packet. The receive thread now
terminates those packet-allocation queries locally. Complete compressed
pictures pass through a bounded queue before decoding, so expensive decode
work does not block socket draining. The queue discards old complete pictures
under overload and waits for a fresh I picture before resuming dependent
pictures. MPEG-2 requests a 512 KiB receive buffer, rather than allowing an
unbounded backlog.

The video packet-repair deadline is 150 ms; ordered pictures start immediately.
It is not a mandatory 150 ms playout delay. The bounded allowance covers
measured encoder startup spikes plus paced delivery and packet reordering
during live quality changes. Audio retains its own 40 ms jitter
budget. XVideo presentation remains immediate with no clock-driven video QoS.
Host encoding and paced packet transmission now overlap through a one-picture,
nonleaky queue, preserving MPEG-2 references. Packet pacing uses the reserved
wire share independently of average codec bitrate, preventing a quality
reduction from unnecessarily delaying large I pictures.

Host MPEG-2 uses rate-distortion block decisions, without B frames, plus a
100 ms VBV. Trellis quantization improved synthetic quality in the study but
caused expensive startup frames during live quality changes; the shipped
profile disables it to protect responsiveness. These encoder choices do not
claim to reduce entropy-decoding work on the HP. Decode or
presentation overload reduces FPS without also cutting quality. Actual RTP
deadline loss reduces bitrate. Stable delivery and measured decode time govern
recovery. Modern peers replace only the video encoder at a picture boundary;
audio, input, socket identities and host display geometry remain active.

## Codec and hardware findings

The N270 has one physical core and two SMT logical CPUs. A 60% combined CPU
reading does not imply 40% spare physical-core capacity. FFmpeg already uses
Atom-compatible x86 SIMD; software cannot enable a missing MPEG-2 GPU decoder.

The supplied hardware report identifies BCM4322 using b43, and advertises
legacy rates without HT. Linux Wireless documents that b43 lacks 802.11n HT,
aggregation and 40 MHz support. Being connected at 5 GHz does not establish
modern Wi-Fi throughput. The current network setup already disables Wi-Fi
power saving. No proprietary driver or firmware is substituted in this build.

The codec study compared MPEG-2, H.264 fastdecode and whole-frame JPEG on
repeatable moving fixtures. H.264 improved quality per bit but used more decode
CPU; JPEG's bandwidth varied sharply with content and exceeded the 40 Mbps
budget on difficult motion. MPEG-2 remains the default. No codec substitute was
shown to improve all three of decoder CPU, bandwidth and quality.

Those codec measurements use an ISA-limited Ryzen builder, not an Atom
emulator. The study used a 250 ms MPEG-2 VBV; the final transport regression
tests use the product's 100 ms VBV. The studies identify tradeoffs, not predicted
HP frame rates. Release validation includes benchmark source, methods and
results separately from native integration and target-library checks.

## Installation and measurement

Use a 4 GB or larger USB drive. Decompress `remoteos-hp2140.img.gz` and write
the resulting whole-disk `.img` using a disk-image writer. Reflashing replaces
the state partition; preserve needed settings and reports beforehand. Extract
`desktop-host-linux-0.1.15.tar.gz` and run `Start-RemoteOS-Host.sh` as your normal
logged-in user. This bundle uses the recorded Ubuntu 26.04 ABI; Ubuntu 24.04
requires a native build. The matching independent desktop requires installed
KDE Plasma 6 and uses separate display, input, configuration and audio, but
the same Unix account and home files.

On the HP, keep only the built-in display active for the first comparison.
Open the power overlay and record at least 60 seconds while reproducing the
same full-screen moving content. Collect the CSV plus its `.start.txt` and
`.end.txt` files. They distinguish received Mbps, decode/display FPS, decode
time, receiver timeline age, presentation/recovery drops, CPU topology and
frequency, socket overflows, and Wi-Fi rates/retries. The reports do not record
Wi-Fi passwords or SSIDs. These measurements determine whether the next limit
is the decoder, presentation or the radio, without treating frame drops as
network packet loss.

Primary references:

- [Intel Atom N270 datasheet](https://www.intel.com/content/dam/doc/datasheet/mobile-atom-n270-single-core-datasheet-.pdf)
- [Linux Wireless b43 capabilities](https://wireless.docs.kernel.org/en/latest/en/users/drivers/b43.html)
- [GStreamer RTP jitterbuffer](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html)
- [FFmpeg 6.1.5 x86 CPU detection](https://github.com/FFmpeg/FFmpeg/blob/n6.1.5/libavutil/x86/cpu.c)
