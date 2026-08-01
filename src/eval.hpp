#pragma once
#include "board.hpp"
#include <string>

// ============================================================
// Evaluator family
// ============================================================
// The family that is *currently active* (after loading).
//   KPP      – compact King-Piece-Piece style evaluator (built-in or from file)
//   NNUE     – Neural Network Unified Evaluator (file detected but NOT supported yet)
//   FALLBACK – built-in KPP defaults, no external file loaded
// NNUE is exposed as a structured extension point; it will never silently
// fall through as KPP – if an NNUE file is detected the status reflects
// NNUE_UNSUPPORTED and the engine uses the KPP fallback instead.
enum class EvalFamily {
    FALLBACK,       // Built-in KPP defaults (no file loaded)
    KPP,            // KPP params loaded from an external file
    NNUE_UNSUPPORTED // NNUE file detected; unsupported – KPP fallback in use
};

// Returns a score in centipawns from the perspective of the side to move.
// Positive = good for the current player.
int evaluate(const Board& board);

// Set the path to an external evaluation parameter file (plain text key=value).
// Call with an empty string to reset and re-enable automatic discovery.
// Missing/invalid/unsupported files fall back to built-in KPP evaluation safely.
void set_eval_file_path(const std::string& path);

// Returns a human-readable status string describing the active evaluator.
// Possible prefixes:
//   "kpp: built-in fallback"           – no file found or parse error
//   "kpp: loaded from <path> [...]"    – KPP params loaded from file
//   "unsupported evaluator: nnue ..."  – NNUE file detected, KPP fallback used
std::string eval_status_message();

// Returns the active evaluator family (see EvalFamily above).
EvalFamily get_eval_family();
