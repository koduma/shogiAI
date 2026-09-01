#include "eval.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

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
// below, using synthetically-sized files, and (b) that a hand-crafted,
// correctly-sized fv.bin with known non-zero table entries at known
// offsets produces exactly the expected numeric evaluate() result (see
// tests/test_main.cpp). Users who place their genuine fv.bin should
// sanity-check `eval_status_message()` after startup; a size mismatch is
// reported explicitly rather than silently mis-parsed.
//
// The scoring formula implemented in evaluate() below is the classic
// Bonanza formula, NOT a re-derived approximation: it is taken directly
// from two independent, publicly available reference implementations
// that both agree on it exactly:
//   - kobanium/aobazero, src/usi-engine/bona/evaluate.cpp (a near-verbatim
//     port of the original Bonanza C source: functions evaluate()/
//     make_list()/doapc()/doacapt()).
//   - HiraokaTakuya/apery, src/evaluate.cpp (evaluateUnUseDiff()).
// Both sources independently confirm: FV_SCALE == 32; the score is always
// accumulated from BLACK's point of view and negated at the very end if
// White is to move; only ONE KKP term is added per non-king piece (using
// Black's own king/opponent-king squares, unmirrored, and Black's own
// feature index); the two-piece KPP term is summed TWICE — once from
// Black's own point of view (own king square, Black's feature list) and
// once from White's point of view (Black-mirrored White king square,
// White's feature list) — and the second sum is SUBTRACTED, not added;
// and a separate material term (see FALLBACK_PIECE_VALUE below) is added
// on top, scaled by FV_SCALE, exactly like the rest of the summation.
// Neither reference implementation's actual evaluate() ever reads the KK
// or KP tables at all (they exist in the raw fv.bin/converted-file layout
// purely for structural/converter compatibility with other consumers), so
// this implementation loads them (to validate file size/layout) but does
// not use them when scoring, matching both reference implementations.

namespace {

// -----------------------------------------------------------------------
// Path to the repository's src/eval/ directory, baked in at compile time
// (see CMakeLists.txt: -DSHOGIAI_SOURCE_DIR="${CMAKE_SOURCE_DIR}"). This
// gives auto-discovery an absolute, cwd-independent candidate that always
// resolves to the same fv.bin the user placed in the checked-out source
// tree, regardless of which directory the engine is launched from.
// -----------------------------------------------------------------------
#ifndef SHOGIAI_SOURCE_DIR
#define SHOGIAI_SOURCE_DIR ""
#endif

// Directory containing the running executable, or "" if it cannot be
// determined on this platform. Used to build cwd-independent discovery
// candidates relative to where the binary actually lives (e.g. the CMake
// build tree), which matters because most USI GUIs launch the engine with
// an arbitrary working directory, not the build or repository directory.
std::string get_executable_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
#if defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    // readlink() silently truncates (no null terminator, and returns the
    // full requested count) if the real path doesn't fit; treat that as
    // "undeterminable" rather than risk a corrupted/truncated path.
    if (len > 0 && len < static_cast<ssize_t>(sizeof(buf) - 1)) {
        buf[len] = '\0';
        fs::path p = fs::canonical(fs::path(buf), ec);
        if (ec) p = fs::path(buf);
        return p.parent_path().string();
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        fs::path p = fs::canonical(fs::path(buf), ec);
        if (ec) p = fs::path(buf);
        return p.parent_path().string();
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // GetModuleFileNameA() truncates silently on some Windows versions
    // when the path doesn't fit, without len reaching MAX_PATH; checking
    // GetLastError() for ERROR_INSUFFICIENT_BUFFER catches that case too.
    if (len > 0 && len < MAX_PATH && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        fs::path p(buf);
        return p.parent_path().string();
    }
#endif
    return "";
}

// -----------------------------------------------------------------------
// Automatic discovery candidates (tried in order when no explicit path is
// set via setoption/set_eval_file_path/SHOGIAI_EVAL_FILE). Built fresh on
// every call so it reflects the *current* working directory (tests change
// cwd at runtime) while still preferring cwd-independent locations first.
// -----------------------------------------------------------------------
std::vector<std::string> build_discovery_candidates() {
    std::vector<std::string> v;

    // Priority A: compile-time repository path (works regardless of cwd
    // or where the built binary was copied/launched from).
    if (SHOGIAI_SOURCE_DIR[0] != '\0') {
        v.push_back(std::string(SHOGIAI_SOURCE_DIR) + "/src/eval/fv.bin");
    }

    // Priority B: paths relative to the running executable's own
    // directory (covers "cmake --build" trees where fv.bin was copied
    // next to the binary, and users who copy the binary elsewhere but
    // keep fv.bin alongside it).
    const std::string exe_dir = get_executable_dir();
    if (!exe_dir.empty()) {
        v.push_back(exe_dir + "/eval/fv.bin");
        v.push_back(exe_dir + "/src/eval/fv.bin");
        v.push_back(exe_dir + "/../eval/fv.bin");
        v.push_back(exe_dir + "/../src/eval/fv.bin");
    }

    // Priority C: paths relative to the current working directory, kept
    // for backward compatibility with users who launch from the build
    // directory or the repository root.
    v.push_back("src/eval/fv.bin");
    v.push_back("eval/fv.bin");

    return v;
}

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

// Piece values used for the material term. Used as the *entire* score for
// MATERIAL_FALLBACK (no valid fv.bin present), and as an additive term
// summed alongside the KKP/KPP relation terms when a real fv.bin is loaded
// (BONANZA_V6_FV) — matching genuine Bonanza, which always adds a
// separately-computed material term rather than relying on it being
// implicit in the relation tables (see file header comment for
// provenance).
constexpr std::array<int, PT_NB> FALLBACK_PIECE_VALUE{
    0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
};

// Bonanza's classic fixed-point scale: raw KPP/KKP table units are 32x a
// centipawn, so the material term (in centipawns) must be multiplied by
// FV_SCALE before being summed with them, and the grand total divided by
// FV_SCALE at the very end. Confirmed via kobanium/aobazero
// src/usi-engine/bona/shogi.h (`#define FV_SCALE 32`).
constexpr int64_t FV_SCALE = 32;

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

// Note: the KK and KP tables are intentionally loaded (to validate the
// on-disk file's size/layout, since they are genuinely part of a real
// fv.bin) but never looked up here — neither reference implementation
// cited in the file header comment ever reads them when scoring a
// position, so no kk_lookup()/kp_lookup() helpers are defined.
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
    std::error_code ec;
    const std::string abs_path = std::filesystem::absolute(std::filesystem::path(path), ec).string();
    g_status = "bonanza-v6 fv.bin loaded from " + (ec ? path : abs_path) + " [" + tag
             + "] [family=BONANZA_V6_FV]";
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

    // Priority 3: auto-discovery. Candidates are recomputed on every call
    // (see build_discovery_candidates()) so they reflect the executable's
    // own location and the current working directory, in that priority
    // order, rather than only ever checking two fixed cwd-relative paths.
    const std::vector<std::string> candidates = build_discovery_candidates();
    for (const std::string& candidate : candidates) {
        if (try_load_from_path(candidate, /*is_auto=*/true)) return;
        // Stop discovery early if we positively identified an unsupported
        // (NNUE-looking) file at this candidate path.
        if (g_family == EvalFamily::NNUE_UNSUPPORTED) return;
    }

    // Priority 4: built-in material fallback
    std::string checked;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i) checked += ", ";
        checked += candidates[i];
    }
    g_status = "material-only fallback"
               " (no compatible fv.bin found; auto-discovery checked: " + checked + ")";
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

    // Material term: the entire score for MATERIAL_FALLBACK, and an
    // additive term (scaled by FV_SCALE) alongside the KKP/KPP relation
    // terms for BONANZA_V6_FV. Computed once, from Black's point of view
    // (positive == good for Black), matching the sign convention used by
    // the rest of the Bonanza-formula computation below.
    int material_black = 0;
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE) continue;
        int v = FALLBACK_PIECE_VALUE[type_of(p)];
        material_black += (color_of(p) == BLACK) ? v : -v;
    }
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        int v = FALLBACK_PIECE_VALUE[pt];
        material_black += board.hand(BLACK, static_cast<PieceType>(pt)) * v;
        material_black -= board.hand(WHITE, static_cast<PieceType>(pt)) * v;
    }

    if (g_family != EvalFamily::BONANZA_V6_FV) {
        // Material-only fallback (no valid fv.bin, or NNUE detected).
        return (us == BLACK) ? material_black : -material_black;
    }

    // Bonanza-v6 KPP/KKP evaluation. Always accumulated from Black's point
    // of view, then negated at the very end if White is to move — exactly
    // like genuine Bonanza (see file header comment for the exact formula
    // and its provenance from two independent reference implementations).
    const int sq_bk     = to_bonanza_sq(board.king_sq(BLACK));
    const int sq_wk     = to_bonanza_sq(board.king_sq(WHITE));
    const int sq_wk_inv = mirror_sq(sq_wk);

    // list0: every non-king piece, expressed from Black's own point of
    // view (own == Black uses f_*, squares unmirrored).
    // list1: the same pieces, expressed from White's own point of view
    // (own == White uses f_*, squares mirrored 180 degrees).
    const std::vector<int> list0 = build_feature_list(board, BLACK);
    const std::vector<int> list1 = build_feature_list(board, WHITE);

    int64_t score = static_cast<int64_t>(material_black) * FV_SCALE;

    // One KKP term per piece, using Black's own king/opponent-king squares
    // (unmirrored) and Black's own feature index — never the White-view
    // list, and never negated.
    for (size_t i = 0; i < list0.size(); ++i) {
        score += kkp_lookup(sq_bk, sq_wk, list0[i]);
    }

    // Two-piece KPP term, summed once from Black's own point of view and
    // once from White's (mirrored) point of view, with the second pass
    // SUBTRACTED rather than added.
    for (size_t i = 0; i < list0.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            score += kpp_lookup(sq_bk, list0[i], list0[j]);
        }
    }
    for (size_t i = 0; i < list1.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            score -= kpp_lookup(sq_wk_inv, list1[i], list1[j]);
        }
    }

    if (us == WHITE) score = -score;
    return static_cast<int>(score / FV_SCALE);
}
