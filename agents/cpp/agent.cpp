/*
 * Minimal snake C++ agent template (stdin/stdout only).
 */

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <set>
#include <string>
#include <vector>

struct Point { int x, y; };

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
    std::cout << action << "\n";
    std::cout.flush();
}
// ======================= END DO NOT TOUCH: protocol glue =======================

static const char *DIR_ORDER[] = {"up", "right", "down", "left"};
static const int DIR_DX[] = {0, 1, 0, -1};
static const int DIR_DY[] = {-1, 0, 1, 0};

static int dir_index(const std::string &d) {
    for (int i = 0; i < 4; i++) if (d == DIR_ORDER[i]) return i;
    return 1;
}

static std::string apply_turn(const std::string &dir, const std::string &turn) {
    int i = dir_index(dir);
    if (turn == "left") return DIR_ORDER[(i + 3) % 4];
    if (turn == "right") return DIR_ORDER[(i + 1) % 4];
    return dir;
}

static Point next_head(Point h, const std::string &dir) {
    int i = dir_index(dir);
    return {h.x + DIR_DX[i], h.y + DIR_DY[i]};
}

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
        emit_action(choose_action(st));
    }
    return 0;
}
