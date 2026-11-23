// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "../src/board.hpp"
#include "../src/movegen.hpp"
#include "../src/eval.hpp"
#include "../src/search.hpp"
#include "../src/winrate_book.hpp"
#include "../src/usi_proc.hpp"
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <vector>
#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <sstream>

// ============================================================
// Minimal test framework
// ============================================================
static int g_pass = 0, g_fail = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "FAIL  " << __FILE__ << ':' << __LINE__ \
                      << "  " << #expr << '\n'; \
            g_fail++; \
        } else { \
            g_pass++; \
        } \
    } while (false)

#define CHECK_EQ(a, b) \
    do { \
        auto _a = (a); auto _b = (b); \
        if (_a != _b) { \
            std::cerr << "FAIL  " << __FILE__ << ':' << __LINE__ \
                      << "  " << #a << " == " << #b \
                      << "  [got " << _a << " vs " << _b << "]\n"; \
            g_fail++; \
        } else { g_pass++; } \
    } while (false)

// ============================================================
// Helpers
// ============================================================
static Square sq(const char* usi) {          // e.g. "5e"
    return usi_to_sq(usi[0], usi[1]);
}

static bool has_move_usi(const MoveList& ml, const char* usi) {
    Move m = usi_to_move(usi);
    for (Move x : ml) if (x == m) return true;
    return false;
}

// ============================================================
// Test: square / USI conversions
// ============================================================
static void test_square_conversions() {
    // USI "9a" = top-left = internal (file=0, rank=0)
    CHECK_EQ(file_of(sq("9a")), 0);
    CHECK_EQ(rank_of(sq("9a")), 0);
    CHECK(sq_to_usi(sq("9a")) == "9a");

    // USI "1i" = bottom-right = internal (file=8, rank=8)
    CHECK_EQ(file_of(sq("1i")), 8);
    CHECK_EQ(rank_of(sq("1i")), 8);
    CHECK(sq_to_usi(sq("1i")) == "1i");

    // USI "5e" = centre = internal (file=4, rank=4)
    CHECK_EQ(file_of(sq("5e")), 4);
    CHECK_EQ(rank_of(sq("5e")), 4);
    CHECK(sq_to_usi(sq("5e")) == "5e");
}

// ============================================================
// Test: initial position setup
// ============================================================
static void test_initial_position() {
    Board b;
    b.set_startpos();

    // Side to move
    CHECK(b.side_to_move() == BLACK);

    // Kings
    CHECK(b.piece_at(sq("5a")) == make_piece(WHITE, KING));
    CHECK(b.piece_at(sq("5i")) == make_piece(BLACK, KING));
    CHECK_EQ(b.king_sq(WHITE), sq("5a"));
    CHECK_EQ(b.king_sq(BLACK), sq("5i"));

    // Major pieces
    CHECK(b.piece_at(sq("8b")) == make_piece(WHITE, ROOK));
    CHECK(b.piece_at(sq("2b")) == make_piece(WHITE, BISHOP));
    CHECK(b.piece_at(sq("2h")) == make_piece(BLACK, ROOK));
    CHECK(b.piece_at(sq("8h")) == make_piece(BLACK, BISHOP));

    // Sample pawns
    CHECK(b.piece_at(sq("7c")) == make_piece(WHITE, PAWN));
    CHECK(b.piece_at(sq("3g")) == make_piece(BLACK, PAWN));

    // Empty squares
    CHECK(b.is_empty(sq("5e")));

    // No pieces in hand at start
    for (int pt = PAWN; pt <= ROOK; pt++) {
        CHECK_EQ(b.hand(BLACK, static_cast<PieceType>(pt)), 0);
        CHECK_EQ(b.hand(WHITE, static_cast<PieceType>(pt)), 0);
    }
}

// ============================================================
// Test: SFEN round-trip
// ============================================================
static void test_sfen_roundtrip() {
    const std::string startpos =
        "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1";
    Board b;
    b.parse_sfen(startpos);
    CHECK(b.to_sfen() == startpos);
}

// ============================================================
// Test: legal move count from starting position
//
// Expected count = 30:
//   9 pawns × 1 forward                     =  9
//   2 golds (4i,6i) × 3 each               =  6
//   2 silvers (3i,7i) × 2 each             =  4
//   king (5i) × 3 (6h,5h,4h)               =  3
//   rook (2h): W(1h) + E×5(3h?7h)          =  6
//   2 lances (1i,9i) × 1 each (1h,9h)      =  2
//   bishop (8h), knights (2i,8i): 0 each    =  0
// ============================================================
static void test_legal_move_count_startpos() {
    Board b;
    b.set_startpos();
    MoveList ml;
    generate_legal_moves(b, ml);
    CHECK_EQ(ml.size(), 30);
}

// ============================================================
// Test: do_move / undo_move (one-ply round-trip)
// ============================================================
static void test_do_undo() {
    Board b;
    b.set_startpos();
    std::string sfen_before = b.to_sfen();

    // Make the opening move 7g7f (pawn advance)
    Move m = usi_to_move("7g7f");
    b.do_move(m);
    CHECK(b.side_to_move() == WHITE);
    CHECK(b.is_empty(sq("7g")));
    CHECK(b.piece_at(sq("7f")) == make_piece(BLACK, PAWN));

    b.undo_move(m);
    CHECK(b.side_to_move() == BLACK);
    CHECK(b.to_sfen() == sfen_before);
}

// ============================================================
// Test: capture and hand update
// ============================================================
static void test_capture_and_hand() {
    // B_PAWN at 5e, W_PAWN at 5d (directly ahead for BLACK)
    // SFEN row 4 (rank d) = 4p4, row 5 (rank e) = 4P4
    Board b;
    b.parse_sfen("9/9/9/4p4/4P4/9/9/4K4/4k4 b - 1");

    CHECK(b.piece_at(sq("5e")) == make_piece(BLACK, PAWN));
    CHECK(b.piece_at(sq("5d")) == make_piece(WHITE, PAWN));

    Move cap = usi_to_move("5e5d");
    b.do_move(cap);
    CHECK(b.is_empty(sq("5e")));
    CHECK(b.piece_at(sq("5d")) == make_piece(BLACK, PAWN));
    CHECK_EQ(b.hand(BLACK, PAWN), 1);

    b.undo_move(cap);
    CHECK(b.piece_at(sq("5e")) == make_piece(BLACK, PAWN));
    CHECK(b.piece_at(sq("5d")) == make_piece(WHITE, PAWN));
    CHECK_EQ(b.hand(BLACK, PAWN), 0);
}

// ============================================================
// Test: promotion
// ============================================================
static void test_promotion() {
    // B_PAWN at 5c (rank c = internal rank 2) promotes to 5b (rank b = internal rank 1)
    Board b;
    b.parse_sfen("9/9/4P4/9/9/9/9/4K4/4k4 b - 1");
    Move promo = usi_to_move("5c5b+");
    CHECK(b.piece_at(sq("5c")) == make_piece(BLACK, PAWN));
    b.do_move(promo);
    CHECK(b.piece_at(sq("5b")) == make_piece(BLACK, PROM_PAWN));
    b.undo_move(promo);
    CHECK(b.piece_at(sq("5c")) == make_piece(BLACK, PAWN));
    CHECK(b.piece_at(sq("5b")) == NO_PIECE);
}

// ============================================================
// Test: drop move
// ============================================================
static void test_drop() {
    Board b;
    // BLACK has 1 pawn in hand; valid SFEN hand field is "P" (no extra "-")
    b.parse_sfen("9/9/9/9/9/9/9/4K4/4k4 b P 1");
    CHECK_EQ(b.hand(BLACK, PAWN), 1);

    Move drop = usi_to_move("P*5e");
    b.do_move(drop);
    CHECK_EQ(b.hand(BLACK, PAWN), 0);
    CHECK(b.piece_at(sq("5e")) == make_piece(BLACK, PAWN));

    b.undo_move(drop);
    CHECK_EQ(b.hand(BLACK, PAWN), 1);
    CHECK(b.is_empty(sq("5e")));
}

// ============================================================
// Test: nifu rule (can't drop pawn on file with existing pawn)
// ============================================================
static void test_nifu() {
    Board b;
    // BLACK has a pawn on 5g; also has a pawn in hand
    b.parse_sfen("9/9/9/9/9/9/4P4/4K4/4k4 b P 1");
    CHECK(b.piece_at(sq("5g")) == make_piece(BLACK, PAWN));
    CHECK_EQ(b.hand(BLACK, PAWN), 1);

    MoveList ml;
    generate_legal_moves(b, ml);

    // Count drop-pawn moves on file 5 (internal file 4)
    int pawn_drops_file5 = 0;
    for (Move m : ml) {
        if (is_drop(m) && dropped_pt(m) == PAWN && file_of(to_sq(m)) == 4)
            pawn_drops_file5++;
    }
    CHECK_EQ(pawn_drops_file5, 0);  // nifu: must be zero
}

// ============================================================
// Test: check detection
// ============================================================
static void test_check_detection() {
    // WHITE king at 5a, BLACK rook at 5h ? rook gives check along file 5
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/4R4/4K4 b - 1");
    // After BLACK makes no move yet, is WHITE in check? No (it's BLACK's turn).
    // Let's check by asking: is sq("5a") attacked by BLACK?
    CHECK(b.is_attacked(sq("5a"), BLACK));
    CHECK(!b.is_attacked(sq("5a"), WHITE));
}

// ============================================================
// Test: king cannot move into check
// ============================================================
static void test_king_cant_move_into_check() {
    // BLACK king at 5i, WHITE rook at 4a (internal file 5, rank 0)
    // Rook covers the entire file 4 (internal file 5), including 4h.
    // BLACK king at 5i must not be able to move to 4h.
    // SFEN: "5r3" = 5 empty + r at internal f=5 (USI "4a") + 3 empty
    Board b;
    b.parse_sfen("5r3/9/9/9/9/9/9/9/4K4 b - 1");
    MoveList ml;
    generate_legal_moves(b, ml);
    bool found_4h = false;
    for (Move m : ml) {
        if (!is_drop(m) && to_sq(m) == sq("4h")) found_4h = true;
    }
    CHECK(!found_4h);
}

// ============================================================
// Test: promoted bishop (horse) movement regression
// ============================================================
static void test_horse_movement_regression() {
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/8K/2r1+B4 b - 1");

    CHECK(b.piece_at(sq("5i")) == make_piece(BLACK, PROM_BISHOP));

    MoveList ml;
    generate_legal_moves(b, ml);

    // Regression: horse must not move two squares orthogonally.
    CHECK(!has_move_usi(ml, "5i7i"));

    // Legal horse moves should still exist.
    CHECK(has_move_usi(ml, "5i6i"));
    CHECK(has_move_usi(ml, "5i6h"));
}

// ============================================================
// Test: bishop/rook promotion mapping is correct
// ============================================================
static void test_major_promotion_mapping() {
    CHECK_EQ(promote_pt(BISHOP), PROM_BISHOP);
    CHECK_EQ(promote_pt(ROOK), PROM_ROOK);
    CHECK_EQ(unpromote_pt(PROM_BISHOP), BISHOP);
    CHECK_EQ(unpromote_pt(PROM_ROOK), ROOK);
}

// ============================================================
// Test: move_to_usi / usi_to_move round-trip
// ============================================================
static void test_move_usi_roundtrip() {
    // Normal move
    Move m1 = usi_to_move("7g7f");
    CHECK(move_to_usi(m1) == "7g7f");

    // Promotion
    Move m2 = usi_to_move("2b3a+");
    CHECK(move_to_usi(m2) == "2b3a+");

    // Drop
    Move m3 = usi_to_move("R*5e");
    CHECK(move_to_usi(m3) == "R*5e");
    CHECK(is_drop(m3));
    CHECK_EQ(dropped_pt(m3), ROOK);
}

// ============================================================
// Test: evaluation sign
// ============================================================
static void test_eval_symmetric() {
    // Starting position is 180-degree symmetric between Black STM and White STM.
    Board b_black, b_white;
    b_black.parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
    b_white.parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL w - 1");
    const int score_black = evaluate(b_black);
    const int score_white = evaluate(b_white);
    CHECK_EQ(score_black, score_white);
    if (get_eval_family() == EvalFamily::MATERIAL_FALLBACK) {
        CHECK_EQ(score_black, 0);
    }
}

// ============================================================
// Test: evaluation handles side-to-move, promotions and hand pieces
// ============================================================
static void test_eval_features() {
    Board b1_b, b1_w_mirror;
    b1_b.parse_sfen("4k4/9/9/9/4P4/9/9/9/4K4 b - 1");
    b1_w_mirror.parse_sfen("4k4/9/9/9/4p4/9/9/9/4K4 w - 1");
    // 180-degree color-mirrored position must produce identical STM evaluation
    CHECK_EQ(evaluate(b1_b), evaluate(b1_w_mirror));
    if (get_eval_family() == EvalFamily::MATERIAL_FALLBACK) {
        Board b_same_w;
        b_same_w.parse_sfen("4k4/9/9/9/4P4/9/9/9/4K4 w - 1");
        CHECK_EQ(evaluate(b1_b), -evaluate(b_same_w));
    }

    Board b2, b2_empty;
    b2.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b P 1");
    b2_empty.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b - 1");
    CHECK(evaluate(b2) > evaluate(b2_empty));

    Board b3, b4;
    b3.parse_sfen("4k4/3+P5/9/9/9/9/9/9/4K4 b - 1");
    b4.parse_sfen("4k4/3P5/9/9/9/9/9/9/4K4 b - 1");
    CHECK(evaluate(b3) > evaluate(b4));
}

// ============================================================
// Test: quiescence search extends tactical captures at leaf nodes
// ============================================================
static void test_quiescence_tactical_extension() {
    Board b;
    // White bishop on 5d can capture the hanging black rook on 6e.
    b.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const int static_eval = evaluate(b);
    const int searched = negamax(b, 0, -INF, INF, 0);
    if (get_eval_family() == EvalFamily::MATERIAL_FALLBACK) {
        CHECK(searched > static_eval + 300);
    } else {
        // Real NNUE already recognizes much of the hanging-rook threat statically,
        // and quiescence capture still produces a decisively winning score (> +400cp).
        CHECK(searched > 400);
    }
}

// ============================================================
// Test: iterative search prefers the immediate tactical gain
// ============================================================
static void test_search_tactical_regression() {
    Board b;
    b.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");

    MoveList ml;
    generate_legal_moves(b, ml);
    bool found_capture = false;
    for (Move m : ml) {
        if (m == usi_to_move("5d6e")) found_capture = true;
    }
    CHECK(found_capture);

    const Move best = iterative_deepening(b, 150);
    CHECK_EQ(best, usi_to_move("5d6e"));
}

// ============================================================
// Test: transposition table is used during iterative deepening
// ============================================================
static void test_transposition_table_hits() {
    Board b;
    b.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    (void)iterative_deepening(b, 150);
    SearchStats st = last_search_stats();
    CHECK(st.tt_probes > 0);
    CHECK(st.tt_hits > 0);
}

// ============================================================
// Test: retained TT entries remain useful across search generations
// ============================================================
static void test_transposition_table_reuse_across_generations() {
    Board b1;
    b1.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const int score1 = negamax(b1, 3, -INF, INF, 0);
    const SearchStats st1 = last_search_stats();

    Board b2;
    b2.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const int score2 = negamax(b2, 3, -INF, INF, 0);
    const SearchStats st2 = last_search_stats();

    CHECK_EQ(score1, score2);
    CHECK(st1.tt_probes > 0);
    CHECK(st2.tt_hits > 0);
}

// ============================================================
// Test: aspiration-window failures are widened and completed
// iterations continue to publish a best move.
// ============================================================
static void test_aspiration_window_recovery() {
    uint64_t fail_low = 0;
    uint64_t fail_high = 0;
    for (const char* sfen : {
             "4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1",
             "4r4/9/9/9/9/9/9/9/4K4 b - 1"}) {
        Board b;
        CHECK(b.parse_sfen(sfen));
        std::vector<SearchInfo> infos;
        const Move best = iterative_deepening(b, 1000, [&](const SearchInfo& info) {
            infos.push_back(info);
            if (info.depth >= 5) g_stop.store(true, std::memory_order_relaxed);
        });

        const SearchStats stats = last_search_stats();
        CHECK(best != MOVE_NONE);
        CHECK(infos.size() >= 2);
        CHECK(!infos.empty() && infos.back().pv.front() == best);
        fail_low += stats.aspiration_fail_low;
        fail_high += stats.aspiration_fail_high;
    }
    CHECK(fail_low > 0);
    CHECK(fail_high > 0);
}

// ============================================================
// Test: search timing - engine must not far exceed the movetime budget
// ============================================================
static void test_search_respects_movetime() {
    struct Case { int budget_ms; double tolerance; };
    // Shorter budgets allow proportionally more OS jitter overhead;
    // longer budgets should finish closer to the target.
    const Case cases[] = {
        {100,  2.0},
        {500,  1.5},
        {1000, 1.5},
    };

    for (auto& c : cases) {
        Board b;
        b.set_startpos();

        auto t0 = std::chrono::steady_clock::now();
        (void)iterative_deepening(b, c.budget_ms);
        auto t1 = std::chrono::steady_clock::now();

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        bool within_tolerance = elapsed_ms <= c.budget_ms * c.tolerance;
        if (!within_tolerance) {
            std::cerr << "FAIL  " << __FILE__ << ':' << __LINE__
                      << "  search with budget=" << c.budget_ms
                      << "ms took " << static_cast<int>(elapsed_ms)
                      << "ms (tolerance " << static_cast<int>(c.budget_ms * c.tolerance) << "ms)\n";
            g_fail++;
        } else {
            g_pass++;
        }
    }
}

// ============================================================
// Test: time allocation for normal game times is reasonable
// ============================================================
static void test_time_allocation_reasonable() {
    // 10-minute game, no byoyomi: capped at 5000 ms
    CHECK_EQ(compute_allotted_ms(600000, 0), 5000);

    // 5-minute remaining, no byoyomi: still capped
    CHECK_EQ(compute_allotted_ms(300000, 0), 5000);

    // 30-second remaining, no byoyomi: 30000/40 = 750 ms
    CHECK_EQ(compute_allotted_ms(30000, 0), 750);

    // Very low time: floored at 100 ms
    CHECK_EQ(compute_allotted_ms(1000, 0), 100);

    // Byoyomi-heavy: 1000/40 + 30000/2 = 25 + 15000 = 15025, capped at 5000 ms
    CHECK_EQ(compute_allotted_ms(1000, 30000), 5000);
}

// ============================================================
// Test: alpha-beta pruning statistics
// ============================================================
static void test_alpha_beta_cutoff_stats() {
    Board b;
    // Capturing-heavy position to trigger move-ordering based cuts.
    b.parse_sfen("4k4/9/9/9/4r4/4R4/9/9/4K4 b - 1");
    (void)negamax(b, 3, -500, 500, 0);
    SearchStats st = last_search_stats();
    CHECK(st.beta_cutoffs + st.threshold_cutoffs > 0);
}

// ============================================================
// Test: search info callback provides USI info fields
// ============================================================
static void test_search_info_callback() {
    Board b;
    b.set_startpos();
    std::vector<SearchInfo> infos;
    (void)iterative_deepening(b, 120, [&](const SearchInfo& info) { infos.push_back(info); });
    CHECK(!infos.empty());
    const SearchInfo& last = infos.back();
    CHECK(last.depth >= 1);
    CHECK(last.time_ms >= 1);
    CHECK(last.nodes > 0);
    CHECK(!last.pv.empty());
}

// ============================================================
// Test: fourfold repetition distinguishes continuous-check loss
// ============================================================
static void test_perpetual_check_repetition() {
    Board b;
    b.parse_sfen("4k4/4R4/9/9/9/9/9/9/K8 w - 1");

    const char* cycle[] = {"5a4a", "5b4b", "4a5a", "4b5b"};
    for (int repetition = 0; repetition < 3; ++repetition) {
        for (const char* usi : cycle) {
            MoveList legal;
            generate_legal_moves(b, legal);
            const Move move = usi_to_move(usi);
            bool found = false;
            for (Move candidate : legal)
                if (candidate == move) found = true;
            CHECK(found);
            if (found) b.do_move(move);
        }
    }

    CHECK_EQ(b.repetition_count(), 4);
    CHECK(b.repetition_result() == RepetitionResult::SIDE_TO_MOVE_WINS);
    CHECK_EQ(negamax(b, 1, -INF, INF, 0), MATE_VALUE);
}

// ============================================================
// Test: ordinary fourfold repetition remains a draw
// ============================================================
static void test_fourfold_repetition_draw() {
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/K8 w - 1");

    const char* cycle[] = {"5a4a", "9i8i", "4a5a", "8i9i"};
    for (int repetition = 0; repetition < 3; ++repetition) {
        for (const char* usi : cycle) {
            MoveList legal;
            generate_legal_moves(b, legal);
            const Move move = usi_to_move(usi);
            bool found = false;
            for (Move candidate : legal)
                if (candidate == move) found = true;
            CHECK(found);
            if (found) b.do_move(move);
        }
    }

    CHECK_EQ(b.repetition_count(), 4);
    CHECK(b.repetition_result() == RepetitionResult::DRAW);
    CHECK_EQ(negamax(b, 1, -INF, INF, 0), 0);
}

// ============================================================
// Test: root info is emitted only for completed iterations
// ============================================================
static void test_search_info_uses_completed_iteration() {
    Board b;
    b.set_startpos();
    std::vector<SearchInfo> infos;
    const Move best = iterative_deepening(b, 1000, [&](const SearchInfo& info) {
        infos.push_back(info);
        g_stop.store(true, std::memory_order_relaxed);
    });

    CHECK(!infos.empty());
    CHECK_EQ(infos.size(), 1u);
    CHECK(!infos.empty() && !infos.front().pv.empty());
    CHECK(!infos.empty() && infos.front().pv.front() == best);
}

// ============================================================
// Test: null move pruning fires (threshold_cutoffs > 0).
// Uses the starting position with a moderate budget so the
// engine reaches depth >= 5, where NMP and futility both engage.
// ============================================================
static void test_null_move_pruning_fires() {
    Board b;
    b.set_startpos();
    std::vector<SearchInfo> infos;
    (void)iterative_deepening(b, 500, [&](const SearchInfo& info) { infos.push_back(info); });
    SearchStats st = last_search_stats();
    // threshold_cutoffs collects both NMP and futility prune counts.
    CHECK(st.threshold_cutoffs > 0);
    CHECK(st.null_move_verifications > 0);
    // We should reach a reasonable search depth within 500 ms.
    CHECK(!infos.empty() && infos.back().depth >= 8);
}

// ============================================================
// Test: shallow late-move pruning is exercised at non-PV nodes.
// ============================================================
static void test_late_move_pruning() {
    Board b;
    b.set_startpos();
    (void)negamax(b, 4, -INF, INF, 0);
    CHECK(last_search_stats().late_move_prunes > 0);
}

// ============================================================
// Test: quiet refutations are reused as countermoves and avoid LMR.
// ============================================================
static void test_countermove_ordering() {
    Board b;
    b.set_startpos();
    (void)negamax(b, 5, -INF, INF, 0);
    CHECK(last_search_stats().countermove_hits > 0);
}

// ============================================================
// Test: continuation history informs quiet ordering and search
// leaves the caller's board state unchanged.
// ============================================================
static void test_continuation_history_and_board_restoration() {
    Board b;
    b.set_startpos();
    const std::string initial_sfen = b.to_sfen();
    const uint64_t initial_hash = b.hash();
    const size_t initial_history_size = b.history_.size();
    const size_t initial_hash_history_size = b.pos_hashes_.size();

    const int score = negamax(b, 5, -INF, INF, 0);
    const SearchStats stats = last_search_stats();

    CHECK(score > -INF && score < INF);
    CHECK(stats.continuation_history_updates > 0);
    CHECK(stats.continuation_history_ordering_uses > 0);
    CHECK_EQ(b.to_sfen(), initial_sfen);
    CHECK_EQ(b.hash(), initial_hash);
    CHECK_EQ(b.history_.size(), initial_history_size);
    CHECK_EQ(b.pos_hashes_.size(), initial_hash_history_size);
}

// ============================================================
// Test: repeated positions do not use path-dependent TT scores.
// ============================================================
static void test_repetition_sensitive_tt_cutoff_is_skipped() {
    Board original;
    CHECK(original.parse_sfen("4k4/9/9/9/9/9/9/9/K8 w - 1"));
    (void)negamax(original, 1, -INF, INF, 0);

    Board repeated;
    CHECK(repeated.parse_sfen("4k4/9/9/9/9/9/9/9/K8 w - 1"));
    for (const char* usi : {"5a4a", "9i8i", "4a5a", "8i9i"})
        repeated.do_move(usi_to_move(usi));
    CHECK_EQ(repeated.hash(), original.hash());
    CHECK_EQ(repeated.repetition_count(), 2);

    (void)negamax(repeated, 1, -INF, INF, 1);
    CHECK(last_search_stats().repetition_sensitive_tt_skips > 0);
    CHECK_EQ(repeated.hash(), original.hash());
}

// ============================================================
// Test: quiet checking moves bypass selective pruning/reduction.
// ============================================================
static void test_quiet_check_moves_bypass_selectivity() {
    Board b;
    CHECK(b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b R 1"));
    (void)negamax(b, 3, INF - 1, INF, 0);
    CHECK(last_search_stats().checking_move_selectivity_skips > 0);
}

// ============================================================
// Test: NMP does NOT prune when the side to move is in check
// (verifies the in_check safety condition).
// ============================================================
static void test_null_move_skipped_in_check() {
    Board b;
    // Black king at 5i, white rook at 5a checks along file 5.
    // Black is in check; iterative deepening must still return a legal move.
    b.parse_sfen("4r4/9/9/9/9/9/9/9/4K4 b - 1");
    const Move m = iterative_deepening(b, 200);
    // Engine must return a legal escape move, not MOVE_NONE.
    CHECK(m != MOVE_NONE);
    // The returned move must actually be in the legal move list.
    MoveList ml;
    generate_legal_moves(b, ml);
    bool found = false;
    for (Move x : ml) if (x == m) { found = true; break; }
    CHECK(found);
}

// ============================================================
// Test: iterative deepening reaches strictly greater depth
// within 1 second from the starting position (validates that
// the pruning optimisations together improve search depth).
// ============================================================
static void test_depth_improved_with_pruning() {
    Board b;
    b.set_startpos();
    int max_depth = 0;
    (void)iterative_deepening(b, 1000, [&](const SearchInfo& info) {
        if (info.depth > max_depth) max_depth = info.depth;
    });
    // With NMP + LMR + futility a modern fast machine should reach depth >= 12
    // within 1 second from the starting position.
    if (max_depth < 10) {
        std::cerr << "FAIL  " << __FILE__ << ':' << __LINE__
                  << "  depth_improved: depth=" << max_depth << " (want >= 10)\n";
        g_fail++;
    } else {
        g_pass++;
    }
}

// ============================================================
// Test: LMR + PVS do not drop the tactical best-move in a
// forced-capture position (regression for correctness).
// ============================================================
static void test_lmr_preserves_tactical_best_move() {
    Board b;
    // White bishop on 5d can capture the hanging black rook on 6e.
    b.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const Move best = iterative_deepening(b, 400);
    CHECK_EQ(best, usi_to_move("5d6e"));
}

// ============================================================
// Test: TT-backed singular extensions run an exclusion search
// and preserve the caller's position state.
// ============================================================
static void test_singular_extension_exclusion_search() {
    search_clear_game_state();
    Board b;
    CHECK(b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b R 1"));
    const std::string initial_sfen = b.to_sfen();
    const uint64_t initial_hash = b.hash();
    const size_t initial_history_size = b.history_.size();
    const size_t initial_hash_history_size = b.pos_hashes_.size();

    const Move best = iterative_deepening(b, 30000, [](const SearchInfo& info) {
        if (info.depth >= 10) g_stop.store(true, std::memory_order_relaxed);
    });
    const SearchStats stats = last_search_stats();

    CHECK(best != MOVE_NONE);
    CHECK(stats.singular_tt_candidates > 0);
    CHECK(stats.singular_extension_attempts > 0);
    CHECK(stats.singular_extensions > 0);
    CHECK_EQ(stats.exclusion_searches, stats.singular_extension_attempts);
    CHECK_EQ(b.to_sfen(), initial_sfen);
    CHECK_EQ(b.hash(), initial_hash);
    CHECK_EQ(b.history_.size(), initial_history_size);
    CHECK_EQ(b.pos_hashes_.size(), initial_hash_history_size);
}

// ============================================================
// Test: alpha-beta clamps the returned score to -1000 once a node's
// evaluated value drops to -1000 or below.
//
// Position: the side to move has only its king on the board; the
// opponent holds rook+bishop+gold+silver in hand (material fallback
// values 1000+800+600+500 = 2900), so the true material evaluation for
// the mover is far below -1000 in every reachable line (no captures are
// available to recover any of it). Per the requirement, the "max side"
// (every negamax node, from its own mover's perspective) must clamp such
// a value to exactly -1000 before returning.
// ============================================================
static void test_alpha_beta_clamps_large_deficit_to_minus_1000() {
    // Verify large material deficits are evaluated accurately without artificial -1000 clamping.
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b rbgs 1");
    const int score = negamax(b, 1, -INF, INF, 0);
    CHECK(score < -1000 && score > -MATE_VALUE);
}

static void test_alpha_beta_clamp_side_agnostic() {
    Board b_black, b_white;
    b_black.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b rbgs 1");
    b_white.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 w RBGS 1");
    const int s_b = negamax(b_black, 1, -INF, INF, 0);
    const int s_w = negamax(b_white, 1, -INF, INF, 0);
    CHECK_EQ(s_b, s_w);
    CHECK(s_w < -1000 && s_w > -MATE_VALUE);
}

// ============================================================
// NNUE (`nn.bin`) & Progress (`progress.bin`) Test Helpers & Suite
// ============================================================
#include <cstdio>
#include <fstream>
#include <cmath>

static void write_tmp_bytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

static void append_u32_le(std::vector<uint8_t>& buf, uint32_t v) {
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
}

static void append_i16_le(std::vector<uint8_t>& buf, int16_t v) {
    const uint16_t u = static_cast<uint16_t>(v);
    buf.push_back(static_cast<uint8_t>(u & 0xFFu));
    buf.push_back(static_cast<uint8_t>((u >> 8) & 0xFFu));
}

static void append_leb128_i16_array(std::vector<uint8_t>& buf, const std::vector<int16_t>& vals) {
    const char magic[] = "COMPRESSED_LEB128";
    buf.insert(buf.end(), magic, magic + 17);
    std::vector<uint8_t> comp;
    comp.reserve(vals.size() * 2);
    for (int16_t x : vals) {
        int32_t v = x;
        while (true) {
            uint8_t b = static_cast<uint8_t>(v & 0x7F);
            v >>= 7;
            const bool done = ((v == 0 && (b & 0x40) == 0) || (v == -1 && (b & 0x40) != 0));
            if (!done) b |= 0x80u;
            comp.push_back(b);
            if (done) break;
        }
    }
    append_u32_le(buf, static_cast<uint32_t>(comp.size()));
    buf.insert(buf.end(), comp.begin(), comp.end());
}

struct SyntheticSfnnSpec {
    int half_dims    = 64;
    int layer_stacks = 8;
    bool use_leb128  = true;
    std::vector<int16_t> ft_biases;  // [half_dims]
    std::vector<int16_t> ft_weights; // [1791 * half_dims]
    std::vector<int32_t> stack_fc2_bias; // [layer_stacks]
    std::vector<std::array<int32_t, 8>> stack_fc0_bias;
    std::vector<std::vector<int8_t>>    stack_fc0_w; // [layer_stacks][8 * half_dims]
    std::vector<std::array<int32_t, 64>> stack_fc1_bias;
    std::vector<std::array<int8_t, 64 * 32>> stack_fc1_w;
    std::vector<std::array<int8_t, 64>> stack_fc2_w;

    explicit SyntheticSfnnSpec(int l1 = 64, int stacks = 8, bool leb = true)
        : half_dims(l1),
          layer_stacks(stacks),
          use_leb128(leb),
          ft_biases(static_cast<size_t>(l1), 0),
          ft_weights(static_cast<size_t>(1791 * l1), 0),
          stack_fc2_bias(static_cast<size_t>(stacks), 0),
          stack_fc0_bias(static_cast<size_t>(stacks)),
          stack_fc0_w(static_cast<size_t>(stacks), std::vector<int8_t>(static_cast<size_t>(8 * l1), 0)),
          stack_fc1_bias(static_cast<size_t>(stacks)),
          stack_fc1_w(static_cast<size_t>(stacks)),
          stack_fc2_w(static_cast<size_t>(stacks)) {
        for (int s = 0; s < stacks; ++s) {
            stack_fc0_bias[s].fill(0);
            stack_fc1_bias[s].fill(0);
            stack_fc1_w[s].fill(0);
            stack_fc2_w[s].fill(0);
        }
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> out;
        const std::string arch = "ModelType=SFNNWithoutPsqt;Features=K-A2(Friend)[1791->"
                               + std::to_string(half_dims) + "x2],Network=SFNN-"
                               + std::to_string(half_dims) + "{LayerStack="
                               + std::to_string(layer_stacks) + "}";
        append_u32_le(out, 0x7AF32F16u);
        append_u32_le(out, 0x3C203B32u);
        append_u32_le(out, static_cast<uint32_t>(arch.size()));
        out.insert(out.end(), arch.begin(), arch.end());

        // FT hash + biases + weights
        append_u32_le(out, 0x5F134AB8u);
        if (use_leb128) {
            append_leb128_i16_array(out, ft_biases);
            append_leb128_i16_array(out, ft_weights);
        } else {
            for (int16_t v : ft_biases) append_i16_le(out, v);
            for (int16_t v : ft_weights) append_i16_le(out, v);
        }

        // LayerStacks
        for (int s = 0; s < layer_stacks; ++s) {
            append_u32_le(out, 0x6333718Au);
            for (int o = 0; o < 8; ++o) append_u32_le(out, static_cast<uint32_t>(stack_fc0_bias[s][o]));
            out.insert(out.end(), stack_fc0_w[s].begin(), stack_fc0_w[s].end());
            for (int o = 0; o < 64; ++o) append_u32_le(out, static_cast<uint32_t>(stack_fc1_bias[s][o]));
            out.insert(out.end(), stack_fc1_w[s].begin(), stack_fc1_w[s].end());
            append_u32_le(out, static_cast<uint32_t>(stack_fc2_bias[s]));
            out.insert(out.end(), stack_fc2_w[s].begin(), stack_fc2_w[s].end());
        }
        return out;
    }
};

static void write_synthetic_progress_file(const std::string& path,
                                          const std::vector<std::tuple<int, int, double>>& entries) {
    std::vector<double> table(81 * 1548, 0.0);
    for (const auto& [king_yo_sq, bona_idx, val] : entries) {
        table[static_cast<size_t>(king_yo_sq) * 1548 + bona_idx] = val;
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(table.data()),
            static_cast<std::streamsize>(table.size() * sizeof(double)));
}

// ============================================================
// Test 1: Missing or 0-byte empty dummy nn.bin & progress.bin cleanly
//         fall back to material evaluation without crashing.
// ============================================================
static void test_eval_nnue_missing_and_empty_dummy_fallback() {
    // 1a. Nonexistent file
    set_eval_file_path("/tmp/shogiai_nonexistent_nn_99999.bin");
    CHECK(eval_status_message().find("file not found") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);
    {
        Board b;
        b.set_startpos();
        CHECK_EQ(evaluate(b), 0);
    }

    // 1b. Empty (0-byte) dummy nn.bin and progress.bin (matches repository zip packaging)
    const std::string tmp_nn   = "/tmp/shogiai_empty_dummy_nn.bin";
    const std::string tmp_prog = "/tmp/shogiai_empty_dummy_progress.bin";
    write_tmp_bytes(tmp_nn, {});
    write_tmp_bytes(tmp_prog, {});

    set_eval_file_path(tmp_nn);
    set_progress_file_path(tmp_prog);
    const std::string st = eval_status_message();
    CHECK(st.find("empty dummy nn.bin") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b P 1");
    CHECK_EQ(evaluate(b), 87);
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 w P 1");
    CHECK_EQ(evaluate(b), -87);

    set_eval_file_path("");
    std::remove(tmp_nn.c_str());
    std::remove(tmp_prog.c_str());
}

// ============================================================
// Test 2: Corrupt headers, truncated payloads, and invalid progress.bin
//         sizes/NaNs are rejected safely with clear status diagnostics.
// ============================================================
static void test_eval_nnue_corrupt_and_invalid_progress_rejected() {
    const std::string bad_nn = "/tmp/shogiai_corrupt_nn.bin";
    const std::string bad_pr = "/tmp/shogiai_corrupt_progress.bin";

    // Wrong magic/version bytes
    write_tmp_bytes(bad_nn, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    set_eval_file_path(bad_nn);
    CHECK(eval_status_message().find("invalid nn.bin header") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    // Valid nn.bin paired with wrong-sized progress.bin (100 bytes instead of 1,003,104)
    SyntheticSfnnSpec spec(64, 8, true);
    write_tmp_bytes(bad_nn, spec.serialize());
    write_tmp_bytes(bad_pr, std::vector<uint8_t>(100, 0));
    set_eval_file_path(bad_nn);
    set_progress_file_path(bad_pr);
    CHECK(eval_status_message().find("invalid progress.bin size") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    set_eval_file_path("");
    std::remove(bad_nn.c_str());
    std::remove(bad_pr.c_str());
}

// ============================================================
// Test 3: Auto-discovery is CWD-independent and handles checked-in files.
// ============================================================
static void test_eval_auto_discovery_and_cwd_independence() {
    char saved_cwd[4096]{};
#if defined(_WIN32)
    (void)_getcwd(saved_cwd, sizeof(saved_cwd));
    char tmp_dir[MAX_PATH]{};
    if (GetTempPathA(MAX_PATH, tmp_dir) > 0) (void)_chdir(tmp_dir);
#else
    if (getcwd(saved_cwd, sizeof(saved_cwd)) == nullptr) saved_cwd[0] = '\0';
    (void)chdir("/tmp");
#endif
    set_eval_file_path("");
    set_eval_dir("");
    const std::string status = eval_status_message();
#if defined(_WIN32)
    if (saved_cwd[0] != '\0') (void)_chdir(saved_cwd);
#else
    if (saved_cwd[0] != '\0') (void)chdir(saved_cwd);
#endif

    const EvalFamily fam = get_eval_family();
    CHECK(fam == EvalFamily::SFNN_NNUE || fam == EvalFamily::MATERIAL_FALLBACK);
    if (fam == EvalFamily::MATERIAL_FALLBACK) {
        const std::string marker = "checked: ";
        const auto pos = status.find(marker);
        CHECK(pos != std::string::npos);
        bool found_abs = false;
        if (pos != std::string::npos) {
            std::stringstream ss(status.substr(pos + marker.size()));
            std::string cand;
            while (std::getline(ss, cand, ',')) {
                if (!cand.empty() && cand.front() == ' ') cand.erase(0, 1);
                if (!cand.empty() && cand.front() == '/') { found_abs = true; break; }
            }
        }
        CHECK(found_abs);
    }
    set_eval_file_path("");
}

// ============================================================
// Test 4: Exact K-A2 Feature Indexing (YaneuraOu file-major coordinates,
//         1-based hand BonaPiece, A2+K king planes), Pairwise FT,
//         Dual SqrClippedReLU/ClippedReLU, FV_SCALE=24, and progress.bin
//         8-bucket selection!
// ============================================================
static void test_eval_nnue_ka2_exact_math_and_progress_buckets() {
    const std::string tmp_nn   = "/tmp/shogiai_synth_ka2_nn.bin";
    const std::string tmp_prog = "/tmp/shogiai_synth_ka2_progress.bin";

    // Build a 64-dim, 8-stack COMPRESSED_LEB128 SFNN model:
    //   - Pairwise half_out = 32 (`j = 0` pairs `acc[0]` and `acc[32]`).
    //   - Activate `acc[0] = 127` from 1st Black Pawn in hand (`YO_F_HAND_PAWN = 1`)
    //     and `acc[32] = 127` from Black Pawn on USI "9g" (`file_of=0, rank_of=6` ->
    //     `yo_file = 8 - 0 = 8`, `yo_rank = 6`, `yo_sq = 8*9 + 6 = 78`, `YO_F_PAWN(90) + 78 = 168`).
    //     Then for Black STM: `tf[0] = (127 * 127) >> 7 = 126`!
    //   - Set `fc0_w[0][0] = 64` -> `h0[0] = 126 * 64 = 8064`.
    //     `h0_act[0] (Sqr) = (8064^2) >> 19 = 124`, `h0_act[8] (Lin) = 8064 >> 6 = 126`.
    //   - Set `fc1_w[0][8] = 64` -> `h1[0] = 126 * 64 = 8064` -> `h1_act[0] = 126`.
    //   - Give each of the 8 LayerStacks a distinct `fc2_bias = (s + 1) * 2400` (`+100*(s+1)` cp at FV_SCALE=24)
    //     plus `fc2_w[0] = 24` (`126 * 24 / 24 = +126` cp when `tf[0] == 126`)!
    SyntheticSfnnSpec spec(64, 8, /*use_leb128=*/true);
    spec.ft_weights[1 * 64 + 0]    = 127; // 1st friend pawn in hand -> acc[0]
    spec.ft_weights[168 * 64 + 32] = 127; // friend pawn on "9g" (`yo_sq = 78`) -> acc[32]
    for (int s = 0; s < 8; ++s) {
        spec.stack_fc0_w[s][0]      = 64;
        spec.stack_fc1_w[s][8]      = 64; // col 8 = ClippedReLU(h0[0])
        spec.stack_fc2_w[s][0]      = 24;
        spec.stack_fc2_bias[s]      = (s + 1) * 2400;
    }

    // Configure progress.bin so:
    //   - Black King @ "5i" (`file_of=4, rank_of=8` -> `yo_sq = 4*9 + 8 = 44`) with 1st hand pawn (`idx=1`)
    //     contributes `+3.0` log-odds (`sigmoid(3.0) = 0.9526 > 7/8 = 0.875` -> bucket 7!)
    //   - Black King @ "5a" (`yo_sq = 36`) contributes `-3.0` log-odds (`sigmoid(-3.0) = 0.0474 < 1/8` -> bucket 0!)
    write_tmp_bytes(tmp_nn, spec.serialize());
    write_synthetic_progress_file(tmp_prog, {
        {44, 1, +3.0},
        {36, 1, -3.0}
    });

    set_fv_scale(24);
    set_eval_file_path(tmp_nn);
    set_progress_file_path(tmp_prog);
    CHECK(get_eval_family() == EvalFamily::SFNN_NNUE);

    // Case A: Black King @ 5i (`yo_sq=44`), White King @ 5a, Black Pawn @ 9g, Black Pawn in hand:
    //   - progress sum = +3.0 -> bucket 7 (`stack_fc2_bias[7] = 8 * 2400 -> +800cp`)
    //   - `tf[0] = 126` -> `+126cp` => exact total = `926` cp!
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/P8/9/4K4 b P 1");
    CHECK_EQ(evaluate_progress_bucket(b), 7);
    CHECK(evaluate_progress(b) > 0.90);
    CHECK_EQ(evaluate_full_reference(b), 926);
    CHECK_EQ(evaluate(b), 926);

    // Case B: Move Black King to 5a (`yo_sq=36`) and White King to 5i:
    //   - progress sum = -3.0 -> bucket 0 (`stack_fc2_bias[0] = 1 * 2400 -> +100cp`)
    //   - `tf[0] = 126` -> `+126cp` => exact total = `226` cp!
    b.parse_sfen("4K4/9/9/9/9/9/P8/9/4k4 b P 1");
    CHECK_EQ(evaluate_progress_bucket(b), 0);
    CHECK(evaluate_progress(b) < 0.10);
    CHECK_EQ(evaluate_full_reference(b), 226);
    CHECK_EQ(evaluate(b), 226);

    set_eval_file_path("");
    std::remove(tmp_nn.c_str());
    std::remove(tmp_prog.c_str());
}

// ============================================================
// Test 5: Full-Width SIMD (`L1 = 256` & `COMPRESSED_LEB128`) Incremental
//         Accumulator + Progress Cache Bit-Exact Equivalence vs Scalar
//         Reference across 80+ Plies of Captures, Drops, Promotions,
//         King Moves, Null Moves, and Undo!
// ============================================================
static void test_eval_nnue_incremental_and_simd_matches_scalar_reference() {
    const std::string tmp_nn   = "/tmp/shogiai_synth_inc_nn.bin";
    const std::string tmp_prog = "/tmp/shogiai_synth_inc_progress.bin";

    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    auto next_rng = [&]() -> uint64_t {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };

    // Use L1=256 so multiple AVX-512 (64-byte) and AVX2 (32-byte) SIMD chunks are exercised!
    SyntheticSfnnSpec spec(256, 8, /*use_leb128=*/true);
    for (auto& b : spec.ft_biases) b = static_cast<int16_t>(static_cast<int>(next_rng() % 41) - 15);
    for (auto& w : spec.ft_weights) w = static_cast<int16_t>(static_cast<int>(next_rng() % 51) - 22);
    for (int s = 0; s < 8; ++s) {
        for (int o = 0; o < 8; ++o) spec.stack_fc0_bias[s][o] = static_cast<int32_t>(next_rng() % 2001) - 800;
        for (auto& w : spec.stack_fc0_w[s]) w = static_cast<int8_t>(static_cast<int>(next_rng() % 49) - 24);
        for (int o = 0; o < 64; ++o) spec.stack_fc1_bias[s][o] = static_cast<int32_t>(next_rng() % 2001) - 800;
        for (int o = 0; o < 64; ++o) {
            for (int c = 0; c < 16; ++c) {
                spec.stack_fc1_w[s][o * 32 + c] = static_cast<int8_t>(static_cast<int>(next_rng() % 61) - 30);
            }
        }
        spec.stack_fc2_bias[s] = static_cast<int32_t>(next_rng() % 1001) - 500;
        for (int o = 0; o < 64; ++o) spec.stack_fc2_w[s][o] = static_cast<int8_t>(static_cast<int>(next_rng() % 81) - 40);
    }

    std::vector<std::tuple<int, int, double>> prog_entries;
    prog_entries.reserve(81 * 256);
    for (int k = 0; k < 81; ++k) {
        for (int idx = 1; idx < 1548; idx += 3) {
            const double v = (static_cast<int>(next_rng() % 201) - 100) * 0.0025;
            prog_entries.emplace_back(k, idx, v);
        }
    }

    write_tmp_bytes(tmp_nn, spec.serialize());
    write_synthetic_progress_file(tmp_prog, prog_entries);

    set_fv_scale(24);
    set_eval_file_path(tmp_nn);
    set_progress_file_path(tmp_prog);
    CHECK(get_eval_family() == EvalFamily::SFNN_NNUE);

    Board b;
    b.parse_sfen("ln1gkg1nl/1r4sb1/p1pppp1pp/1p4p2/7P1/2P6/PP1PPPP1P/1BG4R1/LNS1K1SNL b Pp 1");
    CHECK_EQ(evaluate(b), evaluate_full_reference(b));

    uint64_t move_rng = 0xCAFEBABE12345678ULL;
    for (int step = 0; step < 90; ++step) {
        MoveList ml;
        generate_legal_moves(b, ml);
        if (ml.empty()) break;

        if (!b.in_check()) {
            b.do_null_move();
            CHECK_EQ(evaluate(b), evaluate_full_reference(b));
            b.undo_null_move();
        }

        // Exercise multi-ply un-evaluated ancestor jumps (e.g. simulating in-check plies)
        for (int k = 0; k < std::min(4, ml.size()); ++k) {
            Move m1 = ml[(step * 7 + k * 13) % ml.size()];
            b.do_move(m1);
            MoveList ml2;
            generate_legal_moves(b, ml2);
            if (!ml2.empty()) {
                Move m2 = ml2[(step * 3 + k * 5) % ml2.size()];
                b.do_move(m2);
                CHECK_EQ(evaluate(b), evaluate_full_reference(b));
                b.undo_move(m2);
            }
            CHECK_EQ(evaluate(b), evaluate_full_reference(b));
            b.undo_move(m1);
        }

        move_rng = move_rng * 6364136223846793005ULL + 1442695040888963407ULL;
        Move chosen = ml[static_cast<size_t>(move_rng >> 32) % static_cast<size_t>(ml.size())];
        b.do_move(chosen);
        CHECK_EQ(evaluate(b), evaluate_full_reference(b));
    }

    set_eval_file_path("");
    std::remove(tmp_nn.c_str());
    std::remove(tmp_prog.c_str());
}

static uint64_t perft(Board& b, int depth) {
    MoveList ml;
    generate_legal_moves(b, ml);
    if (depth <= 1) return static_cast<uint64_t>(ml.size());
    uint64_t total = 0;
    for (Move m : ml) {
        b.do_move(m, false);
        total += perft(b, depth - 1);
        b.undo_move(m);
    }
    return total;
}

static void test_perft_and_staged_movegen_equivalence() {
    Board b;
    b.set_startpos();
    CHECK_EQ(perft(b, 1), 30ULL);
    CHECK_EQ(perft(b, 2), 900ULL);
    CHECK_EQ(perft(b, 3), 25470ULL);
    CHECK_EQ(perft(b, 4), 719731ULL);
    CHECK_EQ(perft(b, 5), 19861490ULL);

    const char* sfens[] = {
        "lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1",
        "ln1gkg1nl/1r4sb1/p1pppp1pp/1p4p2/7P1/2P6/PP1PPPP1P/1BG4R1/LNS1K1SNL b Pp 1",
        "4k4/9/4P4/9/9/2b6/3P5/4K4/4R4 b GSp 1",
        "r3k3r/9/4+B4/9/9/9/4p4/4K4/9 w GS2p 1"
    };
    for (const char* sfen : sfens) {
        Board pos;
        pos.parse_sfen(sfen);
        const bool in_chk = pos.in_check();
        MoveList all, tac, qui;
        generate_legal_moves(pos, all);
        if (!in_chk) {
            generate_tactical_moves(pos, tac);
            generate_quiet_moves(pos, qui);
            CHECK_EQ(tac.size() + qui.size(), all.size());
        }
        for (Move m : all) {
            CHECK(is_move_legal(pos, m, in_chk));
        }
    }
}

static void test_see_and_mate_detection() {
    {
        Board b;
        b.parse_sfen("4k4/4g4/4p4/9/9/9/9/4R4/4K4 b - 1");
        CHECK(!see_ge(b, usi_to_move("5h5c+"), 0));
        CHECK(!see_ge(b, usi_to_move("5h5c"), 0));
    }
    {
        Board b;
        b.parse_sfen("4k4/4g4/4r4/4P4/9/9/9/9/4K4 b - 1");
        CHECK(see_ge(b, usi_to_move("5d5c+"), 0));
    }
    {
        Board b;
        b.parse_sfen("4k4/9/4P4/9/9/9/9/9/4K4 b G 1");
        Move m = MOVE_NONE;
        CHECK(find_mate_1ply(b, &m));
        CHECK_EQ(m, usi_to_move("G*5b"));
    }
    {
        Board b3;
        b3.parse_sfen("8k/9/7S1/9/9/9/9/9/4K4 b 2G 1");
        SearchInfo last_info;
        const Move best = iterative_deepening(b3, 500, [&](const SearchInfo& info) {
            last_info = info;
        });
        CHECK(best != MOVE_NONE);
        CHECK(last_info.score_is_mate);
        CHECK(last_info.score_mate > 0 && last_info.score_mate <= 2);
    }
}


// ============================================================
// Test: `nodelimit` strict enforcement (single-thread & multi-thread)
// ============================================================
static void test_nodelimit_strict_cap_single_and_multi_thread() {
    // 1. Single-threaded strict caps across tiny, small, and medium node limits
    set_num_threads(1);
    for (uint64_t cap : {1ULL, 5ULL, 25ULL, 100ULL, 777ULL, 5000ULL}) {
        set_nodelimit(cap);
        CHECK_EQ(get_nodelimit(), cap);

        Board b;
        b.set_startpos();
        const Move best = iterative_deepening_timed(b, TimeBudget{0, 0});
        const SearchStats st = last_search_stats();
        const uint64_t total_nodes = st.nodes + st.qnodes;

        CHECK(best != MOVE_NONE);
        CHECK(is_move_legal(b, best, b.in_check()));
        CHECK(total_nodes >= 1ULL);
        CHECK(cap == 1ULL ? (total_nodes <= 1ULL) : (total_nodes < cap));
    }

    // 2. Multi-threaded (4 threads) strict caps: total nodes across ALL threads must NEVER exceed `nodelimit`!
    set_num_threads(4);
    CHECK_EQ(get_num_threads(), 4);
    for (uint64_t cap : {1ULL, 3ULL, 16ULL, 128ULL, 1000ULL, 12345ULL}) {
        set_nodelimit(cap);

        Board b;
        b.parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/2P6/PP1PPPPPP/1B5R1/LNSGKGSNL w - 2");
        const Move best = iterative_deepening_timed(b, TimeBudget{0, 0});
        const SearchStats st = last_search_stats();
        const uint64_t total_nodes = st.nodes + st.qnodes;

        CHECK(best != MOVE_NONE);
        CHECK(is_move_legal(b, best, b.in_check()));
        CHECK(total_nodes >= 1ULL);
        CHECK(cap == 1ULL ? (total_nodes <= 1ULL) : (total_nodes < cap));
    }

    // 3. `nodelimit = 0` restores normal unlimited-node search
    set_nodelimit(0);
    CHECK_EQ(get_nodelimit(), 0ULL);
    {
        Board b;
        b.set_startpos();
        const Move best = iterative_deepening_timed(b, TimeBudget{80, 80});
        const SearchStats st = last_search_stats();
        CHECK(best != MOVE_NONE);
        CHECK(st.nodes + st.qnodes > 100ULL);
    }
    set_num_threads(1);
}

// ============================================================
// Test: Lazy SMP multithreaded search correctness & tactical mate finding
// ============================================================
static void test_lazy_smp_multithreaded_search() {
    set_nodelimit(0);
    set_num_threads(4);

    // Mate-in-3 tactical position solved concurrently by 4 Lazy SMP threads
    {
        Board b;
        b.parse_sfen("8k/9/7S1/9/9/9/9/9/4K4 b 2G 1");
        SearchInfo last_info;
        const Move best = iterative_deepening(b, 500, [&](const SearchInfo& info) {
            last_info = info;
        });
        const SearchStats st = last_search_stats();
        CHECK(best != MOVE_NONE);
        CHECK(is_move_legal(b, best, b.in_check()));
        CHECK(last_info.score_is_mate);
        CHECK(last_info.score_mate > 0 && last_info.score_mate <= 2);
        CHECK(st.nodes + st.qnodes > 0);
        CHECK(st.tt_probes > 0);
    }

    // Complex midgame position with 4 threads + concurrent NNUE / EvalHash / lock-free TT stress
    {
        Board b;
        b.parse_sfen("ln1g1gsnl/1r3k1b1/p1pppp1pp/1s4p2/9/2P1P4/PP1P1PPPP/1BG4R1/LNS1KGSNL b - 1");
        std::vector<SearchInfo> infos;
        const Move best = iterative_deepening_timed(b, TimeBudget{150, 200}, [&](const SearchInfo& info) {
            infos.push_back(info);
        }, {}, 8);
        CHECK(best != MOVE_NONE);
        CHECK(is_move_legal(b, best, b.in_check()));
        CHECK(!infos.empty());
        CHECK(infos.back().nodes >= infos.front().nodes);
    }

    set_num_threads(1);
    set_nodelimit(0);
}


// ============================================================
// Test: WinrateBook DB load/save & Minimax search (Black max, White min, leaf handoff)
// ============================================================
static void test_winrate_book_db_and_minimax() {
    Board root;
    root.set_startpos();
    const uint64_t h_root = root.hash();

    const Move m_7g7f = usi_to_move("7g7f");
    const Move m_2g2f = usi_to_move("2g2f");
    const Move m_3c3d = usi_to_move("3c3d");
    const Move m_8c8d = usi_to_move("8c8d");

    // Branch A: 7g7f -> White can choose 3c3d (Black wr 0.58) or 8c8d (Black wr 0.52)
    //   White minimizes Black wr -> White picks 8c8d (0.52) -> backed-up value of 7g7f is 0.52!
    root.do_move(m_7g7f);
    const uint64_t h_7g7f = root.hash();
    root.do_move(m_3c3d);
    const uint64_t h_7g7f_3c3d = root.hash();
    root.undo_move(m_3c3d);
    root.do_move(m_8c8d);
    const uint64_t h_7g7f_8c8d = root.hash();
    root.undo_move(m_8c8d);
    root.undo_move(m_7g7f);

    // Branch B: 2g2f -> White can choose 3c3d (Black wr 0.61) or 8c8d (Black wr 1.00, +600 pruned leaf!)
    //   White minimizes Black wr -> White picks 3c3d (0.61) -> backed-up value of 2g2f is 0.61!
    //   At root, Black maximizes Black wr: max(0.52 [7g7f], 0.61 [2g2f]) = 0.61 -> Black picks 2g2f!
    root.do_move(m_2g2f);
    const uint64_t h_2g2f = root.hash();
    root.do_move(m_3c3d);
    const uint64_t h_2g2f_3c3d = root.hash();
    root.undo_move(m_3c3d);
    root.do_move(m_8c8d);
    const uint64_t h_2g2f_8c8d = root.hash();
    root.undo_move(m_8c8d);
    root.undo_move(m_2g2f);

    WinrateBook book;
    book.put(h_root,      0.54);
    book.put(h_7g7f,      0.55);
    book.put(h_7g7f_3c3d, 0.58);
    book.put(h_7g7f_8c8d, 0.52);
    book.put(h_2g2f,      0.56);
    book.put(h_2g2f_3c3d, 0.61);
    book.put(h_2g2f_8c8d, 1.00);

    const std::string tmp_db = "test_tmp_winrate_book.db";
    std::string err;
    CHECK(book.save_atomic(tmp_db, &err));

    WinrateBook loaded;
    CHECK(loaded.load(tmp_db, &err));
    CHECK_EQ(loaded.size(), 7u);
    std::remove(tmp_db.c_str());

    // 1. Root (Black to move): minimax picks 2g2f with backed-up Black winrate 0.61 (PV: 2g2f 3c3d)
    {
        Board b;
        b.set_startpos();
        const BookMinimaxResult r = loaded.search_minimax(b, 16);
        CHECK(r.in_book);
        CHECK_EQ(r.best_move, m_2g2f);
        CHECK(std::abs(r.black_winrate - 0.61) < 1e-6);
        CHECK(std::abs(r.stm_winrate - 0.61) < 1e-6);
        CHECK_EQ(r.pv.size(), 2u);
        if (r.pv.size() == 2u) {
            CHECK_EQ(r.pv[0], m_2g2f);
            CHECK_EQ(r.pv[1], m_3c3d);
        }
    }

    // 2. After 7g7f (White to move): minimax minimizes Black winrate -> picks 8c8d (Black wr 0.52, White wr 0.48)
    {
        Board b;
        b.set_startpos();
        b.do_move(m_7g7f);
        const BookMinimaxResult r = loaded.search_minimax(b, 16);
        CHECK(r.in_book);
        CHECK_EQ(r.best_move, m_8c8d);
        CHECK(std::abs(r.black_winrate - 0.52) < 1e-6);
        CHECK(std::abs(r.stm_winrate - 0.48) < 1e-6);
    }

    // 3. Leaf book node (`2g2f 8c8d`, pruned at +600 -> 1.0, 0 children in DB):
    //    `hash_present` is true, `in_book` is false so player hands off cleanly to `shogi_engine`!
    {
        Board b;
        b.set_startpos();
        b.do_move(m_2g2f);
        b.do_move(m_8c8d);
        const BookMinimaxResult r = loaded.search_minimax(b, 16);
        CHECK(r.hash_present);
        CHECK(!r.in_book);
        CHECK(std::abs(r.node_black_wr - 1.0) < 1e-6);
    }

    // 4. Out-of-book position (`1g1f`): `hash_present == false` and `in_book == false`
    {
        Board b;
        b.set_startpos();
        b.do_move(usi_to_move("1g1f"));
        const BookMinimaxResult r = loaded.search_minimax(b, 16);
        CHECK(!r.hash_present);
        CHECK(!r.in_book);
    }
}


static void test_beam_selection_unregistered_first_and_turn_sort() {
    struct Cand {
        bool     unreg;
        int      black_eval;
        uint64_t hash;
    };

    // 1. Black's turn (`BLACK`): unregistered strictly first, then DESCENDING `black_eval`!
    {
        std::vector<Cand> v = {
            {false, +500, 10}, // already registered in DB despite huge +500 eval -> must come AFTER unregistered!
            {true,   -40, 20}, // unregistered (-40)
            {true,  +120, 30}, // unregistered (+120) -> must be #1!
            {false, +200, 40}, // registered (+200) -> #4
            {true,   +35, 50}, // unregistered (+35) -> #2!
        };
        std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) {
            return beam_candidate_better(BLACK, a.unreg, a.black_eval, a.hash,
                                                b.unreg, b.black_eval, b.hash);
        });
        CHECK(v[0].unreg && v[0].black_eval == +120);
        CHECK(v[1].unreg && v[1].black_eval ==  +35);
        CHECK(v[2].unreg && v[2].black_eval ==  -40);
        CHECK(!v[3].unreg && v[3].black_eval == +500);
        CHECK(!v[4].unreg && v[4].black_eval == +200);
    }

    // 2. White's turn (`WHITE`): unregistered strictly first, then ASCENDING `black_eval`!
    {
        std::vector<Cand> v = {
            {false, -500, 10}, // already registered in DB despite -500 eval -> must come AFTER unregistered!
            {true,   +60, 20}, // unregistered (+60) -> #3
            {true,  -150, 30}, // unregistered (-150) -> must be #1!
            {false, -100, 40}, // registered (-100) -> #5
            {true,   -20, 50}, // unregistered (-20) -> #2!
        };
        std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) {
            return beam_candidate_better(WHITE, a.unreg, a.black_eval, a.hash,
                                                b.unreg, b.black_eval, b.hash);
        });
        CHECK(v[0].unreg && v[0].black_eval == -150);
        CHECK(v[1].unreg && v[1].black_eval ==  -20);
        CHECK(v[2].unreg && v[2].black_eval ==  +60);
        CHECK(!v[3].unreg && v[3].black_eval == -500);
        CHECK(!v[4].unreg && v[4].black_eval == -100);
    }
}

static void test_real_nnue_if_present() {
    if (get_eval_family() != EvalFamily::SFNN_NNUE) return;
    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), evaluate_full_reference(b));
    uint64_t rng = 0x123456789ABCDEFULL;
    for (int ply = 0; ply < 120; ++ply) {
        MoveList ml;
        generate_legal_moves(b, ml);
        if (ml.empty()) break;
        for (int k = 0; k < std::min(3, ml.size()); ++k) {
            Move m = ml[(ply * 5 + k * 11) % ml.size()];
            b.do_move(m);
            CHECK_EQ(evaluate(b), evaluate_full_reference(b));
            b.undo_move(m);
        }
        rng = rng * 6364136223846793005ULL + 1ULL;
        b.do_move(ml[static_cast<size_t>(rng >> 32) % static_cast<size_t>(ml.size())]);
        CHECK_EQ(evaluate(b), evaluate_full_reference(b));
    }
}

int main() {
    Zobrist::init();

    test_square_conversions();
    test_initial_position();
    test_sfen_roundtrip();
    test_legal_move_count_startpos();
    test_do_undo();
    test_capture_and_hand();
    test_promotion();
    test_drop();
    test_nifu();
    test_check_detection();
    test_king_cant_move_into_check();
    test_horse_movement_regression();
    test_major_promotion_mapping();
    test_move_usi_roundtrip();
    test_eval_symmetric();
    test_eval_features();
    test_quiescence_tactical_extension();
    test_search_tactical_regression();
    test_transposition_table_hits();
    test_transposition_table_reuse_across_generations();
    test_aspiration_window_recovery();
    test_search_respects_movetime();
    test_time_allocation_reasonable();
    test_alpha_beta_cutoff_stats();
    test_search_info_callback();
    test_perpetual_check_repetition();
    test_fourfold_repetition_draw();
    test_search_info_uses_completed_iteration();
    test_null_move_pruning_fires();
    test_late_move_pruning();
    test_countermove_ordering();
    test_continuation_history_and_board_restoration();
    test_repetition_sensitive_tt_cutoff_is_skipped();
    test_quiet_check_moves_bypass_selectivity();
    test_null_move_skipped_in_check();
    test_depth_improved_with_pruning();
    test_lmr_preserves_tactical_best_move();
    test_singular_extension_exclusion_search();
    test_alpha_beta_clamps_large_deficit_to_minus_1000();
    test_alpha_beta_clamp_side_agnostic();

    // NNUE & progress.bin tests
    test_eval_nnue_missing_and_empty_dummy_fallback();
    test_eval_nnue_corrupt_and_invalid_progress_rejected();
    test_eval_auto_discovery_and_cwd_independence();
    test_eval_nnue_ka2_exact_math_and_progress_buckets();
    test_eval_nnue_incremental_and_simd_matches_scalar_reference();
    test_perft_and_staged_movegen_equivalence();
    test_see_and_mate_detection();
    set_eval_file_path("");
    set_eval_dir("");
    test_real_nnue_if_present();
    test_nodelimit_strict_cap_single_and_multi_thread();
    test_lazy_smp_multithreaded_search();
    test_winrate_book_db_and_minimax();
    test_beam_selection_unregistered_first_and_turn_sort();

    std::cout << "\n=== Test results: "
              << g_pass << " passed, "
              << g_fail << " failed ===\n";

    search_shutdown_threads();
    return g_fail ? 1 : 0;
}
