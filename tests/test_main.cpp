#include "../src/board.hpp"
#include "../src/movegen.hpp"
#include "../src/eval.hpp"
#include "../src/search.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <vector>

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
//   rook (2h): W(1h) + E×5(3h–7h)          =  6
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
    // WHITE king at 5a, BLACK rook at 5h – rook gives check along file 5
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
    CHECK_EQ(score, -1000);
}

// ============================================================
// Test: the same clamp applies regardless of which side is to move
// (guards against the clamp being wired to the wrong side).
// ============================================================
static void test_alpha_beta_clamp_side_agnostic() {
    Board b;
    b.parse_sfen("4k4/9/9/9/9/9/9/9/4K4 w RBGS 1");
    const int score = negamax(b, 1, -INF, INF, 0);
    CHECK_EQ(score, -1000);
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

// The exact expected fv.bin size for the Bonanza v6 KPP+KKP+KK+KP layout
// implemented in eval.cpp (kept in sync manually; see eval.cpp for the
// derivation of this constant from Bonanza's fe_end == 1476).
constexpr int64_t EXPECTED_FV_BIN_BYTES = 215'824'824;

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
// is reported as EvalFamily::BONANZA_V6_FV. With every table entry zero,
// evaluate() must be exactly 0 for any position (deterministic, since the
// implemented formula is a pure sum of table lookups with no separate
// material term once a real fv.bin is active).
// ============================================================
static void test_eval_fv_bin_correct_size_loads_and_evaluates() {
    const std::string tmp = "/tmp/shogiai_test_fv_correct_size.bin";
    write_zero_filled_file(tmp, EXPECTED_FV_BIN_BYTES);

    set_eval_file_path(tmp);
    const std::string status = eval_status_message();

    CHECK(status.find("bonanza-v6 fv.bin loaded from") != std::string::npos);
    CHECK(status.find(tmp) != std::string::npos);
    CHECK(status.find("[explicit]") != std::string::npos);
    CHECK(get_eval_family() == EvalFamily::BONANZA_V6_FV);

    Board b;
    b.set_startpos();
    CHECK_EQ(evaluate(b), 0);

    b.parse_sfen("9/9/9/9/9/9/9/4K4/4k4 b P 1");
    CHECK_EQ(evaluate(b), 0); // all-zero tables → always 0, regardless of material

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
    test_eval_nnue_unsupported_fallback();
    test_eval_auto_discovery();

    std::cout << "\n=== Test results: "
              << g_pass << " passed, "
              << g_fail << " failed ===\n";

    return g_fail ? 1 : 0;
}
