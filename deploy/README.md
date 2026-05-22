# Operational Scripts

Shell scripts that automate deployment, recovery, scheduling, and
diagnostics for the Event Platform API. All scripts are designed to run
on the tablet host (Termux environment) and are typically invoked by
either the cron daemon or the deployment webhook.

---

## Script Catalogue

### `deploy-api.sh`

End-to-end deployment script for the API server.

**Triggered by:** GitHub webhook → `event-server` → fork → `deploy-api.sh`

**Steps:**
1. Pull the latest source from `origin/main`.
2. Compile to a staging binary (`event-server-staging`).
3. Smoke test: run the staging binary on port 3099 and issue
   `GET /health`.
4. If the smoke test fails, exit; the production binary is untouched.
5. If the smoke test passes:
   - Archive the current binary to `~/projects/event-server-archive/`
     with a timestamp suffix.
   - Atomically replace the production binary with the staging binary.
   - Restart the `event-server` pm2 process.
6. Perform a final health check on the production port.

**Failure isolation:** A compilation failure, smoke test failure, or
final health check failure leaves the running production binary in
place. The deploy log records the failure mode.

**Logs:** `~/deploy-api.log`

### `rollback.sh`

Restore the most recent archived binary.

**Triggered by:**
- Manual: `POST /deploy/run/rollback?secret=...`
- Automatic: invoked by `health-watchdog.sh` when restart fails

**Steps:**
1. Locate the most recent file in `~/projects/event-server-archive/`.
2. Move the current production binary to a `FAILED-` prefixed archive
   entry (preserves the failure for forensic analysis).
3. Copy the archived binary to the production location.
4. Restart the `event-server` pm2 process.
5. Verify the rollback restored a healthy state.

**Logs:** `~/rollback.log`

### `health-watchdog.sh`

Active health monitoring with automatic recovery.

**Triggered by:** cron, every 5 minutes (`*/5 * * * *`)

**Steps:**
1. Issue `GET /health` against `localhost:3001`.
2. If healthy, exit silently (no log entry).
3. If unhealthy, attempt `pm2 restart event-server` and re-check after
   five seconds.
4. If still unhealthy, invoke `rollback.sh`.

**Logs:** `~/watchdog.log` (only on failure)

### `db-backup.sh`

Database backup with retention policy.

**Triggered by:** cron, daily at 03:00 (`0 3 * * *`)

**Steps:**
1. Run `pg_dump` for the `eventplatform` database, piped through
   `gzip`.
2. Write the output to `~/backups/eventplatform_<timestamp>.sql.gz`.
3. Verify the backup file is non-empty.
4. Remove backups older than seven days from `~/backups/`.

**Logs:** `~/backup.log`

**Restore:** Backups are decompressible with `gunzip`. To restore:

```bash
gunzip -c ~/backups/eventplatform_<timestamp>.sql.gz | psql -U rofi -d eventplatform
```

### `setup-cron.sh`

Idempotent installer for the cron job catalogue.

**Triggered by:** Manual on initial setup; can be re-run safely.

**Installs:**

| Schedule       | Command                  | Purpose                |
| -------------- | ------------------------ | ---------------------- |
| `0 3 * * *`    | `db-backup.sh`           | Daily backup           |
| `0 4 * * 0`    | `pm2 flush`              | Weekly log truncation  |
| `*/5 * * * *`  | `health-watchdog.sh`     | Health monitoring      |

The script de-duplicates existing entries before adding new ones, so
re-running it does not produce duplicates.

It also starts the `cronie` daemon if it is not already running, using
`nohup` to detach it from the calling shell.

### `boot-script.sh`

The script copied to `~/.termux/boot/start-event-stack` to run on device
boot.

**Steps performed at boot:**
1. Acquire wake-lock via `termux-wake-lock`.
2. Sleep 8 seconds for system stabilisation.
3. Start PostgreSQL.
4. Start the SSH daemon for emergency access.
5. Start the cron daemon.
6. Restore pm2 process registry (`pm2 resurrect`).
7. Sleep 15 seconds, then log final health state.

**Logs:** `~/boot.log`

### `install-boot.sh`

One-shot installer that copies `boot-script.sh` to the Termux:Boot
directory and ensures it is executable.

**Triggered by:** Manual on initial setup, or via webhook
(`POST /deploy/run/install-boot?secret=...`).

### `status-check.sh`

Brief health status snapshot. Writes a small JSON document to the
console static directory so it can be retrieved without authentication.

**Triggered by:** `POST /deploy/run/status-check?secret=...`

**Output:** `<STATIC_DIR>/status.json` (publicly fetchable)

### `full-status.sh`

Comprehensive diagnostic snapshot covering all platform components.

**Triggered by:** `POST /deploy/run/full-status?secret=...`

**Output:** `<STATIC_DIR>/full-status.json` (publicly fetchable)

**Reports:**
- Tablet IP and uptime
- Boot script presence and permissions
- Cron daemon state and job catalogue
- Backup catalogue and total size
- PostgreSQL state, sshd state, pm2 process count
- Latest log lines from boot, backup, and cron logs

---

## Operational Conventions

### Trapped Signals

All scripts run under the `event-server` process tree, which has
`SIGCHLD` handled with `SIG_IGN` to prevent zombie accumulation. This
breaks `git`'s subprocess management. Each script begins with:

```bash
trap - CHLD
```

This restores the default `SIGCHLD` handler for the script's own
subprocesses.

### Strict Mode

Scripts use `set -e` to abort on the first error, ensuring partial
deployments do not silently succeed. Where intermediate failures are
expected (e.g., missing log files), they are explicitly suppressed
with `2>/dev/null` or `|| true`.

### Logging

All scripts that perform mutations write to a log file in `$HOME` with
ISO-formatted timestamps:

```bash
echo "[$(date)] === <action> ===" >> "$LOG"
```

Logs are rotated weekly (`pm2 flush`) but the script logs accumulate
indefinitely. Manual cleanup is the operator's responsibility.
