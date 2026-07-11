#pragma once
#include "board.hpp"
#include <atomic>
#include <chrono>

constexpr int INF       = 1'000'000;
constexpr int MATE_VALUE= 900'000;   // returned for checkmate/no-moves
constexpr int MAX_DEPTH = 64;

// Global stop flag (set by USI "stop" command or when time expires)
extern std::atomic<bool> g_stop;

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
Move iterative_deepening(Board& board, int allotted_ms);

// Single-depth negamax alpha-beta (exposed for testing)
int negamax(Board& board, int depth, int alpha, int beta, int ply);
