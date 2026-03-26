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
#define CP_APPLE     5
#define CP_BORDER   6
#define CP_INFO     7
#define CP_GOOD     8
#define CP_BAD      9
#define CP_DIM     10
#define CP_POISON  11
#define CP_WALL    12

/* ═══════════════════════════════════════════════════════════════════
 * TYPES
 * ═══════════════════════════════════════════════════════════════════ */

typedef enum { RUN_MENU, RUN_PLAY, RUN_AGENT_PROTO } RunMode;
typedef enum { OUTPUT_JSON = 0, OUTPUT_RAW_BOARD = 1 } OutputMode;

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
    int          difficulty_stage;
    OutputMode   output_mode;
    GameRules    rules;
} AgentCfg;
typedef struct {
    int    score;
    int    ticks;
    int    apples_eaten;
    GameOutcome outcome;
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

typedef struct {
    int          width;
    int          height;
    int          speed_ms;
    unsigned int seed;
    int          play_step;
    int          play_difficulty;
    int          agent_games;
    int          agent_watch;
    int          agent_log;
    int          agent_step;
    int          agent_difficulty;
    OutputMode   agent_output_mode;
    char         default_agent[64];
    GameRules    rules;

    /* Stage 8-12 tunables */
    int stage8_obstacles;
    int stage8_goal_apples;
    int stage9_poison;
    int stage9_obstacles;
    int stage9_goal_apples;
    int stage10_vision_radius;
    int stage10_goal_apples;
    int stage11_apple_decay;
    int stage11_obstacles;
    int stage11_tick_penalty;
    int stage11_goal_apples;
    int stage12_width;
    int stage12_height;
    int stage12_initial_snake_len;
    int stage12_poison;
    int stage12_obstacles;
    int stage12_vision_radius;
    int stage12_apple_decay;
    int stage12_goal_apples;
} AppSettings;

/* ═══════════════════════════════════════════════════════════════════
 * GLOBALS
 * ═══════════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t g_quit    = 0;
static volatile sig_atomic_t g_abort   = 0;   /* abort current batch */
static int                   g_color   = 0;
static char                  g_exe_dir[PATH_MAX] = ".";
static AppSettings           g_settings;
static unsigned long         g_agents_stamp = 0;

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

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (!isdigit((unsigned char)*p) && *p != '-') return 0;
    *out = (int)strtol(p, NULL, 10);
    return 1;
}

static int json_get_bool(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char pat[80];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (strncmp(p, "true", 4) == 0) { *out = 1; return 1; }
    if (strncmp(p, "false", 5) == 0) { *out = 0; return 1; }
    if (*p == '1') { *out = 1; return 1; }
    if (*p == '0') { *out = 0; return 1; }
    return 0;
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

static int parse_bool_text(const char *v) {
    if (!v || !*v) return 0;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 ||
        strcmp(v, "yes") == 0 || strcmp(v, "on") == 0)
        return 1;
    return 0;
}

static void init_settings_defaults(AppSettings *s) {
    memset(s, 0, sizeof(*s));
    game_rules_default(&s->rules);
    s->width = 20;
    s->height = 20;
    s->speed_ms = 200;
    s->seed = 0;
    s->play_step = 0;
    s->play_difficulty = 1;
    s->agent_games = 1;
    s->agent_watch = 1;
    s->agent_log = 0;
    s->agent_step = 1; /* static by default: waits for agent response */
    s->agent_difficulty = 1;
    s->agent_output_mode = OUTPUT_JSON;
    snprintf(s->default_agent, sizeof s->default_agent, "Built-in Naive");

    /* Stage 8-12 defaults */
    s->stage8_obstacles = 8;
    s->stage8_goal_apples = 15;
    s->stage9_poison = 3;
    s->stage9_obstacles = 6;
    s->stage9_goal_apples = 15;
    s->stage10_vision_radius = 5;
    s->stage10_goal_apples = 20;
    s->stage11_apple_decay = 25;
    s->stage11_obstacles = 6;
    s->stage11_tick_penalty = 15;
    s->stage11_goal_apples = 25;
    s->stage12_width = 10;
    s->stage12_height = 10;
    s->stage12_initial_snake_len = 8;
    s->stage12_poison = 3;
    s->stage12_obstacles = 4;
    s->stage12_vision_radius = 4;
    s->stage12_apple_decay = 20;
    s->stage12_goal_apples = 15;
}

static void parse_settings_text(AppSettings *s, const char *buf) {
    int iv = 0;
    char sv[128] = "";

    if (json_get_int(buf, "width", &iv)) s->width = clamp(iv, 5, MAX_GRID_DIM);
    if (json_get_int(buf, "height", &iv)) s->height = clamp(iv, 5, MAX_GRID_DIM);
    if (json_get_int(buf, "speed_ms", &iv)) s->speed_ms = clamp(iv, 10, 5000);
    if (json_get_int(buf, "seed", &iv)) s->seed = (unsigned)clamp(iv, 0, 999999999);

    if (json_get_str(buf, "play_tick_mode", sv, sizeof sv))
        s->play_step = (strcmp(sv, "step") == 0) ? 1 : 0;
    if (json_get_str(buf, "agent_tick_mode", sv, sizeof sv))
        s->agent_step = (strcmp(sv, "timed") == 0) ? 0 : 1;
    if (json_get_int(buf, "play_difficulty", &iv)) s->play_difficulty = clamp(iv, 1, 12);
    if (json_get_int(buf, "agent_difficulty", &iv)) s->agent_difficulty = clamp(iv, 1, 12);
    if (json_get_int(buf, "agent_games", &iv)) s->agent_games = clamp(iv, 1, 100000);

    if (json_get_bool(buf, "agent_watch", &iv)) {
        s->agent_watch = iv;
    } else if (json_get_str(buf, "agent_watch", sv, sizeof sv)) {
        s->agent_watch = parse_bool_text(sv);
    }
    if (json_get_bool(buf, "agent_log_actions", &iv)) {
        s->agent_log = iv;
    } else if (json_get_str(buf, "agent_log_actions", sv, sizeof sv)) {
        s->agent_log = parse_bool_text(sv);
    }
    if (json_get_str(buf, "agent_output_mode", sv, sizeof sv))
        s->agent_output_mode = (strcmp(sv, "raw_board") == 0)
                                   ? OUTPUT_RAW_BOARD
                                   : OUTPUT_JSON;
    if (json_get_str(buf, "default_agent", sv, sizeof sv))
        snprintf(s->default_agent, sizeof s->default_agent, "%s", sv);

    if (json_get_int(buf, "stage2_fixed_apples", &iv))
        s->rules.stage2_fixed_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage4_fixed_apples", &iv))
        s->rules.stage4_fixed_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage5_goal_apples", &iv))
        s->rules.stage5_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage6_fixed_apples", &iv))
        s->rules.stage6_fixed_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage6_goal_apples", &iv))
        s->rules.stage6_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage7_initial_apples", &iv))
        s->rules.stage7_initial_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage7_increment_every", &iv))
        s->rules.stage7_increment_every = clamp(iv, 1, 1000000);
    if (json_get_int(buf, "stage7_increment_by", &iv))
        s->rules.stage7_increment_by = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage7_max_apples", &iv))
        s->rules.stage7_max_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage7_goal_apples", &iv))
        s->rules.stage7_goal_apples = clamp(iv, 1, MAX_APPLES);

    /* Stage 8-12 tunables */
    if (json_get_int(buf, "stage8_obstacles", &iv)) s->stage8_obstacles = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage8_goal_apples", &iv)) s->stage8_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage9_poison", &iv)) s->stage9_poison = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage9_obstacles", &iv)) s->stage9_obstacles = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage9_goal_apples", &iv)) s->stage9_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage10_vision_radius", &iv)) s->stage10_vision_radius = clamp(iv, 1, 50);
    if (json_get_int(buf, "stage10_goal_apples", &iv)) s->stage10_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage11_apple_decay", &iv)) s->stage11_apple_decay = clamp(iv, 1, 1000);
    if (json_get_int(buf, "stage11_obstacles", &iv)) s->stage11_obstacles = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage11_tick_penalty", &iv)) s->stage11_tick_penalty = clamp(iv, 1, 1000);
    if (json_get_int(buf, "stage11_goal_apples", &iv)) s->stage11_goal_apples = clamp(iv, 1, MAX_APPLES);
    if (json_get_int(buf, "stage12_width", &iv)) s->stage12_width = clamp(iv, 5, MAX_GRID_DIM);
    if (json_get_int(buf, "stage12_height", &iv)) s->stage12_height = clamp(iv, 5, MAX_GRID_DIM);
    if (json_get_int(buf, "stage12_initial_snake_len", &iv)) s->stage12_initial_snake_len = clamp(iv, 1, 50);
    if (json_get_int(buf, "stage12_poison", &iv)) s->stage12_poison = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage12_obstacles", &iv)) s->stage12_obstacles = clamp(iv, 0, 50);
    if (json_get_int(buf, "stage12_vision_radius", &iv)) s->stage12_vision_radius = clamp(iv, 1, 50);
    if (json_get_int(buf, "stage12_apple_decay", &iv)) s->stage12_apple_decay = clamp(iv, 1, 1000);
    if (json_get_int(buf, "stage12_goal_apples", &iv)) s->stage12_goal_apples = clamp(iv, 1, MAX_APPLES);
}

static void write_default_settings_file(const char *path, const AppSettings *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
            "{\n"
            "  \"width\": %d,\n"
            "  \"height\": %d,\n"
            "  \"speed_ms\": %d,\n"
            "  \"seed\": %u,\n"
            "  \"play_tick_mode\": \"%s\",\n"
            "  \"agent_tick_mode\": \"%s\",\n"
            "  \"play_difficulty\": %d,\n"
            "  \"agent_difficulty\": %d,\n"
            "  \"agent_games\": %d,\n"
            "  \"agent_watch\": %s,\n"
            "  \"agent_log_actions\": %s,\n"
            "  \"agent_output_mode\": \"%s\",\n"
            "  \"default_agent\": \"%s\",\n"
            "  \"stage2_fixed_apples\": %d,\n"
            "  \"stage4_fixed_apples\": %d,\n"
            "  \"stage5_goal_apples\": %d,\n"
            "  \"stage6_fixed_apples\": %d,\n"
            "  \"stage6_goal_apples\": %d,\n"
            "  \"stage7_initial_apples\": %d,\n"
            "  \"stage7_increment_every\": %d,\n"
            "  \"stage7_increment_by\": %d,\n"
            "  \"stage7_max_apples\": %d,\n"
            "  \"stage7_goal_apples\": %d,\n"
            "  \"stage8_obstacles\": %d,\n"
            "  \"stage8_goal_apples\": %d,\n"
            "  \"stage9_poison\": %d,\n"
            "  \"stage9_obstacles\": %d,\n"
            "  \"stage9_goal_apples\": %d,\n"
            "  \"stage10_vision_radius\": %d,\n"
            "  \"stage10_goal_apples\": %d,\n"
            "  \"stage11_apple_decay\": %d,\n"
            "  \"stage11_obstacles\": %d,\n"
            "  \"stage11_tick_penalty\": %d,\n"
            "  \"stage11_goal_apples\": %d,\n"
            "  \"stage12_width\": %d,\n"
            "  \"stage12_height\": %d,\n"
            "  \"stage12_initial_snake_len\": %d,\n"
            "  \"stage12_poison\": %d,\n"
            "  \"stage12_obstacles\": %d,\n"
            "  \"stage12_vision_radius\": %d,\n"
            "  \"stage12_apple_decay\": %d,\n"
            "  \"stage12_goal_apples\": %d\n"
            "}\n",
            s->width, s->height, s->speed_ms, s->seed,
            s->play_step ? "step" : "timed",
            s->agent_step ? "step" : "timed",
            s->play_difficulty, s->agent_difficulty, s->agent_games,
            s->agent_watch ? "true" : "false",
            s->agent_log ? "true" : "false",
            s->agent_output_mode == OUTPUT_RAW_BOARD ? "raw_board" : "json",
            s->default_agent,
            s->rules.stage2_fixed_apples,
            s->rules.stage4_fixed_apples,
            s->rules.stage5_goal_apples,
            s->rules.stage6_fixed_apples,
            s->rules.stage6_goal_apples,
            s->rules.stage7_initial_apples,
            s->rules.stage7_increment_every,
            s->rules.stage7_increment_by,
            s->rules.stage7_max_apples,
            s->rules.stage7_goal_apples,
            s->stage8_obstacles,
            s->stage8_goal_apples,
            s->stage9_poison,
            s->stage9_obstacles,
            s->stage9_goal_apples,
            s->stage10_vision_radius,
            s->stage10_goal_apples,
            s->stage11_apple_decay,
            s->stage11_obstacles,
            s->stage11_tick_penalty,
            s->stage11_goal_apples,
            s->stage12_width,
            s->stage12_height,
            s->stage12_initial_snake_len,
            s->stage12_poison,
            s->stage12_obstacles,
            s->stage12_vision_radius,
            s->stage12_apple_decay,
            s->stage12_goal_apples);
    fclose(f);
}

static void load_settings(void) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%.900s/settings.json", g_exe_dir);

    init_settings_defaults(&g_settings);

    FILE *f = fopen(path, "r");
    if (!f) {
        write_default_settings_file(path, &g_settings);
        return;
    }

    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    parse_settings_text(&g_settings, buf);
}

static int settings_find_default_agent_idx(void) {
    for (int i = 0; i < g_num_agents; i++) {
        if (strcmp(g_agents[i].name, g_settings.default_agent) == 0)
            return i;
    }
    return 0;
}

static unsigned long fold_stamp(unsigned long cur, const struct stat *st) {
    if (!st) return cur;
    return (cur * 1315423911u) ^ (unsigned long)st->st_mtime ^ (unsigned long)st->st_size;
}

static unsigned long agents_dir_stamp(void) {
    struct stat st;
    char agents_dir[PATH_MAX];
    snprintf(agents_dir, sizeof agents_dir, "%.900s/agents", g_exe_dir);

    if (stat(agents_dir, &st) != 0) {
        snprintf(agents_dir, sizeof agents_dir, "agents");
        if (stat(agents_dir, &st) != 0) return 0;
    }

    unsigned long stamp = fold_stamp(0x9e3779b9u, &st);

    DIR *dp = opendir(agents_dir);
    if (!dp) return stamp;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subdir[PATH_MAX];
        snprintf(subdir, sizeof subdir, "%.800s/%s", agents_dir, de->d_name);
        if (stat(subdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        stamp = fold_stamp(stamp, &st);

        char manifest[PATH_MAX];
        snprintf(manifest, sizeof manifest, "%.780s/agent.json", subdir);
        if (stat(manifest, &st) == 0) stamp = fold_stamp(stamp, &st);
    }
    closedir(dp);
    return stamp;
}

static void refresh_agent_registry_if_needed(void) {
    unsigned long cur = agents_dir_stamp();
    if (cur == 0 || cur == g_agents_stamp) return;
    init_agent_registry();
    g_agents_stamp = cur;
}

/* ═══════════════════════════════════════════════════════════════════
 * LEADERBOARD – per-config best scores persisted to scores.json
 * ═══════════════════════════════════════════════════════════════════ */

#define LEADERBOARD_MAX 500      /* max entries in scores.json */

typedef struct {
    char config_key[128];
    char agent_name[64];
    double adjusted_score;
    int raw_score;
    int ticks;
    int apples_eaten;
    char timestamp[64];          /* ISO-8601 local time */
} LeaderEntry;

static LeaderEntry g_leaders[LEADERBOARD_MAX];
static int         g_leader_count = 0;

static void leaderboard_path(char *out, size_t outsz) {
    snprintf(out, outsz, "%s/scores.json", g_exe_dir);
}

static void leaderboard_load(void) {
    g_leader_count = 0;
    char path[PATH_MAX];
    leaderboard_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);

    /* Minimal parse: scan for objects in the top-level array */
    const char *p = buf;
    while (*p && *p != '[') p++;
    if (*p == '[') p++;

    while (g_leader_count < LEADERBOARD_MAX) {
        while (*p && *p != '{') { if (*p == ']') goto done; p++; }
        if (!*p) break;
        /* Find closing brace */
        const char *obj_start = p;
        int depth = 0;
        const char *obj_end = NULL;
        for (const char *q = p; *q; q++) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { obj_end = q; break; } }
        }
        if (!obj_end) break;

        /* Extract fields from this object */
        size_t olen = (size_t)(obj_end - obj_start + 1);
        char *obj = malloc(olen + 1);
        if (!obj) break;
        memcpy(obj, obj_start, olen);
        obj[olen] = '\0';

        LeaderEntry *e = &g_leaders[g_leader_count];
        memset(e, 0, sizeof(*e));
        int iv;
        json_get_str(obj, "config_key", e->config_key, sizeof e->config_key);
        json_get_str(obj, "agent_name", e->agent_name, sizeof e->agent_name);
        json_get_str(obj, "timestamp", e->timestamp, sizeof e->timestamp);
        if (json_get_int(obj, "raw_score", &iv)) e->raw_score = iv;
        if (json_get_int(obj, "ticks", &iv)) e->ticks = iv;
        if (json_get_int(obj, "apples_eaten", &iv)) e->apples_eaten = iv;
        /* adjusted_score is a double — parse manually */
        {
            const char *as = strstr(obj, "\"adjusted_score\"");
            if (as) {
                as += strlen("\"adjusted_score\"");
                while (*as == ' ' || *as == ':' || *as == '\t') as++;
                e->adjusted_score = strtod(as, NULL);
            }
        }
        if (e->config_key[0]) g_leader_count++;
        free(obj);
        p = obj_end + 1;
    }
done:
    free(buf);
}

static void leaderboard_save(void) {
    char path[PATH_MAX];
    leaderboard_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[\n");
    for (int i = 0; i < g_leader_count; i++) {
        LeaderEntry *e = &g_leaders[i];
        fprintf(f,
            "  {\"config_key\":\"%s\",\"agent_name\":\"%s\","
            "\"adjusted_score\":%.2f,\"raw_score\":%d,"
            "\"ticks\":%d,\"apples_eaten\":%d,"
            "\"timestamp\":\"%s\"}%s\n",
            e->config_key, e->agent_name,
            e->adjusted_score, e->raw_score,
            e->ticks, e->apples_eaten,
            e->timestamp,
            (i + 1 < g_leader_count) ? "," : "");
    }
    fprintf(f, "]\n");
    fclose(f);
}

static int leaderboard_rank(const char *config_key, double adjusted_score) {
    int rank = 1;
    for (int i = 0; i < g_leader_count; i++) {
        if (strcmp(g_leaders[i].config_key, config_key) != 0) continue;
        if (g_leaders[i].adjusted_score > adjusted_score) rank++;
    }
    return rank;
}

static void leaderboard_insert(const char *config_key, const char *agent_name,
                                double adjusted_score, int raw_score,
                                int ticks_val, int apples_eaten) {
    if (g_leader_count >= LEADERBOARD_MAX) {
        /* Evict the oldest entry (first in array) */
        for (int i = 0; i + 1 < g_leader_count; i++)
            g_leaders[i] = g_leaders[i + 1];
        g_leader_count--;
    }
    LeaderEntry *e = &g_leaders[g_leader_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->config_key, sizeof e->config_key, "%s", config_key);
    snprintf(e->agent_name, sizeof e->agent_name, "%s", agent_name);
    e->adjusted_score = adjusted_score;
    e->raw_score = raw_score;
    e->ticks = ticks_val;
    e->apples_eaten = apples_eaten;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(e->timestamp, sizeof e->timestamp,
             "%04d-%02d-%02dT%02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void update_rules_for_stage(GameRules *rules, int stage) {
    if (!rules) return;
    rules->stage = clamp(stage, 1, 12);

    /* Reset modifiers — stages 1-7 use none. */
    rules->poison_count          = 0;
    rules->obstacle_count        = 0;
    rules->vision_radius         = 0;
    rules->apple_decay_ticks     = 0;
    rules->initial_snake_len     = 0;
    rules->tick_penalty_interval = 0;
    rules->score_multiplier      = 1.0;

    switch (stage) {
    case 8: /* Obstacles */
        rules->obstacle_count    = g_settings.stage8_obstacles;
        rules->stage5_goal_apples = g_settings.stage8_goal_apples;
        break;
    case 9: /* Poison + Obstacles */
        rules->poison_count      = g_settings.stage9_poison;
        rules->obstacle_count    = g_settings.stage9_obstacles;
        rules->stage5_goal_apples = g_settings.stage9_goal_apples;
        break;
    case 10: /* Fog of war */
        rules->vision_radius     = g_settings.stage10_vision_radius;
        rules->stage5_goal_apples = g_settings.stage10_goal_apples;
        break;
    case 11: /* Decay + obstacles + tick penalty */
        rules->apple_decay_ticks = g_settings.stage11_apple_decay;
        rules->obstacle_count    = g_settings.stage11_obstacles;
        rules->tick_penalty_interval = g_settings.stage11_tick_penalty;
        rules->stage5_goal_apples = g_settings.stage11_goal_apples;
        break;
    case 12: /* Everything */
        rules->poison_count      = g_settings.stage12_poison;
        rules->obstacle_count    = g_settings.stage12_obstacles;
        rules->vision_radius     = g_settings.stage12_vision_radius;
        rules->apple_decay_ticks = g_settings.stage12_apple_decay;
        rules->initial_snake_len = g_settings.stage12_initial_snake_len;
        rules->stage5_goal_apples = g_settings.stage12_goal_apples;
        break;
    }
}

static void build_config_key(char *out, size_t outsz, const AgentCfg *cfg) {
    GameRules r = cfg->rules;
    update_rules_for_stage(&r, cfg->difficulty_stage);
    int w = cfg->width, h = cfg->height;
    if (cfg->difficulty_stage == 12) {
        w = g_settings.stage12_width;
        h = g_settings.stage12_height;
    }
    snprintf(out, outsz, "s%d_p%d_o%d_v%d_d%d_l%d_t%d_%dx%d",
             cfg->difficulty_stage,
             r.poison_count, r.obstacle_count, r.vision_radius,
             r.apple_decay_ticks, r.initial_snake_len,
             r.tick_penalty_interval, w, h);
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
        init_pair(CP_APPLE,   COLOR_RED,    -1);
        init_pair(CP_BORDER, COLOR_WHITE,  -1);
        init_pair(CP_INFO,   COLOR_CYAN,   -1);
        init_pair(CP_GOOD,   COLOR_GREEN,  -1);
        init_pair(CP_BAD,    COLOR_RED,    -1);
        init_pair(CP_DIM,    COLOR_BLACK,  -1);  /* bright-black = grey */
        init_pair(CP_POISON, COLOR_MAGENTA, -1);
        init_pair(CP_WALL,   COLOR_WHITE,  -1);
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

static int in_vision(const SnakeGame *g, int x, int y) {
    int vr = g->rules.vision_radius;
    if (vr <= 0) return 1; /* full visibility */
    Point h = g->body[g->head_idx];
    return (abs(x - h.x) + abs(y - h.y)) <= vr;
}

static void write_state_json(FILE *out, const SnakeGame *g) {
    fprintf(out,
        "{\"tick\":%d,\"alive\":%s,\"score\":%d,"
        "\"apples_eaten\":%d,\"goal_apples\":%d,"
        "\"difficulty\":%d,\"outcome\":\"%s\",\"score_mode\":\"%s\","
        "\"width\":%d,\"height\":%d,\"dir\":\"%s\","
        "\"vision_radius\":%d,\"apple_decay_ticks\":%d,"
        "\"tick_penalty_interval\":%d,\"score_multiplier\":%.2f,"
        "\"snake\":[",
        g->tick, g->alive ? "true" : "false", g->score,
        g->apples_eaten, g->goal_apples, g->rules.stage,
        game_outcome_name(g->outcome), game_score_mode_name(g->score_mode),
        g->width, g->height, direction_name(g->dir),
        g->rules.vision_radius, g->rules.apple_decay_ticks,
        g->rules.tick_penalty_interval, g->rules.score_multiplier);
    int first = 1;
    for (int i = 0; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        int bx = g->body[idx].x, by = g->body[idx].y;
        if (!in_vision(g, bx, by)) continue;
        fprintf(out, "%s[%d,%d]", first ? "" : ",", bx, by);
        first = 0;
    }
    fprintf(out, "],\"apples\":[");
    first = 1;
    for (int i = 0; i < g->apple_count; i++) {
        if (!in_vision(g, g->apples[i].x, g->apples[i].y)) continue;
        fprintf(out, "%s[%d,%d]", first ? "" : ",",
                g->apples[i].x, g->apples[i].y);
        first = 0;
    }
    fprintf(out, "],\"poison\":[");
    first = 1;
    for (int i = 0; i < g->poison_count; i++) {
        if (!in_vision(g, g->poison[i].x, g->poison[i].y)) continue;
        fprintf(out, "%s[%d,%d]", first ? "" : ",",
                g->poison[i].x, g->poison[i].y);
        first = 0;
    }
    fprintf(out, "],\"obstacles\":[");
    first = 1;
    for (int i = 0; i < g->obstacle_count; i++) {
        if (!in_vision(g, g->obstacles[i].x, g->obstacles[i].y)) continue;
        fprintf(out, "%s[%d,%d]", first ? "" : ",",
                g->obstacles[i].x, g->obstacles[i].y);
        first = 0;
    }
    fprintf(out, "]}\n");
    fflush(out);
}

static void write_state_raw_board(FILE *out, const SnakeGame *g) {
    char board[MAX_GRID_DIM][MAX_GRID_DIM];
    memset(board, '.', sizeof(board));

    for (int y = 0; y < g->height; y++) {
        for (int x = 0; x < g->width; x++) {
            if (g->grid[y][x] == CELL_APPLE)      board[y][x] = '*';
            else if (g->grid[y][x] == CELL_POISON) board[y][x] = 'X';
            else if (g->grid[y][x] == CELL_WALL_BLOCK) board[y][x] = '#';
        }
    }

    for (int i = 1; i < g->snake_len; i++) {
        int idx = (g->head_idx - i + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        Point p = g->body[idx];
        if (p.x >= 0 && p.x < g->width && p.y >= 0 && p.y < g->height)
            board[p.y][p.x] = 'o';
    }

    {
        Point h = g->body[g->head_idx];
        if (h.x >= 0 && h.x < g->width && h.y >= 0 && h.y < g->height)
            board[h.y][h.x] = '@';
    }

    fprintf(out,
            "STATE_BEGIN\n"
            "tick:%d\n"
            "alive:%s\n"
            "score:%d\n"
            "apples_eaten:%d\n"
            "goal_apples:%d\n"
            "difficulty:%d\n"
            "outcome:%s\n"
            "score_mode:%s\n"
            "width:%d\n"
            "height:%d\n"
            "dir:%s\n"
            "board:\n",
            g->tick, g->alive ? "true" : "false", g->score, g->apples_eaten,
            g->goal_apples, g->rules.stage, game_outcome_name(g->outcome),
            game_score_mode_name(g->score_mode), g->width, g->height,
            direction_name(g->dir));
    fputc('+', out);
    for (int x = 0; x < g->width; x++) fputc('-', out);
    fputs("+\n", out);
    for (int y = 0; y < g->height; y++) {
        fputc('|', out);
        for (int x = 0; x < g->width; x++)
            fputc(board[y][x], out);
        fputs("|\n", out);
    }
    fputc('+', out);
    for (int x = 0; x < g->width; x++) fputc('-', out);
    fputs("+\nSTATE_END\n", out);
    fflush(out);
}

static int write_state(FILE *out, const SnakeGame *g, OutputMode mode) {
    if (mode == OUTPUT_RAW_BOARD) write_state_raw_board(out, g);
    else write_state_json(out, g);
    return ferror(out) ? -1 : 0;
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
                            int step, int speed_ms, OutputMode output_mode,
                            const GameRules *rules) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin,  NULL, _IOLBF, 0);
    static SnakeGame game;
    game_init_with_rules(&game, width, height, seed, rules);
    while (game.alive && !g_quit) {
        if (write_state(stdout, &game, output_mode) < 0) break;
        Turn t = step ? read_cmd_blocking() : read_cmd_timed(speed_ms);
        if (g_quit) break;
        game_set_turn(&game, t);
        game_tick(&game);
    }
    write_state(stdout, &game, output_mode);
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

    /* obstacles */
    if (g_color) attron(COLOR_PAIR(CP_WALL) | A_BOLD);
    for (int i = 0; i < g->obstacle_count; i++) {
        Point w = g->obstacles[i];
        mvaddch(oy + w.y + 1, w.x + 1, '#');
    }
    if (g_color) attroff(COLOR_PAIR(CP_WALL) | A_BOLD);

    /* poison */
    if (g_color) attron(COLOR_PAIR(CP_POISON) | A_BOLD);
    for (int i = 0; i < g->poison_count; i++) {
        Point p = g->poison[i];
        mvaddch(oy + p.y + 1, p.x + 1, 'X');
    }
    if (g_color) attroff(COLOR_PAIR(CP_POISON) | A_BOLD);

    /* apples */
    if (g_color) attron(COLOR_PAIR(CP_APPLE) | A_BOLD);
    for (int i = 0; i < g->apple_count; i++) {
        Point a = g->apples[i];
        mvaddch(oy + a.y + 1, a.x + 1, '*');
    }
    if (g_color) attroff(COLOR_PAIR(CP_APPLE) | A_BOLD);

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
        refresh_agent_registry_if_needed();
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
                     unsigned int seed, int step, int difficulty_stage,
                     const GameRules *rules_base) {
    static SnakeGame game;
    GameRules rules = *rules_base;
    update_rules_for_stage(&rules, difficulty_stage);
    game_init_with_rules(&game, width, height, seed, &rules);

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
        mvprintw(sy + 2, 0,
                 "Difficulty: %d  Apples: %d  Eaten: %d  Goal: %d",
                 game.rules.stage, game.apple_count, game.apples_eaten,
                 game.goal_apples);
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
        mvprintw(sy, 0, " %s! Score: %d Ticks: %d Eaten: %d ",
                 game.outcome == GAME_OUTCOME_PASS ? "PASS" : "FAIL",
                 game.score, game.tick, game.apples_eaten);
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
        { "Width",      1, g_settings.width, 5, 100, 1, 5, NULL, 0, NULL, "", 1 },
        { "Height",     1, g_settings.height, 5, 100, 1, 5, NULL, 0, NULL, "", 1 },
        { "Speed",      1, g_settings.speed_ms, 10, 5000, 10, 50, NULL, 0, "ms", "", 1 },
        { "Seed",       1, (int)g_settings.seed, 0, 999999, 1, 100, NULL, 0, NULL, "", 1 },
        { "Tick Mode",  0, g_settings.play_step ? 1 : 0, 0, 0, 0, 0, tick_opts, 2, NULL, "", 1 },
        { "Difficulty", 1, g_settings.play_difficulty, 1, 12, 1, 1, NULL, 0, NULL, "", 1 },
    };
    int nf = sizeof ff / sizeof ff[0];

    if (show_form("PLAY SETTINGS", ff, nf, "PgUp/PgDn or </>: big steps")) {
        g_settings.width = ff[0].val;
        g_settings.height = ff[1].val;
        g_settings.speed_ms = ff[2].val;
        g_settings.seed = (unsigned)ff[3].val;
        g_settings.play_step = ff[4].val;
        g_settings.play_difficulty = ff[5].val;
        run_play(ff[0].val, ff[1].val, ff[2].val,
                 (unsigned)ff[3].val, ff[4].val, ff[5].val, &g_settings.rules);
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
    if (timeout_ms < 0) timeout_ms = -1;
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
        GameRules rules = cfg->rules;
        update_rules_for_stage(&rules, cfg->difficulty_stage);
        int gw = cfg->width, gh = cfg->height;
        if (cfg->difficulty_stage == 12) {
            gw = g_settings.stage12_width;
            gh = g_settings.stage12_height;
        }
        game_init_with_rules(&game, gw, gh, seed, &rules);

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
                if (write_state(ea->wr, &game, cfg->output_mode) < 0) {
                    game.alive = 0;
                    game.outcome = GAME_OUTCOME_FAIL;
                    break;
                }
                t = cfg->step ? read_ext_turn(ea, -1) : read_ext_turn(ea, 5000);
            }

            /* ── Log ──────────────────────────────────────── */
            if (logfp) {
                fprintf(logfp,
                    "{\"game\":%d,\"tick\":%d,\"action\":\"%s\","
                    "\"score\":%d,\"apples_eaten\":%d,"
                    "\"difficulty\":%d,\"outcome\":\"%s\",\"dir\":\"%s\","
                    "\"head\":[%d,%d],\"alive\":true}\n",
                    g + 1, game.tick, turn_name(t),
                    game.score, game.apples_eaten,
                    game.rules.stage, game_outcome_name(game.outcome),
                    direction_name(game.dir),
                    game.body[game.head_idx].x, game.body[game.head_idx].y);
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
                mvprintw(sy + 1, 0,
                    "Difficulty: %d  Apples: %d  Eaten: %d  Goal: %d  Output: %s",
                    game.rules.stage, game.apple_count, game.apples_eaten, game.goal_apples,
                    cfg->output_mode == OUTPUT_RAW_BOARD ? "raw_board" : "json");
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
                    mvprintw(r++, bx + 3, "Difficulty: %d   Output: %s",
                             cfg->difficulty_stage,
                             cfg->output_mode == OUTPUT_RAW_BOARD ? "raw_board" : "json");
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
                "\"score\":%d,\"apples_eaten\":%d,"
                "\"difficulty\":%d,\"outcome\":\"%s\",\"dir\":\"%s\","
                "\"head\":[%d,%d],\"alive\":false}\n",
                g + 1, game.tick,
                game.score, game.apples_eaten,
                game.rules.stage, game_outcome_name(game.outcome),
                direction_name(game.dir),
                game.body[game.head_idx].x, game.body[game.head_idx].y);
        }

        /* send dead state to external agent, then stop it */
        if (ea) {
            write_state(ea->wr, &game, cfg->output_mode);
            stop_ext_agent(ea);
            ea = NULL;
        }

        br->results[g].score   = game.score;
        br->results[g].ticks   = game.tick;
        br->results[g].apples_eaten = game.apples_eaten;
        br->results[g].outcome = game.outcome;
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

static const GameResult *g_rank_results = NULL;

/*
 * Time-rank comparator (best first):
 *   1) pass outcomes before fail outcomes
 *   2) lower ticks
 *   3) higher apples eaten
 *   4) stable by original run index
 */
static int time_rank_idx_cmp(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    const GameResult *ra = &g_rank_results[ia];
    const GameResult *rb = &g_rank_results[ib];

    int pa = (ra->outcome == GAME_OUTCOME_PASS) ? 1 : 0;
    int pb = (rb->outcome == GAME_OUTCOME_PASS) ? 1 : 0;
    if (pa != pb) return pb - pa;

    if (ra->ticks != rb->ticks) return (ra->ticks < rb->ticks) ? -1 : 1;
    if (ra->apples_eaten != rb->apples_eaten)
        return (ra->apples_eaten > rb->apples_eaten) ? -1 : 1;

    return ia - ib;
}

static void build_time_rank_order(const BatchResult *br, int n, int *order) {
    for (int i = 0; i < n; i++) order[i] = i;
    g_rank_results = br->results;
    qsort(order, (size_t)n, sizeof(int), time_rank_idx_cmp);
    g_rank_results = NULL;
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
    int is_time_rank = (cfg->difficulty_stage >= 6);

    /* gather stats */
    int *scores = malloc((size_t)n * sizeof(int));
    int *ticks  = malloc((size_t)n * sizeof(int));
    int *eaten  = malloc((size_t)n * sizeof(int));
    int *rank_order = NULL;
    int sum_sc = 0, sum_tk = 0, sum_eaten = 0, max_sc = 0, min_sc = scores ? scores[0] : 0;
    int max_tk = 0, min_tk = 0;
    int pass_count = 0;

    for (int i = 0; i < n; i++) {
        scores[i] = br->results[i].score;
        ticks[i]  = br->results[i].ticks;
        eaten[i]  = br->results[i].apples_eaten;
        sum_sc += scores[i]; sum_tk += ticks[i]; sum_eaten += eaten[i];
        if (br->results[i].outcome == GAME_OUTCOME_PASS) pass_count++;
        if (i == 0 || scores[i] > max_sc) max_sc = scores[i];
        if (i == 0 || scores[i] < min_sc) min_sc = scores[i];
        if (i == 0 || ticks[i]  > max_tk) max_tk = ticks[i];
        if (i == 0 || ticks[i]  < min_tk) min_tk = ticks[i];
    }
    double avg_sc = (double)sum_sc / n;
    double avg_tk = (double)sum_tk / n;
    double avg_eaten = (double)sum_eaten / n;
    double med_sc = calc_median(scores, n);
    double med_tk = calc_median(ticks, n);
    double std_sc = calc_stddev(scores, n, avg_sc);
    double std_tk = calc_stddev(ticks, n, avg_tk);
    if (is_time_rank) {
        rank_order = malloc((size_t)n * sizeof(int));
        if (rank_order) build_time_rank_order(br, n, rank_order);
    }

    int scroll = 0;          /* scroll offset for per-game table */
    const int tbl_rows = 5;  /* visible rows in the table */

    while (!g_quit) {
        erase();

        int bw = 56;
        /* top stats area: ~18 rows, per-game table: tbl_rows + 2, controls: 3 */
        int stats_h = is_time_rank ? 21 : 20;
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
        mvprintw(r++, bx + 3, "Stage:    %d           Output: %s",
                 cfg->difficulty_stage,
                 cfg->output_mode == OUTPUT_RAW_BOARD ? "raw_board" : "json");
        r++;

        /* scores */
        if (g_color) attron(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "-- Scores %-35s", "---");
        if (g_color) attroff(COLOR_PAIR(CP_INFO));
        mvprintw(r++, bx + 3, "Avg: %-8.1f Max: %-6d Min: %-6d",
                 avg_sc, max_sc, min_sc);
        mvprintw(r++, bx + 3, "Med: %-8.1f Std: %-6.1f", med_sc, std_sc);
        r++;

        if (!is_time_rank)
            mvprintw(r++, bx + 3, "Passes: %d / %d (pass/fail scoring)", pass_count, n);
        else {
            mvprintw(r++, bx + 3, "Time rank: pass > lower ticks > higher apples");
            if (rank_order && n > 0) {
                int bi = rank_order[0];
                mvprintw(r++, bx + 3,
                         "Best run: #%d  %s  ticks=%d  apples=%d",
                         bi + 1, game_outcome_name(br->results[bi].outcome),
                         br->results[bi].ticks, br->results[bi].apples_eaten);
            } else {
                mvprintw(r++, bx + 3, "Best run: unavailable");
            }
        }
        mvprintw(r++, bx + 3, "Avg apples eaten: %.1f", avg_eaten);
        /* Score multiplier & leaderboard rank */
        {
            GameRules tmp_r = cfg->rules;
            update_rules_for_stage(&tmp_r, cfg->difficulty_stage);
            double mult = tmp_r.score_multiplier;
            if (mult < 0.01) mult = 1.0;
            mvprintw(r++, bx + 3, "Multiplier: %.2fx  Best adj: %.1f",
                     mult, max_sc * mult);
            char ckey[128];
            build_config_key(ckey, sizeof ckey, cfg);
            int rank = leaderboard_rank(ckey, max_sc * mult);
            if (g_color) attron(COLOR_PAIR(CP_GOOD));
            mvprintw(r++, bx + 3, "Leaderboard rank: #%d", rank);
            if (g_color) attroff(COLOR_PAIR(CP_GOOD));
        }

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
        if (!is_time_rank)
            mvprintw(r++, bx + 3, " #     Score    Ticks     Eaten   Result");
        else
            mvprintw(r++, bx + 3, " Rank  Game   Score    Ticks   Eaten   Result");
        attroff(A_BOLD);

        /* table rows */
        for (int i = 0; i < tbl_rows && scroll + i < n; i++) {
            int row_idx = scroll + i;
            int idx = (is_time_rank && rank_order) ? rank_order[row_idx] : row_idx;
            if (!is_time_rank) {
                mvprintw(r + i, bx + 3, " %-5d  %-6d   %-7d   %-6d  %-6s",
                         idx + 1,
                         br->results[idx].score,
                         br->results[idx].ticks,
                         br->results[idx].apples_eaten,
                         game_outcome_name(br->results[idx].outcome));
            } else {
                mvprintw(r + i, bx + 3, " %-5d %-5d  %-6d   %-6d  %-6d  %-6s",
                         row_idx + 1, idx + 1,
                         br->results[idx].score,
                         br->results[idx].ticks,
                         br->results[idx].apples_eaten,
                         game_outcome_name(br->results[idx].outcome));
            }
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
                fprintf(f, "Stage:     %d\n", cfg->difficulty_stage);
                fprintf(f, "Output:    %s\n",
                        cfg->output_mode == OUTPUT_RAW_BOARD ? "raw_board" : "json");
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
                if (!is_time_rank)
                    fprintf(f, "Passes: %d / %d\n", pass_count, n);
                else {
                    fprintf(f, "Time ranking: pass > lower ticks > higher apples\n");
                    if (rank_order) {
                        int topn = (n < 3) ? n : 3;
                        fprintf(f, "Top ranked runs:\n");
                        for (int rr = 0; rr < topn; rr++) {
                            int idx = rank_order[rr];
                            fprintf(f, "  #%d run %d: outcome=%s ticks=%d apples=%d\n",
                                    rr + 1, idx + 1,
                                    game_outcome_name(br->results[idx].outcome),
                                    br->results[idx].ticks,
                                    br->results[idx].apples_eaten);
                        }
                    }
                }
                fprintf(f, "Avg apples eaten: %.1f\n\n", avg_eaten);

                if (br->log_path[0])
                    fprintf(f, "Action Log: %s\n\n", br->log_path);

                fprintf(f, "Per-game Results\n");
                if (!is_time_rank) {
                    fprintf(f, "#\tScore\tTicks\tEaten\tResult\tTime (s)\n");
                    for (int i = 0; i < n; i++)
                        fprintf(f, "%d\t%d\t%d\t%d\t%s\t%.3f\n",
                                i + 1, br->results[i].score,
                                br->results[i].ticks,
                                br->results[i].apples_eaten,
                                game_outcome_name(br->results[i].outcome),
                                br->results[i].elapsed);
                } else {
                    fprintf(f, "Rank\tGame\tScore\tTicks\tEaten\tResult\tTime (s)\n");
                    for (int rr = 0; rr < n; rr++) {
                        int idx = rank_order ? rank_order[rr] : rr;
                        fprintf(f, "%d\t%d\t%d\t%d\t%d\t%s\t%.3f\n",
                                rr + 1, idx + 1,
                                br->results[idx].score,
                                br->results[idx].ticks,
                                br->results[idx].apples_eaten,
                                game_outcome_name(br->results[idx].outcome),
                                br->results[idx].elapsed);
                    }
                }
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
        if (ch == 'r' || ch == 'R') {
            free(scores); free(ticks); free(eaten); free(rank_order);
            return 1;
        }
        if (ch == 'c' || ch == 'C') {
            free(scores); free(ticks); free(eaten); free(rank_order);
            return 2;
        }
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            free(scores); free(ticks); free(eaten); free(rank_order);
            return 0;
        }
    }

    free(scores); free(ticks); free(eaten); free(rank_order);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * AGENT CONFIG FORM
 * ═══════════════════════════════════════════════════════════════════ */

static void agent_mode(void) {
    static const char *tick_opts[]  = { "Step (static)" };
    static const char *yesno_opts[] = { "No", "Yes" };
    static const char *output_opts[] = { "json", "raw_board" };

    /* Field indices */
    enum {
        F_AGENT, F_CMD, F_GAMES, F_WIDTH, F_HEIGHT,
        F_SPEED, F_SEED, F_TICK, F_WATCH, F_LOG, F_DIFFICULTY, F_OUTPUT, F_COUNT
    };

    FField ff[F_COUNT];
    memset(ff, 0, sizeof ff);

    ff[F_AGENT]  = (FField){ "Agent",     0, settings_find_default_agent_idx(), 0, g_num_agents - 1, 1, 1,
                              g_agent_names, g_num_agents, NULL, "", 1 };
    ff[F_CMD]    = (FField){ "Command",   2, 0, 0, 0, 0, 0,
                              NULL, 0, NULL, "", 0 };  /* hidden initially */
    if (ff[F_AGENT].val < g_num_agents && g_agents[ff[F_AGENT].val].is_custom)
        snprintf(ff[F_CMD].text, sizeof ff[F_CMD].text, "python3 agent.py --pipe");
    ff[F_GAMES]  = (FField){ "Games",     1, g_settings.agent_games, 1, 100000, 1, 10,
                              NULL, 0, NULL, "", 1 };
    ff[F_WIDTH]  = (FField){ "Width",     1, g_settings.width, 5, 100, 1, 5,
                              NULL, 0, NULL, "", 1 };
    ff[F_HEIGHT] = (FField){ "Height",    1, g_settings.height, 5, 100, 1, 5,
                              NULL, 0, NULL, "", 1 };
    ff[F_SPEED]  = (FField){ "Speed",     1, g_settings.speed_ms, 10, 5000, 10, 50,
                              NULL, 0, "ms", "", 1 };
    ff[F_SEED]   = (FField){ "Seed",      1, (int)g_settings.seed, 0, 999999, 1, 100,
                              NULL, 0, NULL, "", 1 };
    ff[F_TICK]   = (FField){ "Tick Mode", 0, 0, 0, 0, 0, 0,
                              tick_opts, 1, NULL, "", 0 };
    ff[F_WATCH]  = (FField){ "Watch",     0, g_settings.agent_watch ? 1 : 0, 0, 1, 0, 0,
                              yesno_opts, 2, NULL, "", 1 };
    ff[F_LOG]    = (FField){ "Log Actions", 0, g_settings.agent_log ? 1 : 0, 0, 1, 0, 0,
                              yesno_opts, 2, NULL, "", 1 };
    ff[F_DIFFICULTY] = (FField){ "Difficulty", 1, g_settings.agent_difficulty, 1, 12, 1, 1,
                                 NULL, 0, NULL, "", 1 };
    ff[F_OUTPUT] = (FField){ "Output", 0,
                             g_settings.agent_output_mode == OUTPUT_RAW_BOARD ? 1 : 0,
                             0, 1, 0, 0, output_opts, 2, NULL, "", 1 };

    while (1) {
        refresh_agent_registry_if_needed();
        ff[F_AGENT].opts = g_agent_names;
        ff[F_AGENT].nopts = g_num_agents;
        ff[F_AGENT].hi = g_num_agents - 1;
        ff[F_AGENT].val = clamp(ff[F_AGENT].val, 0, g_num_agents - 1);

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
        ac.step      = 1; /* static: wait for each agent response */
        ac.watch     = ff[F_WATCH].val;
        ac.do_log    = ff[F_LOG].val;
        ac.difficulty_stage = ff[F_DIFFICULTY].val;
        ac.output_mode = ff[F_OUTPUT].val ? OUTPUT_RAW_BOARD : OUTPUT_JSON;
        ac.rules = g_settings.rules;
        if (g_agents[ac.agent_idx].is_custom)
            snprintf(ac.cmd, sizeof ac.cmd, "%.*s",
                     (int)(sizeof ac.cmd - 1), ff[F_CMD].text);

        g_settings.agent_games = ac.num_games;
        g_settings.width = ac.width;
        g_settings.height = ac.height;
        g_settings.speed_ms = ac.speed_ms;
        g_settings.seed = ac.seed;
        g_settings.agent_step = 1;
        g_settings.agent_watch = ac.watch;
        g_settings.agent_log = ac.do_log;
        g_settings.agent_difficulty = ac.difficulty_stage;
        g_settings.agent_output_mode = ac.output_mode;
        snprintf(g_settings.default_agent, sizeof g_settings.default_agent,
                 "%s", g_agents[ac.agent_idx].name);

        /* run loop: run → summary → run again / reconfigure / menu */
        while (1) {
            BatchResult br;
            memset(&br, 0, sizeof br);
            run_agent_batch(&ac, &br);

            /* Insert best passing result into leaderboard */
            if (br.completed > 0) {
                int best_idx = -1;
                for (int gi = 0; gi < br.completed; gi++) {
                    if (br.results[gi].outcome != GAME_OUTCOME_PASS) continue;
                    if (best_idx < 0 || br.results[gi].score > br.results[best_idx].score)
                        best_idx = gi;
                }
                if (best_idx >= 0) {
                    char ckey[128];
                    build_config_key(ckey, sizeof ckey, &ac);
                    GameRules tmp_r = ac.rules;
                    update_rules_for_stage(&tmp_r, ac.difficulty_stage);
                    double mult = tmp_r.score_multiplier;
                    if (mult < 0.01) mult = 1.0;
                    double adj = br.results[best_idx].score * mult;
                    leaderboard_insert(ckey, g_agents[ac.agent_idx].name,
                                       adj, br.results[best_idx].score,
                                       br.results[best_idx].ticks,
                                       br.results[best_idx].apples_eaten);
                    leaderboard_save();
                }
            }

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
        "Usage: %s [play|agent]\n\n"
        "No arguments → interactive menu.\n\n"
        "Configuration is loaded from settings.json and in-program menus.\n"
        "CLI configuration options are disabled by design.\n\n"
        "Modes:\n"
        "  play      Interactive TUI\n"
        "  agent     Agent protocol on stdin/stdout (json or raw_board)\n\n"
        "Note: Agent mode is static by default (waits for each action).\n"
        "Use --help to show this message.\n",
        prog);
}

int main(int argc, char *argv[]) {
    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);
    resolve_exe_dir(argv[0]);
    load_settings();
    init_agent_registry();
    g_agents_stamp = agents_dir_stamp();
    leaderboard_load();

    /* ── Detect CLI mode ────────────────────────────────────── */
    RunMode mode = RUN_MENU;
    int width = g_settings.width;
    int height = g_settings.height;
    int speed_ms = g_settings.speed_ms;
    int difficulty = g_settings.play_difficulty;
    OutputMode output_mode = g_settings.agent_output_mode;
    unsigned int seed = g_settings.seed;

    int i = 1;
    if (i < argc && (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (i < argc && argv[i][0] != '-') {
        if      (strcmp(argv[i], "play")  == 0) mode = RUN_PLAY;
        else if (strcmp(argv[i], "agent") == 0) {
            mode = RUN_AGENT_PROTO;
            difficulty = g_settings.agent_difficulty;
        }
        else { fprintf(stderr, "Unknown mode: %s\n", argv[i]); print_usage(argv[0]); return 1; }
        i++;
    }
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "CLI option '%s' is disabled.\n", argv[i]);
            fprintf(stderr, "Edit settings.json or use the in-program menu instead.\n");
            print_usage(argv[0]); return 1;
        }
    }

    /* validate dimensions */
    if (width < 5 || width > MAX_GRID_DIM || height < 5 || height > MAX_GRID_DIM) {
        fprintf(stderr, "Grid dimensions must be in 5..%d\n", MAX_GRID_DIM);
        return 1;
    }
    difficulty = clamp(difficulty, 1, 12);

    /* ── Agent protocol mode (no ncurses) ───────────────────── */
    if (mode == RUN_AGENT_PROTO) {
        GameRules rules = g_settings.rules;
        update_rules_for_stage(&rules, difficulty);
        int pw = width, ph = height;
        if (difficulty == 12) { pw = g_settings.stage12_width; ph = g_settings.stage12_height; }
        run_agent_proto(pw, ph, seed, 1, speed_ms, output_mode, &rules);
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
        run_play(width, height, speed_ms, seed,
                 g_settings.play_step ? 1 : 0, difficulty, &g_settings.rules);
    } else {
        /* interactive menu loop */
        while (!g_quit) {
            refresh_agent_registry_if_needed();
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
