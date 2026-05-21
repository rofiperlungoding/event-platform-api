#!/data/data/com.termux/files/usr/bin/bash
# Setup cron jobs on tablet
# Run once: bash setup-cron.sh

trap - CHLD

# Install cron if not already
pkg install -y cronie >/dev/null 2>&1 || true

# Backup db daily at 3am
CRON_LINE_BACKUP="0 3 * * * /data/data/com.termux/files/usr/bin/bash $HOME/projects/event-platform-api/deploy/db-backup.sh"

# pm2 log rotate weekly (Sunday 4am)
CRON_LINE_LOGS="0 4 * * 0 /data/data/com.termux/files/usr/bin/pm2 flush"

# Get existing crontab, add lines if not present
EXISTING=$(crontab -l 2>/dev/null || echo "")

NEW=""
echo "$EXISTING" | grep -qF "db-backup.sh" || NEW="$NEW$CRON_LINE_BACKUP\n"
echo "$EXISTING" | grep -qF "pm2 flush" || NEW="$NEW$CRON_LINE_LOGS\n"

if [ -n "$NEW" ]; then
    (echo "$EXISTING"; echo -e "$NEW") | crontab -
    echo "Cron jobs installed."
else
    echo "Cron jobs already in place."
fi

# Start crond if not running (background, no -L flag for cronie)
pgrep crond > /dev/null || nohup crond < /dev/null > $HOME/cron.log 2>&1 &
sleep 1
echo ""
echo "Cron daemon: $(pgrep -af crond | head -1)"
echo ""
echo "Active cron jobs:"
crontab -l
