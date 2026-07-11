#pragma once
#include "board.hpp"

// Returns a score in centipawns from the perspective of the side to move.
// Positive = good for the current player.
int evaluate(const Board& board);
