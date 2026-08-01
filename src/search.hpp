#pragma once
#include "board.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <vector>

constexpr int INF       = 1'000'000;
constexpr int MATE_VALUE= 900'000;   // returned for checkmate/no-moves
constexpr int MAX_DEPTH = 64;
// 4000cp is far above normal positional swings in this engine's scale (piece values ~100-1300),
// so only clearly one-sided nodes are treated as +/-INF for early pruning stability.
constexpr int PRUNE_LOSS_THRESHOLD_CP = 4000; // A: max側で eval <= -A なら -INF 扱い
constexpr int PRUNE_WIN_THRESHOLD_CP  = 4000; // B: min側で eval >= +B なら +INF 扱い

// Global stop flag (set by USI "stop" command or when time expires)
extern std::atomic<bool> g_stop;

struct SearchInfo {
    int depth = 0;
    int seldepth = 0;
    int time_ms = 0;
    uint64_t nodes = 0;
    uint64_t nps = 0;
    int score_cp = 0;
    bool score_is_mate = false;
    int score_mate = 0;
    std::vector<Move> pv;
};

struct SearchStats {
    uint64_t nodes = 0;
    uint64_t beta_cutoffs = 0;
    uint64_t threshold_cutoffs = 0;
};

// Start the clock for a search
void search_init_time();

// Has the allocated time expired?
bool time_up(int allotted_ms);

// Compute per-move time budget from remaining clock and byoyomi.
// my_time_ms: remaining time for the side to move (milliseconds, >0).
// byoyomi_ms: byoyomi period (milliseconds, 0 if none).
// Returns milliseconds to allocate for this move.
int compute_allotted_ms(int my_time_ms, int byoyomi_ms);

// Iterative-deepening entry point.
// Returns the best move found within allotted_ms milliseconds.
Move iterative_deepening(Board& board, int allotted_ms, const std::function<void(const SearchInfo&)>& info_cb = {});

// Single-depth negamax alpha-beta (exposed for testing)
int negamax(Board& board, int depth, int alpha, int beta, int ply);

SearchStats last_search_stats();
