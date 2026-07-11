#include "movegen.hpp"

// ============================================================
// Helper predicates
// ============================================================

// Is rank r inside the promotion zone for color c?
static inline bool in_promo_zone(Color c, int r) {
    return (c == BLACK) ? (r <= 2) : (r >= 6);
}

// Must a piece of type pt for color c promote if it lands on rank nr?
// (A piece must promote when it would otherwise have no legal moves.)
static inline bool must_promote(PieceType pt, Color c, int nr) {
    if (c == BLACK) {
        if (pt == PAWN || pt == LANCE)  return nr == 0;
        if (pt == KNIGHT)               return nr <= 1;
    } else {
        if (pt == PAWN || pt == LANCE)  return nr == 8;
        if (pt == KNIGHT)               return nr >= 7;
    }
    return false;
}

// ============================================================
// Low-level move adders (push one or two moves for a target sq)
// ============================================================

static void add_to(MoveList& ml, Square from, Square to,
                   PieceType pt, Color us, int from_rank) {
    bool to_pz   = in_promo_zone(us, rank_of(to));
    bool from_pz = in_promo_zone(us, from_rank);
    bool must_p  = must_promote(pt, us, rank_of(to));

    if (must_p) {
        // Only promotion is legal (piece would be stuck otherwise)
        ml.push(make_promote_move(from, to));
    } else if (is_promotable(pt) && (to_pz || from_pz)) {
        // Promotion is optional; offer both
        ml.push(make_normal_move (from, to));
        ml.push(make_promote_move(from, to));
    } else {
        ml.push(make_normal_move(from, to));
    }
}

// ============================================================
// Step move generator (one fixed direction)
// ============================================================
static void gen_step(const Board& b, MoveList& ml,
                     Square from, int df, int dr,
                     PieceType pt, Color us) {
    int nf = file_of(from) + df;
    int nr = rank_of(from) + dr;
    if (!on_board(nf, nr)) return;
    Square to = make_sq(nf, nr);
    Piece  tgt = b.piece_at(to);
    if (tgt != NO_PIECE && color_of(tgt) == us) return;  // own piece
    add_to(ml, from, to, pt, us, rank_of(from));
}

// ============================================================
// Sliding move generator (one direction, multiple steps)
// ============================================================
static void gen_slide(const Board& b, MoveList& ml,
                      Square from, int df, int dr,
                      PieceType pt, Color us) {
    const int r0 = rank_of(from);
    for (int nf = file_of(from) + df, nr = r0 + dr;
         on_board(nf, nr); nf += df, nr += dr) {
        Square to  = make_sq(nf, nr);
        Piece  tgt = b.piece_at(to);
        if (tgt != NO_PIECE && color_of(tgt) == us) break;  // own piece – stop
        add_to(ml, from, to, pt, us, r0);
        if (tgt != NO_PIECE) break;  // enemy piece – capture and stop
    }
}

// ============================================================
// Pseudo-legal move generator for a single piece
// ============================================================
static void gen_piece(const Board& b, MoveList& ml, Square from, Color us) {
    Piece     p  = b.piece_at(from);
    PieceType pt = type_of(p);
    int fwd      = (us == BLACK) ? -1 : 1;

    switch (pt) {
        case PAWN:
            gen_step(b, ml, from, 0, fwd, PAWN, us);
            break;

        case LANCE:
            gen_slide(b, ml, from, 0, fwd, LANCE, us);
            break;

        case KNIGHT:
            gen_step(b, ml, from, -1, 2*fwd, KNIGHT, us);
            gen_step(b, ml, from,  1, 2*fwd, KNIGHT, us);
            break;

        case SILVER:
            gen_step(b, ml, from,  0,  fwd, SILVER, us);
            gen_step(b, ml, from,  1,  fwd, SILVER, us);
            gen_step(b, ml, from, -1,  fwd, SILVER, us);
            gen_step(b, ml, from,  1, -fwd, SILVER, us);
            gen_step(b, ml, from, -1, -fwd, SILVER, us);
            break;

        case GOLD:
        case PROM_PAWN: case PROM_LANCE: case PROM_KNIGHT: case PROM_SILVER:
            gen_step(b, ml, from,  0,  fwd, pt, us);
            gen_step(b, ml, from,  1,  fwd, pt, us);
            gen_step(b, ml, from, -1,  fwd, pt, us);
            gen_step(b, ml, from,  1,    0, pt, us);
            gen_step(b, ml, from, -1,    0, pt, us);
            gen_step(b, ml, from,  0, -fwd, pt, us);
            break;

        case BISHOP:
            gen_slide(b, ml, from,  1,  1, BISHOP, us);
            gen_slide(b, ml, from,  1, -1, BISHOP, us);
            gen_slide(b, ml, from, -1,  1, BISHOP, us);
            gen_slide(b, ml, from, -1, -1, BISHOP, us);
            break;

        case ROOK:
            gen_slide(b, ml, from,  0,  1, ROOK, us);
            gen_slide(b, ml, from,  0, -1, ROOK, us);
            gen_slide(b, ml, from,  1,  0, ROOK, us);
            gen_slide(b, ml, from, -1,  0, ROOK, us);
            break;

        case PROM_BISHOP:          // diagonal slides + 1-step orthogonal
            gen_slide(b, ml, from,  1,  1, PROM_BISHOP, us);
            gen_slide(b, ml, from,  1, -1, PROM_BISHOP, us);
            gen_slide(b, ml, from, -1,  1, PROM_BISHOP, us);
            gen_slide(b, ml, from, -1, -1, PROM_BISHOP, us);
            gen_step (b, ml, from,  0,  1, PROM_BISHOP, us);
            gen_step (b, ml, from,  0, -1, PROM_BISHOP, us);
            gen_step (b, ml, from,  1,  0, PROM_BISHOP, us);
            gen_step (b, ml, from, -1,  0, PROM_BISHOP, us);
            break;

        case PROM_ROOK:            // orthogonal slides + 1-step diagonal
            gen_slide(b, ml, from,  0,  1, PROM_ROOK, us);
            gen_slide(b, ml, from,  0, -1, PROM_ROOK, us);
            gen_slide(b, ml, from,  1,  0, PROM_ROOK, us);
            gen_slide(b, ml, from, -1,  0, PROM_ROOK, us);
            gen_step (b, ml, from,  1,  1, PROM_ROOK, us);
            gen_step (b, ml, from,  1, -1, PROM_ROOK, us);
            gen_step (b, ml, from, -1,  1, PROM_ROOK, us);
            gen_step (b, ml, from, -1, -1, PROM_ROOK, us);
            break;

        case KING:
            for (int df = -1; df <= 1; df++)
                for (int dr = -1; dr <= 1; dr++)
                    if (df || dr) gen_step(b, ml, from, df, dr, KING, us);
            break;

        default: break;
    }
}

// ============================================================
// Drop move generator
// ============================================================
static void gen_drops(const Board& b, MoveList& ml, Color us) {
    // Determine which piece types are in hand
    bool have[PT_NB] = {};
    for (int pt = PAWN; pt <= ROOK; pt++)
        if (b.hand(us, static_cast<PieceType>(pt)) > 0)
            have[pt] = true;
    if (!have[PAWN] && !have[LANCE] && !have[KNIGHT] &&
        !have[SILVER] && !have[GOLD] && !have[BISHOP] && !have[ROOK]) return;

    // Nifu check: which files already have our unpromoted pawn?
    bool pawn_file[FILE_NB] = {};
    if (have[PAWN]) {
        Piece bp = make_piece(us, PAWN);
        for (int s = 0; s < SQUARE_NB; s++)
            if (b.piece_at(s) == bp) pawn_file[file_of(s)] = true;
    }

    for (int to = 0; to < SQUARE_NB; to++) {
        if (b.piece_at(to) != NO_PIECE) continue;  // must be empty
        int r = rank_of(to), f = file_of(to);

        for (int ipt = PAWN; ipt <= ROOK; ipt++) {
            if (!have[ipt]) continue;
            PieceType pt = static_cast<PieceType>(ipt);

            // Zone restrictions
            if (pt == PAWN || pt == LANCE) {
                if ((us == BLACK && r == 0) || (us == WHITE && r == 8)) continue;
            }
            if (pt == KNIGHT) {
                if ((us == BLACK && r <= 1) || (us == WHITE && r >= 7)) continue;
            }
            // Nifu
            if (pt == PAWN && pawn_file[f]) continue;

            ml.push(make_drop_move(pt, to));
        }
    }
}

// ============================================================
// generate_pseudo_legal  (internal)
// ============================================================
static void gen_pseudo(const Board& b, MoveList& ml) {
    Color us = b.side_to_move();
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = b.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(b, ml, s, us);
    }
    gen_drops(b, ml, us);
}

// ============================================================
// generate_legal_moves
//   Filters pseudo-legal list to moves that do not leave own
//   king in check, and enforces the pawn-drop-mate rule.
// ============================================================
void generate_legal_moves(Board& board, MoveList& out) {
    MoveList pseudo;
    gen_pseudo(board, pseudo);

    Color us = board.side_to_move();

    for (Move m : pseudo) {
        board.do_move(m);

        // Own king must not be in check after the move
        bool legal = !board.is_attacked(board.king_sq(us), ~us);

        // Pawn-drop checkmate is illegal
        if (legal && is_drop(m) && dropped_pt(m) == PAWN) {
            // The opponent must be in check first (otherwise no mate)
            if (board.in_check()) {
                // Check whether the opponent has any escape
                MoveList opp;
                // Use a simple pseudo-legal + filter pass (no recursive PDM check)
                gen_pseudo(board, opp);
                Color them = board.side_to_move(); // after do_move it's their turn
                bool has_escape = false;
                for (Move om : opp) {
                    board.do_move(om);
                    if (!board.is_attacked(board.king_sq(them), ~them))
                        has_escape = true;
                    board.undo_move(om);
                    if (has_escape) break;
                }
                if (!has_escape) legal = false;  // pawn-drop-mate: illegal
            }
        }

        board.undo_move(m);
        if (legal) out.push(m);
    }
}

bool is_checkmate(Board& board) {
    if (!board.in_check()) return false;
    MoveList ml;
    generate_legal_moves(board, ml);
    return ml.empty();
}

bool has_no_moves(Board& board) {
    MoveList ml;
    generate_legal_moves(board, ml);
    return ml.empty();
}
