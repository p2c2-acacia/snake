# Snake Difficulty + Agent UX Design

## Goal

Add a seven-stage difficulty system for Snake agent gameplay, keep collision rules consistent (wall/body collision = fail), support static agent stepping, provide two scoring families (pass/fail for stages 1-5, time ranking for stages 6-7), add an advanced raw-board output mode, simplify starter agents, centralize defaults in `settings.json`, and keep agent discovery dynamic while the app is running.

## Scope

In scope:

- Difficulty stages 1-7 with requested spawn/regen behavior.
- Pass/fail and time-rank scoring modes.
- Static agent progression (wait for agent action each tick).
- JSON and raw-board output modes for agent protocol.
- Beginner-friendly Python/C++ template agents with `DO NOT TOUCH` boundaries.
- `settings.json` as source of defaults.
- Dynamic `agents/` refresh in menu flow.
- Beginner + advanced README refresh.

Out of scope:

- New networked protocols or remote agent APIs.
- Additional game mechanics beyond requested difficulty/score/output behavior.

## Architecture

### 1) Game engine rules (`src/game.h`, `src/game.c`)

Introduce stage-aware rules and multi-apple support in the game state:

- `GameRules` for stage and tunables.
- `ScoreMode` for pass/fail vs time-rank.
- `GameOutcome` for running/pass/fail.
- `apples[]` + `apple_count` for concurrent apples.
- stage-specific setup and regen logic in `spawn_stage_initial()` and `on_apple_eaten()`.

Collision behavior remains unchanged semantically:

- wall hit → fail
- body hit → fail
- tail-chase remains legal when tail retracts

### 2) Runtime and protocol (`src/main.c`)

Add protocol output abstraction:

- JSON writer (`write_state_json`)
- raw board text writer (`write_state_raw_board`)
- dispatcher (`write_state`)

Agent mode remains static by enforcing step-style blocking input.

### 3) Settings-first configuration (`settings.json`)

Use `settings.json` for defaults and stage tunables. Defaults are loaded at startup and file is generated if missing.

The policy is:

- no CLI config option inputs (width/height/speed/stage/output/etc.)
- menu + settings file are the configuration path
- keyboard play/menu controls remain enabled

### 4) Dynamic agent discovery

Track agent directory stamp and refresh registry in menu flow:

- before main menu render loop
- inside agent config flow

This supports adding/removing `agents/*` without restarting the app.

### 5) Starter agents

Replace standalone-launch agent behavior with pure stdin/stdout templates:

- Python template (`agents/python/agent.py`)
- C++ template (`agents/cpp/agent.cpp`)

Both include explicit `DO NOT TOUCH` protocol boundary blocks.

## Difficulty definitions

1. Corner snake spawn + center apple.
2. Center snake (right) + fixed-count apples at fixed locations.
3. Center snake (random heading) + one random apple.
4. Center snake (random heading) + fixed-count random apples.
5. Center snake (random heading) + one regenerating random apple.
6. Center snake (random heading) + fixed-count regenerating random apples.
7. Center snake (random heading) + regenerating apples with increasing active count.

Scoring:

- stages 1-5 → pass/fail
- stages 6-7 → time ranking

### Default completion criteria (normative)

- Stage 1: **pass** when the only apple is eaten.
- Stage 2: **pass** when all initial fixed apples are eaten.
- Stage 3: **pass** when the only random apple is eaten.
- Stage 4: **pass** when all initial random apples are eaten.
- Stage 5: **pass** when `apples_eaten >= stage5_goal_apples`.
- Stage 6: **pass** when `apples_eaten >= stage6_goal_apples`.
- Stage 7: **pass** when `apples_eaten >= stage7_goal_apples`.
- Any stage: wall/body collision => immediate **fail**.

### Time ranking definition (normative for stages 6-7)

- Primary ranking metric: `ticks` to terminal success/fail state.
- Lower `ticks` ranks better.
- If two runs have equal ticks, higher `apples_eaten` ranks better.
- If still tied, ordering is stable by run index (earlier run first).
- Summary should expose at least ticks min/avg/max and pass count.

### Canonical default tunables

These defaults are authoritative and sourced from `settings.json`:

- `stage2_fixed_apples = 4`
- `stage4_fixed_apples = 4`
- `stage5_goal_apples = 10`
- `stage6_fixed_apples = 3`
- `stage6_goal_apples = 20`
- `stage7_initial_apples = 2`
- `stage7_increment_every = 5`
- `stage7_increment_by = 1`
- `stage7_max_apples = 8`
- `stage7_goal_apples = 30`

## Data flow

1. Startup reads settings and initializes agent registry.
2. Selected mode (menu/play/agent protocol) constructs rules for chosen stage.
3. Engine emits state per tick in configured output mode.
4. Agent responds with `straight|left|right`.
5. Tick advances only after agent response in static agent mode.
6. Outcome/scoring collected and shown in summaries.

## Error handling

- Invalid/missing settings fields fall back to safe defaults.
- Unknown agent command failures are surfaced in TUI run screen.
- Agent output timeout path defaults to straight (timed path), while static mode blocks for response by design.
- Protocol parsers in template agents ignore malformed lines safely.

## Testing strategy

1. Build verification:

- `make clean && make`
- `cd agents/cpp && make clean && make`

2. Protocol smoke checks:

- JSON output sample in `./snake agent`
- raw board output sample in `./snake agent` with raw-board mode

3. Template agent smoke checks:

- pipe one test JSON state through Python and C++ templates
- confirm valid action output

4. Stage sanity checks:

- verify stage-dependent initial apples and score mode fields in JSON output

## Documentation updates

README should cover:

- beginner quick start
- difficulty behavior and scoring
- settings-first policy
- advanced raw-board mode
- template agent extension path

## Acceptance criteria

- all seven stages initialize and progress according to definitions
- collision fail behavior is consistent across stages
- stage 1-5 pass/fail and stage 6-7 time ranking are represented in outputs/summaries
- raw-board mode is available as separate output option
- starter agents are simple, educational templates with clear edit boundaries
- settings file controls defaults; CLI config options are not required
- adding/removing agent directories is detected without restart in menu flow
- repository builds cleanly and smoke tests pass
