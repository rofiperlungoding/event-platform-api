# Architecture

## System Topology

```
┌─────────────────────────────────────────────────────────────┐
│                          Internet                           │
└──────────────────────────┬──────────────────────────────────┘
                           │ QUIC
                ┌──────────▼──────────┐
                │  Cloudflare Edge    │
                └──────────┬──────────┘
                           │ cloudflared (named tunnel)
┌──────────────────────────▼───────────────────────────────────┐
│           Galaxy Tab A8 — Termux (Android)                   │
│                                                              │
│   pm2 ─┬─ event-tunnel  (cloudflared)                        │
│        └─ event-server  (C HTTP server)                      │
│                │                                             │
│                ├─ supervisor process                         │
│                └─ 8 forked workers (shared listening fd)     │
│                                                              │
│   Postgres 18 (native Termux service)                        │
│   Static files (event-platform-console)                      │
└──────────────────────────────────────────────────────────────┘
```

A single tablet hosts the entire stack: HTTP server, application
database, and static frontend. Public ingress is provided by
Cloudflare Tunnel; LAN ingress is on port 3001 directly.

## Request Lifecycle

The C server uses a pre-fork worker pool. The supervisor process binds
the listening socket once, then forks N workers that all inherit the
same socket. The Linux kernel distributes accept() calls across the
workers, eliminating the thundering-herd problem and providing fair
load balancing without explicit synchronisation.

```
┌─────────────┐
│  supervisor │  bind(), listen(), fork x N
└──────┬──────┘
       │
   ┌───┴───┬───────┬───────┬───────┐
   ▼       ▼       ▼       ▼       ▼
┌────┐  ┌────┐  ┌────┐  ┌────┐  ┌────┐
│ W0 │  │ W1 │  │ W2 │  │ ...│  │ W7 │   <- accept() round-robin
└─┬──┘  └─┬──┘  └─┬──┘  └─┬──┘  └─┬──┘
  │       │       │       │       │
  └─ each worker holds one persistent libpq connection
```

Each worker runs a synchronous accept loop. On accept it:

1. Reads the request into a 16 KB buffer with a 30 s receive timeout.
2. Parses method, path, headers, body.
3. Resolves the client IP from `CF-Connecting-IP` if present.
4. Applies rate-limit policy (auth-strict, attendance-unlimited).
5. Dispatches to the route handler.
6. Writes the response, closes the connection.

Long-lived endpoints (WebSocket live feed, deploy webhook) fork a
detached child so the worker returns to the accept loop immediately.

## Performance Architecture

Three architectural choices together carry the platform from a
single-threaded ~50 % pass rate at 2,000 concurrent to a 100 % pass
rate at 2,000 concurrent in 1.8 seconds:

| Layer            | Optimisation                          | Effect                                  |
| ---------------- | ------------------------------------- | --------------------------------------- |
| Process model    | Pre-fork pool (8 workers)             | Saturates all 8 cores, removes spawn cost |
| DB layer         | Per-worker persistent libpq connection | Eliminates ~50 ms TCP handshake per request |
| Application code | Single-CTE check-in path              | One round trip resolves device, session, attendance |

### Pre-fork Worker Pool

`fork()` is called N times before any request arrives. Each child
inherits the listening socket fd. The kernel hands each new connection
to whichever worker calls `accept()` first. This mirrors the design of
Apache `prefork` and Nginx workers but in ~80 lines of code. The
supervisor reaps dead children with `wait()` and respawns
replacements.

Worker count defaults to 8 (matching the Unisoc T618 core count) and is
overridable via `WORKERS` environment variable. Smoke tests during
deploy run the binary with `WORKERS=1` so the cleanup signal can target
a single PID.

### Persistent libpq Connection

Every handler uses `db_acquire()` which returns the worker's cached
`PGconn`. The first call creates the connection; subsequent calls
return the same handle. If the connection is found stale (server
restart, network blip), it is closed and reopened transparently. The
pattern eliminates the dominant per-request cost on the hot path.

Because each worker is single-threaded, no mutex is required — the
connection is exclusively owned by one worker process. WebSocket
handlers, which run in a forked grandchild, explicitly invalidate the
inherited cache and open a fresh connection.

### Single-CTE Check-in

The hot endpoint `POST /attendance/quick-checkin` uses one SQL
statement to resolve the device-to-participant mapping, validate the
session, and insert the attendance row:

```sql
WITH d AS (SELECT participant_id FROM "Device" WHERE device_uuid = $1),
     s AS (SELECT id FROM "Session" WHERE code = $2 AND active = true AND expires_at > NOW())
INSERT INTO "Attendance" (participant_id, session_id, device_id, "checkedInAt")
SELECT d.participant_id, s.id, $1, NOW() FROM d, s
RETURNING id, participant_id, session_id, "checkedInAt"
```

A duplicate insertion produces SQLSTATE 23505 which the handler maps
to `409 Conflict`. An empty result indicates either an unknown device
or an inactive session; the handler distinguishes between the two with
one cheap follow-up query.

## Rate Limiting

A fixed-window per-IP token bucket lives in shared memory across the
worker pool (each worker has its own table; rate-limited bursts are
spread across all workers, which is acceptable since the policy is a
soft floor).

| Path prefix       | Limit          | Reason                                            |
| ----------------- | -------------- | ------------------------------------------------- |
| `/auth/...`       | 10 / minute    | Anti brute-force                                  |
| `/attendance/...` | unlimited      | Primary path; venue NAT means all attendees share an IP |
| `/ws/...`         | unlimited      | Long-lived; not per-request                       |
| `/deploy/...`     | unlimited      | Authenticated by shared secret                    |
| everything else   | 600 / minute   | Admin operations; generous default                |

Returns `429 Too Many Requests` with `Retry-After: 60` when exceeded.

## Authentication and Authorisation

- Bearer-token JWT with HMAC FNV-1a signature.
- 24-hour expiry encoded in the payload.
- `role` claim distinguishes `admin` from `participant`.
- Device identity is established with `POST /device/link` and
  thereafter the PWA uses device UUID directly via
  `/attendance/quick-checkin` — eliminating the JWT round trip on the
  hot path.

The signature scheme is intentionally simple (FNV-1a, not HMAC-SHA-256)
because the threat model is a closed campus event, not the public
internet. The shared secret is rotated by editing `JWT_SECRET` in the
process environment and restarting.

## Storage

PostgreSQL 18 runs natively under Termux. The schema is in
`migrations/` and is append-only (numbered, never edited in place).
Critical tables:

- `Participant(id, name, email, team, password_hash, role)`
- `Event(id, slug, name, description, starts_at, ends_at)`
- `Session(id, code, event_id, expires_at, active)`
- `Device(device_uuid, participant_id, user_agent, linkedAt)`
- `Attendance(id, participant_id, session_id, device_id, checkedInAt)`
  with `UNIQUE(participant_id, session_id)`

Indexes are defined for the hot lookup paths: `Device(device_uuid)`,
`Session(code)`, `Attendance(session_id)`,
`Attendance(participant_id, session_id)`.

A daily logical dump runs at 03:00 local time via cron; see
[`deploy/README.md`](../deploy/README.md). Off-site replication to
Supabase is documented in [`REPLICATION.md`](REPLICATION.md).

## Failure Modes and Mitigations

| Failure                   | Detection                  | Mitigation                                      |
| ------------------------- | -------------------------- | ----------------------------------------------- |
| Worker crash              | `wait()` in supervisor     | Auto-respawn within seconds                     |
| Postgres connection bad   | `PQstatus()` in db_acquire | Transparent reconnect on next request           |
| Tablet reboot             | Termux:Boot                | Boot script restarts pm2 + cloudflared          |
| API binary corrupted      | Watchdog cron              | `rollback.sh` restores last known good          |
| Unhealthy beyond watchdog | `health-watchdog.sh`       | Email alert to operator                         |
| Cloudflare Tunnel limit   | Client retry exhaustion    | PWA falls back to LAN endpoint, then offline queue |
| Network partition         | Service Worker             | IndexedDB queue drains via batch-checkin on reconnect |

## Capacity Envelope

| Workload                              | Limit                       | Notes                                         |
| ------------------------------------- | --------------------------- | --------------------------------------------- |
| Concurrent quick-checkins (LAN)       | ≥ 2,000 / 1.8 s @ 100 %     | Verified; reference benchmark                 |
| Sustained quick-checkins (LAN)        | ~1,000 req/s                | CPU-bound on the worker pool                  |
| Concurrent via Cloudflare free tunnel | ~45 req/s                   | Free tier per-account ingress limit           |
| WebSocket clients                     | ~30 simultaneous            | Forked per-connection; bounded by FD limit    |
| Database working set                  | < 100 MB at 2,000 attendees | Small enough to live in OS page cache         |

## Why C, Not a Higher-Level Runtime

The original implementation was Fastify on Node + Prisma. Two failures
forced the rewrite:

1. **Prisma's binary engine is not built for ARMv8 Android** — it
   runtime-loaded an x86_64 ELF on the tablet and crashed.
2. **Memory ceiling** — Node + Prisma + libuv resident set was over
   200 MB, leaving little headroom for Postgres on a 3 GB device.

The C rewrite uses libpq directly, has no framework overhead, and the
binary is ~80 KB. Build time is under a second. The tradeoffs (manual
JSON parsing, manual route dispatch) are localised to a single file
that fits in memory and reads top-to-bottom.

## File Inventory

| File                       | Purpose                                       |
| -------------------------- | --------------------------------------------- |
| `server.c`                 | All application code: routes, handlers, accept loop |
| `migrations/00X_*.sql`     | Append-only schema migrations                 |
| `deploy/*.sh`              | Operational scripts (deploy, rollback, watchdog, backup) |
| `loadtest/run-stampede.js` | Reference 2,000-concurrent load test          |
| `tui/admin-tui.c`          | ncurses live admin dashboard                  |
