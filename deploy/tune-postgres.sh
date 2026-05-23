#!/data/data/com.termux/files/usr/bin/bash
# One-time PostgreSQL tuning for the event-platform workload on a 3 GB
# Android tablet. Idempotent — safe to re-run.
#
# Tunes:
#   max_connections        100 -> 200   (worker pool + WS forks + admin)
#   shared_buffers         128MB -> 256MB
#   effective_cache_size   4GB -> 1GB    (honest about tablet RAM)
#   work_mem               4MB -> 8MB
#   synchronous_commit     on -> off     (loses last <0.5 s on crash; we
#                                        accept that for write throughput
#                                        and to extend eMMC life)
#   wal_compression        off -> on     (smaller WAL writes -> less I/O)
#   wal_writer_delay       200ms -> 1000ms (batch the WAL flushes)
#   autovacuum_naptime     1min -> 30s   (faster cleanup of dead tuples)

set -e
CONF="$PREFIX/var/lib/postgresql/postgresql.conf"

if [ ! -f "$CONF" ]; then
    echo "postgresql.conf not found at $CONF"; exit 1
fi

# Backup once
if [ ! -f "$CONF.bak" ]; then
    cp "$CONF" "$CONF.bak"
    echo "[$(date)] backed up postgresql.conf -> postgresql.conf.bak"
fi

set_param() {
    local k="$1" v="$2"
    if grep -qE "^[#[:space:]]*$k[[:space:]]*=" "$CONF"; then
        sed -i -E "s|^[#[:space:]]*$k[[:space:]]*=.*|$k = $v|" "$CONF"
    else
        echo "$k = $v" >> "$CONF"
    fi
}

set_param max_connections        200
set_param shared_buffers         '256MB'
set_param effective_cache_size   '1GB'
set_param work_mem               '8MB'
set_param maintenance_work_mem   '64MB'
set_param synchronous_commit     off
set_param wal_compression        on
set_param wal_writer_delay       '1000ms'
set_param autovacuum_naptime     '30s'
set_param checkpoint_completion_target  '0.9'
# WAL recycling — bound the pg_wal directory so it cannot grow without
# limit during a long running event with many transactions.
set_param max_wal_size           '512MB'
set_param min_wal_size           '80MB'
set_param checkpoint_timeout     '15min'

echo "[$(date)] postgresql.conf updated. Restart Postgres for changes:"
echo "  pg_ctl -D \$PREFIX/var/lib/postgresql -l \$HOME/postgres.log restart"
