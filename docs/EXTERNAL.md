# External Failure Modes and Mitigations

This document covers risks that originate **outside the platform itself** —
hardware, network, third-party services, adversaries, and operational
human error. Together with [`HARDENING.md`](HARDENING.md) it forms the
complete failure-mode inventory for the reference deployment.

The audit answers the question: **"Setting aside our own code — what can
break the platform during the actual event?"**

## Summary

| #   | Risk                                              | Class           | Status      |
| --- | ------------------------------------------------- | --------------- | ----------- |
| 51  | Power outage / charger unplugged                  | Hardware        | Mitigated   |
| 52  | Venue Wi-Fi router fails                          | Network         | Mitigated   |
| 53  | Cloudflare regional outage                        | Vendor          | Documented  |
| 54  | Venue ISP outage                                  | Network         | Mitigated   |
| 55  | DNS cache poisoning of `api.<domain>`             | Adversarial     | Mitigated   |
| 56  | Cloudflare account suspension                     | Vendor          | Documented  |
| 57  | Domain registration expiry                        | Operational     | Mitigated   |
| 58  | Cloudflare edge rate-limits venue NAT             | Vendor          | Mitigated   |
| 59  | DDoS attack on tunnel                             | Adversarial     | Mitigated   |
| 60  | Slow-loris HTTP attack                            | Adversarial     | Mitigated   |
| 61  | POST flood from a registered device               | Adversarial     | Mitigated   |
| 62  | Fake QR codes                                     | Adversarial     | Mitigated   |
| 63  | Phishing emails to admin                          | Adversarial     | Documented  |
| 64  | ARP spoofing on venue Wi-Fi                       | Adversarial     | Mitigated   |
| 65  | TLS strip on tunnel                               | Adversarial     | Documented  |
| 66  | `pkg upgrade` breaks PostgreSQL                   | Vendor          | Mitigated   |
| 67  | Termux app update wipes filesystem                | Vendor          | Mitigated   |
| 68  | Node update breaks load test runner               | Vendor          | Documented  |
| 69  | cloudflared binary auto-update bug                | Vendor          | Mitigated   |
| 70  | Cloudflare changes tunnel config format           | Vendor          | Documented  |
| 71  | Cloudflare changes free-tier pricing              | Vendor          | Documented  |
| 72  | Cloudflare blocks Indonesian IP range             | Vendor          | Documented  |
| 73  | GitHub webhook silently fails                     | Vendor          | Mitigated   |
| 74  | Termux:Boot mismatch after Android update         | Vendor          | Tracked     |
| 75  | Wi-Fi DHCP lease expiry mid-event                 | Network         | Mitigated   |
| 76  | Android system clock drift                        | Hardware        | Mitigated   |
| 77  | Mobile carrier blocks port 443                    | Network         | Documented  |
| 78  | CDN edge selection sub-optimal                    | Vendor          | Documented  |
| 79  | iOS Safari ITP wipes IndexedDB                    | Vendor          | Mitigated   |
| 80  | Chrome removes Background Sync API                | Vendor          | Mitigated   |
| 81  | Wrong session QR shown by admin                   | Operational     | Documented  |
| 82  | Admin password leaked via screen share            | Operational     | Documented  |
| 83  | Admin account deleted accidentally                | Operational     | Mitigated   |
| 84  | Tablet stolen mid-event                           | Hardware        | Documented  |
| 85  | Android force-update during event                 | Vendor          | Documented  |
| 86  | NTP server returns wrong time                     | Vendor          | Mitigated   |
| 87  | IPv4 / IPv6 mismatch                              | Network         | Mitigated   |
| 88  | Subnet / VLAN mismatch                            | Network         | Mitigated   |
| 89  | Captive portal on venue Wi-Fi                     | Network         | Mitigated   |
| 90  | cloudflared connection rotation                   | Vendor          | Mitigated   |

## Mitigations Detail

### 51. Power outage

`deploy/battery-watchdog.sh` runs every 5 minutes and writes warnings
to `~/battery.log` whenever the tablet is on battery and below 20 %.
On the reference Galaxy Tab A8 this gives at least 30 minutes of
runway from warning to shutdown. Operationally:

1. The tablet must be plugged in to a UPS or wall outlet during the
   event. The watchdog only detects the breach, not the resolution.
2. Termux:Boot acquires `termux-wake-lock` so the tablet does not
   doze even with screen off.
3. If a power cut is unavoidable, the offline-first PWA queues
   check-ins on attendees' phones; once power returns and the tablet
   reboots, queued items drain via `/attendance/batch-checkin`.

The watchdog requires the optional `Termux:API` add-on for accurate
readings on unrooted devices. Without it, the script logs a
"telemetry unavailable" line and the operator must check by hand.

### 52, 54, 87, 88, 89. Network failure

The PWA scanner now resolves API endpoints with a layered strategy:

1. Multiple LAN candidates can be configured via
   `localStorage.ep_lan_urls` (comma separated).
2. Each candidate is probed in parallel with a 1.8 s timeout; the
   first one to answer becomes `API`.
3. If none answer, fall back to the public Cloudflare tunnel.
4. If the tunnel is also unreachable, every check-in queues in
   IndexedDB and drains when any endpoint comes back.

A failed mid-session request triggers `reresolve()` automatically so
DHCP renumbering, captive portal renegotiation, and AP roams recover
without user action.

### 53, 56, 70, 71, 72. Cloudflare-vendor risks

Documented. The reference deployment pins `tabserve` as the named
tunnel; if the credentials file (`~/.cloudflared/<UUID>.json`) is lost
the tunnel can be re-created with `cloudflared tunnel create tabserve`
without changing DNS records.

For Cloudflare account suspension or pricing changes, the alternative
ingress documented in [`REVERSE_PROXY.md`](REVERSE_PROXY.md) (Nginx
in front of the tablet) is the documented fallback.

### 55, 64. DNS hijack and ARP spoofing

The `ALLOWED_HOSTS` env variable enables strict Host header checking.
Requests carrying any other Host value receive `421 Misdirected
Request`. Reference deployment uses:

```
ALLOWED_HOSTS=api.rofidoesthings.site,console.rofidoesthings.site,192.168.100.67:3001,localhost:3001
```

Combined with `CORS_ORIGINS` from R1 mitigation 5, this removes most
DNS-rebinding and ARP-spoof tampering vectors at the application
level. For complete defense the venue should use HTTPS-only Wi-Fi or
a VPN.

### 57. Domain expiry

`deploy/cert-watchdog.sh` runs daily at 05:45. The script reads
`~/.domain-expiry` (a one-line ISO date the operator must maintain)
and warns 30 days before the registered expiry. TLS expiry is
checked via `openssl s_client` if the binary is available; otherwise
the operator gets a one-line note in `~/cert.log`.

### 58. CF edge rate-limits venue NAT

Mitigated by the multi-LAN endpoint resolver in the PWA. When the
attendee is on venue Wi-Fi, the request bypasses Cloudflare entirely
— the rate limit at the edge cannot fire because no edge call is
made.

### 59. DDoS

Standard Cloudflare DDoS protection is in front of the tunnel. The
free tier handles small-scale floods (< 10 k req/s) automatically. If
sustained DDoS becomes a real threat the operator can:

1. Switch the Cloudflare zone to "Under Attack Mode" in the dashboard.
2. Enable Bot Fight Mode (free tier).
3. Add IP allow-listing for venue's public IP only.

### 60. Slow-loris

Mitigated. Each worker has a 30-second `SO_RCVTIMEO`. A connection
that does not send a complete request is closed; the worker returns
to the accept loop.

### 61. POST flood from registered device

Mitigated. New per-device rate limiter caps `quick-checkin` at
30 requests per minute per `device_uuid`. A legitimate attendee
trips this only if they scan once-per-second for half a minute,
which is operationally meaningless. Attackers running scripts trip
it within 2 seconds.

### 62. Fake QR codes

Mitigated by HMAC-signed session codes. Each `/sessions/create` and
`/sessions/:id/refresh` response now carries `signed_code` in the
form `<8-char-code>.<16-hex-tag>`. The 64-bit tag is HMAC-SHA-256
truncated of the bare code with the JWT secret. The PWA scanner
forwards `signed_code` as `session_code`; the server verifies the
signature and rejects forgeries with `401 Unauthorized`.

Plain unsigned codes are still accepted server-side for backward
compatibility with older PWA builds and the load-test harness; the
bypass is fine because the eight-character random code is
unguessable inside the 5-minute session window.

### 63. Phishing

Documented. Standard phishing hygiene: admin should ignore unsolicited
"login" emails. The dashboard URL is fixed; legitimate flows never
ask the admin to click an email link.

### 65. TLS strip

Cloudflare enforces HTTPS at the edge with HSTS. The browser warning
on `http://api.<domain>` redirects automatically. Acceptable risk.

### 66. `pkg upgrade` breaks PostgreSQL

Mitigated. Termux's `pkg upgrade` is run only manually, never by
cron. The `db-backup.sh` daily job ensures any breaking change is
recoverable. Operator should pin the PostgreSQL version in
`~/.termuxrc` if they want stricter control.

### 67. Termux app update wipes filesystem

Mitigated. The `db-backup.sh` daily job and the optional
`replicate-supabase.sh` together provide off-device data redundancy.
Binary recovery: `git clone` the repo and rebuild.

### 68, 69. Vendor binary updates

Documented. Both Node and `cloudflared` are pinned to the package
versions installed at deployment. Manual `pkg upgrade` is not part
of the runbook.

### 73. Webhook silent failure

Mitigated. After every successful deploy `~/.deploy-state` records
`<unix-epoch> <git-sha>`. The `/health/detailed` endpoint surfaces
this as `deploy.last_deploy_epoch` and `deploy.git_sha`. The
operational dashboard can show "Last deploy: 3 minutes ago" so a
stale deploy is visible at a glance.

### 75. DHCP lease

Mitigated by multi-LAN candidate probing in the PWA (see item 52).

### 76, 86. Clock drift

`deploy/time-check.sh` runs every 6 hours and compares the tablet
clock against a quorum of three independent providers (Google,
Cloudflare, Kompas). Drift > 5 s is logged as a warning. JWT
verification tolerates up to 24 h drift so this is purely an early
warning.

The PWA also reads `server_time` from `/health` on every endpoint
probe and refuses an endpoint if the local clock differs by more
than 60 s.

### 77, 78. Carrier / CDN

Documented. Cloudflare's global anycast network handles ISP-level
quirks. For Indonesian audiences the `sin22` POP is typically the
served edge.

### 79, 80. Browser policy

The Service Worker stores the queue in IndexedDB. iOS Safari ITP
clears storage after 7 days of no interaction; for a single-day
event this is irrelevant. Opportunistic 15-second drain (added in
the round-1 hardening pass) handles the absence of Background Sync
on Firefox.

### 81. Wrong session QR

Documented operational risk. The dashboard shows the active session
ID and code; the admin must verify before broadcasting. A future
enhancement would be a QR with the human-readable session name
embedded.

### 82. Password leak

Documented. Operator should not screen-share the dashboard during
demos. Future enhancement: 2FA on the admin login.

### 83. Admin account deleted

Mitigated. The `wipe-all.sql` script preserves `admin@intrivia.test`
specifically. For other accidental deletions, `db-backup.sh` rollback
covers it.

### 84. Tablet theft

Documented. Physical security is the operator's responsibility. The
tablet should be tethered or in a locked enclosure during the event.
Off-site replication (`replicate-supabase.sh`) ensures attendance
data persists even if the device is lost.

### 85. Android force-update

Documented. The operator should disable system auto-update during
event windows via Android Developer Options.

### 90. cloudflared rotation

Mitigated. cloudflared rotates connector connections every ~24
hours. The default rotation strategy keeps at least one connector
alive at all times. The PWA's smart-retry handles the < 1 s blip.

## Verification Matrix

| Capability                              | Test                                                     |
| --------------------------------------- | -------------------------------------------------------- |
| Battery watchdog logs on low battery    | Unplug charger, wait 5 min, `tail ~/battery.log`         |
| Time-drift detection                    | `bash deploy/time-check.sh`                              |
| Cert / domain expiry warning            | `bash deploy/cert-watchdog.sh`                           |
| Signed QR rejection                     | POST `quick-checkin` with `CODE.0000000000000000` → 401  |
| Per-device flood guard                  | 31 quick-checkins from one device in 60 s → 429          |
| Strict Host header                      | `curl -H 'Host: evil.example' /health` → 421             |
| LAN failover after IP change            | Change Wi-Fi network, scan QR, verify automatic switch   |
| Tunnel-only fallback                    | Disconnect tablet from Wi-Fi, scan QR → tunnel succeeds  |
| Stale deploy detection                  | `curl /health/detailed` returns `deploy.last_deploy_epoch` |

## Operational Runbook for the Event

| Time before event       | Action                                                        |
| ----------------------- | ------------------------------------------------------------- |
| T − 7 days              | Run `cert-watchdog.sh` manually; verify domain & TLS dates    |
| T − 24 hours            | `db-backup.sh` confirms backup is healthy (> 1 KB content)    |
| T − 12 hours            | Disable Android auto-update on tablet                         |
| T − 4 hours             | Charge tablet to 100 %; plug into wall outlet                 |
| T − 2 hours             | Run `chaos-checkin.js 5000` against staging, verify 100 %     |
| T − 1 hour              | Confirm `pm2 list` shows event-server + event-tunnel online   |
| T − 30 min              | Open `console.<domain>/attend/admin.html`, create test session, scan with one phone, confirm working |
| T = 0                   | Broadcast real session QR. Watch dashboard live feed.         |
| T + event duration      | Periodically refresh dashboard. Check battery dot is green.   |
| T + end                 | Export attendance CSV: `GET /attendance/session/:id/export`   |
| T + 1 hour              | Run `db-backup.sh` immediately for off-site copy              |
