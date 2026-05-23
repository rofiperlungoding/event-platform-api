#!/data/data/com.termux/files/usr/bin/bash
# Cleanup expired entries from RevokedToken. Run hourly via cron.
# Mitigates audit item 91 — without this the table grows unbounded
# even though revoked tokens become irrelevant after their natural
# expiry.

set -e
LOG="$HOME/auth-cleanup.log"

deleted=$(psql -h 127.0.0.1 -U rofi -d eventplatform -tAc \
    "DELETE FROM \"RevokedToken\" WHERE expires_at < NOW() RETURNING 1" 2>>"$LOG" | wc -l)

# Also vacuum the table so disk space is reclaimed promptly
psql -h 127.0.0.1 -U rofi -d eventplatform -c "VACUUM \"RevokedToken\"" >> "$LOG" 2>&1

echo "[$(date)] auth-cleanup: removed $deleted expired tokens" >> "$LOG"
