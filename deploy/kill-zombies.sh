#!/data/data/com.termux/files/usr/bin/bash
# Kill leftover event-server processes that are holding port 3000
trap - CHLD

LOG="$HOME/projects/event-platform-console/kill-log.txt"

{
    echo "=== Before kill ==="
    for p in $(ls /proc 2>/dev/null | grep '^[0-9]'); do
        if [ -r "/proc/$p/comm" ]; then
            COMM=$(cat "/proc/$p/comm" 2>/dev/null)
            if echo "$COMM" | grep -q "event-server"; then
                echo "Found PID $p: $COMM"
                kill -9 "$p" 2>/dev/null && echo "  killed"
            fi
        fi
    done
    echo ""
    echo "=== Restarting via pm2 ==="
    pm2 stop event-server 2>&1 | tail -3
    sleep 2
    pm2 restart event-server 2>&1 | tail -3
    sleep 3
    echo ""
    echo "=== Status ==="
    pm2 list --no-color | head -10
    echo ""
    echo "=== Health ==="
    curl -sf http://localhost:3000/health 2>&1 || echo "DEAD"
} > "$LOG" 2>&1
chmod 644 "$LOG"
