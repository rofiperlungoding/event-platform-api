#!/data/data/com.termux/files/usr/bin/bash
# Auto-start script — runs on tablet boot via Termux:Boot
# Place at ~/.termux/boot/start-event-stack and chmod +x
#
# Round 10 hardening:
#   - `set -uo pipefail` (deliberately not -e because we want to keep
#     trying every component even if one fails; the watchdog will
#     escalate from there).
#   - Idempotent guards on every step (Postgres, sshd, cron) so a
#     re-run does not produce a fork bomb of duplicate daemons.
#   - Postgres readiness probe replaces the arbitrary `sleep 4`.

set -uo pipefail

LOG="$HOME/boot.log"

# Wake-lock so screen-off doesn't kill processes
termux-wake-lock 2>>"$LOG" || true

# Wait for system to stabilize (Android InitJob warm-up)
sleep 8

# Start PostgreSQL only if it isn't running already (audit item 271)
if ! pg_ctl -D "$PREFIX/var/lib/postgresql" status > /dev/null 2>&1; then
    pg_ctl -D "$PREFIX/var/lib/postgresql" -l "$PREFIX/var/lib/postgresql/logfile" start >> "$LOG" 2>&1 || true

    # Readiness probe (audit item 271) — wait up to 20 s for socket to accept
    for i in 1 2 3 4 5 6 7 8 9 10; do
        if pg_isready -h 127.0.0.1 -q 2>/dev/null; then break; fi
        sleep 2
    done
fi

# Start SSH server (so we can recover remotely if pm2 fails)
# Audit item 274: only restart if not already healthy.
if ! pgrep -x sshd > /dev/null 2>&1; then
    sshd >> "$LOG" 2>&1 || true
fi

# Start cron daemon (for watchdog + backups)
pgrep crond > /dev/null || nohup crond < /dev/null > "$HOME/cron.log" 2>&1 &

# Resurrect pm2 processes (event-server + event-tunnel).
# Audit item 273: pm2 daemon may not be ready immediately after boot;
# retry up to 5 times with a 2-second delay.
for i in 1 2 3 4 5; do
    if pm2 resurrect >> "$LOG" 2>&1; then break; fi
    sleep 2
done

# Wait for boot to settle, then verify health via /health/ready (which
# checks DB reachability rather than just process liveness — audit
# item 234 from round 7 applied here too).
sleep 10
if curl -sf http://localhost:3001/health/ready > /dev/null 2>&1; then
    echo "[$(date)] Boot stack started: HEALTHY" >> "$LOG"
else
    echo "[$(date)] Boot stack started: UNHEALTHY — watchdog will retry" >> "$LOG"
fi
