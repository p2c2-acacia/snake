# Snake Game Agent Generator (Python)

> Paste this entire document into any LLM (Claude, GPT-4, Gemini, etc.) to generate a Python snake-playing agent.

---

## Your Role

You are a code generator. Your task is to produce a **complete, runnable Python snake agent** based on:

1. The game specification below
2. The template code provided
3. **Pseudocode or strategy steps that the user will provide after you acknowledge this prompt**

**Important:** Do NOT implement any strategy yet. Wait for the user to provide pseudocode or step-by-step instructions. Your job is to translate their pseudocode into working Python code that fits the template.

---

## Game Specification

### Overview

- Snake moves on a 2D grid
- Goal: eat apples to grow and pass the stage
- Death occurs on wall collision or self-collision
- Snake grows by 1 segment when eating an apple
- Snake cannot reverse direction (180° turn is impossible)

### Coordinate System

```
(0,0) ──────────────► x
  │
  │
  │
  ▼
  y
```

- Origin `(0, 0)` is **top-left corner**
- x increases to the right
- y increases downward
- Valid coordinates: `0 <= x < width` and `0 <= y < height`

### Directions (Absolute)

The game uses four absolute directions in clockwise order:

| Direction | Delta (dx, dy) | Description |
|-----------|----------------|-------------|
| `up`      | `(0, -1)`      | Move toward top of screen |
| `right`   | `(1, 0)`       | Move toward right |
| `down`    | `(0, 1)`       | Move toward bottom |
| `left`    | `(-1, 0)`      | Move toward left |

### Actions (Relative Turns)

The agent outputs **relative turns**, not absolute directions:

| Action | Effect |
|--------|--------|
| `straight` | Continue in current direction (no turn) |
| `left` | Turn 90° counter-clockwise |
| `right` | Turn 90° clockwise |

**Turn examples:**

```
Current direction: "right"
  - "straight" → still "right"
  - "left"     → "up"
  - "right"    → "down"

Current direction: "up"
  - "straight" → still "up"
  - "left"     → "left"
  - "right"    → "right"
```

**Constraint:** A 180° reversal is impossible. If the current direction is `"right"`, turning `"left"` twice (which would go to `"left"`) is allowed, but the game engine prevents immediate reversal.

### State Format (JSON Input)

Each tick, your agent receives one JSON object on stdin:

```json
{
  "tick": 12,
  "alive": true,
  "score": 3,
  "apples_eaten": 3,
  "goal_apples": 10,
  "difficulty": 5,
  "outcome": "running",
  "score_mode": "pass_fail",
  "width": 20,
  "height": 20,
  "dir": "right",
  "snake": [[10, 10], [9, 10], [8, 10]],
  "apples": [[13, 7], [5, 3]]
}
```

**Field descriptions:**

| Field | Type | Description |
|-------|------|-------------|
| `tick` | int | Current game tick (starts at 0) |
| `alive` | bool | `true` if snake is alive, `false` on death/win |
| `score` | int | Current score |
| `apples_eaten` | int | Total apples consumed |
| `goal_apples` | int | Apples needed to pass (stage-dependent) |
| `difficulty` | int | Stage number 1-7 |
| `outcome` | string | `"running"`, `"pass"`, or `"fail"` |
| `score_mode` | string | `"pass_fail"` or `"time_rank"` |
| `width` | int | Board width in cells |
| `height` | int | Board height in cells |
| `dir` | string | Current direction: `"up"`, `"right"`, `"down"`, or `"left"` |
| `snake` | array | Snake body as `[[x, y], ...]` — **head is first element** |
| `apples` | array | Apple positions as `[[x, y], ...]` |

### Collision Rules

#### Wall Collision
```python
# Death if next head position is outside bounds
if nx < 0 or nx >= width or ny < 0 or ny >= height:
    # DEAD - wall collision
```

#### Self Collision
```python
# Death if next head position overlaps snake body
# EXCEPTION: The tail cell is safe if NOT eating an apple
# (because the tail retracts when not eating)

if (nx, ny) in body:
    # Check if it's the tail and we're not eating
    if (nx, ny) == tail_position and (nx, ny) not in apples:
        # SAFE - tail will move away
    else:
        # DEAD - self collision
```

### Difficulty Stages

| Stage | Description | Win Condition |
|-------|-------------|---------------|
| 1 | Snake starts in corner, one apple in center | Eat the apple |
| 2 | Snake starts center, fixed apples at fixed positions | Eat all apples |
| 3 | Snake starts center (random direction), one random apple | Eat the apple |
| 4 | Snake starts center, multiple fixed apples | Eat all apples |
| 5 | Snake starts center, apples regenerate | Eat `goal_apples` apples |
| 6 | Multiple regenerating apples | Eat `goal_apples` apples (time ranking) |
| 7 | Increasing number of apples over time | Eat `goal_apples` apples (time ranking) |

**Stages 1-5:** Pass/fail  
**Stages 6-7:** Time ranking (fewer ticks = better score)

---

## Protocol Specification

### Input
- One JSON object per line on stdin
- One line per game tick
- Game ends when `alive` is `false`

### Output
- Print exactly one action word per tick: `straight`, `left`, or `right`
- Lowercase, no quotes, no extra whitespace
- Must flush stdout after printing

### Flow
```
Loop:
  1. Read one line from stdin
  2. Parse JSON
  3. If alive is false: exit
  4. Compute action based on state
  5. Print action to stdout
  6. Flush stdout
```

---

## Template Code

Below is the complete template. It contains:

1. **Protocol glue** (marked `DO NOT TOUCH`) — handles stdin/stdout communication
2. **Helper functions** — direction/turn utilities
3. **`choose_action` function** — where the user's strategy goes

**Your task:** Modify ONLY the `choose_action` function to implement the user's pseudocode. Keep everything else unchanged.

```python
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
    """
    Given a current direction and a relative turn ('left'/'right'/'straight'),
    return the resulting absolute direction string.
    """
    idx = DIR_ORDER.index(direction)
    if turn == "left":
        return DIR_ORDER[(idx + 3) % 4]
    if turn == "right":
        return DIR_ORDER[(idx + 1) % 4]
    return direction


def next_head(head: List[int], direction: str) -> Tuple[int, int]:
    """
    Compute the coordinates of the snake's head after moving one step in
    the given direction. Head is [x, y]. Returns a (x, y) tuple.
    """
    dx, dy = DIR_DELTA[direction]
    return (head[0] + dx, head[1] + dy)


def choose_action(state: dict) -> str:
    """
    ========================================
    IMPLEMENT YOUR STRATEGY HERE
    ========================================
    
    This function receives the current game state and must return one of:
    - "straight" : continue in current direction
    - "left"     : turn 90° counter-clockwise
    - "right"    : turn 90° clockwise
    
    Available information in `state`:
    - state["snake"][0]  : head position [x, y]
    - state["snake"]     : full body (head first, tail last)
    - state["apples"]    : list of apple positions [[x, y], ...]
    - state["dir"]       : current direction ("up"/"right"/"down"/"left")
    - state["width"]     : board width
    - state["height"]    : board height
    
    Helper functions available:
    - apply_turn(direction, turn) -> new_direction
    - next_head(head, direction) -> (x, y)
    
    Replace the code below with the user's strategy.
    ========================================
    """
    # Default: random safe move
    snake = [tuple(p) for p in state.get("snake", [])]
    if not snake:
        return "straight"
    head = snake[0]
    body = set(snake)
    apple_set = {tuple(a) for a in state.get("apples", [])}
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
```

---

## Instructions for the LLM

1. **Do NOT implement any strategy yet.** Wait for the user.

2. **When the user provides pseudocode or step-by-step instructions:**
   - Translate their logic faithfully into the `choose_action` function
   - Keep all other code unchanged (especially the protocol glue)
   - The output must be a complete, runnable Python file

3. **Output format:**
   - Return the complete modified Python code
   - The code should work immediately when saved as `agent.py`

4. **Pseudocode translation rules:**
   - Follow the user's logic exactly as written
   - Use the helper functions provided (`apply_turn`, `next_head`)
   - Return only `"straight"`, `"left"`, or `"right"`

---

## Expected Workflow

```
USER: [pastes this entire document]

LLM: I have read the Snake Game Agent specification and template. 
     I am ready to implement your strategy.
     
     Please provide your pseudocode or step-by-step instructions for 
     the choose_action function.

USER: Here is my strategy:
      
      1. Find the nearest apple
      2. Calculate which turn gets me closer to it
      3. If that turn is safe, take it
      4. Otherwise, pick any safe turn randomly
      
LLM: [outputs complete Python code implementing that strategy]
```

---

## Ready

**If you understand this specification, respond with:**

> I have read the Snake Game Agent specification (Python). I am ready to implement your strategy. Please provide your pseudocode or step-by-step instructions for the `choose_action` function.

