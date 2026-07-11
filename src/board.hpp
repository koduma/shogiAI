#pragma once
#include "types.hpp"
#include <vector>
#include <string>

// ============================================================
// Undo information saved before each do_move
// ============================================================
struct StateInfo {
    Piece    captured;   // piece that was on the destination square (NO_PIECE if none)
    uint64_t hash;       // Zobrist hash *before* the move
};

// ============================================================
// Board
// ============================================================
class Board {
public:
    Piece    board_[SQUARE_NB];            // piece at each square
    int      hand_ [COLOR_NB][PT_NB];     // hand[color][piece_type] = count
    Color    stm_;                         // side to move
    int      ply_;                         // half-moves played so far
    Square   king_sq_[COLOR_NB];          // cached king positions
    uint64_t hash_;                        // current Zobrist hash

    std::vector<StateInfo> history_;       // undo stack
    std::vector<uint64_t>  pos_hashes_;   // all hashes seen (for repetition)

    Board();

    // Position setup
    void reset      ();
    void set_startpos();
    bool parse_sfen (const std::string& sfen);
    std::string to_sfen() const;

    // Move execution
    void do_move  (Move m);
    void undo_move(Move m);

    // Queries
    Piece    piece_at    (Square s)  const { return board_[s]; }
    bool     is_empty    (Square s)  const { return board_[s] == NO_PIECE; }
    Color    side_to_move()          const { return stm_; }
    int      hand        (Color c, PieceType pt) const { return hand_[c][pt]; }
    Square   king_sq     (Color c)   const { return king_sq_[c]; }

    // Attack / check detection
    bool is_attacked(Square sq, Color by) const;
    bool in_check   ()                    const;

    // Repetition
    int  repetition_count() const;

    // Debug
    void print() const;

private:
    void compute_hash();
};

// ============================================================
// Zobrist hashing
// ============================================================
namespace Zobrist {
    constexpr int MAX_HAND = 20;  // max pieces of one type in hand

    extern uint64_t piece_sq[PIECE_NB][SQUARE_NB];
    extern uint64_t hand   [COLOR_NB][PT_NB][MAX_HAND + 1];
    extern uint64_t side;  // XOR this when it is WHITE's turn

    void init();
}
