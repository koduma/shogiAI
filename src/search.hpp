#pragma once
#include "board.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

constexpr int INF                = 1'000'000;
constexpr int MATE_VALUE         = 900'000;   // returned for checkmate/no-moves
constexpr int MAX_DEPTH          = 64;
constexpr int MAX_SEARCH_THREADS = 256;

// Global stop flag (set by USI "stop" command, hard time expiry, or nodelimit expiry)
extern std::atomic<bool> g_stop;

// Configurable node cap (`nodelimit` USI engine option):
//   0   -> normal search (no node cap)
//   x>0 -> search at most `x` nodes across all threads and play the best move
extern std::atomic<uint64_t> nodelimit;

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
    uint64_t qnodes = 0;
    uint64_t beta_cutoffs = 0;
    uint64_t threshold_cutoffs = 0;
    uint64_t tt_probes = 0;
    uint64_t tt_hits = 0;
    uint64_t tt_cutoffs = 0;
    uint64_t null_move_verifications = 0;
    uint64_t late_move_prunes = 0;
    uint64_t aspiration_fail_low = 0;
    uint64_t aspiration_fail_high = 0;
    uint64_t countermove_hits = 0;
    uint64_t repetition_sensitive_tt_skips = 0;
    uint64_t checking_move_selectivity_skips = 0;
    uint64_t continuation_history_updates = 0;
    uint64_t continuation_history_ordering_uses = 0;
    uint64_t singular_tt_candidates = 0;
    uint64_t singular_extension_attempts = 0;
    uint64_t singular_extensions = 0;
    uint64_t exclusion_searches = 0;
};

struct TimeBudget {
    int soft_ms = 1000;
    int hard_ms = 1000;
};

// Configure/query number of Lazy SMP worker threads (USI `Threads`, range 1..256).
void set_num_threads(int threads);
int  get_num_threads();
int  default_hardware_threads();

// Cleanly shut down persistent Lazy SMP worker threads before process exit.
void search_shutdown_threads();

// Configure/query `nodelimit` (0 = unlimited, x > 0 = strict cap of x total nodes).
void     set_nodelimit(uint64_t limit);
uint64_t get_nodelimit();

// Configure Transposition Table size in MiB (USI_Hash option, default 256 MiB).
void set_tt_size_mb(size_t mb);

// Clear Transposition Table and all worker history heuristics (called on `usinewgame`).
void search_clear_game_state();

// Start the clock for a search
void search_init_time();

// Has the allocated hard time expired?
bool time_up(int allotted_ms);

// Legacy per-move time helper (preserved for unit-test compatibility).
int compute_allotted_ms(int my_time_ms, int byoyomi_ms);

// YaneuraOu-style soft/hard time budget supporting btime/wtime + binc/winc + byoyomi,
// optionally guided by learned game progress (`progress` in [0.0, 1.0]; < 0 uses ply only).
TimeBudget compute_time_budget(int my_time_ms, int my_inc_ms, int byoyomi_ms,
                               int game_ply = 0, double game_progress = -1.0);

// Iterative-deepening entry point (fixed budget: soft == hard == allotted_ms).
Move iterative_deepening(Board& board, int allotted_ms,
                         const std::function<void(const SearchInfo&)>& info_cb = {},
                         const std::function<void()>& started_cb = {},
                         int max_depth_limit = MAX_DEPTH);

// Iterative-deepening entry point with separate soft/hard time limits, Lazy SMP multithreading,
// and optional per-go node cap (`go_nodes_limit`, combined with `nodelimit` option when > 0).
Move iterative_deepening_timed(Board& board, TimeBudget budget,
                               const std::function<void(const SearchInfo&)>& info_cb = {},
                               const std::function<void()>& started_cb = {},
                               int max_depth_limit = MAX_DEPTH,
                               uint64_t go_nodes_limit = 0);

// Single-depth negamax alpha-beta (exposed for testing)
int negamax(Board& board, int depth, int alpha, int beta, int ply);

SearchStats last_search_stats();
