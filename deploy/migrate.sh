#!/data/data/com.termux/files/usr/bin/bash
# Idempotent migration runner.
#
# Iterates `migrations/*.sql` in lexical order. Skips any migration
# whose id is already in `_migrations`. The numeric prefix in each
# filename (`NNN_`) serves as the id. The `000_meta.sql` migration
# bootstraps the tracking table itself and must succeed before any
# other migration runs.

set -euo pipefail

REPO_DIR="${REPO_DIR:-$HOME/projects/event-platform-api}"
MIG_DIR="$REPO_DIR/migrations"
DB_HOST="${DB_HOST:-127.0.0.1}"
DB_USER="${DB_USER:-rofi}"
DB_NAME="${DB_NAME:-eventplatform}"

[ -d "$MIG_DIR" ] || { echo "migrations directory not found: $MIG_DIR" >&2; exit 1; }

# Apply the meta migration first (creates _migrations if missing).
META="$MIG_DIR/000_meta.sql"
if [ -f "$META" ]; then
    psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -v ON_ERROR_STOP=1 -f "$META" > /dev/null
fi

# Read applied ids
APPLIED=$(psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -tAc "SELECT id FROM _migrations" | tr '\n' ' ')

applied=0; skipped=0
for f in "$MIG_DIR"/[0-9][0-9][0-9]_*.sql; do
    base=$(basename "$f" .sql)
    id=$((10#${base:0:3}))
    [ "$id" = "0" ] && continue   # meta already done
    if echo " $APPLIED " | grep -q " $id "; then
        skipped=$((skipped + 1))
        continue
    fi
    echo "applying $base ..."
    psql -h "$DB_HOST" -U "$DB_USER" -d "$DB_NAME" -v ON_ERROR_STOP=1 -f "$f" > /dev/null
    applied=$((applied + 1))
done

echo "Migrations: $applied applied, $skipped skipped (already up-to-date)."
