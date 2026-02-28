/*
 * agent.cpp – Naive snake agent in C++.
 *
 * Launches the snake binary as a child process (pipe-based),
 * reads JSON game state from its stdout, and writes turn commands
 * to its stdin.  Avoids immediate death; picks randomly otherwise.
 *
 * Build:  make          (or: g++ -O2 -std=c++17 -o agent agent.cpp)
 * Run:    ./agent [--games N] [--visual] [--game-path P] [snake flags...]
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

#include <unistd.h>
#include <sys/wait.h>

// ── Data types ──────────────────────────────────────────────────

struct Point { int x, y; };

struct GameState {
    int  tick   = 0;
    bool alive  = false;
    int  score  = 0;
    int  width  = 0;
    int  height = 0;
    std::string dir;
    std::vector<Point> snake;
    Point food{0, 0};
};

// ── Minimal targeted JSON parser ────────────────────────────────
// Handles exactly the format emitted by the snake binary.

static GameState parse_state(const std::string &line) {
    GameState s;
    const char *p = line.c_str();

    // Advance past '{'
    while (*p && *p != '{') ++p;
    if (!*p) return s;
    ++p;

    while (*p) {
        // skip whitespace / commas
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r') ++p;
        if (*p == '}' || !*p) break;

        // read key
        if (*p != '"') break;
        ++p;
        const char *ks = p;
        while (*p && *p != '"') ++p;
        std::string key(ks, p);
        if (*p == '"') ++p;

        // skip ':'
        while (*p == ' ' || *p == ':') ++p;

        if (key == "tick") {
            s.tick = (int)strtol(p, const_cast<char **>(&p), 10);
        } else if (key == "alive") {
            s.alive = (*p == 't');
            while (*p && *p != ',' && *p != '}') ++p;
        } else if (key == "score") {
            s.score = (int)strtol(p, const_cast<char **>(&p), 10);
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
        } else if (key == "snake") {
            // expect [[x,y],[x,y],...]
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
                    s.snake.push_back(pt);
                } else {
                    break;
                }
            }
            if (*p == ']') ++p;   // closing outer ]
        } else if (key == "food") {
            if (*p == '[') ++p;
            s.food.x = (int)strtol(p, const_cast<char **>(&p), 10);
            if (*p == ',') ++p;
            s.food.y = (int)strtol(p, const_cast<char **>(&p), 10);
            if (*p == ']') ++p;
        } else {
            // skip unknown value
            while (*p && *p != ',' && *p != '}') ++p;
        }
    }
    return s;
}

// ── Direction helpers ───────────────────────────────────────────

static const char *DIR_ORDER[] = {"up", "right", "down", "left"};
static const int   DIR_DX[]    = { 0,    1,       0,     -1};
static const int   DIR_DY[]    = {-1,    0,       1,      0};

static int dir_index(const std::string &d) {
    for (int i = 0; i < 4; i++)
        if (d == DIR_ORDER[i]) return i;
    return 1; // default right
}

static std::string apply_turn(const std::string &dir, const std::string &turn) {
    int idx = dir_index(dir);
    if (turn == "left")  return DIR_ORDER[(idx + 3) % 4];
    if (turn == "right") return DIR_ORDER[(idx + 1) % 4];
    return dir;
}

static Point next_head(const Point &head, const std::string &dir) {
    int i = dir_index(dir);
    return {head.x + DIR_DX[i], head.y + DIR_DY[i]};
}

// ── Safety check ────────────────────────────────────────────────

static bool is_safe(const GameState &st, const std::string &turn) {
    std::string new_dir = apply_turn(st.dir, turn);
    Point nh = next_head(st.snake[0], new_dir);

    if (nh.x < 0 || nh.x >= st.width || nh.y < 0 || nh.y >= st.height)
        return false;

    // Build occupancy set
    std::set<std::pair<int,int>> body;
    for (auto &p : st.snake) body.insert({p.x, p.y});

    // If not eating food, the tail will retract
    if (!(nh.x == st.food.x && nh.y == st.food.y) && !st.snake.empty()) {
        auto &tail = st.snake.back();
        body.erase({tail.x, tail.y});
    }

    return body.find({nh.x, nh.y}) == body.end();
}

static std::string choose_action(const GameState &st) {
    static const std::string actions[] = {"straight", "left", "right"};
    std::vector<std::string> safe;
    for (auto &a : actions)
        if (is_safe(st, a))
            safe.push_back(a);
    if (safe.empty()) return "straight";
    return safe[rand() % safe.size()];
}

// ── Visualisation (optional, writes to stderr) ──────────────────

static void render(const GameState &st) {
    std::vector<std::string> grid(st.height, std::string(st.width, ' '));

    for (size_t i = 0; i < st.snake.size(); i++) {
        int x = st.snake[i].x, y = st.snake[i].y;
        if (x >= 0 && x < st.width && y >= 0 && y < st.height)
            grid[y][x] = (i == 0) ? '@' : 'o';
    }
    if (st.food.x >= 0 && st.food.x < st.width &&
        st.food.y >= 0 && st.food.y < st.height)
        grid[st.food.y][st.food.x] = '*';

    fprintf(stderr, "\033[H\033[2J");
    fprintf(stderr, "+%s+\n", std::string(st.width, '-').c_str());
    for (auto &row : grid)
        fprintf(stderr, "|%s|\n", row.c_str());
    fprintf(stderr, "+%s+\n", std::string(st.width, '-').c_str());
    fprintf(stderr, "Score: %d  Tick: %d  Dir: %s  Alive: %s\n",
            st.score, st.tick, st.dir.c_str(), st.alive ? "yes" : "no");
}

// ── Subprocess management ───────────────────────────────────────

struct GameResult { int score; int ticks; };

static GameResult play_one_game(const std::string &game_path,
                                const std::vector<std::string> &extra_args,
                                bool visual) {
    int pipe_in[2], pipe_out[2];  // in = parent→child, out = child→parent
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        perror("pipe");
        return {0, 0};
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return {0, 0}; }

    if (pid == 0) {
        // ── Child: become the snake game ────────────────────
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0],  STDIN_FILENO);
        dup2(pipe_out[1], STDOUT_FILENO);
        close(pipe_in[0]);
        close(pipe_out[1]);

        // Redirect stderr to /dev/null
        FILE *devnull = fopen("/dev/null", "w");
        if (devnull) { dup2(fileno(devnull), STDERR_FILENO); fclose(devnull); }

        std::vector<const char *> args;
        args.push_back(game_path.c_str());
        args.push_back("agent");
        args.push_back("--step");
        for (auto &a : extra_args) args.push_back(a.c_str());
        args.push_back(nullptr);

        execvp(args[0], const_cast<char *const *>(args.data()));
        perror("execvp");
        _exit(1);
    }

    // ── Parent: the agent ──────────────────────────────────
    close(pipe_in[0]);
    close(pipe_out[1]);

    FILE *to_game   = fdopen(pipe_in[1],  "w");
    FILE *from_game = fdopen(pipe_out[0], "r");
    setvbuf(to_game,   nullptr, _IOLBF, 0);
    setvbuf(from_game, nullptr, _IOLBF, 0);

    GameResult result{0, 0};
    char buf[65536];

    while (fgets(buf, sizeof buf, from_game)) {
        GameState st = parse_state(buf);

        if (visual) render(st);

        if (!st.alive) {
            result.score = st.score;
            result.ticks = st.tick;
            break;
        }

        std::string action = choose_action(st);
        fprintf(to_game, "%s\n", action.c_str());
        fflush(to_game);
    }

    fclose(to_game);
    fclose(from_game);
    waitpid(pid, nullptr, 0);
    return result;
}

// ── Pipe mode (stdin/stdout filter for TUI integration) ────────

static void run_pipe() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        GameState st = parse_state(line);
        if (!st.alive) break;
        std::string action = choose_action(st);
        std::cout << action << "\n";
        std::cout.flush();
    }
}

// ── Entry point ─────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    srand((unsigned)time(nullptr));

    // --pipe: act as a stdin/stdout filter (for TUI integration)
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--pipe") {
            run_pipe();
            return 0;
        }
    }

    // Default game binary: ../../snake relative to this executable
    // (works when run from agents/cpp/)
    std::string game_path = "../../snake";
    int  num_games = 1;
    bool visual    = false;
    std::vector<std::string> extra_args;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--games" && i + 1 < argc) {
            num_games = std::atoi(argv[++i]);
        } else if (arg == "--visual") {
            visual = true;
        } else if (arg == "--game-path" && i + 1 < argc) {
            game_path = argv[++i];
        } else {
            extra_args.push_back(arg);
        }
    }

    // Check binary exists
    if (access(game_path.c_str(), X_OK) != 0) {
        fprintf(stderr, "Error: game binary not found or not executable: %s\n",
                game_path.c_str());
        fprintf(stderr, "Build it first:  cd snake && make\n");
        return 1;
    }

    std::vector<int> scores, ticks_list;

    for (int g = 0; g < num_games; g++) {
        if (num_games > 1 && !visual)
            printf("Game %d/%d ... ", g + 1, num_games);

        GameResult r = play_one_game(game_path, extra_args, visual);
        scores.push_back(r.score);
        ticks_list.push_back(r.ticks);

        if (num_games > 1 && !visual)
            printf("score=%d  ticks=%d\n", r.score, r.ticks);
    }

    // Summary
    if (num_games == 1) {
        printf("Score: %d  Ticks: %d\n", scores[0], ticks_list[0]);
    } else {
        double avg_s = std::accumulate(scores.begin(), scores.end(), 0.0) / scores.size();
        double avg_t = std::accumulate(ticks_list.begin(), ticks_list.end(), 0.0) / ticks_list.size();
        printf("\n========================================\n");
        printf("Games played : %d\n", num_games);
        printf("Avg score    : %.1f\n", avg_s);
        printf("Max score    : %d\n", *std::max_element(scores.begin(), scores.end()));
        printf("Min score    : %d\n", *std::min_element(scores.begin(), scores.end()));
        printf("Avg ticks    : %.1f\n", avg_t);
        printf("========================================\n");
    }

    return 0;
}
