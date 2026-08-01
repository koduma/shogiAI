#pragma once
#include "board.hpp"
#include <string>

// ============================================================
// Evaluator family / status API
// ============================================================
// The family that is *currently active* after lazy loading.
//   KPP_TABLE         – a compatible ShogiAI-KPP-v1 table is loaded and used
//   MATERIAL_FALLBACK – no compatible KPP table is active; material only
//   NNUE_UNSUPPORTED  – an NNUE-labelled file was detected; material fallback used
// This engine does not implement NNUE inference.
enum class EvalFamily {
    KPP_TABLE,
    MATERIAL_FALLBACK,
    NNUE_UNSUPPORTED,
};

// Returns a score in centipawns from the perspective of the side to move.
// Positive = good for the current player.
int evaluate(const Board& board);

// Set the path to an external evaluation file.
// Call with an empty string to reset and re-enable automatic discovery.
// Missing/invalid/unsupported files fall back safely to material-only evaluation.
void set_eval_file_path(const std::string& path);

// Returns a human-readable status string describing the active evaluator.
std::string eval_status_message();

// Returns the active evaluator family (see EvalFamily above).
EvalFamily get_eval_family();
