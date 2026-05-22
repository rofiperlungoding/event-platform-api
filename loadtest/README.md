# Load Testing

Reference workload for the platform's primary capacity goal:
**2,000 attendees checking in simultaneously, zero errors**.

## Files

| File                  | Purpose                                                                          |
| --------------------- | -------------------------------------------------------------------------------- |
| `run-stampede.js`     | One-shot orchestrator: seeds participants and devices, creates a session, fires the parallel burst, prints percentiles, cleans up. |
| `latency-probe.sh`    | Single-request and 100-concurrent baseline latency probe (run on the tablet).    |
| `ws-test.js`          | WebSocket live-feed connectivity check.                                          |
| `wipe-all.sql`        | Reset the entire database to a single admin row; resets all serial sequences.    |
| `package.json`        | One dependency (`undici` for the high-concurrency dispatcher) and `ws`.          |

## Quick Start

From a laptop on the same Wi-Fi as the tablet:

```bash
cd loadtest
npm install
node run-stampede.js 2000 http://<tablet-lan-ip>:3001
```

Default admin credentials are picked up from `ADMIN_EMAIL` and
`ADMIN_PASSWORD` (defaults match the reference deployment).

## Inspecting the Run in the Dashboard

By default the harness cleans up its synthetic data after the run.
Set `NO_CLEANUP=1` to keep the seeded participants, devices, and
attendance records visible in the operational dashboard:

```bash
NO_CLEANUP=1 node run-stampede.js 2000 http://<tablet-lan-ip>:3001
```

The `run_id` printed at the end can be passed back to
`POST /participants/seed-cleanup` later, or the entire database can be
wiped (admin row preserved) via:

```bash
psql -h 127.0.0.1 -U rofi -d eventplatform -f wipe-all.sql
```

## Reference Result

```
💥 Orchestrated stampede: 2000 concurrent

   API:    http://192.168.100.67:3001
   Run id: rmpgvv983

  ✓ admin logged in
  ✓ seeded 2000 participants, 2000 devices in 0.235s
  ✓ session 8pJcSq3z

━━━ STAMPEDE: firing 2000 parallel /attendance/quick-checkin ━━━
  ✓ all settled in 1841ms

━━━ RESULTS ━━━
  Total:        2000
  Successful:   2000  (100.0%)
  Errors:       0
  Throughput:   1086 req/s
  Latency ms:   min=122 avg=404 p50=382 p95=987 p99=1355 max=1367
  Attempts:     1x:2000

  ✅ PASS: 100.00% accepted
```

## Methodology

`run-stampede.js` runs five phases:

1. **Authenticate** as administrator via `POST /auth/login`.
2. **Seed N participants and devices** in a single SQL transaction via
   `POST /participants/seed-stamp`. The endpoint is admin-only and
   exists exclusively for this harness.
3. **Create a fresh attendance session** via `POST /sessions/create`.
4. **Stampede.** Fire N parallel `POST /attendance/quick-checkin`
   requests. Concurrency is gated to 500 in flight to avoid local file
   descriptor exhaustion. Each request retries with jittered
   exponential backoff up to five times. Retries are safe because the
   server enforces `UNIQUE(participant_id, session_id)` — duplicate
   inserts collapse to `409 Conflict` and are counted as success.
5. **Cleanup** via `POST /participants/seed-cleanup`.

The harness uses Node's built-in `fetch` with an `undici` dispatcher
configured for 2,000 concurrent connections so that client-side socket
exhaustion does not mask server behaviour.

## Architecture That Makes 2,000 Possible

| Layer            | Optimisation                                  | Effect                                      |
| ---------------- | --------------------------------------------- | ------------------------------------------- |
| Process model    | Pre-fork pool of 8 workers                    | Saturates all 8 cores; removes spawn cost   |
| DB layer         | Per-worker persistent libpq connection        | Eliminates ~50 ms TCP handshake per request |
| Application code | Single-CTE quick-checkin                      | One DB round trip resolves device, session, attendance |
| Client behaviour | PWA prefers LAN; tunnel is fallback only      | Bypasses Cloudflare free-tier ingress cap   |
| Resilience       | Idempotent retry + offline IndexedDB queue    | Transient failures recover transparently    |

Detailed in [`docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md).

## Capacity by Path

| Path                                | N    | Pass    | Throughput   | Notes                                                 |
| ----------------------------------- | ---- | ------- | ------------ | ----------------------------------------------------- |
| Direct LAN                          | 2000 | 100.00% | ~1,086 req/s | Reference target                                      |
| Direct LAN (single-thread baseline) | 500  | partial | ~140 req/s   | Pre-pool collapse point                               |
| Cloudflare Tunnel free tier         | 2000 | ~60%    | ~21 req/s    | Account-level concurrent connection cap; not a server limit |

For events where some attendees are off-network, the PWA queues
locally and drains via `POST /attendance/batch-checkin` once
connectivity returns. This converts a burst into a sustained
~50 req/s, which the free-tier tunnel handles comfortably.

## Manual Cleanup

`run-stampede.js` cleans up after a successful run unless `NO_CLEANUP=1`
was set. For interrupted runs, drop synthetic data manually:

```sql
DELETE FROM "Attendance" WHERE participant_id IN (
    SELECT id FROM "Participant" WHERE email LIKE 'stamp-%@test.local'
);
DELETE FROM "Device"      WHERE device_uuid LIKE 'dev-stamp-%';
DELETE FROM "Participant" WHERE email LIKE 'stamp-%@test.local';
```

To wipe the entire database back to admin-only state (resets all
sequences too), run [`wipe-all.sql`](wipe-all.sql).
