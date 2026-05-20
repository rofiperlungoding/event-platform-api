# Deploy ke Tablet — Pure Termux (No Docker)

Guide ini buat tablet dari **kondisi fresh**, belum di-setup apa-apa. Pake pure Termux native install, bukan proot/Docker — supaya ringan di RAM 3GB.

Total waktu realistis: **30-60 menit** (paling lama itu `npm install` di tablet, sabar).

---

## PHASE 0 — Install Apps (di tablet)

### 0.1 — Download F-Droid

Buka browser di tablet, pergi ke:
```
https://f-droid.org
```

Klik tombol **"Download F-Droid"** (file APK ~10MB). Setelah download:
- Buka file APK
- Android bakal warning "install dari sumber tidak dikenal" — kasih izin
- Install F-Droid

### 0.2 — Install Termux dari F-Droid

Buka F-Droid app → search **"Termux"** → install. Tunggu sampai selesai.

⚠️ **Jangan install Termux dari Play Store.** Versinya sudah ditinggalkan sejak 2020, banyak package-nya ga jalan.

### 0.3 — Install Termux:Boot (opsional tapi recommended)

Di F-Droid yang sama, search **"Termux:Boot"** → install. Ini biar server lo bisa auto-start kalau tablet restart. Setup-nya nanti di Phase 6.

**Checkpoint Phase 0:** Lo punya icon Termux di home screen tab.

---

## PHASE 1 — First Boot Termux

### 1.1 — Buka Termux

Pertama kali buka, dia bakal download bootstrap (~15MB). Tunggu sampai ada prompt `$`.

### 1.2 — Kasih akses storage

Ketik:
```bash
termux-setup-storage
```

Android popup minta permission — Allow.

### 1.3 — Update package list

```bash
pkg update -y && pkg upgrade -y
```

Kalau ditanya "Configuration file changed... [Y/n]" tinggal tekan Enter (default keep current).

**Checkpoint Phase 1:** Lo bisa ngetik `pkg --version` dan dapet response. Lapor ke gue dengan output `uname -a` dan `df -h /data` (cek storage tablet).

---

## PHASE 2 — Install Tools

```bash
pkg install -y git nodejs-lts postgresql nano openssh
```

Ini bakal download ~150MB. Tunggu.

Verify setelah selesai:
```bash
git --version
node --version
npm --version
psql --version
```

Yang lo harus liat (kira-kira):
```
git version 2.x.x
v22.x.x         ← node
10.x.x          ← npm
psql (PostgreSQL) 16.x
```

**Checkpoint Phase 2:** Semua 4 command di atas kasih versi, ga ada "command not found".

---

## PHASE 3 — Setup PostgreSQL

PostgreSQL di Termux beda dari Ubuntu — datadir-nya manual, ga ada systemd. Sekali setup, beres.

### 3.1 — Initialize database

```bash
mkdir -p $PREFIX/var/lib/postgresql
initdb $PREFIX/var/lib/postgresql
```

### 3.2 — Start PostgreSQL

```bash
pg_ctl -D $PREFIX/var/lib/postgresql -l $PREFIX/var/lib/postgresql/logfile start
```

Output: `server started`. Kalau dapet itu, jalan.

### 3.3 — Bikin database & user

```bash
createuser --superuser rofi
createdb -O rofi eventplatform
```

(`rofi` = nama user database lo, sama kayak yang ada di `.env`. Bebas mau ganti, tapi nanti `.env` juga sesuain.)

### 3.4 — Set password buat user `rofi`

```bash
psql -d postgres -c "ALTER USER rofi WITH PASSWORD 'devsecret';"
```

(Untuk latihan boleh `devsecret`. Production beneran ganti yang kuat.)

### 3.5 — Test koneksi

```bash
psql -U rofi -d eventplatform -c "SELECT version();"
```

Harus muncul versi PostgreSQL. Kalau iya, **DB siap**.

**Checkpoint Phase 3:** Lapor output dari step 3.5.

---

## PHASE 4 — Clone & Setup App

### 4.1 — Clone repo

```bash
mkdir -p ~/projects && cd ~/projects
git clone https://github.com/rofiperlungoding/event-platform-api.git
cd event-platform-api
```

### 4.2 — Bikin file .env

```bash
nano .env
```

Isi dengan:
```
DATABASE_URL="postgresql://rofi:devsecret@localhost:5432/eventplatform"
PORT=3000
NODE_ENV=production
```

Save: `Ctrl+O` → Enter → `Ctrl+X`.

### 4.3 — Install dependencies

```bash
npm ci
```

⚠️ **Ini bagian paling lama.** Di tablet bisa 5-15 menit. Jangan tutup Termux. Kalau layar mati, install pake ini biar Termux tetep jalan:
```bash
termux-wake-lock
```
(Run sekali, abis itu ga perlu lagi sampai reboot.)

### 4.4 — Generate Prisma client

```bash
npx prisma generate
```

### 4.5 — Apply database migrations

```bash
npx prisma migrate deploy
```

Output: `X migrations have been successfully applied.`

### 4.6 — Build TypeScript ke JavaScript

```bash
npm run build
```

Output: ga ada error (silent kalau sukses).

**Checkpoint Phase 4:** Lapor isi folder `dist/` dengan `ls dist/`.

---

## PHASE 5 — Jalanin Server

### 5.1 — Test run

```bash
node dist/index.js
```

Lo bakal liat log:
```
INFO: Server listening at http://0.0.0.0:3000
```

### 5.2 — Test dari Termux yang sama

Buka **session Termux baru** (swipe dari kiri di Termux → New session). Di session baru, ketik:
```bash
curl http://localhost:3000/health
```

Output yang lo mau:
```json
{"status":"ok","uptime":...}
```

### 5.3 — Bikin participant baru via API

```bash
curl -X POST http://localhost:3000/register \
  -H "Content-Type: application/json" \
  -d '{"name":"Rofi","email":"rofi@tablet.test","team":"Alpha"}'
```

Harus dapet response:
```json
{"id":1,"name":"Rofi",...}
```

**Checkpoint Phase 5:** Lapor output `curl /health` + `curl POST /register`. Kalau dapet response, **API lo udah jalan di tablet.** 🎉

Stop server dulu di session pertama: `Ctrl+C`.

---

## PHASE 6 — Bikin Server Persistent

Sekarang server cuma jalan kalau Termux kebuka. Kita bikin auto-start.

### 6.1 — Install pm2 (process manager)

```bash
npm install -g pm2
```

### 6.2 — Start app dengan pm2

```bash
cd ~/projects/event-platform-api
pm2 start dist/index.js --name event-api
pm2 save
```

`pm2 save` simpan state-nya. Cek:
```bash
pm2 status
pm2 logs event-api
```

(`Ctrl+C` keluar dari log view, app tetep jalan.)

### 6.3 — Auto-start postgres + pm2 saat tablet boot

Bikin script boot Termux:Boot:
```bash
mkdir -p ~/.termux/boot
nano ~/.termux/boot/start-server
```

Isi:
```bash
#!/data/data/com.termux/files/usr/bin/bash
termux-wake-lock
pg_ctl -D $PREFIX/var/lib/postgresql -l $PREFIX/var/lib/postgresql/logfile start
sleep 3
pm2 resurrect
```

Save (`Ctrl+O`, `Ctrl+X`), lalu kasih executable:
```bash
chmod +x ~/.termux/boot/start-server
```

Sekarang setiap tablet restart, postgres + app lo auto-jalan. Test dengan reboot tablet (kalau lo PD).

---

## PHASE 7 — Expose ke Internet

### 7.1 — Install cloudflared

```bash
pkg install -y cloudflared
```

### 7.2 — Run quick tunnel

```bash
cloudflared tunnel --url http://localhost:3000
```

Output bakal kasih URL kayak:
```
https://xxxx-yyy-zzz.trycloudflare.com
```

### 7.3 — Test dari laptop

Di laptop lo, buka browser:
```
https://xxxx-yyy-zzz.trycloudflare.com/health
```

Harus dapet `{"status":"ok",...}`.

**🎉 API lo live di internet, di-host dari tablet lo sendiri.**

---

## Workflow Update Code

Tiap lo edit kode di laptop:

```bash
# di laptop
git add .
git commit -m "feat: ..."
git push
```

Di tablet:
```bash
cd ~/projects/event-platform-api
git pull
npm ci                        # cuma kalau ada deps baru
npx prisma migrate deploy     # cuma kalau ada migration baru
npm run build
pm2 restart event-api
```

---

## Troubleshooting Cepat

| Masalah | Solusi |
| ------- | ------ |
| `pg_ctl: command not found` | `pkg install postgresql` |
| postgres ga mau start, error "could not bind" | postgres udah jalan, skip step start. Cek `pg_ctl status -D $PREFIX/var/lib/postgresql` |
| `npm install` stuck/error | Cek koneksi internet, ulang. Kalau berulang error spesifik, kirim screenshot |
| Server mati saat layar tab off | Run `termux-wake-lock` sekali |
| Cloudflared URL ganti tiap restart | Normal untuk quick tunnel. Buat domain tetap pake "Named Tunnel" — itu fase lanjut |
| Tablet panas | Kasih kipas / lepas casing. Jangan run di bawah bantal |

---

## Yang Penting Buat Lo Tahu

1. **Termux ≠ Linux normal.** Path-nya beda (`$PREFIX/...`), ga ada systemd, ga ada `sudo` tradisional. Tapi 90% command shell sama.

2. **PostgreSQL di Termux jalan tanpa root.** Datadir di `$PREFIX/var/lib/postgresql`. Itu kenapa step 3.1 perlu `initdb` manual.

3. **pm2 vs systemctl** — di server beneran (Ubuntu) lo pake systemd. Di tablet pake pm2 karena ga ada systemd. Konsep sama: process supervisor.

4. **Quick tunnel ≠ production.** URL Cloudflare gratis itu ganti tiap restart. Buat latihan sih cukup. Buat sungguhan, daftar domain + Named Tunnel.

5. **Storage tablet.** 3GB RAM cukup. Tapi storage internal? Cek `df -h $PREFIX` — kalau di bawah 3GB free, hati-hati `npm install` bisa fail.
