# Installation Guide

This guide covers the initial bootstrap of the Event Platform API on a
fresh Android tablet running Termux. Complete this procedure once per
deployment target.

---

## Prerequisites

| Requirement                    | Notes                                          |
| ------------------------------ | ---------------------------------------------- |
| Android tablet                 | ARM64, ≥ 3 GB RAM, ≥ 4 GB free storage         |
| Termux app                     | Install **from F-Droid** (the Play Store version is unmaintained) |
| Termux:API add-on              | Required for `termux-wake-lock`                |
| Termux:Boot add-on             | Required for autostart on device reboot        |
| Cloudflare account             | Free tier is sufficient                        |
| Domain name                    | Managed via Cloudflare DNS                     |

---

## Stage 1 — Termux Package Installation

```bash
# Update package indexes
pkg update -y

# Core build chain
pkg install -y git nodejs-lts postgresql clang nano openssh

# Process supervision and tunnelling
npm install -g pm2
pkg install -y cloudflared

# Cron daemon
pkg install -y cronie
```

Verify installations:

```bash
git --version
node --version
psql --version
cc --version
pm2 --version
cloudflared --version
crond --help 2>&1 | head -1
```

---

## Stage 2 — PostgreSQL Initialisation

```bash
# Initialise data directory
initdb $PREFIX/var/lib/postgresql

# Start the server
pg_ctl -D $PREFIX/var/lib/postgresql -l $PREFIX/var/lib/postgresql/logfile start

# Create application user and database
createuser --superuser rofi
createdb -O rofi eventplatform

# Set password
psql -d postgres -c "ALTER USER rofi WITH PASSWORD 'devsecret';"
```

Verify with a connection test:

```bash
psql -U rofi -d eventplatform -c 'SELECT version();'
```

---

## Stage 3 — Source Checkout and Initial Build

```bash
mkdir -p ~/projects && cd ~/projects
git clone https://github.com/<owner>/event-platform-api.git
git clone https://github.com/<owner>/event-platform-console.git

cd event-platform-api

# Apply schema
psql -U rofi -d eventplatform -f migrations/002_auth_attendance.sql

# Compile
cc -O2 -Wall -o ~/projects/event-server server.c \
    -I$PREFIX/include -L$PREFIX/lib -lpq
```

---

## Stage 4 — Process Configuration

Create a pm2 process for the API server:

```bash
DATABASE_URL='postgresql://rofi:devsecret@localhost:5432/eventplatform' \
STATIC_DIR=$HOME/projects/event-platform-console \
JWT_SECRET='<choose a strong random secret>' \
WEBHOOK_SECRET='<choose a different strong random secret>' \
PORT=3001 \
pm2 start $HOME/projects/event-server \
    --name event-server \
    --interpreter none \
    --max-restarts 5

pm2 save
```

Verify:

```bash
pm2 status
curl -sf http://localhost:3001/health
```

---

## Stage 5 — Cloudflare Tunnel Setup

### 5.1 — Authenticate cloudflared

```bash
cloudflared tunnel login
```

This opens a browser-based authorisation flow. Select the domain you wish
to associate with the tunnel.

### 5.2 — Create the tunnel

```bash
cloudflared tunnel create eventplatform
```

Note the tunnel UUID returned by this command. The associated credentials
file is written to `~/.cloudflared/<UUID>.json`.

### 5.3 — Configure routing

Create `~/.cloudflared/config.yml`:

```yaml
tunnel: <your-tunnel-uuid>
credentials-file: /data/data/com.termux/files/home/.cloudflared/<your-tunnel-uuid>.json

ingress:
  - hostname: api.example.com
    service: http://localhost:3001
  - hostname: console.example.com
    service: http://localhost:3001
  - service: http_status:404
```

### 5.4 — Bind DNS records

```bash
cloudflared tunnel route dns eventplatform api.example.com
cloudflared tunnel route dns eventplatform console.example.com
```

### 5.5 — Run the tunnel under pm2

```bash
pm2 start cloudflared \
    --name event-tunnel \
    --interpreter none \
    -- tunnel \
        --config $HOME/.cloudflared/config.yml \
        --no-autoupdate \
        run eventplatform

pm2 save
```

### 5.6 — Verify

```bash
curl -sf https://api.example.com/health
```

Expected output:

```json
{"status":"ok","uptime":<seconds>}
```

---

## Stage 6 — Operational Automation

### 6.1 — Install the boot script

```bash
bash ~/projects/event-platform-api/deploy/install-boot.sh
ls -la ~/.termux/boot/
```

### 6.2 — Install cron jobs

```bash
bash ~/projects/event-platform-api/deploy/setup-cron.sh
crontab -l
```

This installs three jobs:

| Schedule       | Job                                            |
| -------------- | ---------------------------------------------- |
| `0 3 * * *`    | Daily database backup with seven-day retention |
| `0 4 * * 0`    | Weekly pm2 log truncation                      |
| `*/5 * * * *`  | Health watchdog with auto-rollback             |

### 6.3 — Verify cron daemon is active

```bash
pgrep -af crond
```

If absent:

```bash
nohup crond < /dev/null > $HOME/cron.log 2>&1 &
```

---

## Stage 7 — GitHub Webhook Configuration

In the repository settings on GitHub, add a webhook:

- **Payload URL:** `https://api.example.com/deploy/api?secret=<webhook-secret>`
- **Content type:** `application/json`
- **Events:** Only the `push` event
- **Active:** Enabled

Repeat for the `event-platform-console` repository, replacing the path
with `/deploy/console`.

---

## Stage 8 — Verification

Confirm the full stack with a smoke test:

```bash
# Local
curl -sf http://localhost:3001/health

# Tunnel
curl -sf https://api.example.com/health

# Full status report
curl -X POST 'https://api.example.com/deploy/run/full-status?secret=<webhook-secret>'
sleep 3
curl 'https://console.example.com/full-status.json' | jq
```

Expected status report fields:

- `boot_script.installed: true`
- `cron.daemon_running: true`
- `cron.jobs_count: 3`
- `services.postgres_running: true`
- `services.pm2_processes: 2`

---

## Troubleshooting

| Symptom                                     | Diagnostic Step                                        |
| ------------------------------------------- | ------------------------------------------------------ |
| `bind: Address already in use`              | Identify zombie processes; restart Termux app         |
| Tunnel fails to connect                     | Check `pm2 logs event-tunnel`; verify DNS records      |
| `pg_ctl: No such file`                      | Re-run `pkg install postgresql`                        |
| Auto-deploy succeeds but server fails       | Inspect `~/deploy-api.log` and `~/rollback.log`        |
| Cron jobs not executing                     | Confirm `pgrep crond`; restart with `nohup crond &`    |
| Watchdog repeatedly rolls back              | Manual recovery; investigate root cause in deploy log  |
