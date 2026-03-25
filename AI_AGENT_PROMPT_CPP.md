# Snake Game Agent Generator (C++)

> Paste this entire document into any LLM (Claude, GPT-4, Gemini, etc.) to generate a C++ snake-playing agent.

---

## Your Role

You are a code generator. Your task is to produce a **complete, runnable C++ snake agent** based on:

1. The game specification below
2. The template code provided
3. **Pseudocode or strategy steps that the user will provide after you acknowledge this prompt**

**Important:** Do NOT implement any strategy yet. Wait for the user to provide pseudocode or step-by-step instructions. Your job is to translate their pseudocode into working C++ code that fits the template.

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
```
// Death if next head position is outside bounds
if (nx < 0 || nx >= width || ny < 0 || ny >= height) {
    // DEAD - wall collision
}
```

#### Self Collision
```
// Death if next head position overlaps snake body
// EXCEPTION: The tail cell is safe if NOT eating an apple
// (because the tail retracts when not eating)

if (body contains (nx, ny)) {
    // Check if it's the tail and we're not eating
    if ((nx, ny) == tail_position && (nx, ny) not in apples) {
        // SAFE - tail will move away
    } else {
        // DEAD - self collision
    }
}
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

1. **Protocol glue** (marked `DO NOT TOUCH`) — handles stdin/stdout communication and JSON parsing
2. **Helper functions** — direction/turn utilities
3. **`choose_action` function** — where the user's strategy goes

**Your task:** Modify ONLY the `choose_action` function to implement the user's pseudocode. Keep everything else unchanged.

```cpp
/*
 * Minimal snake C++ agent template (stdin/stdout only).
 *
 * This file shows a very small example agent that reads JSON-like game
 * states from stdin and writes a single action per line to stdout. The
 * focus is on clarity for beginners rather than performance.
 */

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <set>
#include <string>
#include <vector>

// Simple 2D point used for coordinates on the board.
struct Point { int x, y; };

// GameState holds the minimal information the agent needs from the engine:
// - alive: whether the snake is still alive
// - width/height: board dimensions
// - dir: current direction of travel ("up"/"right"/...)
// - snake: list of points (head first)
// - apples: locations of apples on the board
struct GameState {
    bool alive = false;
    int width = 0;
    int height = 0;
    std::string dir = "right";
    std::vector<Point> snake;
    std::vector<Point> apples;
};

// ========================= DO NOT TOUCH: protocol glue =========================
// Protocol glue: parse_state reads a single input line from the engine and
// fills a GameState. emit_action below writes a single action line. These
// helpers implement the simple stdin/stdout protocol used by the runner.
static GameState parse_state(const std::string &line) {
    GameState s;
    const char *p = line.c_str();
    while (*p && *p != '{') ++p;
    if (!*p) return s;
    ++p;

    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r') ++p;
        if (*p == '}' || !*p) break;
        if (*p != '"') break;
        ++p;
        const char *ks = p;
        while (*p && *p != '"') ++p;
        std::string key(ks, p);
        if (*p == '"') ++p;
        while (*p == ' ' || *p == ':') ++p;

        if (key == "alive") {
            s.alive = (*p == 't');
            while (*p && *p != ',' && *p != '}') ++p;
        } else if (key == "width") {
            s.width = (int)strtol(p, const_cast<char **>(&p), 10);
        } else if (key == "height") {
            s.height = (int)strtol(p, const_cast<char **>(&p), 10);
        } else if (key == "dir") {
            if (*p == '"') ++p;
            const char *vs = p;
            while (*p && *p != '"') ++p;
            s.dir = std::string(vs, p);
            if (*p == '"') ++p;
        } else if (key == "snake" || key == "apples") {
            std::vector<Point> *target = (key == "snake") ? &s.snake : &s.apples;
            if (*p == '[') ++p;
            while (*p && *p != ']') {
                while (*p == ' ' || *p == ',') ++p;
                if (*p == ']') break;
                if (*p == '[') {
                    ++p;
                    Point pt;
                    pt.x = (int)strtol(p, const_cast<char **>(&p), 10);
                    if (*p == ',') ++p;
                    pt.y = (int)strtol(p, const_cast<char **>(&p), 10);
                    if (*p == ']') ++p;
                    target->push_back(pt);
                } else break;
            }
            if (*p == ']') ++p;
        } else {
            while (*p && *p != ',' && *p != '}') ++p;
        }
    }
    return s;
}

static void emit_action(const std::string &action) {
    // Write one action line and flush so the runner receives it immediately.
    std::cout << action << "\n";
    std::cout.flush();
}
// ======================= END DO NOT TOUCH: protocol glue =======================

// Direction helpers: DIR_ORDER defines clockwise order, DX/DY give movement
// deltas for each direction (used to compute the next head position).
static const char *DIR_ORDER[] = {"up", "right", "down", "left"};
static const int DIR_DX[] = {0, 1, 0, -1};
static const int DIR_DY[] = {-1, 0, 1, 0};

// Return index of a direction string within DIR_ORDER. Defaults to 1
// ("right") if the input is unrecognized.
static int dir_index(const std::string &d) {
    for (int i = 0; i < 4; i++) if (d == DIR_ORDER[i]) return i;
    return 1;
}

// Given a current absolute direction and a relative turn ("left", "right",
// or "straight"), return the new absolute direction string.
static std::string apply_turn(const std::string &dir, const std::string &turn) {
    int i = dir_index(dir);
    if (turn == "left") return DIR_ORDER[(i + 3) % 4];
    if (turn == "right") return DIR_ORDER[(i + 1) % 4];
    return dir;
}

// Compute the position of the head after moving one step in the given
// absolute direction.
static Point next_head(Point h, const std::string &dir) {
    int i = dir_index(dir);
    return {h.x + DIR_DX[i], h.y + DIR_DY[i]};
}

// ========================================
// IMPLEMENT YOUR STRATEGY IN THIS FUNCTION
// ========================================
// This function receives the current game state and must return one of:
// - "straight" : continue in current direction
// - "left"     : turn 90° counter-clockwise
// - "right"    : turn 90° clockwise
//
// Available information in GameState:
// - st.snake[0]    : head position (Point with .x and .y)
// - st.snake       : full body (head first, tail last)
// - st.apples      : list of apple positions
// - st.dir         : current direction ("up"/"right"/"down"/"left")
// - st.width       : board width
// - st.height      : board height
//
// Helper functions available:
// - apply_turn(direction, turn) -> new_direction
// - next_head(head, direction) -> Point
//
// Replace the code below with the user's strategy.
// ========================================
static std::string choose_action(const GameState &st) {
    // Default: random safe move
    if (st.snake.empty()) return "straight";
    
    std::set<std::pair<int, int>> body;
    for (const auto &p : st.snake) body.insert({p.x, p.y});

    std::set<std::pair<int, int>> apple_set;
    for (const auto &a : st.apples) apple_set.insert({a.x, a.y});

    std::vector<std::string> safe;
    for (const auto &turn : {"straight", "left", "right"}) {
        std::string nd = apply_turn(st.dir, turn);
        Point nh = next_head(st.snake.front(), nd);
        if (nh.x < 0 || nh.x >= st.width || nh.y < 0 || nh.y >= st.height) continue;
        auto temp = body;
        if (apple_set.find({nh.x, nh.y}) == apple_set.end() && !st.snake.empty()) {
            const auto &tail = st.snake.back();
            temp.erase({tail.x, tail.y});
        }
        if (temp.find({nh.x, nh.y}) != temp.end()) continue;
        safe.push_back(turn);
    }
    if (safe.empty()) return "straight";
    return safe[rand() % safe.size()];
}

int main() {
    srand((unsigned)time(nullptr));
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        GameState st = parse_state(line);
        if (!st.alive) break;
        emit_action(choose_action(st));
    }
    return 0;
}
```

---

## Instructions for the LLM

1. **Do NOT implement any strategy yet.** Wait for the user.

2. **When the user provides pseudocode or step-by-step instructions:**
   - Translate their logic faithfully into the `choose_action` function
   - Keep all other code unchanged (especially the protocol glue)
   - The output must be a complete, runnable C++ file

3. **Output format:**
   - Return the complete modified C++ code
   - The code should work immediately when saved as `agent.cpp` and compiled with:
     ```
     g++ -Wall -Wextra -O2 -std=c++17 -o agent.out agent.cpp
     ```

4. **Pseudocode translation rules:**
   - Follow the user's logic exactly as written
   - Use the helper functions provided (`apply_turn`, `next_head`, `dir_index`)
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
      
LLM: [outputs complete C++ code implementing that strategy]
```

---

## Ready

**If you understand this specification, respond with:**

> I have read the Snake Game Agent specification (C++). I am ready to implement your strategy. Please provide your pseudocode or step-by-step instructions for the `choose_action` function.
