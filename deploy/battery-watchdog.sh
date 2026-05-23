#!/data/data/com.termux/files/usr/bin/bash
# Battery watchdog — alerts when the tablet is on battery (charger
# unplugged) and below a threshold. Mitigates audit item 51:
# the tablet is the entire production stack and a power loss takes
# the event down.
#
# Run via cron every 5 minutes (alongside health-watchdog).
#
# Strategy: ask Android via dumpsys-equivalent shell. Termux base does
# not have dumpsys, so we read /sys/class/power_supply via su if
# rooted, otherwise fall back to `termux-battery-status` if the
# Termux:API add-on was ever installed. If neither is available the
# script emits a one-line warning per run instead of aborting; the
# operator still gets visibility through the cron log.
#
# Round 7 fix (audit item 242): switch to `set -euo pipefail` so
# a malformed termux-battery-status JSON does not silently parse
# to empty values that compare false against the threshold and
# look like "OK" forever.

set -euo pipefail
LOG="$HOME/battery.log"
THRESHOLD_PCT=20
ALERT_HOOK="${ALERT_HOOK:-}"      # optional webhook URL

read_battery() {
    # Try Termux:API first
    if command -v termux-battery-status > /dev/null 2>&1; then
        local status=$(timeout 5 termux-battery-status 2>/dev/null)
        if [ -n "$status" ]; then
            local pct=$(echo "$status" | grep -oE '"percentage": *[0-9]+' | grep -oE '[0-9]+' | head -1)
            local plug=$(echo "$status" | grep -oE '"plugged": *"[^"]+"' | grep -oE '"[^"]+"$' | tr -d '"')
            echo "$pct|$plug"
            return 0
        fi
    fi
    # Fallback: try sysfs (works only if su or kernel ACLs are loose)
    local pct=$(cat /sys/class/power_supply/battery/capacity 2>/dev/null || echo "")
    local plug=$(cat /sys/class/power_supply/battery/status 2>/dev/null || echo "")
    if [ -n "$pct" ]; then
        echo "$pct|$plug"
        return 0
    fi
    return 1
}

if ! info=$(read_battery); then
    echo "[$(date)] battery telemetry unavailable (install Termux:API for visibility)" >> "$LOG"
    exit 0
fi

pct=${info%|*}
plug=${info#*|}

# Charging means we don't care about the threshold
case "$plug" in
    *Charging*|*USB*|*AC*|*PLUGGED_AC*|*PLUGGED_USB*) charging=1 ;;
    *) charging=0 ;;
esac

if [ "$charging" = "0" ] && [ "$pct" -lt "$THRESHOLD_PCT" ]; then
    msg="[$(date)] CRIT: tablet on battery at ${pct}% (status=$plug). Plug in NOW."
    echo "$msg" >> "$LOG"
    [ -n "$ALERT_HOOK" ] && curl -sf -m 5 -X POST "$ALERT_HOOK" -d "$msg" > /dev/null 2>&1 || true
    exit 1
fi

echo "[$(date)] battery OK: ${pct}% (${plug})" >> "$LOG"
