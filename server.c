/* server.c — Event Platform API Server
 *
 * A single-binary HTTP server providing REST API endpoints, static file
 * hosting, and webhook-based deployment automation for an event registration
 * and attendance platform.
 *
 * BUILD:
 *   cc -O2 -Wall -o event-server server.c -lpq
 *
 * ENVIRONMENT VARIABLES:
 *   PORT             Listen port (default: 3000)
 *   DATABASE_URL     PostgreSQL connection string (default: localhost dev)
 *   STATIC_DIR       Directory to serve static files from
 *   JWT_SECRET       Token signing secret (default: insecure dev value)
 *   WEBHOOK_SECRET   Deploy webhook authentication secret
 *
 * ARCHITECTURE:
 *   - Single-threaded blocking I/O (one request at a time)
 *   - Per-request PostgreSQL connection (no connection pool)
 *   - Suitable for low-to-medium throughput when paired with offline-first
 *     client (PWA + IndexedDB queue + Background Sync)
 *
 * SECURITY:
 *   - SO_LINGER=0 forces RST on close, prevents TIME_WAIT port lock
 *   - Webhook endpoints gated by shared secret query parameter
 *   - Token auth uses FNV-1a non-cryptographic hash (NOT for production)
 *   - Passwords stored in plaintext (NOT for production)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libpq-fe.h>
#include <errno.h>
#include <fcntl.h>

#define BUF_SIZE 65536
#define MAX_PATH 1024
#define MAX_BODY 8192
#define TOKEN_EXPIRY 86400  /* 24 hours */
#define SESSION_EXPIRY 300  /* 5 minutes */
#define SESSION_REFRESH 30  /* 30 seconds extension */
#define CODE_LEN 8

static time_t start_time;
static char static_dir[MAX_PATH];
static char db_url[1024];
static char jwt_secret[256];

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static const char *cors_headers =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, int body_len) {
    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "%s"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, cors_headers, content_type, body_len);
    write(fd, hdr, hlen);
    if (body_len > 0) write(fd, body, body_len);
}

static void send_json(int fd, int status, const char *status_text, const char *json) {
    send_response(fd, status, status_text, "application/json", json, strlen(json));
}

static void send_no_content(int fd) {
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 204 No Content\r\n%sConnection: close\r\n\r\n", cors_headers);
    write(fd, hdr, hlen);
}

static const char *mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static PGconn *db_connect(void) {
    PGconn *conn = PQconnectdb(db_url);
    if (PQstatus(conn) != CONNECTION_OK) {
        fprintf(stderr, "DB connect failed: %s\n", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

/* ─── Per-worker persistent connection ─────────────────────────────────
 * Each worker process keeps a single PGconn open across requests. This
 * eliminates the ~50ms TCP/handshake cost on the hot path, taking
 * /attendance/quick-checkin from ~75ms down to ~10ms.
 *
 * Workflow:
 *   - First call to db_acquire() creates and caches the connection.
 *   - Subsequent calls return the cached connection if it is healthy.
 *   - db_release() is called at the end of a handler; for the cached
 *     connection it is a no-op, otherwise it closes a transient one.
 *   - If the cached connection has gone bad (server restart, network
 *     blip), it is closed and reopened transparently.
 *
 * Each worker is single-threaded (synchronous accept loop), so no mutex
 * is required — the connection is exclusively owned by one worker
 * process. */
static PGconn *worker_conn = NULL;

static PGconn *db_acquire(void) {
    if (worker_conn) {
        if (PQstatus(worker_conn) == CONNECTION_OK) return worker_conn;
        /* Stale — drop it and reconnect. */
        PQfinish(worker_conn);
        worker_conn = NULL;
    }
    worker_conn = db_connect();
    return worker_conn;
}

/* Replacement for PQfinish in callers — leaves the cached connection open. */
static void db_release(PGconn *conn) {
    if (conn && conn != worker_conn) PQfinish(conn);
}

static long ms_since(struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000;
}

/* JSON-escape a string into dst, return bytes written */
static int json_escape(char *dst, int max, const char *src) {
    int i = 0;
    for (; *src && i < max - 2; src++) {
        if (*src == '"' || *src == '\\') { dst[i++] = '\\'; dst[i++] = *src; }
        else if (*src == '\n') { dst[i++] = '\\'; dst[i++] = 'n'; }
        else if (*src == '\r') { dst[i++] = '\\'; dst[i++] = 'r'; }
        else if (*src == '\t') { dst[i++] = '\\'; dst[i++] = 't'; }
        else dst[i++] = *src;
    }
    dst[i] = 0;
    return i;
}

/* ─── Token / Auth Helpers ────────────────────────────────────────────── */

/* ─── SHA-1 (RFC 3174) — for WebSocket handshake ─────────────────────── */
typedef struct { uint32_t state[5]; uint64_t bytes; uint8_t buf[64]; int idx; } sha1_ctx;

static uint32_t sha1_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(sha1_ctx *c, const uint8_t *block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (block[i*4] << 24) | (block[i*4+1] << 16) | (block[i*4+2] << 8) | block[i*4+3];
    }
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3], e = c->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;          k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;          k = 0xCA62C1D6; }
        uint32_t t = sha1_rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = sha1_rol(b, 30); b = a; a = t;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d; c->state[4] += e;
}

static void sha1_init(sha1_ctx *c) {
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89;
    c->state[2] = 0x98BADCFE; c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
    c->bytes = 0; c->idx = 0;
}

static void sha1_update(sha1_ctx *c, const uint8_t *data, int len) {
    c->bytes += len;
    while (len > 0) {
        int take = 64 - c->idx;
        if (take > len) take = len;
        memcpy(c->buf + c->idx, data, take);
        c->idx += take; data += take; len -= take;
        if (c->idx == 64) { sha1_block(c, c->buf); c->idx = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->bytes * 8;
    c->buf[c->idx++] = 0x80;
    if (c->idx > 56) {
        while (c->idx < 64) c->buf[c->idx++] = 0;
        sha1_block(c, c->buf); c->idx = 0;
    }
    while (c->idx < 56) c->buf[c->idx++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->idx++] = (bits >> (i * 8)) & 0xff;
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (c->state[i] >> 24) & 0xff;
        out[i*4+1] = (c->state[i] >> 16) & 0xff;
        out[i*4+2] = (c->state[i] >> 8) & 0xff;
        out[i*4+3] =  c->state[i] & 0xff;
    }
}

/* ─── Base64 encoder ──────────────────────────────────────────────────── */
static void base64_encode(const uint8_t *in, int len, char *out) {
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = alpha[(in[i] >> 2) & 0x3f];
        out[j++] = alpha[((in[i] << 4) | (in[i+1] >> 4)) & 0x3f];
        out[j++] = alpha[((in[i+1] << 2) | (in[i+2] >> 6)) & 0x3f];
        out[j++] = alpha[in[i+2] & 0x3f];
    }
    if (i < len) {
        out[j++] = alpha[(in[i] >> 2) & 0x3f];
        if (i + 1 < len) {
            out[j++] = alpha[((in[i] << 4) | (in[i+1] >> 4)) & 0x3f];
            out[j++] = alpha[(in[i+1] << 2) & 0x3f];
            out[j++] = '=';
        } else {
            out[j++] = alpha[(in[i] << 4) & 0x3f];
            out[j++] = '=';
            out[j++] = '=';
        }
    }
    out[j] = 0;
}


/* ─── Rate Limiter (per-IP, in-memory token bucket) ─────────────────────
 * Simple fixed-window: each IP gets N tokens per window.
 * Limits chosen for ~50 active users/IP — admin operations, scanner PWA.
 * Bucket array is fixed size (256 entries, hash collision = LRU evict). */
#define RL_BUCKETS 256
#define RL_WINDOW_SEC 60
#define RL_DEFAULT_LIMIT 600       /* most endpoints — generous for shared NAT */
#define RL_AUTH_LIMIT 10           /* auth endpoints (anti brute force) */

typedef struct {
    uint32_t ip;
    time_t   window_start;
    int      count;
} rl_entry;

static rl_entry rl_table[RL_BUCKETS];

/* Returns 1 if allowed, 0 if rate-limited */
static int rate_limit_check(uint32_t ip, int limit) {
    int slot = ip % RL_BUCKETS;
    time_t now = time(NULL);
    rl_entry *e = &rl_table[slot];

    if (e->ip != ip || (now - e->window_start) >= RL_WINDOW_SEC) {
        e->ip = ip;
        e->window_start = now;
        e->count = 1;
        return 1;
    }
    e->count++;
    return e->count <= limit;
}

/* Send 429 with Retry-After header */
static void send_rate_limited(int fd) {
    const char *body = "{\"error\":\"rate limit exceeded, retry shortly\"}";
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 429 Too Many Requests\r\n"
        "%s"
        "Content-Type: application/json\r\n"
        "Retry-After: 60\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        cors_headers, strlen(body));
    write(fd, hdr, hlen);
    write(fd, body, strlen(body));
}


/* Growable string buffer — prevents overflow on large result sets */
typedef struct { char *data; int len; int cap; } strbuf;
static void sb_init(strbuf *sb, int cap) { sb->data = malloc(cap); sb->len = 0; sb->cap = cap; sb->data[0] = 0; }
static void sb_ensure(strbuf *sb, int extra) { while (sb->len + extra >= sb->cap) { sb->cap *= 2; sb->data = realloc(sb->data, sb->cap); } }
static void sb_append(strbuf *sb, const char *s) { int l = strlen(s); sb_ensure(sb, l+1); memcpy(sb->data+sb->len, s, l); sb->len += l; sb->data[sb->len] = 0; }
static void sb_appendf(strbuf *sb, const char *fmt, ...) {
    va_list ap, ap2;
    sb_ensure(sb, 512);
    va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n >= sb->cap - sb->len) { sb_ensure(sb, n + 1); n = vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap2); }
    va_end(ap2);
    if (n > 0) sb->len += n;
}
static void sb_free(strbuf *sb) { free(sb->data); sb->data = NULL; sb->len = sb->cap = 0; }

/* FNV-1a hash (64-bit) - simple, fast, non-cryptographic */
static unsigned long long fnv1a_hash(const char *data, int len) {
    unsigned long long hash = 14695981039346656037ULL;
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Generate token: "id:expiry:hex_signature" */
static void generate_token(char *out, int out_sz, int user_id, const char *role) {
    time_t expiry = time(NULL) + TOKEN_EXPIRY;
    char payload[256];
    int plen = snprintf(payload, sizeof(payload), "%d:%ld:%s", user_id, (long)expiry, jwt_secret);
    unsigned long long sig = fnv1a_hash(payload, plen);
    snprintf(out, out_sz, "%d:%ld:%s:%016llx", user_id, (long)expiry, role, sig);
}

/* Verify token, returns participant_id or -1 on failure.
 * Also sets *out_role to "admin" or "participant" if non-NULL */
static int verify_token(const char *token, char *out_role) {
    int user_id;
    long expiry;
    char role[32];
    unsigned long long provided_sig;

    if (sscanf(token, "%d:%ld:%31[^:]:%llx", &user_id, &expiry, role, &provided_sig) != 4)
        return -1;

    /* Check expiry */
    if ((time_t)expiry < time(NULL))
        return -1;

    /* Recompute signature */
    char payload[256];
    int plen = snprintf(payload, sizeof(payload), "%d:%ld:%s", user_id, expiry, jwt_secret);
    unsigned long long expected_sig = fnv1a_hash(payload, plen);

    if (provided_sig != expected_sig)
        return -1;

    if (out_role) {
        strncpy(out_role, role, 31);
        out_role[31] = 0;
    }
    return user_id;
}

/* Extract Bearer token from raw headers */
static const char *extract_bearer(const char *headers) {
    static char token_buf[256];
    const char *auth = strcasestr(headers, "Authorization:");
    if (!auth) return NULL;
    auth += 14;
    while (*auth == ' ') auth++;
    if (strncasecmp(auth, "Bearer ", 7) != 0) return NULL;
    auth += 7;
    while (*auth == ' ') auth++;
    int i = 0;
    while (*auth && *auth != '\r' && *auth != '\n' && i < (int)sizeof(token_buf) - 1)
        token_buf[i++] = *auth++;
    token_buf[i] = 0;
    return token_buf;
}

/* Simple JSON field extractor (non-nested, string values) */
#define EXTRACT_JSON(body, field, dst, sz) do { \
    const char *_p = strstr(body, "\"" field "\""); \
    if (_p) { _p = strchr(_p + strlen(field) + 2, '"'); if (_p) { _p++; \
        int _i = 0; while (*_p && *_p != '"' && _i < (sz)-1) (dst)[_i++] = *_p++; (dst)[_i] = 0; } } \
} while(0)

/* Generate random alphanumeric code */
static void generate_code(char *out, int len) {
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    static unsigned int seed = 0;
    if (seed == 0) seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    for (int i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345; /* LCG */
        out[i] = charset[(seed >> 16) % (sizeof(charset) - 1)];
    }
    out[len] = 0;
}

/* ─── Original Route Handlers (unchanged) ─────────────────────────────── */

static void handle_health(int fd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"uptime\":%ld}", (long)(time(NULL) - start_time));
    send_json(fd, 200, "OK", buf);
}

static void handle_health_detailed(int fd) {
    char buf[512];
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long api_ms = ms_since(&t0);

    struct timespec db_start;
    clock_gettime(CLOCK_MONOTONIC, &db_start);
    PGconn *conn = db_acquire();
    const char *db_status = "ok";
    if (!conn) { db_status = "error"; }
    else {
        PGresult *r = PQexec(conn, "SELECT 1");
        PQclear(r);
        db_release(conn);
    }
    long db_ms = ms_since(&db_start);

    const char *overall = strcmp(db_status, "ok") == 0 ? "healthy" : "degraded";
    snprintf(buf, sizeof(buf),
        "{\"status\":\"%s\",\"checks\":{\"api\":{\"status\":\"ok\",\"latency_ms\":%ld},"
        "\"database\":{\"status\":\"%s\",\"latency_ms\":%ld}},"
        "\"uptime_seconds\":%ld,\"version\":\"0.3.0-c\",\"node_version\":\"native-c\"}",
        overall, api_ms, db_status, db_ms, (long)(time(NULL) - start_time));
    send_json(fd, 200, "OK", buf);
}

/* Helper: run a shell command and capture first N bytes of stdout. */
static int run_capture(const char *cmd, char *out, int sz) {
    FILE *p = popen(cmd, "r");
    if (!p) { out[0] = 0; return -1; }
    int n = fread(out, 1, sz - 1, p);
    out[n > 0 ? n : 0] = 0;
    /* Trim trailing whitespace */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ' || out[n-1] == '\t')) {
        out[--n] = 0;
    }
    pclose(p);
    return n;
}

static void handle_system(int fd) {
    /* Memory — /proc/meminfo IS readable on Termux */
    long total = 0, free_mem = 0, avail = 0, buffers = 0, cached = 0;
    long swap_total = 0, swap_free = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if      (sscanf(line, "MemTotal: %ld kB", &total) == 1) total *= 1024;
            else if (sscanf(line, "MemFree: %ld kB", &free_mem) == 1) free_mem *= 1024;
            else if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1) avail *= 1024;
            else if (sscanf(line, "Buffers: %ld kB", &buffers) == 1) buffers *= 1024;
            else if (sscanf(line, "Cached: %ld kB", &cached) == 1) cached *= 1024;
            else if (sscanf(line, "SwapTotal: %ld kB", &swap_total) == 1) swap_total *= 1024;
            else if (sscanf(line, "SwapFree: %ld kB", &swap_free) == 1) swap_free *= 1024;
        }
        fclose(f);
    }
    long used = total - free_mem - buffers - cached;
    double used_pct = total > 0 ? (double)used / total * 100.0 : 0;
    long swap_used = swap_total - swap_free;
    double swap_pct = swap_total > 0 ? (double)swap_used / swap_total * 100.0 : 0;

    /* Load average — `uptime` shell command is the only working source on
     * Termux because /proc/loadavg is permission-denied on Android. */
    double l1 = 0, l5 = 0, l15 = 0;
    long sys_uptime_sec = 0;
    char up_buf[256];
    if (run_capture("uptime 2>/dev/null", up_buf, sizeof(up_buf)) > 0) {
        char *la = strstr(up_buf, "load average:");
        if (la) sscanf(la, "load average: %lf, %lf, %lf", &l1, &l5, &l15);
        /* Parse "up 14 days, 10:31" */
        char *up = strstr(up_buf, " up ");
        if (up) {
            int days = 0, hours = 0, mins = 0;
            if (sscanf(up + 4, "%d days, %d:%d", &days, &hours, &mins) >= 1 ||
                sscanf(up + 4, "%d:%d", &hours, &mins) >= 1) {
                sys_uptime_sec = (long)days * 86400 + hours * 3600 + mins * 60;
            }
        }
    }

    /* Disk usage of $HOME — use df with default 1K blocks (busybox df
     * does not support -B). Values are multiplied to bytes. */
    long disk_total = 0, disk_used = 0, disk_free = 0;
    int disk_pct = 0;
    char df_buf[512];
    if (run_capture("df \"$HOME\" 2>/dev/null | tail -1", df_buf, sizeof(df_buf)) > 0) {
        char fsname[128];
        long t = 0, u = 0, fr = 0;
        if (sscanf(df_buf, "%127s %ld %ld %ld %d", fsname, &t, &u, &fr, &disk_pct) >= 5) {
            disk_total = t * 1024;
            disk_used  = u * 1024;
            disk_free  = fr * 1024;
        }
    }

    /* Device model + Android version — getprop is freely accessible */
    char device_model[64] = "unknown", android_ver[16] = "?", brand[32] = "";
    run_capture("getprop ro.product.model 2>/dev/null", device_model, sizeof(device_model));
    run_capture("getprop ro.product.brand 2>/dev/null", brand, sizeof(brand));
    run_capture("getprop ro.build.version.release 2>/dev/null", android_ver, sizeof(android_ver));

    /* Hostname (always "localhost" on Termux but harmless) */
    char hostname[64] = "tablet";
    if (gethostname(hostname, sizeof(hostname)) != 0) strcpy(hostname, "tablet");

    /* LAN IP — read from peer of an ephemeral UDP socket. We don't actually
     * send anything; getsockname after connect() reveals the source IP the
     * kernel would use to reach the destination. */
    char lan_ip[32] = "0.0.0.0";
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe >= 0) {
        struct sockaddr_in target;
        memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        inet_aton("1.1.1.1", &target.sin_addr);
        if (connect(probe, (struct sockaddr*)&target, sizeof(target)) == 0) {
            struct sockaddr_in src;
            socklen_t sl = sizeof(src);
            if (getsockname(probe, (struct sockaddr*)&src, &sl) == 0) {
                strncpy(lan_ip, inet_ntoa(src.sin_addr), sizeof(lan_ip) - 1);
            }
        }
        close(probe);
    }

    /* Timestamp */
    long uptime_proc = (long)(time(NULL) - start_time);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    /* Escape strings for JSON */
    char emodel[128], ebrand[64];
    json_escape(emodel, sizeof(emodel), device_model);
    json_escape(ebrand, sizeof(ebrand), brand);

    char buf[2048];
    snprintf(buf, sizeof(buf),
        "{\"hostname\":\"%s\",\"platform\":\"android\",\"arch\":\"arm64\","
        "\"device\":{\"model\":\"%s\",\"brand\":\"%s\",\"android_version\":\"%s\"},"
        "\"network\":{\"lan_ip\":\"%s\",\"port\":3001},"
        "\"cpu\":{\"model\":\"Unisoc T618\",\"cores\":8,\"speed_mhz\":0,"
        "\"load_avg\":{\"1m\":%.2f,\"5m\":%.2f,\"15m\":%.2f}},"
        "\"memory\":{\"total_bytes\":%ld,\"used_bytes\":%ld,\"free_bytes\":%ld,"
        "\"available_bytes\":%ld,\"buffers_bytes\":%ld,\"cached_bytes\":%ld,"
        "\"used_percent\":%.1f},"
        "\"swap\":{\"total_bytes\":%ld,\"used_bytes\":%ld,\"free_bytes\":%ld,"
        "\"used_percent\":%.1f},"
        "\"disk\":{\"total_bytes\":%ld,\"used_bytes\":%ld,\"free_bytes\":%ld,"
        "\"used_percent\":%d},"
        "\"uptime\":{\"system_seconds\":%ld,\"process_seconds\":%ld},"
        "\"timestamp\":\"%s\"}",
        hostname, emodel, ebrand, android_ver, lan_ip,
        l1, l5, l15,
        total, used, free_mem, avail, buffers, cached, used_pct,
        swap_total, swap_used, swap_free, swap_pct,
        disk_total, disk_used, disk_free, disk_pct,
        sys_uptime_sec, uptime_proc, ts);
    send_json(fd, 200, "OK", buf);
}

static void handle_stats_database(int fd) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char buf[2048];
    PGresult *r = PQexec(conn, "SELECT pg_database_size(current_database())");
    long db_size = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) ? atol(PQgetvalue(r, 0, 0)) : 0;
    PQclear(r);

    r = PQexec(conn, "SELECT relname, n_live_tup, n_dead_tup FROM pg_stat_user_tables ORDER BY n_live_tup DESC LIMIT 10");
    char tables[1024] = "[";
    int tlen = 1;
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            if (i > 0) tables[tlen++] = ',';
            tlen += snprintf(tables + tlen, sizeof(tables) - tlen,
                "{\"name\":\"%s\",\"live_tuples\":%s,\"dead_tuples\":%s}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), PQgetvalue(r, i, 2));
        }
    }
    PQclear(r);
    tables[tlen++] = ']'; tables[tlen] = 0;

    r = PQexec(conn, "SELECT COUNT(*) FROM \"Participant\"");
    const char *pcount = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) ? PQgetvalue(r, 0, 0) : "0";
    snprintf(buf, sizeof(buf),
        "{\"database_size_bytes\":%ld,\"tables\":%s,\"participant_count\":%s}",
        db_size, tables, pcount);
    PQclear(r);
    db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_stats_participants(int fd) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char buf[4096];
    PGresult *r = PQexec(conn, "SELECT COUNT(*) FROM \"Participant\"");
    const char *total = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) ? PQgetvalue(r, 0, 0) : "0";
    int off = snprintf(buf, sizeof(buf), "{\"total\":%s,\"by_team\":[", total);
    PQclear(r);

    r = PQexec(conn, "SELECT team, COUNT(*) as c FROM \"Participant\" GROUP BY team ORDER BY c DESC");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            if (i > 0) buf[off++] = ',';
            off += snprintf(buf + off, sizeof(buf) - off, "{\"team\":\"%s\",\"count\":%s}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1));
        }
    }
    PQclear(r);
    off += snprintf(buf + off, sizeof(buf) - off, "],\"recent\":[");

    r = PQexec(conn, "SELECT id, name, email, team, \"createdAt\" FROM \"Participant\" ORDER BY \"createdAt\" DESC LIMIT 5");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char ename[256], eemail[256], eteam[128];
            json_escape(ename, sizeof(ename), PQgetvalue(r, i, 1));
            json_escape(eemail, sizeof(eemail), PQgetvalue(r, i, 2));
            json_escape(eteam, sizeof(eteam), PQgetvalue(r, i, 3));
            if (i > 0) buf[off++] = ',';
            off += snprintf(buf + off, sizeof(buf) - off,
                "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"createdAt\":\"%s\"}",
                PQgetvalue(r, i, 0), ename, eemail, eteam, PQgetvalue(r, i, 4));
        }
    }
    PQclear(r);
    off += snprintf(buf + off, sizeof(buf) - off, "]}");
    db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_participants_list(int fd, const char *path) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    /* Cap limit to keep dashboard responsive. Default 500, max 5000. */
    int limit = 500;
    const char *q = strchr(path, '?');
    if (q) {
        const char *l = strstr(q, "limit=");
        if (l) {
            int v = atoi(l + 6);
            if (v > 0) limit = v > 5000 ? 5000 : v;
        }
    }

    char limit_str[16];
    snprintf(limit_str, sizeof(limit_str), "%d", limit);
    const char *params[1] = { limit_str };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, \"createdAt\", \"updatedAt\" "
        "FROM \"Participant\" ORDER BY \"createdAt\" DESC LIMIT $1::int",
        1, NULL, params, NULL, NULL, 0);
    strbuf sb; sb_init(&sb, 4096);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char ename[512], eemail[512], eteam[256];
            json_escape(ename, sizeof(ename), PQgetvalue(r, i, 1));
            json_escape(eemail, sizeof(eemail), PQgetvalue(r, i, 2));
            json_escape(eteam, sizeof(eteam), PQgetvalue(r, i, 3));
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb, "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"createdAt\":\"%s\",\"updatedAt\":\"%s\"}",
                PQgetvalue(r, i, 0), ename, eemail, eteam, PQgetvalue(r, i, 4), PQgetvalue(r, i, 5));
        }
    }
    PQclear(r); db_release(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

static void handle_participant_get(int fd, const char *id_str) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, \"createdAt\", \"updatedAt\" FROM \"Participant\" WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"participant not found\"}");
        return;
    }
    char buf[1024];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"createdAt\":\"%s\",\"updatedAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), ename, eemail, eteam, PQgetvalue(r, 0, 4), PQgetvalue(r, 0, 5));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_register(int fd, const char *body) {
    char name[256] = "", email[256] = "", team[128] = "";
    EXTRACT_JSON(body, "name", name, sizeof(name));
    EXTRACT_JSON(body, "email", email, sizeof(email));
    EXTRACT_JSON(body, "team", team, sizeof(team));

    if (!name[0] || !email[0] || !team[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"name, email, and team are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[3] = { name, email, team };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Participant\" (name, email, team, \"createdAt\", \"updatedAt\") "
        "VALUES ($1, $2, $3, NOW(), NOW()) RETURNING id, name, email, team, \"createdAt\", \"updatedAt\"",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); db_release(conn);
        if (err && strcmp(err, "23505") == 0)
            send_json(fd, 409, "Conflict", "{\"error\":\"email already registered\"}");
        else
            send_json(fd, 500, "Internal Server Error", "{\"error\":\"insert failed\"}");
        return;
    }
    char buf[1024];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"createdAt\":\"%s\",\"updatedAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), ename, eemail, eteam, PQgetvalue(r, 0, 4), PQgetvalue(r, 0, 5));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_participant_delete(int fd, const char *id_str) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn, "DELETE FROM \"Participant\" WHERE id = $1", 1, NULL, params, NULL, NULL, 0);
    const char *affected = PQcmdTuples(r);
    int deleted = (affected && affected[0] != '0');
    PQclear(r); db_release(conn);
    if (deleted) send_no_content(fd);
    else send_json(fd, 404, "Not Found", "{\"error\":\"participant not found\"}");
}

/* ─── Auth Endpoints ──────────────────────────────────────────────────── */

static void handle_auth_register(int fd, const char *body) {
    char name[256] = "", email[256] = "", team[128] = "", password[128] = "";
    EXTRACT_JSON(body, "name", name, sizeof(name));
    EXTRACT_JSON(body, "email", email, sizeof(email));
    EXTRACT_JSON(body, "team", team, sizeof(team));
    EXTRACT_JSON(body, "password", password, sizeof(password));

    if (!name[0] || !email[0] || !team[0] || !password[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"name, email, team, and password are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[5] = { name, email, team, password, "participant" };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Participant\" (name, email, team, password_hash, role, \"createdAt\", \"updatedAt\") "
        "VALUES ($1, $2, $3, $4, $5, NOW(), NOW()) "
        "RETURNING id, name, email, team, role, \"createdAt\"",
        5, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); db_release(conn);
        if (err && strcmp(err, "23505") == 0)
            send_json(fd, 409, "Conflict", "{\"error\":\"email already registered\"}");
        else
            send_json(fd, 500, "Internal Server Error", "{\"error\":\"registration failed\"}");
        return;
    }

    int user_id = atoi(PQgetvalue(r, 0, 0));
    const char *role = PQgetvalue(r, 0, 4);
    char token[256];
    generate_token(token, sizeof(token), user_id, role);

    char buf[1024];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    snprintf(buf, sizeof(buf),
        "{\"token\":\"%s\",\"participant\":{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\","
        "\"team\":\"%s\",\"role\":\"%s\",\"createdAt\":\"%s\"}}",
        token, user_id, ename, eemail, eteam, role, PQgetvalue(r, 0, 5));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_auth_login(int fd, const char *body) {
    char email[256] = "", password[128] = "";
    EXTRACT_JSON(body, "email", email, sizeof(email));
    EXTRACT_JSON(body, "password", password, sizeof(password));

    if (!email[0] || !password[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"email and password are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { email };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, role, password_hash, \"createdAt\" "
        "FROM \"Participant\" WHERE email = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid email or password\"}");
        return;
    }

    const char *stored_pw = PQgetvalue(r, 0, 5);
    if (!stored_pw || strcmp(stored_pw, password) != 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid email or password\"}");
        return;
    }

    int user_id = atoi(PQgetvalue(r, 0, 0));
    const char *role = PQgetvalue(r, 0, 4);
    char token[256];
    generate_token(token, sizeof(token), user_id, role ? role : "participant");

    char buf[1024];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    snprintf(buf, sizeof(buf),
        "{\"token\":\"%s\",\"participant\":{\"id\":%d,\"name\":\"%s\",\"email\":\"%s\","
        "\"team\":\"%s\",\"role\":\"%s\",\"createdAt\":\"%s\"}}",
        token, user_id, ename, eemail, eteam, role ? role : "participant", PQgetvalue(r, 0, 6));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_auth_me(int fd, const char *headers) {
    const char *token = extract_bearer(headers);
    if (!token) {
        send_json(fd, 401, "Unauthorized", "{\"error\":\"missing or invalid token\"}");
        return;
    }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) {
        send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", user_id);
    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, role, \"createdAt\" FROM \"Participant\" WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"user not found\"}");
        return;
    }

    char buf[1024];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    const char *db_role = PQgetvalue(r, 0, 4);
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"role\":\"%s\",\"createdAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), ename, eemail, eteam,
        db_role ? db_role : "participant", PQgetvalue(r, 0, 5));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

/* ─── Session Endpoints (admin only) ──────────────────────────────────── */

static void handle_session_create(int fd, const char *headers) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    char code[CODE_LEN + 1];
    generate_code(code, CODE_LEN);

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char id_str[16], expiry_str[32];
    snprintf(id_str, sizeof(id_str), "%d", user_id);
    time_t exp_time = time(NULL) + SESSION_EXPIRY;
    struct tm *tm = localtime(&exp_time);
    strftime(expiry_str, sizeof(expiry_str), "%Y-%m-%d %H:%M:%S", tm);

    const char *params[3] = { code, id_str, expiry_str };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Session\" (code, created_by, expires_at, active, \"createdAt\") "
        "VALUES ($1, $2, $3::timestamp, true, NOW()) RETURNING id, code, expires_at",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Session create error: %s\n", PQresultErrorMessage(r));
        PQclear(r); db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"session creation failed\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"code\":\"%s\",\"expires_at\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_sessions_active(int fd, const char *headers) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    PGresult *r = PQexec(conn,
        "SELECT id, code, created_by, expires_at, \"createdAt\", "
        "COALESCE(title, ''), COALESCE(starts_at::text, ''), COALESCE(ends_at::text, '') "
        "FROM \"Session\" "
        "WHERE active = true AND expires_at > NOW() ORDER BY \"createdAt\" DESC");

    strbuf sb; sb_init(&sb, 2048);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char title_esc[512];
            json_escape(title_esc, sizeof(title_esc), PQgetvalue(r, i, 5));
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb, "{\"id\":%s,\"code\":\"%s\",\"created_by\":%s,"
                "\"expires_at\":\"%s\",\"createdAt\":\"%s\","
                "\"title\":\"%s\",\"starts_at\":\"%s\",\"ends_at\":\"%s\"}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), PQgetvalue(r, i, 2),
                PQgetvalue(r, i, 3), PQgetvalue(r, i, 4),
                title_esc, PQgetvalue(r, i, 6), PQgetvalue(r, i, 7));
        }
    }
    PQclear(r); db_release(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

static void handle_session_get(int fd, const char *headers, const char *id_str) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT s.id, s.code, s.created_by, s.expires_at, s.active, s.\"createdAt\", "
        "COUNT(a.id) as attendance_count "
        "FROM \"Session\" s LEFT JOIN \"Attendance\" a ON a.session_id = s.id "
        "WHERE s.id = $1 GROUP BY s.id",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"session not found\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"code\":\"%s\",\"created_by\":%s,\"expires_at\":\"%s\","
        "\"active\":%s,\"createdAt\":\"%s\",\"attendance_count\":%s}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2),
        PQgetvalue(r, 0, 3), PQgetvalue(r, 0, 4)[0] == 't' ? "true" : "false",
        PQgetvalue(r, 0, 5), PQgetvalue(r, 0, 6));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_session_refresh(int fd, const char *headers, const char *id_str) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    char new_code[CODE_LEN + 1];
    generate_code(new_code, CODE_LEN);

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char refresh_str[16];
    snprintf(refresh_str, sizeof(refresh_str), "%d", SESSION_REFRESH);

    const char *params[3] = { new_code, refresh_str, id_str };
    PGresult *r = PQexecParams(conn,
        "UPDATE \"Session\" SET code = $1, "
        "expires_at = expires_at + ($2 || ' seconds')::interval "
        "WHERE id = $3 AND active = true "
        "RETURNING id, code, expires_at",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"session not found or inactive\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"code\":\"%s\",\"expires_at\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

/* ─── Attendance Endpoints ────────────────────────────────────────────── */

static void handle_attendance_checkin(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }

    char session_code[64] = "", device_id[64] = "";
    EXTRACT_JSON(body, "session_code", session_code, sizeof(session_code));
    EXTRACT_JSON(body, "device_id", device_id, sizeof(device_id));

    if (!session_code[0] || !device_id[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"session_code and device_id are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    /* Find active, non-expired session by code */
    const char *p1[1] = { session_code };
    PGresult *r = PQexecParams(conn,
        "SELECT id FROM \"Session\" WHERE code = $1 AND active = true AND expires_at > NOW()",
        1, NULL, p1, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"session not found, inactive, or expired\"}");
        return;
    }
    const char *session_id = PQgetvalue(r, 0, 0);
    char sid_buf[16];
    strncpy(sid_buf, session_id, sizeof(sid_buf) - 1);
    sid_buf[sizeof(sid_buf) - 1] = 0;
    PQclear(r);

    /* Insert attendance (unique constraint prevents duplicates) */
    char uid_str[16];
    snprintf(uid_str, sizeof(uid_str), "%d", user_id);
    const char *p2[3] = { uid_str, sid_buf, device_id };
    r = PQexecParams(conn,
        "INSERT INTO \"Attendance\" (participant_id, session_id, device_id, \"checkedInAt\") "
        "VALUES ($1, $2, $3, NOW()) RETURNING id, \"checkedInAt\"",
        3, NULL, p2, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); db_release(conn);
        if (err && strcmp(err, "23505") == 0)
            send_json(fd, 409, "Conflict", "{\"error\":\"already checked in for this session\"}");
        else
            send_json(fd, 500, "Internal Server Error", "{\"error\":\"checkin failed\"}");
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"participant_id\":%d,\"session_id\":%s,\"checkedInAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), user_id, sid_buf, PQgetvalue(r, 0, 1));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", buf);
}

/* ─── Quick Check-in: device-keyed, no auth round trip ─────────────────
 * The PRIMARY high-throughput path for 2000 simultaneous attendees.
 *
 * Once a participant has registered + linked their device once, the PWA
 * stores device_uuid in localStorage and uses this endpoint exclusively.
 *
 * Performance: ONE SQL statement, ONE round trip — no JWT verify (HMAC),
 * no separate session lookup, no separate device lookup, no separate insert.
 * The CTE resolves all three in a single transaction.
 *
 * Body: {"session_code":"ABC12345","device_uuid":"dev-xxx"}
 * Returns 201 with attendance row, 404 if device unknown / session bad,
 *        409 if already checked in for this session. */
static void handle_attendance_quick_checkin(int fd, const char *body) {
    char session_code[64] = "", device_uuid[128] = "";
    EXTRACT_JSON(body, "session_code", session_code, sizeof(session_code));
    EXTRACT_JSON(body, "device_uuid", device_uuid, sizeof(device_uuid));

    if (!session_code[0] || !device_uuid[0]) {
        send_json(fd, 400, "Bad Request",
            "{\"error\":\"session_code and device_uuid are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) {
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}");
        return;
    }

    /* Single CTE: device → participant + session → insert.
     * If device or session missing/expired, RETURNING is empty → handled below.
     * UNIQUE(participant_id, session_id) gives 409 on duplicate. */
    const char *params[2] = { device_uuid, session_code };
    PGresult *r = PQexecParams(conn,
        "WITH d AS (SELECT participant_id FROM \"Device\" WHERE device_uuid = $1), "
        "     s AS (SELECT id FROM \"Session\" WHERE code = $2 AND active = true AND expires_at > NOW()) "
        "INSERT INTO \"Attendance\" (participant_id, session_id, device_id, \"checkedInAt\") "
        "SELECT d.participant_id, s.id, $1, NOW() FROM d, s "
        "RETURNING id, participant_id, session_id, \"checkedInAt\"",
        2, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *sqlstate = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); db_release(conn);
        if (sqlstate && strcmp(sqlstate, "23505") == 0) {
            send_json(fd, 409, "Conflict",
                "{\"error\":\"already checked in for this session\"}");
        } else {
            send_json(fd, 500, "Internal Server Error", "{\"error\":\"checkin failed\"}");
        }
        return;
    }

    if (PQntuples(r) == 0) {
        /* Either device not linked or session not found/expired — distinguish for UX */
        PQclear(r);
        const char *p1[1] = { device_uuid };
        PGresult *dr = PQexecParams(conn,
            "SELECT 1 FROM \"Device\" WHERE device_uuid = $1", 1, NULL, p1, NULL, NULL, 0);
        int device_known = (PQresultStatus(dr) == PGRES_TUPLES_OK && PQntuples(dr) > 0);
        PQclear(dr);
        db_release(conn);
        if (!device_known) {
            send_json(fd, 401, "Unauthorized",
                "{\"error\":\"device not linked, please register first\"}");
        } else {
            send_json(fd, 404, "Not Found",
                "{\"error\":\"session not found, inactive, or expired\"}");
        }
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"participant_id\":%s,\"session_id\":%s,\"checkedInAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1),
        PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", buf);
}

/* ─── Batch Check-in: admin-only bulk ingest ───────────────────────────
 * Used by the scanner PWA's offline queue when it comes back online — instead
 * of N individual HTTP calls, drains the queue in one round trip.
 *
 * Body: {"session_code":"ABC","items":[{"device_uuid":"dev-1"},{"device_uuid":"dev-2"},...]}
 * Returns 200 with {"accepted":N,"duplicates":M,"unknown":K}. */
static void handle_attendance_batch_checkin(int fd, const char *body) {
    char session_code[64] = "";
    EXTRACT_JSON(body, "session_code", session_code, sizeof(session_code));
    if (!session_code[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"session_code is required\"}");
        return;
    }

    /* Parse items[] — find each "device_uuid":"..." occurrence inside the items array */
    const char *items_start = strstr(body, "\"items\"");
    if (!items_start) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"items array is required\"}");
        return;
    }
    const char *arr_open = strchr(items_start, '[');
    const char *arr_close = arr_open ? strchr(arr_open, ']') : NULL;
    if (!arr_open || !arr_close) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"items must be an array\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    /* Resolve session once */
    const char *p1[1] = { session_code };
    PGresult *sr = PQexecParams(conn,
        "SELECT id FROM \"Session\" WHERE code = $1 AND active = true AND expires_at > NOW()",
        1, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(sr) != PGRES_TUPLES_OK || PQntuples(sr) == 0) {
        PQclear(sr); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"session not found, inactive, or expired\"}");
        return;
    }
    char session_id[16];
    strncpy(session_id, PQgetvalue(sr, 0, 0), sizeof(session_id) - 1);
    session_id[sizeof(session_id) - 1] = 0;
    PQclear(sr);

    int accepted = 0, duplicates = 0, unknown = 0;

    /* Wrap in a single transaction for speed */
    PQexec(conn, "BEGIN");

    const char *p = arr_open;
    while (p && p < arr_close) {
        const char *key = strstr(p, "\"device_uuid\"");
        if (!key || key >= arr_close) break;
        const char *colon = strchr(key, ':');
        if (!colon) break;
        const char *q1 = strchr(colon, '"');
        if (!q1 || q1 >= arr_close) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2 || q2 >= arr_close) break;
        char duuid[128];
        int len = q2 - q1 - 1;
        if (len <= 0 || len >= (int)sizeof(duuid)) { p = q2 + 1; continue; }
        memcpy(duuid, q1 + 1, len);
        duuid[len] = 0;

        const char *params[3] = { duuid, session_id, duuid };
        PGresult *r = PQexecParams(conn,
            "INSERT INTO \"Attendance\" (participant_id, session_id, device_id, \"checkedInAt\") "
            "SELECT participant_id, $2::int, $3, NOW() FROM \"Device\" WHERE device_uuid = $1 "
            "ON CONFLICT (participant_id, session_id) DO NOTHING "
            "RETURNING id",
            3, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK) {
            if (PQntuples(r) > 0) accepted++;
            else {
                /* Either device unknown or duplicate — disambiguate with cheap check */
                PGresult *dr = PQexecParams(conn,
                    "SELECT 1 FROM \"Device\" WHERE device_uuid = $1",
                    1, NULL, &params[0], NULL, NULL, 0);
                if (PQresultStatus(dr) == PGRES_TUPLES_OK && PQntuples(dr) > 0) duplicates++;
                else unknown++;
                PQclear(dr);
            }
        } else {
            unknown++;
        }
        PQclear(r);
        p = q2 + 1;
    }

    PQexec(conn, "COMMIT");
    db_release(conn);

    char out[128];
    snprintf(out, sizeof(out),
        "{\"accepted\":%d,\"duplicates\":%d,\"unknown\":%d}",
        accepted, duplicates, unknown);
    send_json(fd, 200, "OK", out);
}

static void handle_attendance_session(int fd, const char *headers, const char *id_str) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT a.id, a.participant_id, p.name, p.email, p.team, a.device_id, a.\"checkedInAt\" "
        "FROM \"Attendance\" a JOIN \"Participant\" p ON p.id = a.participant_id "
        "WHERE a.session_id = $1 ORDER BY a.\"checkedInAt\" ASC",
        1, NULL, params, NULL, NULL, 0);

    strbuf sb; sb_init(&sb, 4096);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char ename[256], eemail[256], eteam[128];
            json_escape(ename, sizeof(ename), PQgetvalue(r, i, 2));
            json_escape(eemail, sizeof(eemail), PQgetvalue(r, i, 3));
            json_escape(eteam, sizeof(eteam), PQgetvalue(r, i, 4));
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb, "{\"id\":%s,\"participant_id\":%s,\"name\":\"%s\",\"email\":\"%s\","
                "\"team\":\"%s\",\"device_id\":\"%s\",\"checkedInAt\":\"%s\"}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), ename, eemail,
                eteam, PQgetvalue(r, i, 5), PQgetvalue(r, i, 6));
        }
    }
    PQclear(r); db_release(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

static void handle_attendance_me(int fd, const char *headers) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char uid_str[16];
    snprintf(uid_str, sizeof(uid_str), "%d", user_id);
    const char *params[1] = { uid_str };
    PGresult *r = PQexecParams(conn,
        "SELECT a.id, a.session_id, s.code, a.device_id, a.\"checkedInAt\" "
        "FROM \"Attendance\" a JOIN \"Session\" s ON s.id = a.session_id "
        "WHERE a.participant_id = $1 ORDER BY a.\"checkedInAt\" DESC",
        1, NULL, params, NULL, NULL, 0);

    strbuf sb; sb_init(&sb, 2048);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb, "{\"id\":%s,\"session_id\":%s,\"session_code\":\"%s\","
                "\"device_id\":\"%s\",\"checkedInAt\":\"%s\"}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), PQgetvalue(r, i, 2),
                PQgetvalue(r, i, 3), PQgetvalue(r, i, 4));
        }
    }
    PQclear(r); db_release(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

/* ─── Deploy Webhook Endpoint ─────────────────────────────────────────── */

static const char *webhook_secret_env(void) {
    static char secret[128] = "";
    if (!secret[0]) {
        const char *s = getenv("WEBHOOK_SECRET");
        snprintf(secret, sizeof(secret), "%s", s ? s : "deploysecret123");
    }
    return secret;
}

static void handle_deploy_webhook(int fd, const char *headers, const char *body, const char *path) {
    /* Simple shared-secret auth via X-Hub-Signature-256-style header,
       but for simplicity we just check a query param ?secret=XXX */
    const char *q = strchr(path, '?');
    if (!q) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing secret\"}"); return; }

    const char *sec_param = strstr(q, "secret=");
    if (!sec_param) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing secret\"}"); return; }
    sec_param += 7;

    const char *expected = webhook_secret_env();
    int exp_len = strlen(expected);

    /* Compare secret (terminated by & or end) */
    int match = 1;
    for (int i = 0; i < exp_len; i++) {
        if (sec_param[i] != expected[i]) { match = 0; break; }
    }
    if (!match || (sec_param[exp_len] != 0 && sec_param[exp_len] != '&' && sec_param[exp_len] != ' ')) {
        send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid secret\"}");
        return;
    }

    /* Determine which repo from path: /deploy/api or /deploy/console or /deploy/run/<script>.
     * /deploy/api and /deploy/console both run deploy-api.sh because the
     * console is a subfolder of the unified repo — a git pull picks up
     * both API and frontend changes in one operation. The two paths are
     * preserved for backward compatibility with the GitHub webhooks. */
    const char *script;
    if (strstr(path, "/deploy/api?") || strstr(path, "/deploy/console?")) {
        script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/deploy-api.sh";
    } else if (strstr(path, "/deploy/run/")) {
        /* Allowlist scripts that can be run via webhook */
        const char *p = strstr(path, "/deploy/run/") + 12;
        const char *end = strchr(p, '?');
        char name[64] = {0};
        int n = end ? (int)(end - p) : (int)strlen(p);
        if (n <= 0 || n >= (int)sizeof(name)) { send_json(fd, 400, "Bad Request", "{\"error\":\"invalid script name\"}"); return; }
        memcpy(name, p, n); name[n] = 0;
        /* Only allow these specific scripts (security) */
        static char buf[256];
        if (strcmp(name, "setup-cron") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/setup-cron.sh";
        else if (strcmp(name, "db-backup") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/db-backup.sh";
        else if (strcmp(name, "status-check") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/status-check.sh";
        else if (strcmp(name, "full-status") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/full-status.sh";
        else if (strcmp(name, "rollback") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/rollback.sh";
        else if (strcmp(name, "replicate-supabase") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/replicate-supabase.sh";
        else if (strcmp(name, "install-boot") == 0) {
            /* Inline command: copy boot-script.sh to ~/.termux/boot/ and chmod +x */
            snprintf(buf, sizeof(buf), "%s/projects/event-platform-api/deploy/install-boot.sh", getenv("HOME"));
            script = buf;
        }
        else { send_json(fd, 403, "Forbidden", "{\"error\":\"script not in allowlist\"}"); return; }
    } else {
        send_json(fd, 404, "Not Found", "{\"error\":\"unknown deploy target\"}");
        return;
    }

    /* Fork detached child to run deploy script (don't block response) */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: detach, reset signal handlers, run deploy */
        signal(SIGCHLD, SIG_DFL);  /* Allow waitpid in child shell */
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
        close(devnull);
        execl("/data/data/com.termux/files/usr/bin/bash", "bash", script, NULL);
        _exit(1);
    }

    /* Parent: respond immediately */
    char resp[256];
    const char *target = "unknown";
    if (strstr(path, "/deploy/api?")) target = "api";
    else if (strstr(path, "/deploy/console?")) target = "console";
    else if (strstr(path, "/deploy/run/")) target = "script";
    snprintf(resp, sizeof(resp), "{\"status\":\"deploy triggered\",\"target\":\"%s\",\"pid\":%d}", target, pid);
    send_json(fd, 202, "Accepted", resp);
}

/* ─── Device Endpoints ────────────────────────────────────────────────── */
static void handle_device_link(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }

    char device_uuid[64] = "", user_agent[256] = "";
    EXTRACT_JSON(body, "device_uuid", device_uuid, sizeof(device_uuid));
    EXTRACT_JSON(body, "user_agent", user_agent, sizeof(user_agent));

    if (!device_uuid[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"device_uuid is required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char uid_str[16];
    snprintf(uid_str, sizeof(uid_str), "%d", user_id);
    const char *params[3] = { device_uuid, uid_str, user_agent[0] ? user_agent : NULL };

    /* Upsert: insert or update if device already exists */
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Device\" (device_uuid, participant_id, user_agent, \"linkedAt\") "
        "VALUES ($1, $2, $3, NOW()) "
        "ON CONFLICT (device_uuid) DO UPDATE SET participant_id = $2, user_agent = $3, \"linkedAt\" = NOW() "
        "RETURNING id, device_uuid, participant_id, \"linkedAt\"",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Device link error: %s\n", PQresultErrorMessage(r));
        PQclear(r); db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"device link failed\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"device_uuid\":\"%s\",\"participant_id\":%s,\"linkedAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_device_identify(int fd, const char *body) {
    char device_uuid[64] = "";
    EXTRACT_JSON(body, "device_uuid", device_uuid, sizeof(device_uuid));

    if (!device_uuid[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"device_uuid is required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { device_uuid };
    PGresult *r = PQexecParams(conn,
        "SELECT p.id, p.name, p.email, p.team, p.role, d.\"linkedAt\" "
        "FROM \"Device\" d JOIN \"Participant\" p ON p.id = d.participant_id "
        "WHERE d.device_uuid = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); db_release(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"device not linked to any participant\"}");
        return;
    }

    char buf[512];
    char ename[256], eemail[256], eteam[128];
    json_escape(ename, sizeof(ename), PQgetvalue(r, 0, 1));
    json_escape(eemail, sizeof(eemail), PQgetvalue(r, 0, 2));
    json_escape(eteam, sizeof(eteam), PQgetvalue(r, 0, 3));
    const char *db_role = PQgetvalue(r, 0, 4);
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\","
        "\"role\":\"%s\",\"linkedAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), ename, eemail, eteam,
        db_role ? db_role : "participant", PQgetvalue(r, 0, 5));
    PQclear(r); db_release(conn);
    send_json(fd, 200, "OK", buf);
}

/* ─── Event Endpoints (Admin Only) ────────────────────────────────────── */
/* An Event is a top-level container (e.g., "Intrivia 2026"). Sessions
 * and Participants may be scoped to a specific event. */

static void handle_event_create(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    char slug[64] = "", name[256] = "", description[1024] = "", starts_at[64] = "", ends_at[64] = "";
    EXTRACT_JSON(body, "slug", slug, sizeof(slug));
    EXTRACT_JSON(body, "name", name, sizeof(name));
    EXTRACT_JSON(body, "description", description, sizeof(description));
    EXTRACT_JSON(body, "starts_at", starts_at, sizeof(starts_at));
    EXTRACT_JSON(body, "ends_at", ends_at, sizeof(ends_at));

    if (!slug[0] || !name[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"slug and name are required\"}");
        return;
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char uid_str[16];
    snprintf(uid_str, sizeof(uid_str), "%d", user_id);

    const char *params[6] = {
        slug, name,
        description[0] ? description : NULL,
        starts_at[0] ? starts_at : NULL,
        ends_at[0] ? ends_at : NULL,
        uid_str
    };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Event\" (slug, name, description, starts_at, ends_at, created_by) "
        "VALUES ($1, $2, $3, $4::timestamp, $5::timestamp, $6) "
        "RETURNING id, slug, name, \"createdAt\"",
        6, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); db_release(conn);
        if (err && strcmp(err, "23505") == 0) {
            send_json(fd, 409, "Conflict", "{\"error\":\"slug already in use\"}");
        } else {
            send_json(fd, 500, "Internal Server Error", "{\"error\":\"event creation failed\"}");
        }
        return;
    }

    char resp[512];
    snprintf(resp, sizeof(resp),
        "{\"id\":%s,\"slug\":\"%s\",\"name\":\"%s\",\"createdAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1),
        PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", resp);
}

static void handle_events_list(int fd) {
    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    PGresult *r = PQexec(conn,
        "SELECT e.id, e.slug, e.name, COALESCE(e.description, ''), "
        "COALESCE(e.starts_at::text, ''), COALESCE(e.ends_at::text, ''), "
        "e.\"createdAt\", "
        "(SELECT COUNT(*) FROM \"Participant\" WHERE event_id = e.id) AS pcount, "
        "(SELECT COUNT(*) FROM \"Session\" WHERE event_id = e.id) AS scount "
        "FROM \"Event\" e ORDER BY e.\"createdAt\" DESC");

    strbuf sb; sb_init(&sb, 4096);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char ename[512], edesc[2048];
            json_escape(ename, sizeof(ename), PQgetvalue(r, i, 2));
            json_escape(edesc, sizeof(edesc), PQgetvalue(r, i, 3));
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb,
                "{\"id\":%s,\"slug\":\"%s\",\"name\":\"%s\",\"description\":\"%s\","
                "\"starts_at\":\"%s\",\"ends_at\":\"%s\",\"createdAt\":\"%s\","
                "\"participant_count\":%s,\"session_count\":%s}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), ename, edesc,
                PQgetvalue(r, i, 4), PQgetvalue(r, i, 5), PQgetvalue(r, i, 6),
                PQgetvalue(r, i, 7), PQgetvalue(r, i, 8));
        }
    }
    PQclear(r); db_release(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

/* ─── Bulk Participant Import (Admin Only) ────────────────────────────── */
/* Accepts CSV body with header line: name,email,team
 * Returns JSON summary: {"created":N, "skipped":M, "errors":[...]}    */

static void handle_participants_bulk(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    int created = 0, skipped = 0, line_num = 0;
    strbuf errors; sb_init(&errors, 1024);
    sb_append(&errors, "[");

    /* Parse CSV line by line. Skip header. */
    const char *p = body;
    char line_buf[512];
    while (*p) {
        /* Read one line into line_buf */
        int li = 0;
        while (*p && *p != '\n' && *p != '\r' && li < (int)sizeof(line_buf) - 1) {
            line_buf[li++] = *p++;
        }
        line_buf[li] = 0;
        while (*p == '\n' || *p == '\r') p++;
        line_num++;

        /* Skip empty lines and header */
        if (li == 0) continue;
        if (line_num == 1 && (strstr(line_buf, "name") || strstr(line_buf, "email"))) continue;

        /* Parse comma-separated fields: name,email,team,[password] */
        char *fields[4] = {0};
        int fi = 0;
        char *cursor = line_buf;
        fields[fi++] = cursor;
        while (*cursor && fi < 4) {
            if (*cursor == ',') {
                *cursor++ = 0;
                fields[fi++] = cursor;
            } else cursor++;
        }
        if (fi < 3) {
            if (errors.len > 1) sb_append(&errors, ",");
            sb_appendf(&errors, "{\"line\":%d,\"error\":\"too few fields\"}", line_num);
            skipped++;
            continue;
        }
        const char *name  = fields[0];
        const char *email = fields[1];
        const char *team  = fields[2];
        const char *password = fields[3] ? fields[3] : "default-pw-please-change";

        const char *params[5] = { name, email, team, password, "participant" };
        PGresult *r = PQexecParams(conn,
            "INSERT INTO \"Participant\" (name, email, team, password_hash, role, "
            "\"createdAt\", \"updatedAt\") VALUES ($1, $2, $3, $4, $5, NOW(), NOW())",
            5, NULL, params, NULL, NULL, 0);

        if (PQresultStatus(r) == PGRES_COMMAND_OK) {
            created++;
        } else {
            const char *err_code = PQresultErrorField(r, PG_DIAG_SQLSTATE);
            if (err_code && strcmp(err_code, "23505") == 0) {
                skipped++;
                if (errors.len > 1) sb_append(&errors, ",");
                sb_appendf(&errors, "{\"line\":%d,\"email\":\"%s\",\"error\":\"duplicate email\"}",
                    line_num, email);
            } else {
                skipped++;
                if (errors.len > 1) sb_append(&errors, ",");
                sb_appendf(&errors, "{\"line\":%d,\"error\":\"insert failed\"}", line_num);
            }
        }
        PQclear(r);
    }

    db_release(conn);
    sb_append(&errors, "]");

    char resp[2048];
    snprintf(resp, sizeof(resp),
        "{\"created\":%d,\"skipped\":%d,\"errors\":%s}",
        created, skipped, errors.data);
    sb_free(&errors);
    send_json(fd, 200, "OK", resp);
}

/* ─── Stampede Seed (admin only) ──────────────────────────────────────
 * Bulk-creates N synthetic participants and pre-linked devices in a
 * single SQL transaction. Used exclusively by the load test harness so
 * we can isolate the network/check-in path from the auth/register path.
 *
 * Body: {"n": 2000, "run_id": "abc123"}
 *
 * Convention used by the test runner:
 *   email       = stamp-<run_id>-<i>@test.local
 *   device_uuid = dev-stamp-<run_id>-<participant_id>
 *
 * Idempotent on email/device_uuid via ON CONFLICT.
 *
 * Returns: {"created":N, "devices":N}                                  */
static void handle_participants_seed_stamp(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin only\"}"); return; }

    char n_str[16] = "", run_id[64] = "";
    EXTRACT_JSON(body, "n", n_str, sizeof(n_str));
    EXTRACT_JSON(body, "run_id", run_id, sizeof(run_id));
    int n = atoi(n_str);
    if (n <= 0 || n > 5000 || !run_id[0]) {
        send_json(fd, 400, "Bad Request",
            "{\"error\":\"n (1-5000) and run_id required\"}");
        return;
    }

    /* Validate run_id is alphanumeric/underscore/dash (no SQL injection) */
    for (const char *c = run_id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"run_id must be alphanumeric\"}");
            return;
        }
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    PQexec(conn, "BEGIN");

    char n_buf[16];
    snprintf(n_buf, sizeof(n_buf), "%d", n);
    const char *p1[2] = { n_buf, run_id };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Participant\" (name, email, team, password_hash, role, "
        "\"createdAt\", \"updatedAt\") "
        "SELECT 'Stamp ' || i, "
        "       'stamp-' || $2 || '-' || i || '@test.local', "
        "       'T' || (i % 50), 'x', 'participant', NOW(), NOW() "
        "FROM generate_series(0, $1::int - 1) AS s(i) "
        "ON CONFLICT (email) DO NOTHING",
        2, NULL, p1, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "seed participants: %s\n", PQresultErrorMessage(r));
        PQclear(r);
        PQexec(conn, "ROLLBACK");
        db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"participant seed failed\"}");
        return;
    }
    PQclear(r);

    const char *p2[1] = { run_id };
    r = PQexecParams(conn,
        "INSERT INTO \"Device\" (device_uuid, participant_id, user_agent, \"linkedAt\") "
        "SELECT 'dev-stamp-' || $1 || '-' || p.id, p.id, 'stampede-seed', NOW() "
        "FROM \"Participant\" p "
        "WHERE p.email LIKE 'stamp-' || $1 || '-%@test.local' "
        "ON CONFLICT (device_uuid) DO NOTHING",
        1, NULL, p2, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "seed devices: %s\n", PQresultErrorMessage(r));
        PQclear(r);
        PQexec(conn, "ROLLBACK");
        db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"device seed failed\"}");
        return;
    }
    PQclear(r);

    PQexec(conn, "COMMIT");

    /* Report counts AND return the participant ID range so callers
     * don't need to re-fetch /participants for 2000 rows. */
    r = PQexecParams(conn,
        "SELECT (SELECT COUNT(*) FROM \"Participant\" WHERE email LIKE 'stamp-' || $1 || '-%@test.local')::int, "
        "       (SELECT COUNT(*) FROM \"Device\"      WHERE device_uuid LIKE 'dev-stamp-' || $1 || '-%')::int, "
        "       (SELECT MIN(id) FROM \"Participant\" WHERE email LIKE 'stamp-' || $1 || '-%@test.local')::int, "
        "       (SELECT MAX(id) FROM \"Participant\" WHERE email LIKE 'stamp-' || $1 || '-%@test.local')::int",
        1, NULL, p2, NULL, NULL, 0);
    char buf[256] = "{\"created\":0,\"devices\":0}";
    if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
        snprintf(buf, sizeof(buf),
            "{\"created\":%s,\"devices\":%s,\"min_id\":%s,\"max_id\":%s,\"run_id\":\"%s\"}",
            PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1),
            PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3), run_id);
    }
    PQclear(r);
    db_release(conn);
    send_json(fd, 200, "OK", buf);
}

/* Cleanup helper for stampede seeds — admin only.
 * Deletes all attendance, devices, and participants matching a run_id. */
static void handle_participants_seed_cleanup(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin only\"}"); return; }

    char run_id[64] = "";
    EXTRACT_JSON(body, "run_id", run_id, sizeof(run_id));
    if (!run_id[0]) { send_json(fd, 400, "Bad Request", "{\"error\":\"run_id required\"}"); return; }
    for (const char *c = run_id; *c; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) {
            send_json(fd, 400, "Bad Request", "{\"error\":\"run_id must be alphanumeric\"}");
            return;
        }
    }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }
    PQexec(conn, "BEGIN");
    const char *p1[1] = { run_id };
    PQexecParams(conn,
        "DELETE FROM \"Attendance\" WHERE participant_id IN "
        "(SELECT id FROM \"Participant\" WHERE email LIKE 'stamp-' || $1 || '-%@test.local')",
        1, NULL, p1, NULL, NULL, 0);
    PQexecParams(conn,
        "DELETE FROM \"Device\" WHERE device_uuid LIKE 'dev-stamp-' || $1 || '-%'",
        1, NULL, p1, NULL, NULL, 0);
    PQexecParams(conn,
        "DELETE FROM \"Participant\" WHERE email LIKE 'stamp-' || $1 || '-%@test.local'",
        1, NULL, p1, NULL, NULL, 0);
    PQexec(conn, "COMMIT");
    db_release(conn);
    send_json(fd, 200, "OK", "{\"cleaned\":true}");
}

/* ─── Attendance CSV Export (Admin Only) ──────────────────────────────── */

static void handle_attendance_export(int fd, const char *headers, const char *id_str) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT a.id, p.name, p.email, p.team, a.device_id, a.\"checkedInAt\" "
        "FROM \"Attendance\" a JOIN \"Participant\" p ON p.id = a.participant_id "
        "WHERE a.session_id = $1 ORDER BY a.\"checkedInAt\" ASC",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        PQclear(r); db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"query failed\"}");
        return;
    }

    /* Build CSV body */
    strbuf csv; sb_init(&csv, 4096);
    sb_append(&csv, "id,name,email,team,device_id,checked_in_at\r\n");
    int rows = PQntuples(r);
    for (int i = 0; i < rows; i++) {
        /* CSV escape: wrap in quotes if value contains comma/quote/newline */
        sb_appendf(&csv, "%s,\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\r\n",
            PQgetvalue(r, i, 0),
            PQgetvalue(r, i, 1),
            PQgetvalue(r, i, 2),
            PQgetvalue(r, i, 3),
            PQgetvalue(r, i, 4),
            PQgetvalue(r, i, 5));
    }
    PQclear(r); db_release(conn);

    /* Custom response with download headers */
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "%s"
        "Content-Type: text/csv; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"attendance-session-%s.csv\"\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        cors_headers, id_str, csv.len);
    write(fd, hdr, hlen);
    write(fd, csv.data, csv.len);
    sb_free(&csv);
}

/* ─── Scheduled Session (Admin Only) ──────────────────────────────────── */
/* Creates a "named" session with title + scheduled time window.
 * Body: {"title":"...","description":"...","starts_at":"YYYY-MM-DD HH:MM:SS",
 *        "ends_at":"...","duration_minutes":N}                           */

static void handle_session_scheduled(int fd, const char *headers, const char *body) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    char title[256] = "", description[1024] = "", starts_at[64] = "", ends_at[64] = "";
    EXTRACT_JSON(body, "title", title, sizeof(title));
    EXTRACT_JSON(body, "description", description, sizeof(description));
    EXTRACT_JSON(body, "starts_at", starts_at, sizeof(starts_at));
    EXTRACT_JSON(body, "ends_at", ends_at, sizeof(ends_at));

    if (!title[0] || !starts_at[0] || !ends_at[0]) {
        send_json(fd, 400, "Bad Request",
            "{\"error\":\"title, starts_at, and ends_at are required\"}");
        return;
    }

    char code[CODE_LEN + 1];
    generate_code(code, CODE_LEN);

    PGconn *conn = db_acquire();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char uid_str[16];
    snprintf(uid_str, sizeof(uid_str), "%d", user_id);

    const char *params[6] = { code, uid_str, ends_at, title,
                              description[0] ? description : NULL, starts_at };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Session\" (code, created_by, expires_at, active, "
        "\"createdAt\", title, description, starts_at, ends_at) "
        "VALUES ($1, $2, $3::timestamp, true, NOW(), $4, $5, $6::timestamp, $3::timestamp) "
        "RETURNING id, code, title, starts_at, ends_at",
        6, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Scheduled session error: %s\n", PQresultErrorMessage(r));
        PQclear(r); db_release(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"creation failed\"}");
        return;
    }

    char resp[1024];
    snprintf(resp, sizeof(resp),
        "{\"id\":%s,\"code\":\"%s\",\"title\":\"%s\","
        "\"starts_at\":\"%s\",\"ends_at\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1),
        PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3), PQgetvalue(r, 0, 4));
    PQclear(r); db_release(conn);
    send_json(fd, 201, "Created", resp);
}

/* ─── WebSocket Live Attendance Feed ──────────────────────────────────── */
/* Implementation of RFC 6455 (WebSocket protocol) — minimal text-frame
 * server. Supports the upgrade handshake and server-to-client text
 * frames. Used to push attendance check-in events to the admin
 * dashboard in near-real-time.
 *
 * Endpoint: GET /ws/attendance/:session_id
 *   - Upgrades to WebSocket
 *   - Polls the database every 2 seconds for new check-ins
 *   - Pushes JSON frames as new entries appear
 *   - Closes when client disconnects */

static int ws_send_text_frame(int fd, const char *data, int len) {
    /* Server frames: FIN=1, opcode=1 (text), no mask. */
    uint8_t hdr[10];
    int hlen;
    hdr[0] = 0x81;  /* FIN | text */
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (len >> 8) & 0xff;
        hdr[3] = len & 0xff;
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (len >> ((7 - i) * 8)) & 0xff;
        hlen = 10;
    }
    if (write(fd, hdr, hlen) != hlen) return -1;
    if (write(fd, data, len) != len) return -1;
    return 0;
}

static void handle_ws_attendance(int fd, const char *headers, const char *id_str) {
    /* Verify token from query parameter ?token=... since browser WebSocket
     * cannot send arbitrary headers. */
    const char *q = strchr(id_str, '?');
    char token[256] = {0};
    if (q) {
        const char *p = strstr(q, "token=");
        if (p) {
            p += 6;
            int i = 0;
            /* URL-decode while reading: %3A → ':' */
            while (*p && *p != '&' && *p != ' ' && i < (int)sizeof(token) - 1) {
                if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = { p[1], p[2], 0 };
                    token[i++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else if (*p == '+') {
                    token[i++] = ' '; p++;
                } else {
                    token[i++] = *p++;
                }
            }
            token[i] = 0;
        }
    }
    if (!token[0]) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin only\"}"); return; }

    /* Parse session id from the path prefix */
    char session_id[16] = {0};
    int si = 0;
    while (id_str[si] && id_str[si] != '?' && si < (int)sizeof(session_id) - 1) {
        session_id[si] = id_str[si]; si++;
    }
    session_id[si] = 0;

    /* Extract Sec-WebSocket-Key */
    const char *key_hdr = strcasestr(headers, "Sec-WebSocket-Key:");
    if (!key_hdr) { send_json(fd, 400, "Bad Request", "{\"error\":\"not a websocket request\"}"); return; }
    key_hdr += 18;
    while (*key_hdr == ' ') key_hdr++;
    char ws_key[64] = {0};
    int ki = 0;
    while (*key_hdr && *key_hdr != '\r' && *key_hdr != '\n' && ki < (int)sizeof(ws_key) - 1) {
        ws_key[ki++] = *key_hdr++;
    }
    ws_key[ki] = 0;

    /* Compute Sec-WebSocket-Accept = base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) */
    char concat[256];
    int clen = snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", ws_key);
    sha1_ctx sc;
    sha1_init(&sc);
    sha1_update(&sc, (uint8_t *)concat, clen);
    uint8_t digest[20];
    sha1_final(&sc, digest);
    char accept_b64[40];
    base64_encode(digest, 20, accept_b64);

    /* Send upgrade response */
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept_b64);
    if (write(fd, hdr, hlen) != hlen) return;

    /* Send hello frame so client knows we're alive */
    char hello[128];
    snprintf(hello, sizeof(hello),
        "{\"event\":\"connected\",\"session_id\":%s,\"timestamp\":\"%ld\"}",
        session_id, (long)time(NULL));
    ws_send_text_frame(fd, hello, strlen(hello));

    /* Push loop: poll attendance every 2s, send new entries.
     * To keep it simple, track the highest attendance.id we've pushed. */
    long last_id = 0;

    /* Get current max id so we only send new ones */
    PGconn *conn = db_acquire();
    if (conn) {
        const char *params[1] = { session_id };
        PGresult *r = PQexecParams(conn,
            "SELECT COALESCE(MAX(id), 0) FROM \"Attendance\" WHERE session_id = $1",
            1, NULL, params, NULL, NULL, 0);
        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            last_id = atol(PQgetvalue(r, 0, 0));
        }
        PQclear(r);
        db_release(conn);
    }

    /* Configure socket for non-blocking-style read with timeout for ping detection */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int idle_seconds = 0;
    while (idle_seconds < 600) {  /* max 10 min idle */
        sleep(2);

        conn = db_acquire();
        if (!conn) { idle_seconds += 2; continue; }

        char last_str[32];
        snprintf(last_str, sizeof(last_str), "%ld", last_id);
        const char *params[2] = { session_id, last_str };
        PGresult *r = PQexecParams(conn,
            "SELECT a.id, a.participant_id, p.name, p.email, p.team, "
            "a.device_id, a.\"checkedInAt\" "
            "FROM \"Attendance\" a JOIN \"Participant\" p ON p.id = a.participant_id "
            "WHERE a.session_id = $1 AND a.id > $2 ORDER BY a.id ASC",
            2, NULL, params, NULL, NULL, 0);

        int new_rows = 0;
        if (PQresultStatus(r) == PGRES_TUPLES_OK) {
            new_rows = PQntuples(r);
            for (int i = 0; i < new_rows; i++) {
                char ename[256], eemail[256], eteam[128];
                json_escape(ename, sizeof(ename), PQgetvalue(r, i, 2));
                json_escape(eemail, sizeof(eemail), PQgetvalue(r, i, 3));
                json_escape(eteam, sizeof(eteam), PQgetvalue(r, i, 4));
                char frame[1024];
                int flen = snprintf(frame, sizeof(frame),
                    "{\"event\":\"checkin\",\"id\":%s,\"participant_id\":%s,"
                    "\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\","
                    "\"device_id\":\"%s\",\"checkedInAt\":\"%s\"}",
                    PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), ename, eemail,
                    eteam, PQgetvalue(r, i, 5), PQgetvalue(r, i, 6));
                if (ws_send_text_frame(fd, frame, flen) < 0) {
                    PQclear(r); db_release(conn);
                    return;  /* Client disconnected */
                }
                last_id = atol(PQgetvalue(r, i, 0));
            }
        }
        PQclear(r);
        db_release(conn);

        if (new_rows > 0) {
            idle_seconds = 0;
        } else {
            idle_seconds += 2;
            /* Send heartbeat every 30s so client knows we're still alive */
            if (idle_seconds % 30 == 0) {
                const char *hb = "{\"event\":\"heartbeat\"}";
                if (ws_send_text_frame(fd, hb, strlen(hb)) < 0) return;
            }
        }
    }

    /* Send close frame */
    uint8_t close_frame[] = { 0x88, 0x02, 0x03, 0xE8 };  /* opcode 0x8 (close), code 1000 */
    write(fd, close_frame, sizeof(close_frame));
}

/* ─── Static File Serving ─────────────────────────────────────────────── */

static void serve_static(int fd, const char *path) {
    if (strstr(path, "..")) {
        send_json(fd, 403, "Forbidden", "{\"error\":\"forbidden\"}");
        return;
    }

    /* Strip query string before file lookup. Cache busters like
     * styles.css?v=4 must resolve to styles.css on disk. */
    char clean_path[MAX_PATH];
    const char *q = strchr(path, '?');
    int plen = q ? (int)(q - path) : (int)strlen(path);
    if (plen >= (int)sizeof(clean_path)) plen = sizeof(clean_path) - 1;
    memcpy(clean_path, path, plen);
    clean_path[plen] = 0;

    char filepath[MAX_PATH];
    const char *serve_path = (strcmp(clean_path, "/") == 0) ? "/index.html" : clean_path;
    snprintf(filepath, sizeof(filepath), "%s%s", static_dir, serve_path);

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        /* SPA fallback */
        snprintf(filepath, sizeof(filepath), "%s/index.html", static_dir);
        file_fd = open(filepath, O_RDONLY);
        if (file_fd < 0) {
            send_json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
            return;
        }
    }

    struct stat st;
    fstat(file_fd, &st);
    const char *mime = mime_for_ext(filepath);

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n%sContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
        cors_headers, mime, (long)st.st_size);
    write(fd, hdr, hlen);

    char fbuf[8192];
    ssize_t n;
    while ((n = read(file_fd, fbuf, sizeof(fbuf))) > 0)
        write(fd, fbuf, n);
    close(file_fd);
}

/* ─── Request Router ──────────────────────────────────────────────────── */

static void handle_request(int fd, const char *method, const char *path,
                           const char *headers, const char *body) {
    if (strcmp(method, "OPTIONS") == 0) { send_no_content(fd); return; }

    /* ── GET routes ── */
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/health") == 0) { handle_health(fd); return; }
        if (strcmp(path, "/health/detailed") == 0) { handle_health_detailed(fd); return; }
        if (strcmp(path, "/system") == 0) { handle_system(fd); return; }
        if (strcmp(path, "/stats/database") == 0) { handle_stats_database(fd); return; }
        if (strcmp(path, "/stats/participants") == 0) { handle_stats_participants(fd); return; }
        if (strcmp(path, "/events") == 0) { handle_events_list(fd); return; }
        if (strcmp(path, "/participants") == 0 ||
            (strncmp(path, "/participants?", 14) == 0)) {
            handle_participants_list(fd, path); return;
        }
        if (strncmp(path, "/participants/", 14) == 0) {
            handle_participant_get(fd, path + 14);
            return;
        }
        /* Auth */
        if (strcmp(path, "/auth/me") == 0) { handle_auth_me(fd, headers); return; }
        /* Sessions */
        if (strcmp(path, "/sessions/active") == 0) { handle_sessions_active(fd, headers); return; }
        if (strncmp(path, "/sessions/", 10) == 0 && strlen(path) > 10) {
            handle_session_get(fd, headers, path + 10);
            return;
        }
        /* Attendance */
        if (strncmp(path, "/attendance/session/", 20) == 0) {
            const char *sub = path + 20;
            /* Check if /attendance/session/:id/export */
            const char *slash = strchr(sub, '/');
            if (slash && strcmp(slash, "/export") == 0) {
                char id_buf[16];
                int id_len = (int)(slash - sub);
                if (id_len > 0 && id_len < (int)sizeof(id_buf)) {
                    memcpy(id_buf, sub, id_len);
                    id_buf[id_len] = 0;
                    handle_attendance_export(fd, headers, id_buf);
                    return;
                }
            }
            handle_attendance_session(fd, headers, sub);
            return;
        }
        if (strcmp(path, "/attendance/me") == 0) { handle_attendance_me(fd, headers); return; }
        /* WebSocket live feed — runs in forked child to avoid blocking
         * the single-threaded accept loop. */
        if (strncmp(path, "/ws/attendance/", 15) == 0) {
            pid_t pid = fork();
            if (pid == 0) {
                /* Child: handle long-lived WS connection. The cached
                 * worker_conn was inherited from the parent worker; it
                 * is invalid in this process (we can't share a libpq
                 * connection across fork). Drop the pointer so child's
                 * db_acquire opens a fresh one. */
                signal(SIGCHLD, SIG_DFL);
                worker_conn = NULL;
                handle_ws_attendance(fd, headers, path + 15);
                _exit(0);
            }
            /* Parent: SIGCHLD reaper handles zombie cleanup; just close fd
             * (child has its own fd via fork's copy-on-write). */
            return;
        }
        /* Device */
        if (strcmp(path, "/device/identify") == 0) { handle_device_identify(fd, body); return; }

        /* Deploy webhook (POST is preferred but GET also works for browser test) */
        if (strncmp(path, "/deploy/", 8) == 0) {
            handle_deploy_webhook(fd, headers, body, path);
            return;
        }

        /* Fall through to static files */
        serve_static(fd, path);
        return;
    }

    /* ── POST routes ── */
    if (strcmp(method, "POST") == 0) {
        /* Original register */
        if (strcmp(path, "/register") == 0) { handle_register(fd, body); return; }
        /* Auth */
        if (strcmp(path, "/auth/register") == 0) { handle_auth_register(fd, body); return; }
        if (strcmp(path, "/auth/login") == 0) { handle_auth_login(fd, body); return; }
        /* Sessions */
        if (strcmp(path, "/sessions/create") == 0) { handle_session_create(fd, headers); return; }
        if (strcmp(path, "/sessions/scheduled") == 0) { handle_session_scheduled(fd, headers, body); return; }
        /* Events */
        if (strcmp(path, "/events") == 0) { handle_event_create(fd, headers, body); return; }
        /* Bulk participant import */
        if (strcmp(path, "/participants/bulk") == 0) { handle_participants_bulk(fd, headers, body); return; }
        /* Stampede load-test seed (admin only) */
        if (strcmp(path, "/participants/seed-stamp") == 0) { handle_participants_seed_stamp(fd, headers, body); return; }
        if (strcmp(path, "/participants/seed-cleanup") == 0) { handle_participants_seed_cleanup(fd, headers, body); return; }
        if (strncmp(path, "/sessions/", 10) == 0) {
            /* Check for /sessions/:id/refresh */
            const char *rest = path + 10;
            const char *slash = strchr(rest, '/');
            if (slash && strcmp(slash, "/refresh") == 0) {
                char id_buf[16];
                int id_len = (int)(slash - rest);
                if (id_len > 0 && id_len < (int)sizeof(id_buf)) {
                    memcpy(id_buf, rest, id_len);
                    id_buf[id_len] = 0;
                    handle_session_refresh(fd, headers, id_buf);
                    return;
                }
            }
        }
        /* Attendance */
        if (strcmp(path, "/attendance/checkin") == 0) {
            handle_attendance_checkin(fd, headers, body);
            return;
        }
        /* High-throughput path: device-keyed, no auth round trip.
         * Used by scanner PWA after device is linked once. */
        if (strcmp(path, "/attendance/quick-checkin") == 0) {
            handle_attendance_quick_checkin(fd, body);
            return;
        }
        /* Bulk drain for offline queue — single transaction */
        if (strcmp(path, "/attendance/batch-checkin") == 0) {
            handle_attendance_batch_checkin(fd, body);
            return;
        }
        /* Device */
        if (strcmp(path, "/device/link") == 0) { handle_device_link(fd, headers, body); return; }
        if (strcmp(path, "/device/identify") == 0) { handle_device_identify(fd, body); return; }
        /* Deploy webhook */
        if (strncmp(path, "/deploy/", 8) == 0) {
            handle_deploy_webhook(fd, headers, body, path);
            return;
        }
    }

    /* ── DELETE routes ── */
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/participants/", 14) == 0) {
        handle_participant_delete(fd, path + 14);
        return;
    }

    send_json(fd, 404, "Not Found", "{\"error\":\"not found\"}");
}

/* ─── Main ────────────────────────────────────────────────────────────── */

/* ─── Worker accept loop ─────────────────────────────────────────────
 * Runs in each pre-forked child. Reads requests, dispatches to router,
 * then closes the connection. Loops forever. */
static void run_worker_loop(int server_fd) {
    /* Reap our own forked children (WebSocket handlers) automatically. */
    signal(SIGCHLD, SIG_IGN);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); continue;
        }

        /* Per-connection timeout — defeats slow-loris that would block
         * this worker forever. */
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        /* Disable LINGER on accepted sockets — we want graceful close so
         * the client sees the response, not a TCP RST. The LINGER=0 from
         * the listening socket is inherited but applies only to the
         * listening fd; explicitly clear it here to be safe. */
        struct linger no_ling = { .l_onoff = 0, .l_linger = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_LINGER, &no_ling, sizeof(no_ling));

        char buf[BUF_SIZE];
        ssize_t total = 0;
        ssize_t n;
        while ((n = read(client_fd, buf + total, sizeof(buf) - total - 1)) > 0) {
            total += n;
            buf[total] = 0;
            char *header_end = strstr(buf, "\r\n\r\n");
            if (header_end) {
                char *cl = strcasestr(buf, "Content-Length:");
                if (cl) {
                    int content_len = atoi(cl + 15);
                    int header_size = (header_end + 4) - buf;
                    int body_received = total - header_size;
                    if (body_received >= content_len) break;
                } else break;
            }
            if (total >= (ssize_t)sizeof(buf) - 1) break;
        }
        buf[total] = 0;

        char method[16] = "", path[MAX_PATH] = "";
        sscanf(buf, "%15s %1023s", method, path);

        char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4; else body = "";

        uint32_t client_ip = client_addr.sin_addr.s_addr;
        const char *cf_ip = strcasestr(buf, "CF-Connecting-IP:");
        if (cf_ip) {
            cf_ip += 17;
            while (*cf_ip == ' ') cf_ip++;
            char ip_str[64] = {0};
            int j = 0;
            while (*cf_ip && *cf_ip != '\r' && *cf_ip != '\n' && j < (int)sizeof(ip_str) - 1) {
                ip_str[j++] = *cf_ip++;
            }
            struct in_addr ia;
            if (inet_aton(ip_str, &ia)) client_ip = ia.s_addr;
        }

        int skip_rl = (strncmp(path, "/attendance/", 12) == 0) ||
                      (strncmp(path, "/ws/", 4) == 0) ||
                      (strncmp(path, "/deploy/", 8) == 0);
        int rl_limit = (strncmp(path, "/auth/", 6) == 0) ? RL_AUTH_LIMIT : RL_DEFAULT_LIMIT;
        if (!skip_rl && !rate_limit_check(client_ip, rl_limit)) {
            send_rate_limited(client_fd);
        } else {
            handle_request(client_fd, method, path, buf, body);
        }
        close(client_fd);
    }
}

/* On SIGTERM/SIGINT in the supervisor, kill the entire worker pool. */
static void supervisor_term_handler(int sig) {
    (void)sig;
    /* Kill the whole process group (we did setpgid earlier). */
    kill(0, SIGTERM);
    _exit(0);
}

int main(void) {
    start_time = time(NULL);

    const char *port_str = getenv("PORT");
    int port = port_str ? atoi(port_str) : 3000;

    const char *db = getenv("DATABASE_URL");
    snprintf(db_url, sizeof(db_url), "%s",
        db ? db : "postgresql://rofi:devsecret@localhost:5432/eventplatform");

    const char *sd = getenv("STATIC_DIR");
    snprintf(static_dir, sizeof(static_dir), "%s",
        sd ? sd : "/data/data/com.termux/files/home/projects/event-platform-api/console");

    const char *secret = getenv("JWT_SECRET");
    snprintf(jwt_secret, sizeof(jwt_secret), "%s",
        secret ? secret : "devsecret123");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* SO_LINGER with timeout 0 — force RST on close, prevents TIME_WAIT
       holding the port. Critical for tab restart scenarios. */
    struct linger ling = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(server_fd, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 16384) < 0) { perror("listen"); return 1; }

    printf("event-server v0.4.0-c listening on 0.0.0.0:%d\n", port);
    printf("Static dir: %s\n", static_dir);
    printf("Database: %s\n", db_url);
    printf("JWT Secret: %s\n", jwt_secret[0] ? "(set)" : "(default)");

/* Pre-fork worker pool: N children share the listening socket.
     * Override with WORKERS=N env (1 = single-process for smoke tests).
     * Rationale for choosing 8 workers on a 3GB device with an 8-core
     * Unisoc T618: matches CPU count; each worker does sync libpq, so
     * it spends most wall time in PostgreSQL I/O (~5ms typical). */
    int NUM_WORKERS = 8;
    const char *workers_env = getenv("WORKERS");
    if (workers_env) {
        int w = atoi(workers_env);
        if (w >= 1 && w <= 32) NUM_WORKERS = w;
    }
    pid_t worker_pids[32];

    /* Ignore SIGPIPE globally so a client closing a connection mid-write
     * does not kill the worker. */
    signal(SIGPIPE, SIG_IGN);

    /* On graceful termination, kill the entire process group so workers
     * don't orphan into the background and keep port 3001 bound. */
    setpgid(0, 0);
    signal(SIGTERM, supervisor_term_handler);
    signal(SIGINT,  supervisor_term_handler);

    for (int w = 0; w < NUM_WORKERS; w++) {
        pid_t pid = fork();
        if (pid == 0) {
            /* CHILD WORKER */
            run_worker_loop(server_fd);
            exit(0);
        } else if (pid > 0) {
            worker_pids[w] = pid;
        } else {
            perror("fork worker");
        }
    }
    printf("Pre-forked %d worker processes.\n", NUM_WORKERS);

    /* Parent: supervise + restart dead workers */
    while (1) {
        int status;
        pid_t dead = wait(&status);
        if (dead < 0) { sleep(1); continue; }
        for (int w = 0; w < NUM_WORKERS; w++) {
            if (worker_pids[w] == dead) {
                fprintf(stderr, "[supervisor] worker pid=%d died, respawning\n", dead);
                pid_t pid = fork();
                if (pid == 0) {
                    run_worker_loop(server_fd);
                    exit(0);
                } else if (pid > 0) {
                    worker_pids[w] = pid;
                }
                break;
            }
        }
    }

    close(server_fd);
    return 0;
}
