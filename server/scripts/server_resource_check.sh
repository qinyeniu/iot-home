#!/usr/bin/env bash
# Read-only Aliyun ECS resource check. This script never restarts, removes,
# prunes, writes product configuration, or reads/prints secrets.
set -u

WARN_DISK_PERCENT=${WARN_DISK_PERCENT:-80}
CRIT_DISK_PERCENT=${CRIT_DISK_PERCENT:-90}
WARN_MEM_AVAIL_MB=${WARN_MEM_AVAIL_MB:-250}
CRIT_MEM_AVAIL_MB=${CRIT_MEM_AVAIL_MB:-150}

exit_code=0

section() {
    printf '\n===== %s =====\n' "$1"
}

note() {
    printf -- '- %s\n' "$1"
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

set_exit_code() {
    if [ "$1" -gt "$exit_code" ]; then
        exit_code=$1
    fi
}

for value_name in WARN_DISK_PERCENT CRIT_DISK_PERCENT WARN_MEM_AVAIL_MB CRIT_MEM_AVAIL_MB; do
    eval "value=\${$value_name}"
    if ! is_uint "$value"; then
        printf 'ERROR: %s must be a non-negative integer\n' "$value_name" >&2
        exit 2
    fi
done

if [ "$WARN_DISK_PERCENT" -ge "$CRIT_DISK_PERCENT" ]; then
    echo "ERROR: WARN_DISK_PERCENT must be below CRIT_DISK_PERCENT" >&2
    exit 2
fi
if [ "$CRIT_MEM_AVAIL_MB" -ge "$WARN_MEM_AVAIL_MB" ]; then
    echo "ERROR: CRIT_MEM_AVAIL_MB must be below WARN_MEM_AVAIL_MB" >&2
    exit 2
fi

section "Host"
date '+%Y-%m-%d %H:%M:%S %Z'
uptime || true
[ -r /etc/os-release ] && grep -E '^(PRETTY_NAME)=' /etc/os-release || true

section "Memory"
free -m || true
mem_available_kb=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo)
if [ -n "${mem_available_kb:-}" ]; then
    mem_available_mb=$((mem_available_kb / 1024))
    note "MemAvailable: ${mem_available_mb} MB"
    if [ "$mem_available_mb" -lt "$CRIT_MEM_AVAIL_MB" ]; then
        note "CRITICAL: available memory is below ${CRIT_MEM_AVAIL_MB} MB"
        set_exit_code 2
    elif [ "$mem_available_mb" -lt "$WARN_MEM_AVAIL_MB" ]; then
        note "WARNING: available memory is below ${WARN_MEM_AVAIL_MB} MB"
        set_exit_code 1
    fi
else
    note "WARNING: unable to read MemAvailable"
    set_exit_code 1
fi

section "Disk"
df -h -x tmpfs -x devtmpfs -x squashfs || true
while IFS= read -r line; do
    filesystem=$(printf '%s' "$line" | awk '{print $1}')
    usage=$(printf '%s' "$line" | awk '{print $5}' | tr -d '%')
    mountpoint=$(printf '%s' "$line" | awk '{print $6}')
    is_uint "$usage" || continue
    if [ "$usage" -ge "$CRIT_DISK_PERCENT" ]; then
        note "CRITICAL: ${filesystem} on ${mountpoint} is ${usage}% used"
        set_exit_code 2
    elif [ "$usage" -ge "$WARN_DISK_PERCENT" ]; then
        note "WARNING: ${filesystem} on ${mountpoint} is ${usage}% used"
        set_exit_code 1
    fi
done < <(df -P -x tmpfs -x devtmpfs -x squashfs | tail -n +2)

section "Load and CPU"
nproc || true
cat /proc/loadavg || true

if command -v docker >/dev/null 2>&1; then
    section "Docker containers"
    ps_status=0
    ps_output=$(docker ps -q 2>&1) || ps_status=$?

    if [ "$ps_status" -ne 0 ]; then
        note "WARNING: unable to query Docker containers; container state is unknown"
        note "$ps_output"
        set_exit_code 1
    elif [ -z "$ps_output" ]; then
        note "No running Docker containers"
    else
        mapfile -t container_ids <<< "$ps_output"
        for cid in "${container_ids[@]}"; do
            [ -n "$cid" ] || continue
            docker inspect --format='- {{.Name}} state={{.State.Status}} restarts={{.RestartCount}} health={{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' "$cid" || true
        done

        section "Docker live resource usage"
        docker stats --no-stream --format 'table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.MemPerc}}' || true
    fi

    section "Docker disk usage"
    docker system df || true

    section "Largest container JSON logs"
    if [ -r /var/lib/docker/containers ]; then
        log_status=0
        log_find_output=$(find /var/lib/docker/containers -type f -name '*.log' -printf '%s\t%p\n' 2>&1) || log_status=$?
        log_list=$(printf '%s\n' "$log_find_output" | sort -nr | head -n 5)
        if [ "$log_status" -ne 0 ]; then
            note "WARNING: unable to enumerate container JSON logs"
            note "$log_list"
            set_exit_code 1
        elif [ -z "$log_list" ]; then
            note "No readable container JSON logs found"
        else
            while IFS=$'\t' read -r size path; do
                [ -n "$size" ] || continue
                awk -v size="$size" -v path="$path" 'BEGIN { printf "- %.1f MiB %s\n", size / 1024 / 1024, path }'
            done <<< "$log_list"
        fi
    else
        note "WARNING: no read access to /var/lib/docker/containers; run as root/sudo or grant access"
        set_exit_code 1
    fi
else
    note "Docker command not found"
fi

section "Safety"
case "$exit_code" in
    0) note "PASS: no configured resource threshold was exceeded" ;;
    1) note "WARNING: review resource usage before maintenance" ;;
    2) note "CRITICAL: do not start additional resource-heavy work" ;;
esac
printf '\nCheck complete. Exit code: %s\n' "$exit_code"
exit "$exit_code"