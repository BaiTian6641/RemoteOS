# /proc/stat utilization across logical CPUs, not software worker threads.
# Output: valid aggregate_percent busiest_logical_percent logical_count state
# Guest ticks are already part of user/nice. State is only our numeric counters.
BEGIN {
    count = split(previous, entries, " ")
    for (i = 1; i <= count; i++) {
        split(entries[i], fields, ":")
        old_total[fields[1]] = fields[2]
        old_idle[fields[1]] = fields[3]
    }
}
$1 ~ /^cpu([0-9]+)?$/ {
    total = 0
    for (i = 2; i <= 9; i++) total += $i
    idle = $5 + $6
    state = state $1 ":" sprintf("%.0f", total) ":" sprintf("%.0f", idle) " "
    if ($1 != "cpu") logical++
    if (($1 in old_total) && total > old_total[$1] && idle >= old_idle[$1]) {
        busy = 100 * (1 - (idle - old_idle[$1]) / (total - old_total[$1]))
        if (busy < 0) busy = 0
        if (busy > 100) busy = 100
        if ($1 == "cpu") { aggregate = busy; valid = 1 }
        else if (busy > busiest) busiest = busy
    }
}
END { printf "%d %.2f %.2f %d %s\n", valid, aggregate, busiest, logical, state }
