# Load Testing

Synthetic load test harness for the Event Platform API. Validates the
primary project goal: **2,000 attendees checking in simultaneously with
zero errors**.

## Test Scripts

| Script | Purpose |
|--------|---------|
| `run-stampede.js` | Primary 2,000-concurrent stampede against `/attendance/quick-checkin`. Self-contained: seeds participants + devices, creates a session, fires the parallel burst, prints percentiles, and cleans up. |
| `stress.js` | Legacy stress harness against `/attendance/checkin` (auth-token based). Retained for the historical comparison documented below. |
| `setup-session.js` | Helper that logs in as admin, ensures an event exists, and creates a fresh attendance session. Prints the session code on stdout. |
| `seed.sql` | Direct SQL seeder for batch participant + device creation when API access is unavailable. |
| `latency-probe.sh` | Measures sequential and concurrent latency on the tablet itself for performance regression detection. |

## Reference Result (LAN)

The reference workload — 2,000 concurrent check-ins fired from a laptop
on the same Wi-Fi — completes consistently in approximately two seconds
with no failures.

```
� Orchestrated stampede: 2000 concurrent

   API:    http://192.168.100.67:3001
   Run id: rmpgvv983

  ✓ admin logged in
  ✓ seeded 2000 participants, 2000 devices in 0.235s
  ✓ id range: 14175..16174
  ✓ session 8pJcSq3z

━━━ STAMPEDE: firing 2000 parallel /attendance/quick-checkin ━━━
  ✓ all settled in 2021ms

━━━ RESULTS ━━━
  Total:        2000
  Successful:   2000  (100.0%)
  Errors:       0
  Throughput:   990 req/s
  Latency ms:   min=101 avg=327 p50=222 p95=1040 p99=1060 max=1179

  ✅ PASS: 100.00% accepted
```

## Architecture That Makes This Possible

Three changes converted the original ~50% pass rate into a 100% pass
rate at 2,000 concurrent:

1. **`/attendance/quick-checkin` endpoint.** Resolves device UUID,
   session code, and attendance insertion as a single SQL CTE, removing
   one round-trip per request and bypassing the JWT verify path. The
   PWA uses this endpoint exclusively after the device has been linked
   once.
2. **Pre-fork worker pool.** The C server forks 8 workers (matching
   the 8-core Unisoc T618) which share the listening socket. The
   kernel load-balances `accept()` across them. The supervisor process
   restarts dead workers automatically.
3. **Per-worker persistent libpq connection.** Each worker keeps one
   `PGconn` open across requests. This eliminates the
   ~50 ms TCP/handshake cost on the hot path; concurrent end-to-end
   latency drops from ~75 ms to ~10 ms.

## Usage — Quick Stampede

```bash
# Targeted run — admin credentials picked up from env or default
node run-stampede.js 2000 http://192.168.100.67:3001
```

The script orchestrates the whole flow:

1. Logs in as admin (single auth call, no rate limit pressure).
2. Calls `POST /participants/seed-stamp` to bulk-create N participants
   and pre-link N devices in a single SQL transaction.
3. Calls `POST /sessions/create` for a fresh attendance session.
4. Fires N parallel `POST /attendance/quick-checkin` requests, batching
   into rolling flights of 500 to avoid local FD exhaustion.
5. Prints the success rate, throughput, and latency percentiles.
6. Calls `POST /participants/seed-cleanup` to remove the synthetic data.

## Bottleneck Analysis

| Path | Throughput | Pass rate at N=2,000 | Note |
|------|-----------|---------------------|------|
| Direct LAN | ~990 req/s | 100 % | Reference target |
| Cloudflare Tunnel (free tier) | ~45 req/s | ~57 % | Tunnel per-account concurrency cap |
| Direct LAN (older single-thread server) | ~140 req/s | partial | Pre-fork pool change required |

The Cloudflare Tunnel free tier caps concurrent connections per account
at a level well below the burst envelope of a 2,000-attendee event. Two
options exist for events that want to use the tunnel:

- The PWA enqueues check-ins offline-first; the queue drains via
  `POST /attendance/batch-checkin` when the tunnel has spare capacity.
  This converts the burst into a sustained ~50 req/s rate which the
  free tunnel handles comfortably.
- Subscribe to the Cloudflare Tunnel paid tier or front the tablet
  with a self-hosted reverse proxy such as Nginx; see
  `docs/REVERSE_PROXY.md`.

## Cleanup

`run-stampede.js` cleans up synthetic data automatically. For manual
cleanup of seeds left behind by interrupted runs:

```sql
DELETE FROM "Attendance" WHERE participant_id IN (
    SELECT id FROM "Participant" WHERE email LIKE 'stamp-%@test.local'
);
DELETE FROM "Device"      WHERE device_uuid LIKE 'dev-stamp-%';
DELETE FROM "Participant" WHERE email LIKE 'stamp-%@test.local';
```
