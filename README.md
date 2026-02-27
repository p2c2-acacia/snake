# Snake

A terminal-based Snake game written in C with an **agent API** for programmatic play.

## Features

| Feature | Details |
|---|---|
| **Interactive TUI** | ncurses-based; arrow keys / WASD; colour support |
| **Agent mode** | JSON line-protocol on stdin/stdout — language-agnostic |
| **Tick modes** | *Timed* (real-time) or *Step* (advance per input) |
| **Configurable** | Grid size, speed, random seed — all set before the game starts |
| **Performance** | Ring-buffer snake body + O(1) grid collision checks |

## Quick start

```bash
# Build the game
make

# Play interactively (timed mode, 200 ms per tick)
./snake

# Play in step mode (one tick per keypress)
./snake play --step

# Run the Python agent
cd agents/python && python agent.py

# Run the C++ agent
cd agents/cpp && make && ./agent
```

## Building

**Prerequisites:** GCC, Make, ncurses development headers.

```bash
# Debian / Ubuntu
sudo apt-get install build-essential libncurses-dev

# Fedora
sudo dnf install gcc make ncurses-devel

# macOS (ncurses ships with Xcode CLI tools)
xcode-select --install
```

Then:

```bash
make        # produces ./snake
make clean  # remove build artefacts
```

## Usage

```
./snake [play|agent] [options]

Modes:
  play          Interactive ncurses TUI (default)
  agent         JSON line-protocol on stdin/stdout

Tick modes:
  --step        Advance one tick per input  (default for agent)
  --timed       Advance every --speed ms    (default for play)

Options:
  --speed  N    Tick interval in ms         (default: 200)
  --width  N    Grid width in cells         (default: 20, range 5..100)
  --height N    Grid height in cells        (default: 20, range 5..100)
  --seed   N    Random seed (0 = time)      (default: 0)
  --help        Show help
```

### Interactive controls

| Key | Action |
|---|---|
| Arrow keys / WASD | Change direction |
| Space / Enter | Advance one tick without turning (step mode) |
| Q | Quit |

## Agent API

In agent mode (`./snake agent`) the game communicates via **JSON lines** on
stdin/stdout.

### Protocol

1. The game writes a **state line** (JSON) to stdout.
2. The agent reads it, decides, and writes a **command** to stdin.
3. In step mode the game waits for the command; in timed mode it advances
   on a timer and reads commands opportunistically.
4. When `"alive"` is `false` the game outputs the final state and exits.

### State format

```json
{
  "tick": 42,
  "alive": true,
  "score": 5,
  "width": 20,
  "height": 20,
  "dir": "right",
  "snake": [[12,10],[11,10],[10,10]],
  "food": [15,7]
}
```

| Field | Type | Description |
|---|---|---|
| `tick` | int | Steps elapsed |
| `alive` | bool | `false` on death or board-full win |
| `score` | int | Food items eaten |
| `width`, `height` | int | Grid dimensions |
| `dir` | string | Current heading: `"up"`, `"right"`, `"down"`, `"left"` |
| `snake` | `[[x,y],…]` | Body from head to tail |
| `food` | `[x,y]` | Food position (`[-1,-1]` if board is full) |

### Commands

Write **one line** to stdin:

| Command | Effect |
|---|---|
| `straight` | Continue in current direction |
| `left` | Turn 90° counter-clockwise (relative) |
| `right` | Turn 90° clockwise (relative) |

Turns are **relative** to the snake's heading, not absolute directions.

## Agents

### Python (`agents/python/agent.py`)

```bash
cd agents/python
python agent.py                       # single game
python agent.py --games 100           # benchmark 100 games
python agent.py --visual              # show ASCII rendering to stderr
python agent.py --seed 42             # deterministic game
python agent.py --game-path ../../snake  # custom binary path
```

### C++ (`agents/cpp/agent.cpp`)

```bash
cd agents/cpp
make
./agent                               # single game
./agent --games 100                   # benchmark
./agent --visual                      # ASCII rendering
./agent --seed 42                     # deterministic game
./agent --game-path ../../snake       # custom binary path
```

Both agents implement the same strategy: **pick a random action that doesn't
cause immediate death** (wall or self-collision on the very next tick).  They
correctly account for the tail retracting when not eating food.

### Writing your own agent

Any language that can spawn a subprocess and do line-buffered I/O works:

```python
import subprocess, json

proc = subprocess.Popen(
    ["./snake", "agent", "--step"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1,
)

while True:
    state = json.loads(proc.stdout.readline())
    if not state["alive"]:
        break
    proc.stdin.write("straight\n")   # ← your logic here
    proc.stdin.flush()
```

## Architecture

```
snake/
├── Makefile                  Build the game binary
├── src/
│   ├── game.h                Structures & API declarations
│   ├── game.c                Core logic (ring-buffer, grid, tick)
│   └── main.c                CLI, ncurses TUI, agent JSON I/O
└── agents/
    ├── python/
    │   └── agent.py          Naive Python agent
    └── cpp/
        ├── agent.cpp         Naive C++ agent
        └── Makefile          Build the C++ agent
```

## Game mechanics

- The snake starts in the centre of the grid, length 3, heading **right**.
- Each tick the snake advances one cell in its current direction.
- Eating food (`*`) grows the snake by 1 and increments the score.
- The game ends when the snake hits a **wall** or its own **body**.
- Chasing the tail is legal (the tail retracts before collision is checked).
- If the snake fills the entire grid the game ends (you win!).

## License

MIT