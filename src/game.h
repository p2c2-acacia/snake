/*
 * game.h – Snake game core data structures and API.
 *
 * The game grid uses (x, y) coordinates where (0,0) is the top-left corner,
 * x increases to the right, y increases downward.
 *
 * Turns are RELATIVE to the snake's current heading:
 *   TURN_LEFT   – 90° counter-clockwise
 *   TURN_RIGHT  – 90° clockwise
 *   TURN_STRAIGHT – no change
 */

#ifndef SNAKE_GAME_H
#define SNAKE_GAME_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

/* Maximum grid dimension (width or height). */
#define MAX_GRID_DIM  100

/* Maximum possible snake body length (cells on the grid). */
#define MAX_SNAKE_LEN (MAX_GRID_DIM * MAX_GRID_DIM)
#define MAX_APPLES    MAX_SNAKE_LEN

/* ── Cell types on the grid ─────────────────────────────────── */
typedef enum {
    CELL_EMPTY      = 0,
    CELL_SNAKE      = 1,
    CELL_APPLE      = 2,
    CELL_POISON     = 3,
    CELL_WALL_BLOCK = 4
} CellType;

/* ── Absolute directions (clockwise: UP→RIGHT→DOWN→LEFT) ──── */
typedef enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 } Direction;

/* ── Relative turn commands ─────────────────────────────────── */
typedef enum { TURN_STRAIGHT = 0, TURN_LEFT = 1, TURN_RIGHT = 2 } Turn;

/* ── End states and scoring modes ───────────────────────────── */
typedef enum {
    GAME_OUTCOME_RUNNING = 0,
    GAME_OUTCOME_PASS    = 1,
    GAME_OUTCOME_FAIL    = 2
} GameOutcome;

typedef enum {
    SCORE_PASS_FAIL = 0,
    SCORE_TIME_RANK = 1
} ScoreMode;

/* ── Difficulty rules ────────────────────────────────────────── */
typedef struct {
    int stage;                      /* 1..12 */
    int stage2_fixed_apples;        /* stage 2 */
    int stage4_fixed_apples;        /* stage 4 */
    int stage5_goal_apples;         /* stage 5 pass threshold */
    int stage6_fixed_apples;        /* stage 6 active apples */
    int stage6_goal_apples;         /* stage 6 pass threshold */
    int stage7_initial_apples;      /* stage 7 initial active apples */
    int stage7_increment_every;     /* +apples every N eaten */
    int stage7_increment_by;        /* add this many apples */
    int stage7_max_apples;          /* cap active apples */
    int stage7_goal_apples;         /* stage 7 pass threshold */

    /* ── Modifier fields (stages 8+, 0 = off) ──────────────── */
    int poison_count;               /* N = maintain N poison cells on board */
    int obstacle_count;             /* N = place N wall blocks at init */
    int vision_radius;              /* N = fog of war (Manhattan distance) */
    int apple_decay_ticks;          /* N = apples vanish after N ticks */
    int initial_snake_len;          /* 0 = default (3), else override */
    int tick_penalty_interval;      /* N = score-- every N ticks w/o eating */
    double score_multiplier;        /* computed from active modifiers */
} GameRules;

/* ── 2-D point ──────────────────────────────────────────────── */
typedef struct { int x, y; } Point;

/* ── Core game state ────────────────────────────────────────── */
typedef struct {
    int           width, height;       /* playfield dimensions (cells) */

    /* Snake body stored in a ring-buffer.
     * body[head_idx] is the head.
     * The tail is at (head_idx - snake_len + 1 + MAX_SNAKE_LEN) % MAX_SNAKE_LEN.
     * Iterating head→tail: idx = (head_idx - i) % MAX_SNAKE_LEN for i in 0..snake_len-1. */
    Point         body[MAX_SNAKE_LEN];
    int           head_idx;
    int           snake_len;
    int           target_snake_len;    /* snake grows each tick until this length */

    Direction     dir;                 /* current heading */
    Point         apples[MAX_APPLES];  /* active apples on the board */
    int           apple_count;         /* number of active apples */
    int           apple_spawn_tick[MAX_APPLES]; /* tick when each apple was placed */
    int           apples_eaten;        /* total apples collected */
    int           goal_apples;         /* pass threshold */
    int           stage7_active_apples;/* dynamic active apples for stage 7 */

    Point         poison[MAX_APPLES];  /* active poison cells on the board */
    int           poison_count;        /* number of active poison cells */

    Point         obstacles[MAX_APPLES]; /* wall-block positions */
    int           obstacle_count;      /* number of active wall blocks */

    int           last_eat_tick;       /* tick of last apple eaten (for penalty) */

    int           score;
    int           alive;               /* 1 while playing, 0 on death/win */
    int           tick;                /* incremented after each successful step */
    GameOutcome   outcome;
    ScoreMode     score_mode;
    GameRules     rules;

    Turn          pending_turn;        /* queued turn for the next tick */

    /* Grid mirror for O(1) collision look-ups.  grid[y][x]. */
    unsigned char grid[MAX_GRID_DIM][MAX_GRID_DIM];
} SnakeGame;

/* ── Public API ─────────────────────────────────────────────── */

/**
 * Initialise a new game.
 * @param width, height  Playfield size in cells (min 5, max MAX_GRID_DIM).
 * @param seed           Random seed; 0 → use current time.
 */
void game_init(SnakeGame *g, int width, int height, unsigned int seed);
void game_init_with_rules(SnakeGame *g, int width, int height,
                          unsigned int seed, const GameRules *rules);

/** Queue a relative turn for the next tick. */
void game_set_turn(SnakeGame *g, Turn turn);

/** Advance simulation by one tick.  Returns 1 if still alive, 0 otherwise. */
int game_tick(SnakeGame *g);

/** Apply a relative turn to a direction, returning the new direction. */
Direction direction_apply_turn(Direction d, Turn t);

/** Unit-vector delta for a direction. */
Point direction_delta(Direction d);

/** Human-readable name: "up", "right", "down", "left". */
const char *direction_name(Direction d);

/** Parse a turn command string ("left"/"right"/"straight").
 *  Returns TURN_STRAIGHT on unrecognised input. */
Turn parse_turn(const char *s);

/** Convert an absolute desired direction to a relative turn from @p current.
 *  Returns TURN_STRAIGHT for same-direction or 180° (which is forbidden). */
Turn direction_to_turn(Direction current, Direction desired);

/** Human-readable game outcome: "running", "pass", "fail". */
const char *game_outcome_name(GameOutcome outcome);

/** Human-readable score mode: "pass_fail" or "time_rank". */
const char *game_score_mode_name(ScoreMode mode);

/** Fill a GameRules struct with default values. */
void game_rules_default(GameRules *rules);

#ifdef __cplusplus
}
#endif

#endif /* SNAKE_GAME_H */
