#pragma once
#include <cstdint>
#include <string>
#include <cassert>

// ============================================================
// Square
//   sq = rank * 9 + file   (rank-major)
//   file : 0..8  (0 = USI file '9' / leftmost,  8 = USI file '1' / rightmost)
//   rank : 0..8  (0 = USI rank 'a' / top,        8 = USI rank 'i' / bottom)
// ============================================================
using Square = int;
constexpr Square SQ_NONE = -1;
constexpr int FILE_NB   = 9;
constexpr int RANK_NB   = 9;
constexpr int SQUARE_NB = 81;

inline int    file_of  (Square s) { return s % FILE_NB; }
inline int    rank_of  (Square s) { return s / FILE_NB; }
inline Square make_sq  (int f, int r) { return r * FILE_NB + f; }
inline bool   on_board (int f, int r) {
    return static_cast<unsigned>(f) < static_cast<unsigned>(FILE_NB) &&
           static_cast<unsigned>(r) < static_cast<unsigned>(RANK_NB);
}

// USI file '1'–'9'  →  internal file 8–0
// USI rank 'a'–'i'  →  internal rank 0–8
inline Square usi_to_sq(char fc, char rc) {
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
inline Color operator~(Color c) { return static_cast<Color>(c ^ 1); }

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

inline PieceType promote_pt  (PieceType p) { return static_cast<PieceType>(p + 8); }
inline PieceType unpromote_pt(PieceType p) { return static_cast<PieceType>(p - 8); }
inline bool is_promoted   (PieceType p) { return p >= PROM_PAWN; }
// Can this (unpromoted) type be promoted?  Gold and King cannot.
inline bool is_promotable (PieceType p) { return p >= PAWN && p <= ROOK && p != GOLD && p != KING; }
// Base piece type used for hand storage (strips promotion)
inline PieceType to_hand_pt(PieceType p) { return is_promoted(p) ? unpromote_pt(p) : p; }

// USI character for a base piece type (PAWN–KING)
inline char pt_to_char(PieceType p) {
    constexpr const char* TBL = ".PLNSGBRK";
    return TBL[static_cast<int>(p)];
}
inline PieceType char_to_pt(char c) {
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

inline Color     color_of (Piece p) { return static_cast<Color>    (p >> 4); }
inline PieceType type_of  (Piece p) { return static_cast<PieceType>(p & 0xF); }
inline Piece make_piece(Color c, PieceType pt) {
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

inline Square    from_sq   (Move m) { return static_cast<Square>   ( m        & 0x7Fu); }
inline Square    to_sq     (Move m) { return static_cast<Square>   ((m >>  7) & 0x7Fu); }
inline bool      is_promote(Move m) { return static_cast<bool>     ((m >> 14) & 1u);   }
inline bool      is_drop   (Move m) { return static_cast<bool>     ((m >> 15) & 1u);   }
inline PieceType dropped_pt(Move m) { return static_cast<PieceType>((m >> 16) & 0xFu); }

inline Move make_normal_move (Square from, Square to) {
    return static_cast<Move>(static_cast<uint32_t>(from) | (static_cast<uint32_t>(to) << 7));
}
inline Move make_promote_move(Square from, Square to) {
    return static_cast<Move>(static_cast<uint32_t>(from) | (static_cast<uint32_t>(to) << 7) | (1u << 14));
}
inline Move make_drop_move(PieceType pt, Square to) {
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
