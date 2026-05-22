# API Reference

Complete request and response specification for the Event Platform API.

All endpoints return JSON unless otherwise specified. All responses include
permissive CORS headers (`Access-Control-Allow-Origin: *`). Authentication
is via `Authorization: Bearer <token>` header.

**Base URL (reference deployment):** `https://api.rofidoesthings.site`

---

## Conventions

### Authentication

Bearer tokens are obtained from `/auth/login` or `/auth/register` and
expire after 24 hours.

```http
Authorization: Bearer <id>:<expiry>:<role>:<signature>
```

### Error Format

All error responses share a uniform shape:

```json
{ "error": "<human-readable message>" }
```

### Status Codes

| Code | Meaning                                              |
| ---- | ---------------------------------------------------- |
| 200  | Successful request with response body                |
| 201  | Resource created                                     |
| 204  | Successful request with no response body             |
| 400  | Invalid request (missing fields, malformed JSON)     |
| 401  | Authentication required or token invalid             |
| 403  | Authorisation denied (e.g., non-admin requesting admin endpoint) |
| 404  | Resource not found                                   |
| 409  | Conflict (e.g., duplicate email, already checked in) |
| 500  | Server error                                         |

---

## Diagnostics

### `GET /health`

Liveness probe. Returns immediately without database access.

**Response 200**
```json
{ "status": "ok", "uptime": 12345 }
```

### `GET /health/detailed`

Health probe with database connectivity verification.

**Response 200**
```json
{
  "status": "healthy",
  "checks": {
    "api":      { "status": "ok", "latency_ms": 0 },
    "database": { "status": "ok", "latency_ms": 12 }
  },
  "uptime_seconds": 12345,
  "version": "0.3.0-c",
  "node_version": "native-c"
}
```

If the database check fails, `status` becomes `"degraded"` and the
database `status` is `"error"`.

### `GET /system`

System metrics derived from `/proc/meminfo` and `/proc/loadavg`.

**Response 200**
```json
{
  "hostname": "tablet",
  "platform": "android",
  "arch": "arm64",
  "cpu": {
    "model": "Unisoc T618",
    "cores": 8,
    "speed_mhz": 0,
    "load_avg": { "1m": 0.42, "5m": 0.31, "15m": 0.27 }
  },
  "memory": {
    "total_bytes": 3145728000,
    "used_bytes": 1782656000,
    "free_bytes": 1363072000,
    "used_percent": 56.7
  },
  "uptime": {
    "system_seconds": 0,
    "process_seconds": 12345
  },
  "timestamp": "2026-05-22T08:00:00Z"
}
```

### `GET /stats/database`

Database storage statistics and per-table row counts.

**Response 200**
```json
{
  "database_size_bytes": 8054784,
  "tables": [
    { "name": "Participant", "live_tuples": 4, "dead_tuples": 0 },
    { "name": "Session",     "live_tuples": 5, "dead_tuples": 1 }
  ],
  "participant_count": 4
}
```

### `GET /stats/participants`

Aggregate participant statistics including team distribution and recent
registrations.

**Response 200**
```json
{
  "total": 4,
  "by_team": [
    { "team": "Cluster 3", "count": 2 },
    { "team": "Panitia",   "count": 1 }
  ],
  "recent": [
    {
      "id": 4,
      "name": "Rofi",
      "email": "rofi@example.com",
      "team": "Cluster 3",
      "createdAt": "2026-05-21 15:38:45"
    }
  ]
}
```

### `GET /participants`

List of all registered participants, ordered by `createdAt` descending.

**Response 200**
```json
[
  {
    "id": 4,
    "name": "Rofi",
    "email": "rofi@example.com",
    "team": "Cluster 3",
    "createdAt": "2026-05-21 15:38:45.559",
    "updatedAt": "2026-05-21 15:38:45.559"
  }
]
```

---

## Authentication

### `POST /auth/register`

Create a new participant account.

**Request Body**
```json
{
  "name": "Jane Doe",
  "email": "jane@example.com",
  "team": "Engineering",
  "password": "user-supplied-password"
}
```

**Response 201**
```json
{
  "token": "5:1779543600:participant:a1b2c3d4e5f60718",
  "participant": {
    "id": 5,
    "name": "Jane Doe",
    "email": "jane@example.com",
    "team": "Engineering",
    "role": "participant",
    "createdAt": "2026-05-22 08:00:00"
  }
}
```

**Errors**
- `400` — Missing required fields
- `409` — Email already registered

### `POST /auth/login`

Exchange credentials for an authentication token.

**Request Body**
```json
{ "email": "jane@example.com", "password": "user-supplied-password" }
```

**Response 200**
Same shape as `/auth/register`.

**Errors**
- `400` — Missing fields
- `401` — Invalid credentials

### `GET /auth/me`

Return the currently authenticated user.

**Headers**
```
Authorization: Bearer <token>
```

**Response 200**
```json
{
  "id": 5,
  "name": "Jane Doe",
  "email": "jane@example.com",
  "team": "Engineering",
  "role": "participant",
  "createdAt": "2026-05-22 08:00:00"
}
```

**Errors**
- `401` — Missing or expired token
- `404` — User no longer exists

---

## Sessions (Administrator Only)

All session endpoints require a token with `role = "admin"`. Non-admin
tokens receive a `403 Forbidden` response.

### `POST /sessions/create`

Create a new ad-hoc attendance session. The session is active immediately and
expires 30 minutes later (configurable at compile time).

**Response 201**
```json
{
  "id": 12,
  "code": "aB3xY7zQ",
  "expires_at": "2026-05-22 08:30:00"
}
```

### `POST /sessions/scheduled`

Create a named, scheduled attendance session with explicit time window.
Useful for pre-published event programmes.

**Request Body**
```json
{
  "title":       "Day 1 Morning Plenary",
  "description": "Opening keynote",
  "starts_at":   "2026-05-22 09:00:00",
  "ends_at":     "2026-05-22 12:00:00"
}
```

**Response 201**
```json
{
  "id":         15,
  "code":       "rtxRqDN8",
  "title":      "Day 1 Morning Plenary",
  "starts_at":  "2026-05-22 09:00:00",
  "ends_at":    "2026-05-22 12:00:00"
}
```

**Errors**
- `400` — Missing required fields
- `403` — Not an administrator

### `GET /sessions/active`

List all sessions that are currently active and unexpired.

**Response 200**
```json
[
  {
    "id": 12,
    "code": "aB3xY7zQ",
    "created_by": 1,
    "expires_at": "2026-05-22 08:30:00",
    "createdAt":  "2026-05-22 08:00:00"
  }
]
```

### `GET /sessions/:id`

Detail view including attendance count.

**Response 200**
```json
{
  "id": 12,
  "code": "aB3xY7zQ",
  "created_by": 1,
  "expires_at": "2026-05-22 08:30:00",
  "active": true,
  "createdAt": "2026-05-22 08:00:00",
  "attendance_count": 47
}
```

### `POST /sessions/:id/refresh`

Rotate the session code (typically called every 30 seconds by the admin
QR display) and extend `expires_at` by 30 seconds.

**Response 200**
```json
{
  "id": 12,
  "code": "x9Q2pL8m",
  "expires_at": "2026-05-22 08:30:30"
}
```

---

## Attendance

### `POST /attendance/checkin`

Record a check-in. The session must be active and unexpired. The same
participant may not check in twice to the same session (uniqueness
constraint).

**Request Body**
```json
{
  "session_code": "aB3xY7zQ",
  "device_id":    "dev-abc123-uuid"
}
```

**Response 201**
```json
{
  "id": 89,
  "participant_id": 5,
  "session_id": 12,
  "checkedInAt": "2026-05-22 08:05:23.123"
}
```

**Errors**
- `401` — Missing or expired token
- `404` — Session code not found, inactive, or expired
- `409` — Already checked in to this session

### `POST /attendance/quick-checkin`

High-throughput, device-keyed check-in path. Designed as the primary
endpoint for large-scale simultaneous attendance ingestion (the project's
reference target is 2000 concurrent attendees).

Unlike `POST /attendance/checkin`, this endpoint does **not** require a
bearer token. The participant identity is resolved from the
`device_uuid`, which must have been previously linked via
`POST /device/link`. The full operation — device lookup, session lookup,
and attendance insertion — is executed as a single SQL statement (CTE)
to minimise round trips and lock window.

This endpoint is **excluded from rate limiting** to support large
shared-NAT scenarios where every attendee egresses from the same venue
WiFi public IP.

**Request Body**
```json
{
  "session_code": "aB3xY7zQ",
  "device_uuid":  "dev-abc123-uuid"
}
```

**Response 201**
```json
{
  "id": 89,
  "participant_id": 5,
  "session_id": 12,
  "checkedInAt": "2026-05-22 08:05:23.123"
}
```

**Errors**
- `400` — Missing `session_code` or `device_uuid`
- `401` — Device UUID not linked to any participant
- `404` — Session code not found, inactive, or expired
- `409` — Already checked in to this session

### `POST /attendance/batch-checkin`

Bulk drain endpoint used by the scanner Progressive Web Application
when reconciling its offline queue. Accepts up to several hundred
device UUIDs in a single request; the server commits all rows in a
single database transaction.

**Request Body**
```json
{
  "session_code": "aB3xY7zQ",
  "items": [
    { "device_uuid": "dev-aaa-uuid" },
    { "device_uuid": "dev-bbb-uuid" },
    { "device_uuid": "dev-ccc-uuid" }
  ]
}
```

**Response 200**
```json
{
  "accepted":   2,
  "duplicates": 1,
  "unknown":    0
}
```

The three counters partition the input set:
- `accepted` — newly inserted attendance rows.
- `duplicates` — devices that were already checked in for this session.
- `unknown` — devices whose UUID is not registered.

**Errors**
- `400` — Missing `session_code` or malformed `items`
- `404` — Session code not found, inactive, or expired

### `GET /attendance/session/:id`

List all check-ins for a specific session, joined with participant
details.

**Response 200**
```json
[
  {
    "id": 89,
    "participant_id": 5,
    "name": "Jane Doe",
    "email": "jane@example.com",
    "team": "Engineering",
    "device_id": "dev-abc123-uuid",
    "checkedInAt": "2026-05-22 08:05:23.123"
  }
]
```

### `GET /attendance/me`

The authenticated user's check-in history.

**Response 200**
```json
[
  {
    "id": 89,
    "session_id": 12,
    "session_code": "aB3xY7zQ",
    "device_id": "dev-abc123-uuid",
    "checkedInAt": "2026-05-22 08:05:23.123"
  }
]
```

### `GET /attendance/session/:id/export`

Download attendance data for a session as a CSV file. **Administrator
access required.**

**Headers**
```
Authorization: Bearer <admin-token>
```

**Response 200**
- `Content-Type: text/csv; charset=utf-8`
- `Content-Disposition: attachment; filename="attendance-session-<id>.csv"`

**CSV Format**
```csv
id,name,email,team,device_id,checked_in_at
89,"Jane Doe","jane@example.com","Engineering","dev-abc","2026-05-22 08:05:23"
```

### `GET /ws/attendance/:id?token=<token>`

WebSocket endpoint for streaming live check-in events. **Administrator
access required.** The token is passed as a query parameter because
browser WebSocket clients cannot supply arbitrary headers.

**Upgrade Handshake**

Standard RFC 6455 handshake. Server responds `101 Switching Protocols`
with `Sec-WebSocket-Accept` header.

**Server Frames**

The server pushes JSON-encoded text frames for three event types:

```json
{ "event": "connected",  "session_id": 12, "timestamp": "1779543600" }
{ "event": "checkin",    "id": 89, "participant_id": 5,
  "name": "Jane Doe", "email": "jane@example.com", "team": "Engineering",
  "device_id": "dev-abc", "checkedInAt": "2026-05-22 08:05:23.123" }
{ "event": "heartbeat" }
```

- `connected` is sent immediately after the handshake.
- `checkin` is sent each time a new attendance row is inserted.
- `heartbeat` is sent every 30 seconds when no other traffic occurs,
  allowing clients to detect dead connections.

**Client Behaviour**

The reference admin frontend (`/attend/admin.html`) opens the WebSocket
when a session is started and replaces what was previously a 2-second
polling loop. On disconnect, the client auto-reconnects after 3 seconds.

**Connection Lifetime**

The server closes the connection after 600 seconds of idle time
(no new check-ins). Clients should reconnect on close.

---

## Bulk Operations (Administrator Only)

### `POST /participants/bulk`

Bulk-create participants from a CSV body. Each line represents a
participant. Errors are accumulated and returned but do not abort the
batch.

**Request**
```http
POST /participants/bulk
Authorization: Bearer <admin-token>
Content-Type: text/csv

name,email,team[,password]
Alice,alice@example.com,TeamA
Bob,bob@example.com,TeamA,strong-password
```

The `password` column is optional. When omitted, a default placeholder
password is set; participants must reset it on first login.

**Response 200**
```json
{
  "created": 2,
  "skipped": 1,
  "errors":  [
    { "line": 4, "email": "alice@example.com", "error": "duplicate email" }
  ]
}
```

**Limits**
- Each line must be ≤ 511 bytes
- Total request body must fit in the server read buffer (currently 64 KB)
- For larger imports, split into multiple requests

---

## Device Identity

### `POST /device/link`

Bind a device UUID to the authenticated user. Idempotent: if the UUID
already exists, the binding is updated (e.g., user reset their app on
the same device).

**Request Body**
```json
{
  "device_uuid": "dev-abc123-uuid",
  "user_agent":  "Mozilla/5.0 (Linux; Android 13)..."
}
```

**Response 200**
```json
{
  "id": 17,
  "device_uuid": "dev-abc123-uuid",
  "participant_id": 5,
  "linkedAt": "2026-05-22 08:00:00"
}
```

### `POST /device/identify`

Resolve a device UUID to its bound participant. **No authentication
required** — this is the basis for the QR scan flow's instant
identification.

**Request Body**
```json
{ "device_uuid": "dev-abc123-uuid" }
```

**Response 200**
```json
{
  "id": 5,
  "name": "Jane Doe",
  "email": "jane@example.com",
  "team": "Engineering",
  "role": "participant",
  "linkedAt": "2026-05-22 08:00:00"
}
```

**Errors**
- `404` — Device UUID is not linked to any participant

---

## Deployment Webhooks

All webhook endpoints require `?secret=<webhook-secret>` as a query
parameter. The secret is configured via the `WEBHOOK_SECRET` environment
variable on the server.

The receiving handler immediately responds `202 Accepted` and forks a
detached child to execute the script. The HTTP response does not reflect
the script's outcome; check `~/deploy-api.log` or the status webhook
for results.

### `POST /deploy/api`

Trigger redeploy of the API server.

### `POST /deploy/console`

Trigger redeploy of the frontend (git pull only; no build step).

### `POST /deploy/run/:script`

Execute a script from the operational allowlist. Permitted values for
`:script`:

| Name             | Purpose                                            |
| ---------------- | -------------------------------------------------- |
| `setup-cron`     | Install the cron job catalogue.                    |
| `install-boot`   | Install the Termux:Boot autostart script.          |
| `db-backup`      | Run a database backup immediately.                 |
| `status-check`   | Generate a brief status JSON.                      |
| `full-status`    | Generate a comprehensive status JSON.              |
| `rollback`       | Restore the most recent archived binary.           |

**Response 202**
```json
{ "status": "deploy triggered", "target": "api", "pid": 12345 }
```

**Errors**
- `400` — Missing or invalid `:script` name
- `401` — Missing or invalid secret
- `403` — Script name is not in the allowlist
- `404` — Unknown deploy target
