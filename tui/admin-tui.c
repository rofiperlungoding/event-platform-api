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
 *   admin-tui [database-url]
 *
 * KEY BINDINGS:
 *   q   quit
 *   r   refresh now
 *   p   pause/resume auto-refresh
 *   ?   show help overlay
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

static int fetch_stats(platform_stats *out) {
    PGconn *conn = PQconnectdb(g_db_url);
    if (PQstatus(conn) != CONNECTION_OK) {
        out->db_ok = 0;
        PQfinish(conn);
        return -1;
    }
    out->db_ok = 1;

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
        strncpy(out->pg_version, PQgetvalue(r, 0, 0), sizeof(out->pg_version) - 1);
    }
    PQclear(r);

    PQfinish(conn);

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
    if (argc > 1) g_db_url = argv[1];

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

    endwin();
    return 0;
}
