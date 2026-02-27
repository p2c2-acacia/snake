/*
 * game.c – Snake core game logic.
 *
 * Uses a ring-buffer for the snake body and a 2-D grid for O(1)
 * collision detection.  All coordinates are in [0, width) × [0, height).
 */

#include "game.h"
#include <string.h>
#include <time.h>

/* ── Direction look-up tables ───────────────────────────────── */

static const Point DIR_DELTA[] = {
    { 0, -1},   /* DIR_UP    */
    { 1,  0},   /* DIR_RIGHT */
    { 0,  1},   /* DIR_DOWN  */
    {-1,  0},   /* DIR_LEFT  */
};

static const char *DIR_NAMES[] = { "up", "right", "down", "left" };

/* ── Direction helpers ──────────────────────────────────────── */

Direction direction_apply_turn(Direction d, Turn t) {
    switch (t) {
    case TURN_LEFT:  return (Direction)((d + 3) % 4);
    case TURN_RIGHT: return (Direction)((d + 1) % 4);
    default:         return d;
    }
}

Point direction_delta(Direction d) {
    return DIR_DELTA[d];
}

const char *direction_name(Direction d) {
    return DIR_NAMES[d & 3];
}

Turn parse_turn(const char *s) {
    if (!s) return TURN_STRAIGHT;
    /* skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (s[0] == 'l' || s[0] == 'L') return TURN_LEFT;      /* "left"     */
    if (s[0] == 'r' || s[0] == 'R') return TURN_RIGHT;     /* "right"    */
    return TURN_STRAIGHT;                                   /* "straight" / anything else */
}

Turn direction_to_turn(Direction current, Direction desired) {
    if (desired == current)
        return TURN_STRAIGHT;
    if (desired == (Direction)((current + 1) % 4))
        return TURN_RIGHT;
    if (desired == (Direction)((current + 3) % 4))
        return TURN_LEFT;
    return TURN_STRAIGHT;          /* 180° – forbidden, treat as straight */
}

/* ── Internal helpers ───────────────────────────────────────── */

static void spawn_food(SnakeGame *g) {
    /* Board full → the player wins. */
    if (g->snake_len >= g->width * g->height) {
        g->alive = 0;
        g->food  = (Point){-1, -1};
        return;
    }

    int x, y;
    do {
        x = rand() % g->width;
        y = rand() % g->height;
    } while (g->grid[y][x] != CELL_EMPTY);

    g->food = (Point){x, y};
    g->grid[y][x] = CELL_FOOD;
}

/* ── Public API ─────────────────────────────────────────────── */

void game_init(SnakeGame *g, int width, int height, unsigned int seed) {
    memset(g, 0, sizeof(*g));

    g->width  = width;
    g->height = height;
    g->alive  = 1;
    g->dir    = DIR_RIGHT;
    g->pending_turn = TURN_STRAIGHT;

    srand(seed ? seed : (unsigned)time(NULL));

    /* Place the snake in the centre, length 3, heading right.
     * body[0] = tail, body[1] = middle, body[2] = head. */
    int cx = width  / 2;
    int cy = height / 2;

    g->snake_len = 3;
    for (int i = 0; i < 3; i++) {
        g->body[i] = (Point){cx - 2 + i, cy};
        g->grid[cy][cx - 2 + i] = CELL_SNAKE;
    }
    g->head_idx = 2;

    spawn_food(g);
}

void game_set_turn(SnakeGame *g, Turn t) {
    g->pending_turn = t;
}

int game_tick(SnakeGame *g) {
    if (!g->alive) return 0;

    /* 1. Apply the queued turn. */
    g->dir = direction_apply_turn(g->dir, g->pending_turn);
    g->pending_turn = TURN_STRAIGHT;

    /* 2. Compute the new head position. */
    Point head = g->body[g->head_idx];
    Point d    = direction_delta(g->dir);
    Point nh   = {head.x + d.x, head.y + d.y};

    /* 3. Wall collision. */
    if (nh.x < 0 || nh.x >= g->width || nh.y < 0 || nh.y >= g->height) {
        g->alive = 0;
        return 0;
    }

    /* 4. Will we eat food this step? */
    int eating = (g->grid[nh.y][nh.x] == CELL_FOOD);

    /* 5. If NOT eating, retract the tail first (frees its cell so that
     *    chasing the tail is a legal move). */
    if (!eating) {
        int tail_idx = (g->head_idx - g->snake_len + 1 + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        g->grid[g->body[tail_idx].y][g->body[tail_idx].x] = CELL_EMPTY;
    }

    /* 6. Self-collision (checked AFTER tail retraction). */
    if (g->grid[nh.y][nh.x] == CELL_SNAKE) {
        g->alive = 0;
        return 0;
    }

    /* 7. Place the new head. */
    g->head_idx = (g->head_idx + 1) % MAX_SNAKE_LEN;
    g->body[g->head_idx] = nh;
    g->grid[nh.y][nh.x] = CELL_SNAKE;

    /* 8. If eating, grow the snake and spawn new food. */
    if (eating) {
        g->snake_len++;
        g->score++;
        spawn_food(g);
    }

    g->tick++;
    return g->alive;
}
