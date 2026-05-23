#!/data/data/com.termux/files/usr/bin/bash
# Quick post-deploy verification — runs from the tablet itself.
# Confirms the v0.7.2-c rollout landed every Round 6–12 surface area.
#
# Usage:
#   bash deploy/verify-deploy.sh
#
# Exits 0 on full pass, 1 on first failure.

set -uo pipefail

PORT=3001
HOST="http://localhost:$PORT"
PASS=0; FAIL=0

ok()   { echo "  ✓ $*"; PASS=$((PASS+1)); }
fail() { echo "  ✗ $*"; FAIL=$((FAIL+1)); }

echo
echo "━━━ Build / version ━━━"
v=$(curl -sf "$HOST/health/detailed" | grep -oE '"version":"[^"]+"' | cut -d'"' -f4)
[ "$v" = "0.7.2-c" ] && ok "version $v" || fail "version mismatch: got '$v', want 0.7.2-c"

echo
echo "━━━ Round 8: security headers ━━━"
hdrs=$(curl -sIf "$HOST/health")
echo "$hdrs" | grep -qi 'strict-transport-security' && ok "HSTS present" || fail "HSTS missing"
echo "$hdrs" | grep -qi 'permissions-policy' && ok "Permissions-Policy present" || fail "Permissions-Policy missing"
echo "$hdrs" | grep -qi 'x-content-type-options' && ok "X-Content-Type-Options present" || fail "X-Content-Type-Options missing"

echo
echo "━━━ Round 8: redacted DATABASE_URL ━━━"
log=$(pm2 logs event-server --lines 200 --nostream 2>/dev/null | grep -E '^Database:' | tail -1)
echo "$log" | grep -q ':\*\*\*@' && ok "Database URL password redacted" || fail "Database URL not redacted: $log"

echo
echo "━━━ Round 6: shared metrics counter ━━━"
m1=$(curl -sf "$HOST/metrics" | grep -E '^eventplatform_requests_total ' | awk '{print $2}')
curl -sf "$HOST/health" > /dev/null
m2=$(curl -sf "$HOST/metrics" | grep -E '^eventplatform_requests_total ' | awk '{print $2}')
[ "$m2" -gt "$m1" ] && ok "metrics counter advanced ($m1 → $m2)" || fail "metrics counter stuck"

echo
echo "━━━ Round 8: migration 006 applied ━━━"
mig=$(psql -h 127.0.0.1 -U rofi -d eventplatform -tAc "SELECT id FROM _migrations WHERE id = 6" 2>/dev/null)
[ "$mig" = "6" ] && ok "migration 006 in _migrations table" || fail "migration 006 not applied"

idx=$(psql -h 127.0.0.1 -U rofi -d eventplatform -tAc "SELECT 1 FROM pg_indexes WHERE indexname = 'idx_attendance_checkedinat'" 2>/dev/null)
[ "$idx" = "1" ] && ok "idx_attendance_checkedinat exists" || fail "idx_attendance_checkedinat missing"

echo
echo "━━━ Round 6: per-email login throttle ━━━"
codes=""
for i in 1 2 3 4 5 6 7; do
    c=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$HOST/auth/login" \
        -H 'Content-Type: application/json' \
        -d '{"email":"throttle-test@example.com","password":"wrong"}')
    codes="$codes $c"
done
echo "$codes" | grep -q 429 && ok "credential-stuffing throttle fires (codes:$codes)" || fail "no 429 in 7 attempts (codes:$codes)"

echo
echo "━━━ Round 6: Content-Length overflow ━━━"
oc=$(printf 'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999999999999999\r\n\r\n' | nc -w 2 localhost $PORT 2>/dev/null | head -1)
echo "$oc" | grep -q '413' && ok "huge Content-Length → 413" || fail "Content-Length overflow not rejected: $oc"

echo
echo "━━━ Round 7: PWA SW v5 ━━━"
swv=$(curl -sf "$HOST/attend/sw.js" | grep -oE "checkin-v[0-9]+" | head -1)
[ "$swv" = "checkin-v5" ] && ok "SW cache name $swv" || fail "SW cache wrong: $swv"

echo
echo "━━━ Round 10: TUI cached PGconn ━━━"
[ -x ./tui/admin-tui ] && ok "admin-tui binary present" || fail "admin-tui not built"

echo
echo "━━━ Round 11: wipe-all covers AuditLog ━━━"
grep -q '"AuditLog"' loadtest/wipe-all.sql && ok "wipe-all touches AuditLog" || fail "wipe-all missing AuditLog"

echo
echo "━━━ Round 12: console a11y ━━━"
grep -q 'aria-label="Console navigation"' console/index.html && ok "sidebar aria-label" || fail "sidebar a11y missing"
grep -q 'styles.css?v=7' console/index.html && ok "CSS cache-bust v7" || fail "CSS still on old version"

echo
echo "━━━ Summary ━━━"
echo "  $PASS passed,  $FAIL failed"
[ "$FAIL" = "0" ] && echo "  ✅ all checks passed" || { echo "  ❌ failures present"; exit 1; }
