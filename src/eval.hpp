#pragma once
#include "board.hpp"
#include <string>

// ============================================================
// Evaluator family
// ============================================================
// BONANZA_V6_FV   – Bonanza 6.0 fv.bin table-based 3-piece relation evaluator active.
//                   The user manually placed the exact unmodified Bonanza 6.0 fv.bin
//                   (from bonanza_v6.0.zip) at src/eval/fv.bin (or set EvalFile).
// MATERIAL_FALLBACK – No compatible fv.bin found; material-only evaluation.
//                   No pseudo-KPP or distance-based relation term is active.
// NNUE_UNSUPPORTED  – A file was detected whose path/name suggests NNUE; rejected.
//                   Falls back to MATERIAL_FALLBACK.
enum class EvalFamily {
    BONANZA_V6_FV,      // Bonanza v6 fv.bin loaded; 3-piece relation active
    MATERIAL_FALLBACK,  // Material-only; no KPP/relation term
    NNUE_UNSUPPORTED    // NNUE file detected; not supported; material fallback used
};

// Returns a score in centipawns from the perspective of the side to move.
// Positive = good for the current player.
int evaluate(const Board& board);

// Set the path to the Bonanza 6.0 fv.bin file explicitly.
// Priority: setoption EvalFile > SHOGIAI_EVAL_FILE env > auto-discovery.
// Call with an empty string to reset to automatic discovery.
void set_eval_file_path(const std::string& path);

// Returns a human-readable status string describing the active evaluator.
// Examples:
//   "bonanza-v6 fv.bin loaded from <absolute path> [explicit] [family=BONANZA_V6_FV]"
//   "bonanza-v6 fv.bin loaded from <absolute path> [auto-discovered] [family=BONANZA_V6_FV]"
//   "material-only fallback (fv.bin missing)"
//   "material-only fallback (invalid Bonanza v6 fv.bin size: got X bytes, expected Y)"
//   "material-only fallback (no compatible fv.bin found; auto-discovery checked: ...)"
//   "unsupported evaluator: nnue"
std::string eval_status_message();

// Returns the active evaluator family (see EvalFamily above).
EvalFamily get_eval_family();
