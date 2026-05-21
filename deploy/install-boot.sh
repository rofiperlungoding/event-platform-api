#!/data/data/com.termux/files/usr/bin/bash
# Install boot script for Termux:Boot autostart
# Run via webhook: POST /deploy/run/install-boot?secret=...

trap - CHLD
set -e

LOG="$HOME/install-boot.log"
echo "[$(date)] === INSTALL BOOT START ===" >> "$LOG"

mkdir -p "$HOME/.termux/boot"
cp "$HOME/projects/event-platform-api/deploy/boot-script.sh" "$HOME/.termux/boot/start-event-stack"
chmod +x "$HOME/.termux/boot/start-event-stack"

echo "[$(date)] Installed:" >> "$LOG"
ls -la "$HOME/.termux/boot/" >> "$LOG"
echo "[$(date)] === DONE ===" >> "$LOG"
