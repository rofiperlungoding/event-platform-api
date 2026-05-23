#!/data/data/com.termux/files/usr/bin/bash
# Health watchdog — runs every 5 minutes via cron.
# If the API is unreachable, the watchdog will:
#   1. Attempt a pm2 restart.
#   2. If still unhealthy, trigger automatic rollback.
#   3. Optionally push a notification to a webhook URL.
#
# Configuration (via $HOME/.watchdog.env, sourced if present):
#   ALERT_WEBHOOK_URL   — optional URL to POST alerts to (e.g., Discord webhook)
#   ALERT_WEBHOOK_TYPE  — "discord" | "slack" | "generic" (default: generic)

trap - CHLD

LOG="$HOME/watchdog.log"
PORT=3001
[ -f "$HOME/.watchdog.env" ] && . "$HOME/.watchdog.env"

# ─── Quick happy path ────────────────────────────────────────────────────
# Round 7 fix (audit item 234): probe /health/ready rather than /health.
# /health returns 200 as long as the process is alive — even if the
# database is down. /health/ready returns 503 when the worker cannot
# serve a real request, which is what we actually want a watchdog to
# notice.
if curl -sf "http://localhost:$PORT/health/ready" > /dev/null 2>&1; then
    exit 0
fi

echo "[$(date)] WARN: Health probe failed on port $PORT" >> "$LOG"

send_alert() {
    local severity="$1"
    local message="$2"
    [ -z "$ALERT_WEBHOOK_URL" ] && return 0
    case "${ALERT_WEBHOOK_TYPE:-generic}" in
        discord)
            local payload="{\"content\":\"**[$severity]** Event Platform API: $message\"}"
            ;;
        slack)
            local payload="{\"text\":\"*[$severity]* Event Platform API: $message\"}"
            ;;
        *)
            local payload="{\"severity\":\"$severity\",\"service\":\"event-platform-api\",\"message\":\"$message\",\"timestamp\":\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\"}"
            ;;
    esac
    curl -sf -X POST "$ALERT_WEBHOOK_URL" \
        -H "Content-Type: application/json" \
        -d "$payload" > /dev/null 2>&1 || true
}

# ─── Recovery attempt 1: pm2 restart ─────────────────────────────────────
pm2 restart event-server --update-env > /dev/null 2>&1
sleep 5

if curl -sf "http://localhost:$PORT/health" > /dev/null 2>&1; then
    echo "[$(date)] INFO: Restart restored healthy state" >> "$LOG"
    send_alert "INFO" "Service was unhealthy and recovered after pm2 restart"
    exit 0
fi

# ─── Recovery attempt 2: rollback ────────────────────────────────────────
echo "[$(date)] CRIT: Restart did not restore health. Triggering rollback." >> "$LOG"
send_alert "CRIT" "Auto-rollback initiated after restart failed"

bash "$HOME/projects/event-platform-api/deploy/rollback.sh"
sleep 5

if curl -sf "http://localhost:$PORT/health" > /dev/null 2>&1; then
    echo "[$(date)] INFO: Rollback restored healthy state" >> "$LOG"
    send_alert "INFO" "Rollback succeeded; service restored"
else
    echo "[$(date)] FATAL: Even rollback did not restore health. Manual intervention required." >> "$LOG"
    send_alert "FATAL" "Service down and rollback failed. Manual intervention required."
fi
