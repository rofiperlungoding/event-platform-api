#!/data/data/com.termux/files/usr/bin/bash
# Write status info to a public file so we can curl it.
# Run via webhook: POST /deploy/run/status-check?secret=...
#
# Round 11 hardening: superseded by `full-status.sh` for the
# dashboard. This script is kept for the legacy webhook path
# (curl status.json from operator scripts). Hardened with the
# same JSON-escape rules so log content with backslashes or
# quotes does not corrupt the output (audit item 293).

trap - CHLD
set -uo pipefail

OUT="$HOME/projects/event-platform-api/console/status.json"

jstr() {
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
        -e ':a;N;$!ba;s/\n/\\n/g'
}

now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
boot_state=$([ -f "$HOME/.termux/boot/start-event-stack" ] && echo installed || echo missing)
cron_jobs=$(crontab -l 2>/dev/null | wc -l)
cron_running=$(pgrep crond > /dev/null && echo yes || echo no)
backup_count=$(ls "$HOME/backups/" 2>/dev/null | wc -l)
latest_backup=$(ls -t "$HOME/backups/" 2>/dev/null | head -1 | jstr)
backup_size=$(ls -la "$HOME/backups/" 2>/dev/null | tail -n +2 | awk '{ sum += $5 } END { print (sum ? sum : 0) }')

cat > "$OUT" <<EOF
{
  "timestamp": "$now",
  "boot_script": "$boot_state",
  "cron_jobs": $cron_jobs,
  "cron_running": "$cron_running",
  "backups": $backup_count,
  "latest_backup": "$latest_backup",
  "backup_size": "$backup_size"
}
EOF
chmod 644 "$OUT" 2>/dev/null || true
