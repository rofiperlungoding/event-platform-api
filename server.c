/* server.c - Single-file C HTTP server for event-platform
 * Compile: cc -O2 -o event-server server.c -lpq
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
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

static time_t start_time;
static char static_dir[MAX_PATH];
static char db_url[1024];

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static const char *cors_headers =
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n";

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

/* ─── Route Handlers ──────────────────────────────────────────────────── */

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
        "\"uptime_seconds\":%ld,\"version\":\"0.2.0-c\",\"node_version\":\"native-c\"}",
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
    char *buf = malloc(BUF_SIZE * 4);
    int off = 1; buf[0] = '[';
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        for (int i = 0; i < PQntuples(r); i++) {
            char ename[256], eemail[256], eteam[128];
            json_escape(ename, sizeof(ename), PQgetvalue(r, i, 1));
            json_escape(eemail, sizeof(eemail), PQgetvalue(r, i, 2));
            json_escape(eteam, sizeof(eteam), PQgetvalue(r, i, 3));
            if (i > 0) buf[off++] = ',';
            off += snprintf(buf + off, BUF_SIZE * 4 - off,
                "{\"id\":%s,\"name\":\"%s\",\"email\":\"%s\",\"team\":\"%s\",\"createdAt\":\"%s\",\"updatedAt\":\"%s\"}",
                PQgetvalue(r, i, 0), ename, eemail, eteam, PQgetvalue(r, i, 4), PQgetvalue(r, i, 5));
        }
    }
    PQclear(r); PQfinish(conn);
    buf[off++] = ']'; buf[off] = 0;
    send_json(fd, 200, "OK", buf);
    free(buf);
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
    /* Simple JSON parsing for name, email, team */
    char name[256] = "", email[256] = "", team[128] = "";
    const char *p;
    #define EXTRACT(field, dst, sz) do { \
        p = strstr(body, "\"" field "\""); \
        if (p) { p = strchr(p + strlen(field) + 2, '"'); if (p) { p++; \
            int i = 0; while (*p && *p != '"' && i < sz-1) dst[i++] = *p++; dst[i] = 0; } } \
    } while(0)
    EXTRACT("name", name, sizeof(name));
    EXTRACT("email", email, sizeof(email));
    EXTRACT("team", team, sizeof(team));
    #undef EXTRACT

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

static void handle_request(int fd, const char *method, const char *path, const char *body) {
    if (strcmp(method, "OPTIONS") == 0) { send_no_content(fd); return; }

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
        /* Fall through to static files */
        serve_static(fd, path);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/register") == 0) {
        handle_register(fd, body);
        return;
    }

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

    printf("event-server v0.2.0-c listening on 0.0.0.0:%d\n", port);
    printf("Static dir: %s\n", static_dir);
    printf("Database: %s\n", db_url);

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

        handle_request(client_fd, method, path, body);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
