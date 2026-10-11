# Strict, bounded parser for the client metrics CSV. Caller caps input at 4097 B.
BEGIN {
    FS = ","
    expected = "uptime_seconds,connected,internal_codec,internal_width,internal_height,internal_target_fps,internal_fps,vga_codec,vga_width,vga_height,vga_target_fps,vga_fps,control_rtt_ms,rtp_loss_fraction,queue_ms,usb_exported"
    extended = expected ",internal_bitrate_bps,internal_decoded_fps,internal_decode_ms,internal_receiver_timeline_delay_ms,internal_presentation_drop_fraction,internal_recovery_drop_fraction,internal_receive_mbps,internal_compressed_queue_drops,vga_bitrate_bps,vga_decoded_fps,vga_decode_ms,vga_receiver_timeline_delay_ms,vga_presentation_drop_fraction,vga_recovery_drop_fraction,vga_receive_mbps,vga_compressed_queue_drops"
    valid = 1
}
{ bytes += length($0) + 1 }
NR == 1 {
    if ($0 == extended) fields = 32
    else if ($0 == expected) fields = 16
    else valid = 0
    next
}
NR == 2 {
    if (NF != fields) valid = 0
    for (i = 1; i <= NF; i++) {
        if (i == 3 || i == 8) {
            if ($i != "mpeg2" && $i != "raw-i420" && $i != "none" && $i != "") valid = 0
        } else if ($i !~ /^[0-9]+([.][0-9]+)?$/) valid = 0
    }
    if ($1 > now || now - $1 > 5 || $2 > 1 || $14 > 1) valid = 0
    if (fields == 32 && ($21 > 1 || $22 > 1 || $29 > 1 || $30 > 1)) valid = 0
    for (i = 1; i <= 16; i++) basic = basic (i == 1 ? "" : ",") $i
    for (i = 17; i <= 32; i++) stages = stages (i == 17 ? "" : ",") $i
}
NR > 2 { valid = 0 }
END {
    if (valid && NR == 2 && bytes <= 4096 && (section != "stages" || fields == 32))
        print "1," (section == "stages" ? stages : basic)
    else { printf "0"; for (i = 1; i <= 16; i++) printf ","; printf "\n" }
}
