#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <array>
#include <climits>
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
int      g_seldepth          = 0;

std::array<std::array<Move, MAX_DEPTH>, MAX_DEPTH> g_pv{};
std::array<int, MAX_DEPTH> g_pv_len{};
std::array<std::array<Move, 2>, MAX_DEPTH> g_killers{};
std::array<std::array<int, SQUARE_NB>, SQUARE_NB> g_history{};
std::array<std::array<int, SQUARE_NB>, PT_NB> g_drop_history{};
std::array<TTEntry, TT_SIZE> g_tt{};
uint16_t g_tt_generation = 1;

constexpr std::array<int, PT_NB> PIECE_VALUE = {
    0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
};

inline int elapsed_ms() {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_start).count());
}

inline bool is_mate_score(int score) {
    return std::abs(score) >= MATE_SCORE_THRESHOLD;
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
    if (entry.generation == g_tt_generation && entry.key == key) {
        ++g_tt_hits;
        return &entry;
    }
    return nullptr;
}

void tt_store(uint64_t key, int depth, int score, BoundType bound, Move best_move, int ply) {
    TTEntry& entry = tt_slot(key);
    if (entry.generation == g_tt_generation && entry.key == key &&
        entry.depth > depth && entry.bound == BOUND_EXACT && bound != BOUND_EXACT) {
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

void reset_search_state(int allotted_ms) {
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
    g_seldepth = 0;
    g_pv_len.fill(0);
    for (auto& killers : g_killers) killers = {MOVE_NONE, MOVE_NONE};
    for (auto& hist : g_history) hist.fill(0);
    for (auto& hist : g_drop_history) hist.fill(0);
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

void record_killer(int ply, Move m) {
    if (ply < 0 || ply >= MAX_DEPTH) return;
    if (g_killers[ply][0] == m) return;
    g_killers[ply][1] = g_killers[ply][0];
    g_killers[ply][0] = m;
}

void record_history(Move m, int depth) {
    const int bonus = std::min(2048, depth * depth + depth * 4);
    if (is_drop(m)) {
        int& v = g_drop_history[dropped_pt(m)][to_sq(m)];
        v = std::min(1'000'000, v + bonus);
    } else {
        int& v = g_history[from_sq(m)][to_sq(m)];
        v = std::min(1'000'000, v + bonus);
    }
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

int move_score(const Board& board, Move m, int ply, Move hash_move) {
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

    return quiet_history_score(m);
}

std::vector<ScoredMove> order_moves(const Board& board, const MoveList& moves, int ply, Move hash_move, bool tactical_only) {
    std::vector<ScoredMove> ordered;
    ordered.reserve(static_cast<size_t>(moves.size()));
    for (Move m : moves) {
        if (tactical_only && !is_tactical_move(board, m)) continue;
        ordered.push_back({m, move_score(board, m, ply, hash_move)});
    }
    std::sort(ordered.begin(), ordered.end(), [](const ScoredMove& lhs, const ScoredMove& rhs) {
        return lhs.score > rhs.score;
    });
    return ordered;
}

int quiescence(Board& board, int alpha, int beta, int ply);

int search(Board& board, int depth, int alpha, int beta, int ply) {
    if (((++g_nodes) & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_DEPTH) g_pv_len[ply] = ply;

    if (board.repetition_count() >= 4) return 0;

    if (depth <= 0) return quiescence(board, alpha, beta, ply);

    const int original_alpha = alpha;
    const int original_beta = beta;
    Move hash_move = MOVE_NONE;

    if (TTEntry* entry = tt_probe(board.hash())) {
        hash_move = entry->best_move;
        if (entry->depth >= depth) {
            const int tt_score = tt_probe_score(entry->score, ply);
            if (entry->bound == BOUND_EXACT) return tt_score;
            if (entry->bound == BOUND_LOWER) alpha = std::max(alpha, tt_score);
            else if (entry->bound == BOUND_UPPER) beta = std::min(beta, tt_score);
            if (alpha >= beta) {
                ++g_tt_cutoffs;
                return tt_score;
            }
        }
    }

    MoveList legal_moves;
    generate_legal_moves(board, legal_moves);
    if (legal_moves.empty()) {
        return board.in_check() ? -(MATE_VALUE - ply) : 0;
    }

    auto ordered = order_moves(board, legal_moves, ply, hash_move, false);
    int best_score = -INF;
    Move best_move = MOVE_NONE;

    for (const ScoredMove& scored : ordered) {
        const Move m = scored.move;
        board.do_move(m);
        const int score = -search(board, depth - 1, -beta, -alpha, ply + 1);
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
            if (!is_tactical_move(board, m)) {
                record_killer(ply, m);
                record_history(m, depth);
            }
            break;
        }
    }

    if (g_stop.load(std::memory_order_relaxed)) return (best_move == MOVE_NONE) ? 0 : best_score;

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha) bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), depth, best_score, bound, best_move, ply);
    return best_score;
}

int quiescence(Board& board, int alpha, int beta, int ply) {
    ++g_qnodes;
    if ((g_qnodes & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    if (ply < MAX_DEPTH) g_pv_len[ply] = ply;

    if (board.repetition_count() >= 4) return 0;

    const bool in_check = board.in_check();
    const int original_alpha = alpha;
    const int original_beta = beta;
    Move hash_move = MOVE_NONE;

    if (TTEntry* entry = tt_probe(board.hash())) {
        hash_move = entry->best_move;
        if (entry->depth >= 0) {
            const int tt_score = tt_probe_score(entry->score, ply);
            if (entry->bound == BOUND_EXACT) return tt_score;
            if (entry->bound == BOUND_LOWER) alpha = std::max(alpha, tt_score);
            else if (entry->bound == BOUND_UPPER) beta = std::min(beta, tt_score);
            if (alpha >= beta) {
                ++g_tt_cutoffs;
                return tt_score;
            }
        }
    }

    int stand_pat = evaluate(board);
    if (!in_check) {
        if (stand_pat >= beta) {
            tt_store(board.hash(), 0, stand_pat, BOUND_LOWER, MOVE_NONE, ply);
            return stand_pat;
        }
        alpha = std::max(alpha, stand_pat);
    }

    MoveList legal_moves;
    generate_legal_moves(board, legal_moves);
    if (legal_moves.empty()) return in_check ? -(MATE_VALUE - ply) : stand_pat;

    auto ordered = order_moves(board, legal_moves, ply, hash_move, !in_check);
    if (ordered.empty()) return stand_pat;

    int best_score = in_check ? -INF : stand_pat;
    Move best_move = MOVE_NONE;

    for (const ScoredMove& scored : ordered) {
        const Move m = scored.move;
        board.do_move(m);
        const int score = -quiescence(board, -beta, -alpha, ply + 1);
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

    if (g_stop.load(std::memory_order_relaxed)) return (best_move == MOVE_NONE) ? stand_pat : best_score;

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha) bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), 0, best_score, bound, best_move, ply);
    return best_score;
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
        g_tt_cutoffs
    };
}

Move iterative_deepening(Board& board, int allotted_ms, const std::function<void(const SearchInfo&)>& info_cb) {
    reset_search_state(allotted_ms);

    MoveList root_moves;
    generate_legal_moves(board, root_moves);
    if (root_moves.empty()) return MOVE_NONE;

    Move best_move = root_moves.moves_[0];
    for (int depth = 1; depth <= MAX_DEPTH; ++depth) {
        if (time_up(allotted_ms)) break;

        MoveList moves;
        generate_legal_moves(board, moves);
        if (moves.empty()) break;

        auto ordered = order_moves(board, moves, 0, best_move, false);
        int alpha = -INF;
        const int beta = INF;
        int current_score = -INF;
        Move current_best = MOVE_NONE;

        for (const ScoredMove& scored : ordered) {
            const Move m = scored.move;
            if (time_up(allotted_ms)) {
                g_stop.store(true, std::memory_order_relaxed);
                break;
            }

            board.do_move(m);
            const int score = -search(board, depth - 1, -beta, -alpha, 1);
            board.undo_move(m);

            if (g_stop.load(std::memory_order_relaxed)) break;

            if (score > current_score) {
                current_score = score;
                current_best = m;
                g_pv[0][0] = m;
                for (int i = 1; i < g_pv_len[1]; ++i) g_pv[0][i] = g_pv[1][i];
                g_pv_len[0] = g_pv_len[1];
            }
            alpha = std::max(alpha, current_score);
        }

        if (!g_stop.load(std::memory_order_relaxed) && current_best != MOVE_NONE) best_move = current_best;

        if (info_cb && current_best != MOVE_NONE) {
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
