/*
 * main.c – Snake game entry point, interactive TUI menu, agent runner.
 *
 * Three launch modes:
 *   (default)  Interactive ncurses menu → play or run agents
 *   play       Direct interactive play (no menu)
 *   agent      JSON line-protocol on stdin/stdout (for external tools)
 */

#define _POSIX_C_SOURCE 200809L

#include "game.h"
#include "agent.h"

#include <ctype.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <math.h>
#include <ncurses.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ═══════════════════════════════════════════════════════════════════
 * CONSTANTS
 * ═══════════════════════════════════════════════════════════════════ */

/* ncurses colour-pair IDs */
#define CP_TITLE    1
#define CP_SELECT   2
#define CP_SNAKE    3
#define CP_HEAD     4
#define CP_FOOD     5
#define CP_BORDER   6
#define CP_INFO     7
#define CP_GOOD     8
#define CP_BAD      9
#define CP_DIM     10

/* ═══════════════════════════════════════════════════════════════════
 * TYPES
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { RUN_MENU, RUN_PLAY, RUN_AGENT_PROTO } RunMode;

/* ── Dynamic agent registry ─────────────────────────────────────── */

#define MAX_AGENTS 32

typedef struct {
    char name[64];          /* display name, e.g. "Python Naive"       */
    char command[512];      /* shell command to run in pipe mode       */
    char description[128];  /* short description for the TUI           */
    char dir[PATH_MAX];     /* working directory (the agent's folder)  */
    int  is_builtin;        /* 1 = built-in agent (no subprocess)      */
    int  is_custom;         /* 1 = "Custom Command" placeholder        */
} AgentEntry;

static AgentEntry g_agents[MAX_AGENTS];
static const char *g_agent_names[MAX_AGENTS]; /* pointers into g_agents[].name */
static int         g_num_agents = 0;

typedef struct {
    int          agent_idx;      /* index into g_agents[] */
    char         cmd[512];       /* only for custom command */
    int          num_games;
    int          width, height;
    int          speed_ms;
    unsigned int seed;
    int          step;           /* 0 = timed, 1 = step */
    int          watch;
    int          do_log;
} AgentCfg;
typedef struct {
    int    score;
    int    ticks;
    double elapsed;
} GameResult;

typedef struct {
    GameResult *results;
    int         count;
    int         completed;
    double      total_time;
    char        log_path[512];
} BatchResult;

/* External agent sub-process handle */
typedef struct {
    pid_t pid;
    FILE *wr;   /* pipe to agent stdin  */
    FILE *rd;   /* pipe from agent stdout */
    int   rd_fd;
} ExtAgent;

/* ═══════════════════════════════════════════════════════════════════
 * GLOBALS
 * ═══════════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t g_quit    = 0;
static volatile sig_atomic_t g_abort   = 0;   /* abort current batch */
static int                   g_color   = 0;
static char                  g_exe_dir[PATH_MAX] = ".";

/* ═══════════════════════════════════════════════════════════════════
 * AGENT REGISTRY – scan agents/ for agent.json manifests
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Minimal targeted JSON string extractor.
 * Finds "key":"value" in a JSON string and copies value into out.
 */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t outsz) {
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < outsz - 1) {
        if (*p == '\\' && *(p + 1)) { p++; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static void register_builtin(void) {
    AgentEntry *e = &g_agents[g_num_agents];
    memset(e, 0, sizeof *e);
    strncpy(e->name, "Built-in Naive", sizeof e->name - 1);
    strncpy(e->description, "Avoids immediate death, picks randomly",
            sizeof e->description - 1);
    e->is_builtin = 1;
    g_agent_names[g_num_agents] = e->name;
    g_num_agents++;
}

static void register_custom(void) {
    AgentEntry *e = &g_agents[g_num_agents];
    memset(e, 0, sizeof *e);
    strncpy(e->name, "Custom Command", sizeof e->name - 1);
    strncpy(e->description, "Enter a shell command (must use --pipe protocol)",
            sizeof e->description - 1);
    e->is_custom = 1;
    g_agent_names[g_num_agents] = e->name;
    g_num_agents++;
}

static void scan_agents_dir(void) {
    char agents_dir[PATH_MAX];
    snprintf(agents_dir, sizeof agents_dir, "%.900s/agents", g_exe_dir);

    DIR *dp = opendir(agents_dir);
    if (!dp) {
        /* fallback: try cwd/agents */
        dp = opendir("agents");
        if (dp) strncpy(agents_dir, "agents", sizeof agents_dir - 1);
    }
    if (!dp) return;

    struct dirent *de;
    while ((de = readdir(dp)) != NULL && g_num_agents < MAX_AGENTS - 1) {
        if (de->d_name[0] == '.') continue;

        char manifest[PATH_MAX];
        snprintf(manifest, sizeof manifest, "%.800s/%s/agent.json",
                 agents_dir, de->d_name);

        FILE *f = fopen(manifest, "r");
        if (!f) continue;

        char buf[2048];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';

        char name[64] = "", cmd[512] = "", desc[128] = "";
        json_get_str(buf, "name", name, sizeof name);
        json_get_str(buf, "command", cmd, sizeof cmd);
        json_get_str(buf, "description", desc, sizeof desc);

        if (!name[0] || !cmd[0]) continue;  /* need at least name + command */

        AgentEntry *e = &g_agents[g_num_agents];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", name);
        snprintf(e->command, sizeof e->command, "%s", cmd);
        snprintf(e->description, sizeof e->description, "%s", desc);
        snprintf(e->dir, sizeof e->dir, "%.800s/%s", agents_dir, de->d_name);
        g_agent_names[g_num_agents] = e->name;
        g_num_agents++;
    }
    closedir(dp);
}

static void init_agent_registry(void) {
    g_num_agents = 0;
    register_builtin();
    scan_agents_dir();
    register_custom();    /* always last */
}


/* ═══════════════════════════════════════════════════════════════════
 * UTILITY
 * ═══════════════════════════════════════════════════════════════════ */

static void handle_sig(int s) { (void)s; g_quit = 1; g_abort = 1; }

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static const char *turn_name(Turn t) {
    switch (t) {
    case TURN_LEFT:  return "left";
    case TURN_RIGHT: return "right";
    default:         return "straight";
    }
}

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Resolve the directory containing our own binary. */
static void resolve_exe_dir(const char *argv0) {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *d = dirname(buf);
        strncpy(g_exe_dir, d, sizeof(g_exe_dir) - 1);
        return;
    }
    /* fallback: derive from argv[0] */
    strncpy(buf, argv0, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *d = dirname(buf);
    strncpy(g_exe_dir, d, sizeof(g_exe_dir) - 1);
}

/* ═══════════════════════════════════════════════════════════════════
 * NCURSES SETUP
 * ═══════════════════════════════════════════════════════════════════ */

static void init_colors(void) {
    if (has_colors()) {
        start_color();
        use_default_colors();
        g_color = 1;
        init_pair(CP_TITLE,  COLOR_CYAN,   -1);
        init_pair(CP_SELECT, COLOR_BLACK,  COLOR_WHITE);
        init_pair(CP_SNAKE,  COLOR_GREEN,  -1);
        init_pair(CP_HEAD,   COLOR_YELLOW, -1);
        init_pair(CP_FOOD,   COLOR_RED,    -1);
        init_pair(CP_BORDER, COLOR_WHITE,  -1);
        init_pair(CP_INFO,   COLOR_CYAN,   -1);
        init_pair(CP_GOOD,   COLOR_GREEN,  -1);
        init_pair(CP_BAD,    COLOR_RED,    -1);
        init_pair(CP_DIM,    COLOR_BLACK,  -1);  /* bright-black = grey */
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * TUI: BOX-DRAWING HELPERS
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_box(int y, int x, int w, int h) {
    mvaddch(y, x, ACS_ULCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    for (int j = 1; j < h - 1; j++) {
        mvaddch(y + j, x, ACS_VLINE);
        mvaddch(y + j, x + w - 1, ACS_VLINE);
    }
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    for (int i = 1; i < w - 1; i++) mvaddch(y + h - 1, x + i, ACS_HLINE);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
}

static void draw_hsep(int y, int x, int w) {
    mvaddch(y, x, ACS_LTEE);
    for (int i = 1; i < w - 1; i++) mvaddch(y, x + i, ACS_HLINE);
    mvaddch(y, x + w - 1, ACS_RTEE);
}

static void center_str(int y, int x, int w, const char *s) {
    int pad = ((int)w - (int)strlen(s)) / 2;
    if (pad < 0) pad = 0;
    mvprintw(y, x + pad, "%s", s);
}

/* ═══════════════════════════════════════════════════════════════════
 * JSON STATE OUTPUT (shared between agent-protocol mode & runner)
 * ═══════════════════════════════════════════════════════════════════ */

static void write_state_json(FILE *out, const SnakeGame *g) {
    fprintf(out,
        "{\"tick\":%d,\"alive\":%s,\"score\":%d,"
        "\"width\":%d,\"height\":%d,\"dir\":\"%s\",\"snake\":[",
        g->tick, g->alive ? "true" : "false", g->score,
        g->width, g->height, direction_name(g->dir));
    for (int i = 0; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        fprintf(out, "%s[%d,%d]", i ? "," : "",
                g->body[idx].x, g->body[idx].y);
    }
    fprintf(out, "],\"food\":[%d,%d]}\n", g->food.x, g->food.y);
    fflush(out);
}

/* ═══════════════════════════════════════════════════════════════════
 * AGENT-PROTOCOL MODE (CLI: ./snake agent)
 * ═══════════════════════════════════════════════════════════════════ */

static Turn read_cmd_blocking(void) {
    char buf[64];
    if (!fgets(buf, sizeof buf, stdin)) { g_quit = 1; return TURN_STRAIGHT; }
    return parse_turn(buf);
}

static Turn read_cmd_timed(int ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int r = poll(&pfd, 1, ms);
    if (r > 0 && (pfd.revents & POLLIN))  return read_cmd_blocking();
    if (r > 0 && (pfd.revents & (POLLHUP | POLLERR))) g_quit = 1;
    return TURN_STRAIGHT;
}

static void run_agent_proto(int width, int height, unsigned int seed,
                            int step, int speed_ms) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin,  NULL, _IOLBF, 0);
    static SnakeGame game;
    game_init(&game, width, height, seed);
    while (game.alive && !g_quit) {
        write_state_json(stdout, &game);
        Turn t = step ? read_cmd_blocking() : read_cmd_timed(speed_ms);
        if (g_quit) break;
        game_set_turn(&game, t);
        game_tick(&game);
    }
    write_state_json(stdout, &game);
}

/* ═══════════════════════════════════════════════════════════════════
 * GAME RENDERING (ncurses)
 * ═══════════════════════════════════════════════════════════════════ */

static void draw_game(const SnakeGame *g, const char *title_extra) {
    int oy = 1;

    /* title bar */
    if (g_color) attron(COLOR_PAIR(CP_INFO));
    attron(A_BOLD);
    mvprintw(0, 0, " SNAKE ");
    attroff(A_BOLD);
    printw(" Score: %-4d  Tick: %-6d  Dir: %-5s",
           g->score, g->tick, direction_name(g->dir));
    if (title_extra) printw("  %s", title_extra);
    clrtoeol();
    if (g_color) attroff(COLOR_PAIR(CP_INFO));

    /* border */
    if (g_color) attron(COLOR_PAIR(CP_BORDER));
    mvaddch(oy, 0, ACS_ULCORNER);
    for (int x = 0; x < g->width; x++) mvaddch(oy, x + 1, ACS_HLINE);
    mvaddch(oy, g->width + 1, ACS_URCORNER);
    for (int y = 0; y < g->height; y++) {
        mvaddch(oy + y + 1, 0,            ACS_VLINE);
        mvaddch(oy + y + 1, g->width + 1, ACS_VLINE);
    }
    mvaddch(oy + g->height + 1, 0, ACS_LLCORNER);
    for (int x = 0; x < g->width; x++) mvaddch(oy + g->height + 1, x + 1, ACS_HLINE);
    mvaddch(oy + g->height + 1, g->width + 1, ACS_LRCORNER);
    if (g_color) attroff(COLOR_PAIR(CP_BORDER));

    /* food */
    if (g->food.x >= 0) {
        if (g_color) attron(COLOR_PAIR(CP_FOOD) | A_BOLD);
        mvaddch(oy + g->food.y + 1, g->food.x + 1, '*');
        if (g_color) attroff(COLOR_PAIR(CP_FOOD) | A_BOLD);
    }

    /* snake body */
    if (g_color) attron(COLOR_PAIR(CP_SNAKE));
    for (int i = 1; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        mvaddch(oy + g->body[idx].y + 1, g->body[idx].x + 1, 'o');
    }
    if (g_color) attroff(COLOR_PAIR(CP_SNAKE));

    /* head */
    {
        Point h = g->body[g->head_idx];
        if (g_color) attron(COLOR_PAIR(CP_HEAD) | A_BOLD);
        mvaddch(oy + h.y + 1, h.x + 1, '@');
        if (g_color) attroff(COLOR_PAIR(CP_HEAD) | A_BOLD);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * TUI: MAIN MENU
 * ═══════════════════════════════════════════════════════════════════ */

/* Returns: 0=Play, 1=Agent, 2=Quit */
static int show_main_menu(void) {
    const char *items[] = { "Play Game", "Agent Mode", "Quit" };
    int n = 3, sel = 0;

    while (!g_quit) {
        erase();
        int bw = 36, bh = n + 6;
        int bx = (COLS - bw) / 2, by = (LINES - bh) / 2;

        if (g_color) attron(COLOR_PAIR(CP_BORDER));
        draw_box(by, bx, bw, bh);
        draw_hsep(by + 2, bx, bw);
        if (g_color) attroff(COLOR_PAIR(CP_BORDER));

        if (g_color) attron(COLOR_PAIR(CP_TITLE));
        attron(A_BOLD);
        center_str(by + 1, bx, bw, "S N A K E");
        attroff(A_BOLD);
        if (g_color) attroff(COLOR_PAIR(CP_TITLE));

        for (int i = 0; i < n; i++) {
            int iy = by + 3 + i;
            if (i == sel) {
                attron(A_REVERSE | A_BOLD);
                mvprintw(iy, bx + 3, "  > %-24s", items[i]);
                attroff(A_REVERSE | A_BOLD);
            } else {
                mvprintw(iy, bx + 3, "    %-24s", items[i]);
            }
        }

        if (g_color) attron(COLOR_PAIR(CP_DIM));
        mvprintw(LINES - 1, 1, "Up/Down: Navigate   Enter: Select   Q: Quit");
        if (g_color) attroff(COLOR_PAIR(CP_DIM));

        refresh();
        int ch = getch();
        switch (ch) {
        case KEY_UP:   case 'k': sel = (sel + n - 1) % n; break;
        case KEY_DOWN: case 'j': sel = (sel + 1) % n;     break;
        case '\n': case KEY_ENTER: return sel;
        case 'q': case 'Q': return 2;
        case '1': return 0; case '2': return 1; case '3': return 2;
        }
    }
    return 2;
}

/* ═══════════════════════════════════════════════════════════════════
 * TUI: SETTINGS FORM (shared by play & agent config)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Generic form field descriptor.
 *   type=0  toggle (left/right cycles through options[])
 *   type=1  number (left/right adjusts by step; </> by step_big)
 *   type=2  text   (Enter to edit)
 */
typedef struct {
    const char *label;
    int type;                 /* 0=toggle  1=number  2=text */
    int val;                  /* current value */
    int lo, hi;               /* bounds (number) */
    int step_sm, step_lg;     /* ±increments */
    const char **opts;        /* names for toggle options */
    int nopts;
    const char *suffix;       /* e.g. "ms" */
    char text[512];           /* text buffer (type=2) */
    int visible;              /* 0 → skip rendering */
} FField;

/*
 * Simple single-line text editor within the form.
 * Returns 1 on Enter (confirm), 0 on Esc (cancel).
 */
static int edit_text_field(int y, int x, int w, char *buf, int bufsz) {
    int pos = (int)strlen(buf);
    curs_set(1);
    while (1) {
        move(y, x);
        for (int i = 0; i < w; i++) addch(' ');
        mvprintw(y, x, "%.*s", w, buf);
        move(y, x + (pos < w ? pos : w - 1));
        refresh();

        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) { curs_set(0); return 1; }
        if (ch == 27)  { curs_set(0); return 0; }
        if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && pos > 0) {
            memmove(buf + pos - 1, buf + pos, strlen(buf + pos) + 1);
            pos--;
        } else if (ch == KEY_LEFT  && pos > 0)      pos--;
          else if (ch == KEY_RIGHT && buf[pos])      pos++;
          else if (ch == KEY_HOME)                   pos = 0;
          else if (ch == KEY_END)                    pos = (int)strlen(buf);
          else if (ch >= 32 && ch < 127 && (int)strlen(buf) < bufsz - 1) {
            memmove(buf + pos + 1, buf + pos, strlen(buf + pos) + 1);
            buf[pos++] = (char)ch;
        }
    }
}

/*
 * Draw a form and let the user edit it.
 * Returns 1 = confirmed (Enter), 0 = cancelled (Esc).
 */
static int show_form(const char *title, FField *ff, int nf, const char *help) {
    int sel = 0;

    /* ensure sel points to a visible field */
    while (sel < nf && !ff[sel].visible) sel++;

    while (!g_quit) {
        erase();

        /* count visible fields */
        int nvis = 0;
        for (int i = 0; i < nf; i++) if (ff[i].visible) nvis++;

        int bw = 54, bh = nvis + 7;
        int bx = (COLS - bw) / 2, by = (LINES - bh) / 2;
        if (by < 0) by = 0;

        if (g_color) attron(COLOR_PAIR(CP_BORDER));
        draw_box(by, bx, bw, bh);
        draw_hsep(by + 2, bx, bw);
        draw_hsep(by + bh - 3, bx, bw);
        if (g_color) attroff(COLOR_PAIR(CP_BORDER));

        if (g_color) attron(COLOR_PAIR(CP_TITLE));
        attron(A_BOLD);
        center_str(by + 1, bx, bw, title);
        attroff(A_BOLD);
        if (g_color) attroff(COLOR_PAIR(CP_TITLE));

        /* render fields */
        int row = by + 3;
        for (int i = 0; i < nf; i++) {
            if (!ff[i].visible) continue;
            int lx = bx + 3, vx = bx + 19;
            int is_sel = (i == sel);

            mvprintw(row, lx, "%-14s", ff[i].label);

            if (is_sel) attron(A_REVERSE);
            if (ff[i].type == 0) {
                /* toggle */
                mvprintw(row, vx, " < %-23s > ", ff[i].opts[ff[i].val]);
            } else if (ff[i].type == 1) {
                /* number */
                char tmp[64];
                if (ff[i].suffix)
                    snprintf(tmp, sizeof tmp, "%d %s", ff[i].val, ff[i].suffix);
                else
                    snprintf(tmp, sizeof tmp, "%d", ff[i].val);
                mvprintw(row, vx, " < %-23s > ", tmp);
            } else {
                /* text */
                mvprintw(row, vx, " %-27.27s ",
                         ff[i].text[0] ? ff[i].text : "(Enter to edit)");
            }
            if (is_sel) attroff(A_REVERSE);
            row++;
        }

        /* help line */
        mvprintw(by + bh - 2, bx + 3, "%s", help ? help : "");

        if (g_color) attron(COLOR_PAIR(CP_DIM));
        mvprintw(LINES - 1, 1,
                 "Up/Down: Navigate  Left/Right: Change  Enter: Start  Esc: Back");
        if (g_color) attroff(COLOR_PAIR(CP_DIM));

        refresh();
        int ch = getch();

        /* navigation */
        if (ch == KEY_UP || ch == 'k') {
            do { sel = (sel + nf - 1) % nf; } while (!ff[sel].visible);
            continue;
        }
        if (ch == KEY_DOWN || ch == 'j') {
            do { sel = (sel + 1) % nf; } while (!ff[sel].visible);
            continue;
        }

        /* confirm / cancel */
        if (ch == '\n' || ch == KEY_ENTER) {
            if (ff[sel].type == 2) {
                /* Enter on text field → edit it */
                int ey = by + 3;
                for (int i = 0; i < sel; i++) if (ff[i].visible) ey++;
                edit_text_field(ey, bx + 20, 27, ff[sel].text,
                                (int)sizeof(ff[sel].text));
                continue;
            }
            return 1;
        }
        if (ch == 27) return 0;  /* Esc */

        /* value changes */
        FField *f = &ff[sel];
        if (f->type == 0) {
            /* toggle */
            if (ch == KEY_LEFT)  f->val = (f->val + f->nopts - 1) % f->nopts;
            if (ch == KEY_RIGHT) f->val = (f->val + 1) % f->nopts;
        } else if (f->type == 1) {
            /* number */
            if (ch == KEY_LEFT)  f->val = clamp(f->val - f->step_sm, f->lo, f->hi);
            if (ch == KEY_RIGHT) f->val = clamp(f->val + f->step_sm, f->lo, f->hi);
            if (ch == '<' || ch == KEY_PPAGE)
                f->val = clamp(f->val - f->step_lg, f->lo, f->hi);
            if (ch == '>' || ch == KEY_NPAGE)
                f->val = clamp(f->val + f->step_lg, f->lo, f->hi);
            /* digit typing: replace value */
            if (isdigit(ch)) {
                int v = ch - '0';
                timeout(500);  /* short timeout for multi-digit input */
                while (1) {
                    int d = getch();
                    if (d == ERR) break;
                    if (isdigit(d)) { v = v * 10 + (d - '0'); }
                    else { ungetch(d); break; }
                }
                timeout(-1);
                f->val = clamp(v, f->lo, f->hi);
            }
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * PLAY MODE
 * ═══════════════════════════════════════════════════════════════════ */

static Turn key_to_turn(Direction cur, int key) {
    Direction desired;
    switch (key) {
    case KEY_UP:    case 'w': case 'W': desired = DIR_UP;    break;
    case KEY_DOWN:  case 's': case 'S': desired = DIR_DOWN;  break;
    case KEY_LEFT:  case 'a': case 'A': desired = DIR_LEFT;  break;
    case KEY_RIGHT: case 'd': case 'D': desired = DIR_RIGHT; break;
    default: return TURN_STRAIGHT;
    }
    return direction_to_turn(cur, desired);
}

static int is_game_key(int ch) {
    switch (ch) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case 'w': case 'W': case 'a': case 'A':
    case 's': case 'S': case 'd': case 'D':
    case ' ': case '\n': case KEY_ENTER:
        return 1;
    default: return 0;
    }
}

static void run_play(int width, int height, int speed_ms,
                     unsigned int seed, int step) {
    static SnakeGame game;
    game_init(&game, width, height, seed);

    while (game.alive && !g_quit) {
        erase();
        draw_game(&game, NULL);

        int sy = game.height + 4;
        if (step)
            mvprintw(sy, 0, "Mode: Step (press a key to advance)");
        else
            mvprintw(sy, 0, "Mode: Timed (%d ms)", speed_ms);
        mvprintw(sy + 1, 0,
                 "Controls: Arrow keys / WASD | Space/Enter: straight | Q: quit");
        refresh();

        if (step) {
            timeout(-1);
            int ch;
            do {
                ch = getch();
                if (ch == 'q' || ch == 'Q' || g_quit) goto done;
            } while (!is_game_key(ch));
            game_set_turn(&game, key_to_turn(game.dir, ch));
            game_tick(&game);
        } else {
            timeout(speed_ms);
            int ch = getch();
            if (ch == 'q' || ch == 'Q' || g_quit) goto done;
            if (ch != ERR) game_set_turn(&game, key_to_turn(game.dir, ch));
            game_tick(&game);
        }
    }

done:
    if (!game.alive) {
        erase();
        draw_game(&game, NULL);
        int sy = game.height + 4;
        attron(A_BOLD | A_REVERSE);
        mvprintw(sy, 0, " GAME OVER!  Score: %d  Ticks: %d ",
                 game.score, game.tick);
        attroff(A_BOLD | A_REVERSE);
        mvprintw(sy + 1, 0, "Press any key...");
        refresh();
        timeout(-1);
        getch();
    }
    timeout(-1);
}

/* ═══════════════════════════════════════════════════════════════════
 * PLAY SETTINGS FORM
 * ═══════════════════════════════════════════════════════════════════ */

static void show_play_config(void) {
    static const char *tick_opts[] = { "Timed", "Step" };

    FField ff[] = {
        { "Width",     1, 20, 5, 100, 1, 5, NULL, 0, NULL, "", 1 },
        { "Height",    1, 20, 5, 100, 1, 5, NULL, 0, NULL, "", 1 },
        { "Speed",     1, 200, 10, 5000, 10, 50, NULL, 0, "ms", "", 1 },
        { "Seed",      1, 0, 0, 999999, 1, 100, NULL, 0, NULL, "", 1 },
        { "Tick Mode", 0, 0, 0, 0, 0, 0, tick_opts, 2, NULL, "", 1 },
    };
    int nf = sizeof ff / sizeof ff[0];

    if (show_form("PLAY SETTINGS", ff, nf, "PgUp/PgDn or </>: big steps")) {
        run_play(ff[0].val, ff[1].val, ff[2].val,
                 (unsigned)ff[3].val, ff[4].val);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * EXTERNAL AGENT SUB-PROCESS
 * ═══════════════════════════════════════════════════════════════════ */

static void resolve_agent_cmd(const AgentEntry *ae, char *out, size_t sz) {
    /* The command from agent.json is run from the agent's own directory.
     * Build: cd <dir> && <command> */
    if (ae->dir[0])
        snprintf(out, sz, "cd '%.300s' && %s", ae->dir, ae->command);
    else
        snprintf(out, sz, "%s", ae->command);
}

static ExtAgent *start_ext_agent(const char *cmd) {
    int pin[2], pout[2];
    if (pipe(pin) < 0 || pipe(pout) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(pin[0]); close(pin[1]); close(pout[0]); close(pout[1]); return NULL; }

    if (pid == 0) {
        /* child */
        close(pin[1]);  close(pout[0]);
        dup2(pin[0],  STDIN_FILENO);
        dup2(pout[1], STDOUT_FILENO);
        close(pin[0]); close(pout[1]);
        freopen("/dev/null", "w", stderr);
        execlp("/bin/sh", "sh", "-c", cmd, NULL);
        _exit(1);
    }

    close(pin[0]); close(pout[1]);

    ExtAgent *ea = calloc(1, sizeof *ea);
    ea->pid   = pid;
    ea->wr    = fdopen(pin[1], "w");
    ea->rd    = fdopen(pout[0], "r");
    ea->rd_fd = pout[0];
    setvbuf(ea->wr, NULL, _IOLBF, 0);
    setvbuf(ea->rd, NULL, _IOLBF, 0);
    return ea;
}

static Turn read_ext_turn(ExtAgent *ea, int timeout_ms) {
    struct pollfd pfd = { .fd = ea->rd_fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r > 0 && (pfd.revents & POLLIN)) {
        char buf[64];
        if (fgets(buf, sizeof buf, ea->rd))
            return parse_turn(buf);
    }
    return TURN_STRAIGHT;
}

static void stop_ext_agent(ExtAgent *ea) {
    if (!ea) return;
    if (ea->wr) fclose(ea->wr);
    if (ea->rd) fclose(ea->rd);
    kill(ea->pid, SIGTERM);
    waitpid(ea->pid, NULL, 0);
    free(ea);
}

/* ═══════════════════════════════════════════════════════════════════
 * AGENT BATCH RUNNER
 * ═══════════════════════════════════════════════════════════════════ */

static void run_agent_batch(const AgentCfg *cfg, BatchResult *br) {
    br->results   = calloc((size_t)cfg->num_games, sizeof(GameResult));
    br->count     = cfg->num_games;
    br->completed = 0;
    br->log_path[0] = '\0';

    /* open log file if requested */
    FILE *logfp = NULL;
    if (cfg->do_log) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        snprintf(br->log_path, sizeof br->log_path,
                 "snake_log_%04d%02d%02d_%02d%02d%02d.jsonl",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        logfp = fopen(br->log_path, "w");
    }

    /* resolve external agent command */
    const AgentEntry *ae = &g_agents[cfg->agent_idx];
    char ext_cmd[512] = "";
    if (!ae->is_builtin) {
        if (ae->is_custom)
            snprintf(ext_cmd, sizeof ext_cmd, "%.*s",
                     (int)(sizeof ext_cmd - 1), cfg->cmd);
        else
            resolve_agent_cmd(ae, ext_cmd, sizeof ext_cmd);
    }

    g_abort = 0;
    double t0 = now_sec();

    static SnakeGame game;

    for (int g = 0; g < cfg->num_games && !g_abort; g++) {
        unsigned int seed = cfg->seed ? cfg->seed + (unsigned)g : 0;
        game_init(&game, cfg->width, cfg->height, seed);

        /* start external agent for this game */
        ExtAgent *ea = NULL;
        if (!ae->is_builtin) {
            ea = start_ext_agent(ext_cmd);
            if (!ea) {
                /* show error briefly */
                erase();
                mvprintw(LINES / 2, 2,
                         "Error: could not start agent: %s", ext_cmd);
                mvprintw(LINES / 2 + 1, 2, "Press any key...");
                refresh(); timeout(-1); getch();
                break;
            }
        }

        double gt0 = now_sec();
        int paused = 0;

        while (game.alive && !g_abort) {
            /* ── Decide ───────────────────────────────────── */
            Turn t;
            if (ae->is_builtin) {
                t = naive_agent_decide(&game);
            } else {
                write_state_json(ea->wr, &game);
                t = read_ext_turn(ea, 5000); /* 5s timeout */
            }

            /* ── Log ──────────────────────────────────────── */
            if (logfp) {
                fprintf(logfp,
                    "{\"game\":%d,\"tick\":%d,\"action\":\"%s\","
                    "\"score\":%d,\"dir\":\"%s\","
                    "\"head\":[%d,%d],\"food\":[%d,%d],\"alive\":true}\n",
                    g + 1, game.tick, turn_name(t),
                    game.score, direction_name(game.dir),
                    game.body[game.head_idx].x, game.body[game.head_idx].y,
                    game.food.x, game.food.y);
            }

            /* ── Apply ────────────────────────────────────── */
            game_set_turn(&game, t);
            game_tick(&game);

            /* ── Render / Progress ────────────────────────── */
            if (cfg->watch) {
                erase();
                char info[128];
                snprintf(info, sizeof info,
                         "Game %d/%d  [%s]",
                         g + 1, cfg->num_games,
                         g_agents[cfg->agent_idx].name);
                draw_game(&game, info);

                int sy = game.height + 4;
                mvprintw(sy, 0,
                    "Q/Esc: Abort  N: Next game  Space: Pause"
                    "  +/-: Speed (%d ms)", cfg->speed_ms);
                refresh();

                /* handle input using timeout for frame pacing */
                int delay = ((AgentCfg *)cfg)->speed_ms;
                if (paused) delay = -1; /* block until key */

                timeout(delay);
                int ch = getch();
                timeout(-1);

                if (ch == 'q' || ch == 'Q' || ch == 27) { g_abort = 1; break; }
                if (ch == 'n' || ch == 'N') break; /* skip to next game */
                if (ch == ' ') paused = !paused;
                if (ch == '+' || ch == '=')
                    ((AgentCfg *)cfg)->speed_ms = clamp(cfg->speed_ms - 20, 10, 5000);
                if (ch == '-' || ch == '_')
                    ((AgentCfg *)cfg)->speed_ms = clamp(cfg->speed_ms + 20, 10, 5000);
            } else {
                /* no-watch: show progress periodically */
                if (game.tick % 100 == 0 || !game.alive) {
                    erase();
                    int bw = 52, bh = 14;
                    int bx = (COLS - bw) / 2, by = (LINES - bh) / 2;

                    if (g_color) attron(COLOR_PAIR(CP_BORDER));
                    draw_box(by, bx, bw, bh);
                    draw_hsep(by + 2, bx, bw);
                    if (g_color) attroff(COLOR_PAIR(CP_BORDER));

                    if (g_color) attron(COLOR_PAIR(CP_TITLE));
                    attron(A_BOLD);
                    center_str(by + 1, bx, bw, "RUNNING AGENT");
                    attroff(A_BOLD);
                    if (g_color) attroff(COLOR_PAIR(CP_TITLE));

                    int r = by + 3;
                    mvprintw(r++, bx + 3, "Agent:  %s", g_agents[cfg->agent_idx].name);
                    mvprintw(r++, bx + 3, "Grid:   %d x %d", cfg->width, cfg->height);
                    r++;
                    mvprintw(r++, bx + 3, "Game %d of %d  (tick %d)",
                             g + 1, cfg->num_games, game.tick);

                    /* progress bar */
                    int pct = (g * 100) / cfg->num_games;
                    int barw = bw - 14;
                    int fill = (barw * pct) / 100;
                    mvprintw(r, bx + 3, "[");
                    for (int i = 0; i < barw; i++) {
                        if (g_color) attron(COLOR_PAIR(i < fill ? CP_GOOD : CP_DIM));
                        addch(i < fill ? '#' : '.');
                        if (g_color) attroff(COLOR_PAIR(i < fill ? CP_GOOD : CP_DIM));
                    }
                    printw("] %d%%", pct);
                    r++;

                    if (g > 0) {
                        r++;
                        double sum_s = 0; int sum_sc = 0, best_sc = 0;
                        for (int i = 0; i < g; i++) {
                            sum_s += br->results[i].elapsed;
                            sum_sc += br->results[i].score;
                            if (br->results[i].score > best_sc)
                                best_sc = br->results[i].score;
                        }
                        mvprintw(r++, bx + 3, "Avg Score: %.1f   Best: %d",
                                 (double)sum_sc / g, best_sc);
                        mvprintw(r++, bx + 3, "Elapsed: %.1f s", now_sec() - t0);
                    }

                    mvprintw(by + bh - 1, bx + 3, "Press Q to abort");
                    refresh();

                    /* quick non-blocking check for 'q' */
                    timeout(0);
                    int ch = getch();
                    timeout(-1);
                    if (ch == 'q' || ch == 'Q' || ch == 27) { g_abort = 1; break; }
                }
            }
        }

        /* log final state */
        if (logfp) {
            fprintf(logfp,
                "{\"game\":%d,\"tick\":%d,\"action\":null,"
                "\"score\":%d,\"dir\":\"%s\","
                "\"head\":[%d,%d],\"food\":[%d,%d],\"alive\":false}\n",
                g + 1, game.tick,
                game.score, direction_name(game.dir),
                game.body[game.head_idx].x, game.body[game.head_idx].y,
                game.food.x, game.food.y);
        }

        /* send dead state to external agent, then stop it */
        if (ea) {
            write_state_json(ea->wr, &game);
            stop_ext_agent(ea);
            ea = NULL;
        }

        br->results[g].score   = game.score;
        br->results[g].ticks   = game.tick;
        br->results[g].elapsed = now_sec() - gt0;
        br->completed = g + 1;
    }

    br->total_time = now_sec() - t0;
    if (logfp) fclose(logfp);
}

/* ═══════════════════════════════════════════════════════════════════
 * STATISTICS HELPERS
 * ═══════════════════════════════════════════════════════════════════ */

static int int_cmp(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static double calc_median(const int *arr, int n) {
    int *tmp = malloc((size_t)n * sizeof(int));
    memcpy(tmp, arr, (size_t)n * sizeof(int));
    qsort(tmp, (size_t)n, sizeof(int), int_cmp);
    double m = (n % 2) ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2.0;
    free(tmp);
    return m;
}

static double calc_stddev(const int *arr, int n, double mean) {
    double sum = 0;
    for (int i = 0; i < n; i++) {
        double d = arr[i] - mean;
        sum += d * d;
    }
    return sqrt(sum / n);
}

/* ═══════════════════════════════════════════════════════════════════
 * SUMMARY SCREEN
 * Returns: 0=back to menu  1=run again  2=reconfigure
 * ═══════════════════════════════════════════════════════════════════ */

static int show_summary(const AgentCfg *cfg, const BatchResult *br) {
    int n = br->completed;
    if (n <= 0) return 0;

    /* gather stats */
    int *scores = malloc((size_t)n * sizeof(int));
    int *ticks  = malloc((size_t)n * sizeof(int));
    int sum_sc = 0, sum_tk = 0, max_sc = 0, min_sc = scores ? scores[0] : 0;
    int max_tk = 0, min_tk = 0;

    for (int i = 0; i < n; i++) {
        scores[i] = br->results[i].score;
        ticks[i]  = br->results[i].ticks;
        sum_sc += scores[i]; sum_tk += ticks[i];
        if (i == 0 || scores[i] > max_sc) max_sc = scores[i];
        if (i == 0 || scores[i] < min_sc) min_sc = scores[i];
        if (i == 0 || ticks[i]  > max_tk) max_tk = ticks[i];
        if (i == 0 || ticks[i]  < min_tk) min_tk = ticks[i];
    }
    double avg_sc = (double)sum_sc / n;
    double avg_tk = (double)sum_tk / n;
    double med_sc = calc_median(scores, n);
    double med_tk = calc_median(ticks, n);
    double std_sc = calc_stddev(scores, n, avg_sc);
    double std_tk = calc_stddev(ticks, n, avg_tk);

    int scroll = 0;          /* scroll offset for per-game table */
    const int tbl_rows = 5;  /* visible rows in the table */

    while (!g_quit) {
        erase();

        int bw = 56;
        /* top stats area: ~16 rows, per-game table: tbl_rows + 2, controls: 3 */
        int stats_h = 16;
        int bh = stats_h + tbl_rows + 5;
        int bx = (COLS - bw) / 2, by = 0;
        if (LINES > bh + 2) by = (LINES - bh) / 2;

        if (g_color) attron(COLOR_PAIR(CP_BORDER));
        draw_box(by, bx, bw, bh);
        draw_hsep(by + 2, bx, bw);
        if (g_color) attroff(COLOR_PAIR(CP_BORDER));

        if (g_color) attron(COLOR_PAIR(CP_TITLE));
        attron(A_BOLD);
        center_str(by + 1, bx, bw, "AGENT RESULTS");
        attroff(A_BOLD);
        if (g_color) attroff(COLOR_PAIR(CP_TITLE));

        int r = by + 3;
        mvprintw(r++, bx + 3, "Agent:    %s", g_agents[cfg->agent_idx].name);
        mvprintw(r++, bx + 3, "Games:    %d of %d     Duration: %.1f s",
                 n, cfg->num_games, br->total_time);
        mvprintw(r++, bx + 3, "Grid:     %d x %d      Speed: %d ms",
                 cfg->width, cfg->height, cfg->speed_ms);
        r++;

        /* scores */
        if (g_color) attron(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "-- Scores %-35s", "---");
        if (g_color) attroff(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "Avg: %-8.1f Max: %-6d Min: %-6d",
                 avg_sc, max_sc, min_sc);
        mvprintw(r++, bx + 3, "Med: %-8.1f Std: %-6.1f", med_sc, std_sc);
        r++;

        /* ticks */
        if (g_color) attron(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "-- Ticks  %-35s", "---");
        if (g_color) attroff(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "Avg: %-8.1f Max: %-6d Min: %-6d",
                 avg_tk, max_tk, min_tk);
        mvprintw(r++, bx + 3, "Med: %-8.1f Std: %-6.1f", med_tk, std_tk);
        r++;

        /* log path */
        if (br->log_path[0]) {
            if (g_color) attron(COLOR_PAIR(CP_GOOD));
            mvprintw(r, bx + 3, "Log: %s", br->log_path);
            if (g_color) attroff(COLOR_PAIR(CP_GOOD));
        }
        r++;

        /* per-game table separator */
        if (g_color) attron(COLOR_PAIR(CP_BORDER));
        draw_hsep(r, bx, bw);
        if (g_color) attroff(COLOR_PAIR(CP_BORDER));
        r++;

        /* table header */
        attron(A_BOLD);
        mvprintw(r++, bx + 3, " #     Score    Ticks     Time");
        attroff(A_BOLD);

        /* table rows */
        for (int i = 0; i < tbl_rows && scroll + i < n; i++) {
            int idx = scroll + i;
            mvprintw(r + i, bx + 3, " %-5d  %-6d   %-7d   %.3f s",
                     idx + 1,
                     br->results[idx].score,
                     br->results[idx].ticks,
                     br->results[idx].elapsed);
        }
        r += tbl_rows;

        /* scroll indicator */
        if (n > tbl_rows) {
            if (g_color) attron(COLOR_PAIR(CP_DIM));
            mvprintw(r, bx + 3, "Showing %d-%d of %d (Up/Down to scroll)",
                     scroll + 1,
                     (scroll + tbl_rows < n) ? scroll + tbl_rows : n,
                     n);
            if (g_color) attroff(COLOR_PAIR(CP_DIM));
        }

        /* controls */
        if (g_color) attron(COLOR_PAIR(CP_DIM));
        mvprintw(LINES - 1, 1,
                 "S: Save results  R: Run again  C: Reconfigure  Q/Esc: Menu");
        if (g_color) attroff(COLOR_PAIR(CP_DIM));

        refresh();
        timeout(-1);
        int ch = getch();

        if (ch == KEY_UP && scroll > 0) { scroll--; continue; }
        if (ch == KEY_DOWN && scroll + tbl_rows < n) { scroll++; continue; }
        if (ch == KEY_PPAGE) { scroll = clamp(scroll - tbl_rows, 0, n - 1); continue; }
        if (ch == KEY_NPAGE) { scroll = clamp(scroll + tbl_rows, 0, n > tbl_rows ? n - tbl_rows : 0); continue; }

        if (ch == 's' || ch == 'S') {
            /* save summary to file */
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char fn[256];
            snprintf(fn, sizeof fn,
                     "snake_summary_%04d%02d%02d_%02d%02d%02d.txt",
                     tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
            FILE *f = fopen(fn, "w");
            if (f) {
                fprintf(f, "Snake Agent Run Summary\n");
                fprintf(f, "=======================\n\n");
                fprintf(f, "Agent:     %s\n", g_agents[cfg->agent_idx].name);
                fprintf(f, "Games:     %d of %d\n", n, cfg->num_games);
                fprintf(f, "Grid:      %d x %d\n", cfg->width, cfg->height);
                fprintf(f, "Speed:     %d ms\n", cfg->speed_ms);
                fprintf(f, "Tick Mode: %s\n", cfg->step ? "Step" : "Timed");
                fprintf(f, "Duration:  %.1f s\n\n", br->total_time);

                fprintf(f, "Scores\n");
                fprintf(f, "  Average:  %.1f\n", avg_sc);
                fprintf(f, "  Maximum:  %d\n", max_sc);
                fprintf(f, "  Minimum:  %d\n", min_sc);
                fprintf(f, "  Median:   %.1f\n", med_sc);
                fprintf(f, "  Std Dev:  %.1f\n\n", std_sc);

                fprintf(f, "Ticks\n");
                fprintf(f, "  Average:  %.1f\n", avg_tk);
                fprintf(f, "  Maximum:  %d\n", max_tk);
                fprintf(f, "  Minimum:  %d\n", min_tk);
                fprintf(f, "  Median:   %.1f\n", med_tk);
                fprintf(f, "  Std Dev:  %.1f\n\n", std_tk);

                if (br->log_path[0])
                    fprintf(f, "Action Log: %s\n\n", br->log_path);

                fprintf(f, "Per-game Results\n");
                fprintf(f, "#\tScore\tTicks\tTime (s)\n");
                for (int i = 0; i < n; i++)
                    fprintf(f, "%d\t%d\t%d\t%.3f\n",
                            i + 1, br->results[i].score,
                            br->results[i].ticks,
                            br->results[i].elapsed);
                fclose(f);

                /* show confirmation */
                attron(A_BOLD);
                mvprintw(LINES - 2, 1, "Saved: %s", fn);
                attroff(A_BOLD);
                clrtoeol();
                refresh();
                timeout(2000); getch(); timeout(-1);
            }
            continue;
        }
        if (ch == 'r' || ch == 'R') { free(scores); free(ticks); return 1; }
        if (ch == 'c' || ch == 'C') { free(scores); free(ticks); return 2; }
        if (ch == 'q' || ch == 'Q' || ch == 27) { free(scores); free(ticks); return 0; }
    }

    free(scores); free(ticks);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * AGENT CONFIG FORM
 * ═══════════════════════════════════════════════════════════════════ */

static void agent_mode(void) {
    static const char *tick_opts[]  = { "Timed", "Step" };
    static const char *yesno_opts[] = { "No", "Yes" };

    /* Field indices */
    enum {
        F_AGENT, F_CMD, F_GAMES, F_WIDTH, F_HEIGHT,
        F_SPEED, F_SEED, F_TICK, F_WATCH, F_LOG, F_COUNT
    };

    FField ff[F_COUNT];
    memset(ff, 0, sizeof ff);

    ff[F_AGENT]  = (FField){ "Agent",     0, 0, 0, g_num_agents - 1, 1, 1,
                             g_agent_names, g_num_agents, NULL, "", 1 };
    ff[F_CMD]    = (FField){ "Command",   2, 0, 0, 0, 0, 0,
                             NULL, 0, NULL, "", 0 };  /* hidden initially */
    ff[F_GAMES]  = (FField){ "Games",     1, 1, 1, 100000, 1, 10,
                             NULL, 0, NULL, "", 1 };
    ff[F_WIDTH]  = (FField){ "Width",     1, 20, 5, 100, 1, 5,
                             NULL, 0, NULL, "", 1 };
    ff[F_HEIGHT] = (FField){ "Height",    1, 20, 5, 100, 1, 5,
                             NULL, 0, NULL, "", 1 };
    ff[F_SPEED]  = (FField){ "Speed",     1, 200, 10, 5000, 10, 50,
                             NULL, 0, "ms", "", 1 };
    ff[F_SEED]   = (FField){ "Seed",      1, 0, 0, 999999, 1, 100,
                             NULL, 0, NULL, "", 1 };
    ff[F_TICK]   = (FField){ "Tick Mode", 0, 0, 0, 1, 0, 0,
                             tick_opts, 2, NULL, "", 1 };
    ff[F_WATCH]  = (FField){ "Watch",     0, 1, 0, 1, 0, 0,
                             yesno_opts, 2, NULL, "", 1 };
    ff[F_LOG]    = (FField){ "Log Actions", 0, 0, 0, 1, 0, 0,
                             yesno_opts, 2, NULL, "", 1 };

    while (1) {
        /* update command field visibility */
        ff[F_CMD].visible = g_agents[ff[F_AGENT].val].is_custom;

        if (!show_form("AGENT CONFIGURATION", ff, F_COUNT,
                       "PgUp/PgDn or </>: big steps  Digits: type value"))
            return;  /* user pressed Esc */

        /* build config */
        AgentCfg ac;
        memset(&ac, 0, sizeof ac);
        ac.agent_idx = ff[F_AGENT].val;
        ac.num_games = ff[F_GAMES].val;
        ac.width     = ff[F_WIDTH].val;
        ac.height    = ff[F_HEIGHT].val;
        ac.speed_ms  = ff[F_SPEED].val;
        ac.seed      = (unsigned)ff[F_SEED].val;
        ac.step      = ff[F_TICK].val;
        ac.watch     = ff[F_WATCH].val;
        ac.do_log    = ff[F_LOG].val;
        if (g_agents[ac.agent_idx].is_custom)
            snprintf(ac.cmd, sizeof ac.cmd, "%.*s",
                     (int)(sizeof ac.cmd - 1), ff[F_CMD].text);

        /* run loop: run → summary → run again / reconfigure / menu */
        while (1) {
            BatchResult br;
            memset(&br, 0, sizeof br);
            run_agent_batch(&ac, &br);
            int action = show_summary(&ac, &br);
            free(br.results);

            if (action == 1) continue;       /* run again */
            if (action == 2) break;          /* reconfigure → show form again */
            return;                          /* back to main menu */
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * CLI PARSING & MAIN
 * ═══════════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog) {
    printf(
        "Usage: %s [play|agent] [options]\n\n"
        "No arguments → interactive menu.\n\n"
        "Modes:\n"
        "  play      Interactive TUI\n"
        "  agent     JSON line-protocol on stdin/stdout\n\n"
        "Options:\n"
        "  --step        Advance per input  (default for agent)\n"
        "  --timed       Advance per timer  (default for play)\n"
        "  --speed  N    Tick interval ms   (default: 200)\n"
        "  --width  N    Grid width         (default: 20)\n"
        "  --height N    Grid height        (default: 20)\n"
        "  --seed   N    Random seed        (default: 0 = time)\n"
        "  --help        Show this message\n",
        prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    resolve_exe_dir(argv[0]);
    init_agent_registry();

    /* ── Detect CLI mode ────────────────────────────────────── */
    RunMode mode = RUN_MENU;
    int width = 20, height = 20, speed_ms = 200, step = -1;
    unsigned int seed = 0;

    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        if      (strcmp(argv[i], "play")  == 0) mode = RUN_PLAY;
        else if (strcmp(argv[i], "agent") == 0) mode = RUN_AGENT_PROTO;
        else { fprintf(stderr, "Unknown mode: %s\n", argv[i]); print_usage(argv[0]); return 1; }
        i++;
    }
    for (; i < argc; i++) {
        if      (strcmp(argv[i], "--step")  == 0) step = 1;
        else if (strcmp(argv[i], "--timed") == 0) step = 0;
        else if (strcmp(argv[i], "--speed")  == 0 && i + 1 < argc) speed_ms = atoi(argv[++i]);
        else if (strcmp(argv[i], "--width")  == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed")   == 0 && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]); return 1;
        }
    }

    /* validate dimensions */
    if (width < 5 || width > MAX_GRID_DIM || height < 5 || height > MAX_GRID_DIM) {
        fprintf(stderr, "Grid dimensions must be in 5..%d\n", MAX_GRID_DIM);
        return 1;
    }

    /* ── Agent protocol mode (no ncurses) ───────────────────── */
    if (mode == RUN_AGENT_PROTO) {
        if (step == -1) step = 1;
        run_agent_proto(width, height, seed, step, speed_ms);
        return 0;
    }

    /* ── ncurses modes ──────────────────────────────────────── */
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    init_colors();

    if (mode == RUN_PLAY) {
        if (step == -1) step = 0;
        run_play(width, height, speed_ms, seed, step);
    } else {
        /* interactive menu loop */
        while (!g_quit) {
            int choice = show_main_menu();
            switch (choice) {
            case 0: show_play_config(); break;
            case 1: agent_mode();       break;
            case 2: goto quit;
            default: goto quit;
            }
        }
    }

quit:
    endwin();
    return 0;
}
