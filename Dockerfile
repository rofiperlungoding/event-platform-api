# Multi-stage build: compile in builder, ship minimal runtime
# Build:  docker build -t event-platform-api .
# Run:    docker compose up

FROM debian:12-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        libpq-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY server.c .
RUN cc -O2 -Wall -o event-server server.c -lpq && strip event-server

# ─── Runtime stage ────────────────────────────────────────────────────────
FROM debian:12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libpq5 \
        ca-certificates \
        curl \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r app && useradd -r -g app app

COPY --from=builder /src/event-server /usr/local/bin/event-server

# Static assets directory — bind-mount the console repo here at runtime
RUN mkdir -p /var/lib/event-platform/static \
    && chown -R app:app /var/lib/event-platform

USER app
WORKDIR /home/app

ENV PORT=3000 \
    STATIC_DIR=/var/lib/event-platform/static

EXPOSE 3000

HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD curl -fsS http://localhost:${PORT}/health > /dev/null || exit 1

ENTRYPOINT ["event-server"]
