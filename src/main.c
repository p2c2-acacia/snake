/*
 * main.c – Entry point for the Snake game.
 *
 * Two run modes:
 *   play   (default)  Interactive ncurses TUI; human plays with arrow keys / WASD.
 *   agent             JSON line-protocol on stdin/stdout for programmatic agents.
 *
 * Two tick modes:
 *   --timed  (default for play)   Game advances every --speed ms.
 *   --step   (default for agent)  Game advances only after receiving input.
 */

#include "game.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <ncurses.h>
#include <poll.h>
#include <unistd.h>

/* ── Run modes ──────────────────────────────────────────────── */
typedef enum { MODE_PLAY, MODE_AGENT } RunMode;

/* ── Immutable game settings (fixed before the game starts) ── */
typedef struct {
    RunMode      mode;
    int          step;        /* 1 = step-per-input, 0 = time-based       */
    int          speed_ms;    /* tick interval in ms (time-based mode)     */
    int          width;
    int          height;
    unsigned int seed;        /* 0 → derive from time()                   */
} Settings;

/* ── Signal handling ────────────────────────────────────────── */
static volatile sig_atomic_t g_quit = 0;
static void handle_signal(int sig) { (void)sig; g_quit = 1; }

/* ════════════════════════════════════════════════════════════════
 * AGENT MODE – JSON line protocol on stdin / stdout
 * ════════════════════════════════════════════════════════════════ */

/** Write the full game state as a single JSON line to @p out. */
static void write_state(FILE *out, const SnakeGame *g) {
    fprintf(out,
        "{\"tick\":%d,\"alive\":%s,\"score\":%d,"
        "\"width\":%d,\"height\":%d,\"dir\":\"%s\",\"snake\":[",
        g->tick,
        g->alive ? "true" : "false",
        g->score,
        g->width, g->height,
        direction_name(g->dir));

    for (int i = 0; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        fprintf(out, "%s[%d,%d]", i ? "," : "",
                g->body[idx].x, g->body[idx].y);
    }

    fprintf(out, "],\"food\":[%d,%d]}\n", g->food.x, g->food.y);
    fflush(out);
}

/** Blocking read of a single turn command from stdin.
 *  Sets g_quit on EOF / error so the game loop terminates. */
static Turn read_command_blocking(void) {
    char buf[64];
    if (!fgets(buf, sizeof buf, stdin)) { g_quit = 1; return TURN_STRAIGHT; }
    return parse_turn(buf);
}

/** Read a turn command with a timeout (ms).  Returns TURN_STRAIGHT on timeout.
 *  Sets g_quit on EOF / error. */
static Turn read_command_timed(int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        return read_command_blocking();
    }
    if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR))) {
        g_quit = 1;
    }
    return TURN_STRAIGHT;
}

static void run_agent(SnakeGame *g, const Settings *s) {
    /* Use line-buffered I/O for prompt inter-process communication. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin,  NULL, _IOLBF, 0);

    while (g->alive && !g_quit) {
        write_state(stdout, g);

        Turn t;
        if (s->step)
            t = read_command_blocking();
        else
            t = read_command_timed(s->speed_ms);

        if (g_quit) break;

        game_set_turn(g, t);
        game_tick(g);
    }

    /* Always emit the final state so the agent knows the result. */
    write_state(stdout, g);
}

/* ════════════════════════════════════════════════════════════════
 * INTERACTIVE TUI – ncurses
 * ════════════════════════════════════════════════════════════════ */

/* Colour pair IDs */
#define CP_SNAKE  1
#define CP_HEAD   2
#define CP_FOOD   3
#define CP_BORDER 4
#define CP_INFO   5

static int g_has_color = 0;

static void tui_init_colors(void) {
    if (has_colors()) {
        start_color();
        use_default_colors();
        g_has_color = 1;
        init_pair(CP_SNAKE,  COLOR_GREEN,   -1);
        init_pair(CP_HEAD,   COLOR_YELLOW,  -1);
        init_pair(CP_FOOD,   COLOR_RED,     -1);
        init_pair(CP_BORDER, COLOR_WHITE,   -1);
        init_pair(CP_INFO,   COLOR_CYAN,    -1);
    }
}

/** Map an absolute arrow/WASD key to a relative Turn. */
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

/** Return 1 if @p ch is a valid game-input key. */
static int is_game_key(int ch) {
    switch (ch) {
    case KEY_UP: case KEY_DOWN: case KEY_LEFT: case KEY_RIGHT:
    case 'w': case 'W': case 'a': case 'A':
    case 's': case 'S': case 'd': case 'D':
    case ' ': case '\n': case KEY_ENTER:
        return 1;
    default:
        return 0;
    }
}

static void tui_draw(const SnakeGame *g, const Settings *s) {
    erase();

    const int oy = 1;   /* vertical offset: leave row 0 for the title bar */

    /* ── Title bar ─────────────────────────────────────────── */
    if (g_has_color) attron(COLOR_PAIR(CP_INFO));
    attron(A_BOLD);
    mvprintw(0, 0, " SNAKE ");
    attroff(A_BOLD);
    printw("  Score: %-4d  Tick: %-6d  Dir: %-5s", g->score, g->tick, direction_name(g->dir));
    if (g_has_color) attroff(COLOR_PAIR(CP_INFO));

    /* ── Border ────────────────────────────────────────────── */
    if (g_has_color) attron(COLOR_PAIR(CP_BORDER));
    mvaddch(oy, 0, ACS_ULCORNER);
    for (int x = 0; x < g->width; x++) mvaddch(oy, x + 1, ACS_HLINE);
    mvaddch(oy, g->width + 1, ACS_URCORNER);

    for (int y = 0; y < g->height; y++) {
        mvaddch(oy + y + 1, 0,             ACS_VLINE);
        mvaddch(oy + y + 1, g->width + 1,  ACS_VLINE);
    }

    mvaddch(oy + g->height + 1, 0, ACS_LLCORNER);
    for (int x = 0; x < g->width; x++) mvaddch(oy + g->height + 1, x + 1, ACS_HLINE);
    mvaddch(oy + g->height + 1, g->width + 1, ACS_LRCORNER);
    if (g_has_color) attroff(COLOR_PAIR(CP_BORDER));

    /* ── Food ──────────────────────────────────────────────── */
    if (g->food.x >= 0) {
        if (g_has_color) attron(COLOR_PAIR(CP_FOOD) | A_BOLD);
        mvaddch(oy + g->food.y + 1, g->food.x + 1, '*');
        if (g_has_color) attroff(COLOR_PAIR(CP_FOOD) | A_BOLD);
    }

    /* ── Snake body (skip head, drawn separately) ──────────── */
    if (g_has_color) attron(COLOR_PAIR(CP_SNAKE));
    for (int i = 1; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        mvaddch(oy + g->body[idx].y + 1, g->body[idx].x + 1, 'o');
    }
    if (g_has_color) attroff(COLOR_PAIR(CP_SNAKE));

    /* ── Snake head ────────────────────────────────────────── */
    {
        Point h = g->body[g->head_idx];
        if (g_has_color) attron(COLOR_PAIR(CP_HEAD) | A_BOLD);
        mvaddch(oy + h.y + 1, h.x + 1, '@');
        if (g_has_color) attroff(COLOR_PAIR(CP_HEAD) | A_BOLD);
    }

    /* ── Status lines ──────────────────────────────────────── */
    int sy = oy + g->height + 3;
    if (s->step)
        mvprintw(sy, 0, "Mode: Step (press a key to advance)");
    else
        mvprintw(sy, 0, "Mode: Timed (speed: %d ms)", s->speed_ms);
    mvprintw(sy + 1, 0, "Controls: Arrow keys / WASD  |  Space/Enter: straight  |  Q: quit");

    /* ── Game-over overlay ─────────────────────────────────── */
    if (!g->alive) {
        attron(A_BOLD | A_REVERSE);
        mvprintw(sy + 3, 0, " GAME OVER!  Final score: %d  Ticks survived: %d ", g->score, g->tick);
        attroff(A_BOLD | A_REVERSE);
        mvprintw(sy + 4, 0, "Press any key to exit ...");
    }

    refresh();
}

static void run_play(SnakeGame *g, const Settings *s) {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    tui_init_colors();

    while (g->alive && !g_quit) {
        tui_draw(g, s);

        if (s->step) {
            /* STEP mode: block until a valid game key is pressed. */
            timeout(-1);
            int ch;
            do {
                ch = getch();
                if (ch == 'q' || ch == 'Q' || g_quit) goto done;
            } while (!is_game_key(ch));

            game_set_turn(g, key_to_turn(g->dir, ch));
            game_tick(g);
        } else {
            /* TIMED mode: advance every speed_ms; read keys in between. */
            timeout(s->speed_ms);
            int ch = getch();

            if (ch == 'q' || ch == 'Q' || g_quit) goto done;

            if (ch != ERR) {
                game_set_turn(g, key_to_turn(g->dir, ch));
            }
            game_tick(g);
        }
    }

done:
    /* Show the final frame and wait for any key. */
    if (!g->alive) {
        timeout(-1);
        tui_draw(g, s);
        getch();
    }

    endwin();
    printf("Game over.  Score: %d  Ticks: %d\n", g->score, g->tick);
}

/* ════════════════════════════════════════════════════════════════
 * CLI PARSING & ENTRY POINT
 * ════════════════════════════════════════════════════════════════ */

static void print_usage(const char *prog) {
    printf(
        "Usage: %s [play|agent] [options]\n"
        "\n"
        "Modes:\n"
        "  play          Interactive ncurses TUI (default)\n"
        "  agent         JSON line-protocol on stdin/stdout\n"
        "\n"
        "Tick modes:\n"
        "  --step        Advance one tick per input (default for agent)\n"
        "  --timed       Advance every --speed ms   (default for play)\n"
        "\n"
        "Options:\n"
        "  --speed  N    Tick interval in ms       (default: 200)\n"
        "  --width  N    Grid width  in cells      (default: 20, range 5..%d)\n"
        "  --height N    Grid height in cells      (default: 20, range 5..%d)\n"
        "  --seed   N    Random seed (0 = time)    (default: 0)\n"
        "  --help        Show this message\n",
        prog, MAX_GRID_DIM, MAX_GRID_DIM);
}

int main(int argc, char *argv[]) {
    Settings s = {
        .mode     = MODE_PLAY,
        .step     = -1,            /* -1 = use mode default */
        .speed_ms = 200,
        .width    = 20,
        .height   = 20,
        .seed     = 0,
    };

    /* ── Parse positional mode argument ────────────────────── */
    int i = 1;
    if (i < argc && argv[i][0] != '-') {
        if      (strcmp(argv[i], "play")  == 0) s.mode = MODE_PLAY;
        else if (strcmp(argv[i], "agent") == 0) s.mode = MODE_AGENT;
        else {
            fprintf(stderr, "Unknown mode: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
        i++;
    }

    /* ── Parse flag / value options ────────────────────────── */
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--step") == 0) {
            s.step = 1;
        } else if (strcmp(argv[i], "--timed") == 0) {
            s.step = 0;
        } else if (strcmp(argv[i], "--speed") == 0 && i + 1 < argc) {
            s.speed_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            s.width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            s.height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            s.seed = (unsigned)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* ── Apply mode defaults ──────────────────────────────── */
    if (s.step == -1)
        s.step = (s.mode == MODE_AGENT) ? 1 : 0;

    /* ── Validate ─────────────────────────────────────────── */
    if (s.width < 5 || s.width > MAX_GRID_DIM ||
        s.height < 5 || s.height > MAX_GRID_DIM) {
        fprintf(stderr, "Grid dimensions must be in 5..%d\n", MAX_GRID_DIM);
        return 1;
    }
    if (s.speed_ms < 10) {
        fprintf(stderr, "Speed must be >= 10 ms\n");
        return 1;
    }

    /* ── Signals ──────────────────────────────────────────── */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* ── Initialise the game (static to avoid ~90 KB on stack) ─ */
    static SnakeGame game;
    game_init(&game, s.width, s.height, s.seed);

    /* ── Run ──────────────────────────────────────────────── */
    if (s.mode == MODE_AGENT)
        run_agent(&game, &s);
    else
        run_play(&game, &s);

    return 0;
}
