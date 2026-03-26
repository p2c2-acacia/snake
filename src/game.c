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
static const char *OUTCOME_NAMES[] = { "running", "pass", "fail" };
static const char *SCORE_MODE_NAMES[] = { "pass_fail", "time_rank" };

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

const char *game_outcome_name(GameOutcome outcome) {
    int idx = (int)outcome;
    if (idx < 0 || idx > 2) idx = 0;
    return OUTCOME_NAMES[idx];
}

const char *game_score_mode_name(ScoreMode mode) {
    int idx = (int)mode;
    if (idx < 0 || idx > 1) idx = 0;
    return SCORE_MODE_NAMES[idx];
}

void game_rules_default(GameRules *rules) {
    if (!rules) return;
    memset(rules, 0, sizeof(*rules));
    rules->stage                 = 0;   /* classic mode */
    rules->stage2_fixed_apples   = 4;
    rules->stage4_fixed_apples   = 4;
    rules->stage5_goal_apples    = 10;
    rules->stage6_fixed_apples   = 3;
    /* No apple goal for stage 6 (infinite / play until death or fill). */
    rules->stage6_goal_apples    = 0;
    rules->stage7_initial_apples = 2;
    rules->stage7_increment_every = 5;
    rules->stage7_increment_by   = 1;
    rules->stage7_max_apples     = 8;
    /* No apple goal for stage 7 (infinite / play until death or fill). */
    rules->stage7_goal_apples    = 0;

    /* Modifiers (all off by default). */
    rules->poison_count          = 0;
    rules->obstacle_count        = 0;
    rules->vision_radius         = 0;
    rules->apple_decay_ticks     = 0;
    rules->initial_snake_len     = 0;
    rules->tick_penalty_interval = 0;
    rules->score_multiplier      = 1.0;
}

/* ── Internal helpers ───────────────────────────────────────── */

static int clamp_int(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static int point_eq(Point a, Point b) {
    return a.x == b.x && a.y == b.y;
}

static int is_empty_cell(const SnakeGame *g, int x, int y) {
    return x >= 0 && x < g->width && y >= 0 && y < g->height &&
           g->grid[y][x] == CELL_EMPTY;
}

static int add_apple_at(SnakeGame *g, Point p) {
    if (g->apple_count >= MAX_APPLES) return 0;
    if (!is_empty_cell(g, p.x, p.y)) return 0;
    g->apple_spawn_tick[g->apple_count] = g->tick;
    g->apples[g->apple_count++] = p;
    g->grid[p.y][p.x] = CELL_APPLE;

    return 1;
}

static int remove_apple_at(SnakeGame *g, Point p) {
    for (int i = 0; i < g->apple_count; i++) {
        if (!point_eq(g->apples[i], p)) continue;
        g->grid[p.y][p.x] = CELL_EMPTY;
        for (int j = i; j + 1 < g->apple_count; j++) {
            g->apples[j] = g->apples[j + 1];
            g->apple_spawn_tick[j] = g->apple_spawn_tick[j + 1];
        }
        g->apple_count--;
        return 1;
    }
    return 0;
}

static int count_empty_cells(const SnakeGame *g) {
    int n = 0;
    for (int y = 0; y < g->height; y++) {
        for (int x = 0; x < g->width; x++) {
            if (g->grid[y][x] == CELL_EMPTY) n++;
        }
    }
    return n;
}

static int spawn_random_apple(SnakeGame *g) {
    int empty = count_empty_cells(g);
    if (empty <= 0) return 0;

    int pick = rand() % empty;
    for (int y = 0; y < g->height; y++) {
        for (int x = 0; x < g->width; x++) {
            if (g->grid[y][x] != CELL_EMPTY) continue;
            if (pick-- == 0) return add_apple_at(g, (Point){x, y});
        }
    }
    return 0;
}

static void mark_pass(SnakeGame *g) {
    g->alive = 0;
    g->outcome = GAME_OUTCOME_PASS;
}

static void ensure_target_apples(SnakeGame *g, int target_count) {
    target_count = clamp_int(target_count, 0, g->width * g->height);
    while (g->apple_count < target_count) {
        if (!spawn_random_apple(g)) {
            mark_pass(g);
            break;
        }
    }
}

/* ── Poison helpers ─────────────────────────────────────────── */

static int add_poison_at(SnakeGame *g, Point p) {
    if (g->poison_count >= MAX_APPLES) return 0;
    if (!is_empty_cell(g, p.x, p.y)) return 0;
    g->poison[g->poison_count++] = p;
    g->grid[p.y][p.x] = CELL_POISON;
    return 1;
}

static int spawn_random_poison(SnakeGame *g) {
    int empty = count_empty_cells(g);
    if (empty <= 0) return 0;
    int pick = rand() % empty;
    for (int y = 0; y < g->height; y++)
        for (int x = 0; x < g->width; x++) {
            if (g->grid[y][x] != CELL_EMPTY) continue;
            if (pick-- == 0) return add_poison_at(g, (Point){x, y});
        }
    return 0;
}

static void ensure_target_poison(SnakeGame *g, int target) {
    target = clamp_int(target, 0, g->width * g->height);
    while (g->poison_count < target)
        if (!spawn_random_poison(g)) break;
}

/* ── Obstacle helpers ──────────────────────────────────────── */

static int add_obstacle_at(SnakeGame *g, Point p) {
    if (g->obstacle_count >= MAX_APPLES) return 0;
    if (!is_empty_cell(g, p.x, p.y)) return 0;
    g->obstacles[g->obstacle_count++] = p;
    g->grid[p.y][p.x] = CELL_WALL_BLOCK;
    return 1;
}

static int spawn_random_obstacle(SnakeGame *g) {
    int empty = count_empty_cells(g);
    if (empty <= 0) return 0;
    int pick = rand() % empty;
    for (int y = 0; y < g->height; y++)
        for (int x = 0; x < g->width; x++) {
            if (g->grid[y][x] != CELL_EMPTY) continue;
            if (pick-- == 0) return add_obstacle_at(g, (Point){x, y});
        }
    return 0;
}

static void ensure_target_obstacles(SnakeGame *g, int target) {
    target = clamp_int(target, 0, g->width * g->height);
    while (g->obstacle_count < target)
        if (!spawn_random_obstacle(g)) break;
}

/* ── Score multiplier computation ──────────────────────────── */

static double compute_score_multiplier(const GameRules *r) {
    double m = 1.0;
    if (r->obstacle_count > 0)        m *= 1.3;
    if (r->poison_count > 0)          m *= 1.5;
    if (r->vision_radius > 0)         m *= 1.8;
    if (r->apple_decay_ticks > 0)     m *= 1.4;
    if (r->initial_snake_len > 3)     m *= 1.2;
    if (r->tick_penalty_interval > 0) m *= 1.3;
    return m;
}

static void place_snake(SnakeGame *g, Point head, Direction dir, int len) {
    len = clamp_int(len, 1, MAX_SNAKE_LEN);
    g->target_snake_len = len;
    g->snake_len = 1;          /* start with head only */
    g->head_idx = 0;
    g->dir = dir;
    g->pending_turn = TURN_STRAIGHT;

    g->body[0] = head;
    g->grid[head.y][head.x] = CELL_SNAKE;
}

static void spawn_stage2_fixed(SnakeGame *g, int count) {
    Point candidates[12];
    int n = 0;
    candidates[n++] = (Point){1, 1};
    candidates[n++] = (Point){g->width - 2, 1};
    candidates[n++] = (Point){1, g->height - 2};
    candidates[n++] = (Point){g->width - 2, g->height - 2};
    candidates[n++] = (Point){g->width / 2, 1};
    candidates[n++] = (Point){g->width / 2, g->height - 2};
    candidates[n++] = (Point){1, g->height / 2};
    candidates[n++] = (Point){g->width - 2, g->height / 2};
    candidates[n++] = (Point){g->width / 3, 1};
    candidates[n++] = (Point){(2 * g->width) / 3, g->height - 2};
    candidates[n++] = (Point){1, g->height / 3};
    candidates[n++] = (Point){g->width - 2, (2 * g->height) / 3};

    for (int i = 0; i < n && g->apple_count < count; i++)
        add_apple_at(g, candidates[i]);

    while (g->apple_count < count) {
        int added = 0;
        for (int y = 1; y < g->height - 1 && g->apple_count < count; y++) {
            for (int x = 1; x < g->width - 1 && g->apple_count < count; x++) {
                if (((x + y) % 3) != 0) continue;
                if (add_apple_at(g, (Point){x, y})) added = 1;
            }
        }
        if (!added && !spawn_random_apple(g)) break;
    }
}

static void spawn_stage_initial(SnakeGame *g) {
    int cx = g->width / 2;
    int cy = g->height / 2;
    Direction random_dir = (Direction)(rand() % 4);
    int stage = g->rules.stage;
    int slen = (g->rules.initial_snake_len > 0) ? g->rules.initial_snake_len : 3;

    g->score_mode = (stage >= 6) ? SCORE_TIME_RANK : SCORE_PASS_FAIL;
    /* Stages 1-5 and 8-9 are pass/fail. */
    if (stage >= 1 && stage <= 5) g->score_mode = SCORE_PASS_FAIL;
    if (stage == 8 || stage == 9)  g->score_mode = SCORE_PASS_FAIL;

    g->goal_apples = 0;
    g->stage7_active_apples = 0;

    /* Compute multiplier from active modifiers. */
    g->rules.score_multiplier = compute_score_multiplier(&g->rules);

    if (stage == 1) {
        place_snake(g, (Point){2, 0}, DIR_RIGHT, slen);
        add_apple_at(g, (Point){cx, cy});
        return;
    }

    if (stage == 2) {
        place_snake(g, (Point){cx, cy}, DIR_RIGHT, slen);
        spawn_stage2_fixed(g, clamp_int(g->rules.stage2_fixed_apples, 1, MAX_APPLES));
        return;
    }

    if (stage == 3) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        spawn_random_apple(g);
        return;
    }

    if (stage == 4) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        ensure_target_apples(g, clamp_int(g->rules.stage4_fixed_apples, 1, MAX_APPLES));
        return;
    }

    if (stage == 5) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_apples(g, 1);
        return;
    }

    if (stage == 6) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = 0;
        ensure_target_apples(g, clamp_int(g->rules.stage6_fixed_apples, 1, MAX_APPLES));
        return;
    }

    if (stage == 7) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = 0;
        g->stage7_active_apples =
            clamp_int(g->rules.stage7_initial_apples, 1, MAX_APPLES);
        ensure_target_apples(g, g->stage7_active_apples);
        return;
    }

    /* ── Stage 8: Obstacles ────────────────────────────────────── */
    if (stage == 8) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_obstacles(g, g->rules.obstacle_count);
        ensure_target_apples(g, 1);
        return;
    }

    /* ── Stage 9: Poison + Obstacles ───────────────────────────── */
    if (stage == 9) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_obstacles(g, g->rules.obstacle_count);
        ensure_target_poison(g, g->rules.poison_count);
        ensure_target_apples(g, 1);
        return;
    }

    /* ── Stage 10: Fog of war ──────────────────────────────────── */
    if (stage == 10) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_apples(g, 1);
        return;
    }

    /* ── Stage 11: Decay + obstacles + tick penalty ────────────── */
    if (stage == 11) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_obstacles(g, g->rules.obstacle_count);
        ensure_target_apples(g, 1);
        return;
    }

    /* ── Stage 12: Everything ──────────────────────────────────── */
    if (stage == 12) {
        place_snake(g, (Point){cx, cy}, random_dir, slen);
        g->goal_apples = clamp_int(g->rules.stage5_goal_apples, 1, MAX_APPLES);
        ensure_target_obstacles(g, g->rules.obstacle_count);
        ensure_target_poison(g, g->rules.poison_count);
        ensure_target_apples(g, 1);
        return;
    }

    /* Classic fallback: center spawn, facing right, one regenerating apple. */
    place_snake(g, (Point){cx, cy}, DIR_RIGHT, slen);
    ensure_target_apples(g, 1);
}

static void on_apple_eaten(SnakeGame *g) {
    int stage = g->rules.stage;

    g->last_eat_tick = g->tick;

    if (stage >= 1 && stage <= 4) {
        if (g->apple_count == 0) mark_pass(g);
        return;
    }

    if (stage == 5) {
        if (g->apples_eaten >= g->goal_apples) {
            mark_pass(g);
            return;
        }
        ensure_target_apples(g, 1);
        return;
    }

    if (stage == 6) {
        ensure_target_apples(g, clamp_int(g->rules.stage6_fixed_apples, 1, MAX_APPLES));
        return;
    }

    if (stage == 7) {
        int every = clamp_int(g->rules.stage7_increment_every, 1, 1000000);
        int by = clamp_int(g->rules.stage7_increment_by, 1, MAX_APPLES);
        int max_apples = clamp_int(g->rules.stage7_max_apples, 1, MAX_APPLES);

        if ((g->apples_eaten % every) == 0) {
            g->stage7_active_apples =
                clamp_int(g->stage7_active_apples + by, 1, max_apples);
        }

        ensure_target_apples(g, g->stage7_active_apples);
        return;
    }

    /* ── Stages 8-12: goal-based with regenerating apples ──── */
    if (stage >= 8 && stage <= 12) {
        if (g->goal_apples > 0 && g->apples_eaten >= g->goal_apples) {
            mark_pass(g);
            return;
        }
        ensure_target_apples(g, 1);
        /* Maintain poison count (poison consumed by decay or other logic). */
        if (g->rules.poison_count > 0)
            ensure_target_poison(g, g->rules.poison_count);
        return;
    }

    /* Classic fallback */
    ensure_target_apples(g, 1);
}

/* ── Public API ─────────────────────────────────────────────── */

void game_init(SnakeGame *g, int width, int height, unsigned int seed) {
    GameRules rules;
    game_rules_default(&rules);
    game_init_with_rules(g, width, height, seed, &rules);
}

void game_init_with_rules(SnakeGame *g, int width, int height,
                          unsigned int seed, const GameRules *rules) {
    GameRules local_rules;
    game_rules_default(&local_rules);
    if (rules) local_rules = *rules;

    width = clamp_int(width, 5, MAX_GRID_DIM);
    height = clamp_int(height, 5, MAX_GRID_DIM);

    memset(g, 0, sizeof(*g));

    g->width  = width;
    g->height = height;
    g->alive  = 1;
    g->outcome = GAME_OUTCOME_RUNNING;
    g->score_mode = SCORE_PASS_FAIL;
    g->rules = local_rules;
    g->pending_turn = TURN_STRAIGHT;
    srand(seed ? seed : (unsigned)time(NULL));
    spawn_stage_initial(g);
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
        g->outcome = GAME_OUTCOME_FAIL;
        return 0;
    }

    /* 3b. Wall-block collision. */
    if (g->grid[nh.y][nh.x] == CELL_WALL_BLOCK) {
        g->alive = 0;
        g->outcome = GAME_OUTCOME_FAIL;
        return 0;
    }

    /* 3c. Poison collision. */
    if (g->grid[nh.y][nh.x] == CELL_POISON) {
        g->alive = 0;
        g->outcome = GAME_OUTCOME_FAIL;
        return 0;
    }

    /* 4. Will we eat an apple this step? */
    int eating = (g->grid[nh.y][nh.x] == CELL_APPLE);
    if (eating) remove_apple_at(g, nh);

    /* 5. If NOT eating, retract the tail first (frees its cell so that
     *    chasing the tail is a legal move).
     *    Exception: while still spawning (snake_len < target_snake_len),
     *    skip retraction so the snake grows by 1 each tick. */
    int spawning = (g->snake_len < g->target_snake_len);
    if (!eating && !spawning) {
        int tail_idx = (g->head_idx - g->snake_len + 1 + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
        g->grid[g->body[tail_idx].y][g->body[tail_idx].x] = CELL_EMPTY;
    }

    /* 6. Self-collision (checked AFTER tail retraction). */
    if (g->grid[nh.y][nh.x] == CELL_SNAKE) {
        g->alive = 0;
        g->outcome = GAME_OUTCOME_FAIL;
        return 0;
    }

    /* 7. Place the new head. */
    g->head_idx = (g->head_idx + 1) % MAX_SNAKE_LEN;
    g->body[g->head_idx] = nh;
    g->grid[nh.y][nh.x] = CELL_SNAKE;

    /* 8. If spawning (growing to target length), increase snake_len. */
    if (spawning) g->snake_len++;

    /* 9. If eating, grow the snake and apply stage-specific apple logic. */
    if (eating) {
        g->snake_len++;
        g->target_snake_len++;  /* eating always extends the target too */
        g->score++;
        g->apples_eaten++;
        on_apple_eaten(g);
        if (g->snake_len >= g->width * g->height) {
            mark_pass(g);
            return g->alive;
        }
    }

    g->tick++;

    /* 9. Apple decay: remove apples that have lived too long. */
    if (g->alive && g->rules.apple_decay_ticks > 0) {
        for (int i = g->apple_count - 1; i >= 0; i--) {
            if (g->tick - g->apple_spawn_tick[i] >= g->rules.apple_decay_ticks) {
                remove_apple_at(g, g->apples[i]);
            }
        }
        /* Replenish to at least 1 apple so the game remains winnable. */
        if (g->apple_count == 0 && g->alive)
            ensure_target_apples(g, 1);
    }

    /* 10. Tick penalty: score decreases periodically when not eating. */
    if (g->alive && g->rules.tick_penalty_interval > 0 &&
        g->tick > 0 && (g->tick % g->rules.tick_penalty_interval) == 0) {
        if (g->tick != g->last_eat_tick && g->score > 0)
            g->score--;
    }

    return g->alive;
}
