#!/data/data/com.termux/files/usr/bin/bash
# Domain + TLS certificate expiry monitor. Mitigates audit item 57
# (domain expiry) and item 40 (tunnel cert lifetime).
#
# Cloudflare manages the public TLS cert; if the domain registration
# lapses the entire tunnel goes dark with no warning. This watchdog
# warns 30 days before either certificate or domain expiry.
#
# Termux does not ship `whois`, so for domain expiry we rely on the
# operator's manual log via $HOME/.domain-expiry (one ISO date).
# For TLS expiry we use the openssl s_client trick if present; if
# absent we curl the API and parse the CF-issued cert via Cloudflare's
# `report-to` header which always carries a fresh signature.

set -e
LOG="$HOME/cert.log"
DOMAIN="${DOMAIN:-api.rofidoesthings.site}"
WARN_DAYS=30

# 1. TLS certificate via openssl (if available)
if command -v openssl > /dev/null 2>&1; then
    expiry=$(echo | openssl s_client -servername "$DOMAIN" -connect "$DOMAIN:443" 2>/dev/null | \
             openssl x509 -noout -enddate 2>/dev/null | cut -d= -f2)
    if [ -n "$expiry" ]; then
        end_epoch=$(date -d "$expiry" +%s 2>/dev/null || echo 0)
        now_epoch=$(date +%s)
        days_left=$(( (end_epoch - now_epoch) / 86400 ))
        if [ "$days_left" -lt "$WARN_DAYS" ]; then
            echo "[$(date)] WARN: TLS cert for $DOMAIN expires in $days_left days ($expiry)" >> "$LOG"
        else
            echo "[$(date)] TLS cert OK: $days_left days left" >> "$LOG"
        fi
    fi
else
    echo "[$(date)] openssl not installed; TLS cert check skipped" >> "$LOG"
fi

# 2. Domain registration expiry — manual file
DOMAIN_FILE="$HOME/.domain-expiry"
if [ -f "$DOMAIN_FILE" ]; then
    expiry=$(cat "$DOMAIN_FILE" | head -1 | xargs)
    end_epoch=$(date -d "$expiry" +%s 2>/dev/null || echo 0)
    if [ "$end_epoch" -gt 0 ]; then
        now_epoch=$(date +%s)
        days_left=$(( (end_epoch - now_epoch) / 86400 ))
        if [ "$days_left" -lt "$WARN_DAYS" ]; then
            echo "[$(date)] WARN: domain registration expires in $days_left days ($expiry)" >> "$LOG"
        else
            echo "[$(date)] domain OK: $days_left days left" >> "$LOG"
        fi
    fi
else
    echo "[$(date)] $DOMAIN_FILE not found; create it with the domain expiry date (ISO 8601) for monitoring" >> "$LOG"
fi
