#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <climits>

std::atomic<bool> g_stop{false};

static std::chrono::steady_clock::time_point g_start;
static int      g_allotted_ms    = 0;
static uint64_t g_nodes          = 0;
// Check time every (TIME_CHECK_MASK+1) nodes; initial value forces first-call check.
static constexpr uint64_t TIME_CHECK_MASK = 0xFFF;

void search_init_time() {
    g_start = std::chrono::steady_clock::now();
}

bool time_up(int allotted_ms) {
    if (g_stop.load(std::memory_order_relaxed)) return true;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_start).count();
    return elapsed >= allotted_ms;
}

int compute_allotted_ms(int my_time_ms, int byoyomi_ms) {
    // Use ~1/40 of remaining clock plus half the byoyomi,
    // capped at 5 seconds to avoid time-outs in normal games.
    int v = my_time_ms / 40 + byoyomi_ms / 2;
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    return v;
}

// ============================================================
// Move ordering helper  –  captures first, then by piece value
// ============================================================
static int move_score_order(const Board& b, Move m) {
    if (is_drop(m)) return 0;
    Piece cap = b.piece_at(to_sq(m));
    if (cap != NO_PIECE) {
        // MVV-LVA: most valuable victim, least valuable aggressor
        static const int VAL[PT_NB] = {0,100,300,300,500,600,800,1000,0,600,600,600,600,1100,1300};
        return VAL[type_of(cap)] - VAL[type_of(b.piece_at(from_sq(m)))] / 10;
    }
    if (is_promote(m)) return 50;
    return 0;
}

// ============================================================
// negamax alpha-beta
// ============================================================
int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    // Check time every 4096 nodes and set g_stop when budget is exhausted
    if ((++g_nodes & TIME_CHECK_MASK) == 0) {
        if (time_up(g_allotted_ms)) {
            g_stop.store(true, std::memory_order_relaxed);
        }
    }
    if (g_stop.load(std::memory_order_relaxed)) return 0;

    // Repetition draw
    if (board.repetition_count() >= 4) return 0;

    // Leaf node
    if (depth == 0) return evaluate(board);

    MoveList moves;
    generate_legal_moves(board, moves);

    if (moves.empty()) {
        // No legal moves = lose (checkmate or stalemate, both count as loss in shogi)
        return -(MATE_VALUE - ply);
    }

    // Sort moves: captures and promotions first
    std::sort(moves.begin(), moves.end(),
              [&](Move lhs, Move rhs) { return move_score_order(board, lhs) > move_score_order(board, rhs); });

    int best = -INF;
    for (Move m : moves) {
        if (g_stop.load(std::memory_order_relaxed)) break;

        board.do_move(m);
        int score = -negamax(board, depth - 1, -beta, -alpha, ply + 1);
        board.undo_move(m);

        if (score > best) best = score;
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;  // beta cut-off
    }

    return best;
}

// ============================================================
// iterative_deepening
// ============================================================
Move iterative_deepening(Board& board, int allotted_ms) {
    search_init_time();
    g_stop.store(false);
    g_allotted_ms = allotted_ms;
    g_nodes = TIME_CHECK_MASK;  // first negamax call triggers an immediate time check

    Move best_move = MOVE_NONE;

    // Fallback: pick the first legal move
    {
        MoveList ml;
        generate_legal_moves(board, ml);
        if (ml.empty()) return MOVE_NONE;
        best_move = ml.moves_[0];
    }

    for (int depth = 1; depth <= MAX_DEPTH; depth++) {
        if (time_up(allotted_ms)) break;

        // Root search
        int alpha = -INF, beta = INF;
        Move  current_best = MOVE_NONE;
        int   current_score = -INF;

        MoveList moves;
        generate_legal_moves(board, moves);
        if (moves.empty()) break;

        std::sort(moves.begin(), moves.end(),
                  [&](Move lhs, Move rhs) { return move_score_order(board, lhs) > move_score_order(board, rhs); });

        for (Move m : moves) {
            if (time_up(allotted_ms)) { g_stop.store(true); break; }

            board.do_move(m);
            int score = -negamax(board, depth - 1, -beta, -alpha, 1);
            board.undo_move(m);

            if (score > current_score) {
                current_score = score;
                current_best  = m;
            }
            if (score > alpha) alpha = score;
        }

        if (!g_stop.load() && current_best != MOVE_NONE) {
            best_move = current_best;
        }

        // Mate found – no need to search deeper
        if (current_score >= MATE_VALUE - MAX_DEPTH) break;
    }

    return best_move;
}
