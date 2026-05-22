#!/data/data/com.termux/files/usr/bin/bash
# Generic email sender. Used for participant registration confirmations
# and operator alerts. Wraps `msmtp` for SMTP delivery.
#
# Configuration via $HOME/.msmtprc (standard msmtp config). Example:
#   account default
#   host smtp.gmail.com
#   port 587
#   from event-platform@example.com
#   user youraccount@gmail.com
#   password yourapppassword
#   auth on
#   tls on
#   tls_starttls on
#
# Usage:
#   send-email.sh <to> "<subject>" <<<"body text"
#
# Or with template:
#   TEMPLATE=registration ./send-email.sh user@example.com "Welcome"

trap - CHLD
set -e

TO="$1"
SUBJECT="$2"

if [ -z "$TO" ] || [ -z "$SUBJECT" ]; then
    echo "Usage: $0 <to> <subject> [< body]" >&2
    exit 1
fi

if ! command -v msmtp >/dev/null 2>&1; then
    echo "msmtp not installed. Install with: pkg install msmtp" >&2
    exit 1
fi

LOG="$HOME/email.log"

# Read body from stdin
BODY=$(cat)

# Build RFC 5322 message
{
    echo "To: $TO"
    echo "Subject: $SUBJECT"
    echo "Content-Type: text/plain; charset=utf-8"
    echo "MIME-Version: 1.0"
    echo
    echo "$BODY"
} | msmtp --read-recipients --read-envelope-from "$TO" \
    && echo "[$(date)] sent → $TO ($SUBJECT)" >> "$LOG" \
    || { echo "[$(date)] FAILED → $TO" >> "$LOG"; exit 1; }
