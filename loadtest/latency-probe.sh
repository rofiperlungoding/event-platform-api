#!/data/data/com.termux/files/usr/bin/bash
# Measure single-request latency vs concurrent
URL="http://localhost:3001/attendance/quick-checkin"
BODY='{"session_code":"NOPE","device_uuid":"x"}'

echo "=== sequential 10 requests ==="
for i in {1..10}; do
    start=$(date +%s%N)
    curl -sf -X POST "$URL" -H 'Content-Type: application/json' -d "$BODY" > /dev/null
    end=$(date +%s%N)
    echo "$(( (end - start) / 1000000 )) ms"
done

echo
echo "=== 100 concurrent (xargs) ==="
seq 1 100 | xargs -n1 -P100 -I{} sh -c "curl -sf -m 10 -o /dev/null -w '%{http_code} %{time_total}s\n' -X POST '$URL' -H 'Content-Type: application/json' -d '$BODY'" | sort | uniq -c | head -20
