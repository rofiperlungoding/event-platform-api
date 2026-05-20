# Deploy ke Tablet — One-Shot Bootstrap

Dari kondisi tab fresh sampai server jalan + bisa di-SSH dari laptop, semuanya 1 script.

Ada **2 path**, pilih sesuai kenyamanan:

- **Path A — Mandiri:** Lo eksekusi sendiri di Termux. Script handle semuanya.
- **Path B — Remote (recommended):** Setup SSH dulu, abis itu Kiro/Claude bisa nyetir tablet lo dari laptop.

---

## PHASE 0 — Install Termux (di tablet, manual)

Bagian ini wajib manual karena lo install app Android, bukan ngetik di shell.

1. Buka browser di tab → `https://f-droid.org`
2. Download F-Droid APK, install (kasih izin "install dari sumber tidak dikenal")
3. Buka F-Droid → search **"Termux"** → install
4. Buka Termux pertama kali, tunggu bootstrap selesai (~15MB download)

⚠️ **Jangan install Termux dari Play Store.** Versi Play Store ditinggalkan sejak 2020.

✅ Selesai kalau lo udah liat prompt `$` di Termux.

---

## PATH A — Mandiri (Eksekusi Langsung di Tab)

Di Termux, paste 1 baris ini:

```bash
curl -sSL https://raw.githubusercontent.com/rofiperlungoding/event-platform-api/main/scripts/bootstrap.sh | bash
```

Script bakal:
1. Update package list
2. Install: git, node 22, postgres 16, nano, ssh, cloudflared
3. Setup PostgreSQL (init datadir, bikin user + db)
4. Clone repo lo
5. Bikin `.env`
6. `npm ci` + build
7. Start server pake pm2
8. Verify dengan `curl /health`

**Estimasi: 15-30 menit**, paling lama di `npm ci`.

Selesai → server lo jalan di `http://localhost:3000`.

---

## PATH B — Remote Control via SSH (Recommended)

Lebih cepet untuk pemula karena Kiro/Claude bisa eksekusi di laptop, output langsung kelihatan.

### B.1 — Setup SSH di tab

Di Termux, paste:

```bash
pkg install -y openssh && passwd && sshd && ifconfig | grep 'inet ' | grep -v 127.0.0.1
```

- Lo bakal diminta password 2x — ini password buat SSH login. Tulis di notes.
- Output terakhir kasih IP tab. Contoh: `inet 192.168.1.42 ...` → IP nya `192.168.1.42`.
- Tab dan laptop **harus di WiFi yang sama**.

### B.2 — Test dari laptop

Di laptop:
```bash
ssh -p 8022 <username>@<ip-tab>
```

`<username>` = username Termux (ketik `whoami` di Termux buat tahu).

Pertama kali bakal nanya "are you sure...?" → ketik `yes`. Masukkan password.

Kalau berhasil, lo udah di shell tab dari laptop. **Dari sini Kiro bisa take over** — tinggal lo kasih credentials, gue jalanin script bootstrap dari laptop.

### B.3 — Run bootstrap (dari SSH session)

Setelah SSH masuk:

```bash
curl -sSL https://raw.githubusercontent.com/rofiperlungoding/event-platform-api/main/scripts/bootstrap.sh | bash
```

Sama persis dengan Path A, tapi log-nya muncul di laptop lo.

---

## Setelah Bootstrap Selesai

### Test API dari laptop

Kalau di WiFi yang sama:
```bash
curl http://<ip-tab>:3000/health
```

Output: `{"status":"ok",...}`

### Expose ke internet via Cloudflare

Di Termux (atau via SSH):
```bash
cloudflared tunnel --url http://localhost:3000
```

Bakal kasih URL public `https://xxxx.trycloudflare.com`. Buka di browser laptop, tambahin `/health`.

### Update code workflow

```bash
# di laptop
git push

# di tab (atau via SSH)
cd ~/projects/event-platform-api
git pull && npm run build && pm2 restart event-api
```

---

## Auto-Start Saat Tab Boot (Opsional)

Install **Termux:Boot** dari F-Droid juga. Lalu:

```bash
mkdir -p ~/.termux/boot
cat > ~/.termux/boot/start-server <<'EOF'
#!/data/data/com.termux/files/usr/bin/bash
termux-wake-lock
sshd
pg_ctl -D $PREFIX/var/lib/postgresql -l $PREFIX/var/lib/postgresql/logfile start
sleep 3
pm2 resurrect
EOF
chmod +x ~/.termux/boot/start-server
```

Tab restart → semuanya auto-jalan.

---

## Troubleshooting

| Masalah | Solusi |
| ------- | ------ |
| Bootstrap stuck di `npm ci` | Normal di tab, bisa 10-15 menit. Tunggu. |
| `pg_ctl: command not found` | `pkg install postgresql` lalu rerun bootstrap |
| `port 5432 already in use` saat start postgres | Postgres udah jalan, skip. Atau `pg_ctl -D $PREFIX/var/lib/postgresql stop` lalu retry |
| SSH "connection refused" | sshd ga jalan. Di tab: `pkill sshd && sshd` |
| SSH "host key verification failed" | Di laptop: `ssh-keygen -R '[<ip-tab>]:8022'` lalu coba lagi |
| Tab IP berubah | Cek lagi: `ifconfig | grep 'inet '`. Untuk IP tetap, set DHCP reservation di router |
| Server mati saat layar tab off | `termux-wake-lock` (sekali). pm2 + wake-lock bikin server persistent |
| Cloudflared URL beda tiap restart | Quick tunnel memang gitu. Buat domain tetap pake Named Tunnel (advanced). |
