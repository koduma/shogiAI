#include "eval.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

// -----------------------------------------------------------------------
// Deliberately no <filesystem>: this file must compile with plain C++11
// (including older MinGW toolchains without <filesystem>/experimental
// filesystem support). All path handling below is implemented with a
// handful of small helpers on top of POSIX (readlink/realpath/getcwd) or
// Win32 (GetModuleFileNameA/GetFullPathNameA) APIs instead.
// -----------------------------------------------------------------------
#if defined(_WIN32)
#include <windows.h>
#else
#include <climits>  // PATH_MAX
#include <unistd.h> // readlink, realpath, getcwd
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

// ============================================================
// Bonanza 6.0 "fv.bin" three-piece-relation (KPP/KKP) evaluator
// ============================================================
// This file implements the classic Bonanza-style evaluation table format:
// King+Piece+Piece (KPP) and King+King+Piece (KKP) relation tables,
// loaded verbatim from an unmodified Bonanza 6.0 fv.bin.
//
// IMPORTANT (root-cause of a real user-reported bug): an earlier version
// of this file assumed a 215,824,824-byte layout (KPP int16_t + KKP/KK/KP
// as int32_t, KKP indexed by the full `fe_end`), based on a *converter*
// tool's in-memory representation (HiraokaTakuya/apery,
// utils/bonanzatoapery/main.cpp). That converter's *output* struct sizes
// are not the same thing as genuine Bonanza's own on-disk fv.bin format,
// and real user fv.bin files (186,268,248 bytes) were rejected as
// "invalid size", silently degrading every game to material-only scoring
// (evaluate() always returning the same material-only value, i.e. "always
// 0" for symmetric positions).
//
// The genuine, verbatim Bonanza 6.0 engine source (mirrored e.g. at
// puriketu99/bonanza and endlfu/bonanza, src/client/ini.c `load_fv()` and
// src/client/shogi.h) reads fv.bin as *exactly two* tables, both
// `short` (int16_t), in this order:
//   1. KPP : int16_t[SQUARE_NB][pos_n]                        (176,584,212 bytes)
//            pos_n = fe_end*(fe_end+1)/2, fe_end == 1476 (triangular,
//            unordered-pair index over the full feature space).
//   2. KKP : int16_t[SQUARE_NB][SQUARE_NB][kkp_end]             (9,684,036 bytes)
//            kkp_end == 738 (== fe_end/2): KKP does NOT use the full
//            fe_end feature space. Because a KKP entry is always looked
//            up with a specific (my-king, opp-king) pair that already
//            encodes "whose piece this is" (own pieces use the
//            unmirrored (SQ_BKING, SQ_WKING) pair; opponent pieces use
//            the mirrored (Inv(SQ_WKING), Inv(SQ_BKING)) pair, with the
//            resulting term SUBTRACTED), there is no need for separate
//            own/enemy feature ids the way KPP has them — halving the
//            feature space to `kkp_end`.
//   Total: 186,268,248 bytes. There is NO separate on-disk KK or KP
//   table — genuine Bonanza's `load_fv()` never reads them (`ini.c`
//   contains exactly the two `fread()` calls above and nothing else).
//
// This is independently confirmed against the *actual byte size* the
// user's real fv.bin reported: 186,268,248 == 81*1,090,026*2 (KPP) +
// 81*81*738*2 (KKP), an exact match with no slack.
//
// A file whose size happens to match the previous, incorrect
// 215,824,824-byte assumption is still recognized specially and reported
// with its own explicit message (rather than a generic size mismatch),
// per the requirement that unsupported/ambiguous formats must be
// reported clearly instead of guessed at.
//
// The scoring formula implemented in evaluate() below is the classic
// Bonanza formula, transcribed directly from genuine Bonanza source
// (src/client/evaluate.c: `evaluate()` / `make_list()`), not a
// re-derived approximation:
//   - FV_SCALE == 32 (src/client/shogi.h: `#define FV_SCALE 32`).
//   - The score is always accumulated from BLACK's point of view and
//     negated at the very end if White is to move.
//   - KKP: exactly ONE term per non-king piece (hand or board), added
//     for Black's own pieces (looked up at kkp[SQ_BKING][SQ_WKING][idx],
//     idx = kkp_<piece>_offset + own-hand-count or absolute board
//     square) and SUBTRACTED for White's own pieces (looked up at
//     kkp[Inv(SQ_WKING)][Inv(SQ_BKING)][idx], idx using the *mirrored*
//     board square for board pieces). Promoted minor pieces (と/成香/
//     成桂/成銀) share GOLD's slot; promoted bishop/rook use HORSE/DRAGON.
//   - KPP: the two-piece term is summed over EVERY unordered pair
//     (i, j) with i >= j *including the diagonal i == j* (see
//     `evaluate()`'s `for (j = 0; j <= i; j++)`, not `j < i`) — once from
//     Black's own point of view (own king square, Black's feature list)
//     and once from White's point of view (Black-mirrored White king
//     square, White's feature list), with the second sum SUBTRACTED.
//   - A separate material term (see FALLBACK_PIECE_VALUE below) is added
//     on top, scaled by FV_SCALE, exactly like the rest of the summation
//     (`score += MATERIAL * FV_SCALE;` in genuine Bonanza).

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

// Portable (filesystem-free) path helpers
// -----------------------------------------------------------------------
// Minimal replacements for the small subset of std::filesystem::path
// functionality this file needs (parent_path()/canonical()/absolute()),
// implemented with plain C++11 + POSIX/Win32 APIs so this translation
// unit compiles without <filesystem> (unavailable on some older MinGW
// toolchains and pre-C++17 standard libraries).
// -----------------------------------------------------------------------
#if !defined(_WIN32) && !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

// Directory part of `path` (everything before the last '/' or '\\'), or
// "" if there is no separator. Mirrors std::filesystem::path::parent_path()
// closely enough for building discovery candidates below.
std::string path_parent_dir(const std::string& path) {
    const std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return path.substr(0, 1); // root, e.g. "/"
    return path.substr(0, pos);
}

#if !defined(_WIN32)
// Resolves symlinks / "." / ".." components, matching what
// std::filesystem::canonical() did for the POSIX branches below. Requires
// the path to exist (both call sites here only use it on paths that are
// already known to exist). Falls back to the input path unchanged if
// resolution fails.
std::string canonicalize_existing_path(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) return std::string(resolved);
    return path;
}
#endif

// Directory containing the running executable, or "" if it cannot be
// determined on this platform. Used to build cwd-independent discovery
// candidates relative to where the binary actually lives (e.g. the CMake
// build tree), which matters because most USI GUIs launch the engine with
// an arbitrary working directory, not the build or repository directory.
std::string get_executable_dir() {
#if defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    // readlink() silently truncates (no null terminator, and returns the
    // full requested count) if the real path doesn't fit; treat that as
    // "undeterminable" rather than risk a corrupted/truncated path.
    if (len > 0 && len < static_cast<ssize_t>(sizeof(buf) - 1)) {
        buf[len] = '\0';
        return path_parent_dir(canonicalize_existing_path(buf));
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        return path_parent_dir(canonicalize_existing_path(buf));
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    SetLastError(ERROR_SUCCESS);
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    // GetModuleFileNameA() truncates silently on some Windows versions
    // when the path doesn't fit, without len reaching MAX_PATH; checking
    // GetLastError() for ERROR_INSUFFICIENT_BUFFER catches that case too.
    if (len > 0 && len < MAX_PATH && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return path_parent_dir(std::string(buf));
    }
#endif
    return "";
}

// Best-effort absolute form of `path` (used only for cosmetic status
// messages, so falling back to the original relative path on failure is
// acceptable). `path` is expected to already exist at the call site.
std::string make_absolute_path(const std::string& path) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD len = GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr);
    if (len > 0 && len < MAX_PATH) return std::string(buf);
    return path;
#else
    return canonicalize_existing_path(path);
#endif
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
// These are the KPP (full fe_end) feature offsets: verbatim from genuine
// Bonanza 6.0 (src/client/shogi.h).
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

// KKP-specific (halved, kkp_end == fe_end/2) feature offsets: also
// verbatim from genuine Bonanza 6.0 (src/client/shogi.h). KKP does not
// distinguish "own" (f_) vs "enemy" (e_) pieces with separate offsets the
// way KPP does — that distinction is carried instead by which king-pair
// (own unmirrored vs opponent mirrored) is used to index the table, and
// by whether the resulting term is added or subtracted (see evaluate()).
enum {
    kkp_hand_pawn = 0,  kkp_hand_lance = 19, kkp_hand_knight = 24,
    kkp_hand_silver = 29, kkp_hand_gold = 34, kkp_hand_bishop = 39,
    kkp_hand_rook = 42, kkp_hand_end = 45,

    kkp_pawn = 36, kkp_lance = 108, kkp_knight = 171, kkp_silver = 252,
    kkp_gold = 333, kkp_bishop = 414, kkp_horse = 495, kkp_rook = 576,
    kkp_dragon = 657, kkp_end = 738
};

constexpr int64_t SQ_NB   = 81;
constexpr int64_t POS_N   = static_cast<int64_t>(fe_end) * (fe_end + 1) / 2;
constexpr int64_t KPP_CNT = SQ_NB * POS_N;
constexpr int64_t KKP_CNT = SQ_NB * SQ_NB * static_cast<int64_t>(kkp_end);

constexpr int64_t KPP_BYTES = KPP_CNT * static_cast<int64_t>(sizeof(int16_t));
constexpr int64_t KKP_BYTES = KKP_CNT * static_cast<int64_t>(sizeof(int16_t));
constexpr int64_t FV_BIN_BYTES = KPP_BYTES + KKP_BYTES; // 186,268,248

// Size of the previous (incorrect) 215,824,824-byte assumption, kept only
// so a file of that size can be reported with a specific, actionable
// message instead of a generic "invalid size" one (see try_load_from_path).
constexpr int64_t LEGACY_APERY_LAYOUT_BYTES = 215824824;

// Piece values used for the material term. Used as the *entire* score for
// MATERIAL_FALLBACK (no valid fv.bin present), and as an additive term
// summed alongside the KKP/KPP relation terms when a real fv.bin is loaded
// (BONANZA_V6_FV) — matching genuine Bonanza, which always adds a
// separately-computed material term rather than relying on it being
// implicit in the relation tables (see file header comment for
// provenance).
constexpr std::array<int, PT_NB> FALLBACK_PIECE_VALUE{
0, 87, 232, 257, 369, 444, 569, 642, 0, 534, 489, 510, 495, 827, 945    
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
    std::vector<int16_t> kpp; // [SQ_NB][pos_n]     (triangular, full fe_end space)
    std::vector<int16_t> kkp; // [SQ_NB][SQ_NB][kkp_end]
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

// KKP table lookup: `idx` must already be a kkp-space index (either
// kkp_hand_<piece> + hand-count, or kkp_<piece> + a board square — see
// hand_kkp_base()/board_kkp_base() below), NOT a full fe_end feature id.
inline int kkp_lookup(int my_king, int opp_king, int idx) {
    return g_tables.kkp[(static_cast<int64_t>(my_king) * SQ_NB + opp_king) * kkp_end + idx];
}
inline int kpp_lookup(int my_king, int fe_a, int fe_b) {
    return g_tables.kpp[static_cast<int64_t>(my_king) * POS_N + triangular_index(fe_a, fe_b)];
}

// kkp-space base offset for one unit of a hand piece (own/enemy is NOT
// distinguished here — see file header comment).
int hand_kkp_base(PieceType pt) {
    switch (pt) {
        case PAWN:   return kkp_hand_pawn;
        case LANCE:  return kkp_hand_lance;
        case KNIGHT: return kkp_hand_knight;
        case SILVER: return kkp_hand_silver;
        case GOLD:   return kkp_hand_gold;
        case BISHOP: return kkp_hand_bishop;
        case ROOK:   return kkp_hand_rook;
        default:     return -1;
    }
}

// kkp-space base offset for a board piece (own/enemy is NOT distinguished
// here — see file header comment). Promoted minor pieces share GOLD's
// slot (they move like GOLD in Bonanza's feature space); promoted
// bishop/rook use HORSE/DRAGON, matching board_feature_base() below.
int board_kkp_base(PieceType pt) {
    switch (pt) {
        case PAWN:        return kkp_pawn;
        case LANCE:       return kkp_lance;
        case KNIGHT:      return kkp_knight;
        case SILVER:      return kkp_silver;
        case GOLD:        return kkp_gold;
        case PROM_PAWN:   return kkp_gold;
        case PROM_LANCE:  return kkp_gold;
        case PROM_KNIGHT: return kkp_gold;
        case PROM_SILVER: return kkp_gold;
        case BISHOP:      return kkp_bishop;
        case PROM_BISHOP: return kkp_horse;
        case ROOK:        return kkp_rook;
        case PROM_ROOK:   return kkp_dragon;
        default:          return -1;
    }
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
        if (size == static_cast<std::streamoff>(LEGACY_APERY_LAYOUT_BYTES)) {
            // Explicitly recognized: this is the size a previous version of
            // this loader incorrectly assumed for "Bonanza v6" (based on a
            // converter tool's expanded in-memory struct sizes, not genuine
            // Bonanza's own on-disk format). Report this distinctly instead
            // of a generic size mismatch, since it is a known-but-unsupported
            // layout rather than an arbitrary wrong-sized file.
            g_status = "material-only fallback (unsupported fv.bin layout: got "
                     + std::to_string(static_cast<long long>(size))
                     + " bytes, which matches the Apery bonanzatoapery converter's "
                       "expanded KKP/KK/KP layout, not genuine Bonanza v6's own "
                       "186,268,248-byte KPP+KKP layout; source: "
                     + path + " [" + tag + "])";
            g_family = EvalFamily::MATERIAL_FALLBACK;
            return false;
        }
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

    ifs.read(reinterpret_cast<char*>(loaded.kpp.data()), KPP_BYTES);
    ifs.read(reinterpret_cast<char*>(loaded.kkp.data()), KKP_BYTES);

    if (!ifs) {
        g_status = "material-only fallback (failed to read fv.bin: " + path + ")";
        g_family = EvalFamily::MATERIAL_FALLBACK;
        return false;
    }

    g_tables = std::move(loaded);
    const std::string abs_path = make_absolute_path(path);
    g_status = "bonanza-v6 fv.bin loaded from " + (abs_path.empty() ? path : abs_path) + " [" + tag
             + "] [family=BONANZA_V6_FV] [size=" + std::to_string(static_cast<long long>(size))
             + " bytes] [fv_scale=" + std::to_string(static_cast<long long>(FV_SCALE)) + "]";
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
    // and its provenance).
    const int sq_bk     = to_bonanza_sq(board.king_sq(BLACK));
    const int sq_wk     = to_bonanza_sq(board.king_sq(WHITE));
    const int sq_bk_inv = mirror_sq(sq_bk);
    const int sq_wk_inv = mirror_sq(sq_wk);

    // list0: every non-king piece, expressed from Black's own point of
    // view (own == Black uses f_*, squares unmirrored).
    // list1: the same pieces, expressed from White's own point of view
    // (own == White uses f_*, squares mirrored 180 degrees).
    const std::vector<int> list0 = build_feature_list(board, BLACK);
    const std::vector<int> list1 = build_feature_list(board, WHITE);

    int64_t score = static_cast<int64_t>(material_black) * FV_SCALE;

    // KKP term: exactly one lookup per non-king piece (hand or board),
    // ADDED for Black's own pieces (looked up at the unmirrored
    // (SQ_BKING, SQ_WKING) king pair) and SUBTRACTED for White's own
    // pieces (looked up at the mirrored (Inv(SQ_WKING), Inv(SQ_BKING))
    // king pair, with the board square itself also mirrored) — exactly
    // matching genuine Bonanza's make_list() (see file header comment).
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        const int hb = hand_kkp_base(static_cast<PieceType>(pt));
        if (hb < 0) continue;
        score += kkp_lookup(sq_bk, sq_wk, hb + board.hand(BLACK, static_cast<PieceType>(pt)));
        score -= kkp_lookup(sq_wk_inv, sq_bk_inv, hb + board.hand(WHITE, static_cast<PieceType>(pt)));
    }
    for (int s = 0; s < SQUARE_NB; ++s) {
        const Piece p = board.piece_at(s);
        if (p == NO_PIECE || type_of(p) == KING) continue;
        const int bb = board_kkp_base(type_of(p));
        if (bb < 0) continue;
        const int bsq = to_bonanza_sq(static_cast<Square>(s));
        if (color_of(p) == BLACK) {
            score += kkp_lookup(sq_bk, sq_wk, bb + bsq);
        } else {
            score -= kkp_lookup(sq_wk_inv, sq_bk_inv, bb + mirror_sq(bsq));
        }
    }

    // Two-piece KPP term, summed once from Black's own point of view and
    // once from White's (mirrored) point of view, with the second pass
    // SUBTRACTED rather than added. Every unordered pair (i, j) with
    // i >= j is included, INCLUDING the diagonal i == j (a single-piece
    // "self" term folded into the triangular table) — genuine Bonanza's
    // loop is `for (j = 0; j <= i; j++)`, not `j < i`.
    for (size_t i = 0; i < list0.size(); ++i) {
        for (size_t j = 0; j <= i; ++j) {
            score += kpp_lookup(sq_bk, list0[i], list0[j]);
        }
    }
    for (size_t i = 0; i < list1.size(); ++i) {
        for (size_t j = 0; j <= i; ++j) {
            score -= kpp_lookup(sq_wk_inv, list1[i], list1[j]);
        }
    }

    if (us == WHITE) score = -score;
    return static_cast<int>(score / FV_SCALE);
}
