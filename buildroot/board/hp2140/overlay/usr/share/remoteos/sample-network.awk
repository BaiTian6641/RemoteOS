# Caller supplies only interfaces backed by /sys/class/net/INTERFACE/device.
BEGIN {
    FS = ":"
    count = split(physical, names, " ")
    for (i = 1; i <= count; i++) devices[names[i]] = 1
}
NR > 2 && NF == 2 {
    interface = $1
    gsub(/^[ \t]+|[ \t]+$/, "", interface)
    if (interface in devices) {
        counters = $2
        gsub(/^[ \t]+|[ \t]+$/, "", counters)
        split(counters, values, /[ \t]+/)
        rx += values[1]
        tx += values[9]
    }
}
END { printf "%.0f,%.0f", rx, tx }
