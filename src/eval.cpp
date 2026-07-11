#include "eval.hpp"

// ============================================================
// Piece values (centipawns)
// ============================================================
static const int PIECE_VALUE[PT_NB] = {
    0,      // NO_PT
    100,    // PAWN
    300,    // LANCE
    300,    // KNIGHT
    500,    // SILVER
    600,    // GOLD
    800,    // BISHOP
    1000,   // ROOK
    0,      // KING  (infinite in practice; handled separately)
    600,    // PROM_PAWN   (= Gold)
    600,    // PROM_LANCE  (= Gold)
    600,    // PROM_KNIGHT (= Gold)
    600,    // PROM_SILVER (= Gold)
    1100,   // PROM_BISHOP (Horse)
    1300,   // PROM_ROOK   (Dragon)
};

// Hand pieces are slightly less valuable than on-board pieces
// (they still need to be dropped), but we use the same values for simplicity.

int evaluate(const Board& board) {
    int score = 0;
    Color us = board.side_to_move();

    // Board material
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE) continue;
        int v = PIECE_VALUE[type_of(p)];
        score += (color_of(p) == us) ? v : -v;
    }

    // Hand material
    for (int pt = PAWN; pt <= ROOK; pt++) {
        int v = PIECE_VALUE[pt];
        score += board.hand(us,  static_cast<PieceType>(pt)) * v;
        score -= board.hand(~us, static_cast<PieceType>(pt)) * v;
    }

    return score;
}
