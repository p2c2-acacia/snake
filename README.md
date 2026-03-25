# Snake

Terminal Snake in C with:

- Interactive menu (`./snake.out`)
- Agent protocol mode (`./snake.out agent`)
- Dynamic `agents/` discovery
- Difficulty stages 1-7
- `settings.json` defaults

## Prerequisites

### Windows

Install WSL2, then follow the Linux steps above:

```bash
wsl --install
```

Open the Ubuntu terminal from the Start menu and continue with the Linux instructions.

### Linux (Ubuntu/Debian)

```bash
sudo apt update && sudo apt install build-essential libncurses5-dev
```

### macOS

```bash
xcode-select --install
brew install ncurses
```

## Build

Retrieve the repository:
```bash
git clone https://github.com/p2c2-acacia/snake.git
cd snake
```

Build the project:
```bash
make
```

Verify the build succeeded:

```bash
ls -l snake.out
```

### Build the C++ template agent (optional)

```bash
cd agents/cpp && make
```

This produces `agents/cpp/agent.out`. The Python agent (`agents/python/agent.py`)
requires no build step.

## Quick start

- `./snake.out` — menu (Play / Agent Mode)
- `./snake.out play` — direct play mode
- `./snake.out agent` — protocol mode (stdin state, stdout action)

## Difficulties

1. Snake starts in a corner; one apple in the center.
2. Snake starts center (right); fixed apples at fixed board positions.
3. Snake starts center (random direction); one apple at random location.
4. Snake starts center (random direction); fixed number of apples at random locations.
5. Snake starts center (random direction); one apple regenerates randomly.
6. Snake starts center (random direction); fixed number of apples regenerate randomly.
7. Snake starts center (random direction); regenerating apples with increasing active count.

* Snake increases in length by 1 after eating an apple.

Scoring (Goal):

- Stages 1-5: pass/fail
- Stages 6-7: time ranking (ticks)

## Agent protocol

Your agent reads one state and must write one action:

- Actions: `straight`, `left`, `right`
- Agent mode is static by default (waits for agent output before next tick)

### JSON output (set `agent_output_mode: "json"` in settings.json)

Example state:

```json
{
  "tick": 4,
  "alive": true,
  "score": 2,
  "apples_eaten": 2,
  "goal_apples": 10,
  "difficulty": 5,
  "outcome": "running",
  "score_mode": "pass_fail",
  "width": 20,
  "height": 20,
  "dir": "right",
  "snake": [[10,10],[9,10],[8,10]],
  "apples": [[13,7]]
}
```

### Advanced mode (set `agent_output_mode: "raw_board"`)

State is sent as text with a bordered board image:

```text
STATE_BEGIN
tick:12
alive:true
score:3
...
board:
+--------------------+
|....@oo....*.......|
|....................|
...
+--------------------+
STATE_END
```

You parse that text and convert it into your own internal representation.

## settings.json

`settings.json` is loaded at startup and created automatically if missing or corrupt.
Users can edit it while keeping the game running. For agent add/remove edits,
the menu flow refreshes `agents/` dynamically.

Key fields:

```json
{
  "width": 20,
  "height": 20,
  "speed_ms": 200,
  "play_difficulty": 1,
  "agent_difficulty": 1,
  "agent_games": 1,
  "agent_watch": true,
  "agent_output_mode": "json",
  "default_agent": "Built-in Naive"
}
```

Stage tuning fields are also available:

- `stage2_fixed_apples`
- `stage4_fixed_apples`
- `stage5_goal_apples`
- `stage6_fixed_apples`
- `stage6_goal_apples`
- `stage7_initial_apples`
- `stage7_increment_every`
- `stage7_increment_by`
- `stage7_max_apples`
- `stage7_goal_apples`

### In-game settings menu

You may change the settings for the **current session** using the in-game settings menu.

## Adding an agent (beginner path)

1. Create `agents/my_agent/agent.json`
2. Create code that reads stdin state lines and prints one action line
3. Use `--pipe` in `command`

Example manifest:

```json
{
  "name": "My Agent",
  "command": "python3 agent.py --pipe",
  "description": "My first snake agent"
}
```

The bundled Python/C++ agents are simplified templates and contain
`DO NOT TOUCH` labels for protocol glue boundaries.

## Advanced path

- Use `agent_output_mode: "raw_board"` for image-like board parsing.
- Use stage 6/7 for time ranking benchmarks.
- Enable action logs in Agent Mode to analyze decisions per tick.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `ncurses.h: No such file` | Install ncurses headers: `sudo apt install libncurses5-dev` (Linux) or `brew install ncurses` (macOS) |
| `make: gcc: not found` | Install build tools: `sudo apt install build-essential` (Linux) or `xcode-select --install` (macOS) |
| C++ agent won't start | Run `make` inside `agents/cpp/` first |
| settings.json corrupted | Delete it and relaunch `./snake.out` — a fresh default is created automatically |
| Terminal too small | Resize your terminal to at least 24 rows by 80 columns |

## Repository layout

```text
src/            game engine + TUI + agent runner
agents/python/  Python template agent
agents/cpp/     C++ template agent
settings.json   default game/agent configuration
```

## License

MIT
