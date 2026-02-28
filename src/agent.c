/*
 * agent.c – Built-in naive snake agent.
 *
 * Strategy: enumerate all three possible turns (straight, left, right),
 * filter to those that don't cause immediate death (wall or self-collision
 * on the very next tick), and pick one at random.  Correctly accounts for
 * the tail retracting when not eating food.
 */

#include "agent.h"
#include <stdlib.h>

static int is_turn_safe(const SnakeGame *g, Turn t) {
    Direction new_dir = direction_apply_turn(g->dir, t);
    Point d   = direction_delta(new_dir);
    Point head = g->body[g->head_idx];
    Point nh  = {head.x + d.x, head.y + d.y};

    /* Wall collision. */
    if (nh.x < 0 || nh.x >= g->width || nh.y < 0 || nh.y >= g->height)
        return 0;

    int eating = (g->grid[nh.y][nh.x] == CELL_FOOD);

    /* Self-collision (cell occupied by snake body). */
    if (g->grid[nh.y][nh.x] == CELL_SNAKE) {
        /* If we're NOT eating, the tail retracts this tick.
         * Moving into the tail cell is therefore safe. */
        if (!eating) {
            int tail_idx = (g->head_idx - g->snake_len + 1
                            + MAX_SNAKE_LEN) % MAX_SNAKE_LEN;
            if (nh.x == g->body[tail_idx].x && nh.y == g->body[tail_idx].y)
                return 1;
        }
        return 0;
    }

    return 1;
}

Turn naive_agent_decide(const SnakeGame *g) {
    Turn options[] = {TURN_STRAIGHT, TURN_LEFT, TURN_RIGHT};
    Turn safe[3];
    int  n = 0;

    for (int i = 0; i < 3; i++) {
        if (is_turn_safe(g, options[i]))
            safe[n++] = options[i];
    }

    if (n == 0) return TURN_STRAIGHT;
    return safe[rand() % n];
}
