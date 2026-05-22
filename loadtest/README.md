# Load Testing

Synthetic load test harness for the Event Platform API. Simulates concurrent
participant check-ins to validate end-to-end performance under realistic load.

## Test Scenario

The `stress.js` script:

1. Creates _N_ participant accounts via `POST /auth/register`.
2. Acquires an authentication token for each participant.
3. Issues _N_ concurrent `POST /attendance/checkin` requests against a
   single active session.
4. Aggregates results: success rate, throughput (req/s), latency
   percentiles (p50, p95, p99), and error breakdown by HTTP status.

## Usage

```bash
# 1. Authenticate as administrator
ADMIN_TOKEN=$(curl -sX POST https://api.example.com/auth/login \
    -H 'Content-Type: application/json' \
    -d '{"email":"admin@example.com","password":"admin-password"}' \
    | jq -r .token)

# 2. Create a fresh attendance session
SESSION=$(curl -sX POST https://api.example.com/sessions/create \
    -H "Authorization: Bearer $ADMIN_TOKEN" | jq -r .code)

# 3. Run stress test (500 concurrent check-ins)
node stress.js 500 https://api.example.com "$SESSION" "$ADMIN_TOKEN"
```

## Sample Output

```
🔥 Load test: 500 concurrent check-ins

   API:     https://api.example.com
   Session: aY4VGKTT

━━━ Phase 1: Creating users + getting tokens ━━━
  ✓ 500 users ready in 17.7s

━━━ Phase 2: Concurrent check-in stampede ━━━
  ✓ all done in 11220ms

━━━ Phase 3: Results ━━━

  Total requests: 500
  Successful:     485  (97.0%)
  Throughput:     43 req/s

  Latency (ms):
    avg:  3287
    p50:  2841
    p95:  9012
    p99:  10884
```

## Cleanup

After load tests, remove generated data to avoid bloating the production
database:

```sql
DELETE FROM "Attendance" WHERE device_id LIKE 'loadtest-%';
DELETE FROM "Device"     WHERE device_uuid LIKE 'loadtest-%';
DELETE FROM "Participant" WHERE email      LIKE 'loadtest%';
```

## Bottleneck Analysis

The single-threaded C server can sustain approximately 140 req/s when
benchmarked against the local interface (LAN). When traffic is routed
through Cloudflare Tunnel (free tier), throughput drops to approximately
45 req/s due to per-tunnel concurrency limits. This is the documented
trade-off of free-tier tunneling.

For production-grade throughput, either:
- Subscribe to Cloudflare Tunnel paid tier with higher concurrency, or
- Replace the tunnel with a self-hosted reverse proxy (Nginx) on a host
  with a public IP.

For the typical event use case (≤ 2,000 simultaneous check-ins over a
1-minute window), the offline-first PWA client transparently queues and
synchronises check-ins, smoothing the request profile to a sustainable
~50 req/s sustained load. No server changes required.
