#include "eval.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

// ============================================================
// Bonanza 6.0 "fv.bin" three-piece-relation (KPP/KKP) evaluator
//   with Incremental KPP/KKP Differential Updates + Eval Hash Cache
// ============================================================
// Layout of genuine Bonanza 6.0 fv.bin (src/client/ini.c `load_fv()` & shogi.h):
//   1. KPP : int16_t[81][pos_n]     (176,584,212 bytes), pos_n = 1476*1477/2 = 1,090,026
//   2. KKP : int16_t[81][81][738]   (  9,684,036 bytes)
//   Total  : 186,268,248 bytes (~177.6 MiB / ~186.3 MB)
//
// Critical coordinate conventions matching genuine Bonanza 6.0:
//   - Board square indexing in Bonanza (`enum { A9=0, A8=1, ..., A1=8, B9=9, ..., I1=80 }`)
//     is RANK-MAJOR (`rank * 9 + file`, file 9->1 = 0..8, rank a->i = 0..8), which is
//     IDENTICAL to `Square` in `types.hpp` (`to_bonanza_sq(s) == s`).
//   - Hand piece features in KPP (`FOO` macro in Bonanza `evaluate.c` `make_list()`) are
//     1-BASED (`f_hand_pawn + 1 .. f_hand_pawn + count`), whereas KKP hand lookups index
//     directly by count (`kkp_hand_pawn + count`, `0 .. count`).

namespace {

#ifndef SHOGIAI_SOURCE_DIR
#define SHOGIAI_SOURCE_DIR ""
#endif

#if !defined(_WIN32) && !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

std::string path_parent_dir(const std::string& path) {
    const std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

#if !defined(_WIN32)
std::string canonicalize_existing_path(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) return std::string(resolved);
    return path;
}
#endif

std::string get_executable_dir() {
#if defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
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
    if (len > 0 && len < MAX_PATH && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return path_parent_dir(std::string(buf));
    }
#endif
    return "";
}

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

std::vector<std::string> build_discovery_candidates() {
    std::vector<std::string> v;
    if (SHOGIAI_SOURCE_DIR[0] != '\0') {
        v.push_back(std::string(SHOGIAI_SOURCE_DIR) + "/src/eval/fv.bin");
    }
    const std::string exe_dir = get_executable_dir();
    if (!exe_dir.empty()) {
        v.push_back(exe_dir + "/eval/fv.bin");
        v.push_back(exe_dir + "/src/eval/fv.bin");
        v.push_back(exe_dir + "/../eval/fv.bin");
        v.push_back(exe_dir + "/../src/eval/fv.bin");
    }
    v.push_back("src/eval/fv.bin");
    v.push_back("eval/fv.bin");
    return v;
}

// -----------------------------------------------------------------------
// Bonanza 6.0 KPP & KKP feature offsets (verbatim from shogi.h)
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

constexpr int64_t KPP_BYTES    = KPP_CNT * static_cast<int64_t>(sizeof(int16_t));
constexpr int64_t KKP_BYTES    = KKP_CNT * static_cast<int64_t>(sizeof(int16_t));
constexpr int64_t FV_BIN_BYTES = KPP_BYTES + KKP_BYTES; // 186,268,248
constexpr int64_t LEGACY_APERY_LAYOUT_BYTES = 215824824;

// Precomputed triangular row base `x * (x + 1) / 2` for 0 <= x < fe_end
const std::array<int64_t, fe_end> TRI_BASE = [] {
    std::array<int64_t, fe_end> a{};
    for (int i = 0; i < fe_end; ++i)
        a[i] = static_cast<int64_t>(i) * (i + 1) / 2;
    return a;
}();

struct FvTables {
    std::vector<int16_t> kpp; // [SQ_NB][pos_n]
    std::vector<int16_t> kkp; // [SQ_NB][SQ_NB][kkp_end]
};

// Evaluation Hash Table (4 MiB: caches final Black-POV centipawn score by Zobrist key)
constexpr size_t EHASH_SIZE = 1u << 18;
constexpr size_t EHASH_MASK = EHASH_SIZE - 1;
struct EvalHashEntry {
    uint64_t key = 0;
    int32_t  score_black = 0;
    uint32_t epoch = 0;
};

// Incremental Parent-State Cache (8,192 entries: caches KPP/KKP accumulators + feature lists)
constexpr size_t STATE_CACHE_SIZE = 1u << 13;
constexpr size_t STATE_CACHE_MASK = STATE_CACHE_SIZE - 1;
struct EvalStateEntry {
    uint64_t key = 0;
    uint32_t epoch = 0;
    int16_t  sq_bk = -1;
    int16_t  sq_wk = -1;
    uint8_t  nlist = 0;
    int64_t  sum_bkpp = 0;
    int64_t  sum_wkpp = 0;
    int64_t  sum_kkp  = 0;
    int16_t  list0[44]{};
    int16_t  list1[44]{};
};

bool        g_loaded_once   = false;
bool        g_explicit_path = false;
int64_t     g_fv_scale      = 32;
uint32_t    g_eval_epoch    = 1;
std::string g_eval_file_path;
std::string g_status        = "material-only fallback (fv.bin missing)";
EvalFamily  g_family        = EvalFamily::MATERIAL_FALLBACK;
FvTables    g_tables;

std::vector<EvalHashEntry>  g_ehash(EHASH_SIZE);
std::vector<EvalStateEntry> g_state_cache(STATE_CACHE_SIZE);

inline void bump_eval_epoch() {
    if (++g_eval_epoch == 0) {
        std::fill(g_ehash.begin(), g_ehash.end(), EvalHashEntry{});
        std::fill(g_state_cache.begin(), g_state_cache.end(), EvalStateEntry{});
        g_eval_epoch = 1;
    }
}

// -----------------------------------------------------------------------
// Bonanza square & feature helpers
// -----------------------------------------------------------------------
// Bonanza's A9..I1 square numbering is rank-major (`rank * 9 + file`),
// identical to `Square` in `types.hpp`!
inline int to_bonanza_sq(Square s) { return static_cast<int>(s); }
inline int mirror_sq(int bsq)      { return static_cast<int>(SQ_NB) - 1 - bsq; }
inline int view_sq(Square s, Color view) {
    const int bsq = to_bonanza_sq(s);
    return (view == WHITE) ? mirror_sq(bsq) : bsq;
}

inline int64_t triangular_index(int a, int b) {
    return (a >= b) ? (TRI_BASE[a] + b) : (TRI_BASE[b] + a);
}

inline int kkp_lookup(int my_king, int opp_king, int idx) {
    return g_tables.kkp[(static_cast<int64_t>(my_king) * SQ_NB + opp_king) * kkp_end + idx];
}

inline int max_hand_count(PieceType pt) {
    switch (pt) {
        case PAWN:   return 18;
        case LANCE:
        case KNIGHT:
        case SILVER:
        case GOLD:   return 4;
        case BISHOP:
        case ROOK:   return 2;
        default:     return 0;
    }
}

inline int hand_kkp_base(PieceType pt) {
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

inline int board_kkp_base(PieceType pt) {
    switch (pt) {
        case PAWN:        return kkp_pawn;
        case LANCE:       return kkp_lance;
        case KNIGHT:      return kkp_knight;
        case SILVER:      return kkp_silver;
        case GOLD:
        case PROM_PAWN:
        case PROM_LANCE:
        case PROM_KNIGHT:
        case PROM_SILVER: return kkp_gold;
        case BISHOP:      return kkp_bishop;
        case PROM_BISHOP: return kkp_horse;
        case ROOK:        return kkp_rook;
        case PROM_ROOK:   return kkp_dragon;
        default:          return -1;
    }
}

inline int hand_feature_base(PieceType pt, bool own) {
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

inline int board_feature_base(PieceType pt, bool own) {
    switch (pt) {
        case PAWN:        return own ? f_pawn   : e_pawn;
        case LANCE:       return own ? f_lance  : e_lance;
        case KNIGHT:      return own ? f_knight : e_knight;
        case SILVER:      return own ? f_silver : e_silver;
        case GOLD:
        case PROM_PAWN:
        case PROM_LANCE:
        case PROM_KNIGHT:
        case PROM_SILVER: return own ? f_gold   : e_gold;
        case BISHOP:      return own ? f_bishop : e_bishop;
        case PROM_BISHOP: return own ? f_horse  : e_horse;
        case ROOK:        return own ? f_rook   : e_rook;
        case PROM_ROOK:   return own ? f_dragon : e_dragon;
        default:          return -1;
    }
}

// Bounds-checked board KPP feature index (`-1` if piece sits on an illegal dead rank in synthetic SFEN)
inline int board_feature_checked(Color pc, PieceType pt, Square s, Color view) {
    const bool own = (pc == view);
    const int base = board_feature_base(pt, own);
    if (base < 0) return -1;
    const int vsq = view_sq(s, view);
    if (pt == PAWN || pt == LANCE) {
        if (own ? (vsq < 9) : (vsq > 71)) return -1;
    } else if (pt == KNIGHT) {
        if (own ? (vsq < 18) : (vsq > 62)) return -1;
    }
    const int idx = base + vsq;
    return (idx >= fe_hand_end && idx < fe_end) ? idx : -1;
}

// Bounds-checked board KKP index (`-1` if illegal dead rank in synthetic SFEN)
inline int board_kkp_checked(Color pc, PieceType pt, Square s) {
    const int bb = board_kkp_base(pt);
    if (bb < 0) return -1;
    const int own_sq = (pc == BLACK) ? to_bonanza_sq(s) : mirror_sq(to_bonanza_sq(s));
    if (pt == PAWN || pt == LANCE) {
        if (own_sq < 9) return -1;
    } else if (pt == KNIGHT) {
        if (own_sq < 18) return -1;
    }
    const int idx = bb + own_sq;
    return (idx >= kkp_hand_end && idx < kkp_end) ? idx : -1;
}

// Compute full KPP sum for a feature list using sorted sequential row pointers
int64_t compute_kpp_sum_sorted(int king_sq, const int16_t* raw_list, int n) {
    int16_t sorted[44];
    for (int i = 0; i < n; ++i) sorted[i] = raw_list[i];
    // Tiny insertion sort (`n <= 38`) makes inner row reads strictly sequential (`sorted[j] <= sorted[i]`)
    for (int i = 1; i < n; ++i) {
        int16_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }

    const int16_t* kpp_king = &g_tables.kpp[static_cast<int64_t>(king_sq) * POS_N];
    int64_t sum = 0;
    for (int i = 0; i < n; ++i) {
        const int16_t* row = kpp_king + TRI_BASE[sorted[i]];
        int64_t row_sum = 0;
        for (int j = 0; j <= i; ++j) {
            row_sum += row[sorted[j]];
        }
        sum += row_sum;
    }
    return sum;
}

int64_t compute_kkp_sum_full(const Board& board, int sq_bk, int sq_wk, int sq_bk_inv, int sq_wk_inv) {
    int64_t sum_kkp = 0;
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        const PieceType ptype = static_cast<PieceType>(pt);
        const int hb = hand_kkp_base(ptype);
        if (hb < 0) continue;
        const int max_c = max_hand_count(ptype);
        const int cb = std::clamp(board.hand(BLACK, ptype), 0, max_c);
        const int cw = std::clamp(board.hand(WHITE, ptype), 0, max_c);
        sum_kkp += kkp_lookup(sq_bk, sq_wk, hb + cb);
        sum_kkp -= kkp_lookup(sq_wk_inv, sq_bk_inv, hb + cw);
    }
    for (int s = 0; s < SQUARE_NB; ++s) {
        const Piece p = board.piece_at(s);
        if (p == NO_PIECE || type_of(p) == KING) continue;
        const int idx = board_kkp_checked(color_of(p), type_of(p), s);
        if (idx < 0) continue;
        if (color_of(p) == BLACK) {
            sum_kkp += kkp_lookup(sq_bk, sq_wk, idx);
        } else {
            sum_kkp -= kkp_lookup(sq_wk_inv, sq_bk_inv, idx);
        }
    }
    return sum_kkp;
}

// Single-feature differential update on `list[0..n-1]` (exact including diagonal `i == j`)
inline bool replace_kpp_feature(int king_sq, int16_t* list, int n, int old_f, int new_f, int64_t& sum) {
    if (old_f < 0 || new_f < 0) return false;
    int pos = -1;
    for (int i = 0; i < n; ++i) {
        if (list[i] == old_f) { pos = i; break; }
    }
    if (pos < 0) return false;

    const int16_t* kpp_king = &g_tables.kpp[static_cast<int64_t>(king_sq) * POS_N];
    const int16_t* old_row  = kpp_king + TRI_BASE[old_f];
    int64_t delta = 0;
    for (int k = 0; k < n; ++k) {
        const int fk = list[k];
        delta -= (old_f >= fk) ? old_row[fk] : kpp_king[TRI_BASE[fk] + old_f];
    }
    list[pos] = static_cast<int16_t>(new_f);
    const int16_t* new_row = kpp_king + TRI_BASE[new_f];
    for (int k = 0; k < n; ++k) {
        const int fk = list[k];
        delta += (new_f >= fk) ? new_row[fk] : kpp_king[TRI_BASE[fk] + new_f];
    }
    sum += delta;
    return true;
}

void build_full_eval_state(const Board& board, EvalStateEntry& st) {
    const int sq_bk     = to_bonanza_sq(board.king_sq(BLACK));
    const int sq_wk     = to_bonanza_sq(board.king_sq(WHITE));
    const int sq_bk_inv = mirror_sq(sq_bk);
    const int sq_wk_inv = mirror_sq(sq_wk);

    st.key   = board.hash();
    st.epoch = g_eval_epoch;
    st.sq_bk = static_cast<int16_t>(sq_bk);
    st.sq_wk = static_cast<int16_t>(sq_wk);

    int n = 0;
    // Hand features: 1-based (`base + 1 .. base + count`), matching Bonanza `FOO` macro!
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        const PieceType ptype = static_cast<PieceType>(pt);
        const int max_c = max_hand_count(ptype);
        for (Color side : {BLACK, WHITE}) {
            const int cnt = std::clamp(board.hand(side, ptype), 0, max_c);
            if (cnt <= 0) continue;
            const int b0 = hand_feature_base(ptype, side == BLACK);
            const int b1 = hand_feature_base(ptype, side == WHITE);
            for (int k = 1; k <= cnt && n < 42; ++k) {
                st.list0[n] = static_cast<int16_t>(b0 + k);
                st.list1[n] = static_cast<int16_t>(b1 + k);
                ++n;
            }
        }
    }
    for (int s = 0; s < SQUARE_NB && n < 42; ++s) {
        const Piece p = board.piece_at(s);
        if (p == NO_PIECE || type_of(p) == KING) continue;
        const int f0 = board_feature_checked(color_of(p), type_of(p), s, BLACK);
        const int f1 = board_feature_checked(color_of(p), type_of(p), s, WHITE);
        if (f0 < 0 || f1 < 0) continue;
        st.list0[n] = static_cast<int16_t>(f0);
        st.list1[n] = static_cast<int16_t>(f1);
        ++n;
    }

    st.nlist    = static_cast<uint8_t>(n);
    st.sum_bkpp = compute_kpp_sum_sorted(sq_bk, st.list0, n);
    st.sum_wkpp = compute_kpp_sum_sorted(sq_wk_inv, st.list1, n);
    st.sum_kkp  = compute_kkp_sum_full(board, sq_bk, sq_wk, sq_bk_inv, sq_wk_inv);
}

// Attempt incremental update from parent state in `g_state_cache`
bool try_incremental_eval_state(const Board& board, EvalStateEntry& cur) {
    if (board.history_.empty()) return false;
    const StateInfo& si = board.history_.back();
    const EvalStateEntry& parent = g_state_cache[si.hash & STATE_CACHE_MASK];
    if (parent.epoch != g_eval_epoch || parent.key != si.hash)
        return false;

    // 1. Null move: kings, hands, and board pieces are completely unchanged!
    if (si.null_move) {
        cur = parent;
        cur.key = board.hash();
        return true;
    }

    const Move  m     = si.move;
    const Color mover = si.mover;
    if (m == MOVE_NONE) return false;

    const int sq_bk     = to_bonanza_sq(board.king_sq(BLACK));
    const int sq_wk     = to_bonanza_sq(board.king_sq(WHITE));
    const int sq_bk_inv = mirror_sq(sq_bk);
    const int sq_wk_inv = mirror_sq(sq_wk);

    // 2. King move: if non-capturing, reuse the other king's KPP sum (Yaneurao's single-king diff!)
    if (!is_drop(m) && type_of(si.moved_piece) == KING) {
        if (si.captured != NO_PIECE) return false; // King capture: fall back to full state build
        cur = parent;
        cur.key   = board.hash();
        cur.sq_bk = static_cast<int16_t>(sq_bk);
        cur.sq_wk = static_cast<int16_t>(sq_wk);
        if (mover == BLACK) {
            cur.sum_bkpp = compute_kpp_sum_sorted(sq_bk, cur.list0, cur.nlist);
        } else {
            cur.sum_wkpp = compute_kpp_sum_sorted(sq_wk_inv, cur.list1, cur.nlist);
        }
        cur.sum_kkp = compute_kkp_sum_full(board, sq_bk, sq_wk, sq_bk_inv, sq_wk_inv);
        return true;
    }

    if (si.captured != NO_PIECE && type_of(si.captured) == KING)
        return false;

    // 3. Non-king move (drop, quiet, promote, or capture): neither king moved!
    if (parent.sq_bk != sq_bk || parent.sq_wk != sq_wk)
        return false;

    cur = parent;
    cur.key = board.hash();
    const int n = cur.nlist;

    if (is_drop(m)) {
        const PieceType pt = dropped_pt(m);
        const Square    to = to_sq(m);
        const int max_c   = max_hand_count(pt);
        const int new_cnt = board.hand(mover, pt);
        const int old_cnt = new_cnt + 1;
        if (new_cnt < 0 || old_cnt > max_c) return false;

        const int old_f0 = hand_feature_base(pt, mover == BLACK) + old_cnt;
        const int new_f0 = board_feature_checked(mover, pt, to, BLACK);
        const int old_f1 = hand_feature_base(pt, mover == WHITE) + old_cnt;
        const int new_f1 = board_feature_checked(mover, pt, to, WHITE);
        const int bb_idx = board_kkp_checked(mover, pt, to);
        const int hb     = hand_kkp_base(pt);
        if (bb_idx < 0 || hb < 0) return false;

        if (!replace_kpp_feature(sq_bk, cur.list0, n, old_f0, new_f0, cur.sum_bkpp)) return false;
        if (!replace_kpp_feature(sq_wk_inv, cur.list1, n, old_f1, new_f1, cur.sum_wkpp)) return false;

        if (mover == BLACK) {
            cur.sum_kkp += -kkp_lookup(sq_bk, sq_wk, hb + old_cnt)
                          + kkp_lookup(sq_bk, sq_wk, hb + new_cnt)
                          + kkp_lookup(sq_bk, sq_wk, bb_idx);
        } else {
            cur.sum_kkp -= -kkp_lookup(sq_wk_inv, sq_bk_inv, hb + old_cnt)
                          + kkp_lookup(sq_wk_inv, sq_bk_inv, hb + new_cnt)
                          + kkp_lookup(sq_wk_inv, sq_bk_inv, bb_idx);
        }
        return true;
    }

    // Normal / promote move
    const Square    from    = from_sq(m);
    const Square    to      = to_sq(m);
    const PieceType now_pt  = type_of(si.moved_piece);
    const PieceType orig_pt = is_promote(m) ? unpromote_pt(now_pt) : now_pt;

    const int old_f0 = board_feature_checked(mover, orig_pt, from, BLACK);
    const int new_f0 = board_feature_checked(mover, now_pt,  to,   BLACK);
    const int old_f1 = board_feature_checked(mover, orig_pt, from, WHITE);
    const int new_f1 = board_feature_checked(mover, now_pt,  to,   WHITE);
    const int kkp_old_m = board_kkp_checked(mover, orig_pt, from);
    const int kkp_new_m = board_kkp_checked(mover, now_pt,  to);
    if (kkp_old_m < 0 || kkp_new_m < 0) return false;

    if (!replace_kpp_feature(sq_bk, cur.list0, n, old_f0, new_f0, cur.sum_bkpp)) return false;
    if (!replace_kpp_feature(sq_wk_inv, cur.list1, n, old_f1, new_f1, cur.sum_wkpp)) return false;

    if (mover == BLACK) {
        cur.sum_kkp += -kkp_lookup(sq_bk, sq_wk, kkp_old_m)
                      + kkp_lookup(sq_bk, sq_wk, kkp_new_m);
    } else {
        cur.sum_kkp -= -kkp_lookup(sq_wk_inv, sq_bk_inv, kkp_old_m)
                      + kkp_lookup(sq_wk_inv, sq_bk_inv, kkp_new_m);
    }

    if (si.captured != NO_PIECE) {
        const Color     them        = ~mover;
        const PieceType cap_pt      = type_of(si.captured);
        const PieceType cap_hand_pt = to_hand_pt(cap_pt);
        const int       max_c       = max_hand_count(cap_hand_pt);
        const int       new_cnt     = board.hand(mover, cap_hand_pt);
        const int       old_cnt     = new_cnt - 1;
        if (old_cnt < 0 || new_cnt > max_c) return false;

        const int cap_old_f0    = board_feature_checked(them, cap_pt, to, BLACK);
        const int cap_new_f0    = hand_feature_base(cap_hand_pt, mover == BLACK) + new_cnt;
        const int cap_old_f1    = board_feature_checked(them, cap_pt, to, WHITE);
        const int cap_new_f1    = hand_feature_base(cap_hand_pt, mover == WHITE) + new_cnt;
        const int kkp_cap_board = board_kkp_checked(them, cap_pt, to);
        const int hb            = hand_kkp_base(cap_hand_pt);
        if (kkp_cap_board < 0 || hb < 0) return false;

        if (!replace_kpp_feature(sq_bk, cur.list0, n, cap_old_f0, cap_new_f0, cur.sum_bkpp)) return false;
        if (!replace_kpp_feature(sq_wk_inv, cur.list1, n, cap_old_f1, cap_new_f1, cur.sum_wkpp)) return false;

        if (mover == BLACK) {
            // Captured a WHITE piece (which had been subtracted in sum_kkp) + BLACK gained hand piece
            cur.sum_kkp += kkp_lookup(sq_wk_inv, sq_bk_inv, kkp_cap_board)
                         - kkp_lookup(sq_bk, sq_wk, hb + old_cnt)
                         + kkp_lookup(sq_bk, sq_wk, hb + new_cnt);
        } else {
            // Captured a BLACK piece (which had been added in sum_kkp) + WHITE gained hand piece
            cur.sum_kkp += -kkp_lookup(sq_bk, sq_wk, kkp_cap_board)
                          + kkp_lookup(sq_wk_inv, sq_bk_inv, hb + old_cnt)
                          - kkp_lookup(sq_wk_inv, sq_bk_inv, hb + new_cnt);
        }
    }

    return true;
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
        return false;
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
    bump_eval_epoch();
    const std::string abs_path = make_absolute_path(path);
    g_status = "bonanza-v6 fv.bin loaded from " + (abs_path.empty() ? path : abs_path) + " [" + tag
             + "] [family=BONANZA_V6_FV] [size=" + std::to_string(static_cast<long long>(size))
             + " bytes] [fv_scale=" + std::to_string(static_cast<long long>(g_fv_scale)) + "]";
    g_family = EvalFamily::BONANZA_V6_FV;
    return true;
}

void try_load_eval_file_once() {
    if (g_loaded_once) return;
    g_loaded_once = true;

    if (g_explicit_path && !g_eval_file_path.empty()) {
        try_load_from_path(g_eval_file_path, /*is_auto=*/false);
        return;
    }

    {
        const char* env_path = std::getenv("SHOGIAI_EVAL_FILE");
        if (env_path && *env_path) {
            try_load_from_path(std::string(env_path), /*is_auto=*/false);
            return;
        }
    }

    const std::vector<std::string> candidates = build_discovery_candidates();
    for (const std::string& candidate : candidates) {
        if (try_load_from_path(candidate, /*is_auto=*/true)) return;
        if (g_family == EvalFamily::NNUE_UNSUPPORTED) return;
    }

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
    g_eval_file_path = path;
    g_explicit_path  = !path.empty();
    g_loaded_once    = false;
    g_tables         = FvTables{};
    g_status         = "material-only fallback (fv.bin missing)";
    g_family         = EvalFamily::MATERIAL_FALLBACK;
    bump_eval_epoch();
}

void set_fv_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 256) scale = 256;
    if (g_fv_scale != scale) {
        g_fv_scale = scale;
        bump_eval_epoch();
    }
}

int get_fv_scale() {
    return static_cast<int>(g_fv_scale);
}

void clear_eval_caches() {
    bump_eval_epoch();
}

std::string eval_status_message() {
    try_load_eval_file_once();
    return g_status;
}

EvalFamily get_eval_family() {
    try_load_eval_file_once();
    return g_family;
}

int evaluate_full_reference(const Board& board) {
    try_load_eval_file_once();
    const Color us = board.side_to_move();
    const int material_black = board.material_black();

    if (g_family != EvalFamily::BONANZA_V6_FV ||
        board.king_sq(BLACK) == SQ_NONE || board.king_sq(WHITE) == SQ_NONE) {
        return (us == BLACK) ? material_black : -material_black;
    }

    EvalStateEntry st;
    build_full_eval_state(board, st);

    int64_t raw = static_cast<int64_t>(material_black) * g_fv_scale
                + st.sum_kkp + st.sum_bkpp - st.sum_wkpp;
    if (us == WHITE) raw = -raw;
    return static_cast<int>(raw / g_fv_scale);
}

int evaluate(const Board& board) {
    try_load_eval_file_once();

    const Color us = board.side_to_move();
    const int material_black = board.material_black();

    if (g_family != EvalFamily::BONANZA_V6_FV ||
        board.king_sq(BLACK) == SQ_NONE || board.king_sq(WHITE) == SQ_NONE) {
        return (us == BLACK) ? material_black : -material_black;
    }

    // 1. Probe Evaluation Hash Table (O(1) hit across transpositions / qsearch / IID / PVS)
    //    Mask out side-to-move bit so Black-to-move and White-to-move share the same Black-POV slot!
    const uint64_t pos_key = (us == WHITE) ? (board.hash() ^ Zobrist::side) : board.hash();
    EvalHashEntry& eh = g_ehash[pos_key & EHASH_MASK];
    if (eh.epoch == g_eval_epoch && eh.key == pos_key) {
        return (us == BLACK) ? eh.score_black : -eh.score_black;
    }

    // 2. Check if full state is already in `g_state_cache`, else try O(N) incremental update from parent!
    EvalStateEntry& slot = g_state_cache[board.hash() & STATE_CACHE_MASK];
    if (slot.epoch != g_eval_epoch || slot.key != board.hash()) {
        EvalStateEntry next_state;
        if (!try_incremental_eval_state(board, next_state)) {
            build_full_eval_state(board, next_state);
        }
        slot = next_state;
    }

    const int64_t raw_black = static_cast<int64_t>(material_black) * g_fv_scale
                            + slot.sum_kkp + slot.sum_bkpp - slot.sum_wkpp;
    // Note: in C++ integer division toward zero, `(-x) / S == -(x / S)` always holds!
    const int score_black = static_cast<int>(raw_black / g_fv_scale);

    eh.key         = pos_key;
    eh.score_black = score_black;
    eh.epoch       = g_eval_epoch;

    return (us == BLACK) ? score_black : -score_black;
}
