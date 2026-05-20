#!/data/data/com.termux/files/usr/bin/bash
# ─────────────────────────────────────────────────────────────────
#  Event Platform API — One-shot Tablet Bootstrap
#  Run di Termux: setup semuanya dari nol sampai server jalan.
#  Idempotent: aman di-rerun kalau ada yang gagal di tengah.
# ─────────────────────────────────────────────────────────────────

set -e

REPO_URL="https://github.com/rofiperlungoding/event-platform-api.git"
APP_DIR="$HOME/projects/event-platform-api"
PG_DATA="$PREFIX/var/lib/postgresql"
DB_USER="rofi"
DB_PASS="devsecret"
DB_NAME="eventplatform"

c_cyan='\033[1;36m'; c_green='\033[1;32m'; c_yellow='\033[1;33m'; c_off='\033[0m'
log() { echo -e "\n${c_cyan}▶ $1${c_off}"; }
ok()  { echo -e "${c_green}✓ $1${c_off}"; }
warn(){ echo -e "${c_yellow}! $1${c_off}"; }

# ─── 1/8 — Sanity check ─────────────────────────────────────────
log "1/8 — Sanity check (ini Termux kan?)"
if [ -z "$PREFIX" ] || [[ "$PREFIX" != *"com.termux"* ]]; then
  echo "ERROR: Script ini cuma jalan di Termux Android. Abort."
  exit 1
fi
ok "Termux detected, lanjut"

# ─── 2/8 — Update & install packages ────────────────────────────
log "2/8 — Update package list & install tools (~150MB)"
pkg update -y
pkg upgrade -y -o Dpkg::Options::="--force-confnew"
pkg install -y git nodejs-lts postgresql nano openssh termux-api cloudflared
ok "Packages installed"

# ─── 3/8 — PostgreSQL setup ─────────────────────────────────────
log "3/8 — Initialize PostgreSQL"
if [ ! -f "$PG_DATA/PG_VERSION" ]; then
  mkdir -p "$PG_DATA"
  initdb "$PG_DATA"
  ok "Postgres datadir initialized"
else
  ok "Postgres datadir sudah ada, skip initdb"
fi

if ! pg_ctl -D "$PG_DATA" status > /dev/null 2>&1; then
  pg_ctl -D "$PG_DATA" -l "$PG_DATA/logfile" start
  sleep 3
  ok "Postgres started"
else
  ok "Postgres already running"
fi

# Buat user + db (idempotent)
psql -d postgres -tc "SELECT 1 FROM pg_roles WHERE rolname='$DB_USER'" | grep -q 1 \
  || createuser --superuser "$DB_USER"
psql -d postgres -tc "SELECT 1 FROM pg_database WHERE datname='$DB_NAME'" | grep -q 1 \
  || createdb -O "$DB_USER" "$DB_NAME"
psql -d postgres -c "ALTER USER $DB_USER WITH PASSWORD '$DB_PASS';" > /dev/null
ok "DB user & database ready"

# ─── 4/8 — Clone / pull repo ────────────────────────────────────
log "4/8 — Get repo"
mkdir -p "$HOME/projects"
if [ -d "$APP_DIR/.git" ]; then
  cd "$APP_DIR"
  git pull --rebase
  ok "Repo updated"
else
  git clone "$REPO_URL" "$APP_DIR"
  cd "$APP_DIR"
  ok "Repo cloned"
fi

# ─── 5/8 — Environment file ─────────────────────────────────────
log "5/8 — Setup .env"
cat > .env <<EOF
DATABASE_URL="postgresql://$DB_USER:$DB_PASS@localhost:5432/$DB_NAME"
PORT=3000
NODE_ENV=production
EOF
ok ".env written"

# ─── 6/8 — Install deps + build ─────────────────────────────────
log "6/8 — npm install + build (paling lama, ~5-15 menit)"
npm ci
npx prisma generate
npx prisma migrate deploy
npm run build
ok "App built"

# ─── 7/8 — pm2 (process manager) ────────────────────────────────
log "7/8 — Start app dengan pm2"
if ! command -v pm2 > /dev/null 2>&1; then
  npm install -g pm2
fi

if pm2 describe event-api > /dev/null 2>&1; then
  pm2 restart event-api
else
  pm2 start dist/index.js --name event-api
fi
pm2 save
ok "pm2 running"

# ─── 8/8 — Verify ───────────────────────────────────────────────
log "8/8 — Verify"
sleep 3
if curl -sf http://localhost:3000/health > /tmp/health.json; then
  ok "Server responding!"
  echo "Response: $(cat /tmp/health.json)"
else
  warn "Server belum respond. Cek: pm2 logs event-api"
fi

# ─── Summary ────────────────────────────────────────────────────
TAB_IP=$(ifconfig 2>/dev/null | grep -E "inet [0-9]" | grep -v 127.0.0.1 \
         | awk '{print $2}' | head -1 || echo "?")

echo ""
echo "═══════════════════════════════════════════════════════════"
ok "BOOTSTRAP DONE 🎉"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "  Server: http://localhost:3000"
echo "  IP tab: $TAB_IP   (akses dari laptop WiFi sama: http://$TAB_IP:3000)"
echo ""
echo "  Logs:        pm2 logs event-api"
echo "  Status:      pm2 status"
echo "  Restart:     pm2 restart event-api"
echo "  Update code: cd $APP_DIR && git pull && npm run build && pm2 restart event-api"
echo ""
echo "  Expose ke internet:"
echo "    cloudflared tunnel --url http://localhost:3000"
echo ""
echo "  Setup SSH (biar laptop bisa remote):"
echo "    bash $APP_DIR/scripts/setup-ssh.sh"
echo ""
