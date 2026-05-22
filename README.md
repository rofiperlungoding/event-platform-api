# Event Platform API

A self-hosted event registration and attendance platform implemented as a
single-binary HTTP server written in C. Designed to operate from constrained
hardware (Android tablets running Termux) and exposed to the public internet
via Cloudflare Tunnel.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Component Reference](#component-reference)
4. [Operational Model](#operational-model)
5. [API Specification](#api-specification)
6. [Deployment](#deployment)
7. [Operations and Recovery](#operations-and-recovery)
8. [Repository Layout](#repository-layout)
9. [Development](#development)
10. [Security Considerations](#security-considerations)

---

## Overview

The platform supports the lifecycle of a small-to-medium event (target capacity:
~2,000 participants) including:

- Participant registration and authentication
- Time-bound attendance sessions identified by short-lived QR codes
- Device-bound participant identity (one device, one account)
- Real-time attendance feed for organisers
- Offline-first check-in flow with eventual synchronisation

The reference deployment hosts the entire platform — application, database,
and static frontend — on a single Android tablet, exposed publicly through
a Cloudflare Tunnel. This unconventional deployment was deliberately chosen
to demonstrate that disciplined design (offline-first clients, careful
resource management) can run a real production workload on minimal
hardware.

### Production URLs (Reference Deployment)

| Service       | URL                                 |
| ------------- | ----------------------------------- |
| API           | `https://api.rofidoesthings.site`   |
| Console UI    | `https://console.rofidoesthings.site` |
| Admin QR page | `/attend/admin.html`                |
| Scanner PWA   | `/attend/scan.html`                 |

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Internet                                  │
└────────────────┬─────────────────────────────────────────────────┘
                 │
                 ▼
┌──────────────────────────────────────────────────────────────────┐
│                   Cloudflare Edge (sin20)                         │
│                                                                   │
│  api.rofidoesthings.site     console.rofidoesthings.site         │
└────────────────┬─────────────────────────────────────────────────┘
                 │  QUIC tunnel (cloudflared)
                 ▼
┌──────────────────────────────────────────────────────────────────┐
│                Tablet Host (Termux)                               │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  cloudflared (event-tunnel)                              │    │
│  │  - Routes both hostnames to localhost:3001               │    │
│  └────────────────┬─────────────────────────────────────────┘    │
│                   ▼                                                │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  event-server (C, ~55 KB binary)                         │    │
│  │  - HTTP/1.1 server, single-threaded                      │    │
│  │  - REST API endpoints                                    │    │
│  │  - Static file serving (frontend assets)                 │    │
│  │  - Webhook deployment automation                         │    │
│  └────────────────┬─────────────────────────────────────────┘    │
│                   │ libpq                                          │
│                   ▼                                                │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  PostgreSQL 18 (native Termux package)                   │    │
│  │  - Persistent data: participants, sessions, attendance,  │    │
│  │    devices                                                │    │
│  └──────────────────────────────────────────────────────────┘    │
│                                                                   │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  Process supervision: pm2                                 │    │
│  │  Scheduling: cron (cronie) — backup + watchdog           │    │
│  │  Boot: Termux:Boot launcher                              │    │
│  └──────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘
```

### Design Decisions

| Decision                      | Rationale                                              |
| ----------------------------- | ------------------------------------------------------ |
| Single-binary C server        | Minimal resource footprint; ~5 MB resident memory.    |
| Single-threaded model         | Eliminates classes of concurrency bugs (zombies, locks). |
| Per-request DB connection     | Simpler than connection pooling; acceptable at < 50 req/s. |
| Offline-first PWA client      | Smooths request bursts; server sees sustained load.   |
| Cloudflare Tunnel             | Public TLS-terminated HTTPS without static IP.        |
| pm2 supervision               | Auto-restart on crash; max-restart limit prevents loops.|
| Cron-based health watchdog    | Self-healing: detect failure, restart, fall back.     |

---

## Component Reference

### `server.c`

The single source file producing the `event-server` binary. Implements:

- HTTP/1.1 request parsing
- JSON response formatting (with growable string buffer to prevent
  fixed-buffer overflow on large result sets)
- PostgreSQL access via `libpq` (parameterised queries, prepared statements
  where applicable)
- FNV-1a-based authentication token generation and verification
- Static file serving with SPA fallback and MIME detection
- Webhook routing for deployment automation

**Build:**

```bash
cc -O2 -Wall -o event-server server.c -lpq
```

**Run:**

```bash
PORT=3001 \
DATABASE_URL='postgresql://user:pass@localhost:5432/eventplatform' \
STATIC_DIR=/path/to/frontend \
JWT_SECRET=<secret> \
WEBHOOK_SECRET=<secret> \
./event-server
```

### `migrations/`

SQL schema migrations. Applied manually with `psql`:

```bash
psql -U rofi -d eventplatform -f migrations/002_auth_attendance.sql
```

### `deploy/`

Operational shell scripts. See [`deploy/README.md`](deploy/README.md) for
detailed documentation of each script.

### `loadtest/`

Synthetic load testing harness. See [`loadtest/README.md`](loadtest/README.md).

### `.github/workflows/ci.yml`

GitHub Actions workflow that compiles `server.c` on every push and validates
shell script syntax. Acts as a baseline correctness gate before changes
reach the auto-deploy webhook.

---

## Operational Model

### Auto-Deployment

The platform supports zero-touch deployment driven by GitHub webhooks:

1. Developer pushes to `main` branch.
2. GitHub fires a webhook to `https://api.rofidoesthings.site/deploy/api?secret=<secret>`.
3. The receiving server forks a child process and executes
   `deploy/deploy-api.sh`.
4. The deploy script:
   - Pulls the latest source from origin.
   - Compiles to a staging binary.
   - Performs a smoke test by running the staging binary on port 3099 and
     issuing a `GET /health` request.
   - **Only if the smoke test passes**, archives the current binary and
     atomically swaps in the new one.
   - Restarts the `event-server` pm2 process.
   - Performs a final health check.

A failed compilation or smoke test leaves the running binary untouched.

### Health Watchdog

A cron job runs every five minutes (`*/5 * * * *`) and:

1. Issues `GET /health` against `localhost:3001`.
2. On failure, attempts `pm2 restart event-server`.
3. On continued failure, executes `deploy/rollback.sh` which:
   - Selects the most recent archived binary.
   - Replaces the current binary with the archive.
   - Restarts the process.

This provides a self-healing loop with a worst-case detection time of
five minutes. The system tolerates one bad deploy without operator
intervention.

### Backup Policy

A nightly cron job (`0 3 * * *`) executes `deploy/db-backup.sh`:

1. Runs `pg_dump` of the `eventplatform` database.
2. Compresses output with `gzip`.
3. Writes to `~/backups/eventplatform_<timestamp>.sql.gz`.
4. Retains the most recent seven days of backups; older files are deleted.

Backup output is logged to `~/backup.log`.

### Boot Recovery

A Termux:Boot script at `~/.termux/boot/start-event-stack` runs on device
boot and:

1. Acquires a wake-lock (`termux-wake-lock`) to prevent CPU sleep.
2. Starts PostgreSQL.
3. Starts the SSH daemon for remote operator access.
4. Starts the cron daemon.
5. Restores the pm2 process registry (`pm2 resurrect`), which restarts
   `event-server` and `event-tunnel`.
6. Records the boot outcome to `~/boot.log`.

---

## API Specification

All endpoints accept and return JSON, with permissive CORS headers
(`Access-Control-Allow-Origin: *`).

### Authentication

| Method | Endpoint           | Auth     | Description                              |
| ------ | ------------------ | -------- | ---------------------------------------- |
| POST   | `/auth/register`   | Public   | Create participant; returns auth token.  |
| POST   | `/auth/login`      | Public   | Verify credentials; returns auth token.  |
| GET    | `/auth/me`         | Bearer   | Return current authenticated user.       |

### Sessions (Administrator Only)

| Method | Endpoint                | Description                              |
| ------ | ----------------------- | ---------------------------------------- |
| POST   | `/sessions/create`      | Generate ad-hoc attendance session.      |
| POST   | `/sessions/scheduled`   | Create named session with title and time window. |
| GET    | `/sessions/active`      | List sessions that have not expired.     |
| GET    | `/sessions/:id`         | Session detail with attendance count.    |
| POST   | `/sessions/:id/refresh` | Rotate session code; extend expiry.      |

### Attendance

| Method | Endpoint                       | Auth   | Description                            |
| ------ | ------------------------------ | ------ | -------------------------------------- |
| POST   | `/attendance/checkin`          | Bearer | Record check-in for current user.      |
| GET    | `/attendance/session/:id`      | Bearer | List check-ins for a session.          |
| GET    | `/attendance/session/:id/export` | Admin | Download attendance as CSV.            |
| GET    | `/attendance/me`               | Bearer | Authenticated user's attendance log.   |

### Device Identity

| Method | Endpoint              | Auth     | Description                              |
| ------ | --------------------- | -------- | ---------------------------------------- |
| POST   | `/device/link`        | Bearer   | Associate a device UUID with the user.   |
| POST   | `/device/identify`    | Public   | Resolve a device UUID to its owner.      |

### Diagnostics

| Method | Endpoint              | Description                             |
| ------ | --------------------- | --------------------------------------- |
| GET    | `/health`             | Liveness probe.                         |
| GET    | `/health/detailed`    | Health with database latency.           |
| GET    | `/system`             | System metrics (CPU, memory, uptime).   |
| GET    | `/stats/database`     | Database size and per-table statistics. |
| GET    | `/stats/participants` | Aggregate participant counts.           |
| GET    | `/participants`       | List all participants (paginate later). |
| POST   | `/participants/bulk`  | (Admin) Bulk import via CSV body.       |

### Events (Administrator Only)

| Method | Endpoint   | Description                          |
| ------ | ---------- | ------------------------------------ |
| POST   | `/events`  | Create a new event.                  |
| GET    | `/events`  | List all events with summary counts. |

### Deployment Webhooks

| Method | Endpoint                    | Description                              |
| ------ | --------------------------- | ---------------------------------------- |
| POST   | `/deploy/api`               | Trigger API redeploy.                    |
| POST   | `/deploy/console`           | Trigger console (frontend) redeploy.     |
| POST   | `/deploy/run/<script-name>` | Run a script from the operational allowlist. |

All deployment endpoints require a `?secret=<webhook-secret>` query parameter.

---

## Deployment

### Initial Bootstrap (Tablet)

Refer to [`docs/INSTALL.md`](docs/INSTALL.md) for the full bootstrap
procedure, including:

- Termux package installation
- PostgreSQL initialisation
- Cloudflare Tunnel registration and DNS configuration
- pm2 setup
- Boot script installation

### Continuous Deployment

After initial bootstrap, ongoing deployment is fully automated:

1. Configure GitHub webhook on this repository:
   - **URL:** `https://<api-host>/deploy/api?secret=<webhook-secret>`
   - **Content type:** `application/json`
   - **Trigger:** `push` events
2. Push to `main`. The deploy pipeline executes within seconds.

---

## Operations and Recovery

### Manual Deployment

```bash
curl -X POST 'https://api.example.com/deploy/api?secret=<secret>'
```

### Manual Rollback

```bash
curl -X POST 'https://api.example.com/deploy/run/rollback?secret=<secret>'
```

### Emergency Manual Recovery (Tablet Console)

If both the auto-deploy and rollback chains fail:

```bash
# Stop the failing process
pm2 delete event-server

# Restore most recent archived binary
LATEST=$(ls -t ~/projects/event-server-archive/event-server-* | head -1)
cp "$LATEST" ~/projects/event-server

# Start with explicit configuration
PORT=3001 \
DATABASE_URL='postgresql://rofi:devsecret@localhost:5432/eventplatform' \
STATIC_DIR=$HOME/projects/event-platform-console \
JWT_SECRET='<secret>' \
WEBHOOK_SECRET='<secret>' \
pm2 start ~/projects/event-server --name event-server --interpreter none --max-restarts 5

pm2 save
```

### Status Inspection

A status report can be generated via the webhook interface and read as a
static file:

```bash
# Trigger generation
curl -X POST 'https://api.example.com/deploy/run/full-status?secret=<secret>'

# Read result
curl 'https://console.example.com/full-status.json'
```

---

## Repository Layout

```
event-platform-api/
├── server.c                      # Main application source (single file)
├── migrations/
│   └── 002_auth_attendance.sql   # Database schema
├── deploy/
│   ├── README.md                 # Operational scripts documentation
│   ├── deploy-api.sh             # Auto-deploy with smoke test
│   ├── rollback.sh               # Restore previous binary
│   ├── health-watchdog.sh        # 5-min cron: detect + recover
│   ├── db-backup.sh              # Daily pg_dump
│   ├── setup-cron.sh             # Install all cron jobs
│   ├── boot-script.sh            # Termux:Boot autostart
│   ├── install-boot.sh           # Place boot-script in correct location
│   ├── status-check.sh           # Brief status snapshot
│   └── full-status.sh            # Comprehensive status report
├── loadtest/
│   ├── README.md
│   └── stress.js                 # Concurrent request generator
├── docs/
│   ├── INSTALL.md                # Initial bootstrap procedure
│   ├── ARCHITECTURE.md           # Detailed design rationale
│   └── API.md                    # Complete API reference
├── .github/
│   └── workflows/ci.yml          # Compile + lint validation
├── .gitignore
└── README.md                     # This file
```

---

## Development

### Local Compilation

Requires `gcc`/`clang` and `libpq-dev`.

```bash
# Debian/Ubuntu
sudo apt-get install -y build-essential libpq-dev

# macOS (Homebrew)
brew install postgresql

# Compile
cc -O2 -Wall -o event-server server.c -lpq

# Run against local PostgreSQL
PORT=3000 \
DATABASE_URL='postgresql://localhost:5432/eventplatform_dev' \
./event-server
```

### Database Setup

Apply migrations to a local development database:

```bash
createdb eventplatform_dev
psql -d eventplatform_dev -f migrations/002_auth_attendance.sql
```

### Continuous Integration

Every push triggers `.github/workflows/ci.yml`:

- Compilation with `-Wall` (warnings escalated)
- Shell script syntax validation (`bash -n`)
- Binary verification

CI passing does not imply runtime correctness; the smoke test in the
auto-deploy pipeline is the runtime gate.

---

## Security Considerations

This codebase prioritises operational simplicity and educational clarity
over production-grade security. Before any production use, address the
following:

| Issue                                | Recommendation                                |
| ------------------------------------ | --------------------------------------------- |
| Plaintext password storage           | Replace with bcrypt or Argon2 hashing.        |
| FNV-1a token signing                 | Replace with HMAC-SHA256 (use OpenSSL).       |
| Webhook secret in query string       | Move to header-based HMAC verification.       |
| CORS wildcard                        | Restrict to known frontend origins.           |
| No HTTPS at origin                   | Acceptable since Cloudflare terminates TLS;   |
|                                      | for direct LAN exposure, terminate locally.   |
| Single-process database access       | Add connection pooling (PgBouncer) at scale.  |

The server includes a basic in-memory rate limiter:
- `/auth/*` endpoints: 10 requests / 60 seconds / IP
- All other endpoints: 60 requests / 60 seconds / IP
- Real client IP extracted from `CF-Connecting-IP` header when present.

The rate limiter is a fixed-window counter with 256 hash slots;
collisions cause shared limits across IPs. For more sophisticated rate
limiting, use a Redis-backed sliding window or token bucket.

---

## Licence

This project is provided as-is for educational and demonstration purposes.
