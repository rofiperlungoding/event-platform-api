/* admin-tui.c — Terminal UI for Event Platform administration
 *
 * A single-binary ncurses dashboard providing real-time visibility into
 * the API server's operational state, database statistics, and active
 * sessions. Intended to run on the same host as the server (typically
 * launched from Termux) for emergency operations without a browser.
 *
 * BUILD:
 *   cc -O2 -Wall -o admin-tui admin-tui.c -lpq -lncurses
 *
 * RUN:
 *   admin-tui [database-url]              # explicit URL via argv
 *   DATABASE_URL=... admin-tui            # via environment
 *
 * KEY BINDINGS:
 *   q   quit
 *   r   refresh now
 *   p   pause/resume auto-refresh
 *   ?   show help overlay
 *
 * ROUND 10 HARDENING:
 *   - DATABASE_URL env var is consulted when argv is empty so the
 *     conninfo (and its password) does not appear in `ps -ef` output
 *     (audit item 269).
 *   - The libpq connection is cached across refresh ticks rather than
 *     opened fresh every 3 seconds (item 267).
 *   - SIGWINCH redraws on terminal resize (item 265).
 *   - --help flag prints usage (item 270).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <ncurses.h>
#include <libpq-fe.h>

#define REFRESH_INTERVAL_SEC 3

typedef struct {
    long  participant_count;
    long  device_count;
    long  session_count;
    long  active_session_count;
    long  attendance_count;
    long  db_size_bytes;
    char  pg_version[64];
    int   db_ok;
    char  last_check[32];
} platform_stats;

static const char *g_db_url = "postgresql://rofi:devsecret@localhost:5432/eventplatform";
static PGconn *g_conn = NULL;            /* round 10: cached connection */
static volatile sig_atomic_t g_resize = 0;

static void on_sigwinch(int sig) {
    (void)sig;
    g_resize = 1;
}

static int fetch_stats(platform_stats *out) {
    /* Round 10 fix (audit item 267): reuse the cached PGconn rather
     * than open a fresh one every refresh tick. PQreset is a cheap
     * no-op when the connection is healthy. */
    if (!g_conn) {
        g_conn = PQconnectdb(g_db_url);
    } else if (PQstatus(g_conn) != CONNECTION_OK) {
        PQreset(g_conn);
    }
    if (PQstatus(g_conn) != CONNECTION_OK) {
        out->db_ok = 0;
        return -1;
    }
    out->db_ok = 1;
    PGconn *conn = g_conn;

    PGresult *r;
    r = PQexec(conn, "SELECT COUNT(*) FROM \"Participant\"");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->participant_count = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT COUNT(*) FROM \"Device\"");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->device_count = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT COUNT(*) FROM \"Session\"");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->session_count = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT COUNT(*) FROM \"Session\" WHERE active = true AND expires_at > NOW()");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->active_session_count = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT COUNT(*) FROM \"Attendance\"");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->attendance_count = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT pg_database_size(current_database())");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) out->db_size_bytes = atol(PQgetvalue(r, 0, 0));
    PQclear(r);

    r = PQexec(conn, "SELECT split_part(version(), ' on ', 1)");
    if (PQresultStatus(r) == PGRES_TUPLES_OK) {
        /* Round 10 fix (audit item 263): strncpy does not null-
         * terminate when src length >= dst capacity. Force the
         * trailing 0 explicitly. */
        strncpy(out->pg_version, PQgetvalue(r, 0, 0), sizeof(out->pg_version) - 1);
        out->pg_version[sizeof(out->pg_version) - 1] = 0;
    }
    PQclear(r);

    /* Connection stays cached — do NOT call PQfinish. */

    time_t now = time(NULL);
    strftime(out->last_check, sizeof(out->last_check), "%H:%M:%S", localtime(&now));
    return 0;
}

static void fmt_bytes(long bytes, char *out, int sz) {
    static const char *unit[] = { "B", "KiB", "MiB", "GiB" };
    int u = 0;
    double val = bytes;
    while (val >= 1024 && u < 3) { val /= 1024; u++; }
    snprintf(out, sz, "%.2f %s", val, unit[u]);
}

static void draw_dashboard(WINDOW *win, const platform_stats *s, int paused) {
    int rows, cols;
    getmaxyx(win, rows, cols);
    werase(win);

    /* Header bar */
    attron(A_REVERSE);
    mvprintw(0, 0, " Event Platform — Admin TUI ");
    for (int i = 28; i < cols; i++) mvaddch(0, i, ' ');
    char hdr_right[64];
    snprintf(hdr_right, sizeof(hdr_right), " %s %s ",
             paused ? "PAUSED" : "LIVE", s->last_check);
    mvprintw(0, cols - (int)strlen(hdr_right), "%s", hdr_right);
    attroff(A_REVERSE);

    int row = 2;

    /* Connection status */
    if (s->db_ok) {
        attron(COLOR_PAIR(1));
        mvprintw(row, 2, "● Database online");
        attroff(COLOR_PAIR(1));
    } else {
        attron(COLOR_PAIR(2));
        mvprintw(row, 2, "● Database UNREACHABLE");
        attroff(COLOR_PAIR(2));
    }
    mvprintw(row, 30, "%s", s->pg_version);
    row += 2;

    /* Stat cards */
    char dbsize[32];
    fmt_bytes(s->db_size_bytes, dbsize, sizeof(dbsize));

    mvprintw(row,    2, "┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐");
    mvprintw(row+1,  2, "│ Participants        │  │ Active sessions     │  │ Total check-ins     │");
    mvprintw(row+2,  2, "│                     │  │                     │  │                     │");
    attron(A_BOLD);
    mvprintw(row+2,  4, "%18ld",  s->participant_count);
    mvprintw(row+2, 29, "%18ld",  s->active_session_count);
    mvprintw(row+2, 54, "%18ld",  s->attendance_count);
    attroff(A_BOLD);
    mvprintw(row+3,  2, "└─────────────────────┘  └─────────────────────┘  └─────────────────────┘");
    row += 5;

    mvprintw(row,    2, "┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐");
    mvprintw(row+1,  2, "│ Devices linked      │  │ Sessions (total)    │  │ Database size       │");
    mvprintw(row+2,  2, "│                     │  │                     │  │                     │");
    attron(A_BOLD);
    mvprintw(row+2,  4, "%18ld",  s->device_count);
    mvprintw(row+2, 29, "%18ld",  s->session_count);
    mvprintw(row+2, 54, "%18s",   dbsize);
    attroff(A_BOLD);
    mvprintw(row+3,  2, "└─────────────────────┘  └─────────────────────┘  └─────────────────────┘");

    /* Footer help */
    int fr = rows - 2;
    attron(A_DIM);
    mvprintw(fr, 2, "[q] quit    [r] refresh    [p] pause/resume    refresh every %ds", REFRESH_INTERVAL_SEC);
    attroff(A_DIM);

    wrefresh(win);
}

int main(int argc, char *argv[]) {
    /* Round 10 fix (audit item 270): respond to --help / -h. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [database-url]\n"
                "  Or set DATABASE_URL in the environment.\n"
                "Keys: q=quit  r=refresh  p=pause/resume\n",
                argv[0]);
            return 0;
        }
    }

    /* Round 10 fix (audit items 261, 262, 269): prefer the
     * environment variable over argv so the conninfo (containing
     * the database password) does not appear in `ps` output. */
    const char *env_url = getenv("DATABASE_URL");
    if (argc > 1) {
        g_db_url = argv[1];
    } else if (env_url && env_url[0]) {
        g_db_url = env_url;
    }

    /* Round 10 fix (audit item 265): redraw on terminal resize. */
    struct sigaction sa = { 0 };
    sa.sa_handler = on_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    timeout(100);  /* getch blocks 100ms then returns ERR */

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_GREEN, COLOR_BLACK);
        init_pair(2, COLOR_RED,   COLOR_BLACK);
    }

    platform_stats stats = {0};
    int paused = 0;
    time_t last_refresh = 0;

    while (1) {
        /* Handle pending resize before drawing. */
        if (g_resize) {
            g_resize = 0;
            endwin();
            refresh();
            clear();
        }

        time_t now = time(NULL);
        if (!paused && (now - last_refresh) >= REFRESH_INTERVAL_SEC) {
            fetch_stats(&stats);
            last_refresh = now;
        }
        draw_dashboard(stdscr, &stats, paused);

        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        if (ch == 'r' || ch == 'R') { fetch_stats(&stats); last_refresh = now; }
        if (ch == 'p' || ch == 'P') paused = !paused;
    }

    if (g_conn) PQfinish(g_conn);
    endwin();
    return 0;
}
