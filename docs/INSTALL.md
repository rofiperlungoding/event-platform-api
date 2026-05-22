# Installation

End-to-end bootstrap for the reference deployment: a single Android
tablet running Termux, hosting the C API server, PostgreSQL, the static
console, and a Cloudflare Tunnel.

## Prerequisites

| Component                | Minimum                         |
| ------------------------ | ------------------------------- |
| Hardware                 | Android tablet, 3 GB RAM, ARM64 |
| Termux                   | F-Droid build, 2025+            |
| Termux:Boot              | Installed and granted boot perms |
| Network                  | LAN access for SSH, internet for tunnel |
| Cloudflare account       | Free tier sufficient            |
| Domain on Cloudflare DNS | Required for the named tunnel   |

## 1. Termux base packages

```bash
pkg update -y && pkg upgrade -y
pkg install -y openssh tmux git nano postgresql nodejs jq curl \
    cloudflared cronie clang
```

Generate an authorised key for laptop SSH access:

```bash
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo "ssh-ed25519 AAAA... laptop@user" >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
sshd
```

## 2. PostgreSQL

```bash
initdb -D $PREFIX/var/lib/postgresql/data
pg_ctl -D $PREFIX/var/lib/postgresql/data -l $HOME/postgres.log start

createuser -s rofi
createdb -O rofi eventplatform
psql -d eventplatform -c "ALTER USER rofi WITH PASSWORD 'devsecret';"

cd ~/projects/event-platform-api
psql -U rofi -d eventplatform -f migrations/002_auth_attendance.sql
psql -U rofi -d eventplatform -f migrations/003_named_sessions.sql
psql -U rofi -d eventplatform -f migrations/004_multi_event.sql
```

## 3. Build the API

```bash
cd ~/projects/event-platform-api
cc -O2 -o event-server server.c \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lpq
```

Verify by running once in foreground:

```bash
PORT=3001 \
DATABASE_URL=postgresql://rofi:devsecret@localhost:5432/eventplatform \
JWT_SECRET=intrivia2026secret \
WEBHOOK_SECRET=intriviadeploy2026 \
STATIC_DIR=$HOME/projects/event-platform-api/console \
./event-server
```

`curl http://localhost:3001/health` should return `{"status":"ok"}`.

## 4. Process supervision

Install pm2 globally and register the API:

```bash
npm install -g pm2
PORT=3001 \
DATABASE_URL=postgresql://rofi:devsecret@localhost:5432/eventplatform \
JWT_SECRET=intrivia2026secret \
WEBHOOK_SECRET=intriviadeploy2026 \
STATIC_DIR=$HOME/projects/event-platform-api/console \
pm2 start ./event-server --name event-server
pm2 save
```

## 5. Cloudflare Tunnel

```bash
cloudflared tunnel login                   # browser-based one-time auth
cloudflared tunnel create tabserve         # produces a UUID + credentials json
```

Write `~/.cloudflared/config.yml`:

```yaml
tunnel: <UUID>
credentials-file: /data/data/com.termux/files/home/.cloudflared/<UUID>.json

ingress:
  - hostname: api.<your-domain>
    service: http://localhost:3001
  - hostname: console.<your-domain>
    service: http://localhost:3001
  - service: http_status:404
```

Add DNS records in Cloudflare (point both hostnames at
`<UUID>.cfargotunnel.com` as CNAME), then start under pm2:

```bash
pm2 start cloudflared --name event-tunnel -- tunnel --config ~/.cloudflared/config.yml run
pm2 save
```

## 6. Boot autostart

Termux:Boot runs scripts placed in `~/.termux/boot/` at device power-up.

```bash
mkdir -p ~/.termux/boot
cp deploy/boot-script.sh ~/.termux/boot/start-event-stack
chmod +x ~/.termux/boot/start-event-stack
```

The boot script starts Postgres, pm2, and waits for the tunnel to be
healthy.

## 7. Cron jobs

```bash
deploy/setup-cron.sh
```

This installs:

- `db-backup.sh` daily at 03:00 (writes to `~/backups/`)
- `health-watchdog.sh` every 5 minutes
- Log rotation for `~/c-server.log`

## 8. Static frontend

```bash
git clone git@github.com:<you>/event-platform-api/console.git ~/projects/event-platform-api/console
```

The C server serves files from `STATIC_DIR` — no separate web server is
required.

## Verification

```bash
# API
curl https://api.<your-domain>/health

# Console
curl -I https://console.<your-domain>/

# Admin login
curl -X POST https://api.<your-domain>/auth/login \
    -H 'Content-Type: application/json' \
    -d '{"email":"admin@<your-domain>","password":"<set-via-migration>"}'

# 2000-concurrent stampede (run from a laptop on the same LAN)
node loadtest/run-stampede.js 2000 http://<tablet-lan-ip>:3001
```

The expected stampede outcome is documented in
[`loadtest/README.md`](../loadtest/README.md).
