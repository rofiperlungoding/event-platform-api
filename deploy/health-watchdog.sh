#!/data/data/com.termux/files/usr/bin/bash
# Health watchdog — runs every 5 minutes via cron
# If server is down: try restart, if still down: auto-rollback

trap - CHLD

LOG="$HOME/watchdog.log"
PORT=3001

# Quick check
if curl -sf "http://localhost:$PORT/health" > /dev/null 2>&1; then
    # All good, exit silently (no log spam)
    exit 0
fi

echo "[$(date)] ⚠️ Health check FAILED on port $PORT" >> "$LOG"

# Try pm2 restart first
pm2 restart event-server --update-env > /dev/null 2>&1
sleep 5

if curl -sf "http://localhost:$PORT/health" > /dev/null 2>&1; then
    echo "[$(date)] ✓ Restart fixed it" >> "$LOG"
    exit 0
fi

echo "[$(date)] ❌ Restart didn't help. Triggering rollback." >> "$LOG"
bash "$HOME/projects/event-platform-api/deploy/rollback.sh"
