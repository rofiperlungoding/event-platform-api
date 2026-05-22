#!/data/data/com.termux/files/usr/bin/bash
# Auto-deploy script for event-platform-api
# Triggered by GitHub webhook → C server → this script
#
# DEFENSE-IN-DEPTH:
#   1. git pull
#   2. compile to staging name
#   3. smoke test (run binary on test port, curl /health)
#   4. ONLY if healthy: rotate binaries (keep last 5) + atomic swap + restart
#   5. If anything fails: keep current binary running

trap - CHLD
set -e

REPO_DIR="$HOME/projects/event-platform-api"
BUILD_DIR="$HOME/projects/event-platform-zig"
BIN_DIR="$HOME/projects"
ARCHIVE="$HOME/projects/event-server-archive"
LOG="$HOME/deploy-api.log"

mkdir -p "$ARCHIVE"

echo "[$(date)] === API DEPLOY START ===" >> "$LOG"

cd "$REPO_DIR"

# 1. Pull
echo "[$(date)] Pulling..." >> "$LOG"
git fetch origin >> "$LOG" 2>&1
git reset --hard origin/main >> "$LOG" 2>&1
COMMIT=$(git rev-parse --short HEAD)
echo "[$(date)] On commit: $COMMIT" >> "$LOG"

# 2. Compile to staging
cp server.c "$BUILD_DIR/server.c"
cd "$BUILD_DIR"
echo "[$(date)] Compiling..." >> "$LOG"
if ! cc -O2 -o event-server-staging server.c \
    -I/data/data/com.termux/files/usr/include \
    -L/data/data/com.termux/files/usr/lib \
    -lpq >> "$LOG" 2>&1; then
    echo "[$(date)] ❌ COMPILE FAILED — keeping old binary" >> "$LOG"
    rm -f event-server-staging
    exit 1
fi
echo "[$(date)] ✓ Compile OK" >> "$LOG"

# 3. Smoke test on test port (3099)
echo "[$(date)] Smoke testing on port 3099..." >> "$LOG"
PORT=3099 \
DATABASE_URL="postgresql://rofi:devsecret@localhost:5432/eventplatform" \
STATIC_DIR="$HOME/projects/event-platform-console" \
JWT_SECRET="${JWT_SECRET:-intrivia2026secret}" \
WEBHOOK_SECRET="${WEBHOOK_SECRET:-intriviadeploy2026}" \
nohup ./event-server-staging > /dev/null 2>&1 &
TEST_PID=$!
sleep 2

if curl -sf http://localhost:3099/health > /dev/null 2>&1; then
    echo "[$(date)] ✓ Smoke test passed" >> "$LOG"
    kill -9 $TEST_PID 2>/dev/null || true
    sleep 1
else
    echo "[$(date)] ❌ SMOKE TEST FAILED — keeping old binary" >> "$LOG"
    kill -9 $TEST_PID 2>/dev/null || true
    rm -f event-server-staging
    exit 1
fi

# 4. Archive current binary (rotate, keep last 5)
# Note: cp may fail with "Text file busy" because the running binary cannot
# be overwritten. We use a workaround: cp via a temporary path.
if [ -f "$BIN_DIR/event-server" ]; then
    ARCHIVE_NAME="$ARCHIVE/event-server-$(date +%Y%m%d_%H%M%S)"
    # Read the file content (cat) instead of cp — avoids "text file busy"
    cat "$BIN_DIR/event-server" > "$ARCHIVE_NAME" 2>/dev/null || true
    chmod +x "$ARCHIVE_NAME" 2>/dev/null || true
    echo "[$(date)] Archived current to $ARCHIVE_NAME" >> "$LOG"
fi
# Rotate: keep only last 5
ls -t "$ARCHIVE"/event-server-* 2>/dev/null | tail -n +6 | xargs -r rm -f 2>/dev/null || true

# 5. Atomic swap + restart
# pm2 must stop first to release the binary file lock on Termux
pm2 stop event-server >> "$LOG" 2>&1
sleep 2
mv event-server-staging "$BIN_DIR/event-server"
echo "[$(date)] Atomic swap done" >> "$LOG"

pm2 restart event-server --update-env >> "$LOG" 2>&1
sleep 3

# Final health check on real port
PORT_RUNNING=$(pm2 jlist 2>/dev/null | grep -o '"PORT":"[0-9]*"' | head -1 | grep -o '[0-9]*')
PORT_RUNNING=${PORT_RUNNING:-3001}
if curl -sf "http://localhost:$PORT_RUNNING/health" > /dev/null 2>&1; then
    echo "[$(date)] ✓✓✓ DEPLOY SUCCESS ($COMMIT) — server healthy on port $PORT_RUNNING ✓✓✓" >> "$LOG"
else
    echo "[$(date)] ⚠️ Server unhealthy after deploy — consider rollback" >> "$LOG"
fi
