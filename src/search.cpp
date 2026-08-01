#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <array>
#include <climits>

std::atomic<bool> g_stop{false};

namespace {

std::chrono::steady_clock::time_point g_start;
int      g_allotted_ms         = 0;
uint64_t g_nodes               = 0;
uint64_t g_beta_cutoffs        = 0;
uint64_t g_threshold_cutoffs   = 0;
int      g_seldepth            = 0;
Color    g_root_side           = BLACK;

// Check time every (TIME_CHECK_MASK+1) nodes; initial value forces first-call check.
constexpr uint64_t TIME_CHECK_MASK = 0xFFF;

std::array<std::array<Move, MAX_DEPTH>, MAX_DEPTH> g_pv{};
std::array<int, MAX_DEPTH> g_pv_len{};

inline int eval_root_perspective(const Board& board) {
    int e = evaluate(board); // side-to-move perspective
    return (board.side_to_move() == g_root_side) ? e : -e;
}

inline int elapsed_ms() {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_start).count());
}

} // namespace

void search_init_time() {
    g_start = std::chrono::steady_clock::now();
}

bool time_up(int allotted_ms) {
    if (g_stop.load(std::memory_order_relaxed)) return true;
    return elapsed_ms() >= allotted_ms;
}

int compute_allotted_ms(int my_time_ms, int byoyomi_ms) {
    int v = my_time_ms / 40 + byoyomi_ms / 2;
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    return v;
}

static int move_score_order(const Board& b, Move m) {
    if (is_drop(m)) return 0;
    Piece cap = b.piece_at(to_sq(m));
    if (cap != NO_PIECE) {
        static const int VAL[PT_NB] = {0,100,300,300,500,600,800,1000,0,600,600,600,600,1100,1300};
        return VAL[type_of(cap)] - VAL[type_of(b.piece_at(from_sq(m)))] / 10;
    }
    if (is_promote(m)) return 50;
    return 0;
}

static int alpha_beta(Board& board, int depth, int alpha, int beta, int ply, bool maximizing) {
    if (((++g_nodes) & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) g_stop.store(true, std::memory_order_relaxed);
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    g_seldepth = std::max(g_seldepth, ply);
    g_pv_len[ply] = ply;

    if (board.repetition_count() >= 4) return 0;

    // Evaluate every node (required for threshold-based pruning)
    const int static_eval = eval_root_perspective(board);
    if (maximizing && static_eval <= -PRUNE_LOSS_THRESHOLD_CP) {
        ++g_threshold_cutoffs;
        return -INF;
    }
    if (!maximizing && static_eval >= PRUNE_WIN_THRESHOLD_CP) {
        ++g_threshold_cutoffs;
        return INF;
    }

    if (depth == 0) return static_eval;

    MoveList moves;
    generate_legal_moves(board, moves);
    if (moves.empty()) {
        return maximizing ? -(MATE_VALUE - ply) : (MATE_VALUE - ply);
    }

    std::sort(moves.begin(), moves.end(),
              [&](Move lhs, Move rhs) { return move_score_order(board, lhs) > move_score_order(board, rhs); });

    if (maximizing) {
        int best = -INF;
        for (Move m : moves) {
            if (g_stop.load(std::memory_order_relaxed)) break;
            board.do_move(m);
            int score = alpha_beta(board, depth - 1, alpha, beta, ply + 1, false);
            board.undo_move(m);

            if (score > best) {
                best = score;
                g_pv[ply][ply] = m;
                for (int i = ply + 1; i < g_pv_len[ply + 1]; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
                g_pv_len[ply] = g_pv_len[ply + 1];
            }
            alpha = std::max(alpha, best);
            if (alpha >= beta) {
                ++g_beta_cutoffs;
                break;
            }
        }
        return best;
    }

    int best = INF;
    for (Move m : moves) {
        if (g_stop.load(std::memory_order_relaxed)) break;
        board.do_move(m);
        int score = alpha_beta(board, depth - 1, alpha, beta, ply + 1, true);
        board.undo_move(m);

        if (score < best) {
            best = score;
            g_pv[ply][ply] = m;
            for (int i = ply + 1; i < g_pv_len[ply + 1]; ++i) g_pv[ply][i] = g_pv[ply + 1][i];
            g_pv_len[ply] = g_pv_len[ply + 1];
        }
        beta = std::min(beta, best);
        if (alpha >= beta) {
            ++g_beta_cutoffs;
            break;
        }
    }
    return best;
}

int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    if (ply == 0) g_root_side = board.side_to_move();
    return alpha_beta(board, depth, alpha, beta, ply, (ply % 2) == 0);
}

SearchStats last_search_stats() {
    return SearchStats{g_nodes, g_beta_cutoffs, g_threshold_cutoffs};
}

Move iterative_deepening(Board& board, int allotted_ms, const std::function<void(const SearchInfo&)>& info_cb) {
    search_init_time();
    g_stop.store(false, std::memory_order_relaxed);
    g_allotted_ms = allotted_ms;
    g_nodes = 0;
    g_beta_cutoffs = 0;
    g_threshold_cutoffs = 0;
    g_seldepth = 0;
    g_root_side = board.side_to_move();
    g_pv_len.fill(0);

    Move best_move = MOVE_NONE;
    MoveList root_moves;
    generate_legal_moves(board, root_moves);
    if (root_moves.empty()) return MOVE_NONE;
    best_move = root_moves.moves_[0];

    for (int depth = 1; depth <= MAX_DEPTH; ++depth) {
        if (time_up(allotted_ms)) break;

        MoveList moves;
        generate_legal_moves(board, moves);
        if (moves.empty()) break;
        std::sort(moves.begin(), moves.end(),
                  [&](Move lhs, Move rhs) { return move_score_order(board, lhs) > move_score_order(board, rhs); });

        int alpha = -INF;
        int beta = INF;
        int current_score = -INF;
        Move current_best = MOVE_NONE;

        for (Move m : moves) {
            if (time_up(allotted_ms)) { g_stop.store(true, std::memory_order_relaxed); break; }
            board.do_move(m);
            int score = alpha_beta(board, depth - 1, alpha, beta, 1, false);
            board.undo_move(m);

            if (score > current_score) {
                current_score = score;
                current_best = m;
                g_pv[0][0] = m;
                for (int i = 1; i < g_pv_len[1]; ++i) g_pv[0][i] = g_pv[1][i];
                g_pv_len[0] = g_pv_len[1];
            }
            alpha = std::max(alpha, score);
        }

        if (!g_stop.load(std::memory_order_relaxed) && current_best != MOVE_NONE) best_move = current_best;

        if (info_cb && current_best != MOVE_NONE) {
            SearchInfo info;
            info.depth = depth;
            info.seldepth = std::max(depth, g_seldepth);
            info.time_ms = std::max(1, elapsed_ms());
            info.nodes = g_nodes;
            info.nps = static_cast<uint64_t>((info.nodes * 1000ULL) / static_cast<uint64_t>(info.time_ms));
            info.score_cp = current_score;
            if (std::abs(current_score) >= MATE_VALUE - MAX_DEPTH) {
                info.score_is_mate = true;
                const int plies_to_mate = MATE_VALUE - std::abs(current_score);
                info.score_mate = (current_score > 0 ? 1 : -1) * std::max(1, (plies_to_mate + 1) / 2);
            }
            for (int i = 0; i < g_pv_len[0] && i < MAX_DEPTH; ++i) info.pv.push_back(g_pv[0][i]);
            if (info.pv.empty()) info.pv.push_back(current_best);
            info_cb(info);
        }

        if (current_score >= MATE_VALUE - MAX_DEPTH) break;
    }

    return best_move;
}
