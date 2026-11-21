#pragma once
#include "board.hpp"
#include <string>

// ============================================================
// Evaluator family
// ============================================================
enum class EvalFamily {
    BONANZA_V6_FV,      // Bonanza v6 fv.bin loaded; 3-piece relation active
    MATERIAL_FALLBACK,  // Material-only; no KPP/relation term
    NNUE_UNSUPPORTED    // NNUE file detected; not supported; material fallback used
};

// Returns a score in centipawns from the perspective of the side to move.
// Uses incremental KPP/KKP state + evaluation hash cache when BONANZA_V6_FV is active.
int evaluate(const Board& board);

// Full from-scratch reference evaluation (bypassing incremental/ehash caches),
// exposed for unit-test equivalence verification.
int evaluate_full_reference(const Board& board);

// Set the path to the Bonanza 6.0 fv.bin file explicitly.
// Priority: setoption EvalFile > SHOGIAI_EVAL_FILE env > auto-discovery.
void set_eval_file_path(const std::string& path);

// Configure/query FV_SCALE (default 32 for Bonanza 6.0 fv.bin; USI-configurable like YaneuraOu).
void set_fv_scale(int scale);
int  get_fv_scale();

// Clear evaluation hash/incremental caches (e.g. on usinewgame).
void clear_eval_caches();

// Returns a human-readable status string describing the active evaluator.
std::string eval_status_message();

// Returns the active evaluator family (see EvalFamily above).
EvalFamily get_eval_family();
