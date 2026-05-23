#!/data/data/com.termux/files/usr/bin/bash
# Rollback to previous binary
# Triggered manually via webhook: POST /deploy/run/rollback?secret=...
# Or automatically by health-watchdog if server unhealthy

trap - CHLD

ARCHIVE="$HOME/projects/event-server-archive"
BIN_DIR="$HOME/projects"
LOG="$HOME/rollback.log"

echo "[$(date)] === ROLLBACK START ===" >> "$LOG"

# Find latest archived binary (excluding current).
# Round 7 fix (audit item 235): the previous version blindly took
# the most recent archive. If a deploy had archived a broken
# binary moments before health-watchdog escalated to rollback, we
# would roll forward into the same broken binary. Now we scan the
# archive list newest-first and skip any entry tagged FAILED-* —
# those are deposits made by previous rollbacks and known bad.
LATEST=""
for cand in $(ls -t "$ARCHIVE"/event-server-* 2>/dev/null); do
    case "$(basename "$cand")" in
        event-server-FAILED-*) continue ;;
    esac
    LATEST="$cand"
    break
done

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
