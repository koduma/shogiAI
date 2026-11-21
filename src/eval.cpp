#include "eval.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <type_traits>
#include <vector>

#if defined(__AVX2__) || defined(__SSE2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

// ============================================================
// YaneuraOu SFNNWithoutPsqt ("nn.bin" + "progress.bin") Evaluator
//   Target model: TsuyouraOu11mini `SFNN_ka2_4096_8_64_progress8`
//     (and compatible `SFNN_ka2_<L1>_8_64_progress<B>` architectures)
//
//   1. `nn.bin` Header & Architecture:
//      - version = 0x7AF32F16
//      - arch = "ModelType=SFNNWithoutPsqt;Features=K-A2(Friend)[1791-><L1>x2],Network=SFNN-<L1>{LayerStack=<B>}"
//      - FeatureTransformer:
//          biases  : int16_t[L1]          (COMPRESSED_LEB128 or raw LE)
//          weights : int16_t[1791][L1]    (COMPRESSED_LEB128 or raw LE)
//          K-A2(Friend) 1791 features (YaneuraOu file-major sq = (8 - file_of(s))*9 + rank_of(s)):
//            0..89     : 1-based BonaPiece hand slots
//            90..1547  : BonaPiece non-king board pieces (`fe_end = 1548`)
//            1548..1628: A2 merged king plane (`1548 + sq_fk` and `1548 + sq_ek`)
//            1629..1709: K own-king plane (`1629 + sq_fk`)
//            1710..1790: K enemy-king plane (`1710 + sq_ek`)
//          At load time we pre-fold `1548+sq` with `1629+sq` (`f_king_folded[sq]`) and
//          `1548+sq` with `1710+sq` (`e_king_folded[sq]`) into `[1710][L1]`, so EVERY
//          legal Shogi move (including king moves!) updates at most 1 or 2 features!
//          Pairwise activation (`uint8_t[L1]` across `[stm, ~stm]`):
//            `(clamp(acc[j], 0, 127) * clamp(acc[j + L1/2], 0, 127)) >> 7`
//      - Each LayerStack (`0 .. B-1`):
//          fc0 : int32_t bias[8]  + int8_t weight[8][L1]
//                -> Dual activation: SqrClippedReLU `[0..7]` = `clamp((int64_t(z)*z)>>19, 0, 127)`
//                                    ClippedReLU    `[8..15]` = `clamp(z>>6, 0, 127)`
//          fc1 : int32_t bias[64] + int8_t weight[64][32] (cols 0..15 active, 16..31 padded 0)
//                -> ClippedReLU `[0..63]` = `clamp(z>>6, 0, 127)`
//          fc2 : int32_t bias[1]  + int8_t weight[64]
//                -> Final score = `clamp(out / FV_SCALE, -30000, 30000)` (default `FV_SCALE = 24`)
//
//   2. `progress.bin` (`1,003,104` bytes = `double[81][1548]`):
//      - Headerless IEEE-754 float64 KP logistic-regression weights quantized to Q16 (`* 65536`).
//      - `sum_q16 = sum_{p in fb_nonking} w[sq_bk][p] + sum_{p in fw_nonking} w[Inv(sq_wk)][p]`
//      - Selects LayerStack bucket `0 .. B-1` via `B-1` log-odds thresholds `log((i/B)/(1 - i/B)) * 65536`.
// ============================================================

namespace {

#ifndef SHOGIAI_SOURCE_DIR
#define SHOGIAI_SOURCE_DIR ""
#endif

#if !defined(_WIN32) && !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

constexpr uint32_t kNnueVersion        = 0x7AF32F16u;
constexpr int      kKa2RawDims         = 1791;
constexpr int      kFoldedDims         = 1710; // 0..1547 non-kings, 1548..1628 f_king_folded, 1629..1709 e_king_folded
constexpr int      kBonaFeEnd          = 1548;
constexpr int      kYoSqNb             = 81;
constexpr int64_t  kProgressBinBytes   = static_cast<int64_t>(kYoSqNb) * kBonaFeEnd * static_cast<int64_t>(sizeof(double)); // 1,003,104
constexpr double   kProgressQ16Scale   = 65536.0;
constexpr int      kMaxEvalScore       = 30000;
constexpr int      kMaxSupportedL1     = 4096; // supports 4096 (11mini), 2048 (11micro), 1024, 512, 256, 64

// -----------------------------------------------------------------------
// Aligned dynamic buffer helper (64-byte cache-line aligned for AVX-512/AVX2)
// -----------------------------------------------------------------------
template <typename T>
struct AlignedBuffer {
    static_assert(std::is_trivially_copyable<T>::value && std::is_trivially_destructible<T>::value,
                  "AlignedBuffer<T> requires trivially copyable/destructible T");

    uint8_t* raw  = nullptr;
    T*       ptr  = nullptr;
    size_t   size = 0;

    AlignedBuffer() = default;
    ~AlignedBuffer() { reset(); }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& o) noexcept : raw(o.raw), ptr(o.ptr), size(o.size) {
        o.raw  = nullptr;
        o.ptr  = nullptr;
        o.size = 0;
    }
    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            reset();
            raw    = o.raw;
            ptr    = o.ptr;
            size   = o.size;
            o.raw  = nullptr;
            o.ptr  = nullptr;
            o.size = 0;
        }
        return *this;
    }

    void allocate(size_t n) {
        reset();
        if (n == 0) return;
        const size_t bytes = ((n * sizeof(T) + 63u) / 64u) * 64u;
        const size_t alloc_bytes = bytes + 64u;
        raw = new uint8_t[alloc_bytes];
        std::memset(raw, 0, alloc_bytes);
        const uintptr_t aligned_addr = (reinterpret_cast<uintptr_t>(raw) + 63u) & ~static_cast<uintptr_t>(63u);
        ptr  = reinterpret_cast<T*>(aligned_addr);
        size = n;
    }

    void reset() {
        delete[] raw;
        raw  = nullptr;
        ptr  = nullptr;
        size = 0;
    }

    T& operator[](size_t i) { return ptr[i]; }
    const T& operator[](size_t i) const { return ptr[i]; }
};

// -----------------------------------------------------------------------
// Path helpers
// -----------------------------------------------------------------------
std::string path_parent_dir(const std::string& path) {
    const std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string path_join(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + file;
    return dir + "/" + file;
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

// -----------------------------------------------------------------------
// YaneuraOu BonaPiece & K-A2 coordinate mapping
//   Internal `Square s` (`types.hpp`): rank-major (`rank * 9 + file`, file 9->1 = 0..8, rank a->i = 0..8)
//   YaneuraOu `Square` (`nn.bin` & `progress.bin`): file-major (`(8 - file_of(s)) * 9 + rank_of(s)`)
//     SQ_11 ("1a") = 0, SQ_19 ("1i") = 8, ..., SQ_91 ("9a") = 72, SQ_99 ("9i") = 80
//     Inv(yo_sq)   = 80 - yo_sq
// -----------------------------------------------------------------------
enum YoBonaPiece : int {
    YO_BONA_ZERO     = 0,
    YO_F_HAND_PAWN   = 1,   YO_E_HAND_PAWN   = 20,
    YO_F_HAND_LANCE  = 39,  YO_E_HAND_LANCE  = 44,
    YO_F_HAND_KNIGHT = 49,  YO_E_HAND_KNIGHT = 54,
    YO_F_HAND_SILVER = 59,  YO_E_HAND_SILVER = 64,
    YO_F_HAND_GOLD   = 69,  YO_E_HAND_GOLD   = 74,
    YO_F_HAND_BISHOP = 79,  YO_E_HAND_BISHOP = 82,
    YO_F_HAND_ROOK   = 85,  YO_E_HAND_ROOK   = 88,
    YO_FE_HAND_END   = 90,

    YO_F_PAWN   = 90,   YO_E_PAWN   = 171,
    YO_F_LANCE  = 252,  YO_E_LANCE  = 333,
    YO_F_KNIGHT = 414,  YO_E_KNIGHT = 495,
    YO_F_SILVER = 576,  YO_E_SILVER = 657,
    YO_F_GOLD   = 738,  YO_E_GOLD   = 819,
    YO_F_BISHOP = 900,  YO_E_BISHOP = 981,
    YO_F_HORSE  = 1062, YO_E_HORSE  = 1143,
    YO_F_ROOK   = 1224, YO_E_ROOK   = 1305,
    YO_F_DRAGON = 1386, YO_E_DRAGON = 1467,
    YO_FE_END   = 1548,

    // Folded king indices in `ft_weights_folded` (`[1548..1628]` = f_king, `[1629..1709]` = e_king)
    YO_FOLDED_F_KING = 1548,
    YO_FOLDED_E_KING = 1629,

    // Raw K-A2 king indices in `nn.bin` (`[1548..1628]` = A2 merged king, `[1629..1709]` = f_king, `[1710..1790]` = e_king)
    YO_RAW_A2_KING   = 1548,
    YO_RAW_F_KING    = 1629,
    YO_RAW_E_KING    = 1710
};

// Precomputed lookup table converting internal `Square s` (0..80) -> YaneuraOu file-major square (0..80)
const std::array<int, SQUARE_NB> TO_YO_SQ = [] {
    std::array<int, SQUARE_NB> t{};
    for (int s = 0; s < SQUARE_NB; ++s) {
        const int yo_file = 8 - file_of(s);
        const int yo_rank = rank_of(s);
        t[s] = yo_file * 9 + yo_rank;
    }
    return t;
}();

inline int to_yo_sq(Square s) { return TO_YO_SQ[s]; }
inline int yo_inv(int yo_sq)  { return 80 - yo_sq; }
inline int yo_view_sq(Square s, Color view) {
    const int ysq = to_yo_sq(s);
    return (view == BLACK) ? ysq : yo_inv(ysq);
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

inline int yo_hand_base(Color view, Color owner, PieceType pt) {
    const bool friend_piece = (view == owner);
    switch (pt) {
        case PAWN:   return friend_piece ? YO_F_HAND_PAWN   : YO_E_HAND_PAWN;
        case LANCE:  return friend_piece ? YO_F_HAND_LANCE  : YO_E_HAND_LANCE;
        case KNIGHT: return friend_piece ? YO_F_HAND_KNIGHT : YO_E_HAND_KNIGHT;
        case SILVER: return friend_piece ? YO_F_HAND_SILVER : YO_E_HAND_SILVER;
        case GOLD:   return friend_piece ? YO_F_HAND_GOLD   : YO_E_HAND_GOLD;
        case BISHOP: return friend_piece ? YO_F_HAND_BISHOP : YO_E_HAND_BISHOP;
        case ROOK:   return friend_piece ? YO_F_HAND_ROOK   : YO_E_HAND_ROOK;
        default:     return -1;
    }
}

// Returns BonaPiece (90..1547) for non-king board pieces, or folded king slot (1548..1709) for kings.
inline int yo_folded_board_feature(Color view, Piece p, Square s) {
    const bool      friend_piece = (color_of(p) == view);
    const PieceType pt           = type_of(p);
    const int       vsq          = yo_view_sq(s, view);
    switch (pt) {
        case PAWN:        return (friend_piece ? YO_F_PAWN   : YO_E_PAWN)   + vsq;
        case LANCE:       return (friend_piece ? YO_F_LANCE  : YO_E_LANCE)  + vsq;
        case KNIGHT:      return (friend_piece ? YO_F_KNIGHT : YO_E_KNIGHT) + vsq;
        case SILVER:      return (friend_piece ? YO_F_SILVER : YO_E_SILVER) + vsq;
        case GOLD:
        case PROM_PAWN:
        case PROM_LANCE:
        case PROM_KNIGHT:
        case PROM_SILVER: return (friend_piece ? YO_F_GOLD   : YO_E_GOLD)   + vsq;
        case BISHOP:      return (friend_piece ? YO_F_BISHOP : YO_E_BISHOP) + vsq;
        case PROM_BISHOP: return (friend_piece ? YO_F_HORSE  : YO_E_HORSE)  + vsq;
        case ROOK:        return (friend_piece ? YO_F_ROOK   : YO_E_ROOK)   + vsq;
        case PROM_ROOK:   return (friend_piece ? YO_F_DRAGON : YO_E_DRAGON) + vsq;
        case KING:        return (friend_piece ? YO_FOLDED_F_KING : YO_FOLDED_E_KING) + vsq;
        default:          return -1;
    }
}

// -----------------------------------------------------------------------
// Model Data Structures
// -----------------------------------------------------------------------
struct SfnnLayerStack {
    alignas(16) int32_t fc0_b[8]{};
    AlignedBuffer<int8_t> fc0_w;            // [8 * half_dims], row-major [out][in]
    alignas(16) int32_t fc1_b[64]{};
    alignas(16) int8_t  fc1_w_packed[64 * 16]{}; // active 16 cols per output row
    alignas(16) int8_t  fc1_w_raw32[64 * 32]{};  // full 32 cols from file (for reference check)
    int32_t             fc2_b = 0;
    alignas(16) int8_t  fc2_w[64]{};
};

struct SfnnModel {
    int      half_dims    = 0; // e.g. 4096 (11mini) or 2048 (11micro)
    int      layer_stacks = 0; // e.g. 8
    uint32_t file_hash    = 0;
    uint32_t ft_hash      = 0;
    std::string arch_str;

    AlignedBuffer<int16_t> ft_biases;          // [half_dims]
    AlignedBuffer<int16_t> ft_weights_folded;  // [1710 * half_dims]
    // Un-folded king planes preserved for `evaluate_full_reference()` exactness test:
    AlignedBuffer<int16_t> ft_king_a2;         // [81 * half_dims] (raw 1548..1628)
    AlignedBuffer<int16_t> ft_king_f;          // [81 * half_dims] (raw 1629..1709)
    AlignedBuffer<int16_t> ft_king_e;          // [81 * half_dims] (raw 1710..1790)

    std::vector<SfnnLayerStack> stacks;

    bool                  progress_loaded = false;
    std::vector<int32_t>  progress_q16;        // [81 * 1548]
    std::vector<int64_t>  progress_thresholds; // [layer_stacks - 1]
};

// -----------------------------------------------------------------------
// Evaluation Hash + Incremental Accumulator State Cache
// -----------------------------------------------------------------------
constexpr size_t EHASH_SIZE = 1u << 18; // 262,144 entries
constexpr size_t EHASH_MASK = EHASH_SIZE - 1;
struct EvalHashEntry {
    uint64_t key         = 0;
    int32_t  score_stm   = 0;
    uint32_t epoch       = 0;
};

constexpr size_t NNUE_CACHE_SIZE = 1u << 10; // 1,024 entries (~16 MiB at L1=4096; covers full search tree spine + branches)
constexpr size_t NNUE_CACHE_MASK = NNUE_CACHE_SIZE - 1;
struct NnueStateEntry {
    int16_t  acc[COLOR_NB][kMaxSupportedL1];
    uint64_t pos_key;     // Zobrist key ignoring side-to-move (`board.hash() ^ (stm==WHITE ? Zobrist::side : 0)`)
    int64_t  sum_bk_q16;
    int64_t  sum_wk_q16;
    uint32_t epoch;
    int16_t  yo_bk;
    int16_t  yo_wk_inv;
    uint8_t  pad[32];     // pad to exact 64-byte multiple so every entry's `acc` is 64B cache-line aligned
};
static_assert((sizeof(NnueStateEntry) % 64) == 0, "NnueStateEntry size must be a multiple of 64 bytes");

// Global state
bool        g_loaded_once         = false;
bool        g_explicit_eval_file  = false;
bool        g_explicit_prog_file  = false;
bool        g_explicit_eval_dir   = false;
int         g_fv_scale            = 24;
uint32_t    g_cache_epoch         = 1;
uint32_t    g_config_epoch        = 1;
std::string g_eval_dir            = "eval";
std::string g_eval_file_path;
std::string g_prog_file_path;
std::string g_status              = "material-only fallback (nn.bin not loaded)";
EvalFamily  g_family              = EvalFamily::MATERIAL_FALLBACK;
SfnnModel   g_model;

std::vector<EvalHashEntry>        g_ehash(EHASH_SIZE);
AlignedBuffer<NnueStateEntry>     g_state_cache;
AlignedBuffer<NnueStateEntry>     g_tmp_state;
AlignedBuffer<uint8_t>            g_tf_buf;

void ensure_state_cache_allocated() {
    if (!g_state_cache.ptr) {
        g_state_cache.allocate(NNUE_CACHE_SIZE);
    }
    if (!g_tmp_state.ptr) {
        g_tmp_state.allocate(2);
    }
    if (!g_tf_buf.ptr) {
        g_tf_buf.allocate(kMaxSupportedL1);
    }
}

void bump_cache_epoch() {
    if (++g_cache_epoch == 0) {
        std::fill(g_ehash.begin(), g_ehash.end(), EvalHashEntry{});
        if (g_state_cache.ptr) {
            for (size_t i = 0; i < NNUE_CACHE_SIZE; ++i) g_state_cache[i].epoch = 0;
        }
        g_cache_epoch = 1;
    }
}

inline uint64_t board_piece_key(const Board& board) {
    return (board.side_to_move() == WHITE) ? (board.hash() ^ Zobrist::side) : board.hash();
}

// -----------------------------------------------------------------------
// Binary Reader Helpers (LE + COMPRESSED_LEB128)
// -----------------------------------------------------------------------
template <typename UInt>
bool read_le(std::istream& is, UInt& out) {
    uint8_t buf[sizeof(UInt)];
    if (!is.read(reinterpret_cast<char*>(buf), sizeof(buf))) return false;
    UInt v = 0;
    for (size_t i = 0; i < sizeof(UInt); ++i) {
        v |= static_cast<UInt>(buf[i]) << (8 * i);
    }
    out = v;
    return true;
}

bool read_int16_leb128_or_raw(std::istream& is, int16_t* dst, size_t count) {
    static constexpr char kMagic[] = "COMPRESSED_LEB128";
    static constexpr size_t kMagicLen = 17;

    const std::streampos start_pos = is.tellg();
    char magic_buf[kMagicLen]{};
    if (is.read(magic_buf, kMagicLen) && std::memcmp(magic_buf, kMagic, kMagicLen) == 0) {
        uint32_t comp_bytes = 0;
        if (!read_le<uint32_t>(is, comp_bytes)) return false;
        std::vector<uint8_t> comp(comp_bytes);
        if (comp_bytes > 0 && !is.read(reinterpret_cast<char*>(comp.data()), comp_bytes)) {
            return false;
        }
        size_t p = 0;
        for (size_t i = 0; i < count; ++i) {
            if (p >= comp_bytes) return false;
            uint32_t val = 0;
            unsigned shift = 0;
            while (true) {
                if (p >= comp_bytes || shift >= 35) return false;
                const uint8_t b = comp[p++];
                val |= static_cast<uint32_t>(b & 0x7Fu) << shift;
                shift += 7;
                if ((b & 0x80u) == 0) {
                    if ((b & 0x40u) != 0 && shift < 32) {
                        val |= (~0u << shift);
                    }
                    break;
                }
            }
            dst[i] = static_cast<int16_t>(static_cast<int32_t>(val));
        }
        return p == comp_bytes;
    }

    is.clear();
    is.seekg(start_pos);
    std::vector<uint8_t> raw(count * 2);
    if (count > 0 && !is.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint16_t u = static_cast<uint16_t>(raw[2 * i]) |
                           (static_cast<uint16_t>(raw[2 * i + 1]) << 8);
        dst[i] = static_cast<int16_t>(u);
    }
    return true;
}

bool parse_sfnn_arch(const std::string& arch, int& out_half_dims, int& out_stacks) {
    // Expected form:
    // "ModelType=SFNNWithoutPsqt;Features=K-A2(Friend)[1791->4096x2],Network=SFNN-4096{LayerStack=8}"
    if (arch.find("ModelType=SFNNWithoutPsqt;") != 0) return false;
    const std::string feat_prefix = "Features=K-A2(Friend)[1791->";
    const auto fp = arch.find(feat_prefix);
    if (fp == std::string::npos) return false;
    const size_t dim_start = fp + feat_prefix.size();
    const auto x2_pos = arch.find("x2],Network=SFNN-", dim_start);
    if (x2_pos == std::string::npos) return false;

    int l1 = 0, net_in = 0, stacks = 0;
    try {
        l1 = std::stoi(arch.substr(dim_start, x2_pos - dim_start));
        const size_t net_start = x2_pos + std::strlen("x2],Network=SFNN-");
        const auto ls_pos = arch.find("{LayerStack=", net_start);
        if (ls_pos == std::string::npos) return false;
        net_in = std::stoi(arch.substr(net_start, ls_pos - net_start));
        const size_t st_start = ls_pos + std::strlen("{LayerStack=");
        const auto brace_end = arch.find('}', st_start);
        if (brace_end == std::string::npos) return false;
        stacks = std::stoi(arch.substr(st_start, brace_end - st_start));
    } catch (...) {
        return false;
    }

    if (l1 != net_in || l1 < 64 || (l1 % 64) != 0 || l1 > kMaxSupportedL1) return false;
    if (stacks < 1 || stacks > 32) return false;
    out_half_dims = l1;
    out_stacks = stacks;
    return true;
}

std::vector<int64_t> make_progress_thresholds_q16(int bucket_count) {
    std::vector<int64_t> thresholds;
    if (bucket_count <= 1) return thresholds;
    thresholds.resize(static_cast<size_t>(bucket_count - 1));
    for (int i = 1; i < bucket_count; ++i) {
        const double p = static_cast<double>(i) / static_cast<double>(bucket_count);
        const double scaled = std::round(std::log(p / (1.0 - p)) * kProgressQ16Scale);
        thresholds[static_cast<size_t>(i - 1)] = static_cast<int64_t>(scaled);
    }
    return thresholds;
}

enum class ProgressLoadResult {
    LOADED,
    MISSING_OR_EMPTY,
    INVALID_FORMAT
};

ProgressLoadResult load_progress_file(const std::string& path, std::vector<int32_t>& out_q16, std::string& err_detail) {
    std::ifstream pf(path, std::ios::binary | std::ios::ate);
    if (!pf.is_open()) {
        err_detail = "progress.bin not found at " + path;
        return ProgressLoadResult::MISSING_OR_EMPTY;
    }
    const int64_t fsize = static_cast<int64_t>(pf.tellg());
    if (fsize == 0) {
        err_detail = "empty dummy progress.bin at " + path;
        return ProgressLoadResult::MISSING_OR_EMPTY;
    }
    if (fsize != kProgressBinBytes) {
        std::ostringstream oss;
        oss << "invalid progress.bin size at " << path << " (got " << fsize
            << " bytes, expected " << kProgressBinBytes << " bytes = 81*1548 float64)";
        err_detail = oss.str();
        return ProgressLoadResult::INVALID_FORMAT;
    }

    pf.seekg(0, std::ios::beg);
    std::vector<uint8_t> raw(static_cast<size_t>(kProgressBinBytes));
    if (!pf.read(reinterpret_cast<char*>(raw.data()), kProgressBinBytes)) {
        err_detail = "read error on progress.bin at " + path;
        return ProgressLoadResult::INVALID_FORMAT;
    }

    constexpr size_t count = static_cast<size_t>(kYoSqNb) * kBonaFeEnd;
    out_q16.resize(count);
    for (size_t i = 0; i < count; ++i) {
        uint64_t bits = 0;
        for (int b = 0; b < 8; ++b) {
            bits |= static_cast<uint64_t>(raw[8 * i + b]) << (8 * b);
        }
        // Reject NaN / Inf bit patterns directly
        if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) {
            err_detail = "invalid NaN/Inf entry in progress.bin at " + path;
            return ProgressLoadResult::INVALID_FORMAT;
        }
        double val = 0.0;
        std::memcpy(&val, &bits, sizeof(val));
        const double scaled = std::clamp(
            std::round(val * kProgressQ16Scale),
            static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max()));
        out_q16[i] = static_cast<int32_t>(scaled);
    }
    return ProgressLoadResult::LOADED;
}

bool load_sfnn_model_from_files(const std::string& nn_path, const std::string& prog_path,
                                const char* source_tag, SfnnModel& out_model, std::string& out_status) {
    std::ifstream nf(nn_path, std::ios::binary | std::ios::ate);
    if (!nf.is_open()) {
        out_status = "material-only fallback (file not found: " + nn_path + ")";
        return false;
    }
    const int64_t nn_size = static_cast<int64_t>(nf.tellg());
    if (nn_size == 0) {
        out_status = "material-only fallback (empty dummy nn.bin at " + nn_path + ")";
        return false;
    }
    if (nn_size < 16) {
        std::ostringstream oss;
        oss << "material-only fallback (invalid nn.bin size: got " << nn_size
            << " bytes at " << nn_path << ")";
        out_status = oss.str();
        return false;
    }

    nf.seekg(0, std::ios::beg);
    uint32_t version = 0, file_hash = 0, arch_len = 0;
    if (!read_le<uint32_t>(nf, version) || !read_le<uint32_t>(nf, file_hash) || !read_le<uint32_t>(nf, arch_len)) {
        out_status = "material-only fallback (header read failed: " + nn_path + ")";
        return false;
    }
    if (version != kNnueVersion || arch_len == 0 || arch_len > 4096) {
        std::ostringstream oss;
        oss << "material-only fallback (invalid nn.bin header version=0x" << std::hex << version
            << std::dec << " at " << nn_path << ")";
        out_status = oss.str();
        return false;
    }

    std::string arch(arch_len, '\0');
    if (!nf.read(&arch[0], arch_len)) {
        out_status = "material-only fallback (arch string truncated: " + nn_path + ")";
        return false;
    }

    int half_dims = 0, layer_stacks = 0;
    if (!parse_sfnn_arch(arch, half_dims, layer_stacks)) {
        out_status = "material-only fallback (unsupported NNUE architecture '" + arch + "' in " + nn_path + ")";
        return false;
    }

    uint32_t ft_hash = 0;
    if (!read_le<uint32_t>(nf, ft_hash)) {
        out_status = "material-only fallback (FT hash read failed: " + nn_path + ")";
        return false;
    }

    SfnnModel mdl;
    mdl.half_dims    = half_dims;
    mdl.layer_stacks = layer_stacks;
    mdl.file_hash    = file_hash;
    mdl.ft_hash      = ft_hash;
    mdl.arch_str     = arch;

    mdl.ft_biases.allocate(static_cast<size_t>(half_dims));
    if (!read_int16_leb128_or_raw(nf, mdl.ft_biases.ptr, static_cast<size_t>(half_dims))) {
        out_status = "material-only fallback (FT biases decode failed: " + nn_path + ")";
        return false;
    }

    std::vector<int16_t> raw_ft_w(static_cast<size_t>(kKa2RawDims) * half_dims);
    if (!read_int16_leb128_or_raw(nf, raw_ft_w.data(), raw_ft_w.size())) {
        out_status = "material-only fallback (FT weights decode failed: " + nn_path + ")";
        return false;
    }

    // Populate folded FT weights (`[1710][half_dims]`) and un-folded king planes (`[81][half_dims]`)
    mdl.ft_weights_folded.allocate(static_cast<size_t>(kFoldedDims) * half_dims);
    mdl.ft_king_a2.allocate(static_cast<size_t>(kYoSqNb) * half_dims);
    mdl.ft_king_f.allocate(static_cast<size_t>(kYoSqNb) * half_dims);
    mdl.ft_king_e.allocate(static_cast<size_t>(kYoSqNb) * half_dims);

    std::memcpy(mdl.ft_weights_folded.ptr, raw_ft_w.data(),
                static_cast<size_t>(YO_FE_END) * half_dims * sizeof(int16_t));
    std::memcpy(mdl.ft_king_a2.ptr, raw_ft_w.data() + static_cast<size_t>(YO_RAW_A2_KING) * half_dims,
                static_cast<size_t>(kYoSqNb) * half_dims * sizeof(int16_t));
    std::memcpy(mdl.ft_king_f.ptr, raw_ft_w.data() + static_cast<size_t>(YO_RAW_F_KING) * half_dims,
                static_cast<size_t>(kYoSqNb) * half_dims * sizeof(int16_t));
    std::memcpy(mdl.ft_king_e.ptr, raw_ft_w.data() + static_cast<size_t>(YO_RAW_E_KING) * half_dims,
                static_cast<size_t>(kYoSqNb) * half_dims * sizeof(int16_t));

    for (int sq = 0; sq < kYoSqNb; ++sq) {
        const int16_t* a2 = mdl.ft_king_a2.ptr + static_cast<size_t>(sq) * half_dims;
        const int16_t* fk = mdl.ft_king_f.ptr  + static_cast<size_t>(sq) * half_dims;
        const int16_t* ek = mdl.ft_king_e.ptr  + static_cast<size_t>(sq) * half_dims;
        int16_t* dst_fk = mdl.ft_weights_folded.ptr + static_cast<size_t>(YO_FOLDED_F_KING + sq) * half_dims;
        int16_t* dst_ek = mdl.ft_weights_folded.ptr + static_cast<size_t>(YO_FOLDED_E_KING + sq) * half_dims;
        for (int j = 0; j < half_dims; ++j) {
            dst_fk[j] = static_cast<int16_t>(a2[j] + fk[j]);
            dst_ek[j] = static_cast<int16_t>(a2[j] + ek[j]);
        }
    }

    // Read LayerStacks
    mdl.stacks.resize(static_cast<size_t>(layer_stacks));
    for (int s = 0; s < layer_stacks; ++s) {
        SfnnLayerStack& st = mdl.stacks[static_cast<size_t>(s)];
        uint32_t net_hash = 0;
        if (!read_le<uint32_t>(nf, net_hash)) {
            out_status = "material-only fallback (network stack hash read failed: " + nn_path + ")";
            return false;
        }
        for (int o = 0; o < 8; ++o) {
            uint32_t u = 0;
            if (!read_le<uint32_t>(nf, u)) return false;
            st.fc0_b[o] = static_cast<int32_t>(u);
        }
        st.fc0_w.allocate(static_cast<size_t>(8 * half_dims));
        if (!nf.read(reinterpret_cast<char*>(st.fc0_w.ptr), 8 * half_dims)) {
            out_status = "material-only fallback (fc0 weights truncated: " + nn_path + ")";
            return false;
        }
        for (int o = 0; o < 64; ++o) {
            uint32_t u = 0;
            if (!read_le<uint32_t>(nf, u)) return false;
            st.fc1_b[o] = static_cast<int32_t>(u);
        }
        if (!nf.read(reinterpret_cast<char*>(st.fc1_w_raw32), 64 * 32)) {
            out_status = "material-only fallback (fc1 weights truncated: " + nn_path + ")";
            return false;
        }
        for (int o = 0; o < 64; ++o) {
            std::memcpy(st.fc1_w_packed + o * 16, st.fc1_w_raw32 + o * 32, 16);
        }
        {
            uint32_t u = 0;
            if (!read_le<uint32_t>(nf, u)) return false;
            st.fc2_b = static_cast<int32_t>(u);
        }
        if (!nf.read(reinterpret_cast<char*>(st.fc2_w), 64)) {
            out_status = "material-only fallback (fc2 weights truncated: " + nn_path + ")";
            return false;
        }
    }

    if (nf.peek() != std::char_traits<char>::eof()) {
        out_status = "material-only fallback (unexpected trailing bytes in " + nn_path + ")";
        return false;
    }

    // Load companion `progress.bin` when `layer_stacks > 1`
    mdl.progress_thresholds = make_progress_thresholds_q16(layer_stacks);
    std::string prog_note = "progress=unused(1-stack)";
    if (layer_stacks > 1) {
        std::string prog_err;
        const ProgressLoadResult pr = load_progress_file(prog_path, mdl.progress_q16, prog_err);
        if (pr == ProgressLoadResult::INVALID_FORMAT) {
            out_status = "material-only fallback (" + prog_err + ")";
            return false;
        } else if (pr == ProgressLoadResult::LOADED) {
            mdl.progress_loaded = true;
            prog_note = "progress=" + prog_path;
        } else {
            mdl.progress_loaded = false;
            prog_note = "progress=heuristic-fallback(" + prog_err + ")";
        }
    }

    out_model = std::move(mdl);
    std::ostringstream oss;
    oss << "SFNN NNUE loaded from " << nn_path << " [" << source_tag
        << "] [arch=" << arch << "] [" << prog_note
        << "] [FV_SCALE=" << g_fv_scale << "] [family=SFNN_NNUE]";
    out_status = oss.str();
    return true;
}

std::vector<std::string> build_discovery_dirs() {
    std::vector<std::string> dirs;
    if (SHOGIAI_SOURCE_DIR[0] != '\0') {
        dirs.push_back(std::string(SHOGIAI_SOURCE_DIR) + "/src/eval");
    }
    const std::string exe_dir = get_executable_dir();
    if (!exe_dir.empty()) {
        dirs.push_back(exe_dir + "/eval");
        dirs.push_back(exe_dir + "/src/eval");
        dirs.push_back(exe_dir + "/../eval");
        dirs.push_back(exe_dir + "/../src/eval");
    }
    dirs.push_back("src/eval");
    dirs.push_back("eval");
    return dirs;
}

void ensure_loaded() {
    if (g_loaded_once) return;
    g_loaded_once = true;
    g_family      = EvalFamily::MATERIAL_FALLBACK;
    g_model       = SfnnModel{};
    bump_cache_epoch();

    // Priority 1: Explicit `set_eval_file_path` (USI `EvalFile`)
    if (g_explicit_eval_file && !g_eval_file_path.empty()) {
        const std::string nn_p = g_eval_file_path;
        const std::string pr_p = (g_explicit_prog_file && !g_prog_file_path.empty())
                               ? g_prog_file_path
                               : path_join(path_parent_dir(nn_p), "progress.bin");
        if (load_sfnn_model_from_files(nn_p, pr_p, "explicit", g_model, g_status)) {
            g_family = EvalFamily::SFNN_NNUE;
            ensure_state_cache_allocated();
        }
        return;
    }

    // Priority 2: Explicit `set_eval_dir` (USI `EvalDir`)
    if (g_explicit_eval_dir && !g_eval_dir.empty()) {
        const std::string nn_p = path_join(g_eval_dir, "nn.bin");
        const std::string pr_p = (g_explicit_prog_file && !g_prog_file_path.empty())
                               ? g_prog_file_path
                               : path_join(g_eval_dir, "progress.bin");
        if (load_sfnn_model_from_files(nn_p, pr_p, "eval_dir", g_model, g_status)) {
            g_family = EvalFamily::SFNN_NNUE;
            ensure_state_cache_allocated();
        }
        return;
    }

    // Priority 3: Environment variables `SHOGIAI_EVAL_FILE` / `SHOGIAI_EVAL_DIR`
    if (const char* env_file = std::getenv("SHOGIAI_EVAL_FILE")) {
        if (env_file[0] != '\0') {
            const std::string nn_p(env_file);
            const char* env_prog = std::getenv("SHOGIAI_PROGRESS_FILE");
            const std::string pr_p = (env_prog && env_prog[0] != '\0')
                                   ? std::string(env_prog)
                                   : path_join(path_parent_dir(nn_p), "progress.bin");
            if (load_sfnn_model_from_files(nn_p, pr_p, "env:SHOGIAI_EVAL_FILE", g_model, g_status)) {
                g_family = EvalFamily::SFNN_NNUE;
                ensure_state_cache_allocated();
            }
            return;
        }
    }
    if (const char* env_dir = std::getenv("SHOGIAI_EVAL_DIR")) {
        if (env_dir[0] != '\0') {
            const std::string dir(env_dir);
            const std::string nn_p = path_join(dir, "nn.bin");
            const std::string pr_p = path_join(dir, "progress.bin");
            if (load_sfnn_model_from_files(nn_p, pr_p, "env:SHOGIAI_EVAL_DIR", g_model, g_status)) {
                g_family = EvalFamily::SFNN_NNUE;
                ensure_state_cache_allocated();
            }
            return;
        }
    }

    // Priority 4: Auto-discovery across source/executable/cwd directories
    const std::vector<std::string> dirs = build_discovery_dirs();
    std::string first_non_missing_status;
    std::vector<std::string> checked_paths;

    for (const std::string& dir : dirs) {
        const std::string nn_cand = path_join(dir, "nn.bin");
        checked_paths.push_back(nn_cand);
        std::ifstream probe(nn_cand, std::ios::binary | std::ios::ate);
        if (!probe.is_open()) continue;
        probe.close();

        const std::string abs_nn = make_absolute_path(nn_cand);
        const std::string pr_cand = (g_explicit_prog_file && !g_prog_file_path.empty())
                                  ? g_prog_file_path
                                  : path_join(path_parent_dir(abs_nn), "progress.bin");
        std::string st;
        if (load_sfnn_model_from_files(abs_nn, pr_cand, "auto", g_model, st)) {
            g_status = st;
            g_family = EvalFamily::SFNN_NNUE;
            ensure_state_cache_allocated();
            return;
        }
        if (first_non_missing_status.empty()) {
            first_non_missing_status = st;
        }
    }

    std::ostringstream oss;
    if (!first_non_missing_status.empty()) {
        oss << first_non_missing_status << " (checked: ";
    } else {
        oss << "material-only fallback (nn.bin not found; checked: ";
    }
    for (size_t i = 0; i < checked_paths.size(); ++i) {
        if (i) oss << ", ";
        oss << checked_paths[i];
    }
    oss << ")";
    g_status = oss.str();
}

// -----------------------------------------------------------------------
// Material & Heuristic Progress Fallbacks
// -----------------------------------------------------------------------
int material_eval(const Board& board) {
    const int score_black = board.material_black();
    return (board.side_to_move() == BLACK) ? score_black : -score_black;
}

double heuristic_progress(const Board& board) {
    // Smooth phase estimator in [0.0, 1.0] based on ply, hand count, promoted pieces, and king advancement.
    int hand_total = 0;
    for (int c = 0; c < COLOR_NB; ++c)
        for (int pt = PAWN; pt <= ROOK; ++pt)
            hand_total += board.hand(static_cast<Color>(c), static_cast<PieceType>(pt));

    int promoted_cnt = 0;
    for (int s = 0; s < SQUARE_NB; ++s) {
        const Piece p = board.piece_at(s);
        if (p != NO_PIECE && is_promoted(type_of(p))) ++promoted_cnt;
    }

    const double p_ply  = std::clamp(static_cast<double>(board.ply_) / 120.0, 0.0, 1.0);
    const double p_feat = std::clamp((hand_total * 1.5 + promoted_cnt * 3.0) / 36.0, 0.0, 1.0);
    return std::clamp(0.65 * p_ply + 0.35 * p_feat, 0.0, 1.0);
}

int bucket_from_progress_sum_q16(int64_t sum_q16) {
    const auto& th = g_model.progress_thresholds;
    if (th.empty()) return 0;
    const auto it = std::upper_bound(th.begin(), th.end(), sum_q16);
    return static_cast<int>(it - th.begin());
}

// -----------------------------------------------------------------------
// SIMD Kernels for K-A2 Accumulator & SFNN Dense Network
// -----------------------------------------------------------------------
inline void simd_copy_i16(int16_t* dst, const int16_t* src, int dims) {
    std::memcpy(dst, src, static_cast<size_t>(dims) * sizeof(int16_t));
}

inline void simd_add_inplace_i16(int16_t* acc, const int16_t* w, int dims) {
    int j = 0;
#if defined(__AVX512BW__)
    for (; j + 32 <= dims; j += 32) {
        __m512i a = _mm512_loadu_si512(acc + j);
        __m512i b = _mm512_loadu_si512(w + j);
        _mm512_storeu_si512(acc + j, _mm512_add_epi16(a, b));
    }
#elif defined(__AVX2__)
    for (; j + 32 <= dims; j += 32) {
        __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + j));
        __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + j));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + j), _mm256_add_epi16(a0, b0));

        __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + j + 16));
        __m256i b1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w + j + 16));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + j + 16), _mm256_add_epi16(a1, b1));
    }
#elif defined(__SSE2__)
    for (; j + 8 <= dims; j += 8) {
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + j));
        __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(w + j));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(acc + j), _mm_add_epi16(a, b));
    }
#endif
    for (; j < dims; ++j) acc[j] = static_cast<int16_t>(acc[j] + w[j]);
}

inline void simd_sub1_add1_i16(int16_t* dst, const int16_t* src,
                               const int16_t* rem0, const int16_t* add0, int dims) {
    int j = 0;
#if defined(__AVX512BW__)
    for (; j + 64 <= dims; j += 64) {
        __m512i s0 = _mm512_loadu_si512(src + j);
        __m512i r0 = _mm512_loadu_si512(rem0 + j);
        __m512i a0 = _mm512_loadu_si512(add0 + j);
        _mm512_storeu_si512(dst + j, _mm512_add_epi16(_mm512_sub_epi16(s0, r0), a0));

        __m512i s1 = _mm512_loadu_si512(src + j + 32);
        __m512i r1 = _mm512_loadu_si512(rem0 + j + 32);
        __m512i a1 = _mm512_loadu_si512(add0 + j + 32);
        _mm512_storeu_si512(dst + j + 32, _mm512_add_epi16(_mm512_sub_epi16(s1, r1), a1));
    }
#elif defined(__AVX2__)
    for (; j + 32 <= dims; j += 32) {
        __m256i s0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + j));
        __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rem0 + j));
        __m256i a0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add0 + j));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + j), _mm256_add_epi16(_mm256_sub_epi16(s0, r0), a0));

        __m256i s1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + j + 16));
        __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rem0 + j + 16));
        __m256i a1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add0 + j + 16));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + j + 16), _mm256_add_epi16(_mm256_sub_epi16(s1, r1), a1));
    }
#elif defined(__SSE2__)
    for (; j + 8 <= dims; j += 8) {
        __m128i s = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + j));
        __m128i r = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rem0 + j));
        __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(add0 + j));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + j), _mm_add_epi16(_mm_sub_epi16(s, r), a));
    }
#endif
    for (; j < dims; ++j) {
        dst[j] = static_cast<int16_t>(src[j] - rem0[j] + add0[j]);
    }
}

inline void simd_sub2_add2_i16(int16_t* dst, const int16_t* src,
                               const int16_t* rem0, const int16_t* rem1,
                               const int16_t* add0, const int16_t* add1, int dims) {
    int j = 0;
#if defined(__AVX512BW__)
    for (; j + 32 <= dims; j += 32) {
        __m512i v = _mm512_loadu_si512(src + j);
        v = _mm512_sub_epi16(v, _mm512_loadu_si512(rem0 + j));
        v = _mm512_sub_epi16(v, _mm512_loadu_si512(rem1 + j));
        v = _mm512_add_epi16(v, _mm512_loadu_si512(add0 + j));
        v = _mm512_add_epi16(v, _mm512_loadu_si512(add1 + j));
        _mm512_storeu_si512(dst + j, v);
    }
#elif defined(__AVX2__)
    for (; j + 16 <= dims; j += 16) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + j));
        v = _mm256_sub_epi16(v, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rem0 + j)));
        v = _mm256_sub_epi16(v, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rem1 + j)));
        v = _mm256_add_epi16(v, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add0 + j)));
        v = _mm256_add_epi16(v, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(add1 + j)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + j), v);
    }
#elif defined(__SSE2__)
    for (; j + 8 <= dims; j += 8) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + j));
        v = _mm_sub_epi16(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(rem0 + j)));
        v = _mm_sub_epi16(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(rem1 + j)));
        v = _mm_add_epi16(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(add0 + j)));
        v = _mm_add_epi16(v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(add1 + j)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + j), v);
    }
#endif
    for (; j < dims; ++j) {
        dst[j] = static_cast<int16_t>(src[j] - rem0[j] - rem1[j] + add0[j] + add1[j]);
    }
}

inline void pairwise_transform_perspective(const int16_t* acc, uint8_t* out, int half_dims) {
    const int half_out = half_dims / 2;
    int j = 0;
#if defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256();
    const __m256i k127 = _mm256_set1_epi16(127);
    for (; j + 32 <= half_out; j += 32) {
        __m256i a0 = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + j)), zero), k127);
        __m256i b0 = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + half_out + j)), zero), k127);
        __m256i p0 = _mm256_srli_epi16(_mm256_mullo_epi16(a0, b0), 7);

        __m256i a1 = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + j + 16)), zero), k127);
        __m256i b1 = _mm256_min_epi16(_mm256_max_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(acc + half_out + j + 16)), zero), k127);
        __m256i p1 = _mm256_srli_epi16(_mm256_mullo_epi16(a1, b1), 7);

        __m256i packed = _mm256_permute4x64_epi64(_mm256_packus_epi16(p0, p1), 0xD8);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + j), packed);
    }
#elif defined(__SSE2__)
    const __m128i zero = _mm_setzero_si128();
    const __m128i k127 = _mm_set1_epi16(127);
    for (; j + 16 <= half_out; j += 16) {
        __m128i a0 = _mm_min_epi16(_mm_max_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + j)), zero), k127);
        __m128i b0 = _mm_min_epi16(_mm_max_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + half_out + j)), zero), k127);
        __m128i p0 = _mm_srli_epi16(_mm_mullo_epi16(a0, b0), 7);

        __m128i a1 = _mm_min_epi16(_mm_max_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + j + 8)), zero), k127);
        __m128i b1 = _mm_min_epi16(_mm_max_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(acc + half_out + j + 8)), zero), k127);
        __m128i p1 = _mm_srli_epi16(_mm_mullo_epi16(a1, b1), 7);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + j), _mm_packus_epi16(p0, p1));
    }
#endif
    for (; j < half_out; ++j) {
        const int s0 = std::clamp<int>(acc[j], 0, 127);
        const int s1 = std::clamp<int>(acc[half_out + j], 0, 127);
        out[j] = static_cast<uint8_t>((s0 * s1) >> 7);
    }
}

#if defined(__AVX2__)
inline int32_t hsum_epi32_avx2(__m256i v) {
    __m128i sum128 = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(1, 0, 3, 2)));
    sum128 = _mm_add_epi32(sum128, _mm_shuffle_epi32(sum128, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtsi128_si32(sum128);
}
#endif

int32_t propagate_sfnn_stack(const SfnnLayerStack& st, const uint8_t* tf, int input_dims) {
    int32_t h0[8];
    for (int o = 0; o < 8; ++o) h0[o] = st.fc0_b[o];

#if defined(__AVX2__)
  {
    __m256i acc0 = _mm256_setzero_si256();
    __m256i acc1 = _mm256_setzero_si256();
    __m256i acc2 = _mm256_setzero_si256();
    __m256i acc3 = _mm256_setzero_si256();
    __m256i acc4 = _mm256_setzero_si256();
    __m256i acc5 = _mm256_setzero_si256();
    __m256i acc6 = _mm256_setzero_si256();
    __m256i acc7 = _mm256_setzero_si256();
#if !(defined(__AVX512VNNI__) && defined(__AVX512VL__))
    const __m256i ones16 = _mm256_set1_epi16(1);
#endif

    const int8_t* w0 = st.fc0_w.ptr + 0 * input_dims;
    const int8_t* w1 = st.fc0_w.ptr + 1 * input_dims;
    const int8_t* w2 = st.fc0_w.ptr + 2 * input_dims;
    const int8_t* w3 = st.fc0_w.ptr + 3 * input_dims;
    const int8_t* w4 = st.fc0_w.ptr + 4 * input_dims;
    const int8_t* w5 = st.fc0_w.ptr + 5 * input_dims;
    const int8_t* w6 = st.fc0_w.ptr + 6 * input_dims;
    const int8_t* w7 = st.fc0_w.ptr + 7 * input_dims;

    for (int i = 0; i < input_dims; i += 32) {
        const __m256i u = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(tf + i));
        if (_mm256_testz_si256(u, u)) continue;
#if defined(__AVX512VNNI__) && defined(__AVX512VL__)
        acc0 = _mm256_dpbusd_epi32(acc0, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + i)));
        acc1 = _mm256_dpbusd_epi32(acc1, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w1 + i)));
        acc2 = _mm256_dpbusd_epi32(acc2, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w2 + i)));
        acc3 = _mm256_dpbusd_epi32(acc3, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w3 + i)));
        acc4 = _mm256_dpbusd_epi32(acc4, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w4 + i)));
        acc5 = _mm256_dpbusd_epi32(acc5, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w5 + i)));
        acc6 = _mm256_dpbusd_epi32(acc6, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w6 + i)));
        acc7 = _mm256_dpbusd_epi32(acc7, u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w7 + i)));
#else
        acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + i))), ones16));
        acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w1 + i))), ones16));
        acc2 = _mm256_add_epi32(acc2, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w2 + i))), ones16));
        acc3 = _mm256_add_epi32(acc3, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w3 + i))), ones16));
        acc4 = _mm256_add_epi32(acc4, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w4 + i))), ones16));
        acc5 = _mm256_add_epi32(acc5, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w5 + i))), ones16));
        acc6 = _mm256_add_epi32(acc6, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w6 + i))), ones16));
        acc7 = _mm256_add_epi32(acc7, _mm256_madd_epi16(_mm256_maddubs_epi16(u, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w7 + i))), ones16));
#endif
    }
    h0[0] += hsum_epi32_avx2(acc0);
    h0[1] += hsum_epi32_avx2(acc1);
    h0[2] += hsum_epi32_avx2(acc2);
    h0[3] += hsum_epi32_avx2(acc3);
    h0[4] += hsum_epi32_avx2(acc4);
    h0[5] += hsum_epi32_avx2(acc5);
    h0[6] += hsum_epi32_avx2(acc6);
    h0[7] += hsum_epi32_avx2(acc7);
  }
#else
    for (int o = 0; o < 8; ++o) {
        const int8_t* w = st.fc0_w.ptr + o * input_dims;
        int32_t s = 0;
        for (int i = 0; i < input_dims; ++i) s += static_cast<int32_t>(tf[i]) * w[i];
        h0[o] += s;
    }
#endif

    // Dual activation: SqrClippedReLU `[0..7]` + ClippedReLU `[8..15]`
    alignas(16) uint8_t h0_act[16];
    for (int o = 0; o < 8; ++o) {
        const int64_t z = h0[o];
        const int64_t sqr = (z * z) >> 19;
        h0_act[o]     = static_cast<uint8_t>(std::clamp<int64_t>(sqr, 0, 127));
        h0_act[8 + o] = static_cast<uint8_t>(std::clamp<int32_t>(h0[o] >> 6, 0, 127));
    }

    // fc1 (`16 -> 64`) + ClippedReLU + fc2 (`64 -> 1`)
#if defined(__AVX2__)
    alignas(16) uint8_t h1_act[64];
    const __m128i u128   = _mm_loadu_si128(reinterpret_cast<const __m128i*>(h0_act));
    const __m256i u256   = _mm256_broadcastsi128_si256(u128);
    const __m256i ones16 = _mm256_set1_epi16(1);
    for (int o = 0; o < 64; o += 2) {
        const __m256i w2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(st.fc1_w_packed + o * 16));
        const __m256i prod32 = _mm256_madd_epi16(_mm256_maddubs_epi16(u256, w2), ones16);
        const __m128i lo = _mm256_castsi256_si128(prod32);
        const __m128i hi = _mm256_extracti128_si256(prod32, 1);
        // Horizontal sum of `lo` (4 int32 -> row o) and `hi` (4 int32 -> row o+1)
        const __m128i pair = _mm_hadd_epi32(_mm_hadd_epi32(lo, hi), _mm_setzero_si128());
        const int32_t z0 = st.fc1_b[o]     + _mm_cvtsi128_si32(pair);
        const int32_t z1 = st.fc1_b[o + 1] + _mm_extract_epi32(pair, 1);
        h1_act[o]     = static_cast<uint8_t>(std::clamp<int32_t>(z0 >> 6, 0, 127));
        h1_act[o + 1] = static_cast<uint8_t>(std::clamp<int32_t>(z1 >> 6, 0, 127));
    }

    const __m256i h1_0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h1_act));
    const __m256i h1_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(h1_act + 32));
    const __m256i fw_0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(st.fc2_w));
    const __m256i fw_1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(st.fc2_w + 32));
    const __m256i s256 = _mm256_add_epi32(
        _mm256_madd_epi16(_mm256_maddubs_epi16(h1_0, fw_0), ones16),
        _mm256_madd_epi16(_mm256_maddubs_epi16(h1_1, fw_1), ones16));
    return st.fc2_b + hsum_epi32_avx2(s256);
#else
    int32_t out = st.fc2_b;
    for (int o = 0; o < 64; ++o) {
        const int8_t* w = st.fc1_w_packed + o * 16;
        int32_t z = st.fc1_b[o];
        for (int i = 0; i < 16; ++i) z += static_cast<int32_t>(h0_act[i]) * w[i];
        const int32_t act = std::clamp<int32_t>(z >> 6, 0, 127);
        out += act * st.fc2_w[o];
    }
    return out;
#endif
}

// -----------------------------------------------------------------------
// Full Refresh & Incremental State Update
// -----------------------------------------------------------------------
void build_nonking_lists(const Board& board, int* fb_list, int* fw_list, int& out_n) {
    int n = 0;
    for (int s = 0; s < SQUARE_NB; ++s) {
        const Piece p = board.piece_at(s);
        if (p == NO_PIECE || type_of(p) == KING) continue;
        fb_list[n] = yo_folded_board_feature(BLACK, p, s);
        fw_list[n] = yo_folded_board_feature(WHITE, p, s);
        ++n;
    }
    for (Color c : {BLACK, WHITE}) {
        for (int pt_i = PAWN; pt_i <= ROOK; ++pt_i) {
            const PieceType pt = static_cast<PieceType>(pt_i);
            const int cnt = std::min(board.hand(c, pt), max_hand_count(pt));
            if (cnt <= 0) continue;
            const int b_base = yo_hand_base(BLACK, c, pt);
            const int w_base = yo_hand_base(WHITE, c, pt);
            for (int k = 0; k < cnt; ++k) {
                fb_list[n] = b_base + k;
                fw_list[n] = w_base + k;
                ++n;
            }
        }
    }
    out_n = n;
}

void compute_full_state(const Board& board, NnueStateEntry& dst) {
    const int half_dims = g_model.half_dims;
    const int yo_bk     = to_yo_sq(board.king_sq(BLACK));
    const int yo_wk     = to_yo_sq(board.king_sq(WHITE));
    const int yo_wk_inv = yo_inv(yo_wk);
    const int yo_bk_inv = yo_inv(yo_bk);

    int fb_list[44], fw_list[44], n = 0;
    build_nonking_lists(board, fb_list, fw_list, n);

    simd_copy_i16(dst.acc[BLACK], g_model.ft_biases.ptr, half_dims);
    simd_copy_i16(dst.acc[WHITE], g_model.ft_biases.ptr, half_dims);

    const int16_t* w_tbl = g_model.ft_weights_folded.ptr;
    for (int i = 0; i < n; ++i) {
        simd_add_inplace_i16(dst.acc[BLACK], w_tbl + static_cast<size_t>(fb_list[i]) * half_dims, half_dims);
        simd_add_inplace_i16(dst.acc[WHITE], w_tbl + static_cast<size_t>(fw_list[i]) * half_dims, half_dims);
    }
    // Add pre-folded king features (`1548+sq` + `1629/1710+sq`)
    simd_add_inplace_i16(dst.acc[BLACK], w_tbl + static_cast<size_t>(YO_FOLDED_F_KING + yo_bk) * half_dims, half_dims);
    simd_add_inplace_i16(dst.acc[BLACK], w_tbl + static_cast<size_t>(YO_FOLDED_E_KING + yo_wk) * half_dims, half_dims);
    simd_add_inplace_i16(dst.acc[WHITE], w_tbl + static_cast<size_t>(YO_FOLDED_F_KING + yo_wk_inv) * half_dims, half_dims);
    simd_add_inplace_i16(dst.acc[WHITE], w_tbl + static_cast<size_t>(YO_FOLDED_E_KING + yo_bk_inv) * half_dims, half_dims);

    int64_t sb = 0, sw = 0;
    if (g_model.progress_loaded) {
        const int32_t* row_b = g_model.progress_q16.data() + static_cast<size_t>(yo_bk) * kBonaFeEnd;
        const int32_t* row_w = g_model.progress_q16.data() + static_cast<size_t>(yo_wk_inv) * kBonaFeEnd;
        for (int i = 0; i < n; ++i) {
            sb += row_b[fb_list[i]];
            sw += row_w[fw_list[i]];
        }
    }

    dst.pos_key    = board_piece_key(board);
    dst.epoch      = g_cache_epoch;
    dst.yo_bk      = static_cast<int16_t>(yo_bk);
    dst.yo_wk_inv  = static_cast<int16_t>(yo_wk_inv);
    dst.sum_bk_q16 = sb;
    dst.sum_wk_q16 = sw;
}

// Applies a single `StateInfo` step from `src` into `dst`.
// Returns false only if an overflowing synthetic hand count (> max_hand_count) prevents clean 1-slot diff.
bool apply_incremental_step(const StateInfo& si, const NnueStateEntry& src, NnueStateEntry& dst, uint64_t next_piece_key) {
    const int half_dims = g_model.half_dims;
    if (si.null_move) {
        if (&dst != &src) {
            simd_copy_i16(dst.acc[BLACK], src.acc[BLACK], half_dims);
            simd_copy_i16(dst.acc[WHITE], src.acc[WHITE], half_dims);
        }
        dst.pos_key    = next_piece_key;
        dst.epoch      = g_cache_epoch;
        dst.yo_bk      = src.yo_bk;
        dst.yo_wk_inv  = src.yo_wk_inv;
        dst.sum_bk_q16 = src.sum_bk_q16;
        dst.sum_wk_q16 = src.sum_wk_q16;
        return true;
    }

    const Color m_us     = si.mover;
    const Move  m        = si.move;
    const int16_t* w_tbl = g_model.ft_weights_folded.ptr;

    int rem_b[2], add_b[2], rem_w[2], add_w[2];
    int n_diff = 0;
    bool king_moved_b = false, king_moved_w = false;
    int next_yo_bk     = src.yo_bk;
    int next_yo_wk_inv = src.yo_wk_inv;

    if (is_drop(m)) {
        const PieceType pt   = dropped_pt(m);
        const Square    to   = to_sq(m);
        const int       cnt0 = static_cast<int>(si.hand_cnt_before);
        if (cnt0 <= 0 || cnt0 > max_hand_count(pt)) return false;

        rem_b[0] = yo_hand_base(BLACK, m_us, pt) + (cnt0 - 1);
        rem_w[0] = yo_hand_base(WHITE, m_us, pt) + (cnt0 - 1);
        const Piece placed = make_piece(m_us, pt);
        add_b[0] = yo_folded_board_feature(BLACK, placed, to);
        add_w[0] = yo_folded_board_feature(WHITE, placed, to);
        n_diff = 1;
    } else {
        const Square    from      = from_sq(m);
        const Square    to        = to_sq(m);
        const Piece     after_p   = si.moved_piece;
        const PieceType after_pt  = type_of(after_p);
        const PieceType before_pt = is_promote(m) ? unpromote_pt(after_pt) : after_pt;
        const Piece     before_p  = make_piece(m_us, before_pt);

        rem_b[0] = yo_folded_board_feature(BLACK, before_p, from);
        add_b[0] = yo_folded_board_feature(BLACK, after_p,  to);
        rem_w[0] = yo_folded_board_feature(WHITE, before_p, from);
        add_w[0] = yo_folded_board_feature(WHITE, after_p,  to);
        n_diff = 1;

        if (before_pt == KING) {
            if (m_us == BLACK) {
                king_moved_b = true;
                next_yo_bk   = to_yo_sq(to);
            } else {
                king_moved_w   = true;
                next_yo_wk_inv = yo_inv(to_yo_sq(to));
            }
        }

        if (si.captured != NO_PIECE) {
            if (type_of(si.captured) == KING) return false;
            const PieceType raw_cap = to_hand_pt(type_of(si.captured));
            const int       cnt0    = static_cast<int>(si.hand_cnt_before);
            if (cnt0 < 0 || cnt0 >= max_hand_count(raw_cap)) return false;

            rem_b[1] = yo_folded_board_feature(BLACK, si.captured, to);
            rem_w[1] = yo_folded_board_feature(WHITE, si.captured, to);
            add_b[1] = yo_hand_base(BLACK, m_us, raw_cap) + cnt0;
            add_w[1] = yo_hand_base(WHITE, m_us, raw_cap) + cnt0;
            n_diff = 2;
        }
    }

    if (n_diff == 1) {
        simd_sub1_add1_i16(dst.acc[BLACK], src.acc[BLACK],
                           w_tbl + static_cast<size_t>(rem_b[0]) * half_dims,
                           w_tbl + static_cast<size_t>(add_b[0]) * half_dims, half_dims);
        simd_sub1_add1_i16(dst.acc[WHITE], src.acc[WHITE],
                           w_tbl + static_cast<size_t>(rem_w[0]) * half_dims,
                           w_tbl + static_cast<size_t>(add_w[0]) * half_dims, half_dims);
    } else {
        simd_sub2_add2_i16(dst.acc[BLACK], src.acc[BLACK],
                           w_tbl + static_cast<size_t>(rem_b[0]) * half_dims,
                           w_tbl + static_cast<size_t>(rem_b[1]) * half_dims,
                           w_tbl + static_cast<size_t>(add_b[0]) * half_dims,
                           w_tbl + static_cast<size_t>(add_b[1]) * half_dims, half_dims);
        simd_sub2_add2_i16(dst.acc[WHITE], src.acc[WHITE],
                           w_tbl + static_cast<size_t>(rem_w[0]) * half_dims,
                           w_tbl + static_cast<size_t>(rem_w[1]) * half_dims,
                           w_tbl + static_cast<size_t>(add_w[0]) * half_dims,
                           w_tbl + static_cast<size_t>(add_w[1]) * half_dims, half_dims);
    }

    // Update `progress.bin` Q16 sums incrementally whenever the king didn't move;
    // if one king moved, mark that side's sum with INT64_MIN sentinel so we recompute only the 38 ints at the leaf!
    int64_t sb = src.sum_bk_q16;
    int64_t sw = src.sum_wk_q16;
    if (g_model.progress_loaded) {
        if (king_moved_b || sb == std::numeric_limits<int64_t>::min()) {
            sb = std::numeric_limits<int64_t>::min();
        } else {
            const int32_t* row_b = g_model.progress_q16.data() + static_cast<size_t>(next_yo_bk) * kBonaFeEnd;
            for (int k = 0; k < n_diff; ++k) {
                if (rem_b[k] < kBonaFeEnd) sb -= row_b[rem_b[k]];
                if (add_b[k] < kBonaFeEnd) sb += row_b[add_b[k]];
            }
        }

        if (king_moved_w || sw == std::numeric_limits<int64_t>::min()) {
            sw = std::numeric_limits<int64_t>::min();
        } else {
            const int32_t* row_w = g_model.progress_q16.data() + static_cast<size_t>(next_yo_wk_inv) * kBonaFeEnd;
            for (int k = 0; k < n_diff; ++k) {
                if (rem_w[k] < kBonaFeEnd) sw -= row_w[rem_w[k]];
                if (add_w[k] < kBonaFeEnd) sw += row_w[add_w[k]];
            }
        }
    }

    dst.pos_key    = next_piece_key;
    dst.epoch      = g_cache_epoch;
    dst.yo_bk      = static_cast<int16_t>(next_yo_bk);
    dst.yo_wk_inv  = static_cast<int16_t>(next_yo_wk_inv);
    dst.sum_bk_q16 = sb;
    dst.sum_wk_q16 = sw;
    return true;
}

const NnueStateEntry& get_or_update_nnue_state(const Board& board) {
    ensure_state_cache_allocated();
    const uint64_t cur_pkey = board_piece_key(board);
    NnueStateEntry& cur_slot = g_state_cache[cur_pkey & NNUE_CACHE_MASK];
    if (cur_slot.epoch == g_cache_epoch && cur_slot.pos_key == cur_pkey) {
        return cur_slot;
    }

    // Walk back up to 8 plies in `board.history_` to locate the nearest cached ancestor
    const size_t hist_sz = board.history_.size();
    const size_t max_back = std::min<size_t>(hist_sz, 8);
    uint64_t ancestor_pkeys[9];
    ancestor_pkeys[0] = cur_pkey;

    size_t found_back = 0;
    for (size_t back = 1; back <= max_back; ++back) {
        const StateInfo& si = board.history_[hist_sz - back];
        // `si.hash` was before `si.mover` moved, so `si.mover` was side-to-move at `si.hash`
        const uint64_t anc_pkey = (si.mover == WHITE) ? (si.hash ^ Zobrist::side) : si.hash;
        ancestor_pkeys[back] = anc_pkey;
        const NnueStateEntry& anc_slot = g_state_cache[anc_pkey & NNUE_CACHE_MASK];
        if (anc_slot.epoch == g_cache_epoch && anc_slot.pos_key == anc_pkey) {
            found_back = back;
            break;
        }
    }

    if (found_back > 0) {
        const NnueStateEntry* prev_ptr = &g_state_cache[ancestor_pkeys[found_back] & NNUE_CACHE_MASK];
        NnueStateEntry* tmp_buf = g_tmp_state.ptr;
        bool ok = true;
        for (size_t b = found_back; b >= 1; --b) {
            const StateInfo& step_si = board.history_[hist_sz - b];
            NnueStateEntry& next_buf = (b == 1 && prev_ptr != &cur_slot) ? cur_slot : tmp_buf[b & 1];
            if (!apply_incremental_step(step_si, *prev_ptr, next_buf, ancestor_pkeys[b - 1])) {
                ok = false;
                break;
            }
            prev_ptr = &next_buf;
        }
        if (ok) {
            if (prev_ptr != &cur_slot) {
                simd_copy_i16(cur_slot.acc[BLACK], prev_ptr->acc[BLACK], g_model.half_dims);
                simd_copy_i16(cur_slot.acc[WHITE], prev_ptr->acc[WHITE], g_model.half_dims);
                cur_slot.pos_key    = prev_ptr->pos_key;
                cur_slot.epoch      = prev_ptr->epoch;
                cur_slot.yo_bk      = prev_ptr->yo_bk;
                cur_slot.yo_wk_inv  = prev_ptr->yo_wk_inv;
                cur_slot.sum_bk_q16 = prev_ptr->sum_bk_q16;
                cur_slot.sum_wk_q16 = prev_ptr->sum_wk_q16;
            }
            // Finalize any lazy king-move progress sum on `cur_slot` before returning
            if (g_model.progress_loaded &&
                (cur_slot.sum_bk_q16 == std::numeric_limits<int64_t>::min() ||
                 cur_slot.sum_wk_q16 == std::numeric_limits<int64_t>::min())) {
                int fb_list[44], fw_list[44], n = 0;
                build_nonking_lists(board, fb_list, fw_list, n);
                if (cur_slot.sum_bk_q16 == std::numeric_limits<int64_t>::min()) {
                    const int32_t* row_b = g_model.progress_q16.data() + static_cast<size_t>(cur_slot.yo_bk) * kBonaFeEnd;
                    int64_t sb = 0;
                    for (int i = 0; i < n; ++i) sb += row_b[fb_list[i]];
                    cur_slot.sum_bk_q16 = sb;
                }
                if (cur_slot.sum_wk_q16 == std::numeric_limits<int64_t>::min()) {
                    const int32_t* row_w = g_model.progress_q16.data() + static_cast<size_t>(cur_slot.yo_wk_inv) * kBonaFeEnd;
                    int64_t sw = 0;
                    for (int i = 0; i < n; ++i) sw += row_w[fw_list[i]];
                    cur_slot.sum_wk_q16 = sw;
                }
            }
            return cur_slot;
        }
    }

    compute_full_state(board, cur_slot);
    return cur_slot;
}

int select_layer_stack_bucket(const Board& board, const NnueStateEntry& st) {
    if (g_model.layer_stacks <= 1) return 0;
    if (g_model.progress_loaded) {
        return std::clamp(bucket_from_progress_sum_q16(st.sum_bk_q16 + st.sum_wk_q16),
                          0, g_model.layer_stacks - 1);
    }
    const int b = static_cast<int>(heuristic_progress(board) * g_model.layer_stacks);
    return std::clamp(b, 0, g_model.layer_stacks - 1);
}

int evaluate_nnue_incremental(const Board& board) {
    if (board.king_sq(BLACK) == SQ_NONE || board.king_sq(WHITE) == SQ_NONE) {
        return material_eval(board);
    }

    const uint64_t hkey = board.hash();
    EvalHashEntry& e = g_ehash[hkey & EHASH_MASK];
    if (e.epoch == g_cache_epoch && e.key == hkey) {
        return e.score_stm;
    }

    const NnueStateEntry& st = get_or_update_nnue_state(board);
    const int half_dims = g_model.half_dims;
    const Color stm = board.side_to_move();

    uint8_t* const tf = g_tf_buf.ptr;
    pairwise_transform_perspective(st.acc[stm],  tf,                 half_dims);
    pairwise_transform_perspective(st.acc[~stm], tf + half_dims / 2, half_dims);

    const int bucket = select_layer_stack_bucket(board, st);
    const int32_t raw_out = propagate_sfnn_stack(g_model.stacks[static_cast<size_t>(bucket)], tf, half_dims);
    const int32_t scaled  = std::clamp<int32_t>(raw_out / g_fv_scale, -kMaxEvalScore, kMaxEvalScore);

    e.key       = hkey;
    e.score_stm = scaled;
    e.epoch     = g_cache_epoch;
    return scaled;
}

// Pure un-folded scalar reference (no SIMD, no EvalHash, no incremental cache)
int evaluate_nnue_scalar_reference(const Board& board) {
    if (board.king_sq(BLACK) == SQ_NONE || board.king_sq(WHITE) == SQ_NONE) {
        return material_eval(board);
    }

    const int half_dims = g_model.half_dims;
    const int yo_bk     = to_yo_sq(board.king_sq(BLACK));
    const int yo_wk     = to_yo_sq(board.king_sq(WHITE));
    const int yo_wk_inv = yo_inv(yo_wk);
    const int yo_bk_inv = yo_inv(yo_bk);

    int fb_list[44], fw_list[44], n = 0;
    build_nonking_lists(board, fb_list, fw_list, n);

    std::vector<int16_t> acc_b(g_model.ft_biases.ptr, g_model.ft_biases.ptr + half_dims);
    std::vector<int16_t> acc_w(g_model.ft_biases.ptr, g_model.ft_biases.ptr + half_dims);

    const int16_t* w_tbl = g_model.ft_weights_folded.ptr;
    for (int i = 0; i < n; ++i) {
        const int16_t* rb = w_tbl + static_cast<size_t>(fb_list[i]) * half_dims;
        const int16_t* rw = w_tbl + static_cast<size_t>(fw_list[i]) * half_dims;
        for (int j = 0; j < half_dims; ++j) {
            acc_b[j] = static_cast<int16_t>(acc_b[j] + rb[j]);
            acc_w[j] = static_cast<int16_t>(acc_w[j] + rw[j]);
        }
    }

    // Add all 4 un-folded K-A2 king rows explicitly (`1548+fk`, `1548+ek`, `1629+fk`, `1710+ek`)
    for (int j = 0; j < half_dims; ++j) {
        acc_b[j] = static_cast<int16_t>(acc_b[j]
            + g_model.ft_king_a2[static_cast<size_t>(yo_bk) * half_dims + j]
            + g_model.ft_king_a2[static_cast<size_t>(yo_wk) * half_dims + j]
            + g_model.ft_king_f [static_cast<size_t>(yo_bk) * half_dims + j]
            + g_model.ft_king_e [static_cast<size_t>(yo_wk) * half_dims + j]);
        acc_w[j] = static_cast<int16_t>(acc_w[j]
            + g_model.ft_king_a2[static_cast<size_t>(yo_wk_inv) * half_dims + j]
            + g_model.ft_king_a2[static_cast<size_t>(yo_bk_inv) * half_dims + j]
            + g_model.ft_king_f [static_cast<size_t>(yo_wk_inv) * half_dims + j]
            + g_model.ft_king_e [static_cast<size_t>(yo_bk_inv) * half_dims + j]);
    }

    int bucket = 0;
    if (g_model.layer_stacks > 1) {
        if (g_model.progress_loaded) {
            int64_t sum_q16 = 0;
            const int32_t* row_b = g_model.progress_q16.data() + static_cast<size_t>(yo_bk) * kBonaFeEnd;
            const int32_t* row_w = g_model.progress_q16.data() + static_cast<size_t>(yo_wk_inv) * kBonaFeEnd;
            for (int i = 0; i < n; ++i) {
                sum_q16 += row_b[fb_list[i]];
                sum_q16 += row_w[fw_list[i]];
            }
            bucket = std::clamp(bucket_from_progress_sum_q16(sum_q16), 0, g_model.layer_stacks - 1);
        } else {
            bucket = std::clamp(static_cast<int>(heuristic_progress(board) * g_model.layer_stacks),
                                0, g_model.layer_stacks - 1);
        }
    }

    const auto& first_acc  = (board.side_to_move() == BLACK) ? acc_b : acc_w;
    const auto& second_acc = (board.side_to_move() == BLACK) ? acc_w : acc_b;
    const int half_out = half_dims / 2;
    std::vector<uint8_t> tf(static_cast<size_t>(half_dims));
    for (int j = 0; j < half_out; ++j) {
        const int a0 = std::clamp<int>(first_acc[j], 0, 127);
        const int b0 = std::clamp<int>(first_acc[half_out + j], 0, 127);
        tf[j] = static_cast<uint8_t>((a0 * b0) >> 7);

        const int a1 = std::clamp<int>(second_acc[j], 0, 127);
        const int b1 = std::clamp<int>(second_acc[half_out + j], 0, 127);
        tf[half_out + j] = static_cast<uint8_t>((a1 * b1) >> 7);
    }

    const SfnnLayerStack& st = g_model.stacks[static_cast<size_t>(bucket)];
    uint8_t h0_act[32]{};
    for (int o = 0; o < 8; ++o) {
        int32_t z = st.fc0_b[o];
        const int8_t* w = st.fc0_w.ptr + o * half_dims;
        for (int i = 0; i < half_dims; ++i) z += static_cast<int32_t>(tf[i]) * w[i];
        const int64_t zz = z;
        h0_act[o]     = static_cast<uint8_t>(std::clamp<int64_t>((zz * zz) >> 19, 0, 127));
        h0_act[8 + o] = static_cast<uint8_t>(std::clamp<int32_t>(z >> 6, 0, 127));
    }

    int32_t out = st.fc2_b;
    for (int o = 0; o < 64; ++o) {
        int32_t z = st.fc1_b[o];
        const int8_t* w = st.fc1_w_raw32 + o * 32;
        for (int i = 0; i < 32; ++i) z += static_cast<int32_t>(h0_act[i]) * w[i];
        const int32_t act = std::clamp<int32_t>(z >> 6, 0, 127);
        out += act * st.fc2_w[o];
    }

    return std::clamp<int32_t>(out / g_fv_scale, -kMaxEvalScore, kMaxEvalScore);
}

} // namespace

// ============================================================
// Public API
// ============================================================

void set_eval_dir(const std::string& dir) {
    g_eval_dir           = dir;
    g_explicit_eval_dir  = !dir.empty();
    g_loaded_once        = false;
    ++g_config_epoch;
}

void set_eval_file_path(const std::string& path) {
    g_eval_file_path     = path;
    g_explicit_eval_file = !path.empty();
    if (path.empty()) {
        g_explicit_prog_file = false;
        g_prog_file_path.clear();
    }
    g_loaded_once        = false;
    ++g_config_epoch;
}

void set_progress_file_path(const std::string& path) {
    g_prog_file_path     = path;
    g_explicit_prog_file = !path.empty();
    g_loaded_once        = false;
    ++g_config_epoch;
}

void set_fv_scale(int scale) {
    const int clamped = std::clamp(scale, 1, 256);
    if (clamped != g_fv_scale) {
        g_fv_scale = clamped;
        std::fill(g_ehash.begin(), g_ehash.end(), EvalHashEntry{});
        ++g_config_epoch;
    }
}

int get_fv_scale() {
    return g_fv_scale;
}

void clear_eval_caches() {
    bump_cache_epoch();
}

uint32_t get_eval_config_epoch() {
    return g_config_epoch;
}

std::string eval_status_message() {
    ensure_loaded();
    return g_status;
}

EvalFamily get_eval_family() {
    ensure_loaded();
    return g_family;
}

int evaluate(const Board& board) {
    ensure_loaded();
    if (g_family == EvalFamily::SFNN_NNUE)
        return evaluate_nnue_incremental(board);
    return material_eval(board);
}

int evaluate_full_reference(const Board& board) {
    ensure_loaded();
    if (g_family == EvalFamily::SFNN_NNUE)
        return evaluate_nnue_scalar_reference(board);
    return material_eval(board);
}

double evaluate_progress(const Board& board) {
    ensure_loaded();
    if (g_family == EvalFamily::SFNN_NNUE && g_model.progress_loaded &&
        board.king_sq(BLACK) != SQ_NONE && board.king_sq(WHITE) != SQ_NONE) {
        const NnueStateEntry& st = get_or_update_nnue_state(board);
        const double logit = static_cast<double>(st.sum_bk_q16 + st.sum_wk_q16) / kProgressQ16Scale;
        return 1.0 / (1.0 + std::exp(-logit));
    }
    return heuristic_progress(board);
}

int evaluate_progress_bucket(const Board& board) {
    ensure_loaded();
    if (g_family == EvalFamily::SFNN_NNUE &&
        board.king_sq(BLACK) != SQ_NONE && board.king_sq(WHITE) != SQ_NONE) {
        const NnueStateEntry& st = get_or_update_nnue_state(board);
        return select_layer_stack_bucket(board, st);
    }
    return std::clamp(static_cast<int>(heuristic_progress(board) * 8.0), 0, 7);
}
