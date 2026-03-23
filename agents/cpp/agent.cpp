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
// - apples / food: locations of food
struct GameState {
    bool alive = false;
    int width = 0;
    int height = 0;
    std::string dir = "right";
    std::vector<Point> snake;
    std::vector<Point> apples;
    Point food{-1, -1};
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
        } else if (key == "food") {
            if (*p == '[') ++p;
            s.food.x = (int)strtol(p, const_cast<char **>(&p), 10);
            if (*p == ',') ++p;
            s.food.y = (int)strtol(p, const_cast<char **>(&p), 10);
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

// Beginner-friendly strategy implemented in C++:
// - Check the three relative moves (straight/left/right).
// - Reject moves that hit walls or the snake body (allow moving into the
//   tail if the tail will move and the cell is not an apple).
// - Choose uniformly at random among remaining safe moves.
static std::string choose_action(const GameState &st) {
    if (st.snake.empty()) return "straight";
    std::set<std::pair<int, int>> body;
    for (const auto &p : st.snake) body.insert({p.x, p.y});

    std::set<std::pair<int, int>> apple_set;
    if (!st.apples.empty()) {
        for (const auto &a : st.apples) apple_set.insert({a.x, a.y});
    } else {
        apple_set.insert({st.food.x, st.food.y});
    }

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
        // Read state, decide on an action, and emit it to the engine.
        emit_action(choose_action(st));
    }
    return 0;
}
