#!/usr/bin/env python3
import json
import sys
import collections
import random

DIR_ORDER = ["up", "right", "down", "left"]
DIR_DELTA = {"up": (0, -1), "right": (1, 0), "down": (0, 1), "left": (-1, 0)}

# ========================= DO NOT TOUCH: protocol glue =========================
def iter_json_states():
    for line in sys.stdin:
        line = line.strip()
        if not line: continue
        try: yield json.loads(line)
        except json.JSONDecodeError: continue

def emit_action(action: str) -> None:
    sys.stdout.write(action + "\n")
    sys.stdout.flush()
# ======================= END DO NOT TOUCH: protocol glue =======================

def get_neighbors(pos, w, h):
    for dx, dy in DIR_DELTA.values():
        nx, ny = pos[0] + dx, pos[1] + dy
        if 0 <= nx < w and 0 <= ny < h:
            yield (nx, ny)

def find_shortest_path(start, goal, obstacles, w, h):
    if start == goal: return [start]
    q = collections.deque([[start]])
    visited = {start}
    while q:
        path = q.popleft()
        curr = path[-1]
        if curr == goal:
            return path
        for neighbor in get_neighbors(curr, w, h):
            if neighbor not in obstacles and neighbor not in visited:
                visited.add(neighbor)
                q.append(path + [neighbor])
    return None

def get_flood_fill(start, obstacles, w, h):
    """Returns the number of reachable cells from start."""
    if start in obstacles: return 0
    q = collections.deque([start])
    visited = {start}
    while q:
        curr = q.popleft()
        for n in get_neighbors(curr, w, h):
            if n not in obstacles and n not in visited:
                visited.add(n)
                q.append(n)
    return len(visited)

def choose_action(state: dict) -> str:
    w, h = state["width"], state["height"]
    snake = [tuple(p) for p in state["snake"]]
    head = snake[0]
    tail = snake[-1]
    apples = sorted([tuple(a) for a in state["apples"]], 
                   key=lambda a: abs(a[0]-head[0]) + abs(a[1]-head[1]))
    curr_dir = state["dir"]
    body_no_tail = set(snake[:-1])

    # 1. ATTEMPT SHORTEST PATH TO APPLE
    for apple in apples:
        path = find_shortest_path(head, apple, body_no_tail, w, h)
        if path and len(path) > 1:
            # Check safety: Can we reach tail after eating this apple?
            # Simulate snake body after eating
            virtual_snake = path[::-1] + snake[:-(len(path)-1)]
            v_head, v_tail = virtual_snake[0], virtual_snake[-1]
            v_body = set(virtual_snake[:-1])
            if find_shortest_path(v_head, v_tail, v_body, w, h):
                return coords_to_turn(head, path[1], curr_dir)

    # 2. STALLING / SURVIVAL MODE (Tail Chasing + Space Maximizing)
    # We want a move that:
    #   A) Is not a collision
    #   B) Maintains a path to the tail
    #   C) Maximizes the available floor space (Flood Fill)
    
    possible_moves = []
    for turn in ["straight", "left", "right"]:
        new_dir = apply_turn(curr_dir, turn)
        move_pos = next_head(list(head), new_dir)
        
        if 0 <= move_pos[0] < w and 0 <= move_pos[1] < h and move_pos not in body_no_tail:
            # Check if tail is reachable from this new position
            if find_shortest_path(move_pos, tail, body_no_tail, w, h):
                space = get_flood_fill(move_pos, body_no_tail, w, h)
                possible_moves.append((space, turn))

    if possible_moves:
        # Sort by space (descending)
        possible_moves.sort(key=lambda x: x[0], reverse=True)
        max_space = possible_moves[0][0]
        
        # Filter moves that provide roughly the same maximum space
        # (Within 5% of the max space to allow for variation)
        top_moves = [m[1] for m in possible_moves if m[0] >= max_space * 0.95]
        
        # RANDOM TIE-BREAKER: This prevents the infinite figure-8 orbit!
        return random.choice(top_moves)

    # 3. PANIC MODE: Just don't hit a wall
    for turn in ["straight", "left", "right"]:
        new_dir = apply_turn(curr_dir, turn)
        move_pos = next_head(list(head), new_dir)
        if 0 <= move_pos[0] < w and 0 <= move_pos[1] < h and move_pos not in body_no_tail:
            return turn

    return "straight" # Goodbye, world.

def coords_to_turn(cur_head, next_step, cur_dir):
    for turn in ["straight", "left", "right"]:
        if next_head(list(cur_head), apply_turn(cur_dir, turn)) == next_step:
            return turn
    return "straight"

def apply_turn(direction: str, turn: str) -> str:
    idx = DIR_ORDER.index(direction)
    if turn == "left": return DIR_ORDER[(idx + 3) % 4]
    if turn == "right": return DIR_ORDER[(idx + 1) % 4]
    return direction

def next_head(head: List[int], direction: str) -> Tuple[int, int]:
    dx, dy = DIR_DELTA[direction]
    return (head[0] + dx, head[1] + dy)

def main() -> None:
    for state in iter_json_states():
        if not state.get("alive", False): break
        emit_action(choose_action(state))

if __name__ == "__main__":
    main()
