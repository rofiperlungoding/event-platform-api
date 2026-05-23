#!/data/data/com.termux/files/usr/bin/bash
# Daily replication: Postgres (tablet, source-of-truth) → Supabase (read-only mirror)
#
# Triggered by cron daily at 04:00. The replica provides:
#   - Read-only fallback if the tablet is offline
#   - Edge-located queries for clients far from Indonesia
#   - Disaster recovery (geographic redundancy)
#
# Strategy:
#   - pg_dump --data-only --no-owner --no-acl from local Postgres
#   - TRUNCATE replica tables (FK cascade); reset sequences
#   - psql import into Supabase
#   - Insert audit row into _ReplicationStatus
#
# Configuration via $HOME/.replication.env:
#   SUPABASE_HOST              e.g. db.<ref>.supabase.co
#   SUPABASE_PORT              5432 (default) or 6543 (pooler)
#   SUPABASE_DB                postgres (default)
#   SUPABASE_USER              postgres (default)
#   SUPABASE_PASSWORD          (from Supabase project settings)

trap - CHLD
set -euo pipefail

# Round 7 fix (audit item 236): unset PGPASSWORD on every exit
# path, including failure. Without the trap, an early exit (e.g.,
# pg_dump failing) would leave PGPASSWORD set in the parent shell
# environment for any subsequent commands the operator runs.
trap 'unset PGPASSWORD 2>/dev/null || true; rm -f "${DUMP:-}" 2>/dev/null || true' EXIT

LOG="$HOME/replication.log"
ENV_FILE="$HOME/.replication.env"

if [ ! -f "$ENV_FILE" ]; then
    echo "[$(date)] ERROR: $ENV_FILE not found. Aborting." >> "$LOG"
    exit 1
fi
. "$ENV_FILE"

: "${SUPABASE_HOST:?missing SUPABASE_HOST}"
: "${SUPABASE_PASSWORD:?missing SUPABASE_PASSWORD}"
: "${SUPABASE_PORT:=5432}"
: "${SUPABASE_DB:=postgres}"
: "${SUPABASE_USER:=postgres}"

START=$(date +%s)
SOURCE_HOST="$(hostname || echo tablet)"
DUMP="$HOME/replication-dump.sql"

echo "[$(date)] === REPLICATION START ===" >> "$LOG"

# 1. Dump local data (data only, no DDL — schema must already exist on replica)
pg_dump -h localhost -U rofi -d eventplatform \
    --data-only --no-owner --no-acl \
    --table='"Event"' \
    --table='"Participant"' \
    --table='"Session"' \
    --table='"Attendance"' \
    --table='"Device"' \
    > "$DUMP" 2>> "$LOG"

DUMP_SIZE=$(stat -c%s "$DUMP" 2>/dev/null || stat -f%z "$DUMP" 2>/dev/null || echo 0)
echo "[$(date)] Dump complete ($DUMP_SIZE bytes)" >> "$LOG"

# 2. Apply to replica with TRUNCATE first
export PGPASSWORD="$SUPABASE_PASSWORD"
PSQL="psql -h $SUPABASE_HOST -p $SUPABASE_PORT -U $SUPABASE_USER -d $SUPABASE_DB -v ON_ERROR_STOP=1"

# Truncate in dependency order (child tables first)
$PSQL -c 'TRUNCATE "Attendance", "Device", "Session", "Participant", "Event" RESTART IDENTITY CASCADE;' >> "$LOG" 2>&1

# Apply dump
$PSQL -f "$DUMP" >> "$LOG" 2>&1

# Count rows pushed (sum from local source)
ROWS=$(psql -h localhost -U rofi -d eventplatform -tAc \
    "SELECT (SELECT COUNT(*) FROM \"Event\") + \
            (SELECT COUNT(*) FROM \"Participant\") + \
            (SELECT COUNT(*) FROM \"Session\") + \
            (SELECT COUNT(*) FROM \"Attendance\") + \
            (SELECT COUNT(*) FROM \"Device\")")

DURATION=$(( ($(date +%s) - START) * 1000 ))

# 3. Audit entry on replica
$PSQL -c "INSERT INTO \"_ReplicationStatus\" (source_host, last_sync_at, rows_synced, duration_ms, status, notes) \
    VALUES ('$SOURCE_HOST', NOW(), $ROWS, $DURATION, 'success', 'Daily cron')" >> "$LOG" 2>&1

unset PGPASSWORD
rm -f "$DUMP"

echo "[$(date)] ✓ Replication complete: $ROWS rows in ${DURATION}ms" >> "$LOG"
