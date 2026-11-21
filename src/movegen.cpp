#include "movegen.hpp"
#include <algorithm>
#include <cstdlib>

// ============================================================
// Helper predicates
// ============================================================

static inline bool in_promo_zone(Color c, int r) {
    return (c == BLACK) ? (r <= 2) : (r >= 6);
}

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

static inline bool can_drop_on_rank(PieceType pt, Color c, int r) {
    if (pt == PAWN || pt == LANCE)
        return (c == BLACK) ? (r > 0) : (r < 8);
    if (pt == KNIGHT)
        return (c == BLACK) ? (r > 1) : (r < 7);
    return true;
}

static inline int sign_int(int x) { return (x > 0) - (x < 0); }

// Does moving a friendly non-king piece from `from` to `to` expose `ksq` to a
// sliding check along the ray from `ksq` through `from`?
// Precondition: `ksq` is NOT currently in check!
static inline bool exposes_king_along_ray(const Board& b, Color us, Square ksq, Square from, Square to) {
    if (ksq == SQ_NONE) return false;
    const int kf = file_of(ksq), kr = rank_of(ksq);
    const int df = file_of(from) - kf;
    const int dr = rank_of(from) - kr;

    // Off-ray moves can never uncover a sliding attack on `ksq`.
    if (df != 0 && dr != 0 && std::abs(df) != std::abs(dr))
        return false;

    const int sf = sign_int(df);
    const int sr = sign_int(dr);

    // If `to` stays on the exact same half-ray from `ksq` (either sliding along
    // the pin ray or capturing the pinning piece), `ksq` stays shielded.
    const int tdf = file_of(to) - kf;
    const int tdr = rank_of(to) - kr;
    if (tdf * sr == tdr * sf && (tdf * sf > 0 || tdr * sr > 0)) {
        if (sf == 0 || sr == 0 || std::abs(tdf) == std::abs(tdr))
            return false;
    }

    // Verify no blocker lies strictly between `ksq` and `from`.
    for (int nf = kf + sf, nr = kr + sr; ; nf += sf, nr += sr) {
        Square s = make_sq(nf, nr);
        if (s == from) break;
        if (b.piece_at(s) != NO_PIECE) return false;
    }

    // Scan beyond `from` for an enemy slider attacking along (-sf, -sr).
    const Color them = ~us;
    const int them_fwd = (them == BLACK) ? -1 : 1;
    const bool diag = (sf != 0 && sr != 0);
    const bool lance_ray = (sf == 0 && sr == -them_fwd);

    for (int nf = file_of(from) + sf, nr = rank_of(from) + sr;
         on_board(nf, nr); nf += sf, nr += sr) {
        Piece p = b.piece_at(make_sq(nf, nr));
        if (p != NO_PIECE) {
            if (color_of(p) != them) return false;
            PieceType t = type_of(p);
            if (diag) return (t == BISHOP || t == PROM_BISHOP);
            if (t == ROOK || t == PROM_ROOK) return true;
            if (lance_ray && t == LANCE) return true;
            return false;
        }
    }
    return false;
}

// Does a piece of `(us, pt)` placed at `sq` attack `target` on `b` (ignoring `vacated`)?
static bool piece_attacks_sq(const Board& b, Color us, PieceType pt,
                             Square sq, Square target, Square vacated = SQ_NONE) {
    if (sq == target || target == SQ_NONE) return false;
    const int df  = file_of(target) - file_of(sq);
    const int dr  = rank_of(target) - rank_of(sq);
    const int fwd = (us == BLACK) ? -1 : 1;

    switch (pt) {
        case PAWN:
            return df == 0 && dr == fwd;
        case KNIGHT:
            return std::abs(df) == 1 && dr == 2 * fwd;
        case SILVER:
            return std::abs(df) <= 1 && std::abs(dr) <= 1 &&
                   (dr == fwd || (df != 0 && dr == -fwd));
        case GOLD:
        case PROM_PAWN: case PROM_LANCE: case PROM_KNIGHT: case PROM_SILVER:
            return std::abs(df) <= 1 && std::abs(dr) <= 1 &&
                   !(df != 0 && dr == -fwd);
        case KING:
            return std::abs(df) <= 1 && std::abs(dr) <= 1;
        case LANCE: {
            if (df != 0 || (fwd < 0 ? dr >= 0 : dr <= 0)) return false;
            for (int nr = rank_of(sq) + fwd; nr != rank_of(target); nr += fwd) {
                Square s = make_sq(file_of(sq), nr);
                if (s != vacated && b.piece_at(s) != NO_PIECE) return false;
            }
            return true;
        }
        case BISHOP:
        case PROM_BISHOP: {
            if (pt == PROM_BISHOP && std::abs(df) + std::abs(dr) == 1) return true;
            if (std::abs(df) != std::abs(dr)) return false;
            const int sf = sign_int(df), sr = sign_int(dr);
            for (int nf = file_of(sq) + sf, nr = rank_of(sq) + sr;
                 nf != file_of(target); nf += sf, nr += sr) {
                Square s = make_sq(nf, nr);
                if (s != vacated && b.piece_at(s) != NO_PIECE) return false;
            }
            return true;
        }
        case ROOK:
        case PROM_ROOK: {
            if (pt == PROM_ROOK && std::abs(df) == 1 && std::abs(dr) == 1) return true;
            if (df != 0 && dr != 0) return false;
            const int sf = sign_int(df), sr = sign_int(dr);
            for (int nf = file_of(sq) + sf, nr = rank_of(sq) + sr;
                 nf != file_of(target) || nr != rank_of(target); nf += sf, nr += sr) {
                Square s = make_sq(nf, nr);
                if (s != vacated && b.piece_at(s) != NO_PIECE) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

// Fast check whether `board` (whose side to move `them` IS currently in check)
// has at least one legal evasion (exits on first found!).
static bool has_any_legal_evasion(Board& board) {
    const Color them = board.side_to_move();
    const Color by   = ~them;
    const Square ksq = board.king_sq(them);
    if (ksq == SQ_NONE) return true;

    // 1. Try king escapes first (resolves ~98% of checks in <50 ns).
    const int kf = file_of(ksq), kr = rank_of(ksq);
    for (int dr = -1; dr <= 1; ++dr) {
        for (int df = -1; df <= 1; ++df) {
            if (df == 0 && dr == 0) continue;
            if (!on_board(kf + df, kr + dr)) continue;
            Square to = make_sq(kf + df, kr + dr);
            Piece tgt = board.piece_at(to);
            if (tgt != NO_PIECE && color_of(tgt) == them) continue;
            if (!board.is_attacked_ignoring(to, by, ksq))
                return true;
        }
    }

    // 2. Count checkers attacking `ksq` (if >= 2, double check with no king move => mate!).
    Square checker_sq = SQ_NONE;
    int num_checkers = 0;
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != by) continue;
        if (piece_attacks_sq(board, by, type_of(p), s, ksq)) {
            checker_sq = s;
            if (++num_checkers >= 2) return false;
        }
    }
    if (num_checkers == 0) return true;

    // 3. Single check: try capturing `checker_sq` with a non-king piece of `them`.
    for (int s = 0; s < SQUARE_NB; ++s) {
        if (s == ksq) continue;
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != them) continue;
        if (!piece_attacks_sq(board, them, type_of(p), s, checker_sq)) continue;
        // Capturing the sole checker leaves `ksq` safe unless `s` was pinned by another slider!
        Move cap = must_promote(type_of(p), them, rank_of(checker_sq))
                 ? make_promote_move(s, checker_sq)
                 : make_normal_move(s, checker_sq);
        board.do_move(cap, false);
        bool safe = !board.is_attacked(ksq, by);
        board.undo_move(cap);
        if (safe) return true;
    }

    // 4. If `checker_sq` is a distant slider (`max(|df|,|dr|) >= 2`), try interposing!
    const int cdf = file_of(checker_sq) - kf;
    const int cdr = rank_of(checker_sq) - kr;
    if (std::max(std::abs(cdf), std::abs(cdr)) >= 2 &&
        (cdf == 0 || cdr == 0 || std::abs(cdf) == std::abs(cdr))) {
        const int sf = sign_int(cdf), sr = sign_int(cdr);

        bool has_any_major_drop = (board.hand(them, SILVER) > 0 || board.hand(them, GOLD) > 0 ||
                                   board.hand(them, BISHOP) > 0 || board.hand(them, ROOK) > 0);
        bool pawn_file[FILE_NB] = {};
        if (board.hand(them, PAWN) > 0) {
            Piece tp = make_piece(them, PAWN);
            for (int s = 0; s < SQUARE_NB; ++s)
                if (board.piece_at(s) == tp) pawn_file[file_of(s)] = true;
        }

        for (int nf = kf + sf, nr = kr + sr;
             make_sq(nf, nr) != checker_sq; nf += sf, nr += sr) {
            Square inter = make_sq(nf, nr);
            // 4a. Drop interposition on empty square `inter`
            if (has_any_major_drop) return true;
            if (board.hand(them, LANCE) > 0 && can_drop_on_rank(LANCE, them, nr)) return true;
            if (board.hand(them, KNIGHT) > 0 && can_drop_on_rank(KNIGHT, them, nr)) return true;
            if (board.hand(them, PAWN) > 0 && can_drop_on_rank(PAWN, them, nr) && !pawn_file[nf]) {
                Move pd = make_drop_move(PAWN, inter);
                board.do_move(pd, false);
                bool pd_legal = true;
                if (board.in_check()) {
                    // Guard against rare counter-pawn-drop-mate
                    pd_legal = has_any_legal_evasion(board);
                }
                board.undo_move(pd);
                if (pd_legal) return true;
            }

            // 4b. Board move interposition onto `inter`
            for (int s = 0; s < SQUARE_NB; ++s) {
                if (s == ksq) continue;
                Piece p = board.piece_at(s);
                if (p == NO_PIECE || color_of(p) != them) continue;
                if (!piece_attacks_sq(board, them, type_of(p), s, inter)) continue;
                Move mv = must_promote(type_of(p), them, nr)
                        ? make_promote_move(s, inter)
                        : make_normal_move(s, inter);
                board.do_move(mv, false);
                bool safe = !board.is_attacked(ksq, by);
                board.undo_move(mv);
                if (safe) return true;
            }
        }
    }

    return false;
}

static inline bool is_pawn_drop_mate(Board& board, Move m) {
    if (!is_drop(m) || dropped_pt(m) != PAWN) return false;
    const Color us = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return false;
    const int fwd = (us == BLACK) ? -1 : 1;
    const Square to = to_sq(m);
    if (file_of(to) != file_of(opp_ksq) || rank_of(to) + fwd != rank_of(opp_ksq))
        return false;

    board.do_move(m, false);
    const bool mated = !has_any_legal_evasion(board);
    board.undo_move(m);
    return mated;
}

// ============================================================
// Low-level move adders
// ============================================================
enum class GenMode { ALL, TACTICAL_ONLY, QUIET_ONLY };

static inline void add_to(MoveList& ml, Square from, Square to,
                          PieceType pt, Color us, int from_rank,
                          bool is_capture, GenMode mode) {
    const bool to_pz   = in_promo_zone(us, rank_of(to));
    const bool from_pz = in_promo_zone(us, from_rank);
    const bool must_p  = must_promote(pt, us, rank_of(to));
    const bool can_p   = is_promotable(pt) && (to_pz || from_pz);

    if (mode == GenMode::ALL) {
        if (must_p) {
            ml.push(make_promote_move(from, to));
        } else if (can_p) {
            ml.push(make_normal_move (from, to));
            ml.push(make_promote_move(from, to));
        } else {
            ml.push(make_normal_move(from, to));
        }
    } else if (mode == GenMode::TACTICAL_ONLY) {
        if (must_p) {
            ml.push(make_promote_move(from, to));
        } else if (can_p) {
            // Promotion is always tactical; non-promote only if it's a capture!
            if (is_capture) ml.push(make_normal_move(from, to));
            ml.push(make_promote_move(from, to));
        } else if (is_capture) {
            ml.push(make_normal_move(from, to));
        }
    } else { // QUIET_ONLY: !is_capture && !is_promote
        if (!is_capture && !must_p) {
            ml.push(make_normal_move(from, to));
        }
    }
}

static inline void gen_step(const Board& b, MoveList& ml,
                            Square from, int df, int dr,
                            PieceType pt, Color us, GenMode mode) {
    int nf = file_of(from) + df;
    int nr = rank_of(from) + dr;
    if (!on_board(nf, nr)) return;
    Square to = make_sq(nf, nr);
    Piece  tgt = b.piece_at(to);
    if (tgt != NO_PIECE && color_of(tgt) == us) return;
    add_to(ml, from, to, pt, us, rank_of(from), tgt != NO_PIECE, mode);
}

static inline void gen_slide(const Board& b, MoveList& ml,
                             Square from, int df, int dr,
                             PieceType pt, Color us, GenMode mode) {
    const int r0 = rank_of(from);
    for (int nf = file_of(from) + df, nr = r0 + dr;
         on_board(nf, nr); nf += df, nr += dr) {
        Square to  = make_sq(nf, nr);
        Piece  tgt = b.piece_at(to);
        if (tgt != NO_PIECE && color_of(tgt) == us) break;
        add_to(ml, from, to, pt, us, r0, tgt != NO_PIECE, mode);
        if (tgt != NO_PIECE) break;
    }
}

static void gen_piece(const Board& b, MoveList& ml, Square from, Color us, GenMode mode) {
    Piece     p  = b.piece_at(from);
    PieceType pt = type_of(p);
    int fwd      = (us == BLACK) ? -1 : 1;

    switch (pt) {
        case PAWN:
            gen_step(b, ml, from, 0, fwd, PAWN, us, mode);
            break;
        case LANCE:
            gen_slide(b, ml, from, 0, fwd, LANCE, us, mode);
            break;
        case KNIGHT:
            gen_step(b, ml, from, -1, 2 * fwd, KNIGHT, us, mode);
            gen_step(b, ml, from,  1, 2 * fwd, KNIGHT, us, mode);
            break;
        case SILVER:
            gen_step(b, ml, from,  0,  fwd, SILVER, us, mode);
            gen_step(b, ml, from,  1,  fwd, SILVER, us, mode);
            gen_step(b, ml, from, -1,  fwd, SILVER, us, mode);
            gen_step(b, ml, from,  1, -fwd, SILVER, us, mode);
            gen_step(b, ml, from, -1, -fwd, SILVER, us, mode);
            break;
        case GOLD:
        case PROM_PAWN: case PROM_LANCE: case PROM_KNIGHT: case PROM_SILVER:
            gen_step(b, ml, from,  0,  fwd, pt, us, mode);
            gen_step(b, ml, from,  1,  fwd, pt, us, mode);
            gen_step(b, ml, from, -1,  fwd, pt, us, mode);
            gen_step(b, ml, from,  1,    0, pt, us, mode);
            gen_step(b, ml, from, -1,    0, pt, us, mode);
            gen_step(b, ml, from,  0, -fwd, pt, us, mode);
            break;
        case BISHOP:
            gen_slide(b, ml, from,  1,  1, BISHOP, us, mode);
            gen_slide(b, ml, from,  1, -1, BISHOP, us, mode);
            gen_slide(b, ml, from, -1,  1, BISHOP, us, mode);
            gen_slide(b, ml, from, -1, -1, BISHOP, us, mode);
            break;
        case ROOK:
            gen_slide(b, ml, from,  0,  1, ROOK, us, mode);
            gen_slide(b, ml, from,  0, -1, ROOK, us, mode);
            gen_slide(b, ml, from,  1,  0, ROOK, us, mode);
            gen_slide(b, ml, from, -1,  0, ROOK, us, mode);
            break;
        case PROM_BISHOP:
            gen_slide(b, ml, from,  1,  1, PROM_BISHOP, us, mode);
            gen_slide(b, ml, from,  1, -1, PROM_BISHOP, us, mode);
            gen_slide(b, ml, from, -1,  1, PROM_BISHOP, us, mode);
            gen_slide(b, ml, from, -1, -1, PROM_BISHOP, us, mode);
            gen_step (b, ml, from,  0,  1, PROM_BISHOP, us, mode);
            gen_step (b, ml, from,  0, -1, PROM_BISHOP, us, mode);
            gen_step (b, ml, from,  1,  0, PROM_BISHOP, us, mode);
            gen_step (b, ml, from, -1,  0, PROM_BISHOP, us, mode);
            break;
        case PROM_ROOK:
            gen_slide(b, ml, from,  0,  1, PROM_ROOK, us, mode);
            gen_slide(b, ml, from,  0, -1, PROM_ROOK, us, mode);
            gen_slide(b, ml, from,  1,  0, PROM_ROOK, us, mode);
            gen_slide(b, ml, from, -1,  0, PROM_ROOK, us, mode);
            gen_step (b, ml, from,  1,  1, PROM_ROOK, us, mode);
            gen_step (b, ml, from,  1, -1, PROM_ROOK, us, mode);
            gen_step (b, ml, from, -1,  1, PROM_ROOK, us, mode);
            gen_step (b, ml, from, -1, -1, PROM_ROOK, us, mode);
            break;
        case KING:
            for (int df = -1; df <= 1; df++)
                for (int dr = -1; dr <= 1; dr++)
                    if (df || dr) gen_step(b, ml, from, df, dr, KING, us, mode);
            break;
        default: break;
    }
}

static void gen_drops(const Board& b, MoveList& ml, Color us) {
    PieceType have_pts[7];
    int n_have = 0;
    bool have_pawn = false;
    for (int pt = PAWN; pt <= ROOK; pt++) {
        if (b.hand(us, static_cast<PieceType>(pt)) > 0) {
            have_pts[n_have++] = static_cast<PieceType>(pt);
            if (pt == PAWN) have_pawn = true;
        }
    }
    if (n_have == 0) return;

    bool pawn_file[FILE_NB] = {};
    if (have_pawn) {
        Piece bp = make_piece(us, PAWN);
        for (int s = 0; s < SQUARE_NB; s++)
            if (b.piece_at(s) == bp) pawn_file[file_of(s)] = true;
    }

    for (int to = 0; to < SQUARE_NB; to++) {
        if (b.piece_at(to) != NO_PIECE) continue;
        int r = rank_of(to), f = file_of(to);
        for (int i = 0; i < n_have; ++i) {
            PieceType pt = have_pts[i];
            if (!can_drop_on_rank(pt, us, r)) continue;
            if (pt == PAWN && pawn_file[f]) continue;
            ml.push(make_drop_move(pt, to));
        }
    }
}

// Fast filter for pseudo-legal moves when NOT currently in check!
static inline bool is_pseudo_move_legal_not_in_check(Board& board, Move m) {
    const Color us   = board.side_to_move();
    const Square ksq = board.king_sq(us);

    if (is_drop(m)) {
        return !is_pawn_drop_mate(board, m);
    }

    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    if (type_of(board.piece_at(from)) == KING) {
        return !board.is_attacked_ignoring(to, ~us, from);
    }
    return !exposes_king_along_ray(board, us, ksq, from, to);
}

// ============================================================
// Public Move Generators
// ============================================================
void generate_legal_moves(Board& board, MoveList& out) {
    const Color us = board.side_to_move();
    const bool in_chk = board.in_check();

    MoveList pseudo;
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(board, pseudo, s, us, GenMode::ALL);
    }
    gen_drops(board, pseudo, us);

    if (!in_chk) {
        for (Move m : pseudo) {
            if (is_pseudo_move_legal_not_in_check(board, m))
                out.push(m);
        }
        return;
    }

    // In check: verify king safety after move + pawn-drop-mate
    const Square ksq_orig = board.king_sq(us);
    for (Move m : pseudo) {
        if (is_drop(m)) {
            // If ksq_orig is valid, a drop can only block a single sliding check
            // on the ray from ksq_orig! Quick filter:
            if (ksq_orig != SQ_NONE) {
                int df = file_of(to_sq(m)) - file_of(ksq_orig);
                int dr = rank_of(to_sq(m)) - rank_of(ksq_orig);
                if (df != 0 && dr != 0 && std::abs(df) != std::abs(dr))
                    continue;
            }
        }
        board.do_move(m, false);
        bool legal = !board.is_attacked(board.king_sq(us), ~us);
        if (legal && is_drop(m) && dropped_pt(m) == PAWN && board.in_check()) {
            legal = has_any_legal_evasion(board);
        }
        board.undo_move(m);
        if (legal) out.push(m);
    }
}

void generate_tactical_moves(Board& board, MoveList& out) {
    const Color us = board.side_to_move();
    MoveList pseudo;
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(board, pseudo, s, us, GenMode::TACTICAL_ONLY);
    }
    for (Move m : pseudo) {
        if (is_pseudo_move_legal_not_in_check(board, m))
            out.push(m);
    }
}

void generate_quiet_moves(Board& board, MoveList& out) {
    const Color us = board.side_to_move();
    MoveList pseudo;
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(board, pseudo, s, us, GenMode::QUIET_ONLY);
    }
    gen_drops(board, pseudo, us);
    for (Move m : pseudo) {
        if (is_pseudo_move_legal_not_in_check(board, m))
            out.push(m);
    }
}

bool move_gives_check(const Board& board, Move m) {
    const Color us = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return false;

    if (is_drop(m)) {
        return piece_attacks_sq(board, us, dropped_pt(m), to_sq(m), opp_ksq);
    }

    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    PieceType pt = type_of(board.piece_at(from));
    if (is_promote(m)) pt = promote_pt(pt);

    // 1. Direct check by the moved piece from `to` (with `from` vacated)
    if (piece_attacks_sq(board, us, pt, to, opp_ksq, from))
        return true;

    // 2. Discovered check by vacating `from` along a ray from `opp_ksq`
    const int kf = file_of(opp_ksq), kr = rank_of(opp_ksq);
    const int df = file_of(from) - kf, dr = rank_of(from) - kr;
    if (df == 0 || dr == 0 || std::abs(df) == std::abs(dr)) {
        const int sf = sign_int(df), sr = sign_int(dr);
        const int tdf = file_of(to) - kf, tdr = rank_of(to) - kr;
        // If `to` stays on the same ray from `opp_ksq`, it re-blocks the ray
        if (tdf * sr == tdr * sf && (tdf * sf > 0 || tdr * sr > 0) &&
            (sf == 0 || sr == 0 || std::abs(tdf) == std::abs(tdr))) {
            return false;
        }
        // Check empty between `opp_ksq` and `from`
        for (int nf = kf + sf, nr = kr + sr; ; nf += sf, nr += sr) {
            Square s = make_sq(nf, nr);
            if (s == from) break;
            if (board.piece_at(s) != NO_PIECE) return false;
        }
        const int us_fwd = (us == BLACK) ? -1 : 1;
        const bool diag = (sf != 0 && sr != 0);
        const bool lance_ray = (sf == 0 && sr == -us_fwd);
        for (int nf = file_of(from) + sf, nr = rank_of(from) + sr;
             on_board(nf, nr); nf += sf, nr += sr) {
            Piece p = board.piece_at(make_sq(nf, nr));
            if (p != NO_PIECE) {
                if (color_of(p) != us) return false;
                PieceType t = type_of(p);
                if (diag) return (t == BISHOP || t == PROM_BISHOP);
                if (t == ROOK || t == PROM_ROOK) return true;
                if (lance_ray && t == LANCE) return true;
                return false;
            }
        }
    }
    return false;
}

void generate_quiet_checks(Board& board, MoveList& out) {
    const Color us = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return;

    // 1. Quiet board moves that give direct or discovered check
    MoveList pseudo_board;
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(board, pseudo_board, s, us, GenMode::QUIET_ONLY);
    }
    for (Move m : pseudo_board) {
        if (move_gives_check(board, m) && is_pseudo_move_legal_not_in_check(board, m))
            out.push(m);
    }

    // 2. Drop checks targeted directly at `opp_ksq` geometry (super fast!)
    const int kf = file_of(opp_ksq), kr = rank_of(opp_ksq);
    const int fwd = (us == BLACK) ? -1 : 1;

    // Pawn drop check
    if (board.hand(us, PAWN) > 0) {
        int pr = kr - fwd;
        if (on_board(kf, pr) && can_drop_on_rank(PAWN, us, pr)) {
            Square s = make_sq(kf, pr);
            if (board.piece_at(s) == NO_PIECE) {
                bool nifu = false;
                Piece bp = make_piece(us, PAWN);
                for (int r = 0; r < RANK_NB; ++r)
                    if (board.piece_at(make_sq(kf, r)) == bp) { nifu = true; break; }
                if (!nifu) {
                    Move m = make_drop_move(PAWN, s);
                    if (!is_pawn_drop_mate(board, m)) out.push(m);
                }
            }
        }
    }
    // Knight drop checks
    if (board.hand(us, KNIGHT) > 0) {
        int nr = kr - 2 * fwd;
        for (int df : {-1, 1}) {
            int nf = kf + df;
            if (on_board(nf, nr) && can_drop_on_rank(KNIGHT, us, nr)) {
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) == NO_PIECE) out.push(make_drop_move(KNIGHT, s));
            }
        }
    }
    // Silver & Gold adjacent drop checks
    const bool have_s = board.hand(us, SILVER) > 0;
    const bool have_g = board.hand(us, GOLD)   > 0;
    if (have_s || have_g) {
        for (int dr = -1; dr <= 1; ++dr) {
            for (int df = -1; df <= 1; ++df) {
                if (df == 0 && dr == 0) continue;
                int nf = kf + df, nr = kr + dr;
                if (!on_board(nf, nr)) continue;
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) != NO_PIECE) continue;
                if (have_s && (dr == -fwd || (df != 0 && dr == fwd)))
                    out.push(make_drop_move(SILVER, s));
                if (have_g && !(df != 0 && dr == fwd))
                    out.push(make_drop_move(GOLD, s));
            }
        }
    }
    // Lance drop checks
    if (board.hand(us, LANCE) > 0) {
        for (int nr = kr - fwd; on_board(kf, nr); nr -= fwd) {
            Square s = make_sq(kf, nr);
            if (board.piece_at(s) != NO_PIECE) break;
            if (can_drop_on_rank(LANCE, us, nr)) out.push(make_drop_move(LANCE, s));
        }
    }
    // Bishop drop checks
    if (board.hand(us, BISHOP) > 0) {
        static constexpr int DDF[4] = {1, 1, -1, -1};
        static constexpr int DDR[4] = {1, -1, 1, -1};
        for (int d = 0; d < 4; ++d) {
            for (int nf = kf + DDF[d], nr = kr + DDR[d]; on_board(nf, nr);
                 nf += DDF[d], nr += DDR[d]) {
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) != NO_PIECE) break;
                out.push(make_drop_move(BISHOP, s));
            }
        }
    }
    // Rook drop checks
    if (board.hand(us, ROOK) > 0) {
        static constexpr int ODF[4] = {1, -1, 0, 0};
        static constexpr int ODR[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            for (int nf = kf + ODF[d], nr = kr + ODR[d]; on_board(nf, nr);
                 nf += ODF[d], nr += ODR[d]) {
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) != NO_PIECE) break;
                out.push(make_drop_move(ROOK, s));
            }
        }
    }
}

// ============================================================
// O(1) Move Legality Check (for TT move / Killers / Countermove)
// ============================================================
bool is_move_legal(Board& board, Move m, bool currently_in_check) {
    if (m == MOVE_NONE) return false;
    const Color us = board.side_to_move();

    if (is_drop(m)) {
        // Ensure no extra bits (like promote or non-zero from_sq) are set
        if (is_promote(m) || from_sq(m) != 0) return false;
        const PieceType pt = dropped_pt(m);
        const Square    to = to_sq(m);
        if (pt < PAWN || pt > ROOK || to < 0 || to >= SQUARE_NB) return false;
        if (board.hand(us, pt) <= 0) return false;
        if (board.piece_at(to) != NO_PIECE) return false;
        if (!can_drop_on_rank(pt, us, rank_of(to))) return false;
        if (pt == PAWN) {
            Piece bp = make_piece(us, PAWN);
            int f = file_of(to);
            for (int r = 0; r < RANK_NB; ++r)
                if (board.piece_at(make_sq(f, r)) == bp) return false;
        }
        if (!currently_in_check) {
            return !is_pawn_drop_mate(board, m);
        }
        board.do_move(m, false);
        bool legal = !board.is_attacked(board.king_sq(us), ~us);
        if (legal && pt == PAWN && board.in_check())
            legal = has_any_legal_evasion(board);
        board.undo_move(m);
        return legal;
    }

    // Normal / promote move: ensure upper drop-pt bits are zero
    if ((m >> 16) != 0) return false;
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    if (from < 0 || from >= SQUARE_NB || to < 0 || to >= SQUARE_NB || from == to)
        return false;

    const Piece moving = board.piece_at(from);
    if (moving == NO_PIECE || color_of(moving) != us) return false;
    const Piece tgt = board.piece_at(to);
    if (tgt != NO_PIECE && color_of(tgt) == us) return false;

    const PieceType pt = type_of(moving);
    const bool promo = is_promote(m);
    if (promo) {
        if (!is_promotable(pt)) return false;
        if (!in_promo_zone(us, rank_of(from)) && !in_promo_zone(us, rank_of(to)))
            return false;
    } else {
        if (must_promote(pt, us, rank_of(to))) return false;
    }

    if (!piece_attacks_sq(board, us, pt, from, to)) return false;

    if (!currently_in_check) {
        return is_pseudo_move_legal_not_in_check(board, m);
    }
    board.do_move(m, false);
    bool legal = !board.is_attacked(board.king_sq(us), ~us);
    board.undo_move(m);
    return legal;
}

// ============================================================
// Shogi Static Exchange Evaluation (SEE)
// ============================================================
bool see_ge(const Board& board, Move m, int threshold) {
    const Square to = to_sq(m);
    const Color  us = board.side_to_move();

    int swap_val = -threshold;
    PieceType next_victim_pt;
    bool used[SQUARE_NB] = {};

    if (is_drop(m)) {
        next_victim_pt = dropped_pt(m);
    } else {
        const Square from = from_sq(m);
        used[from] = true;
        const Piece victim = board.piece_at(to);
        if (victim != NO_PIECE)
            swap_val += capture_swing_value(type_of(victim));
        PieceType pt = type_of(board.piece_at(from));
        if (is_promote(m)) {
            swap_val += promote_gain_value(pt);
            next_victim_pt = promote_pt(pt);
        } else {
            next_victim_pt = pt;
        }
    }

    // If even without opponent recapturing we can't reach threshold, fail fast!
    if (swap_val < 0) return false;

    // If opponent recaptures `next_victim_pt` for free and we still stay >= threshold, pass fast!
    swap_val -= capture_swing_value(next_victim_pt);
    if (swap_val >= 0) return true;

    const int tf = file_of(to), tr = rank_of(to);
    const bool to_in_pz[COLOR_NB] = { in_promo_zone(BLACK, tr), in_promo_zone(WHITE, tr) };
    Color stm = ~us;

    // Find least-valuable attacker of `to` for `stm` (treating `used[]` squares as empty so X-rays work!)
    auto pick_least_attacker = [&](Color side, Square& best_sq, PieceType& after_pt, int& promo_bonus) -> bool {
        int best_cost = 1'000'000;
        best_sq = SQ_NONE;
        const int fwd = (side == BLACK) ? -1 : 1;

        auto consider = [&](Square s, PieceType pt) {
            bool can_p = is_promotable(pt) && (to_in_pz[side] || in_promo_zone(side, rank_of(s)));
            PieceType res_pt = can_p ? promote_pt(pt) : pt;
            int cost = capture_swing_value(pt);
            if (cost < best_cost) {
                best_cost = cost;
                best_sq = s;
                after_pt = res_pt;
                promo_bonus = can_p ? promote_gain_value(pt) : 0;
            }
        };

        // 1. Pawn
        if (on_board(tf, tr - fwd)) {
            Square s = make_sq(tf, tr - fwd);
            if (!used[s] && board.piece_at(s) == make_piece(side, PAWN)) {
                consider(s, PAWN);
                return true; // Pawn is strictly cheapest possible attacker
            }
        }
        // 2. Knight
        for (int df : {-1, 1}) {
            int nf = tf + df, nr = tr - 2 * fwd;
            if (on_board(nf, nr)) {
                Square s = make_sq(nf, nr);
                if (!used[s] && board.piece_at(s) == make_piece(side, KNIGHT))
                    consider(s, KNIGHT);
            }
        }
        // 3. Adjacent step attackers (Silver, Gold, Promoted minors, Horse, Dragon, King)
        for (int dr = -1; dr <= 1; ++dr) {
            for (int df = -1; df <= 1; ++df) {
                if (df == 0 && dr == 0) continue;
                int nf = tf + df, nr = tr + dr;
                if (!on_board(nf, nr)) continue;
                Square s = make_sq(nf, nr);
                if (used[s]) continue;
                Piece p = board.piece_at(s);
                if (p == NO_PIECE || color_of(p) != side) continue;
                PieceType t = type_of(p);
                if (t == SILVER) {
                    if (dr == -fwd || (df != 0 && dr == fwd)) consider(s, t);
                } else if (t == GOLD || t == PROM_PAWN || t == PROM_LANCE ||
                           t == PROM_KNIGHT || t == PROM_SILVER) {
                    if (!(df != 0 && dr == fwd)) consider(s, t);
                } else if (t == PROM_BISHOP) {
                    if (df == 0 || dr == 0) consider(s, t);
                } else if (t == PROM_ROOK) {
                    if (df != 0 && dr != 0) consider(s, t);
                } else if (t == KING) {
                    consider(s, KING);
                }
            }
        }
        // 4. Orthogonal sliders (Lance, Rook, Dragon) with X-ray through `used[]`
        static constexpr int ODF[4] = {1, -1, 0, 0};
        static constexpr int ODR[4] = {0, 0, 1, -1};
        for (int d = 0; d < 4; ++d) {
            const bool lance_ray = (ODF[d] == 0 && ODR[d] == -fwd);
            for (int nf = tf + ODF[d], nr = tr + ODR[d]; on_board(nf, nr);
                 nf += ODF[d], nr += ODR[d]) {
                Square s = make_sq(nf, nr);
                if (used[s]) continue;
                Piece p = board.piece_at(s);
                if (p != NO_PIECE) {
                    if (color_of(p) == side) {
                        PieceType t = type_of(p);
                        if (t == ROOK || t == PROM_ROOK || (lance_ray && t == LANCE))
                            consider(s, t);
                    }
                    break;
                }
            }
        }
        // 5. Diagonal sliders (Bishop, Horse) with X-ray through `used[]`
        static constexpr int DDF[4] = {1, 1, -1, -1};
        static constexpr int DDR[4] = {1, -1, 1, -1};
        for (int d = 0; d < 4; ++d) {
            for (int nf = tf + DDF[d], nr = tr + DDR[d]; on_board(nf, nr);
                 nf += DDF[d], nr += DDR[d]) {
                Square s = make_sq(nf, nr);
                if (used[s]) continue;
                Piece p = board.piece_at(s);
                if (p != NO_PIECE) {
                    if (color_of(p) == side) {
                        PieceType t = type_of(p);
                        if (t == BISHOP || t == PROM_BISHOP)
                            consider(s, t);
                    }
                    break;
                }
            }
        }
        return best_sq != SQ_NONE;
    };

    while (true) {
        Square atk_sq = SQ_NONE;
        PieceType after_pt = NO_PT;
        int promo_bonus = 0;
        if (!pick_least_attacker(stm, atk_sq, after_pt, promo_bonus))
            break;

        used[atk_sq] = true;
        stm = ~stm;

        // Negamax flip of swap balance
        swap_val = -swap_val - 1 - capture_swing_value(after_pt) + promo_bonus;
        if (swap_val >= 0) {
            // If `after_pt` was KING and the other side still attacks `to`, king capture was illegal!
            if (after_pt == KING) {
                Square dummy_sq; PieceType dummy_pt; int dummy_pb;
                if (pick_least_attacker(stm, dummy_sq, dummy_pt, dummy_pb))
                    return (stm == us);
            }
            break;
        }
    }

    return (stm != us);
}

// ============================================================
// 1-Ply Mate Detection (mate1ply)
// ============================================================
bool find_mate_1ply(Board& board, Move* mate_move) {
    const Color us = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return false;

    auto test_mate_candidate = [&](Move m) -> bool {
        if (!is_pseudo_move_legal_not_in_check(board, m)) return false;
        board.do_move(m, false);
        const bool mated = board.in_check() && !has_any_legal_evasion(board);
        board.undo_move(m);
        if (mated && mate_move) *mate_move = m;
        return mated;
    };

    // 1. Check adjacent drops around `opp_ksq` first (Gold/Silver/Rook/Bishop/Lance + Knight)
    //    (Pawn drop can never be 1-ply mate due to uchifuzume rule!)
    const int kf = file_of(opp_ksq), kr = rank_of(opp_ksq);
    const int fwd = (us == BLACK) ? -1 : 1;

    const bool hg = board.hand(us, GOLD) > 0;
    const bool hs = board.hand(us, SILVER) > 0;
    const bool hr = board.hand(us, ROOK) > 0;
    const bool hb = board.hand(us, BISHOP) > 0;
    const bool hl = board.hand(us, LANCE) > 0;

    if (hg || hs || hr || hb || hl) {
        for (int dr = -1; dr <= 1; ++dr) {
            for (int df = -1; df <= 1; ++df) {
                if (df == 0 && dr == 0) continue;
                int nf = kf + df, nr = kr + dr;
                if (!on_board(nf, nr)) continue;
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) != NO_PIECE) continue;
                if (hg && !(df != 0 && dr == fwd))
                    if (test_mate_candidate(make_drop_move(GOLD, s))) return true;
                if (hs && (dr == -fwd || (df != 0 && dr == fwd)))
                    if (test_mate_candidate(make_drop_move(SILVER, s))) return true;
                if (hr && (df == 0 || dr == 0))
                    if (test_mate_candidate(make_drop_move(ROOK, s))) return true;
                if (hb && (df != 0 && dr != 0))
                    if (test_mate_candidate(make_drop_move(BISHOP, s))) return true;
                if (hl && df == 0 && dr == -fwd && can_drop_on_rank(LANCE, us, nr))
                    if (test_mate_candidate(make_drop_move(LANCE, s))) return true;
            }
        }
    }
    if (board.hand(us, KNIGHT) > 0) {
        int nr = kr - 2 * fwd;
        for (int df : {-1, 1}) {
            int nf = kf + df;
            if (on_board(nf, nr) && can_drop_on_rank(KNIGHT, us, nr)) {
                Square s = make_sq(nf, nr);
                if (board.piece_at(s) == NO_PIECE)
                    if (test_mate_candidate(make_drop_move(KNIGHT, s))) return true;
            }
        }
    }

    // 2. Check board moves (captures, promotions, and quiet moves) that deliver check!
    MoveList pseudo_board;
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != us) continue;
        gen_piece(board, pseudo_board, s, us, GenMode::ALL);
    }
    for (Move m : pseudo_board) {
        if (move_gives_check(board, m)) {
            if (test_mate_candidate(m)) return true;
        }
    }

    return false;
}

bool is_checkmate(Board& board) {
    if (!board.in_check()) return false;
    return !has_any_legal_evasion(board);
}

bool has_no_moves(Board& board) {
    if (board.in_check()) return !has_any_legal_evasion(board);
    MoveList ml;
    generate_legal_moves(board, ml);
    return ml.empty();
}
