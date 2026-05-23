#!/data/data/com.termux/files/usr/bin/bash
# Lightweight log rotation for Termux (logrotate package is not
# guaranteed to be present). Runs daily via cron.
#
# Policy:
#   - Anything > 10 MB is gzipped to .1.gz; older .1.gz becomes .2.gz
#     and so on up to .5.gz.
#   - Older than .5 are dropped.
#   - All log files in $HOME matching the watch list are processed.

set -e
HOME_DIR="${HOME:-/data/data/com.termux/files/home}"
MAX_SIZE_BYTES=$((10 * 1024 * 1024))   # 10 MB
KEEP=5

WATCH_LIST=(
    "$HOME_DIR/c-server.log"
    "$HOME_DIR/deploy-api.log"
    "$HOME_DIR/deploy-console.log"
    "$HOME_DIR/watchdog.log"
    "$HOME_DIR/backup.log"
    "$HOME_DIR/cron.log"
    "$HOME_DIR/postgres.log"
)

rotate_one() {
    local f="$1"
    [ -f "$f" ] || return 0
    local sz
    sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
    [ "$sz" -lt "$MAX_SIZE_BYTES" ] && return 0

    # Drop the oldest
    [ -f "${f}.${KEEP}.gz" ] && rm -f "${f}.${KEEP}.gz"

    # Shift .N.gz -> .N+1.gz
    for ((i = KEEP - 1; i >= 1; i--)); do
        [ -f "${f}.${i}.gz" ] && mv "${f}.${i}.gz" "${f}.$((i+1)).gz"
    done

    # Compress current to .1.gz, then truncate the live file in place
    gzip -c "$f" > "${f}.1.gz"
    : > "$f"

    echo "[$(date)] rotated $f (was $sz bytes)"
}

for f in "${WATCH_LIST[@]}"; do
    rotate_one "$f"
done
