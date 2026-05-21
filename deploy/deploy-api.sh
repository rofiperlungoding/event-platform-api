#!/data/data/com.termux/files/usr/bin/bash
# Auto-deploy script for event-platform-api
# Triggered by GitHub webhook → C server → this script
#
# Steps:
#   1. git pull
#   2. recompile server2.c
#   3. atomic swap binary (rename)
#   4. pm2 restart event-server

set -e

REPO_DIR="$HOME/projects/event-platform-api"
BUILD_DIR="$HOME/projects/event-platform-zig"
LOG="$HOME/deploy-api.log"

echo "[$(date)] === API DEPLOY START ===" >> "$LOG"

cd "$REPO_DIR"

# Pull latest
echo "[$(date)] Pulling..." >> "$LOG"
git fetch origin >> "$LOG" 2>&1
git reset --hard origin/main >> "$LOG" 2>&1
COMMIT=$(git rev-parse --short HEAD)
echo "[$(date)] On commit: $COMMIT" >> "$LOG"

# Copy server2.c to build dir (where libpq paths are set up)
cp server2.c "$BUILD_DIR/server2.c"
cd "$BUILD_DIR"

# Compile new version
echo "[$(date)] Compiling..." >> "$LOG"
if cc -O2 -o event-server-new server2.c \
    -I/data/data/com.termux/files/usr/include \
    -L/data/data/com.termux/files/usr/lib \
    -lpq >> "$LOG" 2>&1; then
    echo "[$(date)] Compile OK" >> "$LOG"
else
    echo "[$(date)] COMPILE FAILED — keeping old binary" >> "$LOG"
    exit 1
fi

# Atomic swap
mv event-server-new "$HOME/projects/event-server.new"
mv "$HOME/projects/event-server.new" "$HOME/projects/event-server"

# Restart
echo "[$(date)] Restarting pm2..." >> "$LOG"
pm2 restart event-server >> "$LOG" 2>&1

echo "[$(date)] === DEPLOY DONE ($COMMIT) ===" >> "$LOG"
