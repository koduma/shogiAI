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

// Iterative-deepening entry point.
// Returns the best move found within allotted_ms milliseconds.
Move iterative_deepening(Board& board, int allotted_ms);

// Single-depth negamax alpha-beta (exposed for testing)
int negamax(Board& board, int depth, int alpha, int beta, int ply);
