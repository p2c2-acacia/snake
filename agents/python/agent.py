#!/usr/bin/env python3
"""
Naive snake agent – avoids immediate death, otherwise picks randomly.

Launches the snake game binary as a subprocess in agent mode and
communicates via the JSON line-protocol.

Usage:
    python agent.py [options]

Options are forwarded to the snake binary.  Extra agent flags:
    --games N       Run N games and print statistics (default: 1)
    --visual        Render each frame to stderr
    --game-path P   Path to snake binary (default: auto-detect ../../snake)
"""

import json
import os
import random
import subprocess
import sys
import time

# ── Direction helpers ────────────────────────────────────────────

DIR_ORDER = ["up", "right", "down", "left"]
DIR_DELTA = {"up": (0, -1), "right": (1, 0), "down": (0, 1), "left": (-1, 0)}


def apply_turn(direction: str, turn: str) -> str:
    """Apply a relative turn to an absolute direction."""
    idx = DIR_ORDER.index(direction)
    if turn == "left":
        return DIR_ORDER[(idx + 3) % 4]
    if turn == "right":
        return DIR_ORDER[(idx + 1) % 4]
    return direction


def next_head(head: list, direction: str) -> tuple:
    dx, dy = DIR_DELTA[direction]
    return (head[0] + dx, head[1] + dy)


# ── Safety check ────────────────────────────────────────────────

def is_safe(state: dict, turn: str) -> bool:
    """Return True if *turn* doesn't lead to immediate death."""
    new_dir = apply_turn(state["dir"], turn)
    hx, hy = next_head(state["snake"][0], new_dir)
    w, h = state["width"], state["height"]

    # Wall check
    if hx < 0 or hx >= w or hy < 0 or hy >= h:
        return False

    # Build set of body positions
    snake_set = set(tuple(p) for p in state["snake"])

    # If we will eat food the tail stays; otherwise the tail retracts.
    food = tuple(state["food"])
    if (hx, hy) != food:
        tail = tuple(state["snake"][-1])
        snake_set.discard(tail)

    return (hx, hy) not in snake_set


def choose_action(state: dict) -> str:
    """Pick a random safe action, or 'straight' if stuck."""
    actions = ["straight", "left", "right"]
    safe = [a for a in actions if is_safe(state, a)]
    return random.choice(safe) if safe else "straight"


# ── Visualisation (optional) ────────────────────────────────────

def render(state: dict) -> None:
    """Print a minimal ASCII view to stderr."""
    w, h = state["width"], state["height"]
    grid = [[" "] * w for _ in range(h)]

    for i, (x, y) in enumerate(state["snake"]):
        if 0 <= x < w and 0 <= y < h:
            grid[y][x] = "@" if i == 0 else "o"

    fx, fy = state["food"]
    if 0 <= fx < w and 0 <= fy < h:
        grid[fy][fx] = "*"

    lines = [
        f"\033[H\033[2J",                          # clear screen
        "+" + "-" * w + "+",
    ]
    for row in grid:
        lines.append("|" + "".join(row) + "|")
    lines.append("+" + "-" * w + "+")
    lines.append(
        f"Score: {state['score']}  Tick: {state['tick']}  "
        f"Dir: {state['dir']}  Alive: {state['alive']}"
    )
    print("\n".join(lines), file=sys.stderr, flush=True)


# ── Single game ─────────────────────────────────────────────────

def play_one_game(game_path: str, extra_args: list, visual: bool) -> dict:
    """
    Run one game via the snake binary.
    Returns {"score": int, "ticks": int}.
    """
    cmd = [game_path, "agent", "--step"] + extra_args
    proc = subprocess.Popen(
        cmd,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )

    result = {"score": 0, "ticks": 0}

    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            state = json.loads(line.strip())

            if visual:
                render(state)

            if not state["alive"]:
                result["score"] = state["score"]
                result["ticks"] = state["tick"]
                break

            action = choose_action(state)
            proc.stdin.write(action + "\n")
            proc.stdin.flush()
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()

    return result


# ── Pipe mode (stdin/stdout filter for TUI integration) ─────────

def run_pipe() -> None:
    """Read game state from stdin, write actions to stdout.

    Used by the snake TUI to drive this agent without launching a
    separate game subprocess.
    """
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            state = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not state.get("alive", False):
            break
        action = choose_action(state)
        sys.stdout.write(action + "\n")
        sys.stdout.flush()


# ── Entry point ─────────────────────────────────────────────────

def main() -> None:
    # --pipe: act as a stdin/stdout filter (for TUI integration)
    if "--pipe" in sys.argv:
        run_pipe()
        return

    # Default game binary path: ../../snake relative to this script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    default_game = os.path.join(script_dir, "..", "..", "snake")

    game_path = default_game
    num_games = 1
    visual = False
    extra_args: list[str] = []

    # Simple arg parsing (agent-specific flags extracted, rest forwarded)
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--games" and i + 1 < len(args):
            num_games = int(args[i + 1])
            i += 2
        elif args[i] == "--visual":
            visual = True
            i += 1
        elif args[i] == "--game-path" and i + 1 < len(args):
            game_path = args[i + 1]
            i += 2
        else:
            extra_args.append(args[i])
            i += 1

    # Verify the binary exists
    if not os.path.isfile(game_path):
        print(f"Error: game binary not found at {game_path}", file=sys.stderr)
        print("Build it first:  cd snake && make", file=sys.stderr)
        sys.exit(1)

    scores: list[int] = []
    ticks_list: list[int] = []

    for g in range(num_games):
        if num_games > 1 and not visual:
            print(f"Game {g + 1}/{num_games} ...", end=" ", flush=True)

        result = play_one_game(game_path, extra_args, visual)
        scores.append(result["score"])
        ticks_list.append(result["ticks"])

        if num_games > 1 and not visual:
            print(f"score={result['score']}  ticks={result['ticks']}")

    # Summary
    if num_games == 1:
        print(f"Score: {scores[0]}  Ticks: {ticks_list[0]}")
    else:
        avg_score = sum(scores) / len(scores)
        avg_ticks = sum(ticks_list) / len(ticks_list)
        print(f"\n{'='*40}")
        print(f"Games played : {num_games}")
        print(f"Avg score    : {avg_score:.1f}")
        print(f"Max score    : {max(scores)}")
        print(f"Min score    : {min(scores)}")
        print(f"Avg ticks    : {avg_ticks:.1f}")
        print(f"{'='*40}")


if __name__ == "__main__":
    main()
