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

/* ── Cell types on the grid ─────────────────────────────────── */
typedef enum { CELL_EMPTY = 0, CELL_SNAKE = 1, CELL_FOOD = 2 } CellType;

/* ── Absolute directions (clockwise: UP→RIGHT→DOWN→LEFT) ──── */
typedef enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 } Direction;

/* ── Relative turn commands ─────────────────────────────────── */
typedef enum { TURN_STRAIGHT = 0, TURN_LEFT = 1, TURN_RIGHT = 2 } Turn;

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

    Direction     dir;                 /* current heading */
    Point         food;                /* food position */

    int           score;
    int           alive;               /* 1 while playing, 0 on death/win */
    int           tick;                /* incremented after each successful step */

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

#ifdef __cplusplus
}
#endif

#endif /* SNAKE_GAME_H */
