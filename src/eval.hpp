#pragma once
#include "board.hpp"
#include <cstdint>
#include <string>

// ============================================================
// Evaluator family
// ============================================================
enum class EvalFamily {
    SFNN_NNUE,          // YaneuraOu SFNNWithoutPsqt nn.bin (+ progress.bin) active
    MATERIAL_FALLBACK   // Material-only fallback (when nn.bin is 0-byte dummy or missing)
};

// Returns a score in centipawns from the perspective of the side to move.
// Uses incremental K-A2 NNUE accumulators + incremental progress.bin Q16 sum + EvalHash.
int evaluate(const Board& board);

// Full from-scratch scalar reference evaluation (bypassing incremental/SIMD/EvalHash caches),
// exposed for unit-test bit-exact equivalence verification.
int evaluate_full_reference(const Board& board);

// Returns learned game progress in [0.0, 1.0] (using progress.bin when loaded,
// or a material/king-advancement phase estimate when in fallback mode).
double evaluate_progress(const Board& board);

// Returns the active progress LayerStack bucket (0 .. layer_stacks-1).
int evaluate_progress_bucket(const Board& board);

// Configure evaluation directory / file paths:
//   - set_eval_dir(dir): sets directory containing nn.bin and progress.bin (USI `EvalDir`)
//   - set_eval_file_path(path): sets explicit nn.bin path (USI `EvalFile`; empty resets auto-discovery)
//   - set_progress_file_path(path): sets explicit progress.bin path (USI `ProgressFile`; empty uses sibling of nn.bin)
void set_eval_dir(const std::string& dir);
void set_eval_file_path(const std::string& path);
void set_progress_file_path(const std::string& path);

// Configure/query FV_SCALE (default 24 for TsuyouraOu11mini SFNN_ka2_4096_8_64_progress8).
void set_fv_scale(int scale);
int  get_fv_scale();

// Clear evaluation hash & incremental accumulator caches (e.g. on usinewgame).
void clear_eval_caches();

// Monotonic configuration epoch incremented whenever eval files or FV_SCALE change,
// allowing search TT to invalidate stale cached static evaluations automatically.
uint32_t get_eval_config_epoch();

// Returns a human-readable status string describing the active evaluator and progress model.
std::string eval_status_message();

// Returns the active evaluator family (see EvalFamily above).
EvalFamily get_eval_family();
