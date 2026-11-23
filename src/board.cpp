// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "board.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cctype>
#include <cstdlib>

// ============================================================
// Precomputed Bitboard & Sliding Attack Tables
// ============================================================
namespace Bitboards {

Bitboard ALL_BB;
Bitboard SQ_BB[SQUARE_NB];
Bitboard FILE_BB[FILE_NB];
Bitboard RANK_BB[RANK_NB];
Bitboard PROMO_ZONE_BB[COLOR_NB];
Bitboard CAN_PROMOTE_TO_MASK[COLOR_NB][SQUARE_NB];
Bitboard MUST_PROMOTE_BB[COLOR_NB][PT_NB];
Bitboard DROP_ALLOWED_BB[COLOR_NB][PT_NB];
Bitboard PAWN_DROP_MASK_BB[COLOR_NB][512];

Bitboard STEP_ATTACKS[COLOR_NB][PT_NB][SQUARE_NB];
Bitboard PSEUDO_ROOK_BB[SQUARE_NB];
Bitboard PSEUDO_BISHOP_BB[SQUARE_NB];
Bitboard PSEUDO_LANCE_BB[COLOR_NB][SQUARE_NB];

Bitboard BETWEEN_BB[SQUARE_NB][SQUARE_NB];
Bitboard LINE_BB[SQUARE_NB][SQUARE_NB];

uint64_t FILE_MID_MASK[FILE_NB];
uint8_t  RANK_SHIFT[RANK_NB];
Bitboard LANCE_ATTACK_TABLE[COLOR_NB][SQUARE_NB][128];
Bitboard ROOK_RANK_ATTACK_TABLE[SQUARE_NB][128];
Bitboard ROOK_FILE_ATTACK_TABLE[SQUARE_NB][128];

uint64_t BISHOP_MID_MASK[SQUARE_NB];
uint32_t BISHOP_OFFSET[SQUARE_NB];
Bitboard BISHOP_ATTACK_TABLE[BISHOP_TABLE_SIZE];

namespace {
bool g_bb_initialized = false;

inline void add_step_if_valid(Bitboard& bb, int f, int r) noexcept {
    if (on_board(f, r)) {
        bb |= SQ_BB[make_sq(f, r)];
    }
}
} // namespace

void init() {
    if (g_bb_initialized) return;
    g_bb_initialized = true;

    ALL_BB = Bitboard(Bitboard::MASK_P0, Bitboard::MASK_P1);

    for (int f = 0; f < FILE_NB; ++f) FILE_BB[f] = Bitboard{};
    for (int r = 0; r < RANK_NB; ++r) RANK_BB[r] = Bitboard{};

    for (int s = 0; s < SQUARE_NB; ++s) {
        if (s < 63) SQ_BB[s] = Bitboard(1ULL << s, 0ULL);
        else        SQ_BB[s] = Bitboard(0ULL, 1ULL << (s - 63));
        FILE_BB[file_of(s)] |= SQ_BB[s];
        RANK_BB[rank_of(s)] |= SQ_BB[s];
    }

    for (int r = 0; r < RANK_NB; ++r) {
        RANK_SHIFT[r] = static_cast<uint8_t>((r < 7) ? (r * 9 + 1) : ((r - 7) * 9 + 1));
    }

    PROMO_ZONE_BB[BLACK] = RANK_BB[0] | RANK_BB[1] | RANK_BB[2];
    PROMO_ZONE_BB[WHITE] = RANK_BB[6] | RANK_BB[7] | RANK_BB[8];

    for (int c_i = 0; c_i < COLOR_NB; ++c_i) {
        const Color c = static_cast<Color>(c_i);
        for (int s = 0; s < SQUARE_NB; ++s) {
            CAN_PROMOTE_TO_MASK[c][s] = PROMO_ZONE_BB[c].test(s) ? ALL_BB : PROMO_ZONE_BB[c];
        }
        for (int pt = 0; pt < PT_NB; ++pt) {
            MUST_PROMOTE_BB[c][pt] = Bitboard{};
            DROP_ALLOWED_BB[c][pt] = ALL_BB;
        }
    }

    MUST_PROMOTE_BB[BLACK][PAWN]   = RANK_BB[0];
    MUST_PROMOTE_BB[BLACK][LANCE]  = RANK_BB[0];
    MUST_PROMOTE_BB[BLACK][KNIGHT] = RANK_BB[0] | RANK_BB[1];

    MUST_PROMOTE_BB[WHITE][PAWN]   = RANK_BB[8];
    MUST_PROMOTE_BB[WHITE][LANCE]  = RANK_BB[8];
    MUST_PROMOTE_BB[WHITE][KNIGHT] = RANK_BB[7] | RANK_BB[8];

    for (int c_i = 0; c_i < COLOR_NB; ++c_i) {
        const Color c = static_cast<Color>(c_i);
        for (int pt = PAWN; pt <= ROOK; ++pt) {
            DROP_ALLOWED_BB[c][pt] = ALL_BB.and_not(MUST_PROMOTE_BB[c][pt]);
        }
        for (uint32_t m9 = 0; m9 < 512u; ++m9) {
            Bitboard allowed = DROP_ALLOWED_BB[c][PAWN];
            for (int f = 0; f < FILE_NB; ++f) {
                if ((m9 >> f) & 1u) allowed = allowed.and_not(FILE_BB[f]);
            }
            PAWN_DROP_MASK_BB[c][m9] = allowed;
        }
    }

    // Step attacks
    for (int c_i = 0; c_i < COLOR_NB; ++c_i) {
        const Color c = static_cast<Color>(c_i);
        const int fwd = (c == BLACK) ? -1 : 1;
        for (int pt = 0; pt < PT_NB; ++pt) {
            for (int s = 0; s < SQUARE_NB; ++s) STEP_ATTACKS[c][pt][s] = Bitboard{};
        }
        for (int s = 0; s < SQUARE_NB; ++s) {
            const int f = file_of(s), r = rank_of(s);

            // Pawn
            add_step_if_valid(STEP_ATTACKS[c][PAWN][s], f, r + fwd);

            // Knight
            add_step_if_valid(STEP_ATTACKS[c][KNIGHT][s], f - 1, r + 2 * fwd);
            add_step_if_valid(STEP_ATTACKS[c][KNIGHT][s], f + 1, r + 2 * fwd);

            // Silver
            add_step_if_valid(STEP_ATTACKS[c][SILVER][s], f,     r + fwd);
            add_step_if_valid(STEP_ATTACKS[c][SILVER][s], f - 1, r + fwd);
            add_step_if_valid(STEP_ATTACKS[c][SILVER][s], f + 1, r + fwd);
            add_step_if_valid(STEP_ATTACKS[c][SILVER][s], f - 1, r - fwd);
            add_step_if_valid(STEP_ATTACKS[c][SILVER][s], f + 1, r - fwd);

            // Gold & promoted minors
            Bitboard g{};
            add_step_if_valid(g, f,     r + fwd);
            add_step_if_valid(g, f - 1, r + fwd);
            add_step_if_valid(g, f + 1, r + fwd);
            add_step_if_valid(g, f - 1, r);
            add_step_if_valid(g, f + 1, r);
            add_step_if_valid(g, f,     r - fwd);
            STEP_ATTACKS[c][GOLD][s]        = g;
            STEP_ATTACKS[c][PROM_PAWN][s]   = g;
            STEP_ATTACKS[c][PROM_LANCE][s]  = g;
            STEP_ATTACKS[c][PROM_KNIGHT][s] = g;
            STEP_ATTACKS[c][PROM_SILVER][s] = g;

            // King
            for (int dr = -1; dr <= 1; ++dr) {
                for (int df = -1; df <= 1; ++df) {
                    if (df != 0 || dr != 0) add_step_if_valid(STEP_ATTACKS[c][KING][s], f + df, r + dr);
                }
            }
        }
    }

    // Between & Line geometry tables
    for (int s1 = 0; s1 < SQUARE_NB; ++s1) {
        for (int s2 = 0; s2 < SQUARE_NB; ++s2) {
            BETWEEN_BB[s1][s2] = Bitboard{};
            LINE_BB[s1][s2]    = Bitboard{};
            if (s1 == s2) continue;
            const int f1 = file_of(s1), r1 = rank_of(s1);
            const int f2 = file_of(s2), r2 = rank_of(s2);
            const int df = f2 - f1, dr = r2 - r1;
            if (df != 0 && dr != 0 && std::abs(df) != std::abs(dr)) continue;

            const int sf = (df > 0) - (df < 0);
            const int sr = (dr > 0) - (dr < 0);

            for (int nf = f1 + sf, nr = r1 + sr; make_sq(nf, nr) != s2; nf += sf, nr += sr) {
                BETWEEN_BB[s1][s2] |= SQ_BB[make_sq(nf, nr)];
            }
            for (int nf = f1, nr = r1; on_board(nf, nr); nf += sf, nr += sr) {
                LINE_BB[s1][s2] |= SQ_BB[make_sq(nf, nr)];
            }
            for (int nf = f1 - sf, nr = r1 - sr; on_board(nf, nr); nf -= sf, nr -= sr) {
                LINE_BB[s1][s2] |= SQ_BB[make_sq(nf, nr)];
            }
        }
    }

    // Rook Rank attacks (`occ7` = inner files 1..7 on rank `r`)
    for (int s = 0; s < SQUARE_NB; ++s) {
        const int f = file_of(s), r = rank_of(s);
        for (uint32_t occ7 = 0; occ7 < 128u; ++occ7) {
            Bitboard att{};
            for (int nf = f + 1; nf < FILE_NB; ++nf) {
                att |= SQ_BB[make_sq(nf, r)];
                if (nf >= 1 && nf <= 7 && ((occ7 >> (nf - 1)) & 1u)) break;
            }
            for (int nf = f - 1; nf >= 0; --nf) {
                att |= SQ_BB[make_sq(nf, r)];
                if (nf >= 1 && nf <= 7 && ((occ7 >> (nf - 1)) & 1u)) break;
            }
            ROOK_RANK_ATTACK_TABLE[s][occ7] = att;
        }
    }

    // Rook File & Lance attacks via `mid63` file PEXT
    for (int f = 0; f < FILE_NB; ++f) {
        FILE_MID_MASK[f] = 0x0040201008040201ULL << f; // r=1..7 -> sq = 9+f .. 63+f -> mid bit f .. f+54
    }
    for (int s = 0; s < SQUARE_NB; ++s) {
        const int f = file_of(s), r = rank_of(s);
        const uint64_t fmask = FILE_MID_MASK[f];
        uint64_t sub = 0ULL;
        do {
            const uint32_t idx = static_cast<uint32_t>(bb_pext64(sub, fmask));
            Bitboard up_bb{}, dn_bb{};
            for (int nr = r - 1; nr >= 0; --nr) {
                const Square ns = make_sq(f, nr);
                up_bb |= SQ_BB[ns];
                if (nr >= 1 && nr <= 7 && ((sub >> (ns - 9)) & 1ULL)) break;
            }
            for (int nr = r + 1; nr < RANK_NB; ++nr) {
                const Square ns = make_sq(f, nr);
                dn_bb |= SQ_BB[ns];
                if (nr >= 1 && nr <= 7 && ((sub >> (ns - 9)) & 1ULL)) break;
            }
            ROOK_FILE_ATTACK_TABLE[s][idx]     = up_bb | dn_bb;
            LANCE_ATTACK_TABLE[BLACK][s][idx]  = up_bb;
            LANCE_ATTACK_TABLE[WHITE][s][idx]  = dn_bb;

            sub = (sub - fmask) & fmask;
        } while (sub != 0ULL);

        PSEUDO_ROOK_BB[s]         = ROOK_RANK_ATTACK_TABLE[s][0] | ROOK_FILE_ATTACK_TABLE[s][0];
        PSEUDO_LANCE_BB[BLACK][s] = LANCE_ATTACK_TABLE[BLACK][s][0];
        PSEUDO_LANCE_BB[WHITE][s] = LANCE_ATTACK_TABLE[WHITE][s][0];
    }

    // Bishop attacks via `mid63` diagonal PEXT
    uint32_t offset = 0;
    static constexpr int DDF[4] = {1, 1, -1, -1};
    static constexpr int DDR[4] = {1, -1, 1, -1};

    for (int s = 0; s < SQUARE_NB; ++s) {
        const int f = file_of(s), r = rank_of(s);
        uint64_t bmask = 0ULL;
        for (int d = 0; d < 4; ++d) {
            for (int nf = f + DDF[d], nr = r + DDR[d]; on_board(nf, nr); nf += DDF[d], nr += DDR[d]) {
                if (nf >= 1 && nf <= 7 && nr >= 1 && nr <= 7) {
                    const Square ns = make_sq(nf, nr);
                    bmask |= (1ULL << (ns - 9));
                }
            }
        }
        BISHOP_MID_MASK[s] = bmask;
        BISHOP_OFFSET[s]   = offset;
        const uint32_t entries = 1u << bb_popcnt64(bmask);
        assert(offset + entries <= BISHOP_TABLE_SIZE);

        uint64_t sub = 0ULL;
        do {
            const uint32_t idx = static_cast<uint32_t>(bb_pext64(sub, bmask));
            Bitboard att{};
            for (int d = 0; d < 4; ++d) {
                for (int nf = f + DDF[d], nr = r + DDR[d]; on_board(nf, nr); nf += DDF[d], nr += DDR[d]) {
                    const Square ns = make_sq(nf, nr);
                    att |= SQ_BB[ns];
                    if (nf >= 1 && nf <= 7 && nr >= 1 && nr <= 7 && ((sub >> (ns - 9)) & 1ULL)) {
                        break;
                    }
                }
            }
            BISHOP_ATTACK_TABLE[offset + idx] = att;
            sub = (sub - bmask) & bmask;
        } while (sub != 0ULL);

        PSEUDO_BISHOP_BB[s] = BISHOP_ATTACK_TABLE[offset];
        offset += entries;
    }
}

} // namespace Bitboards

// ============================================================
// Zobrist tables
// ============================================================
namespace Zobrist {
    uint64_t piece_sq[PIECE_NB][SQUARE_NB];
    uint64_t hand   [COLOR_NB][PT_NB][MAX_HAND + 1];
    uint64_t side;

    static uint64_t rng_state = 0xdeadbeefcafe1234ULL;
    static uint64_t rng64() {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        return rng_state;
    }

    void init() {
        Bitboards::init();
        rng_state = 0xdeadbeefcafe1234ULL;
        for (int p = 0; p < PIECE_NB; p++)
            for (int s = 0; s < SQUARE_NB; s++)
                piece_sq[p][s] = rng64();

        for (int c = 0; c < COLOR_NB; c++)
            for (int pt = 0; pt < PT_NB; pt++) {
                hand[c][pt][0] = 0; // empty hand = 0 contribution
                for (int n = 1; n <= MAX_HAND; n++)
                    hand[c][pt][n] = rng64();
            }

        side = rng64();
    }
}

// ============================================================
// Board constructor / reset
// ============================================================
Board::Board() {
    Bitboards::init();
    history_.reserve(256);
    pos_hashes_.reserve(512);
    reset();
}

Board::Board(const Board& o)
    : stm_(o.stm_),
      ply_(o.ply_),
      hash_(o.hash_),
      material_black_(o.material_black_),
      golds_bb_(o.golds_bb_),
      bishops_bb_(o.bishops_bb_),
      rooks_bb_(o.rooks_bb_),
      checkers_(o.checkers_),
      pin_info_valid_(o.pin_info_valid_),
      history_(o.history_),
      pos_hashes_(o.pos_hashes_) {
    std::memcpy(board_, o.board_, sizeof(board_));
    std::memcpy(hand_, o.hand_, sizeof(hand_));
    std::memcpy(king_sq_, o.king_sq_, sizeof(king_sq_));
    for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = o.by_color_[i];
    for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = o.by_type_[i];
    for (int i = 0; i < COLOR_NB; ++i) blockers_king_[i] = o.blockers_king_[i];
}

Board::Board(Board&& o) noexcept
    : stm_(o.stm_),
      ply_(o.ply_),
      hash_(o.hash_),
      material_black_(o.material_black_),
      golds_bb_(o.golds_bb_),
      bishops_bb_(o.bishops_bb_),
      rooks_bb_(o.rooks_bb_),
      checkers_(o.checkers_),
      pin_info_valid_(o.pin_info_valid_),
      history_(std::move(o.history_)),
      pos_hashes_(std::move(o.pos_hashes_)) {
    std::memcpy(board_, o.board_, sizeof(board_));
    std::memcpy(hand_, o.hand_, sizeof(hand_));
    std::memcpy(king_sq_, o.king_sq_, sizeof(king_sq_));
    for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = o.by_color_[i];
    for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = o.by_type_[i];
    for (int i = 0; i < COLOR_NB; ++i) blockers_king_[i] = o.blockers_king_[i];
}

Board& Board::operator=(const Board& o) {
    if (this != &o) {
        std::memcpy(board_, o.board_, sizeof(board_));
        std::memcpy(hand_, o.hand_, sizeof(hand_));
        stm_ = o.stm_;
        ply_ = o.ply_;
        std::memcpy(king_sq_, o.king_sq_, sizeof(king_sq_));
        hash_ = o.hash_;
        material_black_ = o.material_black_;
        for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = o.by_color_[i];
        for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = o.by_type_[i];
        golds_bb_       = o.golds_bb_;
        bishops_bb_     = o.bishops_bb_;
        rooks_bb_       = o.rooks_bb_;
        checkers_       = o.checkers_;
        for (int i = 0; i < COLOR_NB; ++i) blockers_king_[i] = o.blockers_king_[i];
        pin_info_valid_ = o.pin_info_valid_;
        history_        = o.history_;
        pos_hashes_     = o.pos_hashes_;
    }
    return *this;
}

Board& Board::operator=(Board&& o) noexcept {
    if (this != &o) {
        std::memcpy(board_, o.board_, sizeof(board_));
        std::memcpy(hand_, o.hand_, sizeof(hand_));
        stm_ = o.stm_;
        ply_ = o.ply_;
        std::memcpy(king_sq_, o.king_sq_, sizeof(king_sq_));
        hash_ = o.hash_;
        material_black_ = o.material_black_;
        for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = o.by_color_[i];
        for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = o.by_type_[i];
        golds_bb_       = o.golds_bb_;
        bishops_bb_     = o.bishops_bb_;
        rooks_bb_       = o.rooks_bb_;
        checkers_       = o.checkers_;
        for (int i = 0; i < COLOR_NB; ++i) blockers_king_[i] = o.blockers_king_[i];
        pin_info_valid_ = o.pin_info_valid_;
        history_        = std::move(o.history_);
        pos_hashes_     = std::move(o.pos_hashes_);
    }
    return *this;
}

void Board::reset() {
    std::memset(board_, 0, sizeof(board_));
    std::memset(hand_,  0, sizeof(hand_));
    for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = Bitboard{};
    for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = Bitboard{};
    golds_bb_              = Bitboard{};
    bishops_bb_            = Bitboard{};
    rooks_bb_              = Bitboard{};
    checkers_              = Bitboard{};
    blockers_king_[BLACK]  = Bitboard{};
    blockers_king_[WHITE]  = Bitboard{};
    pin_info_valid_        = false;
    stm_                   = BLACK;
    ply_                   = 0;
    hash_                  = 0;
    material_black_        = 0;
    king_sq_[BLACK]        = SQ_NONE;
    king_sq_[WHITE]        = SQ_NONE;
    history_.clear();
    pos_hashes_.clear();
}

// ============================================================
// Zobrist hash, material & bitboard recomputation from scratch
// ============================================================
void Board::compute_hash_and_material() {
    hash_           = 0;
    material_black_ = 0;
    for (int i = 0; i < COLOR_NB; ++i) by_color_[i] = Bitboard{};
    for (int i = 0; i < PT_NB; ++i)    by_type_[i]  = Bitboard{};
    golds_bb_   = Bitboard{};
    bishops_bb_ = Bitboard{};
    rooks_bb_   = Bitboard{};

    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board_[s];
        if (p != NO_PIECE) {
            const Color c = color_of(p);
            const PieceType pt = type_of(p);
            put_piece_bb(c, pt, s);
            hash_ ^= Zobrist::piece_sq[p][s];
            int v = PIECE_VALUE_BONANZA[pt];
            material_black_ += (c == BLACK) ? v : -v;
        }
    }
    for (int c = 0; c < COLOR_NB; c++) {
        for (int pt = 0; pt < PT_NB; pt++) {
            int cnt = hand_[c][pt];
            if (cnt >= 0 && cnt <= Zobrist::MAX_HAND)
                hash_ ^= Zobrist::hand[c][pt][cnt];
            if (pt >= PAWN && pt <= ROOK && cnt != 0) {
                int v = PIECE_VALUE_BONANZA[pt] * cnt;
                material_black_ += (c == BLACK) ? v : -v;
            }
        }
    }
    if (stm_ == WHITE)
        hash_ ^= Zobrist::side;

    const Square ksq = king_sq_[stm_];
    checkers_       = (ksq != SQ_NONE) ? attackers_to(ksq, ~stm_, by_type_[NO_PT]) : Bitboard{};
    pin_info_valid_ = false;
}

// ============================================================
// set_startpos  –  standard shogi starting position
// ============================================================
void Board::set_startpos() {
    parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
}

// ============================================================
// parse_sfen
// ============================================================
bool Board::parse_sfen(const std::string& sfen) {
    Bitboards::init();
    reset();

    std::istringstream ss(sfen);
    std::string board_str, stm_str, hand_str, move_cnt_str;
    ss >> board_str >> stm_str >> hand_str >> move_cnt_str;

    int r = 0, f = 0;
    bool promo = false;
    for (char c : board_str) {
        if (c == '/') { r++; f = 0; promo = false; continue; }
        if (c == '+') { promo = true; continue; }
        if (c >= '1' && c <= '9') {
            f += (c - '0'); promo = false; continue;
        }
        Color     pc = std::isupper(static_cast<unsigned char>(c)) ? BLACK : WHITE;
        PieceType pt = char_to_pt(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        if (promo) pt = promote_pt(pt);
        promo = false;
        Square sq = make_sq(f, r);
        board_[sq] = make_piece(pc, pt);
        if (pt == KING) king_sq_[pc] = sq;
        f++;
    }

    stm_ = (stm_str == "b") ? BLACK : WHITE;

    if (hand_str != "-") {
        int cnt = 0;
        for (char c : hand_str) {
            if (c >= '0' && c <= '9') {
                cnt = cnt * 10 + (c - '0');
            } else {
                if (cnt == 0) cnt = 1;
                Color     hc = std::isupper(static_cast<unsigned char>(c)) ? BLACK : WHITE;
                PieceType pt = char_to_pt(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                hand_[hc][pt] += cnt;
                cnt = 0;
            }
        }
    }

    if (!move_cnt_str.empty()) {
        try {
            int mc = std::stoi(move_cnt_str);
            ply_ = (mc > 0) ? (mc - 1) : 0;
        } catch (...) {}
    }

    compute_hash_and_material();
    pos_hashes_.push_back(hash_);
    return true;
}

// ============================================================
// to_sfen
// ============================================================
std::string Board::to_sfen() const {
    std::string out;

    for (int rank = 0; rank < RANK_NB; rank++) {
        if (rank > 0) out += '/';
        int empty = 0;
        for (int file = 0; file < FILE_NB; file++) {
            Piece p = board_[make_sq(file, rank)];
            if (p == NO_PIECE) { empty++; continue; }
            if (empty > 0) { out += static_cast<char>('0' + empty); empty = 0; }
            PieceType pt = type_of(p);
            Color     pc = color_of(p);
            if (is_promoted(pt)) { out += '+'; pt = unpromote_pt(pt); }
            char ch = pt_to_char(pt);
            out += (pc == BLACK) ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                                 : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (empty > 0) out += static_cast<char>('0' + empty);
    }

    out += ' ';
    out += (stm_ == BLACK) ? 'b' : 'w';

    out += ' ';
    static const PieceType HAND_ORDER[] = {ROOK, BISHOP, GOLD, SILVER, KNIGHT, LANCE, PAWN};
    bool any = false;
    for (Color c : {BLACK, WHITE}) {
        for (PieceType pt : HAND_ORDER) {
            int cnt = hand_[c][pt];
            if (cnt == 0) continue;
            any = true;
            if (cnt > 1) out += std::to_string(cnt);
            char ch = pt_to_char(pt);
            out += (c == BLACK) ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))
                                : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    if (!any) out += '-';

    out += ' ';
    out += std::to_string(ply_ + 1);

    return out;
}

// ============================================================
// do_move / undo_move / null_move (Incremental Bitboard Updates)
// ============================================================
void Board::do_move(Move m, bool record_check) {
    StateInfo si;
    si.hash            = hash_;
    si.material_black  = material_black_;
    si.captured        = NO_PIECE;
    si.move            = m;
    si.checkers_before = checkers_;
    si.pin_info_valid  = pin_info_valid_;
    if (pin_info_valid_) {
        si.blockers_king[BLACK] = blockers_king_[BLACK];
        si.blockers_king[WHITE] = blockers_king_[WHITE];
    }

    const Color us   = stm_;
    const Color them = ~us;
    si.mover = us;
    const int sign = (us == BLACK) ? 1 : -1;

    if (is_drop(m)) {
        const PieceType pt = dropped_pt(m);
        const Square    to = to_sq(m);

        si.hand_cnt_before = static_cast<uint8_t>(hand_[us][pt]);

        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];
        hand_[us][pt]--;
        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];

        const Piece p = make_piece(us, pt);
        board_[to] = p;
        put_piece_bb(us, pt, to);
        hash_ ^= Zobrist::piece_sq[p][to];
        si.moved_piece = p;
    } else {
        const Square    from      = from_sq(m);
        const Square    to        = to_sq(m);
        Piece           moving    = board_[from];
        const Piece     captured  = board_[to];
        const PieceType orig_type = type_of(moving);

        si.captured = captured;

        board_[from] = NO_PIECE;
        hash_ ^= Zobrist::piece_sq[moving][from];

        if (captured != NO_PIECE) {
            hash_ ^= Zobrist::piece_sq[captured][to];
            const PieceType cap_pt   = type_of(captured);
            const PieceType cap_base = to_hand_pt(cap_pt);
            remove_piece_bb(them, cap_pt, to);

            si.hand_cnt_before = static_cast<uint8_t>(hand_[us][cap_base]);
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
            hand_[us][cap_base]++;
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
            material_black_ += sign * (PIECE_VALUE_BONANZA[cap_pt] + PIECE_VALUE_BONANZA[cap_base]);
        }

        if (is_promote(m)) {
            const PieceType prom_type = promote_pt(orig_type);
            remove_piece_bb(us, orig_type, from);
            put_piece_bb(us, prom_type, to);
            moving = make_piece(us, prom_type);
            material_black_ += sign * (PIECE_VALUE_BONANZA[prom_type] - PIECE_VALUE_BONANZA[orig_type]);
        } else {
            move_piece_same_pt_bb(us, orig_type, from, to);
        }

        board_[to] = moving;
        hash_ ^= Zobrist::piece_sq[moving][to];
        si.moved_piece = moving;

        if (orig_type == KING) king_sq_[us] = to;
    }

    hash_ ^= Zobrist::side;
    stm_  = them;
    ply_++;

    const Square opp_ksq = king_sq_[them];
    checkers_       = (opp_ksq != SQ_NONE) ? attackers_to(opp_ksq, us, by_type_[NO_PT]) : Bitboard{};
    pin_info_valid_ = false;
    si.gave_check   = record_check && static_cast<bool>(checkers_);

    history_.push_back(si);
    pos_hashes_.push_back(hash_);
}

void Board::undo_move(Move m) {
    const Color them = stm_;
    const Color us   = ~them;

    stm_ = us;
    ply_--;

    const StateInfo si = history_.back();
    history_.pop_back();
    pos_hashes_.pop_back();

    hash_           = si.hash;
    material_black_ = si.material_black;
    checkers_       = si.checkers_before;
    pin_info_valid_ = si.pin_info_valid;
    if (pin_info_valid_) {
        blockers_king_[BLACK] = si.blockers_king[BLACK];
        blockers_king_[WHITE] = si.blockers_king[WHITE];
    }

    if (is_drop(m)) {
        const PieceType pt = dropped_pt(m);
        const Square    to = to_sq(m);

        board_[to] = NO_PIECE;
        remove_piece_bb(us, pt, to);
        hand_[us][pt]++;
    } else {
        const Square    from       = from_sq(m);
        const Square    to         = to_sq(m);
        const Piece     moved_now  = board_[to];
        const PieceType after_type = type_of(moved_now);
        const PieceType orig_type  = is_promote(m) ? unpromote_pt(after_type) : after_type;

        if (is_promote(m)) {
            remove_piece_bb(us, after_type, to);
            put_piece_bb(us, orig_type, from);
        } else {
            move_piece_same_pt_bb(us, orig_type, to, from);
        }

        board_[from] = make_piece(us, orig_type);
        if (orig_type == KING) king_sq_[us] = from;

        board_[to] = si.captured;
        if (si.captured != NO_PIECE) {
            const PieceType cap_pt   = type_of(si.captured);
            const PieceType cap_base = to_hand_pt(cap_pt);
            put_piece_bb(them, cap_pt, to);
            hand_[us][cap_base]--;
        }
    }
}

void Board::do_null_move() {
    StateInfo si;
    si.hash            = hash_;
    si.material_black  = material_black_;
    si.mover           = stm_;
    si.null_move       = true;
    si.checkers_before = checkers_;
    si.pin_info_valid  = pin_info_valid_;
    if (pin_info_valid_) {
        si.blockers_king[BLACK] = blockers_king_[BLACK];
        si.blockers_king[WHITE] = blockers_king_[WHITE];
    }

    stm_  = ~stm_;
    hash_ ^= Zobrist::side;
    ply_++;

    const Square ksq = king_sq_[stm_];
    checkers_ = (ksq != SQ_NONE) ? attackers_to(ksq, ~stm_, by_type_[NO_PT]) : Bitboard{};
    // Notice board occupancy & piece positions did NOT change on null move, so `blockers_king_` stays valid!
    history_.push_back(si);
    pos_hashes_.push_back(hash_);
}

void Board::undo_null_move() {
    const StateInfo si = history_.back();
    history_.pop_back();
    pos_hashes_.pop_back();
    ply_--;
    hash_           = si.hash;
    material_black_ = si.material_black;
    stm_            = ~stm_;
    checkers_       = si.checkers_before;
    pin_info_valid_ = si.pin_info_valid;
    if (pin_info_valid_) {
        blockers_king_[BLACK] = si.blockers_king[BLACK];
        blockers_king_[WHITE] = si.blockers_king[WHITE];
    }
}

// ============================================================
// Bitboard Attack, Checker & Slider-Blocker Queries
// ============================================================
Bitboard Board::attackers_to(Square sq, Color by, const Bitboard& occ) const noexcept {
    if (static_cast<unsigned>(sq) >= static_cast<unsigned>(SQUARE_NB)) return Bitboard{};
    const Color opp = ~by;
    const Bitboard side_bb = by_color_[by];

    Bitboard res = (Bitboards::STEP_ATTACKS[opp][PAWN][sq]        & by_type_[PAWN])
                 | (Bitboards::STEP_ATTACKS[opp][KNIGHT][sq]      & by_type_[KNIGHT])
                 | (Bitboards::STEP_ATTACKS[opp][SILVER][sq]      & by_type_[SILVER])
                 | (Bitboards::STEP_ATTACKS[opp][GOLD][sq]        & golds_bb_)
                 | (Bitboards::STEP_ATTACKS[BLACK][KING][sq]      & (by_type_[KING] | by_type_[PROM_BISHOP] | by_type_[PROM_ROOK]));

    const Bitboard side_lances = side_bb & by_type_[LANCE];
    if (side_lances.intersects(Bitboards::PSEUDO_LANCE_BB[opp][sq])) {
        res |= (Bitboards::lance_attacks(opp, sq, occ) & by_type_[LANCE]);
    }
    const Bitboard side_bh = side_bb & bishops_bb_;
    if (side_bh.intersects(Bitboards::PSEUDO_BISHOP_BB[sq])) {
        res |= (Bitboards::bishop_attacks(sq, occ) & bishops_bb_);
    }
    const Bitboard side_rd = side_bb & rooks_bb_;
    if (side_rd.intersects(Bitboards::PSEUDO_ROOK_BB[sq])) {
        res |= (Bitboards::rook_attacks(sq, occ) & rooks_bb_);
    }
    return res & side_bb;
}

Bitboard Board::attackers_to_both(Square sq, const Bitboard& occ) const noexcept {
    if (static_cast<unsigned>(sq) >= static_cast<unsigned>(SQUARE_NB)) return Bitboard{};
    const Bitboard b_bb = by_color_[BLACK];
    const Bitboard w_bb = by_color_[WHITE];

    Bitboard b_step = (Bitboards::STEP_ATTACKS[WHITE][PAWN][sq]   & by_type_[PAWN])
                    | (Bitboards::STEP_ATTACKS[WHITE][KNIGHT][sq] & by_type_[KNIGHT])
                    | (Bitboards::STEP_ATTACKS[WHITE][SILVER][sq] & by_type_[SILVER])
                    | (Bitboards::STEP_ATTACKS[WHITE][GOLD][sq]   & golds_bb_);
    Bitboard w_step = (Bitboards::STEP_ATTACKS[BLACK][PAWN][sq]   & by_type_[PAWN])
                    | (Bitboards::STEP_ATTACKS[BLACK][KNIGHT][sq] & by_type_[KNIGHT])
                    | (Bitboards::STEP_ATTACKS[BLACK][SILVER][sq] & by_type_[SILVER])
                    | (Bitboards::STEP_ATTACKS[BLACK][GOLD][sq]   & golds_bb_);

    Bitboard res = (b_step & b_bb) | (w_step & w_bb)
                 | (Bitboards::STEP_ATTACKS[BLACK][KING][sq] & (by_type_[KING] | by_type_[PROM_BISHOP] | by_type_[PROM_ROOK]));

    const Bitboard lances = by_type_[LANCE];
    if ((lances & b_bb).intersects(Bitboards::PSEUDO_LANCE_BB[WHITE][sq])) {
        res |= (Bitboards::lance_attacks(WHITE, sq, occ) & lances & b_bb);
    }
    if ((lances & w_bb).intersects(Bitboards::PSEUDO_LANCE_BB[BLACK][sq])) {
        res |= (Bitboards::lance_attacks(BLACK, sq, occ) & lances & w_bb);
    }
    if (bishops_bb_.intersects(Bitboards::PSEUDO_BISHOP_BB[sq])) {
        res |= (Bitboards::bishop_attacks(sq, occ) & bishops_bb_);
    }
    if (rooks_bb_.intersects(Bitboards::PSEUDO_ROOK_BB[sq])) {
        res |= (Bitboards::rook_attacks(sq, occ) & rooks_bb_);
    }
    return res;
}

bool Board::is_attacked_occ(Square sq, Color by, const Bitboard& occ) const noexcept {
    if (static_cast<unsigned>(sq) >= static_cast<unsigned>(SQUARE_NB)) return false;
    const Color opp = ~by;
    const Bitboard side_bb = by_color_[by] & occ;

    const Bitboard step_hit =
          (Bitboards::STEP_ATTACKS[opp][PAWN][sq]   & by_type_[PAWN])
        | (Bitboards::STEP_ATTACKS[opp][KNIGHT][sq] & by_type_[KNIGHT])
        | (Bitboards::STEP_ATTACKS[opp][SILVER][sq] & by_type_[SILVER])
        | (Bitboards::STEP_ATTACKS[opp][GOLD][sq]   & golds_bb_)
        | (Bitboards::STEP_ATTACKS[BLACK][KING][sq] & (by_type_[KING] | by_type_[PROM_BISHOP] | by_type_[PROM_ROOK]));
    if (side_bb.intersects(step_hit)) return true;

    const Bitboard side_bh = side_bb & bishops_bb_;
    if (side_bh.intersects(Bitboards::PSEUDO_BISHOP_BB[sq])) {
        if (side_bh.intersects(Bitboards::bishop_attacks(sq, occ))) return true;
    }
    const Bitboard side_rd = side_bb & rooks_bb_;
    if (side_rd.intersects(Bitboards::PSEUDO_ROOK_BB[sq])) {
        if (side_rd.intersects(Bitboards::rook_attacks(sq, occ))) return true;
    }
    const Bitboard side_lances = side_bb & by_type_[LANCE];
    if (side_lances.intersects(Bitboards::PSEUDO_LANCE_BB[opp][sq])) {
        if (side_lances.intersects(Bitboards::lance_attacks(opp, sq, occ))) return true;
    }
    return false;
}

bool Board::is_attacked_ignoring(Square sq, Color by, Square ignore_sq) const {
    if (static_cast<unsigned>(sq) >= static_cast<unsigned>(SQUARE_NB)) return false;
    if (static_cast<unsigned>(ignore_sq) < static_cast<unsigned>(SQUARE_NB)) {
        return is_attacked_occ(sq, by, by_type_[NO_PT].and_not(Bitboards::SQ_BB[ignore_sq]));
    }
    return is_attacked_occ(sq, by, by_type_[NO_PT]);
}

bool Board::is_attacked(Square sq, Color by) const {
    return is_attacked_occ(sq, by, by_type_[NO_PT]);
}

Bitboard Board::slider_blockers(Color king_color) const noexcept {
    const Square ksq = king_sq_[king_color];
    if (ksq == SQ_NONE) return Bitboard{};

    const Color attacker = ~king_color;
    const Bitboard att_side = by_color_[attacker];
    const Bitboard snipers = att_side & (
          (Bitboards::PSEUDO_ROOK_BB[ksq]               & rooks_bb_)
        | (Bitboards::PSEUDO_BISHOP_BB[ksq]             & bishops_bb_)
        | (Bitboards::PSEUDO_LANCE_BB[king_color][ksq]  & by_type_[LANCE])
    );

    if (snipers.empty()) return Bitboard{};

    Bitboard blockers{};
    const Bitboard occ = by_type_[NO_PT];
    snipers.for_each([&](Square sniper_sq) {
        const Bitboard b = Bitboards::BETWEEN_BB[ksq][sniper_sq] & occ;
        if (b.is_single_bit()) {
            blockers |= b;
        }
    });
    return blockers;
}

void Board::compute_pin_info() const noexcept {
    blockers_king_[BLACK] = slider_blockers(BLACK);
    blockers_king_[WHITE] = slider_blockers(WHITE);
    pin_info_valid_       = true;
}

// ============================================================
// repetition_count / repetition_result
// ============================================================
int Board::repetition_count() const {
    int cnt = 0;
    for (uint64_t h : pos_hashes_)
        if (h == hash_) cnt++;
    return cnt;
}

RepetitionResult Board::repetition_result(bool* repetition_sensitive) const {
    if (repetition_sensitive) *repetition_sensitive = false;
    size_t occurrences[4] = {};
    size_t count = 0;
    for (size_t i = pos_hashes_.size(); i > 0 && count < 4; --i) {
        if (pos_hashes_[i - 1] == hash_) {
            if (repetition_sensitive && count > 0) *repetition_sensitive = true;
            occurrences[3 - count++] = i - 1;
        }
    }
    if (count < 4)
        return RepetitionResult::NONE;

    const size_t first = occurrences[0];
    bool moved[COLOR_NB] = {false, false};
    bool checked_every_move[COLOR_NB] = {true, true};

    for (size_t i = first; i + 1 < pos_hashes_.size(); ++i) {
        if (i >= history_.size() || history_[i].null_move)
            return RepetitionResult::NONE;
        const StateInfo& state = history_[i];
        moved[state.mover] = true;
        checked_every_move[state.mover] =
            checked_every_move[state.mover] && state.gave_check;
    }

    Color checker = BLACK;
    if (moved[BLACK] && checked_every_move[BLACK] &&
        !(moved[WHITE] && checked_every_move[WHITE])) {
        checker = BLACK;
    } else if (moved[WHITE] && checked_every_move[WHITE] &&
               !(moved[BLACK] && checked_every_move[BLACK])) {
        checker = WHITE;
    } else {
        return RepetitionResult::DRAW;
    }

    return checker == stm_ ? RepetitionResult::SIDE_TO_MOVE_LOSES
                           : RepetitionResult::SIDE_TO_MOVE_WINS;
}

// ============================================================
// print  (debug)
// ============================================================
void Board::print() const {
    std::cout << "  9 8 7 6 5 4 3 2 1\n";
    for (int rank = 0; rank < RANK_NB; rank++) {
        std::cout << static_cast<char>('a' + rank) << ' ';
        for (int file = 0; file < FILE_NB; file++) {
            Piece p = board_[make_sq(file, rank)];
            if (p == NO_PIECE) { std::cout << ". "; continue; }
            PieceType pt = type_of(p);
            bool prom = is_promoted(pt);
            if (prom) pt = unpromote_pt(pt);
            char ch = pt_to_char(pt);
            if (prom) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (color_of(p) == BLACK) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            std::cout << ch << ' ';
        }
        std::cout << '\n';
    }
    std::cout << "Side: " << (stm_ == BLACK ? "Black" : "White") << '\n';
}

// ============================================================
// move_to_usi / usi_to_move
// ============================================================
std::string move_to_usi(Move m) {
    if (is_drop(m)) {
        std::string s;
        s += static_cast<char>(std::toupper(static_cast<unsigned char>(pt_to_char(dropped_pt(m)))));
        s += '*';
        s += sq_to_usi(to_sq(m));
        return s;
    }
    std::string s = sq_to_usi(from_sq(m)) + sq_to_usi(to_sq(m));
    if (is_promote(m)) s += '+';
    return s;
}

Move usi_to_move(const std::string& s) {
    if (s.size() >= 4 && s[1] == '*') {
        PieceType pt = char_to_pt(static_cast<char>(std::toupper(static_cast<unsigned char>(s[0]))));
        Square    to = usi_to_sq(s[2], s[3]);
        return make_drop_move(pt, to);
    }
    Square from  = usi_to_sq(s[0], s[1]);
    Square to    = usi_to_sq(s[2], s[3]);
    bool   promo = (s.size() >= 5 && s[4] == '+');
    return promo ? make_promote_move(from, to) : make_normal_move(from, to);
}
