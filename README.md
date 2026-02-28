# Snake

A terminal-based Snake game written in C with an **interactive TUI menu**,
a **built-in agent runner** with watch mode, batch statistics, and logging,
and a **JSON line-protocol** for writing agents in any language.

## Features

| Feature | Details |
|---|---|
| **TUI menu** | ncurses main menu → play settings, agent config, all driven by keyboard |
| **Play mode** | Arrow keys / WASD, colour support, timed or step-by-step |
| **Agent runner** | Batch-run any agent with live watch mode, progress bar, per-game stats |
| **Summary screen** | Avg / max / min / median / stddev for scores & ticks; save to file |
| **Action logging** | Optional per-tick JSONL log of every decision the agent made |
| **Dynamic agents** | Drop an `agent.json` manifest into `agents/<name>/` and it appears in the menu |
| **Agent protocol** | JSON line-protocol on stdin/stdout — language-agnostic |
| **Custom command** | Run any shell command as an agent directly from the menu |
| **Performance** | Ring-buffer snake body + O(1) grid collision checks |

## Quick start

```bash
# Build the game
make

# Launch the interactive menu (play or run agents)
./snake

# Play directly (timed mode, 200 ms per tick)
./snake play

# Play in step mode (one tick per keypress)
./snake play --step

# Run the Python agent standalone
cd agents/python && python agent.py

# Run the C++ agent standalone
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
```

With **no arguments** the interactive TUI menu is shown.

```
Modes:
  play          Interactive ncurses TUI
  agent         JSON line-protocol on stdin/stdout (for external tools)

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

## Interactive menu

Running `./snake` (no arguments) presents a main menu:

```
┌──────────────────────────────────┐
│           S N A K E              │
├──────────────────────────────────┤
│   > Play Game                    │
│     Agent Mode                   │
│     Quit                         │
└──────────────────────────────────┘
```

**Navigation:** Up/Down or J/K to move, Enter to select, Q to quit.

### Play Game

Opens a settings form where you configure grid size, speed, seed, and tick
mode (timed vs. step).  Press Enter to start the game.

| Key | Action |
|---|---|
| Arrow keys / WASD | Change direction |
| Space / Enter | Advance one tick without turning (step mode) |
| Q | Quit |

### Agent Mode

Opens the **agent configuration form**:

| Setting | Description |
|---|---|
| **Agent** | Cycle through all discovered agents and "Custom Command" |
| **Command** | *(shown only for Custom Command)* Shell command to run |
| **Games** | Number of games to play (1–100,000) |
| **Width / Height** | Grid dimensions |
| **Speed** | Tick interval in ms (for timed mode / watch mode pacing) |
| **Seed** | Random seed (0 = random) |
| **Tick Mode** | Timed or Step |
| **Watch** | Yes = render each frame live; No = show progress bar only |
| **Log Actions** | Yes = write per-tick JSONL action log to a file |

**Form controls:** Up/Down to navigate, Left/Right to change, `<`/`>` or
PgUp/PgDn for big steps, type digits for numbers.  Enter to start.  Esc to
go back.

#### Watch mode controls

While watching an agent play:

| Key | Action |
|---|---|
| Q / Esc | Abort and go to summary |
| N | Skip to the next game |
| Space | Pause / resume |
| + / - | Adjust speed ±20 ms |

#### Summary screen

After all games complete (or on abort), the summary shows:

- Agent name, grid, total duration
- Score statistics (avg, max, min, median, std dev)
- Tick statistics (avg, max, min, median, std dev)
- Log file path (if logging was enabled)
- Scrollable per-game results table

| Key | Action |
|---|---|
| Up / Down | Scroll per-game table |
| S | Save full summary to a `.txt` file |
| R | Run the same batch again |
| C | Go back to reconfigure settings |
| Q / Esc | Return to main menu |

## Adding your own agent

The agent menu is **dynamic**.  Any subdirectory of `agents/` that contains
an `agent.json` manifest file automatically appears as a selectable agent
in the TUI.

### Step-by-step

1. **Create a directory** under `agents/`:

   ```bash
   mkdir agents/my_agent
   ```

2. **Write your agent program.**  It must support a `--pipe` mode that
   reads JSON game state lines from stdin and writes turn commands
   (`straight`, `left`, or `right`) to stdout — one per line:

   ```
   stdin  →  {"tick":0,"alive":true,"score":0,"width":20,"height":20, ...}
   stdout ←  left
   stdin  →  {"tick":1,"alive":true,"score":0,"width":20,"height":20, ...}
   stdout ←  straight
   ...
   ```

3. **Create `agents/my_agent/agent.json`:**

   ```json
   {
       "name": "My Smart Agent",
       "command": "python3 my_agent.py --pipe",
       "description": "Uses A* pathfinding to chase food"
   }
   ```

   | Field | Required | Description |
   |---|---|---|
   | `name` | Yes | Display name shown in the TUI menu (max 63 chars) |
   | `command` | Yes | Shell command to run **from the agent's directory** |
   | `description` | No | Short description (informational, max 127 chars) |

4. **Restart the snake program.**  Your agent will appear in the Agent
   dropdown immediately.

### Example: minimal Python agent

```
agents/my_agent/
├── agent.json
└── my_agent.py
```

`agent.json`:
```json
{
    "name": "My Agent",
    "command": "python3 my_agent.py --pipe",
    "description": "Always goes straight"
}
```

`my_agent.py`:
```python
#!/usr/bin/env python3
import json, sys

for line in sys.stdin:
    state = json.loads(line.strip())
    if not state["alive"]:
        break
    print("straight", flush=True)
```

### Notes

- The `command` is executed **from the agent's own directory** as the
  working directory, so relative paths in the command refer to files inside
  `agents/my_agent/`.
- The command is run via `/bin/sh -c`, so shell features (pipes, `&&`,
  environment variables) are available.
- The program must use **line-buffered** I/O.  Flush stdout after every
  line.
- Up to 32 agents can be registered (including built-in and custom).
- The "Custom Command" option is always available as the last entry in
  the agent list for one-off testing with an arbitrary command.

### Pipe protocol reference

Each tick the game writes one JSON line to the agent's stdin:

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

The agent must respond with exactly one line:

```
straight | left | right
```

Turns are **relative** to the snake's current heading, not absolute.

When `"alive"` is `false`, the game is over — the agent should exit or
stop reading.

## Agent API (subprocess mode)

The included Python and C++ agents can also be run **standalone** — they
launch `./snake agent --step` as a subprocess and manage the protocol
themselves.  This is useful for running agents outside the TUI.

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

## Architecture

```
snake/
├── Makefile                  Build the game binary
├── src/
│   ├── game.h                Structures & API declarations
│   ├── game.c                Core logic (ring-buffer, grid, tick)
│   ├── agent.h               Built-in agent interface
│   ├── agent.c               Built-in naive agent (C)
│   └── main.c                CLI, ncurses TUI, menu, agent runner
└── agents/
    ├── python/
    │   ├── agent.json         Agent manifest (auto-discovered)
    │   └── agent.py           Naive Python agent
    └── cpp/
        ├── agent.json         Agent manifest (auto-discovered)
        ├── agent.cpp          Naive C++ agent
        └── Makefile           Build the C++ agent
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