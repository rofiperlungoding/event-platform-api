# Event Platform — API Server

A self-hosted attendance platform built around a single-binary HTTP server
written in C. Designed and verified to absorb a 2,000-attendee
simultaneous check-in burst on a 3 GB Android tablet.

| Property                | Value                                       |
| ----------------------- | ------------------------------------------- |
| Reference host          | Galaxy Tab A8, Unisoc T618, 3 GB RAM        |
| Runtime                 | Termux (no container, no proot)             |
| Concurrency model       | Pre-fork worker pool, 8 workers             |
| Database                | PostgreSQL 18                               |
| Public ingress          | Cloudflare Tunnel                           |
| Verified peak           | **2,000 concurrent / 1.8 s / 100 % pass**   |
| Sustained throughput    | ~1,000 req/s LAN                            |
| Median check-in latency | ~380 ms                                     |
| Binary size             | ~80 KB                                      |

Public endpoints:

- API   `https://api.rofidoesthings.site`
- UI    `https://console.rofidoesthings.site`

## Documentation

| Document                       | Purpose                                                                |
| ------------------------------ | ---------------------------------------------------------------------- |
| [`docs/INSTALL.md`](docs/INSTALL.md)         | Bootstrap procedure: Termux, Postgres, build, autostart, tunnel       |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Design rationale, pre-fork pool, persistent connection cache         |
| [`docs/API.md`](docs/API.md)                  | Complete REST + WebSocket reference with request/response schemas    |
| [`docs/REVERSE_PROXY.md`](docs/REVERSE_PROXY.md) | Optional Nginx ingress for hosts with a public IP                  |
| [`docs/REPLICATION.md`](docs/REPLICATION.md)  | Logical replication to Supabase for off-site backup                  |
| [`deploy/README.md`](deploy/README.md)        | Operational scripts: deploy, rollback, watchdog, backup              |
| [`loadtest/README.md`](loadtest/README.md)    | Load test methodology and the verified 2,000-concurrent run         |
| [`tui/README.md`](tui/README.md)              | Curses-based admin dashboard                                         |

## Repository Layout

```
event-platform-api/
├── server.c               # Single-file C HTTP server (~3 kLOC)
├── README.md              # This file
├── .github/workflows/     # CI: build verification
├── deploy/                # Operational shell scripts
├── docs/                  # Architecture, API, install, replication, proxy
├── loadtest/              # Stampede harness, latency probe
├── migrations/            # PostgreSQL schema (numbered, append-only)
└── tui/                   # ncurses admin dashboard
```

## Quick Start (Reference Tablet)

```bash
# Build
cc -O2 -o event-server server.c \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lpq

# Run
PORT=3001 \
DATABASE_URL=postgresql://rofi:devsecret@localhost:5432/eventplatform \
JWT_SECRET=intrivia2026secret \
WEBHOOK_SECRET=intriviadeploy2026 \
STATIC_DIR=/data/data/com.termux/files/home/projects/event-platform-console \
./event-server
```

The server forks 8 workers that share the listening socket. The
supervisor process restarts dead workers automatically. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for design detail.

## Verified Capacity

The reference workload — 2,000 attendees firing
`POST /attendance/quick-checkin` in parallel from a single client over
LAN — completes consistently in approximately two seconds:

```
━━━ STAMPEDE: firing 2000 parallel /attendance/quick-checkin ━━━
  ✓ all settled in 1841ms

━━━ RESULTS ━━━
  Total:        2000
  Successful:   2000  (100.0%)
  Errors:       0
  Throughput:   1086 req/s
  Latency ms:   min=122 avg=404 p50=382 p95=987 p99=1355 max=1367
  ✅ PASS: 100.00% accepted
```

Reproducible via `node loadtest/run-stampede.js 2000 http://<tablet-lan-ip>:3001`.
Method and architecture choices documented in
[`loadtest/README.md`](loadtest/README.md).

## Auto-Deploy

A push to `main` triggers GitHub Actions which sends a webhook to the
tablet. The webhook calls `deploy/deploy-api.sh` which performs:

1. `git pull --ff-only`
2. Recompile to `event-server-staging`
3. Smoke test on a random port
4. Atomic swap, pm2 restart with environment, `pm2 save`
5. Post-deploy health check; rollback on failure

Manual trigger:

```bash
curl -X POST "https://api.rofidoesthings.site/deploy/api?secret=<WEBHOOK_SECRET>"
```

## License

MIT.
