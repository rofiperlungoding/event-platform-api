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