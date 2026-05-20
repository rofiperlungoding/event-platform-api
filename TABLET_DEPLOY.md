# Deploy ke Tablet — Step by Step

Panduan ini lo eksekusi di **tablet** (Termux + proot Ubuntu). Kalau ada error, screenshot kirim ke Claude/Kiro.

## Prasyarat di Tablet

Lo udah punya (dari setup sebelumnya):
- Termux (dari F-Droid)
- proot-distro Ubuntu
- Cloudflared

Cek dulu kondisi sekarang. Login Termux → masuk Ubuntu:

```bash
termux-wake-lock          # tablet ga sleep
proot-distro login ubuntu
```

Di dalam Ubuntu, cek tools:

```bash
git --version
docker --version          # kalau "command not found", lanjut step install
docker compose version
cloudflared --version
```

## Step 1 — Install Docker (kalau belum ada)

⚠️ Docker di proot Ubuntu kadang bermasalah karena kernel limitation. Kalau gagal, kita pake alternatif (lihat di bawah).

```bash
apt update && apt install -y curl
curl -fsSL https://get.docker.com | sh
```

Test:
```bash
docker run --rm hello-world
```

**Kalau error "cannot connect to Docker daemon" atau cgroup error:**
Docker daemon ga bisa jalan di proot. Skip ke **Plan B** di bawah.

## Step 2 — Clone repo

```bash
mkdir -p ~/projects && cd ~/projects
git clone https://github.com/rofiperlungoding/event-platform-api.git
cd event-platform-api
```

## Step 3 — Setup environment

```bash
cp .env.prod.example .env.prod
nano .env.prod
```

Ganti `POSTGRES_PASSWORD` jadi password kuat random (bukan "changeme..."). Contoh generate:

```bash
openssl rand -base64 24
```

Copy hasilnya, paste sebagai password. Save (`Ctrl+O`, Enter, `Ctrl+X`).

## Step 4 — Build & run

```bash
docker compose -f docker-compose.prod.yml --env-file .env.prod up -d --build
```

Ini bakal:
1. Pull `postgres:16-alpine` (~80MB)
2. Build app image lo (~5-10 menit di tablet, sabar)
3. Run migrations otomatis
4. Start app di port 3000

Cek status:
```bash
docker compose -f docker-compose.prod.yml ps
docker compose -f docker-compose.prod.yml logs -f app
```

Test lokal di tablet:
```bash
curl http://localhost:3000/health
```

Harus respon: `{"status":"ok","uptime":...}`

## Step 5 — Expose ke internet

```bash
cloudflared tunnel --url http://localhost:3000
```

Bakal kasih URL kayak `https://xxxxx-yyy-zzz.trycloudflare.com`. Buka URL itu + `/health` di browser laptop — kalau dapet response, **API lo udah live di internet.**

## Workflow Update Code

Tiap lo edit code di laptop:

```bash
# di laptop
git add .
git commit -m "feat: tambah endpoint X"
git push
```

Di tablet, pull + rebuild:
```bash
cd ~/projects/event-platform-api
git pull
docker compose -f docker-compose.prod.yml --env-file .env.prod up -d --build
```

---

## Plan B — Kalau Docker Ga Jalan di proot

Skip Docker, run native. Postgres + Node langsung di Ubuntu.

```bash
# Install postgres
apt install -y postgresql postgresql-contrib

# Start postgres (proot ga punya systemctl)
service postgresql start

# Buat user & db
sudo -u postgres psql -c "CREATE USER rofi WITH PASSWORD 'devsecret' SUPERUSER;"
sudo -u postgres psql -c "CREATE DATABASE eventplatform OWNER rofi;"

# Install Node 20
curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
apt install -y nodejs

# Setup app
cd ~/projects/event-platform-api
npm ci
npx prisma generate
npx prisma migrate deploy
npm run build

# Run via pm2 (process manager, restart kalau crash)
npm install -g pm2
pm2 start dist/index.js --name event-api
pm2 save
```

DATABASE_URL di `.env`:
```
DATABASE_URL="postgresql://rofi:devsecret@localhost:5432/eventplatform"
PORT=3000
```

Test:
```bash
curl http://localhost:3000/health
```

Cloudflare Tunnel sama kayak Step 5.

---

## Troubleshooting

| Error | Solusi |
| ----- | ------ |
| `docker: command not found` | Install ulang atau pake Plan B |
| `Cannot connect to Docker daemon` | proot kernel limitation, pake Plan B |
| `port 5432 already in use` | `sudo service postgresql stop` (kalau ada native postgres) |
| Build app lambat banget | Normal di tablet, bisa 5-15 menit. Sabar. |
| `EACCES` permission denied | `chmod -R u+rwx .` di folder project |
| Cloudflared putus tiap idle | Jalankan di `tmux` atau `screen` biar persistent |

## Checkpoint

Lo selesai kalau:
- [ ] Docker container atau native process jalan
- [ ] `curl localhost:3000/health` respon `ok` di tablet
- [ ] Cloudflare Tunnel kasih URL publik
- [ ] URL publik + `/health` bisa dibuka dari browser laptop
- [ ] POST `/register` dari laptop ke URL publik berhasil bikin participant
