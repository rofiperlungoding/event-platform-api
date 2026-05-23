# Operator Runbook

Reference card for routine and emergency operations against the live
tablet. Every command assumes you are at the tablet via SSH and the
`event-platform-api` working tree is checked out under
`~/projects/event-platform-api`.

## Connection

```bash
ssh -p 8022 u0_a258@192.168.100.67
cd ~/projects/event-platform-api
```

Public surfaces:

| Surface     | URL                                       |
| ----------- | ----------------------------------------- |
| API         | `https://api.rofidoesthings.site`         |
| Console     | `https://console.rofidoesthings.site`     |
| LAN direct  | `http://192.168.100.67:3001`              |

## Standard sync after a remote push

When commits land on `main` from a development host, run the
following on the tablet to bring the running binary up to date.
The webhook automates this; the manual steps below are for the
case where the webhook silently failed (audit item 73).

```bash
# 1. Fetch and fast-forward
git pull --ff-only

# 2. Apply pending migrations (idempotent)
bash deploy/migrate.sh

# 3. Make any new helper scripts executable
chmod +x deploy/*.sh

# 4. Apply Postgres tuning if the file changed
bash deploy/tune-postgres.sh
pg_ctl -D $PREFIX/var/lib/postgresql -l $HOME/postgres.log restart
sleep 4

# 5. Rebuild and hot-swap the API server
bash deploy/hot-swap.sh

# 6. Re-install cron jobs (battery, time, cert, log rotation, auth cleanup)
bash deploy/setup-cron.sh

# 7. Verify
curl -sf http://localhost:3001/health/detailed | jq .
curl -sf http://localhost:3001/metrics | grep checkins_ok
```

To trigger the same flow remotely without SSH:

```bash
curl -X POST "https://api.rofidoesthings.site/deploy/api?secret=intriviadeploy2026"
```

## Smoke test after deploy

```bash
# Capacity sanity (5000 concurrent attempts, 0 errors expected)
node loadtest/chaos-checkin.js 5000 http://192.168.100.67:3001

# Wipe seeded test data (admin row preserved)
psql -h 127.0.0.1 -U rofi -d eventplatform -f loadtest/wipe-all.sql
```

## Read-only health checks

```bash
curl -sf http://localhost:3001/health
curl -sf http://localhost:3001/health/detailed | jq .
curl -sf http://localhost:3001/health/ready
curl -sf http://localhost:3001/system        | jq .
curl -sf http://localhost:3001/stats/database | jq .
curl -sf http://localhost:3001/metrics | head -50
```

## Common interventions

| Symptom                                     | Action                                              |
| ------------------------------------------- | --------------------------------------------------- |
| `/health/ready` returns 503                 | `bash deploy/full-status.sh` to find the failing component |
| `/stats/database long_queries > 0`          | `psql -c "SELECT pid, query FROM pg_stat_activity WHERE state='active'"` then `SELECT pg_terminate_backend(pid)` |
| Battery low                                 | Plug in; the watchdog will throttle on red          |
| Tunnel down                                 | `bash deploy/start-tunnel-replicas.sh`              |
| Backup older than 24 h                      | `bash deploy/db-backup.sh` then check `~/backups`   |
| Need to roll back                           | `bash deploy/rollback.sh`                           |

## Emergency: live event in progress

1. **Don't restart Postgres**. A 10-second restart is a 10-second
   outage; the PWA queue absorbs only ~30 seconds of failures.
2. **Don't run migrations**. They are idempotent, but a long-running
   `ALTER TABLE` will lock the attendance write path.
3. Use `bash deploy/hot-swap.sh` for binary updates only — it
   keeps the listener fd alive.
4. If the API is wedged but Postgres is healthy, check
   `/health/detailed` for `database.latency_ms`. Anything > 100 ms
   is a sign the connection pool is exhausted.

## Configuration sources

| Variable        | Default                                                              | Notes                                       |
| --------------- | -------------------------------------------------------------------- | ------------------------------------------- |
| `PORT`          | `3000` (we deploy with `3001`)                                       | The CDN proxies 443 → 3001.                 |
| `DATABASE_URL`  | `postgresql://rofi:devsecret@localhost:5432/eventplatform`           | Set in `~/.bashrc` for production.          |
| `STATIC_DIR`    | `~/projects/event-platform-api/console`                              | The PWA + admin console.                    |
| `JWT_SECRET`    | `intrivia2026secret`                                                 | A startup `[WARN]` is emitted if missing.   |
| `WEBHOOK_SECRET`| `intriviadeploy2026`                                                 | Required on `POST /deploy/api`.             |
| `CORS_ORIGINS`  | `https://console.rofidoesthings.site`                                | Comma-separated; empty = wildcard.          |
| `ALLOWED_HOSTS` | `api.rofidoesthings.site,localhost`                                  | Comma-separated; empty = accept any.        |
| `WORKERS`       | `8`                                                                  | Match the Unisoc T618 core count.           |

## Default credentials (development)

Replace before exposing publicly.

| Surface | User                  | Secret               |
| ------- | --------------------- | -------------------- |
| SSH     | `u0_a258@…:8022`      | key auth             |
| DB      | `rofi@localhost:5432` | `devsecret`          |
| Console | `admin@intrivia.test` | `admin123`           |
| Webhook | (none)                | `intriviadeploy2026` |
