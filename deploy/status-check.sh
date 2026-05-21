#!/data/data/com.termux/files/usr/bin/bash
# Write status info to a public file so we can curl it
# Run via webhook: POST /deploy/run/status-check?secret=...

trap - CHLD

OUT="$HOME/projects/event-platform-console/status.json"

cat > "$OUT" <<EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "boot_script": "$([ -f $HOME/.termux/boot/start-event-stack ] && echo installed || echo missing)",
  "cron_jobs": $(crontab -l 2>/dev/null | wc -l),
  "cron_running": "$(pgrep crond > /dev/null && echo yes || echo no)",
  "backups": $(ls $HOME/backups/ 2>/dev/null | wc -l),
  "latest_backup": "$(ls -t $HOME/backups/ 2>/dev/null | head -1)",
  "backup_size": "$(ls -la $HOME/backups/ 2>/dev/null | tail -n +2 | awk '{ sum += $5 } END { print sum }')"
}
EOF
chmod 644 "$OUT"
