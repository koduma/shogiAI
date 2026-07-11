#pragma once
#include "board.hpp"

// Generate all legal moves for the current side to move.
// Handles: nifu, forced-promotion zones, pawn-drop-mate.
void generate_legal_moves(Board& board, MoveList& out);

// Returns true if the current side to move is in checkmate.
bool is_checkmate(Board& board);

// Returns true if the current side to move has no legal moves (includes stalemate).
bool has_no_moves(Board& board);
