#!/data/data/com.termux/files/usr/bin/bash
# ─────────────────────────────────────────────────────────────────
#  Setup SSH server di Termux
#  Setelah ini, laptop bisa SSH ke tablet di WiFi yang sama.
#
#  Usage (interactive):
#    curl -sSL <url> | bash
#
#  Usage (non-interactive, password via env):
#    curl -sSL <url> | PASSWORD=yourpass bash
# ─────────────────────────────────────────────────────────────────

set -e

c_cyan='\033[1;36m'; c_green='\033[1;32m'; c_yellow='\033[1;33m'; c_off='\033[0m'
log() { echo -e "\n${c_cyan}▶ $1${c_off}"; }
ok()  { echo -e "${c_green}✓ $1${c_off}"; }
warn(){ echo -e "${c_yellow}! $1${c_off}"; }

# ─── 1/4 — Install openssh ──────────────────────────────────────
log "1/4 — Install openssh"
pkg install -y openssh
ok "openssh installed"

# ─── 2/4 — Set password ─────────────────────────────────────────
log "2/4 — Set password Termux user"

USERNAME=$(whoami)

if [ -n "${PASSWORD:-}" ]; then
  # Non-interactive: pipe password 2x ke passwd
  echo "Setting password non-interactively for user: $USERNAME"
  if printf '%s\n%s\n' "$PASSWORD" "$PASSWORD" | passwd "$USERNAME" 2>&1 | tail -3; then
    ok "Password set (non-interactive)"
  else
    warn "passwd via pipe gagal, mencoba metode lain..."
    # Fallback: pakai chpasswd kalau ada
    if command -v chpasswd > /dev/null 2>&1; then
      echo "$USERNAME:$PASSWORD" | chpasswd
      ok "Password set via chpasswd"
    else
      echo "ERROR: ga bisa set password non-interactive"
      echo "Coba ulang script tanpa env, dan run langsung (bukan curl|bash):"
      echo "  curl -sSL <url> -o /tmp/ssh.sh && bash /tmp/ssh.sh"
      exit 1
    fi
  fi
else
  echo ""
  echo "Lo bakal diminta password baru 2x. INI password buat SSH login dari laptop."
  echo "TIPS: kalau ke-skip, run dengan env: PASSWORD=yourpass curl ... | bash"
  echo ""
  if [ -e /dev/tty ]; then
    passwd < /dev/tty
  else
    passwd
  fi
  ok "Password set"
fi

# ─── 3/4 — Start sshd ───────────────────────────────────────────
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

# ─── 4/4 — Connection info ──────────────────────────────────────
log "4/4 — Connection info"
TAB_IP=$(ifconfig 2>/dev/null | grep -E "inet [0-9]" | grep -v 127.0.0.1 \
         | awk '{print $2}' | head -1)
SSH_PORT=8022

echo ""
echo "═══════════════════════════════════════════════════════════"
ok "SSH READY"
echo "═══════════════════════════════════════════════════════════"
echo ""
echo "Username Termux: $USERNAME"
echo "IP tab:          $TAB_IP"
echo "Port:            $SSH_PORT"
echo ""
echo "Dari LAPTOP (WiFi sama), ketik:"
echo ""
echo "    ssh -p $SSH_PORT $USERNAME@$TAB_IP"
echo ""
echo "Restart sshd kalau perlu:    pkill sshd && sshd"
echo "Cek IP tab:                  ifconfig | grep 'inet '"
echo ""
