#!/data/data/com.termux/files/usr/bin/bash
# Database backup — pg_dump + rotate (keep last 7 days)
# Run via cron daily

set -e

BACKUP_DIR="$HOME/backups"
mkdir -p "$BACKUP_DIR"

DATE=$(date +%Y%m%d_%H%M%S)
BACKUP_FILE="$BACKUP_DIR/eventplatform_$DATE.sql.gz"

# Dump + compress
pg_dump -U rofi eventplatform | gzip > "$BACKUP_FILE"

# Verify
if [ -s "$BACKUP_FILE" ]; then
    echo "[$(date)] Backup OK: $BACKUP_FILE ($(du -h "$BACKUP_FILE" | cut -f1))" >> "$HOME/backup.log"
else
    echo "[$(date)] BACKUP FAILED" >> "$HOME/backup.log"
    rm -f "$BACKUP_FILE"
    exit 1
fi

# Rotate: keep last 7 days
find "$BACKUP_DIR" -name "eventplatform_*.sql.gz" -mtime +7 -delete

echo "[$(date)] Active backups: $(ls "$BACKUP_DIR" | wc -l)" >> "$HOME/backup.log"
