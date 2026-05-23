#!/data/data/com.termux/files/usr/bin/bash
# Install boot script for Termux:Boot autostart
# Run via webhook: POST /deploy/run/install-boot?secret=...
#
# Round 10 hardening (audit items 278, 280):
#   - Verify the source script exists before copying.
#   - Verify the destination directory is writable.
#   - Emit a warning log line if Termux:Boot APK is not detected,
#     so the operator knows to install it before the next reboot.

trap - CHLD
set -euo pipefail

LOG="$HOME/install-boot.log"
SRC="$HOME/projects/event-platform-api/deploy/boot-script.sh"
DST_DIR="$HOME/.termux/boot"
DST="$DST_DIR/start-event-stack"

echo "[$(date)] === INSTALL BOOT START ===" >> "$LOG"

if [ ! -f "$SRC" ]; then
    echo "[$(date)] ERROR: source script $SRC not found" >> "$LOG"
    exit 1
fi

mkdir -p "$DST_DIR"
if [ ! -w "$DST_DIR" ]; then
    echo "[$(date)] ERROR: $DST_DIR not writable" >> "$LOG"
    exit 1
fi

cp "$SRC" "$DST"
chmod +x "$DST"

# Heuristic check that the Termux:Boot APK is actually installed —
# the directory exists either way, but only with the APK does it
# actually fire on boot. We probe pm to detect the package; this is
# best-effort and never fails the install.
if command -v pm > /dev/null 2>&1; then
    if ! pm list packages 2>/dev/null | grep -q com.termux.boot; then
        echo "[$(date)] WARN: com.termux.boot package not detected. Install Termux:Boot APK from F-Droid for autostart to work." >> "$LOG"
    fi
fi

echo "[$(date)] Installed:" >> "$LOG"
ls -la "$DST_DIR/" >> "$LOG"
echo "[$(date)] === DONE ===" >> "$LOG"
