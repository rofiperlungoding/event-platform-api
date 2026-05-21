#!/data/data/com.termux/files/usr/bin/bash
# Debug cron environment
trap - CHLD

LOG="$HOME/projects/event-platform-console/cron-debug.txt"

{
    echo "=== Cron debug ==="
    echo "PATH: $PATH"
    echo "HOME: $HOME"
    echo ""
    echo "which crond: $(which crond 2>&1)"
    echo "which setsid: $(which setsid 2>&1)"
    echo ""
    echo "=== crond paths ==="
    ls -la $PREFIX/etc/cron.d/ 2>/dev/null
    ls -la $PREFIX/var/spool/cron/ 2>/dev/null
    echo ""
    echo "=== try start crond manually ==="
    crond -f -L /tmp/cron-test.log &
    PID=$!
    sleep 2
    if kill -0 $PID 2>/dev/null; then
        echo "crond running with pid $PID"
        kill $PID
    else
        echo "crond died"
        cat /tmp/cron-test.log 2>/dev/null
    fi
    echo ""
    echo "=== nohup approach ==="
    nohup crond -L $HOME/cron.log >/dev/null 2>&1 &
    sleep 2
    pgrep -af crond
} > "$LOG" 2>&1
chmod 644 "$LOG"
