#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

std::atomic<bool> g_stop{false};

namespace {

constexpr uint64_t TIME_CHECK_MASK = 0x3FF; // check clock every 1024 nodes
constexpr int MATE_SCORE_THRESHOLD = MATE_VALUE - MAX_DEPTH * 2;
constexpr int MAX_PLY              = 128;
constexpr int MAX_HISTORY          = 16'384;
constexpr int16_t EVAL_NONE        = -32768;

// Move context key: `[Color][PieceType][to_sq]` => 2 * 15 * 81 = 2,430
constexpr int MOVE_CTX_NB      = COLOR_NB * PT_NB * SQUARE_NB;
constexpr int SIDE_MOVE_CTX_NB = PT_NB * SQUARE_NB; // 1,215

enum BoundType : uint8_t {
    BOUND_NONE  = 0,
    BOUND_EXACT = 1,
    BOUND_LOWER = 2,
    BOUND_UPPER = 3,
};

// ============================================================
// 4-Way Clustered Transposition Table (64-byte cache-line aligned)
// ============================================================
struct TTEntry {
    uint32_t key32     = 0;          // mixed upper 32 bits of Zobrist hash
    int32_t  score     = 0;          // exact / bound score (supports MATE_VALUE = 900,000)
    Move     best_move = MOVE_NONE;
    int16_t  eval16    = EVAL_NONE;  // cached static evaluation
    int8_t   depth     = -1;
    uint8_t  gen_bound = 0;          // bits 2..7: generation (0..63), bits 0..1: BoundType

    uint8_t bound() const { return static_cast<uint8_t>(gen_bound & 0x3u); }
    uint8_t gen()   const { return static_cast<uint8_t>(gen_bound >> 2); }
};
static_assert(sizeof(TTEntry) == 16, "TTEntry must be 16 bytes");

struct alignas(64) TTCluster {
    TTEntry entry[4];
};
static_assert(sizeof(TTCluster) == 64, "TTCluster must fit one 64-byte cache line");

std::vector<TTCluster> g_tt;
size_t   g_tt_mask = 0;
uint8_t  g_tt_gen  = 1; // 6-bit generation counter (1..63)

void ensure_tt_initialized(size_t mb = 64) {
    if (!g_tt.empty()) return;
    size_t target_bytes = std::max<size_t>(4, mb) * 1024ULL * 1024ULL;
    size_t clusters = 1;
    while ((clusters << 1) * sizeof(TTCluster) <= target_bytes) {
        clusters <<= 1;
    }
    g_tt.assign(clusters, TTCluster{});
    g_tt_mask = clusters - 1;
}

inline uint32_t tt_key32(uint64_t key) {
    return static_cast<uint32_t>((key >> 32) ^ ((key * 0x9E3779B97F4A7C15ULL) >> 32));
}

inline int tt_age_diff(uint8_t cur_gen, uint8_t entry_gen) {
    return (static_cast<int>(cur_gen) - static_cast<int>(entry_gen)) & 0x3F;
}

// ============================================================
// Search Statistics & Heuristics State
// ============================================================
std::chrono::steady_clock::time_point g_start;
int      g_hard_ms           = 0;
bool     g_allow_old_gen_cutoff = true;
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
uint64_t g_singular_tt_candidates = 0;
uint64_t g_singular_extension_attempts = 0;
uint64_t g_singular_extensions = 0;
uint64_t g_exclusion_searches = 0;
int      g_seldepth          = 0;

std::array<std::array<Move, MAX_PLY>, MAX_PLY> g_pv{};
std::array<int, MAX_PLY> g_pv_len{};
std::array<std::array<Move, 2>, MAX_PLY> g_killers{};
std::array<int, MAX_PLY> g_eval_stack{};
std::array<int, MAX_PLY> g_move_ctx_stack{};

int16_t g_main_history[COLOR_NB][SQUARE_NB][SQUARE_NB]{};
int16_t g_drop_history[COLOR_NB][PT_NB][SQUARE_NB]{};
int16_t g_capture_history[PIECE_NB][SQUARE_NB][PT_NB]{};
Move    g_countermoves[MOVE_CTX_NB]{};
std::vector<int16_t> g_cont_hist; // [MOVE_CTX_NB][SIDE_MOVE_CTX_NB]

void ensure_history_initialized() {
    if (g_cont_hist.empty()) {
        g_cont_hist.assign(static_cast<size_t>(MOVE_CTX_NB) * SIDE_MOVE_CTX_NB, 0);
    }
}

inline int elapsed_ms() {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_start).count());
}

inline bool is_mate_score(int score) {
    return std::abs(score) >= MATE_SCORE_THRESHOLD;
}

inline int tt_store_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD)  return score + ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score - ply;
    return score;
}

inline int tt_probe_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD)  return score - ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score + ply;
    return score;
}

TTEntry* tt_probe(uint64_t key) {
    ++g_tt_probes;
    TTCluster& cluster = g_tt[key & g_tt_mask];
    const uint32_t k32 = tt_key32(key);
    for (int i = 0; i < 4; ++i) {
        if (cluster.entry[i].bound() != BOUND_NONE && cluster.entry[i].key32 == k32) {
            ++g_tt_hits;
            return &cluster.entry[i];
        }
    }
    return nullptr;
}

void tt_store(uint64_t key, int depth, int score, BoundType bound, Move best_move, int ply, int static_eval) {
    TTCluster& cluster = g_tt[key & g_tt_mask];
    const uint32_t k32 = tt_key32(key);
    const int8_t clamped_depth = static_cast<int8_t>(std::clamp(depth, -1, 125));
    const int16_t ev16 = (std::abs(static_eval) <= 30000) ? static_cast<int16_t>(static_eval) : EVAL_NONE;

    TTEntry* replace = nullptr;
    for (int i = 0; i < 4; ++i) {
        TTEntry& e = cluster.entry[i];
        if (e.bound() == BOUND_NONE || e.key32 == k32) {
            replace = &e;
            break;
        }
    }

    if (!replace) {
        replace = &cluster.entry[0];
        int worst_val = replace->depth - 4 * tt_age_diff(g_tt_gen, replace->gen());
        for (int i = 1; i < 4; ++i) {
            TTEntry& e = cluster.entry[i];
            int val = e.depth - 4 * tt_age_diff(g_tt_gen, e.gen());
            if (val < worst_val) {
                worst_val = val;
                replace = &e;
            }
        }
    }

    const bool same_key = (replace->bound() != BOUND_NONE && replace->key32 == k32);
    if (same_key) {
        if (best_move == MOVE_NONE) best_move = replace->best_move;
        if (bound != BOUND_EXACT && replace->gen() == g_tt_gen &&
            replace->depth > clamped_depth + 2) {
            if (ev16 != EVAL_NONE && replace->eval16 == EVAL_NONE)
                replace->eval16 = ev16;
            return;
        }
    }

    if (!same_key || ev16 != EVAL_NONE)
        replace->eval16 = ev16;
    replace->key32     = k32;
    replace->best_move = best_move;
    replace->score     = tt_store_score(score, ply);
    replace->depth     = clamped_depth;
    replace->gen_bound = static_cast<uint8_t>((g_tt_gen << 2) | bound);
}

void next_tt_generation() {
    g_tt_gen = static_cast<uint8_t>((g_tt_gen + 1) & 0x3Fu);
    if (g_tt_gen == 0) g_tt_gen = 1;
}

int lmr_table[MAX_DEPTH][64];

void init_lmr_table() {
    for (int d = 0; d < MAX_DEPTH; ++d) {
        for (int m = 0; m < 64; ++m) {
            if (d < 2 || m < 1)
                lmr_table[d][m] = 0;
            else
                lmr_table[d][m] = static_cast<int>(0.70 + std::log(d) * std::log(m + 1) / 2.15);
        }
    }
}

void decay_histories_for_new_search() {
    for (int c = 0; c < COLOR_NB; ++c) {
        for (int f = 0; f < SQUARE_NB; ++f)
            for (int t = 0; t < SQUARE_NB; ++t)
                g_main_history[c][f][t] /= 2;
        for (int pt = 0; pt < PT_NB; ++pt)
            for (int t = 0; t < SQUARE_NB; ++t)
                g_drop_history[c][pt][t] /= 2;
    }
}

void reset_search_state(int hard_ms, bool decay_hist) {
    static bool lmr_initialized = false;
    if (!lmr_initialized) {
        init_lmr_table();
        lmr_initialized = true;
    }
    ensure_tt_initialized();
    ensure_history_initialized();

    search_init_time();
    g_stop.store(false, std::memory_order_relaxed);
    g_hard_ms = hard_ms;
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
    g_singular_tt_candidates = 0;
    g_singular_extension_attempts = 0;
    g_singular_extensions = 0;
    g_exclusion_searches = 0;
    g_seldepth = 0;
    g_pv_len.fill(0);
    g_eval_stack.fill(-INF);
    g_move_ctx_stack.fill(-1);
    for (auto& killers : g_killers) killers = {MOVE_NONE, MOVE_NONE};
    if (decay_hist) decay_histories_for_new_search();
    next_tt_generation();
}

// ============================================================
// Move Classification & History Helpers
// ============================================================
inline bool is_tactical_move(const Board& board, Move m) {
    if (is_promote(m)) return true;
    return !is_drop(m) && board.piece_at(to_sq(m)) != NO_PIECE;
}

inline PieceType moved_after_pt(const Board& board, Move m) {
    if (is_drop(m)) return dropped_pt(m);
    PieceType pt = type_of(board.piece_at(from_sq(m)));
    return is_promote(m) ? promote_pt(pt) : pt;
}

inline int move_ctx_key(const Board& board, Color us, Move m) {
    if (m == MOVE_NONE) return -1;
    const PieceType pt = moved_after_pt(board, m);
    const Square    to = to_sq(m);
    if (pt <= NO_PT || pt >= PT_NB || to < 0 || to >= SQUARE_NB) return -1;
    return (static_cast<int>(us) * PT_NB + static_cast<int>(pt)) * SQUARE_NB + to;
}

inline int side_move_ctx_key(const Board& board, Move m) {
    if (m == MOVE_NONE) return -1;
    const PieceType pt = moved_after_pt(board, m);
    const Square    to = to_sq(m);
    if (pt <= NO_PT || pt >= PT_NB || to < 0 || to >= SQUARE_NB) return -1;
    return static_cast<int>(pt) * SQUARE_NB + to;
}

inline int quiet_history_score(Color us, Move m) {
    if (is_drop(m)) return g_drop_history[us][dropped_pt(m)][to_sq(m)];
    return g_main_history[us][from_sq(m)][to_sq(m)];
}

inline int cont_history_lookup(int prev_ctx, int side_ctx) {
    if (prev_ctx < 0 || side_ctx < 0) return 0;
    return g_cont_hist[static_cast<size_t>(prev_ctx) * SIDE_MOVE_CTX_NB + side_ctx];
}

inline void gravity_update(int16_t& entry, int bonus) {
    int v = entry;
    v += bonus - (v * std::abs(bonus)) / MAX_HISTORY;
    entry = static_cast<int16_t>(std::clamp(v, -MAX_HISTORY, MAX_HISTORY));
}

void record_killer(int ply, Move m) {
    if (ply < 0 || ply >= MAX_PLY) return;
    if (g_killers[ply][0] == m) return;
    g_killers[ply][1] = g_killers[ply][0];
    g_killers[ply][0] = m;
}

void update_quiet_histories(const Board& board, Color us, Move m, int depth, bool good,
                            int prev1_ctx, int prev2_ctx) {
    int bonus = std::min(2048, depth * depth * 16 + depth * 32);
    if (!good) bonus = -bonus * 3 / 4;

    if (is_drop(m)) {
        gravity_update(g_drop_history[us][dropped_pt(m)][to_sq(m)], bonus);
    } else {
        gravity_update(g_main_history[us][from_sq(m)][to_sq(m)], bonus);
    }

    const int sctx = side_move_ctx_key(board, m);
    if (sctx >= 0) {
        if (prev1_ctx >= 0) {
            gravity_update(g_cont_hist[static_cast<size_t>(prev1_ctx) * SIDE_MOVE_CTX_NB + sctx], bonus);
            ++g_continuation_history_updates;
        }
        if (prev2_ctx >= 0) {
            gravity_update(g_cont_hist[static_cast<size_t>(prev2_ctx) * SIDE_MOVE_CTX_NB + sctx], bonus * 3 / 4);
            ++g_continuation_history_updates;
        }
    }
}

void update_capture_history(const Board& board, Move m, int depth, bool good) {
    if (is_drop(m)) return;
    Piece attacker = board.piece_at(from_sq(m));
    if (attacker == NO_PIECE) return;
    Piece victim = board.piece_at(to_sq(m));
    PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;
    int bonus = std::min(2048, depth * depth * 12 + depth * 24);
    if (!good) bonus = -bonus / 2;
    gravity_update(g_capture_history[attacker][to_sq(m)][vic_pt], bonus);
}

inline int score_tactical_move(const Board& board, Move m) {
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const Piece  attacker = board.piece_at(from);
    const Piece  victim   = board.piece_at(to);
    const PieceType att_pt = type_of(attacker);
    const PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;

    int score = capture_swing_value(vic_pt) * 16 - PIECE_VALUE_BONANZA[att_pt];
    if (is_promote(m)) score += promote_gain_value(att_pt) * 16;
    score += g_capture_history[attacker][to][vic_pt] / 4;
    return score;
}

inline int score_quiet_move(const Board& board, Color us, Move m, int prev1_ctx, int prev2_ctx) {
    const int sctx  = side_move_ctx_key(board, m);
    const int cont1 = cont_history_lookup(prev1_ctx, sctx);
    const int cont2 = cont_history_lookup(prev2_ctx, sctx);
    if (cont1 != 0 || cont2 != 0) ++g_continuation_history_ordering_uses;
    return quiet_history_score(us, m) * 2 + cont1 * 2 + cont2;
}

// ============================================================
// Staged MovePicker (YaneuraOu-style: zero heap allocations!)
// ============================================================
struct ScoredMove {
    Move move;
    int  score;
};

class MovePicker {
public:
    enum Stage {
        ST_MAIN_TT = 0,
        ST_GEN_TACTICAL,
        ST_GOOD_TACTICAL,
        ST_KILLER_1,
        ST_KILLER_2,
        ST_COUNTERMOVE,
        ST_GEN_QUIET,
        ST_QUIET,
        ST_BAD_TACTICAL,
        ST_DONE,

        ST_EVASION_TT,
        ST_GEN_EVASION,
        ST_EVASION
    };

    MovePicker(Board& board, Move tt_move, int ply, bool in_check, int prev1_ctx, int prev2_ctx)
        : board_(board),
          us_(board.side_to_move()),
          tt_move_(tt_move),
          ply_(ply),
          in_check_(in_check),
          prev1_ctx_(prev1_ctx),
          prev2_ctx_(prev2_ctx) {
        if (in_check_) {
            stage_ = ST_EVASION_TT;
        } else {
            stage_ = ST_MAIN_TT;
            if (ply_ >= 0 && ply_ < MAX_PLY) {
                killer1_ = g_killers[ply_][0];
                killer2_ = g_killers[ply_][1];
            }
            if (prev1_ctx_ >= 0 && prev1_ctx_ < MOVE_CTX_NB) {
                countermove_ = g_countermoves[prev1_ctx_];
            }
        }
    }

    Move next_move(bool& out_is_bad_tactical) {
        out_is_bad_tactical = false;
        while (true) {
            switch (stage_) {
                case ST_MAIN_TT: {
                    stage_ = ST_GEN_TACTICAL;
                    if (tt_move_ != MOVE_NONE && is_move_legal(board_, tt_move_, false)) {
                        return tt_move_;
                    }
                    tt_move_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_TACTICAL: {
                    MoveList tlist;
                    generate_tactical_moves(board_, tlist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    bad_end_ = 0;
                    for (int i = 0; i < tlist.size(); ++i) {
                        Move m = tlist[i];
                        if (m == tt_move_) continue;
                        scored_[end_idx_++] = {m, score_tactical_move(board_, m)};
                    }
                    stage_ = ST_GOOD_TACTICAL;
                    break;
                }
                case ST_GOOD_TACTICAL: {
                    while (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        Piece attacker = board_.piece_at(from_sq(m));
                        Piece victim   = board_.piece_at(to_sq(m));
                        PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;
                        int chist = g_capture_history[attacker][to_sq(m)][vic_pt];
                        if (see_ge(board_, m, -chist / 32)) {
                            return m;
                        }
                        if (bad_end_ < 128) bad_tactical_[bad_end_++] = m;
                    }
                    stage_ = ST_KILLER_1;
                    break;
                }
                case ST_KILLER_1: {
                    stage_ = ST_KILLER_2;
                    if (killer1_ != MOVE_NONE && killer1_ != tt_move_ &&
                        !is_tactical_move(board_, killer1_) &&
                        is_move_legal(board_, killer1_, false)) {
                        return killer1_;
                    }
                    killer1_ = MOVE_NONE;
                    break;
                }
                case ST_KILLER_2: {
                    stage_ = ST_COUNTERMOVE;
                    if (killer2_ != MOVE_NONE && killer2_ != tt_move_ && killer2_ != killer1_ &&
                        !is_tactical_move(board_, killer2_) &&
                        is_move_legal(board_, killer2_, false)) {
                        return killer2_;
                    }
                    killer2_ = MOVE_NONE;
                    break;
                }
                case ST_COUNTERMOVE: {
                    stage_ = ST_GEN_QUIET;
                    if (countermove_ != MOVE_NONE && countermove_ != tt_move_ &&
                        countermove_ != killer1_ && countermove_ != killer2_ &&
                        !is_tactical_move(board_, countermove_) &&
                        is_move_legal(board_, countermove_, false)) {
                        ++g_countermove_hits;
                        return countermove_;
                    }
                    countermove_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_QUIET: {
                    MoveList qlist;
                    generate_quiet_moves(board_, qlist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    for (int i = 0; i < qlist.size(); ++i) {
                        Move m = qlist[i];
                        if (m == tt_move_ || m == killer1_ || m == killer2_ || m == countermove_)
                            continue;
                        scored_[end_idx_++] = {m, score_quiet_move(board_, us_, m, prev1_ctx_, prev2_ctx_)};
                    }
                    stage_ = ST_QUIET;
                    break;
                }
                case ST_QUIET: {
                    if (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        return m;
                    }
                    cur_idx_ = 0;
                    stage_ = ST_BAD_TACTICAL;
                    break;
                }
                case ST_BAD_TACTICAL: {
                    if (cur_idx_ < bad_end_) {
                        out_is_bad_tactical = true;
                        return bad_tactical_[cur_idx_++];
                    }
                    stage_ = ST_DONE;
                    return MOVE_NONE;
                }

                // ---- In-check evasion stages ----
                case ST_EVASION_TT: {
                    stage_ = ST_GEN_EVASION;
                    if (tt_move_ != MOVE_NONE && is_move_legal(board_, tt_move_, true)) {
                        return tt_move_;
                    }
                    tt_move_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_EVASION: {
                    MoveList elist;
                    generate_legal_moves(board_, elist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    for (int i = 0; i < elist.size(); ++i) {
                        Move m = elist[i];
                        if (m == tt_move_) continue;
                        int sc;
                        if (is_tactical_move(board_, m)) {
                            sc = 10'000'000 + score_tactical_move(board_, m);
                        } else {
                            sc = score_quiet_move(board_, us_, m, prev1_ctx_, prev2_ctx_);
                        }
                        scored_[end_idx_++] = {m, sc};
                    }
                    stage_ = ST_EVASION;
                    break;
                }
                case ST_EVASION: {
                    if (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        return m;
                    }
                    stage_ = ST_DONE;
                    return MOVE_NONE;
                }
                case ST_DONE:
                default:
                    return MOVE_NONE;
            }
        }
    }

    Move validated_tt_move() const { return tt_move_; }

private:
    static ScoredMove pick_best(ScoredMove* arr, int start, int end) {
        int best_i = start;
        int best_s = arr[start].score;
        for (int i = start + 1; i < end; ++i) {
            if (arr[i].score > best_s) {
                best_s = arr[i].score;
                best_i = i;
            }
        }
        if (best_i != start) std::swap(arr[start], arr[best_i]);
        return arr[start];
    }

    Board& board_;
    Color  us_;
    Move   tt_move_     = MOVE_NONE;
    Move   killer1_     = MOVE_NONE;
    Move   killer2_     = MOVE_NONE;
    Move   countermove_ = MOVE_NONE;
    int    ply_         = 0;
    bool   in_check_    = false;
    int    prev1_ctx_   = -1;
    int    prev2_ctx_   = -1;
    Stage  stage_       = ST_DONE;

    ScoredMove scored_[MoveList::MAX_MOVES];
    Move       bad_tactical_[128];
    int        cur_idx_ = 0;
    int        end_idx_ = 0;
    int        bad_end_ = 0;
};

int quiescence(Board& board, int alpha, int beta, int ply, int qply);

// ============================================================
// Main Negamax Alpha-Beta Search (PVS + YaneuraOu Selectivity)
// ============================================================
int search(Board& board, int depth, int alpha, int beta, int ply,
           bool no_null = false, Move excluded_move = MOVE_NONE, bool cut_node = false) {
    if (((++g_nodes) & TIME_CHECK_MASK) == 0) {
        if (time_up(g_hard_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_PLY) g_pv_len[ply] = ply;

    // 1. Repetition & perpetual-check detection
    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS)  return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    if (depth <= 0 || ply >= MAX_PLY - 2)
        return quiescence(board, alpha, beta, ply, 0);

    // 2. Mate distance pruning (guarded for synthetic test windows outside mate range)
    if (ply > 0 && alpha < MATE_SCORE_THRESHOLD && beta > -MATE_SCORE_THRESHOLD) {
        alpha = std::max(alpha, -(MATE_VALUE - ply));
        beta  = std::min(beta,   (MATE_VALUE - ply - 1));
        if (alpha >= beta) return alpha;
    }

    const int  original_alpha = alpha;
    const int  original_beta  = beta;
    const bool in_check       = board.in_check();
    const bool is_pv          = (beta > alpha + 1);
    const Color us            = board.side_to_move();

    Move hash_move = MOVE_NONE;
    int  tt_eval   = EVAL_NONE;
    bool try_singular_extension = false;
    int  singular_tt_score = 0;

    // 3. Transposition Table Probe (enabled across all generations!)
    if (excluded_move == MOVE_NONE) {
        if (TTEntry* entry = tt_probe(board.hash())) {
            hash_move = entry->best_move;
            tt_eval   = entry->eval16;
            const uint8_t bnd = entry->bound();
            const bool singular_bound = (bnd == BOUND_LOWER || bnd == BOUND_EXACT);

            if (!repetition_sensitive && ply > 0 && !is_pv && !in_check && depth >= 6 &&
                entry->depth >= depth - 3 && singular_bound && hash_move != MOVE_NONE) {
                ++g_singular_tt_candidates;
                singular_tt_score = tt_probe_score(entry->score, ply);
                try_singular_extension = !is_mate_score(singular_tt_score) &&
                    singular_tt_score >= beta - 2 * depth;
            }

            if ((g_allow_old_gen_cutoff || entry->gen() == g_tt_gen) && entry->depth >= depth && !try_singular_extension) {
                if (repetition_sensitive) {
                    ++g_repetition_sensitive_tt_skips;
                } else if (!is_pv || bnd == BOUND_EXACT) {
                    const int tt_score = tt_probe_score(entry->score, ply);
                    if (bnd == BOUND_EXACT) {
                        ++g_tt_cutoffs;
                        return tt_score;
                    }
                    if (!is_pv) {
                        if (bnd == BOUND_LOWER && tt_score >= beta) {
                            ++g_tt_cutoffs;
                            return tt_score;
                        }
                        if (bnd == BOUND_UPPER && tt_score <= alpha) {
                            ++g_tt_cutoffs;
                            return tt_score;
                        }
                    }
                }
            }
        }
    }

    // 4. Fast 1-ply mate detection (`mate1ply`) when not in check at shallow/frontier nodes
    if (excluded_move == MOVE_NONE && !in_check && depth <= 2 &&
        alpha < MATE_VALUE - (ply + 1)) {
        Move mate_m = MOVE_NONE;
        if (find_mate_1ply(board, &mate_m)) {
            const int mscore = MATE_VALUE - (ply + 1);
            if (ply < MAX_PLY) {
                g_pv[ply][ply] = mate_m;
                g_pv_len[ply]  = ply + 1;
            }
            tt_store(board.hash(), depth, mscore, BOUND_EXACT, mate_m, ply, EVAL_NONE);
            return mscore;
        }
    }

    // 5. Static evaluation & `improving` heuristic
    int static_eval = -INF;
    bool improving  = false;
    if (!in_check) {
        static_eval = (tt_eval != EVAL_NONE) ? tt_eval : evaluate(board);
        if (ply < MAX_PLY) {
            g_eval_stack[ply] = static_eval;
            improving = (ply >= 2 && g_eval_stack[ply - 2] != -INF &&
                         static_eval > g_eval_stack[ply - 2]);
        }
    } else if (ply < MAX_PLY) {
        g_eval_stack[ply] = -INF;
    }

    // 6. Internal Iterative Reductions (IIR): reduce depth when no TT move is available
    if (excluded_move == MOVE_NONE && !in_check && depth >= 4 && hash_move == MOVE_NONE) {
        depth -= 1;
    }

    // 7. Selective Pre-Move Pruning (non-root, non-PV, outside check, non-mate bounds)
    if (ply > 0 && excluded_move == MOVE_NONE && !repetition_sensitive &&
        !is_pv && !in_check && std::abs(beta) < MATE_SCORE_THRESHOLD) {

        // 7a. Reverse Futility Pruning
        if (depth <= 6) {
            const int rfp_margin = (improving ? 80 : 110) * depth;
            if (static_eval - rfp_margin >= beta) {
                ++g_threshold_cutoffs;
                return (static_eval + beta) / 2;
            }
        }

        // 7b. Razoring
        if (depth <= 2 && static_eval + 300 * depth <= alpha) {
            const int qscore = quiescence(board, alpha, alpha + 1, ply, 0);
            if (qscore <= alpha) {
                ++g_threshold_cutoffs;
                return qscore;
            }
        }

        // 7c. Null Move Pruning (requires `static_eval >= beta`!)
        if (!try_singular_extension && !no_null && depth >= 3 && static_eval >= beta) {
            bool has_major = false;
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
                const int R = 3 + depth / 4 + std::min(3, (static_eval - beta) / 200);
                if (ply < MAX_PLY) g_move_ctx_stack[ply] = -1;
                board.do_null_move();
                const int null_score = -search(board, depth - 1 - R, -beta, -beta + 1,
                                               ply + 1, true, MOVE_NONE, !cut_node);
                board.undo_null_move();

                if (g_stop.load(std::memory_order_relaxed)) return 0;

                if (null_score >= beta && !is_mate_score(null_score)) {
                    if (depth < 8) {
                        ++g_threshold_cutoffs;
                        return null_score;
                    }
                    ++g_null_move_verifications;
                    const int verify_score = search(board, depth - R, beta - 1, beta,
                                                    ply, true, MOVE_NONE, false);
                    if (g_stop.load(std::memory_order_relaxed)) return 0;
                    if (verify_score >= beta && !is_mate_score(verify_score)) {
                        ++g_threshold_cutoffs;
                        tt_store(board.hash(), depth, verify_score, BOUND_LOWER,
                                 MOVE_NONE, ply, static_eval);
                        return verify_score;
                    }
                }
            }
        }
    }

    // 8. Singular Extension Exclusion Search
    int singular_extension = 0;
    if (try_singular_extension && hash_move != MOVE_NONE &&
        is_move_legal(board, hash_move, in_check)) {
        ++g_singular_extension_attempts;
        const int singular_beta = singular_tt_score - 2 * depth;
        ++g_exclusion_searches;
        const int alternative_score = search(
            board, std::max(1, (depth - 1) / 2), singular_beta - 1, singular_beta,
            ply, true, hash_move, cut_node);
        if (g_stop.load(std::memory_order_relaxed)) return 0;
        if (alternative_score < singular_beta) {
            singular_extension = 1;
            ++g_singular_extensions;
        } else if (singular_beta >= beta && !is_mate_score(alternative_score)) {
            ++g_threshold_cutoffs;
            return singular_beta;
        }
    }

    // 9. Staged Move Loop
    const int prev1_ctx = (ply >= 1 && ply - 1 < MAX_PLY) ? g_move_ctx_stack[ply - 1] : -1;
    const int prev2_ctx = (ply >= 2 && ply - 2 < MAX_PLY) ? g_move_ctx_stack[ply - 2] : -1;
    if (ply + 1 < MAX_PLY) {
        g_killers[ply + 1] = {MOVE_NONE, MOVE_NONE};
    }

    const bool use_futility   = (excluded_move == MOVE_NONE && !repetition_sensitive &&
                                 !is_pv && !in_check && depth <= 4);
    const int  futility_limit = use_futility ? (static_eval + 100 + 135 * depth) : INF;
    const int  lmp_threshold  = improving ? (4 + depth * depth) : (3 + depth * depth * 3 / 4);

    MovePicker picker(board, hash_move, ply, in_check, prev1_ctx, prev2_ctx);
    Move best_move  = MOVE_NONE;
    int  best_score = -INF;
    int  searched_count = 0;
    int  searched_quiet_count = 0;
    int  legal_seen = 0;

    Move searched_quiets[96];
    int  n_searched_quiets = 0;
    Move searched_tacticals[64];
    int  n_searched_tacticals = 0;

    bool is_bad_tactical = false;
    for (Move m = picker.next_move(is_bad_tactical); m != MOVE_NONE;
         m = picker.next_move(is_bad_tactical)) {
        ++legal_seen;
        if (m == excluded_move) continue;

        const bool is_tactical = is_tactical_move(board, m);
        const bool gives_check = move_gives_check(board, m);
        const int  m_ctx       = move_ctx_key(board, us, m);

        if (searched_count >= 1 && excluded_move == MOVE_NONE && !repetition_sensitive &&
            !is_pv && !in_check && best_score > -MATE_SCORE_THRESHOLD) {

            // 9a. Late Move Pruning (LMP)
            if (!is_tactical && depth <= 4 && searched_quiet_count >= lmp_threshold &&
                quiet_history_score(us, m) <= 1024) {
                if (gives_check) {
                    ++g_checking_move_selectivity_skips;
                } else {
                    ++g_threshold_cutoffs;
                    ++g_late_move_prunes;
                    continue;
                }
            }

            // 9b. Futility Pruning
            if (!is_tactical && use_futility && futility_limit < alpha) {
                if (gives_check) {
                    ++g_checking_move_selectivity_skips;
                } else {
                    ++g_threshold_cutoffs;
                    continue;
                }
            }

            // 9c. Shallow SEE Pruning (guarded against synthetic test windows where alpha >= MATE_VALUE)
            if (depth <= 4 && !gives_check && alpha < MATE_SCORE_THRESHOLD) {
                const int see_margin = is_tactical ? (-95 * depth * depth) : (-60 * depth);
                if (!see_ge(board, m, see_margin)) {
                    ++g_threshold_cutoffs;
                    continue;
                }
            }
        }

        // 9d. Extensions (Check Extension + Singular Extension)
        int extension = 0;
        if (m == picker.validated_tt_move() && singular_extension > 0) {
            extension = 1;
        } else if (gives_check && ply < MAX_DEPTH &&
                   (in_check || see_ge(board, m, 0))) {
            extension = 1;
        }

        if (ply < MAX_PLY) g_move_ctx_stack[ply] = m_ctx;
        board.do_move(m);

        int score;
        const int new_depth = depth - 1 + extension;

        if (searched_count == 0) {
            score = -search(board, new_depth, -beta, -alpha, ply + 1, false, MOVE_NONE, false);
        } else {
            int lmr_depth = new_depth;
            bool did_lmr  = false;
            if (excluded_move == MOVE_NONE && !repetition_sensitive && !in_check &&
                depth >= 3 && searched_count >= 2 && m != picker.validated_tt_move()) {

                if (!is_tactical || is_bad_tactical) {
                    int r = lmr_table[std::min(depth, MAX_DEPTH - 1)][std::min(searched_count, 63)];
                    const bool is_killer = (ply < MAX_PLY) &&
                        (m == g_killers[ply][0] || m == g_killers[ply][1]);
                    const bool is_cm = (prev1_ctx >= 0 && g_countermoves[prev1_ctx] == m);

                    if (!improving) r += 1;
                    if (cut_node)   r += 1;
                    if (is_pv)      r -= 1;
                    if (is_killer || is_cm) r -= 1;

                    if (!is_tactical) {
                        const int sctx = (m_ctx >= 0) ? (m_ctx % SIDE_MOVE_CTX_NB) : -1;
                        const int hist_sum = quiet_history_score(us, m)
                            + cont_history_lookup(prev1_ctx, sctx)
                            + cont_history_lookup(prev2_ctx, sctx) / 2;
                        r -= hist_sum / 6144;
                    }

                    const int adj_r = std::clamp(r, 0, new_depth - 1);
                    if (adj_r > 0 && !(is_killer || is_cm)) {
                        if (gives_check) {
                            ++g_checking_move_selectivity_skips;
                        } else {
                            lmr_depth = std::max(1, new_depth - adj_r);
                            did_lmr   = true;
                        }
                    }
                }
            }

            score = -search(board, lmr_depth, -alpha - 1, -alpha, ply + 1,
                            false, MOVE_NONE, true);

            if (!g_stop.load(std::memory_order_relaxed) && did_lmr && score > alpha) {
                score = -search(board, new_depth, -alpha - 1, -alpha, ply + 1,
                                false, MOVE_NONE, !cut_node);
            }

            if (!g_stop.load(std::memory_order_relaxed) && is_pv && score > alpha && score < beta) {
                score = -search(board, new_depth, -beta, -alpha, ply + 1,
                                false, MOVE_NONE, false);
            }
        }

        board.undo_move(m);
        ++searched_count;
        if (!is_tactical) {
            ++searched_quiet_count;
            if (n_searched_quiets < 96) searched_quiets[n_searched_quiets++] = m;
        } else {
            if (n_searched_tacticals < 64) searched_tacticals[n_searched_tacticals++] = m;
        }

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            if (ply < MAX_PLY) {
                g_pv[ply][ply] = m;
                if (ply + 1 < MAX_PLY) {
                    const int child_len = std::min<int>(g_pv_len[ply + 1], MAX_PLY);
                    for (int i = ply + 1; i < child_len; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
                    g_pv_len[ply] = std::max(ply + 1, child_len);
                } else {
                    g_pv_len[ply] = ply + 1;
                }
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
            if (alpha >= beta) {
                ++g_beta_cutoffs;
                if (!is_tactical) {
                    record_killer(ply, m);
                    if (prev1_ctx >= 0) g_countermoves[prev1_ctx] = m;
                    for (int i = 0; i < n_searched_quiets; ++i) {
                        if (searched_quiets[i] != m)
                            update_quiet_histories(board, us, searched_quiets[i], depth, false, prev1_ctx, prev2_ctx);
                    }
                    update_quiet_histories(board, us, m, depth, true, prev1_ctx, prev2_ctx);
                } else {
                    for (int i = 0; i < n_searched_tacticals; ++i) {
                        if (searched_tacticals[i] != m)
                            update_capture_history(board, searched_tacticals[i], depth, false);
                    }
                    update_capture_history(board, m, depth, true);
                }
                break;
            }
        }
    }

    if (legal_seen == 0) {
        return in_check ? -(MATE_VALUE - ply) : 0;
    }
    if (excluded_move != MOVE_NONE && searched_count == 0) {
        return -INF;
    }
    if (g_stop.load(std::memory_order_relaxed)) {
        return (best_move == MOVE_NONE) ? 0 : best_score;
    }

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha)     bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    if (excluded_move == MOVE_NONE) {
        tt_store(board.hash(), depth, best_score, bound, best_move, ply, static_eval);
    }
    return best_score;
}

// ============================================================
// Quiescence Search (Tactical-only MoveGen + SEE + qply==0 Checks + Mate1Ply)
// ============================================================
int quiescence(Board& board, int alpha, int beta, int ply, int qply) {
    if (((++g_qnodes) & TIME_CHECK_MASK) == 0) {
        if (time_up(g_hard_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_PLY) g_pv_len[ply] = ply;

    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS)  return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    if (ply >= MAX_PLY - 2) return evaluate(board);

    const bool in_check       = board.in_check();
    const int  original_alpha = alpha;
    const int  original_beta  = beta;
    const bool is_pv          = (beta > alpha + 1);
    const Color us            = board.side_to_move();

    Move hash_move = MOVE_NONE;
    int  tt_eval   = EVAL_NONE;
    const int req_tt_depth = (!in_check && qply == 0) ? 0 : -1;

    if (TTEntry* entry = tt_probe(board.hash())) {
        hash_move = entry->best_move;
        tt_eval   = entry->eval16;
        const uint8_t bnd = entry->bound();
        if ((g_allow_old_gen_cutoff || entry->gen() == g_tt_gen) && entry->depth >= req_tt_depth) {
            if (repetition_sensitive) {
                ++g_repetition_sensitive_tt_skips;
            } else if (!is_pv || bnd == BOUND_EXACT) {
                const int tt_score = tt_probe_score(entry->score, ply);
                if (bnd == BOUND_EXACT) {
                    ++g_tt_cutoffs;
                    return tt_score;
                }
                if (!is_pv) {
                    if (bnd == BOUND_LOWER && tt_score >= beta) {
                        ++g_tt_cutoffs;
                        return tt_score;
                    }
                    if (bnd == BOUND_UPPER && tt_score <= alpha) {
                        ++g_tt_cutoffs;
                        return tt_score;
                    }
                }
            }
        }
    }

    int stand_pat = -INF;
    if (!in_check) {
        stand_pat = (tt_eval != EVAL_NONE) ? tt_eval : evaluate(board);
        if (stand_pat >= beta) {
            tt_store(board.hash(), req_tt_depth, stand_pat, BOUND_LOWER, MOVE_NONE, ply, stand_pat);
            return stand_pat;
        }
        if (stand_pat > alpha) alpha = stand_pat;

        if (qply == 0 && alpha < MATE_VALUE - (ply + 1)) {
            Move mate_m = MOVE_NONE;
            if (find_mate_1ply(board, &mate_m)) {
                const int mscore = MATE_VALUE - (ply + 1);
                if (ply < MAX_PLY) {
                    g_pv[ply][ply] = mate_m;
                    g_pv_len[ply]  = ply + 1;
                }
                tt_store(board.hash(), 0, mscore, BOUND_EXACT, mate_m, ply, stand_pat);
                return mscore;
            }
        }
    }

    ScoredMove candidates[MoveList::MAX_MOVES];
    int n_cand = 0;

    if (in_check) {
        MoveList evasions;
        generate_legal_moves(board, evasions);
        if (evasions.empty()) return -(MATE_VALUE - ply);

        const int prev1_ctx = (ply >= 1 && ply - 1 < MAX_PLY) ? g_move_ctx_stack[ply - 1] : -1;
        const int prev2_ctx = (ply >= 2 && ply - 2 < MAX_PLY) ? g_move_ctx_stack[ply - 2] : -1;
        for (int i = 0; i < evasions.size(); ++i) {
            Move m = evasions[i];
            int sc = (m == hash_move) ? 20'000'000
                   : is_tactical_move(board, m) ? (10'000'000 + score_tactical_move(board, m))
                   : score_quiet_move(board, us, m, prev1_ctx, prev2_ctx);
            candidates[n_cand++] = {m, sc};
        }
    } else {
        MoveList tlist;
        generate_tactical_moves(board, tlist);
        for (int i = 0; i < tlist.size(); ++i) {
            Move m = tlist[i];
            if (m == hash_move) {
                candidates[n_cand++] = {m, 20'000'000};
                continue;
            }
            const Piece victim = board.piece_at(to_sq(m));
            const PieceType att_pt = type_of(board.piece_at(from_sq(m)));
            if (victim == NO_PIECE && is_promote(m) &&
                att_pt != PAWN && att_pt != BISHOP && att_pt != ROOK &&
                !move_gives_check(board, m)) {
                continue;
            }
            const int max_gain = (victim != NO_PIECE ? capture_swing_value(type_of(victim)) : 0)
                               + (is_promote(m) ? promote_gain_value(att_pt) : 0);
            if (stand_pat + max_gain + 220 < alpha && !move_gives_check(board, m))
                continue;

            if (!see_ge(board, m, 0)) continue;

            candidates[n_cand++] = {m, 10'000'000 + score_tactical_move(board, m)};
        }

        if (qply == 0) {
            MoveList qchecks;
            generate_quiet_checks(board, qchecks);
            for (int i = 0; i < qchecks.size(); ++i) {
                Move m = qchecks[i];
                if (m == hash_move) {
                    candidates[n_cand++] = {m, 19'000'000};
                    continue;
                }
                if (!see_ge(board, m, 0)) continue;
                candidates[n_cand++] = {m, 1'000'000 + quiet_history_score(us, m)};
            }
        }
    }

    if (n_cand == 0) return stand_pat;

    int  best_score = in_check ? -INF : stand_pat;
    Move best_move  = MOVE_NONE;

    for (int idx = 0; idx < n_cand; ++idx) {
        int best_i = idx;
        for (int j = idx + 1; j < n_cand; ++j) {
            if (candidates[j].score > candidates[best_i].score) best_i = j;
        }
        if (best_i != idx) std::swap(candidates[idx], candidates[best_i]);

        const Move m = candidates[idx].move;
        if (ply < MAX_PLY) g_move_ctx_stack[ply] = move_ctx_key(board, us, m);
        board.do_move(m);
        const int score = -quiescence(board, -beta, -alpha, ply + 1, qply + 1);
        board.undo_move(m);

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            if (ply < MAX_PLY) {
                g_pv[ply][ply] = m;
                if (ply + 1 < MAX_PLY) {
                    const int child_len = std::min<int>(g_pv_len[ply + 1], MAX_PLY);
                    for (int i = ply + 1; i < child_len; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
                    g_pv_len[ply] = std::max(ply + 1, child_len);
                } else {
                    g_pv_len[ply] = ply + 1;
                }
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
            if (alpha >= beta) {
                ++g_beta_cutoffs;
                break;
            }
        }
    }

    if (g_stop.load(std::memory_order_relaxed))
        return (best_move == MOVE_NONE) ? (in_check ? 0 : stand_pat) : best_score;

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha)     bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), req_tt_depth, best_score, bound, best_move, ply,
             in_check ? EVAL_NONE : stand_pat);
    return best_score;
}

} // namespace

// ============================================================
// Public Search API
// ============================================================

void set_tt_size_mb(size_t mb) {
    size_t target_bytes = std::max<size_t>(1, mb) * 1024ULL * 1024ULL;
    size_t clusters = 1;
    while ((clusters << 1) * sizeof(TTCluster) <= target_bytes) {
        clusters <<= 1;
    }
    g_tt.assign(clusters, TTCluster{});
    g_tt_mask = clusters - 1;
    g_tt_gen  = 1;
}

void search_clear_game_state() {
    ensure_tt_initialized();
    ensure_history_initialized();
    std::fill(g_tt.begin(), g_tt.end(), TTCluster{});
    g_tt_gen = 1;
    std::memset(g_main_history, 0, sizeof(g_main_history));
    std::memset(g_drop_history, 0, sizeof(g_drop_history));
    std::memset(g_capture_history, 0, sizeof(g_capture_history));
    for (auto& cm : g_countermoves) cm = MOVE_NONE;
    std::fill(g_cont_hist.begin(), g_cont_hist.end(), static_cast<int16_t>(0));
    clear_eval_caches();
}

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

TimeBudget compute_time_budget(int my_time_ms, int my_inc_ms, int byoyomi_ms, int game_ply) {
    constexpr int SAFETY_MS = 50;
    if (my_time_ms <= 0 && byoyomi_ms > 0) {
        int hard = std::max(20, std::min(byoyomi_ms - SAFETY_MS, byoyomi_ms * 92 / 100));
        return {hard * 85 / 100, hard};
    }
    if (my_time_ms <= 0 && my_inc_ms <= 0) {
        return {1500, 2000};
    }

    const int mtg = std::max(24, 44 - game_ply / 6);
    const int avail = std::max(20, my_time_ms + byoyomi_ms - SAFETY_MS);
    int soft = my_time_ms / mtg + my_inc_ms * 3 / 4 + byoyomi_ms * 2 / 3;
    int hard = std::min(avail, std::max(soft * 3, my_time_ms / 8 + byoyomi_ms * 9 / 10));
    soft = std::clamp(soft, 20, hard);
    return {soft, hard};
}

int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    if (ply == 0) { reset_search_state(0, /*decay_hist=*/false); g_allow_old_gen_cutoff = false; }
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
        g_continuation_history_ordering_uses,
        g_singular_tt_candidates,
        g_singular_extension_attempts,
        g_singular_extensions,
        g_exclusion_searches
    };
}

Move iterative_deepening_timed(Board& board, TimeBudget budget,
                               const std::function<void(const SearchInfo&)>& info_cb,
                               const std::function<void()>& started_cb,
                               int max_depth_limit) {
    reset_search_state(budget.hard_ms, /*decay_hist=*/true);
    g_allow_old_gen_cutoff = true;
    if (started_cb) started_cb();

    MoveList root_moves_list;
    generate_legal_moves(board, root_moves_list);
    if (root_moves_list.empty()) return MOVE_NONE;

    const Color us = board.side_to_move();
    const bool root_in_check = board.in_check();

    Move tt_root = MOVE_NONE;
    if (TTEntry* e = tt_probe(board.hash())) tt_root = e->best_move;

    std::vector<ScoredMove> root_moves;
    root_moves.reserve(static_cast<size_t>(root_moves_list.size()));
    for (int i = 0; i < root_moves_list.size(); ++i) {
        Move m = root_moves_list[i];
        int sc = (m == tt_root) ? 20'000'000
               : is_tactical_move(board, m) ? (10'000'000 + score_tactical_move(board, m))
               : quiet_history_score(us, m);
        root_moves.push_back({m, sc});
    }
    std::sort(root_moves.begin(), root_moves.end(), [](const ScoredMove& a, const ScoredMove& b) {
        return a.score > b.score;
    });

    if (root_moves.size() == 1 && budget.soft_ms < budget.hard_ms && budget.hard_ms > 0) {
        budget.soft_ms = std::min(budget.soft_ms, 30);
        budget.hard_ms = std::min(budget.hard_ms, 80);
        g_hard_ms      = budget.hard_ms;
    }

    Move best_move      = root_moves[0].move;
    int  previous_score = 0;
    int  stable_count   = 0;
    const int target_max_depth = std::clamp(max_depth_limit, 1, MAX_DEPTH);

    for (int depth = 1; depth <= target_max_depth; ++depth) {
        if (time_up(budget.hard_ms)) break;

        int delta       = 35;
        int alpha_bound = (depth == 1) ? -INF : std::max(-INF, previous_score - delta);
        int beta_bound  = (depth == 1) ?  INF : std::min( INF, previous_score + delta);

        int  current_score = -INF;
        Move current_best  = MOVE_NONE;

        while (!g_stop.load(std::memory_order_relaxed)) {
            int alpha = alpha_bound;
            const int beta = beta_bound;
            current_score = -INF;
            current_best  = MOVE_NONE;
            g_pv_len[0]   = 0;

            for (size_t idx = 0; idx < root_moves.size(); ++idx) {
                const Move m = root_moves[idx].move;
                if (time_up(budget.hard_ms)) {
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }

                const bool gives_chk = move_gives_check(board, m);
                const int ext = (gives_chk && (root_in_check || see_ge(board, m, 0))) ? 1 : 0;
                g_move_ctx_stack[0] = move_ctx_key(board, us, m);
                board.do_move(m);

                int score;
                const int child_depth = depth - 1 + ext;
                if (idx == 0) {
                    score = -search(board, child_depth, -beta, -alpha, 1, false, MOVE_NONE, false);
                } else {
                    int r = 0;
                    if (!root_in_check && !gives_chk && depth >= 4 && idx >= 4 &&
                        !is_tactical_move(board, m)) {
                        r = 1 + (idx >= 12 ? 1 : 0);
                    }
                    score = -search(board, std::max(0, child_depth - r), -alpha - 1, -alpha,
                                    1, false, MOVE_NONE, true);
                    if (!g_stop.load(std::memory_order_relaxed) && r > 0 && score > alpha) {
                        score = -search(board, child_depth, -alpha - 1, -alpha,
                                        1, false, MOVE_NONE, true);
                    }
                    if (!g_stop.load(std::memory_order_relaxed) && score > alpha && score < beta) {
                        score = -search(board, child_depth, -beta, -alpha,
                                        1, false, MOVE_NONE, false);
                    }
                }

                board.undo_move(m);
                if (g_stop.load(std::memory_order_relaxed)) break;

                if (score > current_score) {
                    current_score = score;
                    current_best  = m;
                    g_pv[0][0]    = m;
                    const int clen = std::min<int>(g_pv_len[1], MAX_PLY);
                    for (int i = 1; i < clen; ++i) g_pv[0][i] = g_pv[1][i];
                    g_pv_len[0] = std::max(1, clen);

                    if (idx > 0) {
                        ScoredMove promoted = root_moves[idx];
                        for (size_t k = idx; k > 0; --k) root_moves[k] = root_moves[k - 1];
                        root_moves[0] = promoted;
                    }
                }

                if (current_score > alpha) {
                    alpha = current_score;
                    if (alpha >= beta) break;
                }
            }

            if (g_stop.load(std::memory_order_relaxed) || current_best == MOVE_NONE)
                break;

            if (current_score <= alpha_bound && alpha_bound > -INF) {
                ++g_aspiration_fail_low;
                beta_bound  = (alpha_bound + beta_bound) / 2;
                delta       = std::min(INF, delta * 2 + 20);
                alpha_bound = (delta >= 4000) ? -INF : std::max(-INF, current_score - delta);
                continue;
            }
            if (current_score >= beta_bound && beta_bound < INF) {
                ++g_aspiration_fail_high;
                delta      = std::min(INF, delta * 2 + 20);
                beta_bound = (delta >= 4000) ? INF : std::min(INF, current_score + delta);
                continue;
            }
            break;
        }

        if (g_stop.load(std::memory_order_relaxed) || current_best == MOVE_NONE) break;

        if (current_best == best_move) ++stable_count;
        else                           stable_count = 0;

        best_move = current_best;
        const int score_drop = previous_score - current_score;
        previous_score = current_score;

        tt_store(board.hash(), depth, current_score, BOUND_EXACT, best_move, 0, EVAL_NONE);

        if (info_cb) {
            SearchInfo info;
            info.depth    = depth;
            info.seldepth = std::max(depth, g_seldepth);
            info.time_ms  = std::max(1, elapsed_ms());
            info.nodes    = g_nodes + g_qnodes;
            info.nps      = static_cast<uint64_t>((info.nodes * 1000ULL) / static_cast<uint64_t>(info.time_ms));
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

        if (budget.soft_ms > 0 && budget.soft_ms < budget.hard_ms && depth >= 5) {
            double factor = 1.0;
            if (stable_count >= 4)      factor *= 0.65;
            else if (stable_count >= 2) factor *= 0.80;
            else if (stable_count == 0) factor *= 1.35;
            if (score_drop >= 40)       factor *= 1.40;
            const int adjusted_soft = std::min(budget.hard_ms,
                                               static_cast<int>(budget.soft_ms * factor));
            if (elapsed_ms() >= adjusted_soft) break;
        }
    }

    return best_move;
}

Move iterative_deepening(Board& board, int allotted_ms,
                         const std::function<void(const SearchInfo&)>& info_cb,
                         const std::function<void()>& started_cb,
                         int max_depth_limit) {
    return iterative_deepening_timed(board, TimeBudget{allotted_ms, allotted_ms},
                                     info_cb, started_cb, max_depth_limit);
}
