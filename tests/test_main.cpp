#include "../src/board.hpp"
#include "../src/movegen.hpp"
#include "../src/eval.hpp"
#include "../src/search.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <chrono>

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
    test_move_usi_roundtrip();
    test_eval_symmetric();
    test_search_respects_movetime();
    test_time_allocation_reasonable();

    std::cout << "\n=== Test results: "
              << g_pass << " passed, "
              << g_fail << " failed ===\n";

    return g_fail ? 1 : 0;
}
