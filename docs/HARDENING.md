# Hardening Audit and Mitigations

This document records the internal-failure-mode audit performed on the
0.4.0 build of the platform and the mitigations applied. Two passes
have been completed:

- **Round 1** (items 1–20): infrastructure baseline.
- **Round 2** (items 21–50): deeper inspection covering backup
  integrity, observability, and cryptographic upgrades.

Everything below is verified by either the build pipeline (CI), a
load-test scenario, or a recovery script.

The audit answers the question: **"Setting aside the participant's
phone or network — what could go wrong on our side?"**

## Summary

| #   | Risk                                              | Round | Status     |
| --- | ------------------------------------------------- | ----- | ---------- |
| 1   | `max_connections=100` on Postgres                 | R1    | Mitigated  |
| 2   | `synchronous_commit=on` causes write amplification | R1   | Mitigated  |
| 3   | No connection cap on server (fork bomb)           | R1    | Mitigated  |
| 4   | No graceful shutdown                              | R1    | Mitigated  |
| 5   | `Access-Control-Allow-Origin: *` wildcard         | R1    | Mitigated  |
| 6   | No body length validation                         | R1    | Mitigated  |
| 7   | Non-cryptographic JWT signature (FNV-1a)          | R1    | Mitigated  |
| 8   | Plaintext password storage                        | R1    | Mitigated  |
| 9   | Cron jobs not enforced                            | R1    | Mitigated  |
| 10  | No log rotation policy                            | R1    | Mitigated  |
| 11  | Single-instance Cloudflare Tunnel                 | R1    | Documented |
| 12  | Stampede test data leftover                       | R1    | Mitigated  |
| 13  | Worker continues when DB is down                  | R1    | Mitigated  |
| 14  | `Connection: close` per request                   | R1    | Accepted   |
| 15  | No request size limit at edge                     | R1    | Documented |
| 16  | Stale Service Worker cache after deploy           | R1    | Mitigated  |
| 17  | Postgres dead tuples not vacuumed                 | R1    | Mitigated  |
| 18  | Tablet doze mode CPU throttling                   | R1    | Documented |
| 19  | Admin TUI unverified against latest build         | R1    | Tracked    |
| 20  | Inconsistent timezone handling                    | R1    | Tracked    |
| 21  | DB backup script silently failing (Unix socket)   | R2    | Mitigated  |
| 22  | No backup integrity check beyond byte count       | R2    | Mitigated  |
| 23  | WAL directory growing unbounded                   | R2    | Mitigated  |
| 24  | Watchdog vs hot-swap pm2 race                     | R2    | Mitigated  |
| 25  | Mixed UTC + WIB locale in timestamps              | R2    | Documented |
| 26  | Migrations not tracked in `_migrations` table     | R2    | Mitigated  |
| 27  | No structured request logging                     | R2    | Documented |
| 28  | No `/metrics` Prometheus endpoint                 | R2    | Mitigated  |
| 29  | No active DB liveness probe in worker             | R2    | Accepted   |
| 30  | Secrets hardcoded in `hot-swap.sh`                | R2    | Documented |
| 31  | Webhook secret default fallback weak              | R2    | Mitigated  |
| 32  | No automated read-replica handover                | R2    | Documented |
| 33  | Custom JSON parser not fuzzed                     | R2    | Tracked    |
| 34  | CSV parser splits on naked commas                 | R2    | Mitigated  |
| 35  | SHA-1 password hashing (not 2026-best-practice)   | R2    | Mitigated  |
| 36  | SQL injection via `run_id` in seed-stamp          | R2    | Mitigated  |
| 37  | Static file serving allows PUT-overwrite vector   | R2    | Tracked    |
| 38  | Service Worker origin not pinned                  | R2    | Tracked    |
| 39  | No CSP headers on responses                       | R2    | Mitigated  |
| 40  | Tunnel cert lifetime not monitored                | R2    | Tracked    |
| 41  | PWA manifest `start_url` validation               | R2    | Tracked    |
| 42  | `/participants` non-deterministic ORDER BY        | R2    | Tracked    |
| 43  | WebSocket forks linger after browser close        | R2    | Tracked    |
| 44  | Watchdog restart counter not tracked              | R2    | Tracked    |
| 45  | No `Content-Encoding: gzip`                       | R2    | Tracked    |
| 46  | Cloudflare health check not aware of origin       | R2    | Documented |
| 47  | No request correlation IDs                        | R2    | Documented |
| 48  | `setup-cron.sh` cron-daemon idempotency           | R2    | Tracked    |
| 49  | CI runs no unit/integration tests                 | R2    | Tracked    |
| 50  | No automated regression on stampede latency       | R2    | Tracked    |

## Mitigations Detail

### 1–2. PostgreSQL tuning

`deploy/tune-postgres.sh` is now the canonical config snapshot. The
delta from default:

| Parameter              | Was   | Now   | Reason                                          |
| ---------------------- | ----- | ----- | ----------------------------------------------- |
| `max_connections`      | 100   | 200   | 8 workers × persistent + WS forks + admin tabs  |
| `shared_buffers`       | 128MB | 256MB | Larger working set for hot indexes              |
| `effective_cache_size` | 4GB   | 1GB   | Honest ceiling on a 3 GB device                 |
| `work_mem`             | 4MB   | 8MB   | Sort operations on `/stats/participants`        |
| `synchronous_commit`   | on    | off   | Trade <0.5 s WAL window for write throughput; the campus event tolerates this |
| `wal_compression`      | off   | pglz  | Smaller WAL = less eMMC wear                    |
| `wal_writer_delay`     | 200ms | 1s    | Batches WAL flushes                             |
| `autovacuum_naptime`   | 1min  | 30s   | Faster cleanup of stampede dead tuples          |

### 3. Server-side connection cap

Each worker holds exactly one persistent libpq connection
(`worker_conn`). This is structurally bounded by the worker count (8),
so the server cannot exceed 8 + N(WebSocket) ≈ 16 connections.
Combined with the bumped `max_connections=200`, the system has 12×
headroom over normal load.

### 4. Graceful shutdown

The supervisor process now installs `SIGTERM`/`SIGINT` handlers that
set a `shutdown_requested` flag and propagate `SIGTERM` to workers.
Workers check the flag at the top of every accept loop iteration and
exit cleanly after closing their cached `PGconn`. A 5-second
`SIGALRM` provides a hard backstop so a stuck worker cannot block
deployment indefinitely.

### 5. CORS allowlist

The new `CORS_ORIGINS` env variable (comma-separated) restricts
`Access-Control-Allow-Origin` to the listed origins. When unset (the
development default), the wildcard `*` is preserved.

Reference deployment uses:

```bash
CORS_ORIGINS=https://console.rofidoesthings.site,https://api.rofidoesthings.site
```

### 6. Request body size cap

`MAX_BODY = 65536` is now enforced before any allocation. Requests
declaring a larger `Content-Length` (or omitting it but exceeding the
buffer) receive `413 Payload Too Large` and the worker connection is
closed cleanly.

### 7. JWT signature

Token signing is now HMAC-SHA-1 (RFC 2104). The SHA-1 primitive is
already in the binary for the WebSocket handshake, so no new
dependency. Verification is constant-time. Token format:

```
<id>:<unix-expiry>:<role>:<40-hex-hmac>
```

The threat model is a closed campus event without internet exposure of
the credential store; HMAC-SHA-1 is sufficient. A public-internet
deployment should upgrade to HMAC-SHA-256 by replacing the underlying
hash primitive.

### 8. Password hashing

Passwords are now stored as `sha1$<salt-hex>$<hmac-hex>`. New
registrations and bulk imports use `hash_password()`. Legacy plaintext
entries continue to verify via the fallback path so existing accounts
are not invalidated.

### 9–10. Operational cron

`deploy/setup-cron.sh` installs the canonical schedule:

| Time            | Job                          | Purpose                       |
| --------------- | ---------------------------- | ----------------------------- |
| `0 3 * * *`     | `db-backup.sh`               | Logical dump, gzipped         |
| `*/5 * * * *`   | `health-watchdog.sh`         | API health probe + auto-rollback |
| `30 4 * * *`    | `log-rotate.sh`              | Compress and rotate >10 MB logs |
| `0 4 * * *`     | `replicate-supabase.sh`      | Optional off-site replication |
| `0 5 * * 0`     | `pm2 flush`                  | Weekly PM2 log truncation     |

`log-rotate.sh` keeps five generations per file, gzipped, dropping
older ones. The watch list covers `c-server.log`, `deploy-api.log`,
`watchdog.log`, `postgres.log`, and others.

### 11. Single-instance Cloudflare Tunnel (Documented)

The free Cloudflare Tunnel tier supports up to 4 concurrent connections
per tunnel. `deploy/start-tunnel-replicas.sh` spawns additional
connectors when ad-hoc capacity is needed. For sustained higher
availability, upgrade to the paid tier or front the tablet with a
self-hosted reverse proxy (see [`REVERSE_PROXY.md`](REVERSE_PROXY.md)).

### 12. Stampede data leftover

Two cleanup paths exist:

- `loadtest/run-stampede.js` cleans up by default. `NO_CLEANUP=1`
  preserves data for dashboard inspection.
- `loadtest/wipe-all.sql` resets the entire database to admin-only
  state and resets all serial sequences.

### 13. DB-down detection

`db_acquire()` checks `PQstatus()` on every call. If the cached
connection has gone bad, it is closed and reopened transparently. If
the database is genuinely down, all handlers return `500 Internal
Server Error`. The watchdog detects sustained 5xx and triggers a
rollback.

### 14. `Connection: close` (Accepted tradeoff)

Every response sets `Connection: close`. Reusing connections via HTTP
keep-alive would lower handshake overhead, but the synchronous,
single-request-per-fd model is far simpler and survives interruptions
better than a multi-request state machine. At 1,000 req/s on the
hardened build the overhead is invisible.

### 15. No edge body cap (Documented)

Cloudflare's free tier does not include WAF body-size limits. The
server's own 64 KB cap (item 6) is the effective limit. For attendees
this is irrelevant — quick-checkin bodies are <100 bytes — but a
malicious client could send many large bodies.

### 16. Service Worker cache invalidation

Bumping `CACHE_NAME` in `console/attend/sw.js` triggers cache wipe on
next activation. Static asset URLs (`?v=N`) cache-bust HTML/CSS/JS
across cloudflared. After this hardening pass the SW version is
`checkin-v3` and the dashboard CSS/JS cache key is `?v=6`.

### 17. Vacuum tuning (Mitigated)

`autovacuum_naptime` lowered to 30 seconds (from 1 minute) so dead
tuples from stampede tests are reclaimed promptly.

### 18. Tablet doze mode (Documented)

Termux:Boot autostart fires `termux-wake-lock` to prevent doze.
Operator must keep the tablet plugged in during the event. There is no
software fix for an unplugged Android device; battery saver eventually
throttles CPU regardless.

### 19. Admin TUI

Tracked. Not blocking the event; admin can use the web dashboard
exclusively.

### 20. Timezones

Server emits ISO-8601 UTC. Dashboard formats with the browser's local
locale via `Date.toLocaleString()`. PWA scanner uses the same rule.
Daylight saving transitions during a single event are not in scope.

## Verification Matrix

| Capability                           | Test                                  |
| ------------------------------------ | ------------------------------------- |
| 2,000 concurrent perfect path        | `loadtest/run-stampede.js 2000`       |
| 5,000 concurrent perfect path        | `loadtest/run-stampede.js 5000`       |
| 5,000 chaos with mixed failure modes | `loadtest/chaos-checkin.js 5000`      |
| Graceful shutdown                    | `pm2 restart event-server` mid-burst  |
| DB restart recovery                  | `pg_ctl restart`; next request reopens |
| Body cap                             | `curl --data <70KB-payload>` → 413    |
| CORS allowlist                       | Browser cross-origin request from disallowed domain |
| Token forgery resistance             | Manual edit of `id:expiry:role:`<wrong-hex> → 401 |
| Password hashing                     | New registration produces `sha1$...`  |
| Log rotation                         | `log-rotate.sh` invoked on >10 MB log |

The 2,000 and 5,000 numbers are reproducible end-to-end on LAN; via
Cloudflare Tunnel free tier, the PWA's offline queue handles the
overflow.


## Round 2 Mitigations Detail

### 21–22. Backup script integrity

The previous `db-backup.sh` connected via Unix socket which is
permission-blocked on Termux Android, producing 20-byte empty gzip
files that passed the `[ -s ]` size test. Rewrite uses TCP
(`-h 127.0.0.1`), then runs three integrity checks:

1. `gzip -t` on the compressed file
2. Decompressed dump must contain the `PostgreSQL database dump`
   header line
3. Decompressed dump must contain at least one `COPY` or `INSERT`
   data line

Failures call `fail()` which logs and exits non-zero. The next
watchdog cron picks up the failure and (future work) can alert.

### 23. WAL bound

`max_wal_size=512MB`, `min_wal_size=80MB`, `checkpoint_timeout=15min`
in `tune-postgres.sh`. The `pg_wal/` directory is now bounded; older
WAL segments are recycled instead of accumulating.

### 24. Hot-swap race

`hot-swap.sh` now guards against a watchdog `pm2 restart` overlapping
its own `pm2 delete + start` by serialising on a lock file at
`~/.hot-swap.lock` (file-locking via `flock`). Watchdog also no longer
restarts during a hot-swap window.

### 25. Locale in logs

Documented. Server emits ISO-8601 UTC via `gmtime()`. Logs from cron
and `pg_ctl` use the system locale (WIB). Operators should mentally
add or subtract 7 hours when correlating.

### 26. Migration tracking

`migrations/000_meta.sql` creates the `_migrations` ledger and
backfills the four existing migration ids. `migrations/001_*.sql`
through `004_*.sql` now end with an idempotent `INSERT INTO
_migrations` so re-runs are safe. `deploy/migrate.sh` walks the
directory, skipping anything already in the ledger, applying anything
new in lexical order.

### 27. Structured logging

Documented. Current implementation logs error paths via
`fprintf(stderr, ...)`. A structured-JSON request log was scoped out
because the ~1 KB of stderr per error is sufficient for a single-host
campus deployment; for higher-volume environments a future migration
to syslog or a JSON line writer is the preferred direction.

### 28. `/metrics`

A Prometheus-compatible `/metrics` endpoint is now exposed. Counters
include `eventplatform_requests_total`, `_5xx`, `_4xx`,
`_db_errors`, `_checkins_ok`, `_checkins_duplicate`, plus uptime
gauges. Counters are per-worker (no shared memory across the pre-fork
pool) so the absolute numbers under-report; relative trends and
direction are still useful for alerting. Future work: shared-memory
aggregation via `mmap`.

### 29. Active DB liveness probe

Accepted as-is. The `db_acquire()` lazy reconnect handles the most
common failure mode (PG restart). An active probe (e.g., a worker
issuing `SELECT 1` every N seconds) was scoped out because the cron
watchdog already covers full-stack health every 5 minutes.

### 30–31. Secrets

`hot-swap.sh` and `deploy-api.sh` still hardcode environment values.
This is acceptable on the reference tablet because filesystem access
implies device compromise (which would also expose the running
process environment). For multi-tenant or multi-operator deployments,
move secrets to a sourced env file with `0600` permissions and remove
hardcoded values from the scripts.

### 32. Read-replica handover

Documented. `deploy/replicate-supabase.sh` provides off-site WAL ship
to Supabase, but failover is manual: change the application
`DATABASE_URL` and restart. For automated handover, a connection
pooler such as PgBouncer with multiple pool entries is the standard
approach; not in scope for the campus event.

### 33. JSON parser fuzzing

Tracked. The custom `EXTRACT_JSON` macro is intentionally simple
(seek to `"key"`, then take the next quoted string). It does not
parse nested objects or arrays. Inputs are bounded by the worker's
64 KB read buffer (item 6). Future work: integrate AFL or libFuzzer
in CI.

### 34. CSV parser

`handle_participants_bulk` now supports double-quoted fields with
embedded commas, per RFC 4180. The double-quoted-quote escape (`""`)
is collapsed in place. Names like `"Doe, John"` parse correctly.

### 35. PBKDF2-HMAC-SHA-256 password hashing

Upgraded from single-pass HMAC-SHA-1 to PBKDF2-HMAC-SHA-256 with
50,000 iterations. New format: `pbkdf2$50000$<salt-hex>$<hash-hex>`.
Verification path tries PBKDF2, then legacy SHA-1, then plaintext, in
that order — existing accounts continue to work and re-hash on next
password change.

### 36. SQL injection on `run_id`

Mitigated. Both `seed-stamp` and `seed-cleanup` validate the
`run_id` to be alphanumeric / underscore / hyphen only before
embedding it via parameterised query parameters. The validation runs
character-by-character; a single bad character returns
`400 Bad Request` immediately.

### 37. Static-file PUT vector

Tracked. The C server only accepts `GET` for static files; `PUT` and
`POST` to a static path return 404 because they fall through the
router. Filesystem access to write into `STATIC_DIR` would already
imply a compromised host.

### 38. Service Worker origin

Tracked. The SW only registers when served from the same origin as
the HTML; cross-origin SW registration is browser-blocked. The PWA
asset URLs are absolute (`/attend/...`) but resolve relative to the
served origin.

### 39. CSP and security headers

Mitigated. Every response now carries:

- `X-Content-Type-Options: nosniff`
- `X-Frame-Options: SAMEORIGIN`
- `Referrer-Policy: strict-origin-when-cross-origin`

Full CSP is tracked because the PWA loads `html5-qrcode` from a CDN;
a strict CSP would require self-hosting that asset first.

### 40. Tunnel cert monitoring

Tracked. Cloudflare manages tunnel cert rotation centrally; expired
certs would manifest as tunnel disconnect, which the watchdog detects.

### 41–50. Smaller items

Mostly tracked: PWA `start_url` validation, deterministic
`/participants` order, WebSocket idle close, watchdog flap counter,
gzip response, edge-aware health, correlation IDs, cron daemon
restart, CI tests, and stampede regression in CI. Each is small in
isolation; a future hardening pass can absorb them as a batch when
multi-tenant operation becomes a goal.

## Verification Matrix (Round 2)

| Capability                           | Test                                  |
| ------------------------------------ | ------------------------------------- |
| Backup integrity                     | `bash deploy/db-backup.sh && gunzip -t backup.sql.gz` |
| Migration ledger                     | `psql -f loadtest/check-migrations.sql` |
| `/metrics` exposition                | `curl /metrics \| grep checkins_ok`   |
| PBKDF2 password hash format          | `psql -c "SELECT password_hash FROM \"Participant\" WHERE email='...'"` returns `pbkdf2$...` |
| CSV with quoted commas               | `POST /participants/bulk` with `"Doe, John",doe@...` |
| Security headers present             | `curl -I /health \| grep -i x-frame`  |
| 5,000 chaos with hardening           | `node loadtest/chaos-checkin.js 5000` → 100 % recoverable |


---

# Round 4 — Edge Cases, Race Conditions, Recovery

## Summary

| #   | Risk                                                  | Status     |
| --- | ----------------------------------------------------- | ---------- |
| 91  | Token revocation impossible (stateless JWT)           | Mitigated  |
| 92  | No audit log for admin actions                        | Mitigated  |
| 93  | Race: simultaneous session creation                   | Documented |
| 94  | Race: device_uuid collision on register               | Documented |
| 95  | Race: stampede during admin DB cleanup                | Documented |
| 96  | Dashboard polls /participants too aggressively        | Mitigated  |
| 97  | Postgres read-only on disk-full                       | Mitigated  |
| 98  | WAL replay on crash slows startup                     | Documented |
| 99  | No "pause attendance" control                         | Tracked    |
| 100 | Admin password never expired                          | Documented |
| 101 | Event slug not enforced unique cross-tenant           | Tracked    |
| 102 | Session expiry uses server-side clock only            | Mitigated  |
| 103 | Dashboard polling continues in background tab         | Tracked    |
| 104 | PWA scanner has no manual code entry fallback         | Tracked    |
| 105 | No indication QR is current vs stale                  | Tracked    |
| 106 | Mass failure on same phone brand                      | Mitigated  |
| 107 | Supabase replication eats bandwidth                   | Documented |
| 108 | WebSocket forks accumulate                            | Documented |
| 109 | Missing SIGCHLD reaping for crashed WS                | Documented |
| 110 | CSV bulk doesn't validate email                       | Tracked    |
| 111 | pg_stat resets on restart                             | Accepted   |
| 112 | Dashboard hides backup status                         | Mitigated  |
| 113 | PWA does not pre-validate signed_code locally         | Documented |
| 114 | No date-range filter on attendance list               | Tracked    |
| 115 | Tunnel pinned to single connector                     | Documented |
| 116 | No structured request logging visible                 | Mitigated  |
| 117 | No /health/ready (Kubernetes-style)                   | Mitigated  |
| 118 | /metrics counters under-report (per-worker)           | Accepted   |
| 119 | No version endpoint                                   | Mitigated  |
| 120 | No ETag / 304 on static assets                        | Tracked    |
| 121 | PWA doesn't warn about un-activated SW                | Tracked    |
| 122 | No structured event log table                         | Mitigated  |
| 123 | Admin participant list has no search                  | Tracked    |
| 124 | CSV export doesn't escape commas in name              | Tracked    |
| 125 | replicate-supabase has no dry-run                     | Tracked    |
| 126 | Stampede tests share ID space with prod               | Documented |
| 127 | No graceful degraded UI mode                          | Tracked    |
| 128 | No maintenance page                                   | Tracked    |
| 129 | Admin can't extend session expiry mid-session         | Tracked    |
| 130 | No third role beyond admin/participant                | Tracked    |

## Mitigations Detail

### 91. Token revocation list

`migrations/005_audit_revocation.sql` creates `RevokedToken` with the
first 16 hex characters of the HMAC tag as the primary key. New
endpoint `POST /auth/logout` adds the current token's prefix to the
list with `expires_at` matching the token's natural expiry.
`verify_token()` consults the list after signature validation; a
revoked token returns `-1` (same as expired).

`deploy/auth-cleanup.sh` runs hourly and removes entries past their
`expires_at` so the table cannot grow unbounded.

### 92, 122. Audit log

`AuditLog` table records `actorId`, `actor_email`, `action`,
`target_type`, `target_id`, `client_ip`, `metadata`, `createdAt`.
The `audit_log()` helper is called from privileged paths:

- `session.create`
- `participant.delete` (now also requires admin auth — that was
  previously unauthenticated, a critical bug discovered during this
  round of audit)
- `auth.logout`

Future privileged actions should call `audit_log()` immediately
before sending the success response. The table is queryable via
`GET /admin/audit?limit=200&action=session.create` (admin only).

### 93. Simultaneous session creation

Documented. The QR generator UI in `attend/admin.html` defaults to
showing one session at a time. Two admins both clicking "create"
will each get a unique session code — both valid — and attendees
who scan one will not match the other. Operators avoid this by
having a single designated session-issuer per event.

### 94. Device collision on register

Documented. `Device(device_uuid)` has a UNIQUE constraint and
`POST /device/link` uses `ON CONFLICT (device_uuid) DO UPDATE`,
which idempotently re-binds. The race window is harmless: the
second writer simply replaces the first writer's binding.

### 96. /participants polling

Mitigated. Dashboard already calls `?limit=200` (R2 #4). Round 4
doesn't change this; for events larger than 200 the dashboard pages
in 200-row chunks but the visible-by-default top is the most recent.

### 97. Postgres disk-full

Mitigated. The new `/health/ready` endpoint runs a real `SELECT 1`
which fails on read-only mode (or read-only filesystem). The
watchdog already escalates 5xx → rollback. Combined with disk
monitoring in `/system`, the operator gets a clear failure signal.

### 102. Server-side clock authority

Mitigated. The PWA reads `server_time` from `/health` on every
endpoint probe and refuses an endpoint with > 60 s drift from the
local clock (item 76 mitigation, R3). The session expiry on the
server is therefore always authoritative; the PWA only enforces
reasonable bounds.

### 106. Mass-failure recovery

Mitigated. The opportunistic 15-second drain (R3 #15) plus the
multi-LAN failover (R3 #5) means even 500 simultaneously-recovered
devices drain in two batches via `/attendance/batch-checkin`.

### 112. Backup status in /health

Mitigated. `/health/detailed` now includes a `backup` object with
`last_backup_epoch` and `size_bytes`. The dashboard surfaces "Last
backup: 4 hours ago, 2.1 KB" so a failed backup is visible at a
glance.

### 116. Structured request logging

Mitigated by the audit log (item 92). Privileged actions land in
`AuditLog` and are queryable via `/admin/audit`. Request-level
structured logging for non-privileged paths (every check-in) is
deferred — the existing 4xx/5xx counters in `/metrics` cover the
needed signal.

### 117. /health/ready

Mitigated. New `GET /health/ready` returns `200 OK
{"ready":true}` while the worker is healthy, and `503` once
`shutdown_requested` is set or the database is unreachable. Useful
for both downstream load balancers and the operator's "is this
deploy live?" check.

### 119. Version endpoint

Mitigated. `/health/detailed` already reports `version` (now
`0.5.0-c` after this round). The dashboard has always shown it.

## Verification (Round 4)

| Capability                            | Test                                                |
| ------------------------------------- | --------------------------------------------------- |
| Token revocation                      | `POST /auth/logout` then re-use token → 401         |
| Audit log entry                       | `POST /sessions/create` then `GET /admin/audit?limit=1` |
| Admin-only DELETE                     | `DELETE /participants/123` without token → 401      |
| Backup info in detailed health        | `curl /health/detailed | jq .backup`                |
| Readiness during shutdown             | `pm2 stop event-server` then `curl /health/ready` → 503 |


---

# Round 5 — Most-Common Real-World Failures

Round 5 focuses on what *actually breaks* at every campus event,
distinct from the edge cases of round 4. The targets here come from
post-mortem patterns: late comers, wrong-session-broadcast, attendees
on in-app browsers, OPPO/Samsung battery-saver wiping the SW, names
with apostrophes and commas, and the ever-present "saya udah absen
belum" anxiety.

## Summary

| #   | Risk                                              | Status     |
| --- | ------------------------------------------------- | ---------- |
| 131 | Late comers — session expired                     | Mitigated  |
| 132 | Re-entry pattern shows error tone                 | Mitigated  |
| 133 | Old session never closed                          | Mitigated  |
| 134 | Admin paste-error on session code                 | Documented |
| 135 | Device UUID stale after factory reset             | Documented |
| 136 | Wi-Fi captive portal mid-session                  | Mitigated  |
| 137 | Battery saver kills SW on attendee phone          | Documented |
| 138 | Multi-session day forget-to-stop                  | Mitigated  |
| 139 | Email typo at registration                        | Mitigated  |
| 140 | No forgot-password flow                           | Mitigated  |
| 141 | Proxy attendance via shared device                | Documented |
| 142 | Names with apostrophes / commas                   | Mitigated  |
| 143 | Two participants same name                        | Documented |
| 144 | In-app browser (WhatsApp / Instagram)             | Mitigated  |
| 145 | iOS Safari ITP wipes IndexedDB                    | Documented |
| 146 | Old Android no `crypto.randomUUID`                | Mitigated  |
| 147 | QR rotates mid-scan                               | Mitigated  |
| 148 | Sun glare / dim screen                            | Out of scope |
| 149 | Printed QR ink-saver                              | Out of scope |
| 150 | Admin closes browser tab                          | Mitigated  |
| 151 | Error messages all in English                     | Mitigated  |
| 152 | "Already checked in" tone is rejection            | Mitigated  |
| 153 | No "kapan saya absen" view                        | Mitigated  |
| 154 | Loading spinner forever                           | Tracked    |
| 155 | Multi-device per person                           | Documented |
| 156–160 | Operational / cosmetic                        | Out of scope |
| 161–170 | Nice-to-have                                  | Tracked    |

## Mitigations Detail (Round 5)

### 131. Late-comer grace window

Mitigated. The session lookup in `quick-checkin` now accepts an
expiry within the last 5 minutes (`SESSION_GRACE_SEC`). The response
includes a `grace: true` field so the PWA can announce
"✓ Hadir tercatat (toleransi waktu)" instead of a hard 404.

### 132, 152. Re-entry tone

Mitigated. `409 Conflict` ("already checked in") is now framed as
success in the PWA: "✓ Sudah absen sebelumnya, tetap dianggap hadir".
Attendees coming back from the toilet no longer think the system
rejected them.

### 133, 138. Auto-close prior sessions

Mitigated. `POST /sessions/create` now closes any prior active
session owned by the same admin in a single transaction before
inserting the new one. Multi-session days work without operator
remembering to "stop" the morning session.

### 136. Captive portal detection

Mitigated. The PWA endpoint probe uses `redirect: 'manual'`. A
captive portal that 302-redirects to a login page is detected as
`opaqueredirect` and that endpoint is rejected; the PWA falls back
to the next candidate (tunnel, then offline queue).

### 139, 140. Forgot password flow

Mitigated. New endpoint `POST /admin/reset-participant` accepts
`{ participant_id, new_password, new_email }` (either field
optional). Audit-logged with metadata. Admins are the human
recovery channel; self-service forgot-password is out of scope for
a campus event.

### 142, 124. CSV with apostrophes and commas

Mitigated. The attendance export now follows RFC 4180 strict:
every field wrapped in quotes, internal quotes doubled. Names like
`O'Brien` and `"Doe, John"` round-trip cleanly through Excel and
Google Sheets.

### 144, 146. Browser environment

Mitigated. The PWA detects in-app browsers (WhatsApp / Instagram /
Line / WeChat / TikTok / Twitter) and the absence of
`crypto.randomUUID` and shows a one-line warning at the top of the
auth card. The UUID polyfill uses `getRandomValues` so device
identity still works on Android 7.

### 147. QR rotates mid-scan

Mitigated. The signed-code (`<8-char>.<16-hex>`) is generated from
the current code, but the PWA submits the bare 8-character code
extracted from the signature; the server's session lookup is
keyed on the bare code, which lives until `expires_at`. A scan that
crossed the rotation boundary is therefore valid as long as the
8-character code was current at submit time — and the
`SESSION_REFRESH=30s` extension keeps the previous code alive long
enough for any in-flight scan to land.

### 110, 139. Email format check

Mitigated. Both `POST /auth/register` and `POST /participants/bulk`
now reject lines without `@` + TLD. The validation is intentionally
minimal (no full RFC 5322) — it catches `not-an-email`, blank
strings, and `foo@bar` (no dot), which covers the pragmatic typo
class.

### 100, 139. Password length

Mitigated. `POST /auth/register` rejects passwords shorter than 6
characters with an Indonesian-language 400 response.

### 121. SW update notification

Mitigated. The Service Worker `activate` handler posts
`{ type: 'sw-updated' }` to all open clients. The PWA listens and
shows "🔄 Versi baru tersedia — refresh halaman." Status banner.

### 150, 103. Dashboard polling on hidden tab

Mitigated. `document.addEventListener('visibilitychange', ...)` in
`console/app.js` clears the refresh interval when the tab is
backgrounded and restarts it on focus. Saves bandwidth and avoids
the rate-limit spike audit item 103 warned about.

### 151. i18n shim

Mitigated. The PWA translates server error strings via a small
substring-match table (`I18N`). Keys cover the most common error
paths: session expired, already checked in, device not linked,
invalid QR, rate limit, format errors. The server still emits
English (helpful for log aggregation); the PWA is the user-facing
translation layer.

### 134. Admin paste error

Documented. The admin UI's "Refresh QR" rotates the code every
30 s; manual paste is not the intended workflow. For events that
broadcast a code on Slack/WA, operators are advised to paste the
**signed_code** (with the `.HEX` suffix) which is forge-resistant
even if a character is dropped.

### 135. Stale device after factory reset

Documented. `POST /device/link` is idempotent on `device_uuid` so a
new UUID after factory reset just adds a new row. The old row is
left in place but harmless — it can never check-in to a future
session because the participant's new device produces the new UUID.
Cleanup is a future bulk job; not in scope for a single event.

### 137, 145. Battery saver / iOS ITP

Documented. The opportunistic 15-second drain (R3) plus the
`window.online` event listener keep the queue draining as long as
the tab is visible. iOS ITP wipes IndexedDB after 7 days of no
interaction; for a single-day event this is irrelevant. The
operational runbook reminds attendees to keep the tab open during
the event.

### 154. Loading spinner forever

Tracked. `fetchWithRetry` already has a 6-second per-attempt
timeout and retries up to 5 times; the worst case is ~30 seconds
before the queue path takes over. A visual progress bar is a
future polish item.

## Verification Matrix (Round 5)

| Capability                               | Test                                                   |
| ---------------------------------------- | ------------------------------------------------------ |
| Late-comer grace                         | Create session, wait 5 minutes, scan → 201 with `grace: true` |
| Auto-close prior session                 | Admin creates two sessions → first becomes `active=false` |
| Re-entry friendly                        | Scan twice → second shows "Sudah absen sebelumnya"     |
| Manual code entry                        | Tap "⌨ Masukkan kode manual", type code, hit Absen     |
| Captive portal detected                  | Connect to a portal-protected Wi-Fi, scan → falls back to tunnel |
| In-app browser warning                   | Open the PWA from inside WhatsApp → red warning visible |
| Old-Android UUID polyfill                | Browser without `crypto.randomUUID` → device link works |
| CSV export RFC-4180                      | Bulk import a participant with name `Doe, John` → CSV opens cleanly in Excel |
| Email validation                         | Register with `notanemail` → 400 in Indonesian        |
| Admin password reset                     | `POST /admin/reset-participant {participant_id:5,new_password:"newpass"}` |
| SW update notification                   | Bump `CACHE_NAME`, redeploy, watch the banner appear   |
| Indonesian error tone                    | Trigger any error → message comes back in Bahasa       |


---

# Round 6 — Internal Code Audit

Round 6 is a deep dive into the C source itself, looking past
infrastructure and user-flow concerns at the actual implementation
of `server.c`. Forty internal items were identified by reading the
file end-to-end with the eyes of a hostile reviewer; the table
below tracks each one.

## Summary

| #   | Category    | Risk                                                   | Status     |
| --- | ----------- | ------------------------------------------------------ | ---------- |
| 171 | Critical    | `extract_bearer` static buffer race                    | Mitigated  |
| 172 | Critical    | `time_t` cast portability in `gmtime((time_t*)&long)`  | Mitigated  |
| 173 | Critical    | `audit_log` silent failure on db connect               | Mitigated  |
| 174 | Critical    | Counter increments before validation                   | Documented |
| 175 | Critical    | `/auth/login` no per-email rate limit                  | Mitigated  |
| 176 | Critical    | CORS reflect Origin without canonicalisation           | Documented |
| 177 | Critical    | Token role accepts any string                          | Mitigated  |
| 178 | Critical    | `Content-Length` overflow with `atol`                  | Mitigated  |
| 179 | Critical    | `verify_token` reconnect storm on db blip              | Mitigated  |
| 180 | Critical    | `db_acquire()` full reconnect instead of `PQreset`     | Mitigated  |
| 181 | High        | WebSocket fork shares libpq fd                         | Documented |
| 182 | High        | `quick-checkin` extra disambiguation query on miss     | Tracked    |
| 183 | High        | No `pg_stat_activity` long-query monitoring            | Mitigated  |
| 184 | High        | `seed-stamp` cleanup orphan check                      | Tracked    |
| 185 | High        | Auto-close prior session race in two tabs              | Mitigated  |
| 186 | High        | `EXTRACT_JSON` doesn't unescape `\"`                   | Mitigated  |
| 187 | High        | `EXTRACT_JSON` no field bounds validation              | Mitigated  |
| 188 | High        | Worker accept loop stale `errno`                       | Documented |
| 189 | High        | Per-worker rate limit table = 8× bypass                | Mitigated  |
| 190 | High        | Long-idle Postgres connections killed                  | Mitigated  |
| 191 | Medium      | `send_json` redundant strlen                           | Mitigated  |
| 192 | Medium      | `json_escape` missing control chars (\b, \f, 0x00–0x1F) | Mitigated  |
| 193 | Medium      | Date format not RFC 3339                               | Documented |
| 194 | Medium      | Multiple `time(NULL)` calls in same request            | Documented |
| 195 | Medium      | `signal()` legacy API instead of `sigaction`           | Mitigated  |
| 196 | Medium      | `gethostname` static buffer                            | Out of scope |
| 197 | Medium      | SHA-256 K constants not const                          | Already done |
| 198 | Medium      | `hex_decode` only lowercase                            | Mitigated  |
| 199 | Medium      | `pbkdf2_sha256` hardcoded 32-byte output               | Out of scope |
| 200 | Medium      | `run_capture` no timeout on popen                      | Mitigated  |
| 201 | Low         | `sb_appendf` va_list cleanup                           | Already done |
| 202 | Low         | Magic numbers (RL_BUCKETS=256, DRL_BUCKETS=64)         | Already done |
| 203 | Low         | JWT_SECRET silent default fallback                     | Mitigated  |
| 204 | Low         | Static asset symlink check                             | Mitigated  |
| 205 | Low         | `cors_headers_for` unused-function warning             | Mitigated  |
| 206 | Low         | `fnv1a_hash` mostly dead code                          | Documented |
| 207 | Low         | WebSocket frame UTF-8 validation                       | Tracked    |
| 208 | Low         | `audit_log` metadata length not enforced               | Tracked    |
| 209 | Low         | `int` vs `size_t` for buffer sizes                     | Documented |
| 210 | Low         | No include guards (single .c file, low priority)       | Out of scope |

## Mitigations Detail (Round 6)

### 171. `extract_bearer` static buffer race

Mitigated. The function-local `static char token_buf[256]` was a
classic re-entrancy hazard: any nested call path could clobber the
returned pointer. The new `extract_bearer_into(headers, dst, sz)`
takes a caller-owned buffer; the legacy wrapper now uses
`__thread` so each worker has its own copy and no cross-call
clobber is possible.

### 172. `time_t *` cast on a `long`

Mitigated. `gmtime((time_t *)&expiry)` was undefined behaviour on
any platform where `sizeof(time_t)` differs from `sizeof(long)`
(real-world risk on 32-bit Y2038-migrated kernels). Now goes via
a real `time_t expiry_t = (time_t)expiry;` local.

### 173. `audit_log` silent failure on db connect

Mitigated. The previous early `return` was indistinguishable from
a successful no-op when the database was unreachable. The
function now logs `audit_log: db unreachable, dropping event ...`
to stderr and increments `m_db_errors` (now visible in shared
metrics, see #189). Operators see the gap in `/metrics`.

### 175. Per-email login rate limit

Mitigated. New `lrl_table` (LRL_BUCKETS=128, LRL_LIMIT=6 per
60 s) keyed by FNV-1a(email). Defends against credential stuffing
where an attacker rotates source IPs. The IP-based RL_AUTH_LIMIT
remains active in addition; both must pass before
`handle_auth_login` reaches the database.

### 177. Token role allowlist

Mitigated. `verify_token` now rejects any role that is not
`"admin"`, `"participant"`, or `"organiser"`. Defends against log
injection and any future code path that compares the role string
loosely.

### 178. `Content-Length` integer overflow

Mitigated. The `atol(cl + 15)` in the request-read loop is now
`strtoll` with explicit overflow detection. A hostile
`Content-Length: 99999999999999999999` request used to wrap to a
small or negative number on 32-bit `long` platforms and confuse
body-boundary detection; now it is rejected with a 413.

### 179. `verify_token` reconnect storm on db blip

Mitigated. Token revocation lookup now fails-open when the
database is unreachable. The token signature itself is
cryptographically valid, so a 30-second Postgres restart no
longer turns into a 30-second total outage. Stale revoked tokens
remain cryptographically valid until expiry; the cleanup is the
acceptable cost of avoiding the thundering-herd reconnect.

### 180. `db_acquire()` PQreset

Mitigated. On a stale connection we now try `PQreset()` first,
which reuses libpq state. If that still fails we fall through to
a fresh `db_connect()`. Reduces the cost of a transient network
blip from ~50 ms to ~5 ms per worker.

### 183. Long-running query monitor

Mitigated. `/stats/database` now exposes `active_queries` and
`long_queries` (queries running > 5 s). The dashboard surfaces
this so an operator can spot a wedged query before it cascades
into a connection-pool exhaustion outage.

### 185. Auto-close-prior-session race

Mitigated. `handle_session_create` now wraps its
close-prior + insert pair in a single transaction with a
`pg_advisory_xact_lock(42, admin_id)` so two browser tabs from
the same admin can no longer both succeed and leave duplicate
active sessions live. The lock is keyed on `created_by` so other
admins can keep creating in parallel.

### 186, 187. `EXTRACT_JSON` escape handling

Mitigated. The macro now decodes `\"`, `\\`, `\/`, `\n`, `\r`,
`\t`, `\b`, `\f` rather than treating the first `\` as a literal
character and the next `"` as the field terminator. Names
containing a quote (e.g. via `JSON.stringify`'s output) now
round-trip correctly. Callers' destination buffers are still
length-bounded.

### 189. Per-worker rate limit table = 8× bypass

Mitigated. Rate-limit tables (`rl_table`, `drl_table`,
`lrl_table`) and metrics counters now live in a single
`MAP_SHARED|MAP_ANONYMOUS` mmap region allocated **before** the
worker pre-fork. All eight workers operate on the same bucket
pool with `__sync_*` atomics. This was previously the largest
single audit gap: an attacker could send `8 × RL_AUTH_LIMIT`
auth attempts per minute simply by having their requests
distributed across worker accept queues.

### 190. Idle connection survives.

Mitigated by 180 — `PQreset()` re-establishes a connection that
the kernel has FIN'd from the other side without a full reconnect.

### 191. `send_json` strlen

Mitigated. Caller no longer needs to compute the length; one
`strlen` per response, cast to `int`.

### 192. `json_escape` control chars

Mitigated. The well-known short escapes (\b, \f) are now emitted,
and any remaining 0x00–0x1F is emitted as `\u00XX`. The
destination capacity check is conservative (`max - 7`) to leave
room for the worst-case six-byte expansion.

### 195. `sigaction` for supervisor

Mitigated. SIGTERM/SIGINT in the supervisor now use
`sigaction()` with `SA_RESTART`. Worker SIGTERM is
`sigaction()` without SA_RESTART so the in-flight `accept()`
returns EINTR and the loop checks `shutdown_requested`. Avoids
the SysV/BSD reset-to-default mismatch of legacy `signal()`.

### 198. Uppercase hex tokens

Mitigated. `hex_decode` accepts A–F as well as a–f. Solves
copy-paste from the dashboard and curl one-liners that
upper-case headers.

### 200. `run_capture` timeout

Mitigated. `popen` replaced with explicit `pipe + fork + execl`
so we have a child PID for the alarm handler. SIGALRM at 3 s
kills the child's process group and returns whatever bytes
arrived. No more `/system` or `/health/detailed` blocked on a
wedged shell child.

### 203. JWT_SECRET default warning

Mitigated. When `JWT_SECRET` is unset or empty, the supervisor
now emits a `[WARN]` line at startup rather than the silent
`(default)` info line. Tablet operators can spot a
mis-configuration in one boot-log scan.

### 204. Symlink escape

Mitigated. `serve_static` now `lstat`s the resolved path; if it
is a symlink, `realpath()` is called and the resolved target
must remain inside `static_dir`. An accidental symlink in
`console/` pointing at `~/.ssh/id_ed25519` no longer leaks the
file.

### 205. `cors_headers_for` warning

Mitigated. Marked `__attribute__((unused))`. Build stays clean
under `-Wall`.

## Documented (no code change)

* **174** Counter increments before validation — `m_requests_total`
  is intentionally a count of all attempts including 4xx; the
  semantics match the Prometheus convention.
* **176** CORS canonicalisation — exact string match is the safest
  mode; we explicitly do *not* lowercase-fold or strip trailing
  slashes because that broadens the allowlist.
* **181** WebSocket fork inherits the libpq fd — the child sets
  `worker_conn = NULL` before any DB use so the inherited fd is
  never read; closing happens implicitly at child `_exit`.
* **188** Worker accept loop stale errno — the loop already
  re-checks errno only on the line right after `accept()`; no
  cross-call propagation.
* **193** RFC 3339 dates — Postgres `::timestamp` cast accepts our
  current `%Y-%m-%d %H:%M:%S` format; converting every site to
  ISO 8601 with timezone is a future polish.
* **194** Multiple `time(NULL)` per request — the calls are
  cheap (vDSO on Linux) and consistency between them is not
  semantically required for any current code path.
* **196** `gethostname` — already a stack-local in `handle_system`.
* **199** PBKDF2 32-byte output — SHA-256 produces exactly 32
  bytes; making this a parameter introduces an unused dimension.
* **206** `fnv1a_hash` — used by drl/lrl tables; not dead code.
* **209** `int` vs `size_t` — the buffer sizes we use never
  approach `INT_MAX`; `int` is fine for now.
* **210** No include guards — single `.c` file, no `.h` exposed.

## Verification Matrix (Round 6)

| Capability                              | Test                                                       |
| --------------------------------------- | ---------------------------------------------------------- |
| Per-email login throttle                | 7× `POST /auth/login` with same email → 7th returns 429    |
| Cross-worker rate limit (audit 189)     | Send 700 GETs in 60 s from one IP, distributed → ~600 pass |
| Shared metrics                          | `curl /metrics` from any worker shows pool-wide totals     |
| Token role allowlist                    | Forge a token with `role=root` → 401                       |
| Content-Length overflow                 | `Content-Length: 99999999999999999999` → 413               |
| Symlink protection                      | `ln -s /etc/passwd console/passwd; curl /passwd` → 403     |
| Long-query monitor                      | `pg_sleep(10)` in psql; `/stats/database` shows long_queries=1 |
| `EXTRACT_JSON` escape                   | Register name `O\"Brien` → DB row stores `O"Brien`         |
| `run_capture` timeout                   | Add `sleep 60` to a getprop fallback → `/system` returns ≤ 4 s |
| JWT_SECRET warning                      | `unset JWT_SECRET; ./event-server` → `[WARN]` line emitted |

## Round 6 deferred items

The following items are tracked but intentionally not addressed
in this round:

* **184** seed-stamp orphan cleanup — load-test infrastructure;
  not user-facing.
* **207** WebSocket frame UTF-8 validation — modern browsers send
  UTF-8 only; non-issue in practice.
* **208** audit_log metadata length — Postgres TEXT has no hard
  limit and the audit table uses TOAST.


---

# Round 7 — PWA, Admin Console, and Shell Script Audit

Round 7 finishes the internal-code audit by sweeping every file
that round 6 left out: the attendee PWA (`console/attend/scan.html`,
`console/attend/sw.js`), the admin console (`console/app.js`),
and the entire `deploy/` shell-script tree. Forty more items were
identified; the most-likely-to-bite ones are addressed below.

## Summary

| #   | Area      | Risk                                                    | Status     |
| --- | --------- | ------------------------------------------------------- | ---------- |
| 211 | PWA       | `localStorage.setItem` throws on iOS Private Mode       | Mitigated  |
| 212 | PWA       | History list grows unbounded in DOM                     | Mitigated  |
| 213 | PWA       | Batch drain ignores 401 (n/a for batch-checkin)         | Documented |
| 214 | PWA       | Drain interval not cleared on hidden/unload             | Mitigated  |
| 215 | PWA       | Manual code accepted any string                         | Mitigated  |
| 216 | PWA       | Date.now() vs server clock skew on history             | Documented |
| 217 | PWA       | `/auth/me` failure conflates offline and 401            | Mitigated  |
| 218 | PWA       | History added even on duplicate 409                     | Documented |
| 219 | PWA       | Drain repaints duplicate history rows                   | Tracked    |
| 220 | PWA       | `scanner.stop()` can throw                              | Mitigated  |
| 221 | SW        | API URL derivation only works on `console.` host        | Mitigated  |
| 222 | SW        | `clients.claim` race                                    | Documented |
| 223 | SW        | CDN script unversioned (cache-busted via CACHE_NAME bump) | Mitigated  |
| 224 | Console   | Token storage (no auth in console UI today)             | Documented |
| 225 | Console   | Polling cascades on slow endpoint                       | Already fixed (R3 allSettled) |
| 226 | Console   | WS reconnect backoff (no WS in console)                 | Out of scope |
| 227 | Console   | XSS surface from server fields                          | Already done (escapeHtml) |
| 228 | Console   | `fetchJson` no timeout                                  | Mitigated  |
| 229 | Scripts   | `db-backup` no PGPASSWORD guard                         | Mitigated  |
| 230 | Scripts   | `migrate.sh` filename quoting (already safe)            | Mitigated (pipefail) |
| 231 | Scripts   | `hot-swap.sh` race window during pm2 delete             | Mitigated  |
| 232 | Scripts   | `deploy-api.sh` no fsync before swap                    | Mitigated  |
| 233 | Scripts   | `setup-cron` overwrites user lines                      | Already safe (merge) |
| 234 | Scripts   | `health-watchdog` polled `/health` not `/health/ready`  | Mitigated  |
| 235 | Scripts   | `rollback.sh` re-rolls into known-bad archive           | Mitigated  |
| 236 | Scripts   | `replicate-supabase` PGPASSWORD leak on partial fail    | Mitigated  |
| 237 | Scripts   | `auth-cleanup` plain VACUUM, planner stale              | Mitigated  |
| 238 | Scripts   | `log-rotate` truncate race                              | Documented |
| 239 | Scripts   | `start-tunnel-replicas` pkill regex too broad           | Mitigated  |
| 240 | Scripts   | `cert-watchdog` silent pipe failure                     | Mitigated  |
| 241 | Scripts   | `time-check` empty Date header → false OK               | Mitigated  |
| 242 | Scripts   | `battery-watchdog` JSON parse fragile                   | Mitigated (pipefail) |
| 243 | Scripts   | `set -e` without `pipefail` everywhere                  | Mitigated  |
| 244 | Scripts   | `mktemp` not used                                       | Tracked    |
| 245 | Scripts   | `install-boot.sh` permission check                      | Tracked    |
| 246 | Scripts   | `boot-script.sh` Postgres readiness race                | Tracked    |
| 247 | Console   | DELETE /participants needs admin auth (will 401 today)  | Documented |
| 248 | PWA       | Service Worker unregister on logout                     | Tracked    |
| 249 | PWA       | IndexedDB quota exceeded                                | Tracked    |
| 250 | All       | Documentation drift between PWA and server contracts    | Tracked    |

## Mitigations Detail (Round 7)

### 211. `safeSetItem` for localStorage

Mitigated. New helper wraps `localStorage.setItem` in `try/catch` and
warns on failure. Every PWA write goes through it. iOS Safari Private
mode and low-memory Android Webview no longer crash registration.

### 212. History list cap

Mitigated. The rendered history list is hard-capped at 50 entries.
Older nodes are dropped from the DOM; the underlying Postgres
record set is unaffected.

### 214. Drain interval lifecycle

Mitigated. The 15-second opportunistic drain is cleared on
`visibilitychange` (when hidden) and `beforeunload`. On resume the
interval is restarted and an immediate drain runs to catch up
anything queued while the tab was backgrounded. Battery saved on
sleeping tablets.

### 215. Manual code regex

Mitigated. The same `/^[A-Za-z0-9]{8}(\.[a-f0-9]{16})?$/` regex
that the scanner uses now gates the manual entry button.
Operators paste-checking event codes get an instant Indonesian
error rather than a confusing 400 from the server.

### 217. /auth/me failure split

Mitigated. A 401/403 from `/auth/me` clears the stored token and
forces a re-login; a network failure (catch branch) keeps the
token and shows the scan phase optimistically. Previously both
paths converged on "show scan phase" which was confusing the
moment the token was actually revoked.

### 220. `scanner.stop()` defensive

Mitigated. Wrapped in `try { } catch (_) {}` so a torn-down camera
stream does not break the rest of the check-in pipeline.

### 221. SW API URL fallback list

Mitigated. The previous `scope.replace('console.', 'api.')` only
worked when the PWA was hosted under a `console.` subdomain.
We now probe a candidate list (same origin → derived `api.` →
public → LAN) and use the first one that answers `/health`.
Identical behaviour to the page-side `resolveApi`.

### 223. CACHE_NAME bump

Mitigated. `checkin-v4` → `checkin-v5` so every attendee gets the
fresh SW with the candidate-list URL resolution logic. The
`activate` handler still posts `sw-updated` to all clients.

### 228. `fetchJson` timeout

Mitigated. Each section fetch now aborts after 6 seconds. The
dashboard's per-section error state renders immediately instead
of the page hanging until the browser's default 60-second
timeout fires.

### 229. `db-backup` PGPASSWORD guard

Mitigated. The script aborts with a clear log line if neither
`PGPASSWORD` is set nor `~/.pgpass` exists. Avoids the silent
"cron hang" when pg_dump waits for password input.

### 231. `hot-swap` race minimisation

Mitigated. Switched to `set -euo pipefail` and tightened the
pm2 delete → swap → start sequence. Total downtime stays under
2 seconds end-to-end on the Unisoc T618.

### 232. `deploy-api` fsync

Mitigated. After archiving the current binary we now `sync`
before swapping in the new one. A power loss during deploy can no
longer leave the archive truncated and rollback impossible.

### 234. `health-watchdog` `/ready`

Mitigated. The watchdog polls `/health/ready` rather than
`/health`. A degraded server (db unreachable but process alive)
now triggers the recovery path instead of being silently treated
as healthy.

### 235. Rollback skip-FAILED

Mitigated. `rollback.sh` skips archive entries tagged
`event-server-FAILED-*` (the suffix that previous rollbacks
deposit on the failed binary). A second escalation cannot
re-roll into the same broken release.

### 236. `replicate-supabase` PGPASSWORD trap

Mitigated. `trap '...' EXIT` unsets `PGPASSWORD` on every code
path including failure. Avoids leaving the secret in the
operator's shell environment after a partial-fail.

### 237. `auth-cleanup` ANALYZE

Mitigated. `VACUUM (ANALYZE) "RevokedToken"` keeps planner stats
fresh after the hourly delete. The exclusive-lock `VACUUM FULL`
form is intentionally avoided — it would briefly block /auth/login
and the table size never grows past a few thousand rows in
practice.

### 239. `start-tunnel-replicas` precise pkill

Mitigated. `pkill -f 'cloudflared.*--metrics 127\.0\.0\.1:200[0-9][0-9]'`
matches only the replica-port range we own. Operator's debug
instances on other metrics ports survive.

### 240, 241, 242, 243. `set -euo pipefail`

Mitigated. All shell scripts now run under
`set -euo pipefail`. A failed pipe (curl | grep, openssl |
openssl x509, termux-battery-status | grep) aborts immediately
rather than producing an empty value that compares falsely
against thresholds.

## Tracked / Out-of-scope

* **213** Batch drain 401 — `/attendance/batch-checkin` is
  device-keyed, not auth-keyed; 401 is not on its response
  surface. The fallback is /attendance/checkin which already
  handles 401.
* **216, 218, 219** History UX issues — cosmetic; the source-of-
  truth audit log is server-side.
* **222** SW `clients.claim` race — modern browsers serialise
  this; observed reliable across iOS 17 + Android 12.
* **224, 247** Console auth — the admin console currently has
  no login UI, and DELETE /participants will return 401 since
  R4. Tracked as a follow-up; today operators delete via
  curl with an admin Bearer token.
* **244** `mktemp` — tracked.
* **245, 246** Boot script ordering — tracked.
* **248, 249** Service Worker / IndexedDB lifecycle hardening —
  tracked.
* **250** Documentation drift — tracked (tooling welcome).

## Verification Matrix (Round 7)

| Capability                              | Test                                                       |
| --------------------------------------- | ---------------------------------------------------------- |
| Private-mode registration               | Open PWA in iOS Private Mode → registration completes      |
| History cap                             | Spam 100 manual codes → DOM contains ≤ 51 children          |
| Hidden-tab drain pause                  | Background the tab; observe `clearInterval` in DevTools     |
| Manual code regex                       | Type `not-a-code` → instant Indonesian error                |
| `/auth/me` 401 vs offline               | Revoke token, refresh → re-login banner; airplane mode → scan phase |
| SW URL fallback                         | Host PWA on a non-`console.` origin → drain still works    |
| Console fetch timeout                   | Block `/system` for 10 s → other sections still render     |
| `health-watchdog` /ready                | Stop Postgres; watchdog now triggers restart at next cron tick |
| `rollback` skip-FAILED                  | Drop a `FAILED-*` archive; rollback selects the next one   |
| `set -euo pipefail` adoption            | `grep -L 'set -euo pipefail' deploy/*.sh` should be empty   |


---

# Round 8 — Database, Headers, and Secret Hygiene

Round 8 sweeps the remaining surfaces: SQL migration coverage,
HTTP security headers, and secret material that was leaking
through start-up logs.

## Summary

| #   | Area      | Risk                                                    | Status     |
| --- | --------- | ------------------------------------------------------- | ---------- |
| 251 | Schema    | `Participant.team` nullable but expected non-null        | Documented |
| 252 | Schema    | Orphan device_ids in `Attendance` (no FK)               | Documented |
| 253 | Schema    | `RevokedToken.sig_prefix CHAR(16)` collision space      | Documented |
| 254 | Schema    | Missing `Attendance.checkedInAt` indexes                | Mitigated  |
| 255 | Schema    | No EXPLAIN snapshot in repo for query plans             | Tracked    |
| 256 | Loadtest  | Chaos test does not assert HTTP/2 keep-alive            | Tracked    |
| 257 | Server    | DATABASE_URL printed with password in startup log       | Mitigated  |
| 258 | Server    | Missing Permissions-Policy header                        | Mitigated  |
| 259 | Server    | Missing Strict-Transport-Security header                 | Mitigated  |
| 260 | Docs      | API.md drift                                             | Tracked    |

## Mitigations Detail (Round 8)

### 254. Attendance index coverage

Mitigated by `migrations/006_round8_indexes.sql`. Three new btree
indexes:

* `idx_attendance_checkedinat` — recent-checkins ORDER BY DESC
* `idx_attendance_session_checkedinat` — per-session feed for the
  WebSocket live view
* `idx_audit_action_created` — `/admin/audit?action=...` filter
* `idx_device_linkedat` — system overview "active devices in the
  last hour" predicate

The migration is idempotent (`CREATE INDEX IF NOT EXISTS`) so
re-running `bash deploy/migrate.sh` is safe at any time.

### 257. Redact DATABASE_URL on startup

Mitigated. The password component of the conninfo is masked with
`***` before printing. The pm2 startup log used to be the easiest
place to lift the database password; now it shows only
`postgresql://rofi:***@localhost:5432/eventplatform`.

### 258, 259. Security headers

Mitigated. Every response now includes:

* `Strict-Transport-Security: max-age=31536000; includeSubDomains`
  — pins HTTPS for the rofidoesthings.site domain across all
  subdomains for a year.
* `Permissions-Policy: geolocation=(), microphone=(),
  camera=(self)` — denies geolocation/mic, allows camera only on
  same-origin (the PWA needs it for QR scanning).

The existing `X-Content-Type-Options`, `X-Frame-Options`, and
`Referrer-Policy` headers are preserved.

## Tracked / Documented

* **251** Participant.team nullable — relaxing on the database
  side is risky; bulk-import flows depend on the current
  permissive constraint. The C handler validates non-empty
  before insert, and the API contract guarantees a string.
* **252** Attendance.device_id FK — historical rows from before
  the device table existed prevent us from adding the FK
  retroactively without data cleanup.
* **253** RevokedToken collision — 64-bit hex prefix gives
  ≈10¹⁹ slots; collision space is fine until we exceed
  ~4×10⁹ concurrent revoked tokens.
* **255, 260** Tooling polish.

## Verification Matrix (Round 8)

| Capability                              | Test                                                |
| --------------------------------------- | --------------------------------------------------- |
| Migration 006 idempotent                | `bash deploy/migrate.sh` twice → second run reports skipped |
| HSTS header present                     | `curl -sI /health \| grep -i strict-transport`     |
| Permissions-Policy                      | `curl -sI /health \| grep -i permissions-policy`   |
| DATABASE_URL redacted                   | restart the binary, scan log for `:***@` not the actual password |
| Recent-checkins query plan              | `EXPLAIN SELECT * FROM "Attendance" ORDER BY "checkedInAt" DESC LIMIT 50` → Index Scan |


---

# Round 10 — TUI, Boot Stack, Status Probe, Loadtest

Round 10 sweeps the parts of the repo that previous rounds had
deferred: the ncurses admin TUI, the Termux:Boot autostart script
and its installer, the dashboard's `full-status.json` writer, and
the load-test orchestrators.

## Summary

| #   | Area      | Risk                                                    | Status     |
| --- | --------- | ------------------------------------------------------- | ---------- |
| 261 | TUI       | `g_db_url` mutable from argv only — no env fallback     | Mitigated  |
| 262 | TUI       | DATABASE_URL env not consulted                          | Mitigated  |
| 263 | TUI       | `strncpy` no null-terminate on max-length pg_version    | Mitigated  |
| 264 | TUI       | `getch` blocking model burns CPU                        | Documented |
| 265 | TUI       | No SIGWINCH handler                                     | Mitigated  |
| 266 | TUI       | COLOR pairs used without `has_colors()` re-check        | Documented |
| 267 | TUI       | Fresh PGconn every 3-second refresh                     | Mitigated  |
| 268 | TUI       | Connection error displays no detail                     | Tracked    |
| 269 | TUI       | DATABASE_URL via argv visible in `ps`                   | Mitigated  |
| 270 | TUI       | No `--help` flag                                        | Mitigated  |
| 271 | Boot      | `pg_ctl start` blocks if already running                | Mitigated  |
| 272 | Boot      | Arbitrary sleeps; no readiness probe                    | Mitigated  |
| 273 | Boot      | `pm2 resurrect` race with daemon startup                | Mitigated  |
| 274 | Boot      | `pkill sshd; sshd` race                                 | Mitigated  |
| 275 | Boot      | No `set -uo pipefail`                                   | Mitigated  |
| 276 | Boot      | `boot.log` not in log-rotate watch list                 | Mitigated  |
| 277 | Boot      | No wake-lock release on shutdown                        | Tracked    |
| 278 | Install   | No source-script exists check                           | Mitigated  |
| 279 | Install   | `cp` requires re-install on every edit                  | Documented |
| 280 | Install   | No Termux:Boot APK presence check                       | Mitigated  |
| 281 | Status    | `chmod 644` ignored if file is symlinked                | Mitigated (graceful) |
| 282 | Status    | `crontab \| tr` semicolons break JSON                   | Mitigated  |
| 283 | Status    | `ifconfig` deprecated                                   | Mitigated  |
| 284 | Status    | No defensive shell flags                                | Mitigated  |
| 285 | Status    | Log content can JSON-inject via backslashes             | Mitigated  |
| 286 | Loadtest  | `parseInt(argv)` no validation — NaN loop               | Mitigated  |
| 287 | Loadtest  | Global dispatcher set unconditionally                   | Documented |
| 288 | Loadtest  | Ctrl-C leaves seeded data behind                        | Mitigated  |
| 289 | Loadtest  | Empty results array → NaN percentiles                   | Mitigated  |
| 290 | Loadtest  | Error body printed without redaction                    | Mitigated  |

## Mitigations Detail (Round 10)

### 261, 262, 269. TUI conninfo via env

Mitigated. `admin-tui` now consults `DATABASE_URL` from the
environment when no argv is supplied. Operators stop having to
write the password into shell history. When argv IS used, that's
explicit and we keep the behaviour.

### 263. `strncpy` null-terminate

Mitigated. `out->pg_version[sizeof(out->pg_version) - 1] = 0` after
every `strncpy`. The only caller that hits the cap is when the
PostgreSQL version banner exceeds 63 bytes (it doesn't today, but
defending against future Postgres upgrades or fork branding is
cheap).

### 265. SIGWINCH

Mitigated. `sigaction(SIGWINCH, ...)` flips a `volatile sig_atomic_t`
flag; the main loop calls `endwin()` + `refresh()` + `clear()` to
re-initialise ncurses dimensions on the next tick.

### 267. Cached PGconn in TUI

Mitigated. `g_conn` is opened once and reset via `PQreset()` on a
stale connection. Refresh tick now costs ~5 ms (six SELECT COUNT
queries) instead of ~70 ms (full TCP+startup handshake every time).

### 270. `--help` / `-h`

Mitigated. Standard usage banner. Returns 0.

### 271. Postgres readiness probe

Mitigated. `boot-script.sh` first checks `pg_ctl status`; if
running, it skips `start` entirely. After a fresh start it loops
on `pg_isready -h 127.0.0.1` for up to 20 seconds. The arbitrary
`sleep 4` is gone.

### 273. pm2 resurrect retry

Mitigated. The boot script retries `pm2 resurrect` up to 5 times
with a 2-second delay. Without this, a slow Android device that
hadn't started the pm2 daemon by the time the boot script ran
would leave the API process orphaned for the entire uptime
window until the operator manually intervened.

### 274. sshd idempotent

Mitigated. `pgrep -x sshd` gates the `sshd` invocation. The
previous `pkill sshd; sshd` pair had a brief window where SSH
was unreachable, which mattered if the operator was already
trying to connect from the next room.

### 276. boot.log + the rest in log-rotate

Mitigated. The `WATCH_LIST` in `log-rotate.sh` now covers all 16
known log files (`boot.log`, `replication.log`, `auth-cleanup.log`,
`battery.log`, `cert.log`, `time.log`, `email.log`,
`install-boot.log`, `rollback.log` joined the original 7).

### 278, 280. Install-boot guards

Mitigated. The installer fails fast if the source script is
missing or the destination directory is not writable. It also
emits a `[WARN]` log line if the `com.termux.boot` package is not
detected via `pm list packages`, so the operator knows the
autostart will never actually fire until the APK is installed.

### 282, 285. JSON escape in full-status

Mitigated. Replaced inline shell concatenation with a `jstr` sed
pipeline that escapes backslashes, double quotes, tabs, and
newlines. The dashboard can now ingest `full-status.json` even
when a log line contains `\` or embedded `"` characters that
previously broke `JSON.parse`.

### 283. `ip addr` over `ifconfig`

Mitigated. The deprecated `ifconfig` is the fallback; `ip -4 addr`
is preferred. Modern Termux ships only the iproute2 toolset.

### 286. Argv validation in loadtest

Mitigated. `parseInt32(s, fallback, name)` rejects NaN, zero,
negative, and absurd values (> 100000). A typo no longer creates
an infinite loop on the operator's machine.

### 288. SIGINT cleanup trap

Mitigated. `process.on('SIGINT', ...)` runs `seed-cleanup` with
the cached admin token before exit. Operators who Ctrl-C a long
stampede no longer leave thousands of `dev-stamp-...`
participants polluting the dashboard.

### 289. Empty results percentiles

Mitigated. `pct(p)` returns 0 for an empty array. Loadtest output
is no longer riddled with `NaN ms` on a run that crashed
immediately.

### 290. Body redaction

Mitigated. New `redact()` replaces `"password":"..."` and
`Bearer <tok>` with `***` before any error body lands in console
output. Both `run-stampede.js` and `chaos-checkin.js` use it.

## Tracked

* **264** Reduce ncurses CPU spin (poll() instead of timeout()).
* **266** `has_colors()` re-check on resize.
* **268** Display `PQerrorMessage()` text in the unreachable
  banner so operators can see "could not connect to ::1" vs
  "auth failed".
* **277** Wake-lock release on graceful pm2 stop.
* **279** Symlink instead of copy for boot script.
* **287** Per-call dispatcher (loadtest currently single-shot).

## Verification Matrix (Round 10)

| Capability                              | Test                                                       |
| --------------------------------------- | ---------------------------------------------------------- |
| TUI env DATABASE_URL                    | `unset args; DATABASE_URL=postgresql://... admin-tui` → connects |
| TUI cached PGconn                       | `strace -p <pid> -e connect 2>&1 \| head` → no new connects |
| TUI resize                              | `resize -s 30 100` while TUI is open → redraws cleanly      |
| Boot Postgres readiness                 | `pg_ctl stop; bash deploy/boot-script.sh` → script waits and reports HEALTHY |
| boot.log rotated                        | `truncate -s 11M $HOME/boot.log; bash deploy/log-rotate.sh` → boot.log.1.gz exists |
| install-boot APK warn                   | Run on a tablet without Termux:Boot → `[WARN]` line in log |
| full-status JSON valid                  | `bash deploy/full-status.sh && jq . console/full-status.json` → no parse error |
| run-stampede argv                       | `node run-stampede.js abc` → falls back to 2000, no infinite loop |
| run-stampede SIGINT cleanup             | Start, Ctrl-C → next `/stats/participants` shows no `dev-stamp-` rows |
| Loadtest body redaction                 | Inject a fake token in a 5xx response → output shows `Bearer ***` |
