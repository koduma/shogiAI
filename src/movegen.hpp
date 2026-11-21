#pragma once
#include "board.hpp"

// Generate all legal moves for the current side to move.
// Handles: nifu, forced-promotion zones, pawn-drop-mate.
void generate_legal_moves(Board& board, MoveList& out);

// Generate only legal tactical moves (captures and promotions) when NOT in check.
// Used by MovePicker and quiescence search to avoid generating ~150 quiet/drop moves.
void generate_tactical_moves(Board& board, MoveList& out);

// Generate only legal quiet moves (non-captures, non-promotions, and drops) when NOT in check.
void generate_quiet_moves(Board& board, MoveList& out);

// Generate legal non-tactical checking moves (including drop checks) when NOT in check.
// Used at the first ply of quiescence search to prevent check/drop-mate horizon blindness.
void generate_quiet_checks(Board& board, MoveList& out);

// O(1) validation whether an arbitrary Move (e.g. TT / killer / countermove) is legal
// in the current position without generating the full move list.
bool is_move_legal(Board& board, Move m, bool currently_in_check);

// Fast check whether playing legal move `m` delivers check to the opponent's king.
bool move_gives_check(const Board& board, Move m);

// Shogi Static Exchange Evaluation (SEE): returns true iff the material+hand exchange
// sequence initiated by `m` on `to_sq(m)` achieves net gain >= `threshold` (centipawns).
bool see_ge(const Board& board, Move m, int threshold = 0);

// Fast 1-ply checkmate detector (mate1ply). Assumes `!board.in_check()`.
// If a legal move delivers immediate checkmate, writes it to `*mate_move` (if non-null)
// and returns true.
bool find_mate_1ply(Board& board, Move* mate_move = nullptr);

// Returns true if the current side to move is in checkmate.
bool is_checkmate(Board& board);

// Returns true if the current side to move has no legal moves (includes stalemate).
bool has_no_moves(Board& board);
