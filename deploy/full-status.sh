#!/data/data/com.termux/files/usr/bin/bash
# Comprehensive status report for verifying Round 1 setup
trap - CHLD

OUT="$HOME/projects/event-platform-api/console/full-status.json"

# Build JSON manually
cat > "$OUT" <<EOF
{
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "system": {
    "uptime": "$(uptime -p 2>/dev/null || echo unknown)",
    "tab_ip": "$(ifconfig 2>/dev/null | grep -E 'inet [0-9]' | grep -v 127.0.0.1 | awk '{print $2}' | head -1)"
  },
  "boot_script": {
    "installed": $([ -f $HOME/.termux/boot/start-event-stack ] && echo true || echo false),
    "executable": $([ -x $HOME/.termux/boot/start-event-stack ] && echo true || echo false),
    "size_bytes": $(stat -c %s $HOME/.termux/boot/start-event-stack 2>/dev/null || echo 0)
  },
  "cron": {
    "daemon_running": $(pgrep crond > /dev/null && echo true || echo false),
    "daemon_pid": "$(pgrep crond | head -1)",
    "jobs_count": $(crontab -l 2>/dev/null | grep -c '^[^#]'),
    "jobs": "$(crontab -l 2>/dev/null | grep -v '^#' | tr '\n' ';')"
  },
  "backups": {
    "count": $(ls $HOME/backups/ 2>/dev/null | wc -l),
    "latest": "$(ls -t $HOME/backups/ 2>/dev/null | head -1)",
    "total_bytes": $(du -bc $HOME/backups/ 2>/dev/null | tail -1 | awk '{print $1}' || echo 0)
  },
  "services": {
    "postgres_running": $(pg_ctl -D $PREFIX/var/lib/postgresql status > /dev/null 2>&1 && echo true || echo false),
    "sshd_running": $(pgrep sshd > /dev/null && echo true || echo false),
    "pm2_processes": $(pm2 jlist 2>/dev/null | grep -o '"name":"[^"]*"' | wc -l)
  },
  "logs": {
    "boot_log_lines": $(wc -l < $HOME/boot.log 2>/dev/null || echo 0),
    "backup_log_tail": "$(tail -1 $HOME/backup.log 2>/dev/null | sed 's/"/\\"/g')",
    "cron_log_tail": "$(tail -1 $HOME/cron.log 2>/dev/null | sed 's/"/\\"/g')"
  }
}
EOF
chmod 644 "$OUT"
