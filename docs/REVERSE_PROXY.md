# Alternative Deployment: Nginx Reverse Proxy

This document describes an alternative production topology using Nginx
as a reverse proxy in front of the Event Platform API. The reference
deployment uses Cloudflare Tunnel, which is suitable for hosts without
public IPs. When you have a static public IP and prefer to manage
ingress yourself, Nginx provides finer control over TLS termination,
caching, and rate limiting at the edge.

This is **not the recommended setup** for the reference tablet
deployment. Use this guide if you are deploying on a VPS, dedicated
server, or any host with direct internet exposure.

---

## Comparison

| Aspect              | Cloudflare Tunnel (default) | Nginx Reverse Proxy        |
| ------------------- | --------------------------- | -------------------------- |
| Public IP required  | No                          | Yes                        |
| TLS termination     | Cloudflare edge             | Local (Let's Encrypt)      |
| DDoS protection     | Included                    | Operator's responsibility  |
| WebSocket support   | Yes (transparent)           | Yes (manual upgrade pass)  |
| Concurrency limit   | Tunnel-bound (free tier)    | Bound only by host         |
| Operational cost    | Free                        | Free, plus IP/server cost  |

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                Internet                          │
└────────────────────┬────────────────────────────┘
                     │
                     ▼  TCP 443 (HTTPS)
┌─────────────────────────────────────────────────┐
│                  Nginx                           │
│  - TLS termination (Let's Encrypt)              │
│  - HTTP/2 + WebSocket upgrade                   │
│  - Optional: rate limiting, gzip, access log    │
└────────────────────┬────────────────────────────┘
                     │
                     ▼  proxy_pass http://127.0.0.1:3001
┌─────────────────────────────────────────────────┐
│              event-server (C)                    │
│            listening on localhost:3001           │
└─────────────────────────────────────────────────┘
```

---

## Installation (Debian/Ubuntu)

### 1. Install Nginx and Certbot

```bash
sudo apt-get update
sudo apt-get install -y nginx certbot python3-certbot-nginx
```

### 2. Configure Nginx

Create `/etc/nginx/sites-available/event-platform.conf`:

```nginx
# HTTP — redirect to HTTPS only
server {
    listen 80;
    listen [::]:80;
    server_name api.example.com console.example.com;

    location /.well-known/acme-challenge/ {
        root /var/www/html;
    }

    location / {
        return 301 https://$host$request_uri;
    }
}

# HTTPS — main proxy
server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name api.example.com console.example.com;

    # TLS — managed by certbot, edit if customising
    ssl_certificate     /etc/letsencrypt/live/api.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/api.example.com/privkey.pem;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         HIGH:!aNULL:!MD5;

    # Rate limiting — applies before reaching the C server
    limit_req_zone $binary_remote_addr zone=api_general:10m rate=10r/s;
    limit_req_zone $binary_remote_addr zone=api_auth:10m    rate=2r/s;

    # Auth endpoints — strict rate limit
    location ~ ^/auth/ {
        limit_req zone=api_auth burst=5 nodelay;
        proxy_pass http://127.0.0.1:3001;
        proxy_set_header Host              $host;
        proxy_set_header X-Real-IP         $remote_addr;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header CF-Connecting-IP  $remote_addr;
    }

    # WebSocket — explicit upgrade headers required
    location /ws/ {
        proxy_pass         http://127.0.0.1:3001;
        proxy_http_version 1.1;
        proxy_set_header   Upgrade           $http_upgrade;
        proxy_set_header   Connection        "upgrade";
        proxy_set_header   Host              $host;
        proxy_set_header   X-Real-IP         $remote_addr;
        proxy_set_header   CF-Connecting-IP  $remote_addr;
        proxy_read_timeout 600s;       # WebSocket lifetime up to 10 min
        proxy_send_timeout 600s;
    }

    # Everything else
    location / {
        limit_req zone=api_general burst=20 nodelay;
        proxy_pass http://127.0.0.1:3001;
        proxy_set_header Host              $host;
        proxy_set_header X-Real-IP         $remote_addr;
        proxy_set_header X-Forwarded-For   $proxy_add_x_forwarded_for;
        proxy_set_header CF-Connecting-IP  $remote_addr;
    }

    # Optional: gzip static assets
    gzip          on;
    gzip_vary     on;
    gzip_types    text/css application/javascript text/html;
    gzip_min_length 1024;

    # Optional: cache static assets aggressively
    location ~* \.(js|css|svg|png|ico|woff2)$ {
        proxy_pass        http://127.0.0.1:3001;
        proxy_cache_valid 200 1h;
        add_header        Cache-Control "public, max-age=3600";
    }

    access_log /var/log/nginx/event-platform.access.log;
    error_log  /var/log/nginx/event-platform.error.log;
}
```

Enable and reload:

```bash
sudo ln -s /etc/nginx/sites-available/event-platform.conf /etc/nginx/sites-enabled/
sudo nginx -t                  # syntax check
sudo systemctl reload nginx
```

### 3. Provision TLS certificates

```bash
sudo certbot --nginx -d api.example.com -d console.example.com
```

Certbot installs an auto-renewal timer; certificates renew automatically
every 60-90 days.

### 4. Reconfigure the C server to bind localhost only

In your pm2 environment, set the server to bind to 127.0.0.1 instead of
0.0.0.0 (security: prevents direct external access bypassing Nginx):

This requires a small change to `server.c`:

```diff
-    addr.sin_addr.s_addr = INADDR_ANY;
+    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
```

Or add an environment variable read for `BIND_ADDR` before recompiling.

### 5. Verify

```bash
curl https://api.example.com/health                         # HTTPS
curl https://api.example.com/system | jq .platform          # System info
wscat -c wss://api.example.com/ws/attendance/1?token=...    # WebSocket
```

---

## Operational Concerns

### Logs

Nginx access logs include client IPs and response codes. Forward to
your log aggregation system (Loki, Elasticsearch) for analytics:

```bash
sudo tail -f /var/log/nginx/event-platform.access.log
```

### Cache invalidation

If you cache static assets, ensure the deploy script invalidates the
cache after frontend changes:

```bash
# Add to deploy.sh on the server:
sudo nginx -s reload
```

For aggressive caching, version your asset filenames (e.g.,
`app.v2.js`) so URLs change on deploy and cache is naturally bypassed.

### Rate limiting tiers

The example configuration enforces:

- 2 requests/second on `/auth/*` (burst 5)
- 10 requests/second on everything else (burst 20)

The C server's in-process rate limiter remains active, providing a
second line of defence and per-IP fairness.

### Removing Cloudflare Tunnel

If migrating from the reference deployment to Nginx:

1. Stop and disable `cloudflared`:
   ```bash
   pm2 stop event-tunnel
   pm2 delete event-tunnel
   pm2 save
   ```
2. Update DNS records to point at the host's public IP
   (replace the `*.cfargotunnel.com` CNAME with an `A` record).
3. Remove the tunnel routes via the Cloudflare dashboard or
   `cloudflared tunnel route dns` (no `--delete` flag exists; use the
   API or web UI).

---

## When to Use Which

Use **Cloudflare Tunnel** when:

- Hosting from behind NAT (home WiFi, mobile network, tablet)
- You want managed DDoS protection without configuration
- Reducing operational complexity is a goal

Use **Nginx Reverse Proxy** when:

- You have a VPS or dedicated server with public IP
- You need fine-grained control over TLS, caching, rate limiting
- Higher sustained throughput is required (no tunnel concurrency limit)
- You want to avoid third-party dependencies for ingress
