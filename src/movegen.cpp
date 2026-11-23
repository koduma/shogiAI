// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "movegen.hpp"
#include <algorithm>
#include <cstdlib>

namespace {

inline bool in_promo_zone(Color c, int r) noexcept {
    return (c == BLACK) ? (r <= 2) : (r >= 6);
}

inline bool must_promote(PieceType pt, Color c, int nr) noexcept {
    if (c == BLACK) {
        if (pt == PAWN || pt == LANCE) return nr == 0;
        if (pt == KNIGHT)              return nr <= 1;
    } else {
        if (pt == PAWN || pt == LANCE) return nr == 8;
        if (pt == KNIGHT)              return nr >= 7;
    }
    return false;
}

inline bool can_drop_on_rank(PieceType pt, Color c, int r) noexcept {
    if (pt == PAWN || pt == LANCE)
        return (c == BLACK) ? (r > 0) : (r < 8);
    if (pt == KNIGHT)
        return (c == BLACK) ? (r > 1) : (r < 7);
    return true;
}

// Fast bitboard check whether piece `(us, pt)` standing on `sq` attacks `target` under occupancy `occ`.
inline bool piece_attacks_sq_occ(Color us, PieceType pt, Square sq, Square target, const Bitboard& occ) noexcept {
    if (sq == target || target == SQ_NONE) return false;
    const Color opp = ~us;
    switch (pt) {
        case PAWN:
        case KNIGHT:
        case SILVER:
        case GOLD:
        case PROM_PAWN:
        case PROM_LANCE:
        case PROM_KNIGHT:
        case PROM_SILVER:
        case KING:
            return Bitboards::STEP_ATTACKS[opp][pt][target].test(sq);
        case LANCE:
            return Bitboards::PSEUDO_LANCE_BB[opp][target].test(sq) &&
                   !Bitboards::BETWEEN_BB[sq][target].intersects(occ);
        case BISHOP:
            return Bitboards::PSEUDO_BISHOP_BB[target].test(sq) &&
                   !Bitboards::BETWEEN_BB[sq][target].intersects(occ);
        case ROOK:
            return Bitboards::PSEUDO_ROOK_BB[target].test(sq) &&
                   !Bitboards::BETWEEN_BB[sq][target].intersects(occ);
        case PROM_BISHOP:
            return Bitboards::STEP_ATTACKS[BLACK][KING][target].test(sq) ||
                   (Bitboards::PSEUDO_BISHOP_BB[target].test(sq) &&
                    !Bitboards::BETWEEN_BB[sq][target].intersects(occ));
        case PROM_ROOK:
            return Bitboards::STEP_ATTACKS[BLACK][KING][target].test(sq) ||
                   (Bitboards::PSEUDO_ROOK_BB[target].test(sq) &&
                    !Bitboards::BETWEEN_BB[sq][target].intersects(occ));
        default:
            return false;
    }
}

bool has_any_legal_evasion(Board& board);

inline bool is_pawn_drop_mate(Board& board, Move m) {
    if (!is_drop(m) || dropped_pt(m) != PAWN) return false;
    const Color  us      = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return false;
    const Square to = to_sq(m);
    if (!Bitboards::STEP_ATTACKS[~us][PAWN][opp_ksq].test(to)) {
        return false;
    }
    board.do_move(m, false);
    const bool mated = !has_any_legal_evasion(board);
    board.undo_move(m);
    return mated;
}

// Fast bitboard check whether `board` (currently in check) has at least 1 legal evasion!
bool has_any_legal_evasion(Board& board) {
    const Color  them = board.side_to_move();
    const Color  by   = ~them;
    const Square ksq  = board.king_sq(them);
    if (ksq == SQ_NONE) return true;

    // 1. King escapes (no do_move/undo_move needed!)
    const Bitboard occ_all     = board.occ_all();
    const Bitboard occ_no_king = occ_all.and_not(Bitboards::SQ_BB[ksq]);
    Bitboard k_cands = Bitboards::STEP_ATTACKS[BLACK][KING][ksq].and_not(board.pieces(them));
    while (k_cands) {
        const Square to = k_cands.pop_lsb();
        if (!board.is_attacked_occ(to, by, occ_no_king)) {
            return true;
        }
    }

    // 2. Checkers count: if double check and king cannot move -> checkmate!
    const Bitboard chk = board.checkers();
    if (chk.empty()) return true;
    if (chk.more_than_one()) return false;

    const Square   checker_sq       = chk.lsb();
    const Bitboard pinned_them      = board.pinned(them);
    const Bitboard unpinned_nonking = board.pieces(them).and_not(Bitboards::SQ_BB[ksq] | pinned_them);

    // 3. Single check: capture `checker_sq` with any unpinned non-king piece of `them`
    if (board.attackers_to(checker_sq, them, occ_all).intersects(unpinned_nonking)) {
        return true;
    }

    // 4. Single distant slider check: interpose on `BETWEEN_BB[ksq][checker_sq]`
    const Bitboard inter_bb = Bitboards::BETWEEN_BB[ksq][checker_sq];
    if (inter_bb) {
        // 4a. Drop interposition
        if (board.hand(them, SILVER) > 0 || board.hand(them, GOLD) > 0 ||
            board.hand(them, BISHOP) > 0 || board.hand(them, ROOK) > 0) {
            return true;
        }
        if (board.hand(them, LANCE) > 0 && inter_bb.intersects(Bitboards::DROP_ALLOWED_BB[them][LANCE])) {
            return true;
        }
        if (board.hand(them, KNIGHT) > 0 && inter_bb.intersects(Bitboards::DROP_ALLOWED_BB[them][KNIGHT])) {
            return true;
        }
        if (board.hand(them, PAWN) > 0) {
            const uint32_t pf9 = Bitboards::pawn_files_mask9(board.pieces(them, PAWN));
            Bitboard p_drops = inter_bb & Bitboards::PAWN_DROP_MASK_BB[them][pf9];
            while (p_drops) {
                const Square sq = p_drops.pop_lsb();
                if (!is_pawn_drop_mate(board, make_drop_move(PAWN, sq))) {
                    return true;
                }
            }
        }

        // 4b. Unpinned board move interposition onto `inter_bb`
        Bitboard ib = inter_bb;
        while (ib) {
            const Square inter = ib.pop_lsb();
            if (board.attackers_to(inter, them, occ_all).intersects(unpinned_nonking)) {
                return true;
            }
        }
    }

    return false;
}

enum class GenMode { ALL, TACTICAL_ONLY, QUIET_ONLY };

inline void emit_promotable_targets(MoveList& out, Square from, PieceType pt, Color us,
                                    const Bitboard& att, const Bitboard& our_occ,
                                    const Bitboard& enemy_occ, const Bitboard& empty_occ,
                                    GenMode mode) {
    const Bitboard can_p  = Bitboards::CAN_PROMOTE_TO_MASK[us][from];
    const Bitboard must_p = Bitboards::MUST_PROMOTE_BB[us][pt];

    if (mode == GenMode::ALL) {
        const Bitboard valid = att.and_not(our_occ);
        const Bitboard mp = valid & must_p;
        const Bitboard cp = valid & can_p.and_not(must_p);
        const Bitboard np = valid.and_not(can_p);

        mp.for_each([&](Square to) {
            out.push(make_promote_move(from, to));
        });
        cp.for_each([&](Square to) {
            out.push(make_normal_move(from, to));
            out.push(make_promote_move(from, to));
        });
        np.for_each([&](Square to) {
            out.push(make_normal_move(from, to));
        });
    } else if (mode == GenMode::TACTICAL_ONLY) {
        const Bitboard promo_targets = att.and_not(our_occ) & can_p;
        const Bitboard unpromo_caps  = (att & enemy_occ).and_not(must_p);

        promo_targets.for_each([&](Square to) {
            out.push(make_promote_move(from, to));
        });
        unpromo_caps.for_each([&](Square to) {
            out.push(make_normal_move(from, to));
        });
    } else { // QUIET_ONLY
        const Bitboard quiets = (att & empty_occ).and_not(must_p);
        quiets.for_each([&](Square to) {
            out.push(make_normal_move(from, to));
        });
    }
}

inline void emit_nonpromotable_targets(MoveList& out, Square from,
                                       const Bitboard& att, const Bitboard& our_occ,
                                       const Bitboard& enemy_occ, const Bitboard& empty_occ,
                                       GenMode mode) {
    Bitboard targets = (mode == GenMode::ALL)           ? att.and_not(our_occ)
                     : (mode == GenMode::TACTICAL_ONLY) ? (att & enemy_occ)
                                                        : (att & empty_occ);
    targets.for_each([&](Square to) {
        out.push(make_normal_move(from, to));
    });
}

// Generates all legal board moves when `!board.in_check()` directly using Bitboards!
void gen_board_moves_not_in_check(const Board& board, MoveList& out, GenMode mode) {
    const Color    us        = board.side_to_move();
    const Color    them      = ~us;
    const Square   ksq       = board.king_sq(us);
    const Bitboard occ       = board.occ_all();
    const Bitboard our_occ   = board.pieces(us);
    const Bitboard enemy_occ = board.pieces(them);
    const Bitboard empty_occ = ~occ;
    const Bitboard pinned    = (ksq != SQ_NONE) ? board.pinned(us) : Bitboard{};

    // 1. Batch unpinned Pawns (all 9 pawns shifted in 3 instructions!)
    {
        const Bitboard our_pawns      = board.pieces(us, PAWN);
        const Bitboard unpinned_pawns = our_pawns.and_not(pinned);
        if (unpinned_pawns) {
            const Bitboard p_att = Bitboards::pawn_attacks_bb(us, unpinned_pawns);
            const int from_delta = (us == BLACK) ? +9 : -9;
            const Bitboard pz    = Bitboards::PROMO_ZONE_BB[us];
            const Bitboard mp    = Bitboards::MUST_PROMOTE_BB[us][PAWN];

            if (mode == GenMode::ALL) {
                const Bitboard valid = p_att.and_not(our_occ);
                (valid & mp).for_each([&](Square to) {
                    out.push(make_promote_move(static_cast<Square>(to + from_delta), to));
                });
                (valid & pz.and_not(mp)).for_each([&](Square to) {
                    const Square from = static_cast<Square>(to + from_delta);
                    out.push(make_normal_move(from, to));
                    out.push(make_promote_move(from, to));
                });
                valid.and_not(pz).for_each([&](Square to) {
                    out.push(make_normal_move(static_cast<Square>(to + from_delta), to));
                });
            } else if (mode == GenMode::TACTICAL_ONLY) {
                const Bitboard promo_t = p_att.and_not(our_occ) & pz;
                const Bitboard cap_np  = (p_att & enemy_occ).and_not(mp);
                promo_t.for_each([&](Square to) {
                    out.push(make_promote_move(static_cast<Square>(to + from_delta), to));
                });
                cap_np.for_each([&](Square to) {
                    out.push(make_normal_move(static_cast<Square>(to + from_delta), to));
                });
            } else {
                const Bitboard quiets = (p_att & empty_occ).and_not(mp);
                quiets.for_each([&](Square to) {
                    out.push(make_normal_move(static_cast<Square>(to + from_delta), to));
                });
            }
        }

        // Pinned pawns (can only advance if pinned along their own file!)
        const Bitboard pinned_pawns = our_pawns & pinned;
        if (pinned_pawns) {
            pinned_pawns.for_each([&](Square from) {
                const Bitboard att = Bitboards::STEP_ATTACKS[us][PAWN][from] & Bitboards::LINE_BB[ksq][from];
                if (att) {
                    emit_promotable_targets(out, from, PAWN, us, att, our_occ, enemy_occ, empty_occ, mode);
                }
            });
        }
    }

    // 2. Promotable non-pawn pieces: LANCE, KNIGHT, SILVER, BISHOP, ROOK
    //    Note: a pinned Knight can NEVER move along `LINE_BB[ksq][from]`, so mask `~pinned` directly!
    board.pieces(us, KNIGHT).and_not(pinned).for_each([&](Square from) {
        const Bitboard att = Bitboards::STEP_ATTACKS[us][KNIGHT][from];
        emit_promotable_targets(out, from, KNIGHT, us, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, LANCE).for_each([&](Square from) {
        Bitboard att = Bitboards::lance_attacks(us, from, occ);
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_promotable_targets(out, from, LANCE, us, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, SILVER).for_each([&](Square from) {
        Bitboard att = Bitboards::STEP_ATTACKS[us][SILVER][from];
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_promotable_targets(out, from, SILVER, us, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, BISHOP).for_each([&](Square from) {
        Bitboard att = Bitboards::bishop_attacks(from, occ);
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_promotable_targets(out, from, BISHOP, us, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, ROOK).for_each([&](Square from) {
        Bitboard att = Bitboards::rook_attacks(from, occ);
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_promotable_targets(out, from, ROOK, us, att, our_occ, enemy_occ, empty_occ, mode);
    });

    // 3. Non-promotable non-king pieces: Gold movers (`GOLD | PROM_PAWN..PROM_SILVER`), Horse, Dragon
    board.gold_movers(us).for_each([&](Square from) {
        Bitboard att = Bitboards::STEP_ATTACKS[us][GOLD][from];
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, PROM_BISHOP).for_each([&](Square from) {
        Bitboard att = Bitboards::horse_attacks(from, occ);
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, mode);
    });

    board.pieces(us, PROM_ROOK).for_each([&](Square from) {
        Bitboard att = Bitboards::dragon_attacks(from, occ);
        if (pinned.test(from)) att &= Bitboards::LINE_BB[ksq][from];
        if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, mode);
    });

    // 4. King moves (verified directly via `is_attacked_occ` with `ksq` vacated!)
    if (ksq != SQ_NONE) {
        const Bitboard k_att = Bitboards::STEP_ATTACKS[BLACK][KING][ksq];
        Bitboard k_cands = (mode == GenMode::ALL)           ? k_att.and_not(our_occ)
                         : (mode == GenMode::TACTICAL_ONLY) ? (k_att & enemy_occ)
                                                            : (k_att & empty_occ);
        if (k_cands) {
            const Bitboard occ_no_king = occ.and_not(Bitboards::SQ_BB[ksq]);
            k_cands.for_each([&](Square to) {
                if (!board.is_attacked_occ(to, them, occ_no_king)) {
                    out.push(make_normal_move(ksq, to));
                }
            });
        }
    }
}

// Generates legal drop moves onto `target_empty` (`target_empty` MUST be a subset of `~board.occ_all()`)
void gen_drops_bb(Board& board, MoveList& out, Color us, const Bitboard& target_empty) {
    if (target_empty.empty()) return;

    // 1. Pawn drops (O(1) Nifu mask + single-square uchifuzume pre-filter!)
    if (board.hand(us, PAWN) > 0) {
        const uint32_t pf9 = Bitboards::pawn_files_mask9(board.pieces(us, PAWN));
        Bitboard p_targets = target_empty & Bitboards::PAWN_DROP_MASK_BB[us][pf9];
        const Square opp_ksq = board.king_sq(~us);
        if (opp_ksq != SQ_NONE) {
            const Bitboard chk_sq = p_targets & Bitboards::STEP_ATTACKS[~us][PAWN][opp_ksq];
            if (chk_sq) {
                const Square uchi_sq = chk_sq.lsb();
                if (is_pawn_drop_mate(board, make_drop_move(PAWN, uchi_sq))) {
                    p_targets = p_targets.and_not(chk_sq);
                }
            }
        }
        p_targets.for_each([&](Square to) {
            out.push(make_drop_move(PAWN, to));
        });
    }

    // 2. Lance & Knight drops (rank-restricted)
    if (board.hand(us, LANCE) > 0) {
        (target_empty & Bitboards::DROP_ALLOWED_BB[us][LANCE]).for_each([&](Square to) {
            out.push(make_drop_move(LANCE, to));
        });
    }
    if (board.hand(us, KNIGHT) > 0) {
        (target_empty & Bitboards::DROP_ALLOWED_BB[us][KNIGHT]).for_each([&](Square to) {
            out.push(make_drop_move(KNIGHT, to));
        });
    }

    // 3. Unrestricted drops (SILVER, GOLD, BISHOP, ROOK)
    PieceType unres[4];
    int n_unres = 0;
    if (board.hand(us, SILVER) > 0) unres[n_unres++] = SILVER;
    if (board.hand(us, GOLD)   > 0) unres[n_unres++] = GOLD;
    if (board.hand(us, BISHOP) > 0) unres[n_unres++] = BISHOP;
    if (board.hand(us, ROOK)   > 0) unres[n_unres++] = ROOK;

    if (n_unres == 1) {
        const PieceType pt0 = unres[0];
        target_empty.for_each([&](Square to) {
            out.push(make_drop_move(pt0, to));
        });
    } else if (n_unres == 2) {
        const PieceType pt0 = unres[0], pt1 = unres[1];
        target_empty.for_each([&](Square to) {
            out.push(make_drop_move(pt0, to));
            out.push(make_drop_move(pt1, to));
        });
    } else if (n_unres > 2) {
        target_empty.for_each([&](Square to) {
            for (int i = 0; i < n_unres; ++i) {
                out.push(make_drop_move(unres[i], to));
            }
        });
    }
}

// Fast Bitboard Check Evasion Generator (`board.in_check() == true`)
void gen_evasions_bb(Board& board, MoveList& out) {
    const Color    us        = board.side_to_move();
    const Color    them      = ~us;
    const Square   ksq       = board.king_sq(us);
    const Bitboard occ       = board.occ_all();
    const Bitboard our_occ   = board.pieces(us);
    const Bitboard enemy_occ = board.pieces(them);
    const Bitboard empty_occ = ~occ;

    if (ksq == SQ_NONE) return;

    // 1. King evasions
    const Bitboard occ_no_king = occ.and_not(Bitboards::SQ_BB[ksq]);
    Bitboards::STEP_ATTACKS[BLACK][KING][ksq].and_not(our_occ).for_each([&](Square to) {
        if (!board.is_attacked_occ(to, them, occ_no_king)) {
            out.push(make_normal_move(ksq, to));
        }
    });

    // 2. If double check, ONLY king moves can evade check!
    const Bitboard chk = board.checkers();
    if (chk.more_than_one() || chk.empty()) return;

    const Square   checker_sq   = chk.lsb();
    const Bitboard inter_bb     = Bitboards::BETWEEN_BB[ksq][checker_sq];
    const Bitboard evade_target = inter_bb | Bitboards::SQ_BB[checker_sq];
    const Bitboard unpinned     = our_occ.and_not(Bitboards::SQ_BB[ksq] | board.pinned(us));

    // 3. Unpinned non-king board moves landing on `evade_target` (capture checker or block slider)
    {
        const Bitboard pawns = board.pieces(us, PAWN) & unpinned;
        if (pawns) {
            const Bitboard p_att = Bitboards::pawn_attacks_bb(us, pawns) & evade_target;
            if (p_att) {
                const int from_delta = (us == BLACK) ? +9 : -9;
                const Bitboard pz    = Bitboards::PROMO_ZONE_BB[us];
                const Bitboard mp    = Bitboards::MUST_PROMOTE_BB[us][PAWN];
                (p_att & mp).for_each([&](Square to) {
                    out.push(make_promote_move(static_cast<Square>(to + from_delta), to));
                });
                (p_att & pz.and_not(mp)).for_each([&](Square to) {
                    const Square from = static_cast<Square>(to + from_delta);
                    out.push(make_normal_move(from, to));
                    out.push(make_promote_move(from, to));
                });
                p_att.and_not(pz).for_each([&](Square to) {
                    out.push(make_normal_move(static_cast<Square>(to + from_delta), to));
                });
            }
        }

        (board.pieces(us, KNIGHT) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::STEP_ATTACKS[us][KNIGHT][from] & evade_target;
            if (att) emit_promotable_targets(out, from, KNIGHT, us, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, LANCE) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::lance_attacks(us, from, occ) & evade_target;
            if (att) emit_promotable_targets(out, from, LANCE, us, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, SILVER) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::STEP_ATTACKS[us][SILVER][from] & evade_target;
            if (att) emit_promotable_targets(out, from, SILVER, us, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, BISHOP) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::bishop_attacks(from, occ) & evade_target;
            if (att) emit_promotable_targets(out, from, BISHOP, us, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, ROOK) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::rook_attacks(from, occ) & evade_target;
            if (att) emit_promotable_targets(out, from, ROOK, us, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.gold_movers(us) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::STEP_ATTACKS[us][GOLD][from] & evade_target;
            if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, PROM_BISHOP) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::horse_attacks(from, occ) & evade_target;
            if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
        (board.pieces(us, PROM_ROOK) & unpinned).for_each([&](Square from) {
            const Bitboard att = Bitboards::dragon_attacks(from, occ) & evade_target;
            if (att) emit_nonpromotable_targets(out, from, att, our_occ, enemy_occ, empty_occ, GenMode::ALL);
        });
    }

    // 4. Drop interpositions onto `inter_bb`
    if (inter_bb) {
        gen_drops_bb(board, out, us, inter_bb);
    }
}

// Precomputed direct-check target bitboards around `opp_ksq` for fast check generation & mate1ply
struct DirectCheckMasks {
    Bitboard by_pt[PT_NB];

    DirectCheckMasks(Color us, Square opp_ksq, const Bitboard& occ) noexcept {
        const Color them = ~us;
        const Bitboard king_s = Bitboards::STEP_ATTACKS[BLACK][KING][opp_ksq];
        const Bitboard bish_s = Bitboards::bishop_attacks(opp_ksq, occ);
        const Bitboard rook_s = Bitboards::rook_attacks(opp_ksq, occ);
        const Bitboard gold_s = Bitboards::STEP_ATTACKS[them][GOLD][opp_ksq];

        by_pt[NO_PT]       = Bitboard{};
        by_pt[PAWN]        = Bitboards::STEP_ATTACKS[them][PAWN][opp_ksq];
        by_pt[LANCE]       = Bitboards::lance_attacks(them, opp_ksq, occ);
        by_pt[KNIGHT]      = Bitboards::STEP_ATTACKS[them][KNIGHT][opp_ksq];
        by_pt[SILVER]      = Bitboards::STEP_ATTACKS[them][SILVER][opp_ksq];
        by_pt[GOLD]        = gold_s;
        by_pt[BISHOP]      = bish_s;
        by_pt[ROOK]        = rook_s;
        by_pt[KING]        = Bitboard{};
        by_pt[PROM_PAWN]   = gold_s;
        by_pt[PROM_LANCE]  = gold_s;
        by_pt[PROM_KNIGHT] = gold_s;
        by_pt[PROM_SILVER] = gold_s;
        by_pt[PROM_BISHOP] = bish_s | king_s;
        by_pt[PROM_ROOK]   = rook_s | king_s;
    }
};

} // namespace

// ============================================================
// Public Move Generators
// ============================================================
void generate_legal_moves(Board& board, MoveList& out) {
    if (board.in_check()) {
        gen_evasions_bb(board, out);
        return;
    }
    gen_board_moves_not_in_check(board, out, GenMode::ALL);
    gen_drops_bb(board, out, board.side_to_move(), ~board.occ_all());
}

void generate_tactical_moves(Board& board, MoveList& out) {
    gen_board_moves_not_in_check(board, out, GenMode::TACTICAL_ONLY);
}

void generate_quiet_moves(Board& board, MoveList& out) {
    gen_board_moves_not_in_check(board, out, GenMode::QUIET_ONLY);
    gen_drops_bb(board, out, board.side_to_move(), ~board.occ_all());
}

bool move_gives_check(const Board& board, Move m) {
    const Color  us      = board.side_to_move();
    const Square opp_ksq = board.king_sq(~us);
    if (opp_ksq == SQ_NONE) return false;

    if (is_drop(m)) {
        return piece_attacks_sq_occ(us, dropped_pt(m), to_sq(m), opp_ksq, board.occ_all());
    }

    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    PieceType pt = type_of(board.piece_at(from));
    if (is_promote(m)) pt = promote_pt(pt);

    const Bitboard occ_vacated = board.occ_all().and_not(Bitboards::SQ_BB[from]);
    if (piece_attacks_sq_occ(us, pt, to, opp_ksq, occ_vacated)) {
        return true;
    }

    const Bitboard disc = board.discoverers(us);
    return disc.test(from) && !Bitboards::LINE_BB[opp_ksq][from].test(to);
}

void generate_quiet_checks(Board& board, MoveList& out) {
    const Color  us      = board.side_to_move();
    const Color  them    = ~us;
    const Square opp_ksq = board.king_sq(them);
    if (opp_ksq == SQ_NONE) return;

    const Square   ksq       = board.king_sq(us);
    const Bitboard occ       = board.occ_all();
    const Bitboard empty_occ = ~occ;
    const Bitboard pinned    = (ksq != SQ_NONE) ? board.pinned(us) : Bitboard{};
    const Bitboard disc      = board.discoverers(us);
    const DirectCheckMasks dchk(us, opp_ksq, occ);

    // 1. Quiet board moves (non-capture, non-promotion) giving direct or discovered check
    board.pieces(us).and_not(Bitboards::SQ_BB[ksq]).for_each([&](Square from) {
        const PieceType pt = type_of(board.piece_at(from));
        Bitboard q_att = Bitboards::attacks_from(us, pt, from, occ) & empty_occ;
        q_att = q_att.and_not(Bitboards::MUST_PROMOTE_BB[us][pt]);
        if (pinned.test(from)) {
            q_att &= Bitboards::LINE_BB[ksq][from];
        }
        if (q_att.empty()) return;

        Bitboard chk_targets = q_att & dchk.by_pt[pt];
        if (disc.test(from)) {
            chk_targets |= q_att.and_not(Bitboards::LINE_BB[opp_ksq][from]);
        }
        chk_targets.for_each([&](Square to) {
            out.push(make_normal_move(from, to));
        });
    });

    // King quiet move giving discovered check
    if (ksq != SQ_NONE && disc.test(ksq)) {
        const Bitboard k_cands = (Bitboards::STEP_ATTACKS[BLACK][KING][ksq] & empty_occ)
                                     .and_not(Bitboards::LINE_BB[opp_ksq][ksq]);
        if (k_cands) {
            const Bitboard occ_no_king = occ.and_not(Bitboards::SQ_BB[ksq]);
            k_cands.for_each([&](Square to) {
                if (!board.is_attacked_occ(to, them, occ_no_king)) {
                    out.push(make_normal_move(ksq, to));
                }
            });
        }
    }

    // 2. Drop checks via direct bitboard intersection
    if (board.hand(us, PAWN) > 0) {
        const uint32_t pf9 = Bitboards::pawn_files_mask9(board.pieces(us, PAWN));
        Bitboard pd = empty_occ & dchk.by_pt[PAWN] & Bitboards::PAWN_DROP_MASK_BB[us][pf9];
        if (pd) {
            const Square s = pd.lsb();
            const Move m = make_drop_move(PAWN, s);
            if (!is_pawn_drop_mate(board, m)) out.push(m);
        }
    }
    if (board.hand(us, KNIGHT) > 0) {
        (empty_occ & dchk.by_pt[KNIGHT] & Bitboards::DROP_ALLOWED_BB[us][KNIGHT]).for_each([&](Square s) {
            out.push(make_drop_move(KNIGHT, s));
        });
    }
    if (board.hand(us, SILVER) > 0) {
        (empty_occ & dchk.by_pt[SILVER]).for_each([&](Square s) {
            out.push(make_drop_move(SILVER, s));
        });
    }
    if (board.hand(us, GOLD) > 0) {
        (empty_occ & dchk.by_pt[GOLD]).for_each([&](Square s) {
            out.push(make_drop_move(GOLD, s));
        });
    }
    if (board.hand(us, LANCE) > 0) {
        (empty_occ & dchk.by_pt[LANCE] & Bitboards::DROP_ALLOWED_BB[us][LANCE]).for_each([&](Square s) {
            out.push(make_drop_move(LANCE, s));
        });
    }
    if (board.hand(us, BISHOP) > 0) {
        (empty_occ & dchk.by_pt[BISHOP]).for_each([&](Square s) {
            out.push(make_drop_move(BISHOP, s));
        });
    }
    if (board.hand(us, ROOK) > 0) {
        (empty_occ & dchk.by_pt[ROOK]).for_each([&](Square s) {
            out.push(make_drop_move(ROOK, s));
        });
    }
}

// ============================================================
// O(1) Bitboard Move Legality Check (for TT move / Killers / Countermove)
// ============================================================
bool is_move_legal(Board& board, Move m, bool currently_in_check) {
    if (m == MOVE_NONE) return false;
    const Color  us  = board.side_to_move();
    const Square ksq = board.king_sq(us);

    if (is_drop(m)) {
        if ((m >> 20) != 0 || is_promote(m) || from_sq(m) != 0) return false;
        const PieceType pt = dropped_pt(m);
        const Square    to = to_sq(m);
        if (pt < PAWN || pt > ROOK || static_cast<unsigned>(to) >= static_cast<unsigned>(SQUARE_NB))
            return false;
        if (board.hand(us, pt) <= 0) return false;
        if (board.piece_at(to) != NO_PIECE) return false;
        if (!can_drop_on_rank(pt, us, rank_of(to))) return false;
        if (pt == PAWN) {
            if (board.pieces(us, PAWN).intersects(Bitboards::FILE_BB[file_of(to)]))
                return false;
        }
        if (currently_in_check) {
            const Bitboard chk = board.checkers();
            if (chk.more_than_one()) return false;
            if (ksq != SQ_NONE && !chk.empty()) {
                if (!Bitboards::BETWEEN_BB[ksq][chk.lsb()].test(to)) return false;
            }
        }
        return !is_pawn_drop_mate(board, m);
    }

    if ((m >> 16) != 0) return false;
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    if (static_cast<unsigned>(from) >= static_cast<unsigned>(SQUARE_NB) ||
        static_cast<unsigned>(to)   >= static_cast<unsigned>(SQUARE_NB) ||
        from == to) {
        return false;
    }

    const Piece moving = board.piece_at(from);
    if (moving == NO_PIECE || color_of(moving) != us) return false;
    const Piece tgt = board.piece_at(to);
    if (tgt != NO_PIECE && color_of(tgt) == us) return false;

    const PieceType pt    = type_of(moving);
    const bool      promo = is_promote(m);
    if (promo) {
        if (!is_promotable(pt)) return false;
        if (!in_promo_zone(us, rank_of(from)) && !in_promo_zone(us, rank_of(to)))
            return false;
    } else {
        if (must_promote(pt, us, rank_of(to))) return false;
    }

    if (!piece_attacks_sq_occ(us, pt, from, to, board.occ_all())) return false;

    if (pt == KING) {
        const Bitboard occ_no_king = board.occ_all().and_not(Bitboards::SQ_BB[from]);
        return !board.is_attacked_occ(to, ~us, occ_no_king);
    }

    if (!currently_in_check) {
        if (ksq == SQ_NONE) return true;
        return !board.pinned(us).test(from) || Bitboards::LINE_BB[ksq][from].test(to);
    }

    // Currently in check and moving a non-king piece:
    const Bitboard chk = board.checkers();
    if (chk.more_than_one()) return false;
    if (ksq != SQ_NONE) {
        if (board.pinned(us).test(from)) return false;
        if (!chk.empty()) {
            const Square csq = chk.lsb();
            if (to != csq && !Bitboards::BETWEEN_BB[ksq][csq].test(to)) return false;
        }
        return true;
    }

    board.do_move(m, false);
    const bool legal = !board.is_attacked(board.king_sq(us), ~us);
    board.undo_move(m);
    return legal;
}

// ============================================================
// Bitboard Shogi Static Exchange Evaluation (SEE with hand swing + X-ray)
// ============================================================
bool see_ge(const Board& board, Move m, int threshold) {
    const Square to = to_sq(m);
    const Color  us = board.side_to_move();

    int swap_val = -threshold;
    PieceType next_victim_pt;

    if (is_drop(m)) {
        next_victim_pt = dropped_pt(m);
    } else {
        const Square from   = from_sq(m);
        const Piece  victim = board.piece_at(to);
        if (victim != NO_PIECE)
            swap_val += capture_swing_value(type_of(victim));
        const PieceType pt = type_of(board.piece_at(from));
        if (is_promote(m)) {
            swap_val += promote_gain_value(pt);
            next_victim_pt = promote_pt(pt);
        } else {
            next_victim_pt = pt;
        }
    }

    if (swap_val < 0) return false;

    swap_val -= capture_swing_value(next_victim_pt);
    if (swap_val >= 0) return true;

    Bitboard occ = board.occ_all();
    if (!is_drop(m)) {
        bb_clear(occ, from_sq(m));
    }
    bb_clear(occ, to);

    Bitboard attackers = board.attackers_to_both(to, occ) & occ;

    const int tr = rank_of(to);
    const bool to_in_pz[COLOR_NB] = { in_promo_zone(BLACK, tr), in_promo_zone(WHITE, tr) };

    // Exact ascending order of `capture_swing_value(pt)` in Bonanza piece values:
    // PAWN(174), LANCE(464), KNIGHT(514), PROM_PAWN(621), PROM_LANCE(721),
    // SILVER(738), PROM_KNIGHT(767), PROM_SILVER(864), GOLD(888),
    // BISHOP(1138), ROOK(1284), PROM_BISHOP(1396), PROM_ROOK(1587), KING(30000)
    static constexpr PieceType SEE_ORDER[14] = {
        PAWN, LANCE, KNIGHT, PROM_PAWN, PROM_LANCE,
        SILVER, PROM_KNIGHT, PROM_SILVER, GOLD,
        BISHOP, ROOK, PROM_BISHOP, PROM_ROOK, KING
    };

    const Bitboard b_lances_on_ray = board.pieces(BLACK, LANCE) & Bitboards::PSEUDO_LANCE_BB[WHITE][to];
    const Bitboard w_lances_on_ray = board.pieces(WHITE, LANCE) & Bitboards::PSEUDO_LANCE_BB[BLACK][to];
    const Bitboard ortho_xray_pool = board.rooks_bb_ | b_lances_on_ray | w_lances_on_ray;
    const Bitboard diag_xray_pool  = board.bishops_bb_;

    auto pick_least_attacker_bb = [&](Color side, Square& out_sq, PieceType& after_pt, int& promo_bonus) -> bool {
        const Bitboard side_att = attackers & board.pieces(side);
        if (side_att.empty()) return false;

        for (PieceType pt : SEE_ORDER) {
            const Bitboard cand = side_att & board.pieces(pt);
            if (cand) {
                Square s;
                bool can_p = false;
                if (is_promotable(pt)) {
                    if (to_in_pz[side]) {
                        s     = cand.lsb();
                        can_p = true;
                    } else {
                        const Bitboard from_pz = cand & Bitboards::PROMO_ZONE_BB[side];
                        if (from_pz) {
                            s     = from_pz.lsb();
                            can_p = true;
                        } else {
                            s     = cand.lsb();
                            can_p = false;
                        }
                    }
                } else {
                    s = cand.lsb();
                }
                out_sq      = s;
                after_pt    = can_p ? promote_pt(pt) : pt;
                promo_bonus = can_p ? promote_gain_value(pt) : 0;
                return true;
            }
        }
        return false;
    };

    Color stm = ~us;
    while (true) {
        Square    atk_sq      = SQ_NONE;
        PieceType after_pt    = NO_PT;
        int       promo_bonus = 0;
        if (!pick_least_attacker_bb(stm, atk_sq, after_pt, promo_bonus))
            break;

        bb_clear(occ, atk_sq);
        bb_clear(attackers, atk_sq);

        // Uncover X-ray sliders behind `atk_sq`
        if (Bitboards::PSEUDO_ROOK_BB[to].test(atk_sq)) {
            if (ortho_xray_pool.intersects(occ)) {
                attackers |= (Bitboards::rook_attacks(to, occ) & ortho_xray_pool & occ);
            }
        } else if (Bitboards::PSEUDO_BISHOP_BB[to].test(atk_sq)) {
            if (diag_xray_pool.intersects(occ)) {
                attackers |= (Bitboards::bishop_attacks(to, occ) & diag_xray_pool & occ);
            }
        }

        stm = ~stm;
        swap_val = -swap_val - 1 - capture_swing_value(after_pt) + promo_bonus;
        if (swap_val >= 0) {
            if (after_pt == KING && attackers.intersects(board.pieces(stm))) {
                return (stm == us);
            }
            break;
        }
    }

    return (stm != us);
}

// ============================================================
// Bitboard 1-Ply Mate Detection (`mate1ply`)
// ============================================================
bool find_mate_1ply(Board& board, Move* mate_move) {
    const Color  us      = board.side_to_move();
    const Color  them    = ~us;
    const Square opp_ksq = board.king_sq(them);
    if (opp_ksq == SQ_NONE) return false;

    auto test_legal_check_for_mate = [&](Move m) -> bool {
        board.do_move(m, false);
        const bool mated = board.in_check() && !has_any_legal_evasion(board);
        board.undo_move(m);
        if (mated && mate_move) *mate_move = m;
        return mated;
    };

    const Bitboard occ       = board.occ_all();
    const Bitboard our_occ   = board.pieces(us);
    const Bitboard empty_occ = ~occ;
    const DirectCheckMasks dchk(us, opp_ksq, occ);

    // 1. Adjacent drop checks around `opp_ksq`:
    //    Since a drop never vacates a square (no discovered check), an adjacent drop onto `s`
    //    can ONLY be mate if `s` is defended by `us` (`board.is_attacked(s, us)`), otherwise
    //    `opp_ksq` immediately captures the dropped piece!
    const bool hg = board.hand(us, GOLD)   > 0;
    const bool hs = board.hand(us, SILVER) > 0;
    const bool hr = board.hand(us, ROOK)   > 0;
    const bool hb = board.hand(us, BISHOP) > 0;
    const bool hl = board.hand(us, LANCE)  > 0;

    if (hg || hs || hr || hb || hl) {
        Bitboard adj_empty = Bitboards::STEP_ATTACKS[BLACK][KING][opp_ksq] & empty_occ;
        while (adj_empty) {
            const Square s = adj_empty.pop_lsb();
            if (!board.is_attacked_occ(s, us, occ)) continue;

            if (hg && dchk.by_pt[GOLD].test(s)) {
                if (test_legal_check_for_mate(make_drop_move(GOLD, s))) return true;
            }
            if (hs && dchk.by_pt[SILVER].test(s)) {
                if (test_legal_check_for_mate(make_drop_move(SILVER, s))) return true;
            }
            if (hr && dchk.by_pt[ROOK].test(s)) {
                if (test_legal_check_for_mate(make_drop_move(ROOK, s))) return true;
            }
            if (hb && dchk.by_pt[BISHOP].test(s)) {
                if (test_legal_check_for_mate(make_drop_move(BISHOP, s))) return true;
            }
            if (hl && dchk.by_pt[LANCE].test(s) && Bitboards::DROP_ALLOWED_BB[us][LANCE].test(s)) {
                if (test_legal_check_for_mate(make_drop_move(LANCE, s))) return true;
            }
        }
    }

    // Knight drop mate (cannot be captured by `opp_ksq`)
    if (board.hand(us, KNIGHT) > 0) {
        Bitboard kd = dchk.by_pt[KNIGHT] & empty_occ & Bitboards::DROP_ALLOWED_BB[us][KNIGHT];
        while (kd) {
            const Square s = kd.pop_lsb();
            if (test_legal_check_for_mate(make_drop_move(KNIGHT, s))) return true;
        }
    }

    // 2. Direct & discovered checking board moves ONLY (via bitboard target masking!)
    const Square   ksq    = board.king_sq(us);
    const Bitboard pinned = (ksq != SQ_NONE) ? board.pinned(us) : Bitboard{};
    const Bitboard disc   = board.discoverers(us);

    Bitboard movers = our_occ.and_not(ksq != SQ_NONE ? Bitboards::SQ_BB[ksq] : Bitboard{});
    while (movers) {
        const Square    from = movers.pop_lsb();
        const PieceType pt   = type_of(board.piece_at(from));
        Bitboard att = Bitboards::attacks_from(us, pt, from, occ).and_not(our_occ);
        if (pinned.test(from)) {
            att &= Bitboards::LINE_BB[ksq][from];
        }
        if (att.empty()) continue;

        const bool is_disc = disc.test(from);
        const Bitboard same_line = is_disc ? Bitboards::LINE_BB[opp_ksq][from] : Bitboards::ALL_BB;

        if (is_promotable(pt)) {
            const PieceType ppt   = promote_pt(pt);
            const Bitboard  can_p = Bitboards::CAN_PROMOTE_TO_MASK[us][from];
            const Bitboard  must_p = Bitboards::MUST_PROMOTE_BB[us][pt];

            // Try promotion checks first (stronger!)
            Bitboard p_chk = (att & can_p) & (is_disc ? (att.and_not(same_line) | dchk.by_pt[ppt]) : dchk.by_pt[ppt]);
            while (p_chk) {
                const Square to = p_chk.pop_lsb();
                if (test_legal_check_for_mate(make_promote_move(from, to))) return true;
            }

            // Unpromoted checks
            Bitboard u_chk = att.and_not(must_p) & (is_disc ? (att.and_not(same_line) | dchk.by_pt[pt]) : dchk.by_pt[pt]);
            while (u_chk) {
                const Square to = u_chk.pop_lsb();
                if (test_legal_check_for_mate(make_normal_move(from, to))) return true;
            }
        } else {
            Bitboard chk_t = att & (is_disc ? (att.and_not(same_line) | dchk.by_pt[pt]) : dchk.by_pt[pt]);
            while (chk_t) {
                const Square to = chk_t.pop_lsb();
                if (test_legal_check_for_mate(make_normal_move(from, to))) return true;
            }
        }
    }

    // King move delivering discovered check
    if (ksq != SQ_NONE && disc.test(ksq)) {
        Bitboard k_cands = Bitboards::STEP_ATTACKS[BLACK][KING][ksq]
                               .and_not(our_occ | Bitboards::LINE_BB[opp_ksq][ksq]);
        if (k_cands) {
            const Bitboard occ_no_king = occ.and_not(Bitboards::SQ_BB[ksq]);
            while (k_cands) {
                const Square to = k_cands.pop_lsb();
                if (!board.is_attacked_occ(to, them, occ_no_king)) {
                    if (test_legal_check_for_mate(make_normal_move(ksq, to))) return true;
                }
            }
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
