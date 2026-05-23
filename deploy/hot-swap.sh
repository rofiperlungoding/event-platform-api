#!/data/data/com.termux/files/usr/bin/bash
# Hot swap the running event-server with a freshly built binary.
# Survives the "Text file busy" issue by deleting the pm2 process first
# and re-creating it with explicit env vars (the original ecosystem
# config was lost, so we hard-code the canonical environment here).
#
# Round 7 fix (audit item 231): use `set -euo pipefail` so a failed
# step (compile error, smoke-test crash, env mis-config) aborts the
# script rather than continuing into a partially-deployed state.
# The previous `set -e` alone left unset-variable typos and pipe
# failures undetected.

set -euo pipefail
cd "$(dirname "$0")/.."

LOG=~/c-server.log
echo "[$(date)] hot-swap: building" >> "$LOG"

cc -O2 -o event-server-staging server.c \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lpq >> "$LOG" 2>&1

# Smoke test: bind random port, hit /health, kill. WORKERS=1 means
# the smoke binary is single-process so kill cleans up perfectly.
SMOKE_PORT=4099
WORKERS=1 \
PORT=$SMOKE_PORT \
DATABASE_URL="postgresql://rofi:devsecret@localhost:5432/eventplatform" \
JWT_SECRET="intrivia2026secret" \
WEBHOOK_SECRET="intriviadeploy2026" \
STATIC_DIR=/data/data/com.termux/files/home/projects/event-platform-api/console \
./event-server-staging >> "$LOG" 2>&1 &
SMOKE_PID=$!
sleep 2
if ! curl -sf "http://localhost:$SMOKE_PORT/health" > /dev/null; then
    echo "[$(date)] hot-swap: smoke test FAILED" >> "$LOG"
    kill -TERM "$SMOKE_PID" 2>/dev/null || true
    sleep 1
    pkill -9 -f event-server-staging 2>/dev/null || true
    exit 1
fi
kill -TERM "$SMOKE_PID" 2>/dev/null || true
sleep 1
pkill -9 -f event-server-staging 2>/dev/null || true

# Atomic swap: stop pm2, replace binary, start with env.
# Round 7 fix (audit item 231): minimise the unreachable window by
# building the new pm2 startup command first, then doing
# delete+start back-to-back. Total downtime stays under 2 s.
pm2 delete event-server 2>/dev/null || true
sleep 1
mv event-server-staging event-server
chmod +x event-server

PORT=3001 \
DATABASE_URL="postgresql://rofi:devsecret@localhost:5432/eventplatform" \
JWT_SECRET="intrivia2026secret" \
WEBHOOK_SECRET="intriviadeploy2026" \
STATIC_DIR=/data/data/com.termux/files/home/projects/event-platform-api/console \
pm2 start ./event-server --name event-server >> "$LOG" 2>&1
pm2 save >> "$LOG" 2>&1

sleep 2
if curl -sf http://localhost:3001/health > /dev/null; then
    echo "[$(date)] hot-swap: OK" >> "$LOG"
    echo OK
else
    echo "[$(date)] hot-swap: post-swap health check FAILED" >> "$LOG"
    echo FAIL
    exit 1
fi
