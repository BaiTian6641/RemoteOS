#!/usr/bin/env bash
# Execute actual i386 plugins without loading any native-host GStreamer plugins.
# Requires a Linux builder capable of running 32-bit ELF; does not use devices,
# network sockets, audio playback or a display. This is not a throughput test.
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output=${1:-"$repo/output/hp2140"}
output=$(cd -- "$output" && pwd)
target="$output/target"
evidence="$output/validation/target-media"
mkdir -p -- "$evidence"
loader="$target/lib/ld-linux.so.2"
libraries="$target/lib:$target/usr/lib"
run_gst() {
    local program=$1
    shift
    env GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0="$target/usr/lib/gstreamer-1.0" \
        GST_REGISTRY_1_0="$evidence/registry-i386.bin" GST_REGISTRY_FORK=no \
        "$loader" --library-path "$libraries" "$target/usr/bin/$program" "$@"
}
for element in xvimagesink rtpvrawpay rtpvrawdepay rtpgstpay rtpgstdepay rtpjitterbuffer dtlssrtpenc dtlssrtpdec srtpenc srtpdec avenc_mpeg2video avdec_mpeg2video rtpbin opusenc opusdec alsasink; do
    run_gst gst-inspect-1.0 "$element" > "$evidence/$element.txt" 2>&1 || {
        echo "Target media factory failed: $element (see $evidence/$element.txt)" >&2
        exit 1
    }
    printf '%s: PASS\n' "$element"
done
run_gst gst-launch-1.0 -q videotestsrc num-buffers=10 \
    '!' video/x-raw,format=I420,width=1366,height=768,framerate=30/1 \
    '!' avenc_mpeg2video max-bframes=0 flags=low_delay thread-type=slice threads=0 gop-size=15 bitrate=12000000 \
    '!' rtpmpvpay max-ptime=1 '!' rtpmpvdepay '!' mpegvideoparse \
    '!' avdec_mpeg2video max-threads=2 thread-type=slice \
    '!' fakesink sync=false > "$evidence/mpeg2-roundtrip.log" 2>&1
printf '%s\n' '1366x768 target MPEG-2 RTP roundtrip: PASS (10 generated frames)'
run_gst gst-launch-1.0 -q videotestsrc num-buffers=10 \
    '!' video/x-raw,format=I420,width=1366,height=768,framerate=60/1 \
    '!' avenc_mpeg2video max-bframes=0 flags=low_delay thread-type=slice threads=0 bitrate=12000000 \
    '!' video/mpeg,mpegversion=2,systemstream=false,parsed=true \
    '!' rtpgstpay mtu=1200 config-interval=1 '!' rtpgstdepay \
    '!' avdec_mpeg2video max-threads=2 thread-type=slice \
    '!' fakesink sync=false > "$evidence/mpeg2-frame-roundtrip.log" 2>&1
printf '%s\n' '1366x768 target frame-preserving MPEG-2 RTP at 60 fps: PASS (10 generated frames; not a throughput measurement)'
run_gst gst-launch-1.0 -q videotestsrc num-buffers=10 \
    '!' video/x-raw,format=I420,width=1366,height=768,framerate=60/1 \
    '!' rtpvrawpay '!' rtpvrawdepay '!' fakesink sync=false \
    > "$evidence/raw-roundtrip.log" 2>&1
printf '%s\n' '1366x768 target raw-I420 RTP roundtrip: PASS (10 generated frames)'
if [[ -x "$target/usr/bin/remoteosctl" ]]; then
    env GST_PLUGIN_SYSTEM_PATH_1_0= GST_PLUGIN_PATH_1_0="$target/usr/lib/gstreamer-1.0" \
        GST_REGISTRY_1_0="$evidence/registry-i386.bin" GST_REGISTRY_FORK=no \
        "$loader" --library-path "$libraries" "$target/usr/bin/remoteosctl" --media-capabilities \
        > "$evidence/remoteos-capabilities.json"
fi
