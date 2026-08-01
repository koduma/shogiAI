#include "eval.hpp"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// -----------------------------------------------------------------------
// Automatic discovery candidates (tried in order when no explicit path is
// set via setoption/set_eval_file_path/SHOGIAI_EVAL_FILE).
//
// Paths are relative to the working directory at runtime:
//   src/eval/kpp_weights.txt  – preferred; found when running from the
//                               repository root or when CMake copies it.
//   eval/kpp_weights.txt      – found when running from the build directory
//                               (CMake copies src/eval/ → build/eval/).
// -----------------------------------------------------------------------
static const char* const DISCOVERY_CANDIDATES[] = {
    "src/eval/kpp_weights.txt",
    "eval/kpp_weights.txt",
};

// Internal family tag used while loading (maps to public EvalFamily later).
enum class LoadFamily { KPP, NNUE, UNSUPPORTED };

struct EvalParams {
    std::array<int, PT_NB> piece_value{
        0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
    };
    int kpp_weight = 12;
    int king_zone_bonus = 6;
};

// Global state
EvalParams  g_params;
bool        g_loaded_once  = false;
bool        g_explicit_path = false; // true if set via set_eval_file_path(non-empty)
std::string g_eval_file_path;        // empty = use auto-discovery
std::string g_status       = "kpp: built-in fallback";
EvalFamily  g_family       = EvalFamily::FALLBACK;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------
inline int manhattan_dist(Square a, Square b) {
    return std::abs(file_of(a) - file_of(b)) + std::abs(rank_of(a) - rank_of(b));
}

// Compact KPP-style king-piece-piece interaction score for one side.
int kpp_term_for_side(const Board& board, Color c, const EvalParams& params) {
    std::array<Square, 40> feat_sq{};
    int n = 0;

    const Square king = board.king_sq(c);
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != c || type_of(p) == KING) continue;
        if (n < static_cast<int>(feat_sq.size())) feat_sq[n++] = s;
    }

    // Hand pieces are modeled as "virtual nearby pieces" so holdings affect KPP.
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        int count = board.hand(c, static_cast<PieceType>(pt));
        while (count-- > 0 && n < static_cast<int>(feat_sq.size())) {
            int f = std::clamp(file_of(king) + ((pt % 2) ? 1 : -1), 0, 8);
            int r = std::clamp(rank_of(king) + ((pt % 3) ? 1 : -1), 0, 8);
            feat_sq[n++] = make_sq(f, r);
        }
    }

    int acc = 0;
    for (int i = 0; i < n; ++i) {
        const int d1 = manhattan_dist(king, feat_sq[i]);
        acc += std::max(0, 6 - d1) * params.king_zone_bonus;
        for (int j = i + 1; j < n; ++j) {
            const int d2 = manhattan_dist(king, feat_sq[j]);
            acc += std::max(0, 8 - (d1 + d2)) * params.kpp_weight;
        }
    }
    return acc;
}

// Parse the model_type value string into a LoadFamily tag.
LoadFamily parse_model_type(const std::string& val) {
    if (val == "kpp" || val == "three-piece-relation") return LoadFamily::KPP;
    if (val == "nnue")                                  return LoadFamily::NNUE;
    return LoadFamily::UNSUPPORTED;
}

// -----------------------------------------------------------------------
// Load one candidate file.
// is_auto: true when called from auto-discovery (affects status message).
// Returns true if the file was successfully loaded as KPP.
// Returns false on: file not found, parse error, or unsupported model type.
// On failure, g_status is updated only when !is_auto (explicit path error).
// -----------------------------------------------------------------------
bool try_load_from_path(const std::string& path, bool is_auto) {
    std::ifstream ifs(path);
    if (!ifs) {
        if (!is_auto) {
            g_status = "kpp: built-in fallback (file not found: " + path + ")";
            g_family = EvalFamily::FALLBACK;
        }
        return false;
    }

    EvalParams loaded = EvalParams{}; // start from built-in defaults
    std::string line;
    int loaded_values = 0;
    LoadFamily file_family = LoadFamily::KPP; // default: KPP if no model_type key
    bool family_declared = false;

    while (std::getline(ifs, line)) {
        // Strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = line.substr(0, eq);
        // Strip leading/trailing whitespace from value
        const std::string raw_val = line.substr(eq + 1);
        const auto v0 = raw_val.find_first_not_of(" \t");
        const auto v1 = raw_val.find_last_not_of(" \t\r\n");
        const std::string val = (v0 == std::string::npos)
                                ? std::string{}
                                : raw_val.substr(v0, v1 - v0 + 1);

        // ---- model_type declaration ----
        if (key == "model_type") {
            file_family = parse_model_type(val);
            family_declared = true;
            if (file_family == LoadFamily::NNUE || file_family == LoadFamily::UNSUPPORTED) {
                // Unsupported evaluator family – report and fall back.
                const std::string tag = is_auto ? "auto-discovered" : "explicit";
                const std::string family_name = (file_family == LoadFamily::NNUE) ? "nnue" : val;
                g_status = "unsupported evaluator: " + family_name
                         + " (falling back to built-in kpp; source: " + path
                         + " [" + tag + "])";
                g_family = EvalFamily::NNUE_UNSUPPORTED;
                // Stop auto-discovery even for NNUE; user placed this file intentionally.
                return false;
            }
            continue;
        }

        // ---- numeric parameters ----
        int parsed = 0;
        {
            std::istringstream iss(val);
            if (!(iss >> parsed)) continue;
        }

        if (key == "kpp_weight")      { loaded.kpp_weight      = parsed; loaded_values++; continue; }
        if (key == "king_zone_bonus") { loaded.king_zone_bonus  = parsed; loaded_values++; continue; }
        if (key.rfind("piece_", 0) == 0) {
            const std::string pt_name = key.substr(6);
            PieceType t = NO_PT;
            if      (pt_name == "pawn")        t = PAWN;
            else if (pt_name == "lance")       t = LANCE;
            else if (pt_name == "knight")      t = KNIGHT;
            else if (pt_name == "silver")      t = SILVER;
            else if (pt_name == "gold")        t = GOLD;
            else if (pt_name == "bishop")      t = BISHOP;
            else if (pt_name == "rook")        t = ROOK;
            else if (pt_name == "prom_pawn")   t = PROM_PAWN;
            else if (pt_name == "prom_lance")  t = PROM_LANCE;
            else if (pt_name == "prom_knight") t = PROM_KNIGHT;
            else if (pt_name == "prom_silver") t = PROM_SILVER;
            else if (pt_name == "prom_bishop") t = PROM_BISHOP;
            else if (pt_name == "prom_rook")   t = PROM_ROOK;
            if (t != NO_PT) { loaded.piece_value[t] = parsed; loaded_values++; }
        }
    }

    (void)family_declared; // used only for model_type detection above

    if (loaded_values == 0) {
        if (!is_auto) {
            g_status = "kpp: built-in fallback (parse error: " + path + ")";
            g_family = EvalFamily::FALLBACK;
        }
        return false;
    }

    g_params = loaded;
    const std::string tag = is_auto ? "auto-discovered" : "explicit";
    g_status = "kpp: loaded from " + path + " [" + tag + "]";
    g_family = EvalFamily::KPP;
    return true;
}

// -----------------------------------------------------------------------
// Load on first use (lazy, called before every evaluate() call).
// Priority:
//   1. Explicit path set via set_eval_file_path(non-empty)
//   2. SHOGIAI_EVAL_FILE environment variable
//   3. Auto-discovery (DISCOVERY_CANDIDATES in order)
//   4. Built-in KPP fallback
// -----------------------------------------------------------------------
void try_load_eval_file_once() {
    if (g_loaded_once) return;
    g_loaded_once = true;

    // Priority 1: explicit setoption / set_eval_file_path
    if (g_explicit_path && !g_eval_file_path.empty()) {
        try_load_from_path(g_eval_file_path, /*is_auto=*/false);
        return;
    }

    // Priority 2: environment variable (explicit but lower than setoption)
    {
        const char* env_path = std::getenv("SHOGIAI_EVAL_FILE");
        if (env_path && *env_path) {
            try_load_from_path(std::string(env_path), /*is_auto=*/false);
            return;
        }
    }

    // Priority 3: auto-discovery
    for (const char* candidate : DISCOVERY_CANDIDATES) {
        bool ok = try_load_from_path(candidate, /*is_auto=*/true);
        if (ok || g_family == EvalFamily::NNUE_UNSUPPORTED) {
            // Stop on success OR on an explicitly identified unsupported file.
            return;
        }
    }

    // Priority 4: built-in fallback
    g_status = "kpp: built-in fallback"
               " (no compatible eval file found;"
               " auto-discovery checked: src/eval/kpp_weights.txt,"
               " eval/kpp_weights.txt)";
    g_family = EvalFamily::FALLBACK;
}

} // namespace

// ============================================================
// Public API
// ============================================================

void set_eval_file_path(const std::string& path) {
    g_eval_file_path  = path;
    g_explicit_path   = !path.empty();
    g_loaded_once     = false;
    g_params          = EvalParams{};
    g_status          = "kpp: built-in fallback";
    g_family          = EvalFamily::FALLBACK;
}

std::string eval_status_message() {
    try_load_eval_file_once();
    return g_status;
}

EvalFamily get_eval_family() {
    try_load_eval_file_once();
    return g_family;
}

int evaluate(const Board& board) {
    try_load_eval_file_once();

    const Color us = board.side_to_move();
    int score = 0;

    // Material (board + hand)
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE) continue;
        int v = g_params.piece_value[type_of(p)];
        score += (color_of(p) == us) ? v : -v;
    }
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        int v = g_params.piece_value[pt];
        score += board.hand(us,  static_cast<PieceType>(pt)) * v;
        score -= board.hand(~us, static_cast<PieceType>(pt)) * v;
    }

    // KPP-style king-piece-piece interaction (symmetric)
    int us_kpp  = kpp_term_for_side(board, us,  g_params);
    int opp_kpp = kpp_term_for_side(board, ~us, g_params);
    score += us_kpp - opp_kpp;

    return score;
}

