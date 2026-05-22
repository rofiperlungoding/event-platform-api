# Operational Scripts

Shell scripts that automate deployment, recovery, scheduling, and
diagnostics for the platform. All scripts target the tablet host
(Termux + bash) and are invoked either by cron, by the deploy webhook
in `server.c`, or by an operator over SSH.

## Catalogue

| Script                         | Trigger                          | Purpose                                                         |
| ------------------------------ | -------------------------------- | --------------------------------------------------------------- |
| `deploy-api.sh`                | Deploy webhook                   | git pull, compile, smoke test, atomic swap, pm2 restart         |
| `hot-swap.sh`                  | Manual / SSH                     | Same as above without the git pull; runs on already-pushed source |
| `rollback.sh`                  | Watchdog or manual               | Restore the previous binary from `~/projects/event-server-archive/` |
| `health-watchdog.sh`           | Cron (every 5 minutes)           | Probe `/health`; auto-restart, then auto-rollback if needed     |
| `db-backup.sh`                 | Cron (daily 03:00 local)         | Logical dump of the database to `~/backups/<YYYY-MM-DD>.sql.gz` |
| `boot-script.sh`               | Termux:Boot                      | Start Postgres, pm2, cloudflared after device power-up          |
| `install-boot.sh`              | One-time, manual                 | Symlink `boot-script.sh` into `~/.termux/boot/`                 |
| `setup-cron.sh`                | One-time, manual                 | Install backup, watchdog, and log rotation cron entries         |
| `status-check.sh`              | Operator                         | Print uptime, pm2 state, tunnel state, DB state in one screen   |
| `full-status.sh`               | Operator                         | Verbose status: process tree, network, recent errors            |
| `replicate-supabase.sh`        | Cron (hourly)                    | Sync deltas to a Supabase replica; see `docs/REPLICATION.md`    |
| `send-email.sh`                | Watchdog alerts                  | Wrapper around msmtp for SMTP delivery                          |
| `start-tunnel-replicas.sh`     | Manual, ad-hoc capacity test     | Spawn additional cloudflared instances against the same tunnel  |

## Deploy Pipeline (Reference)

```
GitHub push to main
        ↓
GitHub Actions
        ↓
POST https://api.<domain>/deploy/api?secret=<WEBHOOK_SECRET>
        ↓
event-server forks deploy-api.sh
        ↓
git pull --ff-only
        ↓
cc -O2 server.c -o event-server-staging
        ↓
smoke test on random port
        ↓
[fail] keep production binary; log; exit 1
[pass] archive current; mv staging → event-server; pm2 restart
        ↓
post-deploy health check
        ↓
[fail] rollback.sh
[pass] log success
```

## Recovery Runbook

### Server unhealthy

```bash
pm2 restart event-server
pm2 logs event-server --lines 50
```

If `/health` does not return inside 10 seconds, the watchdog will
escalate automatically; manual escalation:

```bash
deploy/rollback.sh
```

### Atomic swap blocked by "text file busy"

This historically happened on Termux when the previous binary was held
by a forked child. The fix is built into `hot-swap.sh`: it deletes the
pm2 process before the `mv`, then re-creates it explicitly. If a
deploy still fails, run `hot-swap.sh` over SSH after the push.

### Port 3001 already in use

```bash
pkill -9 -f event-server-staging
pkill -9 -f event-server
sleep 2
PORT=3001 ... pm2 start ./event-server --name event-server
```

### Tunnel down

```bash
pm2 restart event-tunnel
pm2 logs event-tunnel --lines 30
```

If the tunnel credentials file has been lost, re-create the tunnel:

```bash
cloudflared tunnel login
cloudflared tunnel create tabserve
# update config.yml and DNS records
```

## Cron Schedule

```
0  3  * * *   db-backup.sh
*/5 *  * * *   health-watchdog.sh
*/30 *  * * * replicate-supabase.sh   # optional, see REPLICATION.md
```

Defined in `setup-cron.sh`. Termux uses `cronie`.
