#!/data/data/com.termux/files/usr/bin/bash
# Setup cron jobs on tablet
# Run once: bash setup-cron.sh
# Idempotent — safe to re-run

trap - CHLD

# Install cron if not already
pkg install -y cronie >/dev/null 2>&1 || true

# Cron jobs (use absolute paths since cron has minimal env)
JOBS=(
  "0 3 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/db-backup.sh"
  "0 4 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/replicate-supabase.sh"
  "0 5 * * 0 /data/data/com.termux/files/usr/bin/pm2 flush"
  "*/5 * * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/health-watchdog.sh"
  "30 4 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/log-rotate.sh"
  "*/5 * * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/battery-watchdog.sh"
  "15 */6 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/time-check.sh"
  "45 5 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/cert-watchdog.sh"
  "0 * * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/auth-cleanup.sh"
)

# Get existing crontab
EXISTING=$(crontab -l 2>/dev/null || echo "")

# Build new crontab: existing lines that aren't being replaced + new jobs
NEW_CRON="$EXISTING"
for JOB in "${JOBS[@]}"; do
    # Extract command part for matching
    CMD=$(echo "$JOB" | awk '{$1=$2=$3=$4=$5=""; print substr($0,6)}')
    # Remove existing line with same command (avoid dupes)
    NEW_CRON=$(echo "$NEW_CRON" | grep -vF "$CMD" || true)
    NEW_CRON="$NEW_CRON
$JOB"
done

echo "$NEW_CRON" | grep -v '^$' | crontab -

# Start crond if not running (background, no -L flag for cronie)
pgrep crond > /dev/null || nohup crond < /dev/null > $HOME/cron.log 2>&1 &
sleep 1

echo ""
echo "Cron daemon: $(pgrep -af crond | head -1 || echo NOT RUNNING)"
echo ""
echo "Active cron jobs:"
crontab -l
