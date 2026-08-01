#pragma once
#include "board.hpp"
#include <string>

// Returns a score in centipawns from the perspective of the side to move.
// Positive = good for the current player.
int evaluate(const Board& board);

// Optional external evaluation file path (plain text key=value format).
// Missing/invalid file safely falls back to built-in lightweight KPP evaluation.
void set_eval_file_path(const std::string& path);
std::string eval_status_message();
