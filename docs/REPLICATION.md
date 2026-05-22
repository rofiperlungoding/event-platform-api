# Replication to Supabase

The reference deployment hosts the source-of-truth database on the
tablet. This document describes how to set up a daily one-way
replication of that database to a hosted Supabase project for read-only
fallback, geographic redundancy, and edge-located queries.

---

## Why Replicate

The tablet is a constrained environment:

- Single point of failure (one device)
- Tied to the WiFi network it joins
- Limited CPU and memory
- Reachable only via Cloudflare Tunnel

A Supabase replica addresses these limitations:

- **Read-only failover.** When the tablet is offline (battery dead,
  WiFi outage, OS killed processes), the replica still responds to
  read queries.
- **Edge proximity.** Supabase is reachable globally with low latency.
  Read traffic from far-away clients can be routed to the replica.
- **Disaster recovery.** A hardware failure does not erase the data.
- **Analytics.** Heavy reporting queries can run against the replica
  without affecting the production tablet.

The replica is **read-only** and one-way. Writes always go to the
tablet. The replica is regenerated each night.

---

## Topology

```
┌──────────────────────────────────────────────┐
│  Production write path                       │
│                                               │
│  Clients ──HTTPS──► Cloudflare Tunnel        │
│                          │                    │
│                          ▼                    │
│                  Tablet (event-server)        │
│                          │                    │
│                          ▼                    │
│                  Postgres 18 (Termux)         │
└──────────────────────────────────────────────┘
            │  Daily 04:00 cron (one-way)
            │
            ▼  pg_dump → psql restore
┌──────────────────────────────────────────────┐
│  Supabase project (read-only mirror)         │
│  Region: ap-southeast-1 (Singapore)          │
│  Postgres 17                                 │
│                                               │
│  Use cases:                                  │
│    - Read-only failover                      │
│    - Analytics dashboards                    │
│    - Geographic edge access                  │
└──────────────────────────────────────────────┘
```

---

## Setup

### 1. Provision a Supabase Project

The reference replica was created via the Supabase MCP:

- **Project name:** `event-platform-replica`
- **Region:** `ap-southeast-1` (Singapore)
- **Postgres version:** 17.6
- **Project ID:** `gqkquihvjduifjgyxygh`
- **Database host:** `db.gqkquihvjduifjgyxygh.supabase.co`

The schema (Participant, Event, Session, Attendance, Device,
_ReplicationStatus) was applied via `apply_migration` during project
provisioning.

### 2. Configure Replication Credentials

On the tablet, create `~/.replication.env`:

```bash
SUPABASE_HOST=db.gqkquihvjduifjgyxygh.supabase.co
SUPABASE_PORT=5432
SUPABASE_DB=postgres
SUPABASE_USER=postgres
SUPABASE_PASSWORD=<from Supabase project settings → Database → Connection string>
```

Set restrictive permissions:

```bash
chmod 600 ~/.replication.env
```

### 3. Test Manually

```bash
bash ~/projects/event-platform-api/deploy/replicate-supabase.sh
tail -20 ~/replication.log
```

Successful output:

```
[Fri May 22 04:00:00 WIB 2026] === REPLICATION START ===
[Fri May 22 04:00:01 WIB 2026] Dump complete (8421 bytes)
[Fri May 22 04:00:04 WIB 2026] ✓ Replication complete: 47 rows in 4231ms
```

### 4. Enable in Cron

Add to the cron job catalogue in `deploy/setup-cron.sh`:

```cron
0 4 * * * /data/data/com.termux/files/usr/bin/bash /data/data/com.termux/files/home/projects/event-platform-api/deploy/replicate-supabase.sh
```

Re-run the setup script to install:

```bash
bash ~/projects/event-platform-api/deploy/setup-cron.sh
```

---

## How It Works

### Dump Phase

```bash
pg_dump -h localhost -U rofi -d eventplatform \
    --data-only --no-owner --no-acl \
    --table='"Event"' \
    --table='"Participant"' \
    --table='"Session"' \
    --table='"Attendance"' \
    --table='"Device"'
```

`--data-only` produces `INSERT` statements only — no DDL. The replica
schema must already match.

### Truncate + Restore Phase

```sql
TRUNCATE "Attendance", "Device", "Session", "Participant", "Event"
    RESTART IDENTITY CASCADE;
```

A full truncate-and-restore is simpler than incremental sync. For our
volume (≤ 2,000 participants), the entire dump fits comfortably in a
~10 KB SQL file and restores in seconds.

### Audit Phase

Each replication writes a row to `_ReplicationStatus`:

```sql
INSERT INTO "_ReplicationStatus" (source_host, last_sync_at, rows_synced,
                                  duration_ms, status, notes)
VALUES (...);
```

To check replication health:

```sql
SELECT * FROM "_ReplicationStatus"
ORDER BY last_sync_at DESC LIMIT 5;
```

---

## Querying the Replica

Connect with any PostgreSQL client:

```bash
psql 'postgresql://postgres:<pw>@db.gqkquihvjduifjgyxygh.supabase.co:5432/postgres'
```

The Supabase project also exposes:

- **REST API** (PostgREST): auto-generated from the schema
- **GraphQL** (pg_graphql extension)
- **Realtime subscriptions** (Postgres LISTEN/NOTIFY → WebSocket)

Use the publishable key from the project dashboard for read-only access
from a frontend application.

---

## Limitations

| Limitation                         | Mitigation                                |
| ---------------------------------- | ----------------------------------------- |
| Up to 24 hours of staleness        | Run replication more frequently (hourly)  |
| Truncate/restore = brief downtime  | Acceptable for a read-only replica        |
| One-way only (no write merge)      | Writes still must go to the tablet        |
| Sequence IDs reset on each cycle   | Replica IDs match source; safe for reads  |
| Replica schema drift               | Apply schema migrations to both sides     |

---

## Failover Plan

If the tablet is down for an extended period:

1. **Read traffic** continues to work via the Supabase REST/PostgREST
   API. Clients can query `https://gqkquihvjduifjgyxygh.supabase.co/rest/v1/Participant`
   etc. directly.
2. **Write traffic** must be redirected manually:
   - Spin up a temporary instance of the C server pointed at the
     Supabase database (`DATABASE_URL=postgresql://postgres:...`).
   - Update DNS or tunnel configuration to direct traffic to the new
     instance.
3. When the tablet returns, **discard new writes that happened on
   Supabase during the outage**, or perform a manual merge. There is no
   automatic conflict resolution.

This is a low-cost disaster recovery posture suitable for events that
can tolerate read-only mode during outages.
