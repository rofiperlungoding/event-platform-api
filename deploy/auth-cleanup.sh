#!/data/data/com.termux/files/usr/bin/bash
# Cleanup expired entries from RevokedToken. Run hourly via cron.
# Mitigates audit item 91 — without this the table grows unbounded
# even though revoked tokens become irrelevant after their natural
# expiry.
#
# Round 7 fix (audit item 237): add `pipefail` so a failed psql
# pipeline does not silently report 0 deletions and a successful
# return code. Use `VACUUM (ANALYZE)` rather than plain VACUUM so
# the planner stays aware of size shrinkage; full VACUUM FULL is
# avoided because it takes an exclusive lock that would briefly
# block /auth/login.

set -euo pipefail
LOG="$HOME/auth-cleanup.log"

deleted=$(psql -h 127.0.0.1 -U rofi -d eventplatform -tAc \
    "DELETE FROM \"RevokedToken\" WHERE expires_at < NOW() RETURNING 1" 2>>"$LOG" | wc -l)

# Also vacuum the table so disk space is reclaimed promptly.
# ANALYZE keeps the planner up to date without taking the exclusive
# lock that VACUUM FULL would, so /auth/login is unaffected.
psql -h 127.0.0.1 -U rofi -d eventplatform -c "VACUUM (ANALYZE) \"RevokedToken\"" >> "$LOG" 2>&1

echo "[$(date)] auth-cleanup: removed $deleted expired tokens" >> "$LOG"
