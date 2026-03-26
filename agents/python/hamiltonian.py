#!/usr/bin/env python3
import json
import sys

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

DIR_ORDER = ["up", "right", "down", "left"]
DIR_DELTA = {"up": (0, -1), "right": (1, 0), "down": (0, 1), "left": (-1, 0)}

class TankAgent:
    def __init__(self):
        self.cycle_map = {} # (x, y) -> index
        self.initialized = False
        self.w = 0
        self.h = 0

    def generate_cycle(self, w, h):
        """Creates a robust Hamiltonian cycle (S-curve)."""
        path = []
        # 1. Down first column
        for y in range(h):
            path.append((0, y))
        # 2. Zig-zag through remaining columns
        for x in range(1, w):
            if x % 2 == 1: # Odd columns: go UP
                for y in range(h - 1, 0, -1):
                    path.append((x, y))
            else: # Even columns: go DOWN
                for y in range(1, h):
                    path.append((x, y))
        # 3. Return across the top row to (0,0)
        for x in range(w - 1, 0, -1):
            path.append((x, 0))
            
        for i, coord in enumerate(path):
            self.cycle_map[coord] = i
        self.w, self.h = w, h
        self.initialized = True

    def get_dist(self, start_idx, end_idx):
        return (end_idx - start_idx + (self.w * self.h)) % (self.w * self.h)

    def choose_action(self, state):
        if not self.initialized:
            self.generate_cycle(state["width"], state["height"])

        snake = [tuple(p) for p in state["snake"]]
        head = snake[0]
        tail = snake[-1]
        apples = [tuple(a) for a in state["apples"]]
        curr_dir = state["dir"]
        body_set = set(snake[:-1]) # Tail is safe to move into
        
        # 1. Evaluate all 3 possible moves for immediate safety
        valid_moves = []
        for turn in ["straight", "left", "right"]:
            nd = self.apply_turn(curr_dir, turn)
            nx, ny = self.next_head(head, nd)
            
            # Boundary Check
            if not (0 <= nx < self.w and 0 <= ny < self.h):
                continue
            
            # Self-Collision Check (Tail is safe unless we eat)
            is_apple = (nx, ny) in apples
            if is_apple:
                if (nx, ny) in set(snake): continue
            else:
                if (nx, ny) in body_set: continue
            
            valid_moves.append({
                "turn": turn,
                "pos": (nx, ny),
                "idx": self.cycle_map.get((nx, ny), 0)
            })

        if not valid_moves:
            return "straight" # Guaranteed death, but prevents logic crash

        # 2. Hamiltonian Logic
        head_idx = self.cycle_map.get(head, 0)
        tail_idx = self.cycle_map.get(tail, 0)
        
        # Priority 1: If we can see an apple, check if a shortcut is safe
        if apples:
            # Find the apple closest to us in cycle distance
            target_apple = min(apples, key=lambda a: self.get_dist(head_idx, self.cycle_map.get(a, 0)))
            apple_idx = self.cycle_map.get(target_apple, 0)
            
            best_shortcut = None
            min_apple_dist = float('inf')
            
            for move in valid_moves:
                # Shortcut Safety Rule:
                # Shortcut index (n_idx) must be between head and tail in the cycle
                # AND it must not "skip over" the tail.
                dist_to_tail = self.get_dist(move["idx"], tail_idx)
                
                # We require a buffer of empty space so we don't trap ourselves
                if dist_to_tail > 2:
                    dist_to_apple = self.get_dist(move["idx"], apple_idx)
                    if dist_to_apple < min_apple_dist:
                        min_apple_dist = dist_to_apple
                        best_shortcut = move["turn"]
            
            if best_shortcut:
                return best_shortcut

        # Priority 2: Follow the cycle exactly if no shortcut is safe/available
        target_idx = (head_idx + 1) % (self.w * self.h)
        for move in valid_moves:
            if move["idx"] == target_idx:
                return move["turn"]

        # Priority 3: If cycle is blocked (rare), pick move with most reachable space
        return valid_moves[0]["turn"]

    def apply_turn(self, direction, turn):
        idx = DIR_ORDER.index(direction)
        if turn == "left": return DIR_ORDER[(idx + 3) % 4]
        if turn == "right": return DIR_ORDER[(idx + 1) % 4]
        return direction

    def next_head(self, head, direction):
        dx, dy = DIR_DELTA[direction]
        return (head[0] + dx, head[1] + dy)

agent = TankAgent()

def main():
    for state in iter_json_states():
        if not state.get("alive", False): break
        emit_action(agent.choose_action(state))

if __name__ == "__main__":
    main()
