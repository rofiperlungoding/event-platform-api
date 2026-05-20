# Event Platform API

Latihan backend + DevOps. TypeScript · Fastify · Prisma · PostgreSQL · Docker.

## Stack

- **Runtime:** Node.js 20+
- **Framework:** Fastify 5
- **ORM:** Prisma 6
- **Database:** PostgreSQL 16
- **Validation:** Zod
- **Container:** Docker Compose

## Setup di Laptop (development)

```bash
# 1. Install dependencies
npm install

# 2. Generate Prisma client
npx prisma generate

# 3. Start database (docker)
npm run db:up

# 4. Apply schema ke database (bikin tabel)
npm run prisma:migrate

# 5. Run dev server (auto-reload)
npm run dev
```

Server jalan di `http://localhost:3000`.

## Endpoints

| Method | Path                  | Body                              | Response       |
| ------ | --------------------- | --------------------------------- | -------------- |
| GET    | `/health`             | -                                 | `{status, uptime}` |
| GET    | `/participants`       | -                                 | array          |
| GET    | `/participants/:id`   | -                                 | object \| 404  |
| POST   | `/register`           | `{name, email, team}`             | 201 \| 409     |
| DELETE | `/participants/:id`   | -                                 | 204 \| 404     |

### Test cepat

```bash
# Daftar peserta
curl -X POST http://localhost:3000/register \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"Rofi\",\"email\":\"rofi@test.com\",\"team\":\"Alpha\"}"

# Liat semua
curl http://localhost:3000/participants
```

## Deploy ke Tablet (production)

Di **laptop** — push code:
```bash
git push origin main
```

Di **tablet** (Termux + proot Ubuntu):
```bash
git clone <repo-url>
cd event-platform-api

# Copy & isi password kuat
cp .env.prod.example .env.prod
nano .env.prod

# Build & run full stack
docker compose -f docker-compose.prod.yml --env-file .env.prod up -d --build

# Lihat logs
docker compose -f docker-compose.prod.yml logs -f app
```

Expose ke internet via Cloudflare Tunnel:
```bash
cloudflared tunnel --url http://localhost:3000
```

## Workflow Harian

```
Edit di laptop → git commit → git push
                                  ↓
                          (CI jalan, type check)
                                  ↓
              SSH ke tablet → git pull → docker compose up -d --build
```

## Struktur

```
event-platform-api/
├── prisma/
│   └── schema.prisma          # Database schema
├── src/
│   └── index.ts               # API entry point
├── .github/workflows/ci.yml   # GitHub Actions
├── docker-compose.yml         # Dev (db only)
├── docker-compose.prod.yml    # Prod (db + app)
├── Dockerfile                 # Image untuk app
└── package.json
```
