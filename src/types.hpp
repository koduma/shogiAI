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
#include <array>
#include <cstdint>
#include <string>
#include <cassert>

// ============================================================
// Square
//   sq = rank * 9 + file   (rank-major, identical to Bonanza A9=0 .. I1=80)
//   file : 0..8  (0 = USI file '9' / leftmost,  8 = USI file '1' / rightmost)
//   rank : 0..8  (0 = USI rank 'a' / top,        8 = USI rank 'i' / bottom)
// ============================================================
using Square = int;
constexpr Square SQ_NONE = -1;
constexpr int FILE_NB   = 9;
constexpr int RANK_NB   = 9;
constexpr int SQUARE_NB = 81;

inline constexpr int    file_of  (Square s) { return s % FILE_NB; }
inline constexpr int    rank_of  (Square s) { return s / FILE_NB; }
inline constexpr Square make_sq  (int f, int r) { return r * FILE_NB + f; }
inline constexpr bool   on_board (int f, int r) {
    return static_cast<unsigned>(f) < static_cast<unsigned>(FILE_NB) &&
           static_cast<unsigned>(r) < static_cast<unsigned>(RANK_NB);
}

// USI file '1'–'9'  →  internal file 8–0
// USI rank 'a'–'i'  →  internal rank 0–8
inline constexpr Square usi_to_sq(char fc, char rc) {
    return make_sq(FILE_NB - 1 - (fc - '1'), rc - 'a');
}
inline std::string sq_to_usi(Square s) {
    std::string out;
    out += static_cast<char>('1' + FILE_NB - 1 - file_of(s));
    out += static_cast<char>('a' + rank_of(s));
    return out;
}

// ============================================================
// Color
// ============================================================
enum Color : int { BLACK = 0, WHITE = 1, COLOR_NB = 2 };
inline constexpr Color operator~(Color c) { return static_cast<Color>(c ^ 1); }

// ============================================================
// PieceType
// ============================================================
enum PieceType : int {
    NO_PT      = 0,
    PAWN       = 1,  LANCE      = 2,  KNIGHT      = 3,  SILVER = 4,
    GOLD       = 5,  BISHOP     = 6,  ROOK        = 7,  KING   = 8,
    PROM_PAWN  = 9,  PROM_LANCE = 10, PROM_KNIGHT = 11, PROM_SILVER = 12,
    PROM_BISHOP= 13, PROM_ROOK  = 14,
    PT_NB      = 15
};

inline constexpr PieceType promote_pt(PieceType p) {
    switch (p) {
        case PAWN:   return PROM_PAWN;
        case LANCE:  return PROM_LANCE;
        case KNIGHT: return PROM_KNIGHT;
        case SILVER: return PROM_SILVER;
        case BISHOP: return PROM_BISHOP;
        case ROOK:   return PROM_ROOK;
        default:     return p;
    }
}
inline constexpr PieceType unpromote_pt(PieceType p) {
    switch (p) {
        case PROM_PAWN:   return PAWN;
        case PROM_LANCE:  return LANCE;
        case PROM_KNIGHT: return KNIGHT;
        case PROM_SILVER: return SILVER;
        case PROM_BISHOP: return BISHOP;
        case PROM_ROOK:   return ROOK;
        default:          return p;
    }
}
inline constexpr bool is_promoted   (PieceType p) { return p >= PROM_PAWN; }
// Can this (unpromoted) type be promoted?  Gold and King cannot.
inline constexpr bool is_promotable (PieceType p) { return p >= PAWN && p <= ROOK && p != GOLD && p != KING; }
// Base piece type used for hand storage (strips promotion)
inline constexpr PieceType to_hand_pt(PieceType p) { return is_promoted(p) ? unpromote_pt(p) : p; }

// Bonanza 6.0 piece values (src/client/param.h)
constexpr std::array<int, PT_NB> PIECE_VALUE_BONANZA = {
    0, 87, 232, 257, 369, 444, 569, 642, 0, 534, 489, 510, 495, 827, 945
};

// Shogi capture swing value: capturing `pt` removes enemy `pt` and adds `to_hand_pt(pt)` to hand.
inline constexpr int capture_swing_value(PieceType pt) {
    return (pt == KING) ? 30000
         : (pt <= NO_PT || pt >= PT_NB) ? 0
         : (PIECE_VALUE_BONANZA[pt] + PIECE_VALUE_BONANZA[to_hand_pt(pt)]);
}
inline constexpr int promote_gain_value(PieceType pt) {
    return is_promotable(pt) ? (PIECE_VALUE_BONANZA[promote_pt(pt)] - PIECE_VALUE_BONANZA[pt]) : 0;
}

// USI character for a base piece type (PAWN–KING)
inline constexpr char pt_to_char(PieceType p) {
    constexpr const char TBL[] = ".PLNSGBRK";
    const int idx = static_cast<int>(p);
    if (idx < 0 || idx > static_cast<int>(KING)) return '?';
    return TBL[idx];
}
inline constexpr PieceType char_to_pt(char c) {
    switch (c) {
        case 'P': return PAWN;   case 'L': return LANCE;
        case 'N': return KNIGHT; case 'S': return SILVER;
        case 'G': return GOLD;   case 'B': return BISHOP;
        case 'R': return ROOK;   case 'K': return KING;
        default:  return NO_PT;
    }
}

// ============================================================
// Piece  =  (color << 4) | piece_type
//   BLACK pieces :  1–14   (BLACK = 0, so top nibble = 0)
//   WHITE pieces : 17–30   (WHITE = 1, so top nibble = 1)
// ============================================================
enum Piece : int {
    NO_PIECE  = 0,
    B_PAWN=1, B_LANCE=2, B_KNIGHT=3, B_SILVER=4, B_GOLD=5,
    B_BISHOP=6, B_ROOK=7, B_KING=8,
    B_PPAWN=9, B_PLANCE=10, B_PKNIGHT=11, B_PSILVER=12,
    B_PBISHOP=13, B_PROOK=14,
    W_PAWN=17, W_LANCE=18, W_KNIGHT=19, W_SILVER=20, W_GOLD=21,
    W_BISHOP=22, W_ROOK=23, W_KING=24,
    W_PPAWN=25, W_PLANCE=26, W_PKNIGHT=27, W_PSILVER=28,
    W_PBISHOP=29, W_PROOK=30,
    PIECE_NB = 31
};

inline constexpr Color     color_of (Piece p) { return static_cast<Color>    (p >> 4); }
inline constexpr PieceType type_of  (Piece p) { return static_cast<PieceType>(p & 0xF); }
inline constexpr Piece make_piece(Color c, PieceType pt) {
    return static_cast<Piece>((c << 4) | static_cast<int>(pt));
}

// ============================================================
// Move  (32-bit packed)
//   bits  0– 6 : from-square  (0–80)
//   bits  7–13 : to-square    (0–80)
//   bit  14    : promote flag
//   bit  15    : is-drop flag
//   bits 16–19 : dropped piece type (only when is-drop set)
// ============================================================
using Move = uint32_t;
constexpr Move MOVE_NONE = 0u;

inline constexpr Square    from_sq   (Move m) { return static_cast<Square>   ( m        & 0x7Fu); }
inline constexpr Square    to_sq     (Move m) { return static_cast<Square>   ((m >>  7) & 0x7Fu); }
inline constexpr bool      is_promote(Move m) { return static_cast<bool>     ((m >> 14) & 1u);   }
inline constexpr bool      is_drop   (Move m) { return static_cast<bool>     ((m >> 15) & 1u);   }
inline constexpr PieceType dropped_pt(Move m) { return static_cast<PieceType>((m >> 16) & 0xFu); }

inline constexpr Move make_normal_move (Square from, Square to) {
    return static_cast<Move>(static_cast<uint32_t>(from) | (static_cast<uint32_t>(to) << 7));
}
inline constexpr Move make_promote_move(Square from, Square to) {
    return static_cast<Move>(static_cast<uint32_t>(from) | (static_cast<uint32_t>(to) << 7) | (1u << 14));
}
inline constexpr Move make_drop_move(PieceType pt, Square to) {
    return static_cast<Move>((1u << 15) |
                             (static_cast<uint32_t>(to) << 7) |
                             (static_cast<uint32_t>(pt) << 16));
}

// ============================================================
// Move list
// ============================================================
struct MoveList {
    static constexpr int MAX_MOVES = 600;
    Move  moves_[MAX_MOVES];
    int   size_ = 0;

    void  push (Move m)        { moves_[size_++] = m; }
    int   size ()        const { return size_; }
    bool  empty()        const { return size_ == 0; }
    Move  operator[](int i) const { return moves_[i]; }
    Move& operator[](int i)       { return moves_[i]; }
    Move* begin()              { return moves_; }
    Move* end  ()              { return moves_ + size_; }
    const Move* begin()  const { return moves_; }
    const Move* end  ()  const { return moves_ + size_; }
};

// ============================================================
// USI move string  ↔  Move
// ============================================================
std::string move_to_usi(Move m);
Move        usi_to_move(const std::string& s);
