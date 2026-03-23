#!/usr/bin/env python3
"""Minimal snake agent template (stdin/stdout only)."""

import json
import random
import sys
from typing import List, Tuple

DIR_ORDER = ["up", "right", "down", "left"]
DIR_DELTA = {"up": (0, -1), "right": (1, 0), "down": (0, 1), "left": (-1, 0)}


# ========================= DO NOT TOUCH: protocol glue =========================
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
    """Write exactly one action line."""
    sys.stdout.write(action + "\n")
    sys.stdout.flush()
# ======================= END DO NOT TOUCH: protocol glue =======================


def apply_turn(direction: str, turn: str) -> str:
    idx = DIR_ORDER.index(direction)
    if turn == "left":
        return DIR_ORDER[(idx + 3) % 4]
    if turn == "right":
        return DIR_ORDER[(idx + 1) % 4]
    return direction


def next_head(head: List[int], direction: str) -> Tuple[int, int]:
    dx, dy = DIR_DELTA[direction]
    return (head[0] + dx, head[1] + dy)


def choose_action(state: dict) -> str:
    """
    Beginner-friendly strategy:
    - try safe moves first (wall/body check using apples list if present)
    - pick one at random.
    """
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
