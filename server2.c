/* server2.c - Single-file C HTTP server for event-platform (v2 with auth+attendance)
 * Compile: cc -O2 -o event-server server2.c -lpq
 */
#include <stdio.h>
#include <stdlib.h>
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
    PGconn *conn = db_connect();
    const char *db_status = "ok";
    if (!conn) { db_status = "error"; }
    else {
        PGresult *r = PQexec(conn, "SELECT 1");
        PQclear(r);
        PQfinish(conn);
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

static void handle_system(int fd) {
    char buf[1024];
    long total = 0, free_mem = 0, avail = 0, buffers = 0, cached = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %ld kB", &total) == 1) total *= 1024;
            else if (sscanf(line, "MemFree: %ld kB", &free_mem) == 1) free_mem *= 1024;
            else if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1) avail *= 1024;
            else if (sscanf(line, "Buffers: %ld kB", &buffers) == 1) buffers *= 1024;
            else if (sscanf(line, "Cached: %ld kB", &cached) == 1) cached *= 1024;
        }
        fclose(f);
    }
    long used = total - free_mem - buffers - cached;
    double used_pct = total > 0 ? (double)used / total * 100.0 : 0;

    double l1 = 0, l5 = 0, l15 = 0;
    f = fopen("/proc/loadavg", "r");
    if (f) { fscanf(f, "%lf %lf %lf", &l1, &l5, &l15); fclose(f); }

    long uptime_proc = (long)(time(NULL) - start_time);
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm);

    snprintf(buf, sizeof(buf),
        "{\"hostname\":\"tablet\",\"platform\":\"android\",\"arch\":\"arm64\","
        "\"cpu\":{\"model\":\"Unisoc T618\",\"cores\":8,\"speed_mhz\":0,"
        "\"load_avg\":{\"1m\":%.2f,\"5m\":%.2f,\"15m\":%.2f}},"
        "\"memory\":{\"total_bytes\":%ld,\"used_bytes\":%ld,\"free_bytes\":%ld,\"used_percent\":%.1f},"
        "\"process\":{\"rss_bytes\":0,\"heap_total_bytes\":0,\"heap_used_bytes\":0,\"external_bytes\":0},"
        "\"uptime\":{\"system_seconds\":0,\"process_seconds\":%ld},"
        "\"timestamp\":\"%s\"}",
        l1, l5, l15, total, used, free_mem, used_pct, uptime_proc, ts);
    send_json(fd, 200, "OK", buf);
}

static void handle_stats_database(int fd) {
    PGconn *conn = db_connect();
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
    PQfinish(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_stats_participants(int fd) {
    PGconn *conn = db_connect();
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
    PQfinish(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_participants_list(int fd) {
    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    PGresult *r = PQexec(conn, "SELECT id, name, email, team, \"createdAt\", \"updatedAt\" FROM \"Participant\" ORDER BY \"createdAt\" DESC");
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
    PQclear(r); PQfinish(conn);
    sb_append(&sb, "]");
    send_json(fd, 200, "OK", sb.data);
    sb_free(&sb);
}

static void handle_participant_get(int fd, const char *id_str) {
    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, \"createdAt\", \"updatedAt\" FROM \"Participant\" WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[3] = { name, email, team };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Participant\" (name, email, team, \"createdAt\", \"updatedAt\") "
        "VALUES ($1, $2, $3, NOW(), NOW()) RETURNING id, name, email, team, \"createdAt\", \"updatedAt\"",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_participant_delete(int fd, const char *id_str) {
    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn, "DELETE FROM \"Participant\" WHERE id = $1", 1, NULL, params, NULL, NULL, 0);
    const char *affected = PQcmdTuples(r);
    int deleted = (affected && affected[0] != '0');
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[5] = { name, email, team, password, "participant" };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Participant\" (name, email, team, password_hash, role, \"createdAt\", \"updatedAt\") "
        "VALUES ($1, $2, $3, $4, $5, NOW(), NOW()) "
        "RETURNING id, name, email, team, role, \"createdAt\"",
        5, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        const char *err = PQresultErrorField(r, PG_DIAG_SQLSTATE);
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { email };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, role, password_hash, \"createdAt\" "
        "FROM \"Participant\" WHERE email = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
        send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid email or password\"}");
        return;
    }

    const char *stored_pw = PQgetvalue(r, 0, 5);
    if (!stored_pw || strcmp(stored_pw, password) != 0) {
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char id_str[16];
    snprintf(id_str, sizeof(id_str), "%d", user_id);
    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT id, name, email, team, role, \"createdAt\" FROM \"Participant\" WHERE id = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    char id_str[16], expiry_str[32];
    snprintf(id_str, sizeof(id_str), "%d", user_id);
    time_t exp_time = time(NULL) + SESSION_EXPIRY;
    struct tm *tm = gmtime(&exp_time);
    strftime(expiry_str, sizeof(expiry_str), "%Y-%m-%d %H:%M:%S", tm);

    const char *params[3] = { code, id_str, expiry_str };
    PGresult *r = PQexecParams(conn,
        "INSERT INTO \"Session\" (code, created_by, expires_at, active, \"createdAt\") "
        "VALUES ($1, $2, $3::timestamp, true, NOW()) RETURNING id, code, expires_at",
        3, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        fprintf(stderr, "Session create error: %s\n", PQresultErrorMessage(r));
        PQclear(r); PQfinish(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"session creation failed\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"code\":\"%s\",\"expires_at\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    PQclear(r); PQfinish(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_sessions_active(int fd, const char *headers) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }
    if (strcmp(role, "admin") != 0) { send_json(fd, 403, "Forbidden", "{\"error\":\"admin access required\"}"); return; }

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    PGresult *r = PQexec(conn,
        "SELECT id, code, created_by, expires_at, \"createdAt\" FROM \"Session\" "
        "WHERE active = true AND expires_at > NOW() ORDER BY \"createdAt\" DESC");

    strbuf sb; sb_init(&sb, 2048);
    sb_append(&sb, "[");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            if (i > 0) sb_append(&sb, ",");
            sb_appendf(&sb, "{\"id\":%s,\"code\":\"%s\",\"created_by\":%s,\"expires_at\":\"%s\",\"createdAt\":\"%s\"}",
                PQgetvalue(r, i, 0), PQgetvalue(r, i, 1), PQgetvalue(r, i, 2),
                PQgetvalue(r, i, 3), PQgetvalue(r, i, 4));
        }
    }
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { id_str };
    PGresult *r = PQexecParams(conn,
        "SELECT s.id, s.code, s.created_by, s.expires_at, s.active, s.\"createdAt\", "
        "COUNT(a.id) as attendance_count "
        "FROM \"Session\" s LEFT JOIN \"Attendance\" a ON a.session_id = s.id "
        "WHERE s.id = $1 GROUP BY s.id",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
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
        PQclear(r); PQfinish(conn);
        send_json(fd, 404, "Not Found", "{\"error\":\"session not found or inactive\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"code\":\"%s\",\"expires_at\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    /* Find active, non-expired session by code */
    const char *p1[1] = { session_code };
    PGresult *r = PQexecParams(conn,
        "SELECT id FROM \"Session\" WHERE code = $1 AND active = true AND expires_at > NOW()",
        1, NULL, p1, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
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
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
    send_json(fd, 201, "Created", buf);
}

static void handle_attendance_session(int fd, const char *headers, const char *id_str) {
    const char *token = extract_bearer(headers);
    if (!token) { send_json(fd, 401, "Unauthorized", "{\"error\":\"missing token\"}"); return; }
    char role[32];
    int user_id = verify_token(token, role);
    if (user_id < 0) { send_json(fd, 401, "Unauthorized", "{\"error\":\"invalid or expired token\"}"); return; }

    PGconn *conn = db_connect();
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
    PQclear(r); PQfinish(conn);
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

    PGconn *conn = db_connect();
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
    PQclear(r); PQfinish(conn);
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

    /* Determine which repo from path: /deploy/api or /deploy/console or /deploy/run/<script> */
    const char *script;
    if (strstr(path, "/deploy/api?")) {
        script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/deploy-api.sh";
    } else if (strstr(path, "/deploy/console?")) {
        script = "/data/data/com.termux/files/home/projects/event-platform-console/deploy.sh";
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
        else if (strcmp(name, "debug-cron") == 0) script = "/data/data/com.termux/files/home/projects/event-platform-api/deploy/debug-cron.sh";
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

    PGconn *conn = db_connect();
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
        PQclear(r); PQfinish(conn);
        send_json(fd, 500, "Internal Server Error", "{\"error\":\"device link failed\"}");
        return;
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"device_uuid\":\"%s\",\"participant_id\":%s,\"linkedAt\":\"%s\"}",
        PQgetvalue(r, 0, 0), PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2), PQgetvalue(r, 0, 3));
    PQclear(r); PQfinish(conn);
    send_json(fd, 200, "OK", buf);
}

static void handle_device_identify(int fd, const char *body) {
    char device_uuid[64] = "";
    EXTRACT_JSON(body, "device_uuid", device_uuid, sizeof(device_uuid));

    if (!device_uuid[0]) {
        send_json(fd, 400, "Bad Request", "{\"error\":\"device_uuid is required\"}");
        return;
    }

    PGconn *conn = db_connect();
    if (!conn) { send_json(fd, 500, "Internal Server Error", "{\"error\":\"db connection failed\"}"); return; }

    const char *params[1] = { device_uuid };
    PGresult *r = PQexecParams(conn,
        "SELECT p.id, p.name, p.email, p.team, p.role, d.\"linkedAt\" "
        "FROM \"Device\" d JOIN \"Participant\" p ON p.id = d.participant_id "
        "WHERE d.device_uuid = $1",
        1, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r); PQfinish(conn);
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
    PQclear(r); PQfinish(conn);
    send_json(fd, 200, "OK", buf);
}

/* ─── Static File Serving ─────────────────────────────────────────────── */

static void serve_static(int fd, const char *path) {
    if (strstr(path, "..")) {
        send_json(fd, 403, "Forbidden", "{\"error\":\"forbidden\"}");
        return;
    }

    char filepath[MAX_PATH];
    const char *serve_path = (strcmp(path, "/") == 0) ? "/index.html" : path;
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
        if (strcmp(path, "/participants") == 0) { handle_participants_list(fd); return; }
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
            handle_attendance_session(fd, headers, path + 20);
            return;
        }
        if (strcmp(path, "/attendance/me") == 0) { handle_attendance_me(fd, headers); return; }
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

int main(void) {
    start_time = time(NULL);

    const char *port_str = getenv("PORT");
    int port = port_str ? atoi(port_str) : 3000;

    const char *db = getenv("DATABASE_URL");
    snprintf(db_url, sizeof(db_url), "%s",
        db ? db : "postgresql://rofi:devsecret@localhost:5432/eventplatform");

    const char *sd = getenv("STATIC_DIR");
    snprintf(static_dir, sizeof(static_dir), "%s",
        sd ? sd : "/data/data/com.termux/files/home/projects/event-platform-console");

    const char *secret = getenv("JWT_SECRET");
    snprintf(jwt_secret, sizeof(jwt_secret), "%s",
        secret ? secret : "devsecret123");

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 16) < 0) { perror("listen"); return 1; }

    printf("event-server v0.3.0-c listening on 0.0.0.0:%d\n", port);
    printf("Static dir: %s\n", static_dir);
    printf("Database: %s\n", db_url);
    printf("JWT Secret: %s\n", jwt_secret[0] ? "(set)" : "(default)");

    /* Prefork: spawn 4 workers for parallel request handling */
    signal(SIGCHLD, SIG_IGN);
    int opt2 = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt2, sizeof(opt2));
    for (int i = 1; i < 4; i++) {
        pid_t p = fork();
        if (p == 0) break;  /* child: continue to accept loop */
        if (p < 0) { perror("fork worker"); break; }
        printf("  worker %d spawned (pid %d)\n", i, p);
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        char buf[BUF_SIZE];
        ssize_t total = 0;
        ssize_t n;
        while ((n = read(client_fd, buf + total, sizeof(buf) - total - 1)) > 0) {
            total += n;
            /* Check if we have full headers */
            buf[total] = 0;
            char *header_end = strstr(buf, "\r\n\r\n");
            if (header_end) {
                /* Check Content-Length for body */
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

        /* Parse method and path */
        char method[16] = "", path[MAX_PATH] = "";
        sscanf(buf, "%15s %1023s", method, path);

        /* Find body */
        char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4; else body = "";

        /* Pass full headers buffer for Authorization extraction */
        handle_request(client_fd, method, path, buf, body);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
