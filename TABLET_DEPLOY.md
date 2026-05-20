# Deploy ke Tablet — One-Shot Bootstrap

Dari kondisi tab fresh sampai server jalan + bisa di-SSH dari laptop, semuanya 1 script.

Ada **2 path**, pilih sesuai kenyamanan:

- **Path A — Mandiri:** Lo eksekusi sendiri di Termux. Script handle semuanya.
- **Path B — Remote (recommended):** Setup SSH dulu, abis itu Kiro bisa nyetir tablet lo dari laptop via SSH.

---

## PHASE 0 — Install Termux (di tablet, manual)

Bagian ini wajib manual karena lo install app Android, bukan ngetik di shell.

1. Buka browser di tab → `https://f-droid.org`
2. Download F-Droid APK, install (kasih izin "install dari sumber tidak dikenal")
3. Buka F-Droid → search **"Termux"** → install
4. Sekalian install **"Termux:API"** dan **"Termux:Boot"** (buat wake-lock + auto-start)
5. Buka Termux pertama kali, tunggu bootstrap selesai (~15MB download)

⚠️ **Jangan install Termux dari Play Store.** Versi Play Store ditinggalkan sejak 2020.

✅ Selesai kalau lo udah liat prompt `$` di Termux.

---

## PATH A — Mandiri (Eksekusi Langsung di Tab)

Di Termux, paste 1 baris ini:

```bash
curl -sSL https://raw.githubusercontent.com/rofiperlungoding/event-platform-api/main/scripts/termux-setup.sh | bash
```

Script bakal:
1. Cek storage tab cukup (minimal 2GB free)
2. Update package list
3. Install: git, node 22, postgres 16, nano, ssh, cloudflared
4. Setup PostgreSQL (init datadir, bikin user + db)
5. Clone/sync repo lo
6. Bikin `.env`
7. `npm ci` + Prisma generate + migrate deploy + build
8. Start server pake pm2
9. Bikin auto-start script di `~/.termux/boot/`
10. Verify dengan `curl /health`

Idempotent — aman di-rerun kalau gagal di tengah.

**Estimasi: 15-30 menit**, paling lama di `npm ci` (5-15 menit).

Selesai → server lo jalan di `http://localhost:3000`.

---

## PATH B — Remote Control via SSH (Recommended)

Lebih cepet untuk pemula karena Kiro bisa eksekusi di laptop, output langsung kelihatan.

### B.1 — Setup SSH di tab

Di Termux, paste 1 baris (script handle semuanya):

```bash
curl -sSL https://raw.githubusercontent.com/rofiperlungoding/event-platform-api/main/scripts/setup-ssh.sh | bash
```

- Lo bakal diminta password 2x — ini password buat SSH login. Tulis di notes.
- Output akhir kasih command SSH siap copy-paste.
- Tab dan laptop **harus di WiFi yang sama**.

### B.2 — Test dari laptop

Output script akan kasih command lengkap:
```bash
ssh -p 8022 <username>@<ip-tab>
```

Pertama kali bakal nanya "are you sure...?" → ketik `yes`. Masukkan password.

Kalau berhasil, lo udah di shell tab dari laptop. **Dari sini Kiro bisa take over** — kasih Kiro IP + username, gue jalanin sisanya dari laptop lo.

### B.3 — Run setup (dari SSH session atau via Kiro)

```bash
curl -sSL https://raw.githubusercontent.com/rofiperlungoding/event-platform-api/main/scripts/termux-setup.sh | bash
```

Sama persis dengan Path A, tapi log-nya muncul di laptop lo.

---

## Setelah Setup Selesai

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
bash ~/projects/event-platform-api/scripts/termux-update.sh
```

Script ini auto-detect: kalau lockfile berubah → `npm ci`, kalau ada migration baru → `migrate deploy`, terus rebuild + restart pm2.

---

## Troubleshooting

| Masalah | Solusi |
| ------- | ------ |
| Setup stuck di `npm ci` | Normal di tab, bisa 10-15 menit. Tunggu. |
| Storage warning < 2GB | Hapus app/file lain dulu di tab. App ini butuh ~1.5GB |
| `pg_ctl: command not found` | Setup script auto-install postgres. Rerun. |
| `port 5432 already in use` | Postgres udah jalan, script handle. Aman. |
| SSH "connection refused" | sshd ga jalan. Di tab: `pkill sshd; sshd` |
| SSH "host key verification failed" | Di laptop: `ssh-keygen -R '[<ip-tab>]:8022'` lalu coba lagi |
| Tab IP berubah | Cek lagi: `ifconfig \| grep 'inet '`. Untuk IP tetap, set DHCP reservation di router |
| Server mati saat layar tab off | Script udah set `termux-wake-lock`. Pastikan Termux:API ke-install |
| Cloudflared URL beda tiap restart | Quick tunnel memang gitu. Buat domain tetap pake Named Tunnel (advanced) |

---

## Yang Lo Pelajari dari Setup Ini

1. **Idempotent scripts** — script bisa di-rerun tanpa nambah duplikasi atau error. Cek pattern `if [ ! -d ... ]` di setup. Ini wajib di automation.
2. **PostgreSQL tanpa systemd** — Termux ga punya systemd. Kita pake `pg_ctl` langsung. Ini juga jadi insight untuk container yang minimal.
3. **pm2 sebagai process manager** — replacement systemd buat Node.js. `pm2 save` + `pm2 resurrect` = auto-restart pattern.
4. **Boot script** — `~/.termux/boot/*` jalan saat reboot kalau Termux:Boot ke-install. Konsep sama kayak `/etc/rc.local` di Linux.
5. **SSH dari laptop ke tab** — port default Termux SSH adalah **8022** (bukan 22). Ini security trick Termux supaya ga bentrok dengan SSH server lain.
