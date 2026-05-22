# Event Platform — Console

The web frontend for the platform: an administrator dashboard for
operational telemetry, an attendance QR generator for organisers, and a
Progressive Web Application (PWA) for participant check-in.

This folder contains static assets only — no build step, no
dependencies, no transpilation. The assets are served directly by the
`event-server` C binary, which co-hosts both API endpoints and frontend
files. Unmatched paths fall through to static-file resolution from
`STATIC_DIR`, which points at this folder.

## Components

### Console Dashboard (`index.html`)

An AWS-style operational dashboard that polls the API every 8 seconds
to display:

- API health and database latency
- System metrics (CPU, memory, load average, uptime)
- Database statistics (size, table row counts)
- Participant statistics (total, by team, recent registrations)
- Full participant list with delete capability

Implemented in vanilla HTML, CSS, and JavaScript. No framework, no
build tooling. Total payload below 25 KB.

### Attendance Admin Page (`attend/admin.html`)

The QR code generator used by event organisers. Features:

- Login as administrator
- Display a session QR code that rotates every 30 seconds
- Live feed of check-ins via WebSocket
- Pre-loaded next QR code for instant rotation
- Stop session control

QR generation uses the `qrcode-generator` library served from CDN.

### Check-In PWA (`attend/scan.html`)

The participant-facing scanner. Features:

- Service Worker for offline asset caching
- IndexedDB-backed check-in queue
- Background Sync API for automatic queue drainage
- Device UUID generation and binding
- Online/offline state detection
- Visual indicators for synced versus pending entries
- LAN-first endpoint resolver — bypasses Cloudflare Tunnel when the
  attendee is on the same Wi-Fi as the tablet

Camera scanning uses the `html5-qrcode` library.

PWA manifest is at `attend/manifest.json`. Service Worker is at
`attend/sw.js`. Icons are in `attend/`.

## Configuration

API base URLs are defined per file:

```javascript
// app.js  (admin dashboard, no LAN failover)
const API = 'https://api.rofidoesthings.site';

// attend/admin.html  (organiser QR generator)
const API = 'https://api.rofidoesthings.site';

// attend/scan.html  (participant PWA — auto-resolves LAN first)
const PUBLIC_API     = 'https://api.rofidoesthings.site';
const DEFAULT_LAN_API = 'http://192.168.100.67:3001';
```

The scanner PWA probes the LAN endpoint with a 1.5 s timeout on page
load. If reachable, all subsequent requests bypass the Cloudflare
Tunnel — this is what makes 2,000 simultaneous check-ins return in
under two seconds (the tunnel free tier caps at ~45 req/s on its own).

Operators can override the LAN URL with `?lan=http://...` query
parameter or `localStorage.ep_lan_url`. When forking for a new
deployment, update both constants in each file.

## Deployment

This folder is part of the unified `event-platform-api` repository. A
push to `main` triggers a single deploy that recompiles the server and
syncs the static assets in one operation. There is no separate console
deploy.

## Local Preview

```bash
npx serve .
```

Service Workers require HTTPS or `localhost`; opening files via
`file://` will not register the worker.

## File Layout

```
console/
├── index.html              # Operational dashboard
├── styles.css              # AWS-style theme
├── app.js                  # Dashboard polling logic
├── attend/
│   ├── admin.html          # QR generator (admin)
│   ├── scan.html           # QR scanner (PWA, participant)
│   ├── sw.js               # Service Worker
│   ├── manifest.json       # PWA manifest
│   ├── icon-192.svg        # App icon (small)
│   └── icon-512.svg        # App icon (large)
└── README.md               # This file
```

## Browser Support

| Feature              | Required Support                                      |
| -------------------- | ----------------------------------------------------- |
| Fetch API            | All evergreen browsers                                |
| Service Workers      | Chrome ≥ 40, Firefox ≥ 44, Safari ≥ 11.1, Edge ≥ 17   |
| Background Sync API  | Chrome ≥ 49 (Firefox falls back to retry-on-online)   |
| IndexedDB            | All evergreen browsers                                |
| `crypto.randomUUID`  | Chrome ≥ 92, Firefox ≥ 95, Safari ≥ 15.4              |

The PWA gracefully degrades on browsers without Background Sync: it
relies on the `online` event listener to trigger queue drainage.

## Security Notes

- API base URL is publicly visible in source — the API enforces
  authentication and CORS at the server.
- Auth tokens are stored in `localStorage`, which is acceptable for a
  PWA used inside trusted-network events. Production deployments
  facing the open internet should consider httpOnly cookies + CSRF
  tokens.
- The Service Worker scope is `/attend/` only; it does not intercept
  console dashboard requests.
