#!/data/data/com.termux/files/usr/bin/bash
# Comprehensive status report for verifying setup.
#
# Round 10 hardening:
#   - `set -uo pipefail` (no -e: we WANT to capture every component
#     even if one probe fails; the JSON should always be a valid
#     document that the dashboard can render).
#   - JSON values are escaped via a `jstr` helper so embedded
#     backslashes / quotes / newlines do not corrupt the output
#     (audit items 282, 285).
#   - `ip addr` replaces the deprecated `ifconfig` (audit item 283).

trap - CHLD
set -uo pipefail

OUT="$HOME/projects/event-platform-api/console/full-status.json"

# JSON string escaper: take stdin, emit a JSON-quoted string body
# (without surrounding quotes). Handles backslashes, quotes, control chars.
jstr() {
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' \
        -e 's/\t/\\t/g' -e 's/\r//g' \
        -e ':a;N;$!ba;s/\n/\\n/g'
}

now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
uptime_pretty=$(uptime -p 2>/dev/null | jstr || echo "")
tab_ip=$(ip -4 addr show 2>/dev/null | awk '/inet / && $2 !~ /^127\./ {split($2,a,"/"); print a[1]; exit}' \
    || ifconfig 2>/dev/null | awk '/inet [0-9]/ && $2 != "127.0.0.1" {print $2; exit}')

boot_installed=$([ -f "$HOME/.termux/boot/start-event-stack" ] && echo true || echo false)
boot_executable=$([ -x "$HOME/.termux/boot/start-event-stack" ] && echo true || echo false)
boot_size=$(stat -c %s "$HOME/.termux/boot/start-event-stack" 2>/dev/null || echo 0)

cron_running=$(pgrep crond > /dev/null && echo true || echo false)
cron_pid=$(pgrep crond | head -1 || echo "")
cron_count=$(crontab -l 2>/dev/null | grep -cE '^[^#[:space:]]' || echo 0)
cron_lines=$(crontab -l 2>/dev/null | grep -vE '^#' | jstr || echo "")

backup_count=$(ls "$HOME/backups/" 2>/dev/null | wc -l)
backup_latest=$(ls -t "$HOME/backups/" 2>/dev/null | head -1 | jstr || echo "")
backup_total_bytes=$(du -bc "$HOME/backups/" 2>/dev/null | tail -1 | awk '{print $1}' || echo 0)

pg_running=$(pg_ctl -D "$PREFIX/var/lib/postgresql" status > /dev/null 2>&1 && echo true || echo false)
sshd_running=$(pgrep -x sshd > /dev/null && echo true || echo false)
pm2_count=$(pm2 jlist 2>/dev/null | grep -o '"name":"[^"]*"' | wc -l)

boot_log_lines=$(wc -l < "$HOME/boot.log" 2>/dev/null || echo 0)
backup_log_tail=$(tail -1 "$HOME/backup.log" 2>/dev/null | jstr || echo "")
cron_log_tail=$(tail -1 "$HOME/cron.log" 2>/dev/null | jstr || echo "")

cat > "$OUT" <<EOF
{
  "timestamp": "$now",
  "system": {
    "uptime": "$uptime_pretty",
    "tab_ip": "$tab_ip"
  },
  "boot_script": {
    "installed": $boot_installed,
    "executable": $boot_executable,
    "size_bytes": $boot_size
  },
  "cron": {
    "daemon_running": $cron_running,
    "daemon_pid": "$cron_pid",
    "jobs_count": $cron_count,
    "jobs": "$cron_lines"
  },
  "backups": {
    "count": $backup_count,
    "latest": "$backup_latest",
    "total_bytes": $backup_total_bytes
  },
  "services": {
    "postgres_running": $pg_running,
    "sshd_running": $sshd_running,
    "pm2_processes": $pm2_count
  },
  "logs": {
    "boot_log_lines": $boot_log_lines,
    "backup_log_tail": "$backup_log_tail",
    "cron_log_tail": "$cron_log_tail"
  }
}
EOF
chmod 644 "$OUT" 2>/dev/null || true
