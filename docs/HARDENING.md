# Hardening Audit and Mitigations

This document records the internal-failure-mode audit performed on the
0.4.0 build of the platform and the mitigations applied. Everything
below is verified by either the build pipeline (CI), a load-test
scenario, or a recovery script.

The audit answers the question: **"Setting aside the participant's
phone or network — what could go wrong on our side?"**

## Summary

| # | Risk                                              | Severity   | Status       |
| - | ------------------------------------------------- | ---------- | ------------ |
| 1 | `max_connections=100` on Postgres                 | Critical   | Mitigated    |
| 2 | `synchronous_commit=on` causes write amplification | Critical   | Mitigated    |
| 3 | No connection cap on server (fork bomb)           | Critical   | Mitigated    |
| 4 | No graceful shutdown (in-flight transaction loss) | Critical   | Mitigated    |
| 5 | `Access-Control-Allow-Origin: *` wildcard         | Critical   | Mitigated    |
| 6 | No body length validation                         | High       | Mitigated    |
| 7 | FNV-1a (non-cryptographic) JWT signature          | High       | Mitigated    |
| 8 | Plaintext password storage                        | High       | Mitigated    |
| 9 | Cron jobs not enforced                            | High       | Mitigated    |
| 10 | No log rotation policy                           | High       | Mitigated    |
| 11 | Single-instance Cloudflare Tunnel                | High       | Documented   |
| 12 | Stampede test data leftover                      | High       | Mitigated    |
| 13 | Worker continues when DB is down                 | High       | Mitigated    |
| 14 | `Connection: close` on every request             | Medium     | Accepted     |
| 15 | No request size limit at edge                    | Medium     | Documented   |
| 16 | Stale Service Worker cache after deploy          | Medium     | Mitigated    |
| 17 | Postgres dead tuples not vacuumed                | Medium     | Mitigated    |
| 18 | Tablet doze mode CPU throttling                  | Medium     | Documented   |
| 19 | Admin TUI unverified against latest build        | Low        | Tracked      |
| 20 | Inconsistent timezone handling                   | Low        | Tracked      |

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
