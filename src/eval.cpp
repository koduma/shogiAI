#include "eval.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

// ============================================================
// Bonanza 6.0 "fv.bin" three-piece-relation (KPP/KKP) evaluator
// ============================================================
// This file implements the classic Bonanza-style evaluation table format:
// King+Piece+Piece (KPP), King+King+Piece (KKP), King+King (KK) and
// King+Piece (KP) relation tables, loaded verbatim from an unmodified
// Bonanza 6.0 fv.bin (as distributed in bonanza_v6.0.zip).
//
// Feature space & byte layout are NOT invented for this task: they are
// taken from the publicly available Bonanza→Apery table converter
// (HiraokaTakuya/apery, utils/bonanzatoapery/main.cpp), which reads a
// genuine Bonanza 6 fv.bin with exactly this structure and these
// constants (`Bonanza::fe_end == 1476`, `Bonanza::pos_n == fe_end*(fe_end+1)/2`,
// KPPType == int16_t, KKPType/KKType/KPType == int32_t). We reproduce that
// structure here so the *actual* fv.bin bytes the user places on disk are
// interpreted the same way the reference implementation does.
//
// fv.bin byte layout (read in this exact order):
//   1. KPP : int16_t[SQUARE_NB][pos_n]                      (~168.4 MiB)
//   2. KKP : int32_t[SQUARE_NB][SQUARE_NB][fe_end]           (~36.9 MiB)
//   3. KK  : int32_t[SQUARE_NB][SQUARE_NB]                   (~25.6 KiB)
//   4. KP  : int32_t[SQUARE_NB][fe_end]                      (~466.9 KiB)
//   Total: 215,824,824 bytes (~205.8 MiB). The KPP table alone accounts
//   for ~168 MiB, which is why a real fv.bin "feels" like the commonly
//   quoted "about 170MB" file size even though the full file is larger.
//
// Because this sandbox does not have access to a real Bonanza 6.0 fv.bin,
// the exact numeric weights obviously cannot be exercised or validated
// here. What *is* validated is: (a) the file-size / structural checks
// below, using synthetically-sized files, and (b) that an all-zero table
// of the correct size loads and evaluates deterministically to 0. Users
// who place their genuine fv.bin should sanity-check `eval_status_message()`
// after startup; a size mismatch is reported explicitly rather than
// silently mis-parsed.
//
// The scoring formula implemented in evaluate() below is a single-sided
// (side-to-move-perspective) reduction of the classic Bonanza summation:
//   score = KK[my_king][opp_king]
//         + sum_i KP [my_king][feature_i]
//         + sum_i KKP[my_king][opp_king][feature_i]
//         + sum_{i<j} KPP[my_king][feature_i][feature_j]
// where `feature_i` ranges over every non-king piece (board + hand) using
// Bonanza's f_/e_ (own/enemy) encoding, mirrored to `my_king`'s point of
// view. This omits the reference engine's separate opponent-mirrored KPP
// subtraction pass; it is documented here as a deliberate simplification
// (see PR description) rather than a silent approximation, since exact
// parity with the original dual-pass Bonanza algorithm cannot be verified
// without the real weight file.

namespace {

// -----------------------------------------------------------------------
// Automatic discovery candidates (tried in order when no explicit path is
// set via setoption/set_eval_file_path/SHOGIAI_EVAL_FILE).
// -----------------------------------------------------------------------
static const char* const DISCOVERY_CANDIDATES[] = {
    "src/eval/fv.bin",
    "eval/fv.bin",
};

// -----------------------------------------------------------------------
// Bonanza feature-space constants (see file header comment for provenance).
// -----------------------------------------------------------------------
enum {
    f_hand_pawn = 0,   e_hand_pawn = 19,  f_hand_lance = 38,  e_hand_lance = 43,
    f_hand_knight = 48, e_hand_knight = 53, f_hand_silver = 58, e_hand_silver = 63,
    f_hand_gold = 68,  e_hand_gold = 73,  f_hand_bishop = 78, e_hand_bishop = 81,
    f_hand_rook = 84,  e_hand_rook = 87,  fe_hand_end = 90,

    f_pawn = 81,  e_pawn = 162,  f_lance = 225,  e_lance = 306,
    f_knight = 360, e_knight = 441, f_silver = 504, e_silver = 585,
    f_gold = 666, e_gold = 747, f_bishop = 828, e_bishop = 909,
    f_horse = 990, e_horse = 1071, f_rook = 1152, e_rook = 1233,
    f_dragon = 1314, e_dragon = 1395, fe_end = 1476
};

constexpr int64_t SQ_NB   = 81;
constexpr int64_t POS_N   = static_cast<int64_t>(fe_end) * (fe_end + 1) / 2;
constexpr int64_t KPP_CNT = SQ_NB * POS_N;
constexpr int64_t KKP_CNT = SQ_NB * SQ_NB * static_cast<int64_t>(fe_end);
constexpr int64_t KK_CNT  = SQ_NB * SQ_NB;
constexpr int64_t KP_CNT  = SQ_NB * static_cast<int64_t>(fe_end);

constexpr int64_t KPP_BYTES = KPP_CNT * static_cast<int64_t>(sizeof(int16_t));
constexpr int64_t KKP_BYTES = KKP_CNT * static_cast<int64_t>(sizeof(int32_t));
constexpr int64_t KK_BYTES  = KK_CNT  * static_cast<int64_t>(sizeof(int32_t));
constexpr int64_t KP_BYTES  = KP_CNT  * static_cast<int64_t>(sizeof(int32_t));
constexpr int64_t FV_BIN_BYTES = KPP_BYTES + KKP_BYTES + KK_BYTES + KP_BYTES; // 215,824,824

// Built-in material values, used only for MATERIAL_FALLBACK (no valid
// fv.bin present). The Bonanza table format has no separate material
// table of its own (material is implicit in KP/KKP/KPP), so a small
// hand-written fallback keeps the engine playable without any file.
constexpr std::array<int, PT_NB> FALLBACK_PIECE_VALUE{
    0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
};

// -----------------------------------------------------------------------
// Global state
// -----------------------------------------------------------------------
struct FvTables {
    std::vector<int16_t> kpp; // [SQ_NB][pos_n]                (triangular)
    std::vector<int32_t> kkp; // [SQ_NB][SQ_NB][fe_end]
    std::vector<int32_t> kk;  // [SQ_NB][SQ_NB]
    std::vector<int32_t> kp;  // [SQ_NB][fe_end]
};

bool        g_loaded_once   = false;
bool        g_explicit_path = false; // true if set via set_eval_file_path(non-empty)
std::string g_eval_file_path;        // empty = use auto-discovery
std::string g_status  = "material-only fallback (fv.bin missing)";
EvalFamily  g_family   = EvalFamily::MATERIAL_FALLBACK;
FvTables    g_tables;                // populated only when g_family == BONANZA_V6_FV

// -----------------------------------------------------------------------
// Bonanza square: file-major, file counted 9→1 (left→right from Black's
// side), rank counted a→i (top→bottom). Our own Square is rank-major with
// file 0 == USI file '9', so bonanza_sq == file_of(s)*9 + rank_of(s).
// -----------------------------------------------------------------------
inline int to_bonanza_sq(Square s) { return file_of(s) * 9 + rank_of(s); }
// 180-degree board rotation, used to view the board from White's side.
inline int mirror_sq(int bsq) { return static_cast<int>(SQ_NB) - 1 - bsq; }

// Bonanza square of `s`, as seen from `view`'s point of view (mirrored
// 180 degrees when `view == WHITE` so every KPP/KKP/KP lookup is always
// expressed "facing" the side whose king is being indexed).
inline int view_sq(Square s, Color view) {
    const int bsq = to_bonanza_sq(s);
    return (view == WHITE) ? mirror_sq(bsq) : bsq;
}

// Triangular (unordered pair) index into the KPP table's `pos_n` axis.
inline int64_t triangular_index(int a, int b) {
    const int hi = std::max(a, b);
    const int lo = std::min(a, b);
    return static_cast<int64_t>(hi) * (hi + 1) / 2 + lo;
}

inline int kk_lookup(int my_king, int opp_king) {
    return g_tables.kk[static_cast<int64_t>(my_king) * SQ_NB + opp_king];
}
inline int kp_lookup(int my_king, int fe) {
    return g_tables.kp[static_cast<int64_t>(my_king) * fe_end + fe];
}
inline int kkp_lookup(int my_king, int opp_king, int fe) {
    return g_tables.kkp[(static_cast<int64_t>(my_king) * SQ_NB + opp_king) * fe_end + fe];
}
inline int kpp_lookup(int my_king, int fe_a, int fe_b) {
    return g_tables.kpp[static_cast<int64_t>(my_king) * POS_N + triangular_index(fe_a, fe_b)];
}

// Base feature offset (own="f_", enemy="e_") for one unit of a hand piece.
int hand_feature_base(PieceType pt, bool own) {
    switch (pt) {
        case PAWN:   return own ? f_hand_pawn   : e_hand_pawn;
        case LANCE:  return own ? f_hand_lance  : e_hand_lance;
        case KNIGHT: return own ? f_hand_knight : e_hand_knight;
        case SILVER: return own ? f_hand_silver : e_hand_silver;
        case GOLD:   return own ? f_hand_gold   : e_hand_gold;
        case BISHOP: return own ? f_hand_bishop : e_hand_bishop;
        case ROOK:   return own ? f_hand_rook   : e_hand_rook;
        default:     return -1;
    }
}

// Base feature offset for a board piece occupying a square (own/enemy).
int board_feature_base(PieceType pt, bool own) {
    switch (pt) {
        case PAWN:        return own ? f_pawn   : e_pawn;
        case LANCE:       return own ? f_lance  : e_lance;
        case KNIGHT:      return own ? f_knight : e_knight;
        case SILVER:      return own ? f_silver : e_silver;
        case GOLD:        return own ? f_gold   : e_gold;
        case BISHOP:      return own ? f_bishop : e_bishop;
        case ROOK:        return own ? f_rook   : e_rook;
        case PROM_PAWN:   return own ? f_gold   : e_gold;   // promoted minor pieces
        case PROM_LANCE:  return own ? f_gold   : e_gold;   // move like GOLD in Bonanza's
        case PROM_KNIGHT: return own ? f_gold   : e_gold;   // KPP/KKP feature space.
        case PROM_SILVER: return own ? f_gold   : e_gold;
        case PROM_BISHOP: return own ? f_horse  : e_horse;
        case PROM_ROOK:   return own ? f_dragon : e_dragon;
        default:          return -1;
    }
}

// Build the list of non-king feature indices as seen from `view`'s own
// perspective (own pieces use f_*, opponent pieces use e_*; squares are
// mirrored when view == WHITE so that the board is always presented
// "facing" the viewing side, matching Bonanza's own convention).
std::vector<int> build_feature_list(const Board& board, Color view) {
    std::vector<int> list;
    list.reserve(40);

    for (int pt = PAWN; pt <= ROOK; ++pt) {
        if (hand_feature_base(static_cast<PieceType>(pt), true) < 0) continue;
        for (Color side : {BLACK, WHITE}) {
            const bool own = (side == view);
            const int base = hand_feature_base(static_cast<PieceType>(pt), own);
            const int count = board.hand(side, static_cast<PieceType>(pt));
            for (int k = 0; k < count; ++k) list.push_back(base + k);
        }
    }

    for (int s = 0; s < SQUARE_NB; ++s) {
        const Piece p = board.piece_at(s);
        if (p == NO_PIECE || type_of(p) == KING) continue;
        const bool own = (color_of(p) == view);
        const int base = board_feature_base(type_of(p), own);
        if (base < 0) continue;
        list.push_back(base + view_sq(s, view));
    }
    return list;
}

// -----------------------------------------------------------------------
// fv.bin loading
// -----------------------------------------------------------------------
bool looks_like_nnue_path(const std::string& path) {
    std::string lower = path;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("nnue") != std::string::npos;
}

bool try_load_from_path(const std::string& path, bool is_auto) {
    const std::string tag = is_auto ? "auto-discovered" : "explicit";

    if (looks_like_nnue_path(path)) {
        g_status = "unsupported evaluator: nnue (falling back to material-only; source: "
                 + path + " [" + tag + "])";
        g_family = EvalFamily::NNUE_UNSUPPORTED;
        return false; // stop auto-discovery too; the file was placed intentionally.
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        if (!is_auto) {
            g_status = "material-only fallback (file not found: " + path + ")";
            g_family = EvalFamily::MATERIAL_FALLBACK;
        }
        return false;
    }

    ifs.seekg(0, std::ios::end);
    const std::streamoff size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    if (size != static_cast<std::streamoff>(FV_BIN_BYTES)) {
        g_status = "material-only fallback (invalid Bonanza v6 fv.bin size: got "
                 + std::to_string(static_cast<long long>(size)) + " bytes, expected "
                 + std::to_string(static_cast<long long>(FV_BIN_BYTES)) + " bytes; source: "
                 + path + " [" + tag + "])";
        g_family = EvalFamily::MATERIAL_FALLBACK;
        return false;
    }

    FvTables loaded;
    loaded.kpp.resize(static_cast<size_t>(KPP_CNT));
    loaded.kkp.resize(static_cast<size_t>(KKP_CNT));
    loaded.kk.resize(static_cast<size_t>(KK_CNT));
    loaded.kp.resize(static_cast<size_t>(KP_CNT));

    ifs.read(reinterpret_cast<char*>(loaded.kpp.data()), KPP_BYTES);
    ifs.read(reinterpret_cast<char*>(loaded.kkp.data()), KKP_BYTES);
    ifs.read(reinterpret_cast<char*>(loaded.kk.data()),  KK_BYTES);
    ifs.read(reinterpret_cast<char*>(loaded.kp.data()),  KP_BYTES);

    if (!ifs) {
        g_status = "material-only fallback (failed to read fv.bin: " + path + ")";
        g_family = EvalFamily::MATERIAL_FALLBACK;
        return false;
    }

    g_tables = std::move(loaded);
    g_status = "bonanza-v6 fv.bin loaded from " + path + " [" + tag + "]";
    g_family = EvalFamily::BONANZA_V6_FV;
    return true;
}

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
        if (try_load_from_path(candidate, /*is_auto=*/true)) return;
        // Stop discovery early if we positively identified an unsupported
        // (NNUE-looking) file at this candidate path.
        if (g_family == EvalFamily::NNUE_UNSUPPORTED) return;
    }

    // Priority 4: built-in material fallback
    g_status = "material-only fallback"
               " (no compatible fv.bin found;"
               " auto-discovery checked: src/eval/fv.bin, eval/fv.bin)";
    g_family = EvalFamily::MATERIAL_FALLBACK;
}

} // namespace

// ============================================================
// Public API
// ============================================================

void set_eval_file_path(const std::string& path) {
    g_eval_file_path  = path;
    g_explicit_path   = !path.empty();
    g_loaded_once     = false;
    g_tables          = FvTables{};
    g_status          = "material-only fallback (fv.bin missing)";
    g_family          = EvalFamily::MATERIAL_FALLBACK;
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

    if (g_family != EvalFamily::BONANZA_V6_FV) {
        // Material-only fallback (no valid fv.bin, or NNUE detected).
        int score = 0;
        for (int s = 0; s < SQUARE_NB; ++s) {
            Piece p = board.piece_at(s);
            if (p == NO_PIECE) continue;
            int v = FALLBACK_PIECE_VALUE[type_of(p)];
            score += (color_of(p) == us) ? v : -v;
        }
        for (int pt = PAWN; pt <= ROOK; ++pt) {
            int v = FALLBACK_PIECE_VALUE[pt];
            score += board.hand(us,  static_cast<PieceType>(pt)) * v;
            score -= board.hand(~us, static_cast<PieceType>(pt)) * v;
        }
        return score;
    }

    // Bonanza-v6 three-piece-relation (KPP/KKP) evaluation, computed from
    // the perspective of the side to move (`us`). See file header comment
    // for the exact formula and its known simplifications.
    const int my_king  = view_sq(board.king_sq(us),  us);
    const int opp_king = view_sq(board.king_sq(~us), us);
    const std::vector<int> features = build_feature_list(board, us);

    int64_t score = kk_lookup(my_king, opp_king);
    for (size_t i = 0; i < features.size(); ++i) {
        score += kp_lookup(my_king, features[i]);
        score += kkp_lookup(my_king, opp_king, features[i]);
        for (size_t j = i + 1; j < features.size(); ++j) {
            score += kpp_lookup(my_king, features[i], features[j]);
        }
    }
    return static_cast<int>(score);
}
