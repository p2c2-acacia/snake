#!/usr/bin/env python3
"""Minimal snake agent template (stdin/stdout only)."""

import json
import random
import sys
from typing import List, Tuple

# Direction helpers:
# - DIR_ORDER defines a clockwise ordering used to compute left/right turns.
# - DIR_DELTA maps a direction name to its movement delta (dx, dy).
DIR_ORDER = ["up", "right", "down", "left"]
DIR_DELTA = {"up": (0, -1), "right": (1, 0), "down": (0, 1), "left": (-1, 0)}


# ========================= DO NOT TOUCH: protocol glue =========================
# Protocol glue: functions below handle the line-based stdin/stdout protocol
# used by the game runner. Beginners can ignore these when writing strategy.
def iter_json_states():
    """Yield JSON states from stdin. Ignores invalid lines safely."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            yield json.loads(line)
        except json.JSONDecodeError:
            continue


def emit_action(action: str) -> None:
    """Write exactly one action line.

    The engine expects exactly one action per line on stdout (e.g. "left").
    """
    sys.stdout.write(action + "\n")
    sys.stdout.flush()
# ======================= END DO NOT TOUCH: protocol glue =======================


def apply_turn(direction: str, turn: str) -> str:
    # Given a current direction and a relative turn ('left'/'right'/'straight'),
    # return the resulting absolute direction string.
    idx = DIR_ORDER.index(direction)
    if turn == "left":
        return DIR_ORDER[(idx + 3) % 4]
    if turn == "right":
        return DIR_ORDER[(idx + 1) % 4]
    return direction


def next_head(head: List[int], direction: str) -> Tuple[int, int]:
    # Compute the coordinates of the snake's head after moving one step in
    # the given direction. Head is [x, y]. Returns a (x, y) tuple.
    dx, dy = DIR_DELTA[direction]
    return (head[0] + dx, head[1] + dy)


def choose_action(state: dict) -> str:
    """
    Beginner-friendly strategy:
    - try safe moves first (wall/body check using apples list if present)
    - pick one at random.
    """
    # High-level notes for beginners:
    # - The agent inspects the current snake, food, and board size.
    # - For each candidate turn (straight/left/right) it checks for collisions
    #   with walls or the snake body and rejects unsafe moves.
    # - If the next cell contains food, we allow moving there even if it's the
    #   current tail (tail won't be discarded when eating).
    snake = [tuple(p) for p in state.get("snake", [])]
    if not snake:
        return "straight"
    head = snake[0]
    body = set(snake)

    apples = state.get("apples")
    if isinstance(apples, list) and apples:
        apple_set = {tuple(a) for a in apples}
    else:
        food = state.get("food", [-1, -1])
        apple_set = {tuple(food)}

    w = int(state.get("width", 0))
    h = int(state.get("height", 0))
    direction = state.get("dir", "right")

    candidates = []
    for turn in ("straight", "left", "right"):
        nd = apply_turn(direction, turn)
        nx, ny = next_head(list(head), nd)
        if nx < 0 or nx >= w or ny < 0 or ny >= h:
            continue
        temp_body = set(body)
        if (nx, ny) not in apple_set and snake:
            temp_body.discard(snake[-1])
        if (nx, ny) in temp_body:
            continue
        candidates.append(turn)
    return random.choice(candidates) if candidates else "straight"


def main() -> None:
    for state in iter_json_states():
        if not state.get("alive", False):
            break
        emit_action(choose_action(state))


if __name__ == "__main__":
    main()
