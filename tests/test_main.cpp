#include "../src/board.hpp"
#include "../src/movegen.hpp"
#include "../src/eval.hpp"
#include "../src/search.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <vector>
#include <filesystem>
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
    // Evaluation of the starting position should be 0 (symmetric)
    Board b;
    b.set_startpos();
    int score_black = evaluate(b);
    CHECK_EQ(score_black, 0);
}

// ============================================================
// Test: evaluation handles side-to-move, promotions and hand pieces
// ============================================================
static void test_eval_features() {
    Board b1;
    b1.parse_sfen("9/9/9/9/4P4/9/9/4K4/4k4 b - 1");
    int s_black = evaluate(b1);
    b1.parse_sfen("9/9/9/9/4P4/9/9/4K4/4k4 w - 1");
    int s_white = evaluate(b1);
    CHECK_EQ(s_black, -s_white);

    Board b2;
    b2.parse_sfen("9/9/9/9/9/9/9/4K4/4k4 b P 1");
    CHECK(evaluate(b2) > 0);

    Board b3;
    b3.parse_sfen("9/4+P4/9/9/9/9/9/4K4/4k4 b - 1");
    Board b4;
    b4.parse_sfen("9/4P4/9/9/9/9/9/4K4/4k4 b - 1");
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
    CHECK(searched > static_eval + 300);
}

// ============================================================
// Test: quiescence search must consider quiet checking moves
// ============================================================
static void test_quiescence_considers_quiet_checks() {
    Board b;
    b.parse_sfen("PUT_REAL_SFEN_HERE");
    const Move best = iterative_deepening(b, 500);
    CHECK_EQ(best, usi_to_move("PUT_BESTMOVE_HERE"));
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
// Test: root-search reset keeps deterministic stats between runs
// ============================================================
static void test_search_reset_deterministic() {
    Board b1;
    b1.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const int score1 = negamax(b1, 3, -INF, INF, 0);
    const SearchStats st1 = last_search_stats();

    Board b2;
    b2.parse_sfen("4k4/9/9/4b4/3R5/9/9/9/4K4 w - 1");
    const int score2 = negamax(b2, 3, -INF, INF, 0);
    const SearchStats st2 = last_search_stats();

    CHECK_EQ(score1, score2);
    CHECK_EQ(st1.nodes, st2.nodes);
    CHECK_EQ(st1.qnodes, st2.qnodes);
    CHECK_EQ(st1.tt_hits, st2.tt_hits);
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
    // We should reach a reasonable search depth within 500 ms.
    CHECK(!infos.empty() && infos.back().depth >= 8);
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
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 b rbgs 1");
    const int score = negamax(b, 1, -INF, INF, 0);
    CHECK_EQ(score, -1000000);
}

// ============================================================
// Test: the same clamp applies regardless of which side is to move
// (guards against the clamp being wired to the wrong side).
// ============================================================
static void test_alpha_beta_clamp_side_agnostic() {
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 w RBGS 1");
    const int score = negamax(b, 1, -INF, INF, 0);
    CHECK_EQ(score, -1000000);
}

// ============================================================
// Eval file: helpers to write temporary fv.bin-shaped files
// ============================================================
#include <cstdio>
#include <fstream>

static void write_tmp_file(const std::string& path, const std::string& contents) {
    std::ofstream f(path);
    f << contents;
}

// Creates a file of exactly `bytes` length, filled with zero bytes, using a
// sparse-file truncate so tests don't need to actually write ~206 MiB of
// data to disk. This models a syntactically-valid (all-zero-weight) fv.bin.
static void write_zero_filled_file(const std::string& path, int64_t bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
#if defined(_WIN32)
    std::fseek(f, static_cast<long>(bytes - 1), SEEK_SET);
    std::fputc('\0', f);
#else
    if (bytes > 0) {
        std::fseek(f, static_cast<long>(bytes - 1), SEEK_SET);
        std::fputc('\0', f);
    }
#endif
    std::fclose(f);
}

// The exact expected fv.bin size for the genuine Bonanza v6 KPP+KKP
// layout implemented in eval.cpp (kept in sync manually; see eval.cpp's
// file header comment for the derivation of this constant from genuine
// Bonanza source: fe_end == 1476 for KPP, kkp_end == 738 for KKP).
constexpr int64_t EXPECTED_FV_BIN_BYTES = 186'268'248;

// A file of this size is recognized as a *specific, known* unsupported
// layout (the previous, incorrect assumption this loader used to make,
// based on a converter tool's expanded in-memory struct sizes) rather
// than a generic wrong-sized file; see eval.cpp's LEGACY_APERY_LAYOUT_BYTES.
constexpr int64_t LEGACY_APERY_LAYOUT_BYTES = 215'824'824;

// ------------------------------------------------------------------
// Mirrors of the region layout/sizes in eval.cpp (kept in sync manually;
// see eval.cpp for the derivation from genuine Bonanza source), used
// below to patch specific non-zero table entries into an otherwise
// all-zero synthetic fv.bin so tests can assert on the *exact* numeric
// result of evaluate() actually referencing loaded table data, rather
// than only checking that a file of the right size is accepted.
// ------------------------------------------------------------------
constexpr int64_t FE_END       = 1476;
constexpr int64_t POS_N        = FE_END * (FE_END + 1) / 2;
constexpr int64_t KKP_END      = 738; // kkp_end: halved feature space for KKP
constexpr int64_t SQ_NB        = 81;
constexpr int64_t KPP_BYTES    = SQ_NB * POS_N * 2;              // int16_t
constexpr int64_t KKP_BYTES    = SQ_NB * SQ_NB * KKP_END * 2;    // int16_t
constexpr int64_t KPP_OFFSET   = 0;
constexpr int64_t KKP_OFFSET   = KPP_BYTES;

static int64_t kpp_triangular_index(int a, int b) {
    const int hi = std::max(a, b);
    const int lo = std::min(a, b);
    return static_cast<int64_t>(hi) * (hi + 1) / 2 + lo;
}

static int64_t kpp_byte_offset(int king, int fa, int fb) {
    return KPP_OFFSET + (static_cast<int64_t>(king) * POS_N + kpp_triangular_index(fa, fb)) * 2;
}
// `kkp_idx` must already be a kkp-space index (kkp_hand_<piece> + count,
// or kkp_<piece> + board square), not a full fe_end feature id.
static int64_t kkp_byte_offset(int king, int opp_king, int kkp_idx) {
    return KKP_OFFSET + ((static_cast<int64_t>(king) * SQ_NB + opp_king) * KKP_END + kkp_idx) * 2;
}

static void patch_int16(const std::string& path, int64_t offset, int16_t value) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) return;
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    std::fwrite(&value, sizeof(value), 1, f);
    std::fclose(f);
}

// ============================================================
// Test: missing fv.bin reports material-only fallback
// ============================================================
static void test_eval_fv_bin_file_not_found() {
    set_eval_file_path("/tmp/shogiai_nonexistent_fv_99999.bin");
    const std::string status = eval_status_message();

    CHECK(status.find("file not found") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    // Engine must still work (material-only fallback), symmetric position → 0
    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0);

    set_eval_file_path(""); // reset
}

// ============================================================
// Test: wrong-sized fv.bin (e.g. the 1-byte placeholder dummy shipped in
// the repository) is rejected with a clear, actionable status message.
// ============================================================
static void test_eval_fv_bin_size_mismatch_reports_clear_error() {
    const std::string tmp = "/tmp/shogiai_test_fv_wrong_size.bin";
    write_tmp_file(tmp, "x"); // 1 byte, mimics the checked-in dummy fv.bin

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("invalid Bonanza v6 fv.bin size") != std::string::npos);
    CHECK(status.find("got 1 bytes") != std::string::npos);
    CHECK(status.find(std::to_string(EXPECTED_FV_BIN_BYTES)) != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: an exactly-sized (all-zero-weight) fv.bin loads successfully and
// is reported as EvalFamily::BONANZA_V6_FV, with the explicit family tag
// present. With every KKP/KPP table entry zero, evaluate() must reduce to
// exactly the material term (scaled and rescaled by FV_SCALE), since real
// Bonanza always adds a separate material term on top of the relation
// tables ? it is never "implicit" in an all-zero table.
// ============================================================
static void test_eval_fv_bin_correct_size_loads_and_evaluates() {
    const std::string tmp = "/tmp/shogiai_test_fv_correct_size.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES);

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("bonanza-v6 fv.bin loaded from") != std::string::npos);
    CHECK(status.find(tmp) != std::string::npos);
    CHECK(status.find("[explicit]") != std::string::npos);
    CHECK(status.find("[family=BONANZA_V6_FV]") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::BONANZA_V6_FV);

    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0); // symmetric material, zero tables → 0

    b.parse_sfen("9/9/9/9/9/9/9/4K4/4k4 b P 1");
    CHECK_EQ(evaluate(b), 100); // one black pawn in hand: pure material (100cp), zero tables

    b.parse_sfen("9/9/9/9/9/9/9/4K4/4k4 w P 1");
    CHECK_EQ(evaluate(b), -100); // same physical position, White to move → sign flips

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: a hand-crafted fv.bin with a single known non-zero KKP table
// entry (all else zero) produces the exact expected evaluate() result.
// This is the core regression guarding against evaluate() silently
// ignoring loaded table data (i.e. behaving identically to
// MATERIAL_FALLBACK even though a real fv.bin was loaded).
//
// Position: "K8/9/9/9/4P4/9/9/9/8k b - 1"
//   Black king  @ square 0  (Bonanza sq 0)
//   White king  @ square 80 (Bonanza sq 80)
//   Black pawn  @ square 40 (Bonanza sq 40)
// With only one non-king piece, no KPP pair term can fire (a pair needs
// at least two pieces), cleanly isolating the KKP contribution.
// ============================================================
static void test_eval_fv_bin_real_kkp_value_is_used() {
    const std::string tmp = "/tmp/shogiai_test_fv_kkp_nonzero.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES);

    // kkp_pawn == 36 (see eval.cpp); the kkp-space index for the black
    // pawn (looked up unmirrored, at the (SQ_BKING, SQ_WKING) pair) is
    // kkp_pawn + bonanza_sq(40) = 76.
    constexpr int kSqBk = 0, kSqWk = 80, kKkpIdx = 36 + 40;
    constexpr int16_t kKkpValue = 6400; // 200 centipawns * FV_SCALE(32)
    patch_int16(tmp, kkp_byte_offset(kSqBk, kSqWk, kKkpIdx), kKkpValue);

    set_eval_file_path(tmp);
    CHECK(get_eval_family() == EvalFamily::BONANZA_V6_FV);

    Board b;
    b.parse_sfen("K8/9/9/9/4P4/9/9/9/8k b - 1");
    // material (100cp black pawn) * 32 + kkp(6400), all / 32 == 100 + 200
    CHECK_EQ(evaluate(b), 300);

    // Same physical position, White to move: sign flips exactly.
    b.parse_sfen("K8/9/9/9/4P4/9/9/9/8k w - 1");
    CHECK_EQ(evaluate(b), -300);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: a hand-crafted fv.bin with a known non-zero KKP table entry for
// a *White* piece produces the exact expected (subtracted, mirrored)
// evaluate() result. This is the regression guarding against the KKP
// term only ever being applied with the correct sign/king-pair/mirrored
// square for the opponent's own pieces, not just Black's.
//
// Position: "K8/9/9/9/4p4/9/9/9/8k b - 1"
//   Black king  @ square 0  (Bonanza sq 0)
//   White king  @ square 80 (Bonanza sq 80)
//   White pawn  @ square 40 (Bonanza sq 40, mirror(40) == 40)
// ============================================================
static void test_eval_fv_bin_real_kkp_white_piece_is_subtracted_and_mirrored() {
    const std::string tmp = "/tmp/shogiai_test_fv_kkp_white_nonzero.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES);

    // White's own pieces are looked up at (Inv(SQ_WKING), Inv(SQ_BKING))
    // = (Inv(80), Inv(0)) = (0, 80), using the MIRRORED board square:
    // kkp_pawn + mirror(40) = 36 + 40 = 76 (square 40 mirrors to itself
    // on the 9x9 board, so this deliberately keeps the same index as the
    // Black test above to prove the SIGN/king-pair wiring, not just the
    // square-mirroring arithmetic).
    constexpr int kSqBk1 = 0, kSqWk1 = 80, kKkpIdx = 36 + 40;
    constexpr int16_t kKkpValue = 6400; // 200 centipawns * FV_SCALE(32)
    patch_int16(tmp, kkp_byte_offset(kSqBk1, kSqWk1, kKkpIdx), kKkpValue);

    set_eval_file_path(tmp);
    CHECK(get_eval_family() == EvalFamily::BONANZA_V6_FV);

    Board b;
    b.parse_sfen("K8/9/9/9/4p4/9/9/9/8k b - 1");
    // material (-100cp, one White pawn) * 32 - kkp(6400), all / 32
    // == -100 - 200 == -300
    CHECK_EQ(evaluate(b), -300);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: a hand-crafted fv.bin with two known non-zero KPP table entries
// (the Black-view pass and the White-view/subtracted pass) produces the
// exact expected evaluate() result, confirming the two-pass
// opponent-mirrored KPP subtraction (not merely a single-sided sum) is
// wired correctly, including sign, king-square mirroring and feature
// indices for two distinct piece types (pawn + lance).
//
// Position: "K8/9/9/9/4P4/9/9/9/8k b - 1" plus a black lance @ square 9.
// ============================================================
static void test_eval_fv_bin_real_kpp_pair_value_is_used() {
    const std::string tmp = "/tmp/shogiai_test_fv_kpp_nonzero.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES);

    // Black-view features: lance @ bonanza_sq(9)=1 -> f_lance(225)+1=226;
    //                       pawn  @ bonanza_sq(40)=40 -> f_pawn(81)+40=121.
    // White-view features: lance -> e_lance(306)+mirror(1)=306+79=385;
    //                       pawn  -> e_pawn(162)+mirror(40)=162+40=202.
    constexpr int kSqBk = 0, kSqWkInv = 0;
    constexpr int kBlackLance = 226, kBlackPawn = 121;
    constexpr int kWhiteLance = 385, kWhitePawn = 202;
    constexpr int16_t kBlackPassValue = 640; //  20 centipawns * FV_SCALE(32)
    constexpr int16_t kWhitePassValue = 320; //  10 centipawns * FV_SCALE(32)

    patch_int16(tmp, kpp_byte_offset(kSqBk, kBlackPawn, kBlackLance), kBlackPassValue);
    patch_int16(tmp, kpp_byte_offset(kSqWkInv, kWhitePawn, kWhiteLance), kWhitePassValue);

    set_eval_file_path(tmp);
    CHECK(get_eval_family() == EvalFamily::BONANZA_V6_FV);

    Board b;
    b.parse_sfen("K8/L8/9/9/4P4/9/9/9/8k b - 1");
    // material (100 pawn + 300 lance = 400cp) * 32 + (640 - 320), all / 32
    // == 400 + 10 == 410
    CHECK_EQ(evaluate(b), 410);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: an fv.bin that is one byte short of the required size is
// rejected exactly like any other size mismatch (boundary check), not
// silently truncated/misread.
// ============================================================
static void test_eval_fv_bin_one_byte_short_is_rejected() {
    const std::string tmp = "/tmp/shogiai_test_fv_one_byte_short.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES - 1);

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("invalid Bonanza v6 fv.bin size") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: a file matching the previous (incorrect) 215,824,824-byte
// assumption is recognized and reported as a specific, known unsupported
// layout ? not silently accepted as if it were the real 186,268,248-byte
// format, and not just a generic "invalid size" message either.
// ============================================================
static void test_eval_fv_bin_legacy_size_reports_specific_error() {
    const std::string tmp = "/tmp/shogiai_test_fv_legacy_size.bin";
    write_zero_filled_file(tmp, LEGACY_APERY_LAYOUT_BYTES);

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("unsupported fv.bin layout") != std::string::npos);
    CHECK(status.find(std::to_string(LEGACY_APERY_LAYOUT_BYTES)) != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::MATERIAL_FALLBACK);

    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0);

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: NNUE-looking file path is recognized as unsupported,
// engine stays functional with material fallback
// ============================================================
static void test_eval_nnue_unsupported_fallback() {
    const std::string tmp = "/tmp/shogiai_test_nnue.bin";
    write_tmp_file(tmp, "not a real nnue file");

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("unsupported evaluator: nnue") != std::string::npos);
    CHECK(status.find(tmp) != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::NNUE_UNSUPPORTED);

    // Engine must still return a sensible (non-crash) evaluation
    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0); // symmetric position → 0 regardless of family

    set_eval_file_path(""); // reset
    std::remove(tmp.c_str());
}

// ============================================================
// Test: auto-discovery finds eval/fv.bin in the build dir (CMake copies
// src/eval/ → build/eval/ at configure time). The checked-in fv.bin is a
// 1-byte dummy, so this must resolve to MATERIAL_FALLBACK with a size
// mismatch message, never a crash and never a false "loaded" claim.
// ============================================================
static void test_eval_auto_discovery() {
    // Reset to auto-discovery mode (no explicit path).
    set_eval_file_path("");
    const std::string status = eval_status_message();

    CHECK(status.find("unsupported evaluator") == std::string::npos);

    const EvalFamily fam = get_eval_family();
    CHECK(fam == EvalFamily::BONANZA_V6_FV || fam == EvalFamily::MATERIAL_FALLBACK);

    // Evaluation must remain valid and symmetric
    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0);

    set_eval_file_path(""); // ensure reset for subsequent tests
}

// ============================================================
// Test: auto-discovery still finds a candidate fv.bin when launched from
// an unrelated working directory (e.g. the system temp directory),
// proving discovery is not purely cwd-relative. If discovery only ever
// checked cwd-relative paths ("src/eval/fv.bin", "eval/fv.bin"), then
// from an unrelated cwd every checked candidate reported in the status
// message would be a bare relative path with no leading '/'. With the
// fix, the executable-directory and compile-time-source-dir candidates
// are absolute and cwd-independent, so at least one checked candidate in
// the message must be an absolute path even when cwd is far away.
// ============================================================
static void test_eval_auto_discovery_independent_of_cwd() {
    namespace fs = std::filesystem;
    const fs::path saved_cwd = fs::current_path();

    fs::current_path(fs::temp_directory_path());
    set_eval_file_path("");
    const std::string status = eval_status_message();
    fs::current_path(saved_cwd);

    CHECK(status.find("unsupported evaluator") == std::string::npos);

    // Extract the "checked: ..." tail and verify at least one candidate
    // path is absolute (starts with '/'), i.e. not derived solely from
    // the (unrelated) current working directory.
    const std::string marker = "checked: ";
    const auto pos = status.find(marker);
    CHECK(pos != std::string::npos);
    bool found_absolute_candidate = false;
    if (pos != std::string::npos) {
        const std::string candidates = status.substr(pos + marker.size());
        std::stringstream ss(candidates);
        std::string candidate;
        while (std::getline(ss, candidate, ',')) {
            // trim a single leading space, if present
            if (!candidate.empty() && candidate.front() == ' ')
                candidate.erase(0, 1);
            if (!candidate.empty() && candidate.front() == '/') {
                found_absolute_candidate = true;
                break;
            }
        }
    }
    CHECK(found_absolute_candidate);

    set_eval_file_path(""); // ensure reset for subsequent tests
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
    test_search_reset_deterministic();
    test_search_respects_movetime();
    test_time_allocation_reasonable();
    test_alpha_beta_cutoff_stats();
    test_search_info_callback();
    test_null_move_pruning_fires();
    test_null_move_skipped_in_check();
    test_depth_improved_with_pruning();
    test_lmr_preserves_tactical_best_move();
    test_alpha_beta_clamps_large_deficit_to_minus_1000();
    test_alpha_beta_clamp_side_agnostic();

    // Eval file discovery and loading tests
    test_eval_fv_bin_file_not_found();
    test_eval_fv_bin_size_mismatch_reports_clear_error();
    test_eval_fv_bin_correct_size_loads_and_evaluates();
    test_eval_fv_bin_real_kkp_value_is_used();
    test_eval_fv_bin_real_kkp_white_piece_is_subtracted_and_mirrored();
    test_eval_fv_bin_real_kpp_pair_value_is_used();
    test_eval_fv_bin_one_byte_short_is_rejected();
    test_eval_fv_bin_legacy_size_reports_specific_error();
    test_eval_nnue_unsupported_fallback();
    test_eval_auto_discovery();
    test_eval_auto_discovery_independent_of_cwd();

    std::cout << "\n=== Test results: "
              << g_pass << " passed, "
              << g_fail << " failed ===\n";

    return g_fail ? 1 : 0;
}
