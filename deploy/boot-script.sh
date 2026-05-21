#!/data/data/com.termux/files/usr/bin/bash
# Auto-start script — runs on tablet boot via Termux:Boot
# Place at ~/.termux/boot/start-event-stack and chmod +x

# Wake-lock so screen-off doesn't kill processes
termux-wake-lock

# Wait for system to stabilize
sleep 8

# Start PostgreSQL
pg_ctl -D $PREFIX/var/lib/postgresql -l $PREFIX/var/lib/postgresql/logfile start || true
sleep 4

# Start SSH server (so we can recover remotely if pm2 fails)
pkill sshd 2>/dev/null
sshd

# Start cron daemon (for watchdog + backups)
pgrep crond > /dev/null || nohup crond < /dev/null > $HOME/cron.log 2>&1 &

# Resurrect pm2 processes (event-server + event-tunnel)
pm2 resurrect

# Wait for boot to settle, then verify health
sleep 15
if curl -sf http://localhost:3001/health > /dev/null 2>&1; then
    echo "[$(date)] Boot stack started: HEALTHY" >> $HOME/boot.log
else
    echo "[$(date)] Boot stack started: UNHEALTHY — watchdog will retry" >> $HOME/boot.log
fi
