#!/data/data/com.termux/files/usr/bin/bash
# Find and kill zombie event-server processes
echo "=== Looking for event-server processes ==="
FOUND=0
for f in /proc/[0-9]*/comm; do
    if [ -r "$f" ]; then
        c=$(cat "$f" 2>/dev/null)
        if [ "$c" = "event-server" ]; then
            pid=$(echo "$f" | sed 's|/proc/||;s|/comm||')
            echo "Found PID $pid (comm=$c)"
            kill -9 "$pid" 2>/dev/null && echo "  → killed"
            FOUND=$((FOUND+1))
        fi
    fi
done
echo "Total zombies killed: $FOUND"

sleep 2
echo ""
echo "=== Restart pm2 event-server ==="
pm2 restart event-server 2>&1 | tail -3
sleep 4
pm2 list --no-color | head -10
echo ""
echo "=== Health check ==="
curl -sf http://localhost:3000/health || echo "STILL DEAD"
