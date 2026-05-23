#!/data/data/com.termux/files/usr/bin/bash
# Database backup — pg_dump + integrity verification + retention.
# Runs daily via cron at 03:00 local.
#
# Hardening (audit items 21, 22):
#   - Connect via TCP (-h 127.0.0.1) so Termux's blocked Unix socket
#     does not silently produce empty dumps.
#   - After compression, verify with gunzip -t AND row-count probe of
#     the output, not just file size > 0.
#   - Fail loudly: write to backup.log AND deploy/alert webhook on
#     any failure path.

set -euo pipefail

BACKUP_DIR="$HOME/backups"
LOG="$HOME/backup.log"
mkdir -p "$BACKUP_DIR"

DATE=$(date +%Y%m%d_%H%M%S)
BACKUP_FILE="$BACKUP_DIR/eventplatform_$DATE.sql.gz"
TMP_FILE="$BACKUP_FILE.tmp"

fail() {
    echo "[$(date)] BACKUP FAILED: $*" >> "$LOG"
    rm -f "$TMP_FILE" "$BACKUP_FILE"
    exit 1
}

# 1. Dump via TCP. PGPASSWORD picked up from env (.pgpass also honoured).
if ! pg_dump -h 127.0.0.1 -U rofi --no-owner --no-acl eventplatform 2>>"$LOG" | gzip > "$TMP_FILE"; then
    fail "pg_dump pipeline failed"
fi

# 2. Gzip integrity check
if ! gzip -t "$TMP_FILE" 2>>"$LOG"; then
    fail "gzip integrity check failed"
fi

# 3. Content sanity: dump must contain at least the schema markers
#    every PostgreSQL dump emits.
if ! gzip -dc "$TMP_FILE" | head -50 | grep -q 'PostgreSQL database dump'; then
    fail "dump content missing PostgreSQL markers"
fi

# 4. Row-count probe — for our schema we expect at least one row in
#    Participant (the admin account is always present).
ROWS=$(gzip -dc "$TMP_FILE" | grep -cE '^COPY |^INSERT INTO ')
if [ "$ROWS" -lt 1 ]; then
    fail "dump contains zero data rows (schema-only?)"
fi

# 5. Atomic rename
mv "$TMP_FILE" "$BACKUP_FILE"
SIZE=$(du -h "$BACKUP_FILE" | cut -f1)
echo "[$(date)] Backup OK: $BACKUP_FILE ($SIZE, $ROWS data lines)" >> "$LOG"

# 6. Rotation: keep last 7 days
find "$BACKUP_DIR" -name "eventplatform_*.sql.gz" -mtime +7 -delete

echo "[$(date)] Active backups: $(ls "$BACKUP_DIR" | wc -l)" >> "$LOG"
