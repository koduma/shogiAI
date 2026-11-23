#pragma once
#include "board.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct BookMinimaxResult {
    bool              in_book       = false; // true iff current hash is in DB AND has >=1 book move
    bool              hash_present  = false; // true iff current hash itself exists in DB
    Move              best_move     = MOVE_NONE;
    double            black_winrate = 0.5;   // backed-up minimax Black winrate in [0.0, 1.0]
    double            stm_winrate   = 0.5;   // backed-up minimax side-to-move winrate in [0.0, 1.0]
    double            node_black_wr = 0.5;   // raw recorded Black winrate of current node
    int               score_cp_stm  = 0;     // Ponanza-logit centipawn conversion for GUI graph
    int               reached_depth = 0;
    uint64_t          visited_nodes = 0;
    std::vector<Move> pv;
};

// Next-generation Beam Search selection comparator (`mover` = side to move at current beam ply):
//   1. Unregistered nodes in `winrate_book.db` (`unregistered == true`) strictly first!
//   2. Black-POV evaluation (`black_eval`):
//        - `mover == BLACK` (先手番): descending sort (`a_black_eval > b_black_eval`)
//        - `mover == WHITE` (後手番): ascending sort  (`a_black_eval < b_black_eval`)
//   3. Deterministic Zobrist hash tie-breaker (`a_hash < b_hash`)
inline bool beam_candidate_better(Color mover,
                                  bool a_unregistered, int a_black_eval, uint64_t a_hash,
                                  bool b_unregistered, int b_black_eval, uint64_t b_hash) noexcept {
    if (a_unregistered != b_unregistered) {
        return a_unregistered > b_unregistered;
    }
    if (a_black_eval != b_black_eval) {
        return (mover == BLACK) ? (a_black_eval > b_black_eval)
                                : (a_black_eval < b_black_eval);
    }
    return a_hash < b_hash;
}

class WinrateBook {
public:
    void   clear() { table_.clear(); }
    size_t size() const { return table_.size(); }
    bool   empty() const { return table_.empty(); }

    bool   contains(uint64_t hash) const;
    double get_black_winrate(uint64_t hash, double fallback = 0.5) const;
    void   put(uint64_t hash, double black_winrate);

    // Load `(hash, black_winrate)` entries from `winrate_book.db`
    bool load(const std::string& path, std::string* err_out = nullptr);

    // Atomically write the entire `(hash, black_winrate)` table to `path`
    bool save_atomic(const std::string& path, std::string* err_out = nullptr) const;

    // Append a single `(hash, black_winrate)` record immediately for live durability
    static bool append_record(const std::string& path, uint64_t hash, double black_winrate);

    // Convert side-to-move winrate [0.0, 1.0] -> centipawns (`600 * ln(p / (1 - p))`)
    static int winrate_to_cp(double stm_winrate);

    // Minimax (alpha-beta) search over positions present in `table_`:
    //   - BLACK maximizes `black_winrate`
    //   - WHITE minimizes `black_winrate` (maximizes `1.0 - black_winrate`)
    // If `board.hash()` is not in `table_`, or has 0 legal continuations in `table_`,
    // returns `in_book = false` so the caller hands off to the regular Shogi AI `.exe`.
    BookMinimaxResult search_minimax(Board& board,
                                     int max_depth = 24,
                                     double max_transposition_drop = 0.35) const;

    const std::unordered_map<uint64_t, double>& entries() const { return table_; }

private:
    std::unordered_map<uint64_t, double> table_;
};
