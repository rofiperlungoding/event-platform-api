#!/data/data/com.termux/files/usr/bin/bash
# ─────────────────────────────────────────────────────────────────
#  Setup SSH server di Termux
#  Setelah ini, laptop bisa SSH ke tablet di WiFi yang sama.
# ─────────────────────────────────────────────────────────────────

set -e

c_cyan='\033[1;36m'; c_green='\033[1;32m'; c_yellow='\033[1;33m'; c_off='\033[0m'
log() { echo -e "\n${c_cyan}▶ $1${c_off}"; }
ok()  { echo -e "${c_green}✓ $1${c_off}"; }

log "1/4 — Install openssh"
pkg install -y openssh
ok "openssh installed"

log "2/4 — Set password Termux user"
echo ""
echo "Lo bakal diminta password baru 2x. INI password buat SSH login dari laptop."
echo "Pilih password yang lo INGAT, atau tulis di notes."
echo ""
passwd
ok "Password set"

log "3/4 — Start sshd"
pkill sshd 2>/dev/null || true
sleep 1
sshd
sleep 2

if pgrep -af sshd > /dev/null; then
  ok "sshd running"
else
  echo "ERROR: sshd gagal start. Coba manual: sshd"
  exit 1
fi

log "4/4 — Connection info"
USERNAME=$(whoami)
TAB_IP=$(ifconfig 2>/dev/null | grep -E "inet [0-9]" | grep -v 127.0.0.1 \
         | awk '{print $2}' | head -1)
SSH_PORT=8022

echo ""
echo "═══════════════════════════════════════════════════════════"
ok "SSH READY"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Dari LAPTOP (yang konek WiFi sama dengan tab), ketik:"
echo ""
echo "    ssh -p $SSH_PORT $USERNAME@$TAB_IP"
echo ""
echo "Pakai password yang baru lo set di atas."
echo ""
echo "Kalau IP tab berubah (ganti WiFi/restart), cek lagi dengan:"
echo "    ifconfig | grep 'inet '"
echo ""
echo "Restart sshd: pkill sshd && sshd"
echo ""
