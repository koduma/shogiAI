#pragma once
#include "types.hpp"
#include "bitboard.hpp"
#include <vector>
#include <string>

// ============================================================
// Undo information saved before each do_move
// ============================================================
struct StateInfo {
    Piece    captured = NO_PIECE;    // piece on destination square before move
    Piece    moved_piece = NO_PIECE; // piece on destination square after move
    Move     move = MOVE_NONE;       // move executed to reach child position
    uint64_t hash = 0;               // Zobrist hash *before* the move
    int      material_black = 0;     // Black-perspective material *before* the move
    Color    mover = BLACK;
    uint8_t  hand_cnt_before = 0;    // mover's hand count of dropped/captured base piece *before* move
    bool     gave_check = false;
    bool     null_move = false;
    bool     pin_info_valid = false;
    Bitboard checkers_before{};
    Bitboard blockers_king[COLOR_NB]{};
};

enum class RepetitionResult : int {
    NONE = 0,
    DRAW,
    SIDE_TO_MOVE_WINS,
    SIDE_TO_MOVE_LOSES = -1,
};

// ============================================================
// Board (Full Bitboard + Incremental Mailbox & Pin/Checker State)
// ============================================================
class Board {
public:
    Piece    board_[SQUARE_NB];            // piece at each square
    int      hand_ [COLOR_NB][PT_NB];      // hand[color][piece_type] = count
    Color    stm_;                         // side to move
    int      ply_;                         // half-moves played so far
    Square   king_sq_[COLOR_NB];           // cached king positions
    uint64_t hash_;                        // current Zobrist hash
    int      material_black_;              // incremental Bonanza material (Black POV)

    // Incremental Bitboard state
    Bitboard by_color_[COLOR_NB];          // occupied squares per side
    Bitboard by_type_ [PT_NB];             // [NO_PT] = all occupied, [PAWN..PROM_ROOK] = per PieceType
    Bitboard golds_bb_;                    // GOLD | PROM_PAWN | PROM_LANCE | PROM_KNIGHT | PROM_SILVER
    Bitboard bishops_bb_;                  // BISHOP | PROM_BISHOP
    Bitboard rooks_bb_;                    // ROOK | PROM_ROOK
    Bitboard checkers_;                    // opponent pieces currently checking `king_sq_[stm_]`
    mutable Bitboard blockers_king_[COLOR_NB]; // sole blockers on slider rays to `king_sq_[c]`
    mutable bool     pin_info_valid_ = false;

    std::vector<StateInfo> history_;       // undo stack
    std::vector<uint64_t>  pos_hashes_;    // all hashes seen (for repetition)

    Board();
    Board(const Board& o);
    Board(Board&& o) noexcept;
    Board& operator=(const Board& o);
    Board& operator=(Board&& o) noexcept;

    // Position setup
    void reset      ();
    void set_startpos();
    bool parse_sfen (const std::string& sfen);
    std::string to_sfen() const;

    // Move execution
    void do_move       (Move m, bool record_check = true);
    void undo_move     (Move m);
    void do_null_move  ();
    void undo_null_move();

    // Queries
    Piece    piece_at      (Square s)  const noexcept { return board_[s]; }
    bool     is_empty      (Square s)  const noexcept { return board_[s] == NO_PIECE; }
    Color    side_to_move  ()          const noexcept { return stm_; }
    int      hand          (Color c, PieceType pt) const noexcept { return hand_[c][pt]; }
    Square   king_sq       (Color c)   const noexcept { return king_sq_[c]; }
    uint64_t hash          ()          const noexcept { return hash_; }
    int      material_black()          const noexcept { return material_black_; }

    // Bitboard queries
    const Bitboard& occ_all()          const noexcept { return by_type_[NO_PT]; }
    const Bitboard& pieces(Color c)    const noexcept { return by_color_[c]; }
    const Bitboard& pieces(PieceType pt) const noexcept { return by_type_[pt]; }
    Bitboard pieces(Color c, PieceType pt) const noexcept { return by_color_[c] & by_type_[pt]; }
    Bitboard gold_movers(Color c)      const noexcept { return by_color_[c] & golds_bb_; }
    Bitboard bishops_horses(Color c)   const noexcept { return by_color_[c] & bishops_bb_; }
    Bitboard rooks_dragons(Color c)    const noexcept { return by_color_[c] & rooks_bb_; }
    const Bitboard& checkers()         const noexcept { return checkers_; }

    // Fast bitboard attackers to `sq` given arbitrary occupancy `occ`
    Bitboard attackers_to(Square sq, Color by, const Bitboard& occ) const noexcept;
    Bitboard attackers_to_both(Square sq, const Bitboard& occ) const noexcept;

    // Lazy once-per-node pin & discovered-check blocker bitboards (YaneuraOu / Stockfish `slider_blockers`)
    void ensure_pin_info() const noexcept {
        if (!pin_info_valid_) compute_pin_info();
    }
    const Bitboard& blockers_for_king(Color c) const noexcept {
        ensure_pin_info();
        return blockers_king_[c];
    }
    Bitboard pinned(Color us) const noexcept {
        ensure_pin_info();
        return blockers_king_[us] & by_color_[us];
    }
    Bitboard discoverers(Color us) const noexcept {
        ensure_pin_info();
        return blockers_king_[~us] & by_color_[us];
    }

    // Attack / check detection
    bool is_attacked         (Square sq, Color by) const;
    bool is_attacked_ignoring(Square sq, Color by, Square ignore_sq) const;
    bool is_attacked_occ     (Square sq, Color by, const Bitboard& occ) const noexcept;
    bool in_check            () const noexcept { return static_cast<bool>(checkers_); }

    // Fast non-pawn material query for Null Move Pruning
    bool has_non_pawn_material(Color c) const noexcept {
        for (int pt = LANCE; pt <= ROOK; ++pt) {
            if (hand_[c][pt] > 0) return true;
        }
        const Bitboard non_pawn = by_color_[c].and_not(by_type_[KING] | by_type_[PAWN] | by_type_[PROM_PAWN]);
        return static_cast<bool>(non_pawn);
    }

    // Repetition
    int  repetition_count() const;
    RepetitionResult repetition_result(bool* repetition_sensitive = nullptr) const;

    // Debug
    void print() const;

private:
    inline void put_piece_bb(Color c, PieceType pt, Square s) noexcept {
        const Bitboard b = Bitboards::SQ_BB[s];
        by_color_[c]     |= b;
        by_type_[NO_PT]  |= b;
        by_type_[pt]     |= b;
        if (pt == GOLD || (pt >= PROM_PAWN && pt <= PROM_SILVER)) golds_bb_   |= b;
        else if (pt == BISHOP || pt == PROM_BISHOP)               bishops_bb_ |= b;
        else if (pt == ROOK   || pt == PROM_ROOK)                 rooks_bb_   |= b;
    }

    inline void remove_piece_bb(Color c, PieceType pt, Square s) noexcept {
        const Bitboard b = Bitboards::SQ_BB[s];
        by_color_[c]     = by_color_[c].and_not(b);
        by_type_[NO_PT]  = by_type_[NO_PT].and_not(b);
        by_type_[pt]     = by_type_[pt].and_not(b);
        if (pt == GOLD || (pt >= PROM_PAWN && pt <= PROM_SILVER)) golds_bb_   = golds_bb_.and_not(b);
        else if (pt == BISHOP || pt == PROM_BISHOP)               bishops_bb_ = bishops_bb_.and_not(b);
        else if (pt == ROOK   || pt == PROM_ROOK)                 rooks_bb_   = rooks_bb_.and_not(b);
    }

    inline void move_piece_same_pt_bb(Color c, PieceType pt, Square from, Square to) noexcept {
        const Bitboard fb_tb = Bitboards::SQ_BB[from] ^ Bitboards::SQ_BB[to];
        by_color_[c]     ^= fb_tb;
        by_type_[NO_PT]  ^= fb_tb;
        by_type_[pt]     ^= fb_tb;
        if (pt == GOLD || (pt >= PROM_PAWN && pt <= PROM_SILVER)) golds_bb_   ^= fb_tb;
        else if (pt == BISHOP || pt == PROM_BISHOP)               bishops_bb_ ^= fb_tb;
        else if (pt == ROOK   || pt == PROM_ROOK)                 rooks_bb_   ^= fb_tb;
    }

    void compute_hash_and_material();
    void compute_pin_info() const noexcept;
    Bitboard slider_blockers(Color king_color) const noexcept;
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
