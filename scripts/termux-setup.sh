#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# event-platform-api — Termux Setup Script
# ----------------------------------------------------------------------------
# Idempotent: safe to re-run kalau gagal di tengah jalan.
# Run: bash scripts/termux-setup.sh
# ============================================================================

set -euo pipefail

# ─── Config ─────────────────────────────────────────────────────────────────
DB_USER="rofi"
DB_PASS="devsecret"
DB_NAME="eventplatform"
APP_PORT="3000"
APP_DIR="$HOME/projects/event-platform-api"
REPO_URL="https://github.com/rofiperlungoding/event-platform-api.git"
PG_DATA="$PREFIX/var/lib/postgresql"

# ─── Colors ─────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
  GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'
  BLUE='\033[0;34m'; BOLD='\033[1m'; NC='\033[0m'
else
  GREEN=''; YELLOW=''; RED=''; BLUE=''; BOLD=''; NC=''
fi

log()   { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $1"; }
phase() { echo -e "\n${BLUE}${BOLD}━━━ $1 ━━━${NC}"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $1"; }
err()   { echo -e "${RED}[error]${NC} $1" >&2; exit 1; }

# ─── Sanity checks ──────────────────────────────────────────────────────────
phase "PHASE 0 — Pre-flight"

if [ -z "${PREFIX:-}" ] || [ ! -d "/data/data/com.termux" ]; then
  err "Script ini cuma jalan di Termux. PREFIX not set."
fi

# Pastikan storage cukup (minimal 2GB free)
FREE_MB=$(df -m "$PREFIX" | awk 'NR==2 {print $4}')
if [ "$FREE_MB" -lt 2048 ]; then
  warn "Storage Termux cuma ${FREE_MB}MB free. Disarankan minimal 2GB."
  if [ -t 0 ]; then
    read -rp "Lanjutkan? (y/N) " confirm
  else
    read -rp "Lanjutkan? (y/N) " confirm < /dev/tty
  fi
  [ "$confirm" = "y" ] || exit 1
fi

log "Termux detected, storage OK (${FREE_MB}MB free)"
log "User: $(whoami) | Arch: $(uname -m)"

# ─── PHASE 1: Install packages ──────────────────────────────────────────────
phase "PHASE 1 — Install packages"

log "Updating package list..."
pkg update -y -o Dpkg::Options::="--force-confnew"

log "Installing core packages..."
pkg install -y git nodejs-lts postgresql nano openssh termux-api

# cloudflared di tur-repo (tidak selalu ada, optional)
if ! command -v cloudflared >/dev/null 2>&1; then
  log "Trying to install cloudflared (optional)..."
  pkg install -y tur-repo 2>/dev/null || warn "tur-repo tidak tersedia, skip"
  pkg install -y cloudflared 2>/dev/null || warn "cloudflared install gagal — bisa diinstall manual nanti"
fi

# Verify
for cmd in git node npm psql; do
  command -v "$cmd" >/dev/null || err "$cmd tidak ke-install"
done

log "Versions:"
log "  git:  $(git --version | awk '{print $3}')"
log "  node: $(node --version)"
log "  npm:  $(npm --version)"
log "  psql: $(psql --version | awk '{print $3}')"

# ─── PHASE 2: PostgreSQL setup ──────────────────────────────────────────────
phase "PHASE 2 — PostgreSQL"

# 2.1 — initdb (idempotent: cek folder PG_VERSION)
if [ ! -f "$PG_DATA/PG_VERSION" ]; then
  log "Initializing data directory at $PG_DATA..."
  mkdir -p "$PG_DATA"
  initdb "$PG_DATA"
else
  log "Postgres data dir already initialized"
fi

# 2.2 — start (idempotent: pg_ctl status)
if ! pg_ctl status -D "$PG_DATA" >/dev/null 2>&1; then
  log "Starting postgres..."
  pg_ctl -D "$PG_DATA" -l "$PG_DATA/logfile" start
  sleep 3
else
  log "Postgres already running"
fi

# 2.3 — create role (idempotent)
ROLE_EXISTS=$(psql -d postgres -tAc "SELECT 1 FROM pg_roles WHERE rolname='$DB_USER'" 2>/dev/null || echo "")
if [ "$ROLE_EXISTS" != "1" ]; then
  log "Creating role '$DB_USER'..."
  createuser --superuser "$DB_USER"
  psql -d postgres -c "ALTER USER \"$DB_USER\" WITH PASSWORD '$DB_PASS';" >/dev/null
else
  log "Role '$DB_USER' already exists, ensuring password..."
  psql -d postgres -c "ALTER USER \"$DB_USER\" WITH PASSWORD '$DB_PASS';" >/dev/null
fi

# 2.4 — create database (idempotent)
DB_EXISTS=$(psql -d postgres -tAc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" 2>/dev/null || echo "")
if [ "$DB_EXISTS" != "1" ]; then
  log "Creating database '$DB_NAME'..."
  createdb -O "$DB_USER" "$DB_NAME"
else
  log "Database '$DB_NAME' already exists"
fi

# 2.5 — verify
psql -U "$DB_USER" -d "$DB_NAME" -c "SELECT 'postgres ok' AS status;" >/dev/null \
  || err "Tidak bisa konek ke database. Cek log: cat $PG_DATA/logfile"
log "Postgres OK, connection verified"

# ─── PHASE 3: Clone repo ────────────────────────────────────────────────────
phase "PHASE 3 — Clone & sync repo"

if [ ! -d "$APP_DIR/.git" ]; then
  log "Cloning $REPO_URL..."
  mkdir -p "$(dirname "$APP_DIR")"
  git clone "$REPO_URL" "$APP_DIR"
else
  log "Repo exists, pulling latest..."
  git -C "$APP_DIR" fetch origin
  git -C "$APP_DIR" reset --hard origin/main
fi

cd "$APP_DIR"
log "On commit: $(git rev-parse --short HEAD) — $(git log -1 --pretty=%s)"

# ─── PHASE 4: Env file ──────────────────────────────────────────────────────
phase "PHASE 4 — .env config"

if [ ! -f ".env" ]; then
  cat > .env <<EOF
DATABASE_URL="postgresql://$DB_USER:$DB_PASS@localhost:5432/$DB_NAME"
PORT=$APP_PORT
NODE_ENV=production
EOF
  log ".env created"
else
  log ".env already exists, skipping"
fi

# ─── PHASE 5: Install deps ──────────────────────────────────────────────────
phase "PHASE 5 — npm install (BIG, takes 5-15 min on tablet)"

# wake-lock biar layar mati ga ngehentiin install
termux-wake-lock 2>/dev/null || warn "termux-wake-lock not available, install Termux:API"

# pake npm ci kalau ada lockfile, fallback ke install
if [ -f "package-lock.json" ]; then
  log "Running: npm ci (clean install dari lockfile)"
  npm ci --no-audit --no-fund
else
  log "Running: npm install"
  npm install --no-audit --no-fund
fi

# ─── PHASE 6: Prisma & build ────────────────────────────────────────────────
phase "PHASE 6 — Prisma & TypeScript build"

log "Generating Prisma client..."
npx prisma generate

log "Applying migrations..."
npx prisma migrate deploy

log "Building TypeScript..."
npm run build

[ -f "dist/index.js" ] || err "Build gagal, dist/index.js ga ada"
log "Build artifact: dist/index.js ($(stat -c%s dist/index.js 2>/dev/null || stat -f%z dist/index.js) bytes)"

# ─── PHASE 7: pm2 process manager ───────────────────────────────────────────
phase "PHASE 7 — pm2 (process manager)"

if ! command -v pm2 >/dev/null 2>&1; then
  log "Installing pm2 globally..."
  npm install -g pm2
fi

if pm2 jlist 2>/dev/null | grep -q '"name":"event-api"'; then
  log "Restarting existing event-api process..."
  pm2 restart event-api --update-env
else
  log "Starting event-api with pm2..."
  pm2 start dist/index.js --name event-api --time
fi

pm2 save >/dev/null
log "pm2 status:"
pm2 list

# ─── PHASE 8: Boot script (Termux:Boot) ─────────────────────────────────────
phase "PHASE 8 — Auto-start on tablet boot"

BOOT_DIR="$HOME/.termux/boot"
BOOT_SCRIPT="$BOOT_DIR/start-event-api"

mkdir -p "$BOOT_DIR"
cat > "$BOOT_SCRIPT" <<EOF
#!/data/data/com.termux/files/usr/bin/bash
# Auto-generated by termux-setup.sh
termux-wake-lock
sleep 5
pg_ctl -D "$PG_DATA" -l "$PG_DATA/logfile" start || true
sleep 3
pm2 resurrect
EOF
chmod +x "$BOOT_SCRIPT"
log "Boot script: $BOOT_SCRIPT"
log "Install Termux:Boot dari F-Droid biar auto-start beneran jalan saat reboot"

# ─── PHASE 9: Smoke test ────────────────────────────────────────────────────
phase "PHASE 9 — Verify"

sleep 3
HEALTH=$(curl -sf "http://localhost:$APP_PORT/health" || echo "")
if [ -n "$HEALTH" ]; then
  log "Server responding ✅"
  echo "  → $HEALTH"
else
  err "Server ga respon di http://localhost:$APP_PORT/health. Cek: pm2 logs event-api"
fi

# ─── Done ───────────────────────────────────────────────────────────────────
phase "🎉 SETUP COMPLETE"

cat <<EOF

API lo udah jalan di tablet pada port $APP_PORT.

  Health check:    curl http://localhost:$APP_PORT/health
  List participant: curl http://localhost:$APP_PORT/participants

  Pm2 commands:
    pm2 status              — liat status semua process
    pm2 logs event-api      — liat log realtime
    pm2 restart event-api   — restart app
    pm2 stop event-api      — stop app

  Update code (kalau lo push perubahan dari laptop):
    bash $APP_DIR/scripts/termux-update.sh

  Expose ke internet:
    cloudflared tunnel --url http://localhost:$APP_PORT

EOF
