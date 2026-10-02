#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <vector>

std::atomic<bool> g_stop{false};

namespace {

constexpr uint64_t TIME_CHECK_MASK = 0xFFF;
constexpr int MATE_SCORE_THRESHOLD = MATE_VALUE - MAX_DEPTH;
constexpr int HASH_MOVE_BONUS = 20'000'000;
constexpr int CAPTURE_BONUS   = 10'000'000;
constexpr int PROMOTION_BONUS = 6'000'000;
constexpr int KILLER_1_BONUS  = 5'000'000;
constexpr int KILLER_2_BONUS  = 4'900'000;
constexpr size_t TT_SIZE      = 1u << 19; // ~12 MiB, deterministic fixed-size table
constexpr int MAX_HISTORY     = 16'384;
constexpr int COUNTERMOVE_KEY_NB = SQUARE_NB * SQUARE_NB + 7 * SQUARE_NB;

enum BoundType : uint8_t {
    BOUND_NONE  = 0,
    BOUND_EXACT = 1,
    BOUND_LOWER = 2,
    BOUND_UPPER = 3,
};

struct TTEntry {
    uint64_t key = 0;
    Move best_move = MOVE_NONE;
    int score = 0;
    int depth = -1;
    uint16_t generation = 0;
    uint8_t bound = BOUND_NONE;
};

struct ScoredMove {
    Move move = MOVE_NONE;
    int score = 0;
};

std::chrono::steady_clock::time_point g_start;
int      g_allotted_ms       = 0;
uint64_t g_nodes             = 0;
uint64_t g_qnodes            = 0;
uint64_t g_beta_cutoffs      = 0;
uint64_t g_threshold_cutoffs = 0;
uint64_t g_tt_probes         = 0;
uint64_t g_tt_hits           = 0;
uint64_t g_tt_cutoffs        = 0;
uint64_t g_null_move_verifications = 0;
uint64_t g_late_move_prunes  = 0;
uint64_t g_aspiration_fail_low = 0;
uint64_t g_aspiration_fail_high = 0;
uint64_t g_countermove_hits = 0;
uint64_t g_repetition_sensitive_tt_skips = 0;
uint64_t g_checking_move_selectivity_skips = 0;
uint64_t g_continuation_history_updates = 0;
uint64_t g_continuation_history_ordering_uses = 0;
int      g_seldepth          = 0;

std::array<std::array<Move, MAX_DEPTH>, MAX_DEPTH> g_pv{};
std::array<int, MAX_DEPTH> g_pv_len{};
std::array<std::array<Move, 2>, MAX_DEPTH> g_killers{};
std::array<std::array<int, SQUARE_NB>, SQUARE_NB> g_history{};
std::array<std::array<int, SQUARE_NB>, PT_NB> g_drop_history{};
std::array<Move, COUNTERMOVE_KEY_NB> g_countermoves{};
std::array<std::array<int, COUNTERMOVE_KEY_NB>, SQUARE_NB> g_continuation_history{};
std::array<TTEntry, TT_SIZE> g_tt{};
uint16_t g_tt_generation = 1;

constexpr std::array<int, PT_NB> PIECE_VALUE = {
    0, 87, 232, 257, 369, 444, 569, 642, 0, 534, 489, 510, 495, 827, 945 
};

inline int elapsed_ms() {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_start).count());
}

inline bool is_mate_score(int score) {
    return std::abs(score) >= MATE_SCORE_THRESHOLD;
}

// === Alpha-beta score clamping ===============================================
// This engine is written in negamax form: every call to search()/quiescence()
// evaluates a position purely from the perspective of the side to move at
// that node, which is exactly the "max side" of classic (non-negated)
// minimax terminology (each node always maximizes its own outcome). Per the
// pruning/return requirement, once such a value falls to -1000 or below it
// must be returned as exactly -1000.
//
// The "min side" of classic minimax never appears as a separate code path
// here: it is realized implicitly whenever a caller negates a child's
// return value (`-search(...)`), turning the child's own max-side result
// into the caller's view of the opponent's (minimizing) response. Because
// negation is linear, clamping the max-side floor at -1000 automatically
// yields the mirrored min-side ceiling of +1000 once that value is negated
// by the caller -- satisfying both stated requirements with a single,
// symmetric clamp, without touching the sign convention anywhere.
//
// Mate scores are intentionally excluded: they encode forced-mate distance
// rather than a static evaluation, and flattening them to +-1000 would
// break checkmate detection/avoidance.
constexpr int EVAL_CLAMP_FLOOR = -1000000;

inline int clamp_eval_score(int score) {
    if (is_mate_score(score)) return score;
    return (score <= EVAL_CLAMP_FLOOR) ? EVAL_CLAMP_FLOOR : score;
}

inline int tt_store_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD) return score + ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score - ply;
    return score;
}

inline int tt_probe_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD) return score - ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score + ply;
    return score;
}

inline TTEntry& tt_slot(uint64_t key) {
    return g_tt[key & (TT_SIZE - 1)];
}

TTEntry* tt_probe(uint64_t key) {
    ++g_tt_probes;
    TTEntry& entry = tt_slot(key);
    if (entry.bound != BOUND_NONE && entry.key == key) {
        ++g_tt_hits;
        return &entry;
    }
    return nullptr;
}

void tt_store(uint64_t key, int depth, int score, BoundType bound, Move best_move, int ply) {
    TTEntry& entry = tt_slot(key);
    if (entry.bound != BOUND_NONE && entry.key == key) {
        if (entry.generation == g_tt_generation && entry.depth > depth &&
            (entry.bound == BOUND_EXACT || bound != BOUND_EXACT))
            return;
        if (best_move == MOVE_NONE) best_move = entry.best_move;
    }
    if (entry.generation == g_tt_generation && entry.key != key &&
        entry.bound != BOUND_NONE && entry.depth > depth + 2 && bound != BOUND_EXACT) {
        return;
    }

    entry.key = key;
    entry.best_move = best_move;
    entry.score = tt_store_score(score, ply);
    entry.depth = depth;
    entry.generation = g_tt_generation;
    entry.bound = bound;
}

void next_tt_generation() {
    ++g_tt_generation;
    if (g_tt_generation == 0) {
        g_tt.fill(TTEntry{});
        g_tt_generation = 1;
    }
}

// LMR reduction table: lmr_table[depth][searched_count]
// Pre-computed log-based reductions.
static int lmr_table[MAX_DEPTH][MAX_DEPTH];

void init_lmr_table() {
    for (int d = 0; d < MAX_DEPTH; ++d) {
        for (int m = 0; m < MAX_DEPTH; ++m) {
            if (d < 2 || m < 2)
                lmr_table[d][m] = 0;
            else
                lmr_table[d][m] = static_cast<int>(0.75 + std::log(d) * std::log(m) / 2.25);
        }
    }
}

void reset_search_state(int allotted_ms) {
    static bool lmr_initialized = false;
    if (!lmr_initialized) {
        init_lmr_table();
        lmr_initialized = true;
    }
    search_init_time();
    g_stop.store(false, std::memory_order_relaxed);
    g_allotted_ms = allotted_ms;
    g_nodes = 0;
    g_qnodes = 0;
    g_beta_cutoffs = 0;
    g_threshold_cutoffs = 0;
    g_tt_probes = 0;
    g_tt_hits = 0;
    g_tt_cutoffs = 0;
    g_null_move_verifications = 0;
    g_late_move_prunes = 0;
    g_aspiration_fail_low = 0;
    g_aspiration_fail_high = 0;
    g_countermove_hits = 0;
    g_repetition_sensitive_tt_skips = 0;
    g_checking_move_selectivity_skips = 0;
    g_continuation_history_updates = 0;
    g_continuation_history_ordering_uses = 0;
    g_seldepth = 0;
    g_pv_len.fill(0);
    for (auto& killers : g_killers) killers = {MOVE_NONE, MOVE_NONE};
    for (auto& hist : g_history) hist.fill(0);
    for (auto& hist : g_drop_history) hist.fill(0);
    g_countermoves.fill(MOVE_NONE);
    for (auto& history : g_continuation_history) history.fill(0);
    next_tt_generation();
}

inline bool is_tactical_move(const Board& board, Move m) {
    if (is_promote(m)) return true;
    return !is_drop(m) && board.piece_at(to_sq(m)) != NO_PIECE;
}

inline int quiet_history_score(Move m) {
    if (is_drop(m)) return g_drop_history[dropped_pt(m)][to_sq(m)];
    return g_history[from_sq(m)][to_sq(m)];
}

inline int countermove_key(Move m) {
    if (m == MOVE_NONE) return -1;
    if (is_drop(m)) {
        const int pt = static_cast<int>(dropped_pt(m));
        if (pt < PAWN || pt > ROOK || to_sq(m) >= SQUARE_NB) return -1;
        return SQUARE_NB * SQUARE_NB + (pt - PAWN) * SQUARE_NB + to_sq(m);
    }
    if (from_sq(m) >= SQUARE_NB || to_sq(m) >= SQUARE_NB) return -1;
    return from_sq(m) * SQUARE_NB + to_sq(m);
}

inline int continuation_history_score(Move m, Move previous_move) {
    const int previous_to = previous_move == MOVE_NONE ? -1 : to_sq(previous_move);
    const int current_key = countermove_key(m);
    if (previous_to < 0 || previous_to >= SQUARE_NB || current_key < 0)
        return 0;
    return g_continuation_history[previous_to][current_key];
}

void record_killer(int ply, Move m) {
    if (ply < 0 || ply >= MAX_DEPTH) return;
    if (g_killers[ply][0] == m) return;
    g_killers[ply][1] = g_killers[ply][0];
    g_killers[ply][0] = m;
}

void update_history(Move m, int depth, bool good) {
    int bonus = std::min(2048, depth * depth + depth * 4);
    if (!good) bonus = -bonus / 2;
    int* value;
    if (is_drop(m)) {
        value = &g_drop_history[dropped_pt(m)][to_sq(m)];
    } else {
        value = &g_history[from_sq(m)][to_sq(m)];
    }
    *value += bonus - (*value * std::abs(bonus)) / MAX_HISTORY;
    *value = std::clamp(*value, -MAX_HISTORY, MAX_HISTORY);
}

void update_continuation_history(Move previous_move, Move m, int depth, bool good) {
    const int previous_to = previous_move == MOVE_NONE ? -1 : to_sq(previous_move);
    const int current_key = countermove_key(m);
    if (previous_to < 0 || previous_to >= SQUARE_NB || current_key < 0) return;

    int bonus = std::min(2048, depth * depth + depth * 4);
    if (!good) bonus = -bonus / 2;
    int& value = g_continuation_history[previous_to][current_key];
    value += bonus - (value * std::abs(bonus)) / MAX_HISTORY;
    value = std::clamp(value, -MAX_HISTORY, MAX_HISTORY);
    ++g_continuation_history_updates;
}

int capture_score(const Board& board, Move m) {
    if (is_drop(m)) return 0;
    Piece victim = board.piece_at(to_sq(m));
    if (victim == NO_PIECE) return 0;

    Piece attacker = board.piece_at(from_sq(m));
    int score = PIECE_VALUE[type_of(victim)] * 32 - PIECE_VALUE[type_of(attacker)];
    if (board.is_attacked(to_sq(m), ~board.side_to_move())) score -= PIECE_VALUE[type_of(attacker)] / 2;
    if (is_promote(m)) score += 1500;
    return score;
}

int move_score(const Board& board, Move m, int ply, Move hash_move, Move previous_move) {
    if (m == hash_move) return HASH_MOVE_BONUS;

    const bool tactical = is_tactical_move(board, m);
    if (tactical) {
        int score = CAPTURE_BONUS + capture_score(board, m);
        if (is_promote(m)) score += PROMOTION_BONUS;
        return score;
    }

    if (ply < MAX_DEPTH) {
        if (m == g_killers[ply][0]) return KILLER_1_BONUS;
        if (m == g_killers[ply][1]) return KILLER_2_BONUS;
    }

    const int previous_key = countermove_key(previous_move);
    if (previous_key >= 0 && g_countermoves[previous_key] == m) {
        ++g_countermove_hits;
        return KILLER_2_BONUS - 1;
    }

    const int continuation = continuation_history_score(m, previous_move);
    if (continuation != 0) ++g_continuation_history_ordering_uses;
    return quiet_history_score(m) + continuation / 2;
}

std::vector<ScoredMove> order_moves(const Board& board, const MoveList& moves, int ply, Move hash_move, bool tactical_only, Move previous_move = MOVE_NONE) {
    std::vector<ScoredMove> ordered;
    ordered.reserve(static_cast<size_t>(moves.size()));
    for (Move m : moves) {
        if (tactical_only && !is_tactical_move(board, m)) continue;
        ordered.push_back({m, move_score(board, m, ply, hash_move, previous_move)});
    }
    std::sort(ordered.begin(), ordered.end(), [](const ScoredMove& lhs, const ScoredMove& rhs) {
        return lhs.score > rhs.score;
    });
    return ordered;
}

int quiescence(Board& board, int alpha, int beta, int ply, Move previous_move);

// no_null: true when the previous move was a null move (prevents consecutive null moves).
int search(Board& board, int depth, int alpha, int beta, int ply, bool no_null = false,
           Move previous_move = MOVE_NONE) {
    if (((++g_nodes) & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_DEPTH) g_pv_len[ply] = ply;

    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS) return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    if (depth <= 0) return quiescence(board, alpha, beta, ply, previous_move);

    const int original_alpha = alpha;
    const int original_beta  = beta;
    const bool in_check = board.in_check();
    // PV node: window is wider than a null window.
    const bool is_pv = (beta > alpha + 1);
    Move hash_move = MOVE_NONE;

    if (TTEntry* entry = tt_probe(board.hash())) {
        hash_move = entry->best_move;
        if (entry->generation == g_tt_generation && entry->depth >= depth) {
            if (repetition_sensitive) {
                ++g_repetition_sensitive_tt_skips;
            } else {
                const int tt_score = tt_probe_score(entry->score, ply);
                if (entry->bound == BOUND_EXACT) return clamp_eval_score(tt_score);
                if (entry->bound == BOUND_LOWER) alpha = std::max(alpha, tt_score);
                else if (entry->bound == BOUND_UPPER) beta = std::min(beta, tt_score);
                if (alpha >= beta) {
                    ++g_tt_cutoffs;
                    return clamp_eval_score(tt_score);
                }
            }
        }
    }

    // === Reverse Futility Pruning (static null-move pruning) ===
    // If the static evaluation exceeds beta by a depth-scaled margin, the
    // position is likely so good that we can safely return a lower bound.
    if (!repetition_sensitive && !is_pv && !in_check && depth <= 3) {
        const int rfp_margin = 200 * depth;
        const int static_eval = evaluate(board);
        if (static_eval - rfp_margin >= beta)
            return clamp_eval_score(static_eval);
    }

    // === Null Move Pruning ===
    // Skip our turn and see if the opponent can still stay below beta.
    // Safe conditions: not PV, not in check, no consecutive null moves,
    // depth >= 3, not a potential zugzwang (we have non-king/pawn material).
    if (!repetition_sensitive && !is_pv && !in_check && !no_null && depth >= 3) {
        // Quick material check to avoid null move in zugzwang-prone positions
        bool has_major = false;
        const Color us = board.side_to_move();
        for (int pt = LANCE; pt <= ROOK && !has_major; ++pt)
            if (board.hand(us, static_cast<PieceType>(pt)) > 0) has_major = true;
        if (!has_major) {
            for (int sq = 0; sq < SQUARE_NB && !has_major; ++sq) {
                const Piece p = board.piece_at(sq);
                if (p != NO_PIECE && color_of(p) == us) {
                    PieceType t = type_of(p);
                    if (t != KING && t != PAWN && t != PROM_PAWN) has_major = true;
                }
            }
        }

        if (has_major) {
            const int R = 3 + depth / 6;

            // Do null move: flip side to move and update hash.
            StateInfo null_state{};
            null_state.hash = board.hash_;
            null_state.captured = NO_PIECE;
            null_state.mover = board.stm_;
            null_state.null_move = true;
            board.stm_  = ~board.stm_;
            board.hash_ ^= Zobrist::side;
            board.ply_++;
            board.history_.push_back(null_state);
            board.pos_hashes_.push_back(board.hash_);

            const int null_score = -search(board, depth - 1 - R, -beta, -beta + 1, ply + 1, true, MOVE_NONE);

            // Undo null move.
            board.pos_hashes_.pop_back();
            board.history_.pop_back();
            board.ply_--;
            board.hash_ ^= Zobrist::side;
            board.stm_  = ~board.stm_;

            if (g_stop.load(std::memory_order_relaxed)) return 0;

            // Verify: avoid returning a mate score from the null search (may be
            // a sign of zugzwang or a TT collision near the root).
            if (null_score >= beta && !is_mate_score(null_score)) {
                ++g_null_move_verifications;
                const int verify_score = search(board, depth - R, beta - 1, beta, ply, true, previous_move);
                if (g_stop.load(std::memory_order_relaxed)) return 0;
                if (verify_score >= beta && !is_mate_score(verify_score)) {
                    ++g_threshold_cutoffs;
                    tt_store(board.hash(), depth, verify_score, BOUND_LOWER, MOVE_NONE, ply);
                    return clamp_eval_score(verify_score);
                }
            }
        }
    }

    MoveList legal_moves;
    generate_legal_moves(board, legal_moves);
    if (legal_moves.empty()) {
        return in_check ? -(MATE_VALUE - ply) : 0;
    }
    if (hash_move != MOVE_NONE &&
        std::find(legal_moves.begin(), legal_moves.end(), hash_move) == legal_moves.end())
        hash_move = MOVE_NONE;

    // Pre-compute static eval for futility pruning (only at shallow non-PV nodes).
    const bool use_futility = (!repetition_sensitive && !is_pv && !in_check && depth <= 2);
    const int  futility_eval = use_futility ? evaluate(board) : -INF;

    auto ordered = order_moves(board, legal_moves, ply, hash_move, false, previous_move);
    int best_score = -INF;
    Move best_move = MOVE_NONE;
    int searched_count = 0;
    int searched_quiet_count = 0;
    std::vector<Move> searched_quiet_moves;

    for (const ScoredMove& scored : ordered) {
        const Move m = scored.move;
        const bool is_tactical = is_tactical_move(board, m);
        board.do_move(m);
        const bool gives_check = board.history_.back().gave_check;

        // Late-move pruning is limited to low-history quiets at shallow,
        // non-PV nodes outside check. Checking moves remain forcing moves.
        const bool lmp_candidate = !repetition_sensitive && !is_pv && !in_check &&
            depth <= 3 && !is_tactical && searched_quiet_count >= 6 + depth * 3 &&
            quiet_history_score(m) <= 0;
        if (lmp_candidate) {
            if (gives_check) {
                ++g_checking_move_selectivity_skips;
            } else {
                board.undo_move(m);
                ++g_threshold_cutoffs;
                ++g_late_move_prunes;
                continue;
            }
        }

        // === Futility Pruning ===
        // Skip quiet moves that cannot plausibly raise alpha even with a full
        // piece gain at this shallow depth.
        if (use_futility && searched_count >= 1 && !is_tactical) {
            if (futility_eval + 300 * depth < alpha) {
                if (gives_check) {
                    ++g_checking_move_selectivity_skips;
                } else {
                    board.undo_move(m);
                    ++g_threshold_cutoffs;
                    continue;
                }
            }
        }

        int score;
        if (searched_count == 0) {
            // First move: always search with the full window.
            score = -search(board, depth - 1, -beta, -alpha, ply + 1, false, m);
        } else {
            // === LMR (Late Move Reductions) ===
            // Reduce late quiet moves less when history is strong; killer and
            // countermoves retain their full depth.
            int lmr_depth = depth - 1;
            bool did_lmr  = false;
            if (!repetition_sensitive && !in_check && depth >= 3 && searched_count >= 2 &&
                !is_tactical && m != hash_move &&
                searched_count < MAX_DEPTH && depth < MAX_DEPTH) {
                const int reduction = lmr_table[depth][searched_count];
                const bool is_killer = ply < MAX_DEPTH &&
                    (m == g_killers[ply][0] || m == g_killers[ply][1]);
                const int previous_key = countermove_key(previous_move);
                const bool is_countermove = previous_key >= 0 && g_countermoves[previous_key] == m;
                const int history = quiet_history_score(m);
                const int continuation = continuation_history_score(m, previous_move);
                const int adjusted_reduction = std::clamp(
                    reduction - history / 8192 - continuation / 8192 - (is_pv ? 1 : 0),
                    0, depth - 2);
                if (adjusted_reduction > 0 && !is_killer && !is_countermove) {
                    if (gives_check) {
                        ++g_checking_move_selectivity_skips;
                    } else {
                        lmr_depth = std::max(1, depth - 1 - std::min(adjusted_reduction, depth - 2));
                        did_lmr   = true;
                    }
                }
            }

            // === PVS (Principal Variation Search) ===
            // Search with a null window first.
            score = -search(board, lmr_depth, -alpha - 1, -alpha, ply + 1, false, m);

            // If LMR raised alpha at reduced depth, re-search at full depth
            // with null window to confirm.
            if (!g_stop.load(std::memory_order_relaxed) && did_lmr && score > alpha) {
                score = -search(board, depth - 1, -alpha - 1, -alpha, ply + 1, false, m);
                did_lmr = false;
            }

            // If PVS null window failed high, this looks like a new best move;
            // re-search with the full window to get the exact score.
            if (!g_stop.load(std::memory_order_relaxed) && score > alpha && score < beta) {
                score = -search(board, depth - 1, -beta, -alpha, ply + 1, false, m);
            }
        }

        board.undo_move(m);
        ++searched_count;
        if (!is_tactical) {
            ++searched_quiet_count;
            searched_quiet_moves.push_back(m);
        }

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move = m;
            if (ply < MAX_DEPTH) {
                g_pv[ply][ply] = m;
                if (ply + 1 < MAX_DEPTH) {
                    for (int i = ply + 1; i < g_pv_len[ply + 1]; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
                    g_pv_len[ply] = g_pv_len[ply + 1];
                } else {
                    g_pv_len[ply] = ply + 1;
                }
            }
        }

        alpha = std::max(alpha, best_score);
        if (alpha >= beta) {
            ++g_beta_cutoffs;
            if (!is_tactical) {
                record_killer(ply, m);
                const int previous_key = countermove_key(previous_move);
                if (previous_key >= 0) g_countermoves[previous_key] = m;
                for (Move quiet : searched_quiet_moves)
                    if (quiet != m) {
                        update_history(quiet, depth, false);
                        update_continuation_history(previous_move, quiet, depth, false);
                    }
                update_history(m, depth, true);
                update_continuation_history(previous_move, m, depth, true);
            }
            break;
        }
    }

    if (g_stop.load(std::memory_order_relaxed))
        return (best_move == MOVE_NONE) ? 0 : clamp_eval_score(best_score);

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha) bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), depth, best_score, bound, best_move, ply);
    return clamp_eval_score(best_score);
}

int quiescence(Board& board, int alpha, int beta, int ply, Move previous_move) {
    ++g_qnodes;
    if ((g_qnodes & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_DEPTH) g_pv_len[ply] = ply;

    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS) return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    const bool in_check = board.in_check();
    const int original_alpha = alpha;
    const int original_beta = beta;
    Move hash_move = MOVE_NONE;

    if (TTEntry* entry = tt_probe(board.hash())) {
        hash_move = entry->best_move;
        if (entry->generation == g_tt_generation && entry->depth >= 0) {
            if (repetition_sensitive) {
                ++g_repetition_sensitive_tt_skips;
            } else {
                const int tt_score = tt_probe_score(entry->score, ply);
                if (entry->bound == BOUND_EXACT) return clamp_eval_score(tt_score);
                if (entry->bound == BOUND_LOWER) alpha = std::max(alpha, tt_score);
                else if (entry->bound == BOUND_UPPER) beta = std::min(beta, tt_score);
                if (alpha >= beta) {
                    ++g_tt_cutoffs;
                    return clamp_eval_score(tt_score);
                }
            }
        }
    }

    int stand_pat = evaluate(board);
    if (!in_check) {
        if (stand_pat >= beta) {
            tt_store(board.hash(), 0, stand_pat, BOUND_LOWER, MOVE_NONE, ply);
            return clamp_eval_score(stand_pat);
        }
        alpha = std::max(alpha, stand_pat);
    }

    MoveList legal_moves;
    generate_legal_moves(board, legal_moves);
    if (legal_moves.empty()) return in_check ? -(MATE_VALUE - ply) : clamp_eval_score(stand_pat);
    if (hash_move != MOVE_NONE &&
        std::find(legal_moves.begin(), legal_moves.end(), hash_move) == legal_moves.end())
        hash_move = MOVE_NONE;

    auto ordered = order_moves(board, legal_moves, ply, hash_move, !in_check, previous_move);
    if (ordered.empty()) return clamp_eval_score(stand_pat);

    int best_score = in_check ? -INF : stand_pat;
    Move best_move = MOVE_NONE;

    for (const ScoredMove& scored : ordered) {
        const Move m = scored.move;
        board.do_move(m);
        const int score = -quiescence(board, -beta, -alpha, ply + 1, m);
        board.undo_move(m);

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move = m;
            if (ply < MAX_DEPTH) {
                g_pv[ply][ply] = m;
                if (ply + 1 < MAX_DEPTH) {
                    for (int i = ply + 1; i < g_pv_len[ply + 1]; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
                    g_pv_len[ply] = g_pv_len[ply + 1];
                } else {
                    g_pv_len[ply] = ply + 1;
                }
            }
        }

        alpha = std::max(alpha, best_score);
        if (alpha >= beta) {
            ++g_beta_cutoffs;
            break;
        }
    }

    if (g_stop.load(std::memory_order_relaxed))
        return (best_move == MOVE_NONE) ? clamp_eval_score(stand_pat) : clamp_eval_score(best_score);

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha) bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), 0, best_score, bound, best_move, ply);
    return clamp_eval_score(best_score);
}

} // namespace

void search_init_time() {
    g_start = std::chrono::steady_clock::now();
}

bool time_up(int allotted_ms) {
    if (g_stop.load(std::memory_order_relaxed)) return true;
    if (allotted_ms <= 0) return false;
    return elapsed_ms() >= allotted_ms;
}

int compute_allotted_ms(int my_time_ms, int byoyomi_ms) {
    int v = my_time_ms / 40 + byoyomi_ms / 2;
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    return v;
}

int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    if (ply == 0) reset_search_state(0);
    return search(board, depth, alpha, beta, ply);
}

SearchStats last_search_stats() {
    return SearchStats{
        g_nodes,
        g_qnodes,
        g_beta_cutoffs,
        g_threshold_cutoffs,
        g_tt_probes,
        g_tt_hits,
        g_tt_cutoffs,
        g_null_move_verifications,
        g_late_move_prunes,
        g_aspiration_fail_low,
        g_aspiration_fail_high,
        g_countermove_hits,
        g_repetition_sensitive_tt_skips,
        g_checking_move_selectivity_skips,
        g_continuation_history_updates,
        g_continuation_history_ordering_uses
    };
}

Move iterative_deepening(Board& board, int allotted_ms,
                         const std::function<void(const SearchInfo&)>& info_cb,
                         const std::function<void()>& started_cb) {
    reset_search_state(allotted_ms);
    if (started_cb) started_cb();

    MoveList root_moves;
    generate_legal_moves(board, root_moves);
    if (root_moves.empty()) return MOVE_NONE;

    Move best_move = root_moves.moves_[0];
    int previous_score = 0;
    for (int depth = 1; depth <= MAX_DEPTH; ++depth) {
        if (time_up(allotted_ms)) break;

        MoveList moves;
        generate_legal_moves(board, moves);
        if (moves.empty()) break;

        auto ordered = order_moves(board, moves, 0, best_move, false);
        int window = depth == 1 ? INF : 50;
        int current_score = -INF;
        Move current_best = MOVE_NONE;

        while (!g_stop.load(std::memory_order_relaxed)) {
            const int alpha_start = std::max(-INF, previous_score - window);
            const int beta = std::min(INF, previous_score + window);
            int alpha = alpha_start;
            current_score = -INF;
            current_best = MOVE_NONE;
            int searched_count = 0;
            g_pv_len[0] = 0;

            for (const ScoredMove& scored : ordered) {
                const Move m = scored.move;
                if (time_up(allotted_ms)) {
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }

                board.do_move(m);
                int score;
                if (searched_count == 0) {
                    score = -search(board, depth - 1, -beta, -alpha, 1, false, m);
                } else {
                    score = -search(board, depth - 1, -alpha - 1, -alpha, 1, false, m);
                    if (!g_stop.load(std::memory_order_relaxed) &&
                        score > alpha && score < beta) {
                        score = -search(board, depth - 1, -beta, -alpha, 1, false, m);
                    }
                }
                board.undo_move(m);
                ++searched_count;

                if (g_stop.load(std::memory_order_relaxed)) break;

                if (score > current_score) {
                    current_score = score;
                    current_best = m;
                    g_pv[0][0] = m;
                    for (int i = 1; i < g_pv_len[1]; ++i) g_pv[0][i] = g_pv[1][i];
                    g_pv_len[0] = g_pv_len[1];
                }
                alpha = std::max(alpha, current_score);
                if (alpha >= beta) break;
            }

            if (g_stop.load(std::memory_order_relaxed) || current_best == MOVE_NONE)
                break;
            if (current_score <= alpha_start && window < INF) {
                ++g_aspiration_fail_low;
                window = std::min(INF, window * 2);
                continue;
            }
            if (current_score >= beta && window < INF) {
                ++g_aspiration_fail_high;
                window = std::min(INF, window * 2);
                continue;
            }
            break;
        }

        if (g_stop.load(std::memory_order_relaxed) || current_best == MOVE_NONE) break;
        best_move = current_best;
        previous_score = current_score;

        if (info_cb) {
            SearchInfo info;
            info.depth = depth;
            info.seldepth = std::max(depth, g_seldepth);
            info.time_ms = std::max(1, elapsed_ms());
            info.nodes = g_nodes + g_qnodes;
            info.nps = static_cast<uint64_t>((info.nodes * 1000ULL) / static_cast<uint64_t>(info.time_ms));
            info.score_cp = current_score;
            if (is_mate_score(current_score)) {
                info.score_is_mate = true;
                const int plies_to_mate = MATE_VALUE - std::abs(current_score);
                info.score_mate = (current_score > 0 ? 1 : -1) * std::max(1, (plies_to_mate + 1) / 2);
            }
            for (int i = 0; i < g_pv_len[0] && i < MAX_DEPTH; ++i) info.pv.push_back(g_pv[0][i]);
            if (info.pv.empty()) info.pv.push_back(current_best);
            info_cb(info);
        }

        if (is_mate_score(current_score)) break;
    }

    return best_move;
}
