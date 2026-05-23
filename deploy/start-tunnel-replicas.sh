#!/data/data/com.termux/files/usr/bin/bash
# Start N parallel cloudflared instances against the same named tunnel.
# Cloudflare will load-balance incoming requests across all healthy
# replicas, multiplying the per-account concurrency cap by N.
#
# Cloudflare's documented behaviour: a single named tunnel can have many
# concurrent connectors; ingress is distributed via anycast. The free
# tier per-connector concurrency cap (~45 req/s observed) becomes the
# per-instance cap, so 4 instances ≈ 4× headroom.

set -e

REPLICAS=${REPLICAS:-4}
CONFIG=$HOME/.cloudflared/config.yml
LOG_DIR=$HOME/.cloudflared/logs
mkdir -p "$LOG_DIR"

# Stop any existing replica processes (does NOT touch the pm2-managed
# event-tunnel — that one stays as a reference instance).
#
# Round 7 fix (audit item 239): the previous regex matched any
# cloudflared process with `--metrics`, which would also kill a
# manually-started instance an operator was using for debugging.
# We now match on the specific replica metrics-port range (20001+)
# so debugging instances on other ports survive.
pkill -f 'cloudflared.*--metrics 127\.0\.0\.1:200[0-9][0-9]' 2>/dev/null || true
sleep 1

for i in $(seq 1 "$REPLICAS"); do
    METRICS_PORT=$((20000 + i))
    LOG="$LOG_DIR/replica-$i.log"
    nohup cloudflared tunnel \
        --config "$CONFIG" \
        --metrics "127.0.0.1:$METRICS_PORT" \
        run >> "$LOG" 2>&1 &
    echo "[replica $i] pid=$! metrics=$METRICS_PORT log=$LOG"
    disown
done

echo
echo "Started $REPLICAS replicas. Verify in a minute:"
echo "  curl https://api.rofidoesthings.site/health"
