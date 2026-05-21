#!/data/data/com.termux/files/usr/bin/bash
# Rollback to previous binary
# Triggered manually via webhook: POST /deploy/run/rollback?secret=...
# Or automatically by health-watchdog if server unhealthy

trap - CHLD

ARCHIVE="$HOME/projects/event-server-archive"
BIN_DIR="$HOME/projects"
LOG="$HOME/rollback.log"

echo "[$(date)] === ROLLBACK START ===" >> "$LOG"

# Find latest archived binary (excluding current)
LATEST=$(ls -t "$ARCHIVE"/event-server-* 2>/dev/null | head -1)

if [ -z "$LATEST" ]; then
    echo "[$(date)] ❌ No archived binary to rollback to" >> "$LOG"
    exit 1
fi

echo "[$(date)] Rolling back to: $LATEST" >> "$LOG"

# Stash current as failed backup
if [ -f "$BIN_DIR/event-server" ]; then
    mv "$BIN_DIR/event-server" "$ARCHIVE/event-server-FAILED-$(date +%Y%m%d_%H%M%S)"
fi

# Restore previous
cp "$LATEST" "$BIN_DIR/event-server"
chmod +x "$BIN_DIR/event-server"

# Restart
pm2 restart event-server --update-env >> "$LOG" 2>&1
sleep 3

# Verify
PORT_RUNNING=3001
if curl -sf "http://localhost:$PORT_RUNNING/health" > /dev/null 2>&1; then
    echo "[$(date)] ✓ ROLLBACK SUCCESS — server healthy" >> "$LOG"
else
    echo "[$(date)] ⚠️ Even rollback unhealthy. Manual intervention needed." >> "$LOG"
fi
