#!/data/data/com.termux/files/usr/bin/bash
# ─────────────────────────────────────────────────────────────────
#  Update code di tab dari main branch
#  Run: bash scripts/termux-update.sh
# ─────────────────────────────────────────────────────────────────

set -e

APP_DIR="$HOME/projects/event-platform-api"
cd "$APP_DIR"

echo "▶ Pulling latest..."
git fetch origin
git reset --hard origin/main

# Reinstall deps kalau lockfile berubah
if ! git diff HEAD@{1} HEAD -- package-lock.json | grep -q .; then
  echo "▶ Lockfile unchanged, skip npm install"
else
  echo "▶ Lockfile changed, running npm ci..."
  npm ci --no-audit --no-fund
fi

# Apply new migrations kalau ada
if git diff --name-only HEAD@{1} HEAD -- prisma/migrations/ | grep -q .; then
  echo "▶ New migrations detected, deploying..."
  npx prisma migrate deploy
  npx prisma generate
fi

echo "▶ Rebuilding..."
npm run build

echo "▶ Restarting pm2..."
pm2 restart event-api --update-env

echo ""
echo "✓ Updated to $(git rev-parse --short HEAD) — $(git log -1 --pretty=%s)"
echo ""
pm2 list
