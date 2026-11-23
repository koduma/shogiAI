#pragma once
// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif

#include "types.hpp"
#include <cstdint>

#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

// ============================================================
// Low-level bit manipulation intrinsics (BMI1/BMI2/POPCNT + portable fallbacks)
// ============================================================
inline int bb_ctz64(uint64_t x) noexcept {
#if defined(_MSC_VER)
    unsigned long idx = 0;
    _BitScanForward64(&idx, x);
    return static_cast<int>(idx);
#else
    return __builtin_ctzll(x);
#endif
}

inline int bb_popcnt64(uint64_t x) noexcept {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(x));
#else
    return __builtin_popcountll(x);
#endif
}

inline uint64_t bb_pext64(uint64_t val, uint64_t mask) noexcept {
#if defined(__BMI2__) && (defined(__x86_64__) || defined(_M_X64))
    return _pext_u64(val, mask);
#else
    uint64_t res = 0;
    uint64_t bit = 1;
    while (mask) {
        if (val & (mask & (~mask + 1ULL))) res |= bit;
        mask &= (mask - 1ULL);
        bit <<= 1;
    }
    return res;
#endif
}

// ============================================================
// 81-Square Shogi Bitboard (YaneuraOu-style 63 + 18 split in 128-bit XMM)
//   p[0] : bits 0..62 -> squares  0..62 (ranks 0..6 = 7 * 9 = 63 squares; bit 63 always 0)
//   p[1] : bits 0..17 -> squares 63..80 (ranks 7..8 = 2 * 9 = 18 squares; bits 18..63 always 0)
// ============================================================
struct alignas(16) Bitboard {
    static constexpr uint64_t MASK_P0 = 0x7FFFFFFFFFFFFFFFULL; // (1ULL << 63) - 1
    static constexpr uint64_t MASK_P1 = 0x000000000003FFFFULL; // (1ULL << 18) - 1

    uint64_t p[2];

    constexpr Bitboard() noexcept : p{0ULL, 0ULL} {}
    constexpr Bitboard(uint64_t p0, uint64_t p1) noexcept : p{p0, p1} {}

    inline bool empty() const noexcept {
#if defined(__SSE4_1__) || defined(__AVX2__)
        const __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        return _mm_testz_si128(v, v) != 0;
#else
        return (p[0] | p[1]) == 0ULL;
#endif
    }

    inline explicit operator bool() const noexcept {
        return !empty();
    }

    inline bool operator==(const Bitboard& o) const noexcept {
        return p[0] == o.p[0] && p[1] == o.p[1];
    }
    inline bool operator!=(const Bitboard& o) const noexcept {
        return p[0] != o.p[0] || p[1] != o.p[1];
    }

    inline Bitboard operator|(const Bitboard& o) const noexcept {
#if defined(__SSE2__) || defined(__AVX2__)
        Bitboard r;
        const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(o.p));
        _mm_store_si128(reinterpret_cast<__m128i*>(r.p), _mm_or_si128(a, b));
        return r;
#else
        return Bitboard(p[0] | o.p[0], p[1] | o.p[1]);
#endif
    }

    inline Bitboard operator&(const Bitboard& o) const noexcept {
#if defined(__SSE2__) || defined(__AVX2__)
        Bitboard r;
        const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(o.p));
        _mm_store_si128(reinterpret_cast<__m128i*>(r.p), _mm_and_si128(a, b));
        return r;
#else
        return Bitboard(p[0] & o.p[0], p[1] & o.p[1]);
#endif
    }

    inline Bitboard operator^(const Bitboard& o) const noexcept {
#if defined(__SSE2__) || defined(__AVX2__)
        Bitboard r;
        const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(o.p));
        _mm_store_si128(reinterpret_cast<__m128i*>(r.p), _mm_xor_si128(a, b));
        return r;
#else
        return Bitboard(p[0] ^ o.p[0], p[1] ^ o.p[1]);
#endif
    }

    inline Bitboard operator~() const noexcept {
        return Bitboard((~p[0]) & MASK_P0, (~p[1]) & MASK_P1);
    }

    // Fast `(*this) & ~o` preserving zeroed upper bits in 1 XMM instruction
    inline Bitboard and_not(const Bitboard& o) const noexcept {
#if defined(__SSE2__) || defined(__AVX2__)
        Bitboard r;
        const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(o.p));
        _mm_store_si128(reinterpret_cast<__m128i*>(r.p), _mm_andnot_si128(b, a));
        return r;
#else
        return Bitboard(p[0] & ~o.p[0], p[1] & ~o.p[1]);
#endif
    }

    inline Bitboard& operator|=(const Bitboard& o) noexcept {
        *this = *this | o;
        return *this;
    }
    inline Bitboard& operator&=(const Bitboard& o) noexcept {
        *this = *this & o;
        return *this;
    }
    inline Bitboard& operator^=(const Bitboard& o) noexcept {
        *this = *this ^ o;
        return *this;
    }

    inline bool intersects(const Bitboard& o) const noexcept {
#if defined(__SSE4_1__) || defined(__AVX2__)
        const __m128i a = _mm_load_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i b = _mm_load_si128(reinterpret_cast<const __m128i*>(o.p));
        return _mm_testz_si128(a, b) == 0;
#else
        return ((p[0] & o.p[0]) | (p[1] & o.p[1])) != 0ULL;
#endif
    }

    inline bool test(Square s) const noexcept;

    inline bool is_single_bit() const noexcept {
        const uint64_t a = p[0];
        const uint64_t b = p[1];
        if (a != 0ULL) return b == 0ULL && (a & (a - 1ULL)) == 0ULL;
        return b != 0ULL && (b & (b - 1ULL)) == 0ULL;
    }

    inline bool more_than_one() const noexcept {
        const uint64_t a = p[0];
        const uint64_t b = p[1];
        if (a != 0ULL) return (b != 0ULL) || ((a & (a - 1ULL)) != 0ULL);
        return (b & (b - 1ULL)) != 0ULL;
    }

    inline int count() const noexcept {
        return bb_popcnt64(p[0]) + bb_popcnt64(p[1]);
    }

    inline Square lsb() const noexcept {
        return (p[0] != 0ULL) ? static_cast<Square>(bb_ctz64(p[0]))
                              : static_cast<Square>(63 + bb_ctz64(p[1]));
    }

    inline Square pop_lsb() noexcept {
        if (p[0] != 0ULL) {
            const int s = bb_ctz64(p[0]);
            p[0] &= (p[0] - 1ULL);
            return static_cast<Square>(s);
        }
        const int s = 63 + bb_ctz64(p[1]);
        p[1] &= (p[1] - 1ULL);
        return static_cast<Square>(s);
    }

    template <typename Fn>
    inline void for_each(Fn&& fn) const {
        uint64_t b0 = p[0];
        while (b0 != 0ULL) {
            fn(static_cast<Square>(bb_ctz64(b0)));
            b0 &= (b0 - 1ULL);
        }
        uint64_t b1 = p[1];
        while (b1 != 0ULL) {
            fn(static_cast<Square>(63 + bb_ctz64(b1)));
            b1 &= (b1 - 1ULL);
        }
    }
};

// ============================================================
// Precomputed Bitboard & Sliding Attack Tables (L1/L2 Cache-Resident)
//   Total sliding table footprint < 1 MiB so zero L3 cache contention with NNUE weights!
// ============================================================
namespace Bitboards {

constexpr size_t BISHOP_TABLE_SIZE = 24576;

extern Bitboard ALL_BB;
extern Bitboard SQ_BB[SQUARE_NB];
extern Bitboard FILE_BB[FILE_NB];
extern Bitboard RANK_BB[RANK_NB];
extern Bitboard PROMO_ZONE_BB[COLOR_NB];
extern Bitboard CAN_PROMOTE_TO_MASK[COLOR_NB][SQUARE_NB];
extern Bitboard MUST_PROMOTE_BB[COLOR_NB][PT_NB];
extern Bitboard DROP_ALLOWED_BB[COLOR_NB][PT_NB];
extern Bitboard PAWN_DROP_MASK_BB[COLOR_NB][512];

extern Bitboard STEP_ATTACKS[COLOR_NB][PT_NB][SQUARE_NB];
extern Bitboard PSEUDO_ROOK_BB[SQUARE_NB];
extern Bitboard PSEUDO_BISHOP_BB[SQUARE_NB];
extern Bitboard PSEUDO_LANCE_BB[COLOR_NB][SQUARE_NB];

extern Bitboard BETWEEN_BB[SQUARE_NB][SQUARE_NB];
extern Bitboard LINE_BB[SQUARE_NB][SQUARE_NB];

extern uint64_t FILE_MID_MASK[FILE_NB];
extern uint8_t  RANK_SHIFT[RANK_NB];
extern Bitboard LANCE_ATTACK_TABLE[COLOR_NB][SQUARE_NB][128];
extern Bitboard ROOK_RANK_ATTACK_TABLE[SQUARE_NB][128];
extern Bitboard ROOK_FILE_ATTACK_TABLE[SQUARE_NB][128];

extern uint64_t BISHOP_MID_MASK[SQUARE_NB];
extern uint32_t BISHOP_OFFSET[SQUARE_NB];
extern Bitboard BISHOP_ATTACK_TABLE[BISHOP_TABLE_SIZE];

void init();

inline const Bitboard& sq_bb(Square s) noexcept {
    return SQ_BB[s];
}

// Extracts all 63 squares of inner ranks 1..7 (`sq = 9..71`) into a single 64-bit word!
inline uint64_t mid63(const Bitboard& occ) noexcept {
    return (occ.p[0] >> 9) | ((occ.p[1] & 0x1FFULL) << 54);
}

inline uint32_t rank_occ7(int r, const Bitboard& occ) noexcept {
    const uint64_t w = occ.p[r >= 7 ? 1 : 0];
    return static_cast<uint32_t>((w >> RANK_SHIFT[r]) & 0x7Fu);
}

inline uint32_t file_occ7_from_mid(int f, uint64_t mid) noexcept {
    return static_cast<uint32_t>(bb_pext64(mid, FILE_MID_MASK[f]));
}

inline const Bitboard& lance_attacks(Color c, Square sq, const Bitboard& occ) noexcept {
    return LANCE_ATTACK_TABLE[c][sq][file_occ7_from_mid(file_of(sq), mid63(occ))];
}

inline const Bitboard& bishop_attacks(Square sq, const Bitboard& occ) noexcept {
    const uint64_t idx = bb_pext64(mid63(occ), BISHOP_MID_MASK[sq]);
    return BISHOP_ATTACK_TABLE[BISHOP_OFFSET[sq] + idx];
}

inline Bitboard rook_attacks(Square sq, const Bitboard& occ) noexcept {
    const int f = file_of(sq);
    const int r = rank_of(sq);
    return ROOK_RANK_ATTACK_TABLE[sq][rank_occ7(r, occ)] |
           ROOK_FILE_ATTACK_TABLE[sq][file_occ7_from_mid(f, mid63(occ))];
}

inline Bitboard horse_attacks(Square sq, const Bitboard& occ) noexcept {
    return bishop_attacks(sq, occ) | STEP_ATTACKS[BLACK][KING][sq];
}

inline Bitboard dragon_attacks(Square sq, const Bitboard& occ) noexcept {
    return rook_attacks(sq, occ) | STEP_ATTACKS[BLACK][KING][sq];
}

inline Bitboard attacks_from(Color c, PieceType pt, Square sq, const Bitboard& occ) noexcept {
    switch (pt) {
        case PAWN:
        case KNIGHT:
        case SILVER:
        case GOLD:
        case KING:
        case PROM_PAWN:
        case PROM_LANCE:
        case PROM_KNIGHT:
        case PROM_SILVER:
            return STEP_ATTACKS[c][pt][sq];
        case LANCE:
            return lance_attacks(c, sq, occ);
        case BISHOP:
            return bishop_attacks(sq, occ);
        case ROOK:
            return rook_attacks(sq, occ);
        case PROM_BISHOP:
            return horse_attacks(sq, occ);
        case PROM_ROOK:
            return dragon_attacks(sq, occ);
        default:
            return Bitboard{};
    }
}

// Batch pawn forward attacks for ALL pawns in `pawns` in 3 instructions!
inline Bitboard pawn_attacks_bb(Color c, const Bitboard& pawns) noexcept {
    if (c == BLACK) {
        return Bitboard(
            (pawns.p[0] >> 9) | ((pawns.p[1] & 0x1FFULL) << 54),
            pawns.p[1] >> 9);
    } else {
        return Bitboard(
            (pawns.p[0] << 9) & Bitboard::MASK_P0,
            ((pawns.p[0] >> 54) & 0x1FFULL) | ((pawns.p[1] << 9) & Bitboard::MASK_P1));
    }
}

// O(1) Nifu 9-bit file mask (`bit f == 1` iff `pawns` has a pawn on internal file `f`)
inline uint32_t pawn_files_mask9(const Bitboard& pawns) noexcept {
    uint64_t x = pawns.p[0] | pawns.p[1];
    x |= (x >> 36);
    x |= (x >> 18);
    return static_cast<uint32_t>((x | (x >> 9)) & 0x1FFu);
}

} // namespace Bitboards

inline bool Bitboard::test(Square s) const noexcept {
    return intersects(Bitboards::SQ_BB[s]);
}
inline void bb_set(Bitboard& bb, Square s) noexcept {
    bb |= Bitboards::SQ_BB[s];
}
inline void bb_clear(Bitboard& bb, Square s) noexcept {
    bb = bb.and_not(Bitboards::SQ_BB[s]);
}
inline void bb_xor(Bitboard& bb, Square s) noexcept {
    bb ^= Bitboards::SQ_BB[s];
}
