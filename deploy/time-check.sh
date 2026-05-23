#!/data/data/com.termux/files/usr/bin/bash
# Time drift check — mitigates audit items 76, 86.
#
# Compares the tablet's clock against a quorum of public NTP servers
# (each fetched via plain HTTP date header — no NTP client required
# in Termux). Warns if drift exceeds 5 seconds; the JWT verification
# tolerates drift up to TOKEN_EXPIRY (24 h) so the threshold is well
# below anything that breaks attendance, but we want early warning.
#
# Round 7 fix (audit item 241): `set -euo pipefail` so a broken
# curl pipe (e.g., DNS hiccup) does not look like an empty
# "Date:" header and silently degrade the quorum below the
# minimum sample threshold.

set -euo pipefail
LOG="$HOME/time.log"
THRESHOLD_SEC=5

# Use a quorum of independent providers. The Date header carries
# RFC 7231 GMT time. If any one server lies the median still wins.
PROVIDERS=(
    "https://www.google.com"
    "https://www.cloudflare.com"
    "https://www.kompas.com"
)

now_epoch=$(date -u +%s)

# Collect remote epochs
declare -a remote
for url in "${PROVIDERS[@]}"; do
    hdr=$(curl -sI -m 5 "$url" 2>/dev/null | grep -i '^date:' | head -1 | cut -d: -f2- | xargs)
    [ -z "$hdr" ] && continue
    epoch=$(date -u -d "$hdr" +%s 2>/dev/null || echo 0)
    [ "$epoch" -gt 0 ] && remote+=("$epoch")
done

if [ "${#remote[@]}" -lt 2 ]; then
    echo "[$(date)] insufficient NTP quorum (${#remote[@]} providers responded)" >> "$LOG"
    exit 0
fi

# Median of the quorum
sorted=($(printf '%s\n' "${remote[@]}" | sort -n))
mid=$((${#sorted[@]} / 2))
median=${sorted[$mid]}

drift=$((now_epoch - median))
abs=${drift#-}

if [ "$abs" -gt "$THRESHOLD_SEC" ]; then
    echo "[$(date)] WARN: clock drift is ${drift}s vs quorum median (samples: ${remote[*]})" >> "$LOG"
else
    echo "[$(date)] clock OK: drift ${drift}s" >> "$LOG"
fi
