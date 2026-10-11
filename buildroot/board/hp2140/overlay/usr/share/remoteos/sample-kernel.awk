# Lightweight cumulative kernel counters. Missing files leave empty fields.
# UDP and softnet counters describe the whole client, not just RemoteOS RTP.
function hexadecimal(value, result, digit, i) {
    result = 0
    for (i = 1; i <= length(value); i++) {
        digit = index("0123456789abcdef", tolower(substr(value, i, 1))) - 1
        if (digit < 0) return 0
        result = result * 16 + digit
    }
    return result
}
FILENAME ~ /\/snmp$/ && $1 == "Udp:" {
    if ($2 == "InDatagrams") {
        for (i = 2; i <= NF; i++) udp_header[i] = $i
    } else if (udp_header[2] == "InDatagrams") {
        for (i = 2; i <= NF; i++) udp[udp_header[i]] = $i
    }
}
FILENAME ~ /\/softnet_stat$/ && NF >= 3 {
    softnet_seen = 1
    softnet_dropped += hexadecimal($2)
    softnet_squeezed += hexadecimal($3)
}
FILENAME ~ /\/statistics\/rx_dropped$/ { rx_seen = 1; rx_dropped += $1 }
FILENAME ~ /\/statistics\/rx_errors$/ { rx_error_seen = 1; rx_errors += $1 }
FILENAME ~ /\/scaling_cur_freq$/ && $1 ~ /^[0-9]+$/ {
    if (!frequency_seen || $1 < minimum_frequency) minimum_frequency = $1
    if (!frequency_seen || $1 > maximum_frequency) maximum_frequency = $1
    frequency_seen = 1
}
END {
    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s", udp["InDatagrams"], udp["InErrors"], udp["RcvbufErrors"], udp["SndbufErrors"],
        rx_seen ? sprintf("%.0f", rx_dropped) : "", rx_error_seen ? sprintf("%.0f", rx_errors) : "",
        softnet_seen ? sprintf("%.0f", softnet_dropped) : "", softnet_seen ? sprintf("%.0f", softnet_squeezed) : "",
        frequency_seen ? sprintf("%.0f", minimum_frequency) : "", frequency_seen ? sprintf("%.0f", maximum_frequency) : ""
}
