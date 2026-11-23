// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "search.hpp"
#include "movegen.hpp"
#include "eval.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

std::atomic<bool>     g_stop{false};
std::atomic<uint64_t> nodelimit{0};

namespace {

constexpr uint64_t TIME_CHECK_MASK      = 0x3FF; // sync counters & check clock every 1024 nodes
constexpr int      MATE_SCORE_THRESHOLD = MATE_VALUE - MAX_DEPTH * 2;
constexpr int      MAX_PLY              = 128;
constexpr int      MAX_HISTORY          = 16'384;
constexpr int16_t  EVAL_NONE            = -32768;

// Move context key: `[Color][PieceType][to_sq]` => 2 * 15 * 81 = 2,430
constexpr int MOVE_CTX_NB      = COLOR_NB * PT_NB * SQUARE_NB;
constexpr int SIDE_MOVE_CTX_NB = PT_NB * SQUARE_NB; // 1,215

enum BoundType : uint8_t {
    BOUND_NONE  = 0,
    BOUND_EXACT = 1,
    BOUND_LOWER = 2,
    BOUND_UPPER = 3,
};

// ============================================================
// Lock-Free 4-Way Clustered Transposition Table (1-Cycle Hyatt/Stockfish XOR)
//   word0 = ((uint64_t(key32) << 32) | uint32_t(score)) ^ word1
//   word1 = uint64_t(move) | (uint64_t(uint16_t(eval16)) << 32)
//         | (uint64_t(uint8_t(depth)) << 48) | (uint64_t(gen_bound) << 56)
// ============================================================
inline uint64_t pack_tt_word1(Move best_move, int16_t eval16, int8_t depth, uint8_t gen_bound) {
    return static_cast<uint64_t>(best_move) |
           (static_cast<uint64_t>(static_cast<uint16_t>(eval16)) << 32) |
           (static_cast<uint64_t>(static_cast<uint8_t>(depth)) << 48) |
           (static_cast<uint64_t>(gen_bound) << 56);
}

struct TTData {
    uint32_t key32     = 0;
    int32_t  score     = 0;
    Move     best_move = MOVE_NONE;
    int16_t  eval16    = EVAL_NONE;
    int8_t   depth     = -1;
    uint8_t  gen_bound = 0;

    uint8_t bound() const { return static_cast<uint8_t>(gen_bound & 0x3u); }
    uint8_t gen()   const { return static_cast<uint8_t>(gen_bound >> 2); }
};

inline TTData unpack_tt_words(uint64_t w0, uint64_t w1) {
    const uint64_t r0 = w0 ^ w1;
    TTData d;
    d.key32     = static_cast<uint32_t>(r0 >> 32);
    d.score     = static_cast<int32_t>(static_cast<uint32_t>(r0));
    d.best_move = static_cast<Move>(static_cast<uint32_t>(w1));
    d.eval16    = static_cast<int16_t>(static_cast<uint16_t>(w1 >> 32));
    d.depth     = static_cast<int8_t>(static_cast<uint8_t>(w1 >> 48));
    d.gen_bound = static_cast<uint8_t>(w1 >> 56);
    return d;
}

struct TTEntry {
    std::atomic<uint64_t> word0{0};
    std::atomic<uint64_t> word1{0};

    void clear() noexcept {
        word0.store(0, std::memory_order_relaxed);
        word1.store(0, std::memory_order_relaxed);
    }
};
static_assert(sizeof(TTEntry) == 16, "TTEntry must be 16 bytes");

struct alignas(16) TTCluster {
    TTEntry entry[4];
    TTCluster() noexcept {
        for (int i = 0; i < 4; ++i) entry[i].clear();
    }
};
static_assert(sizeof(TTCluster) == 64, "TTCluster must fit one 64-byte cache line");

std::unique_ptr<TTCluster[]> g_tt;
size_t   g_tt_clusters = 0;
size_t   g_tt_mask     = 0;
uint8_t  g_tt_gen      = 1; // 6-bit generation counter (1..63)

void clear_tt_clusters(int num_threads) {
    if (!g_tt || g_tt_clusters == 0) return;
    const int threads = std::clamp(num_threads, 1, 16);
    if (threads <= 1 || g_tt_clusters < 4096) {
        for (size_t i = 0; i < g_tt_clusters; ++i) {
            for (int k = 0; k < 4; ++k) g_tt[i].entry[k].clear();
        }
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads - 1));
    const size_t chunk = (g_tt_clusters + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads);
    for (int t = 1; t < threads; ++t) {
        const size_t beg = static_cast<size_t>(t) * chunk;
        const size_t end = std::min(g_tt_clusters, beg + chunk);
        if (beg >= end) break;
        pool.emplace_back([beg, end]() {
            for (size_t i = beg; i < end; ++i) {
                for (int k = 0; k < 4; ++k) g_tt[i].entry[k].clear();
            }
        });
    }
    const size_t end0 = std::min(g_tt_clusters, chunk);
    for (size_t i = 0; i < end0; ++i) {
        for (int k = 0; k < 4; ++k) g_tt[i].entry[k].clear();
    }
    for (auto& th : pool) th.join();
}

void ensure_tt_initialized(size_t mb = 64) {
    if (g_tt) return;
    size_t target_bytes = std::max<size_t>(4, mb) * 1024ULL * 1024ULL;
    size_t clusters = 1;
    while ((clusters << 1) * sizeof(TTCluster) <= target_bytes) {
        clusters <<= 1;
    }
    g_tt = std::make_unique<TTCluster[]>(clusters);
    g_tt_clusters = clusters;
    g_tt_mask     = clusters - 1;
}

inline uint32_t tt_key32(uint64_t key) {
    return static_cast<uint32_t>((key >> 32) ^ ((key * 0x9E3779B97F4A7C15ULL) >> 32));
}

inline int tt_age_diff(uint8_t cur_gen, uint8_t entry_gen) {
    return (static_cast<int>(cur_gen) - static_cast<int>(entry_gen)) & 0x3F;
}

inline bool is_mate_score(int score) {
    return std::abs(score) >= MATE_SCORE_THRESHOLD;
}

inline int tt_store_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD)  return score + ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score - ply;
    return score;
}

inline int tt_probe_score(int score, int ply) {
    if (score >= MATE_SCORE_THRESHOLD)  return score - ply;
    if (score <= -MATE_SCORE_THRESHOLD) return score + ply;
    return score;
}

// ============================================================
// Per-Thread SearchWorker State (Register-Resident Counters, Zero False Sharing)
// ============================================================
struct ScoredMove {
    Move move;
    int  score;
};

struct alignas(16) SearchWorker {
    int thread_id = 0;

    // Fast L1/register-private counters during search (flushed periodically to pub_* atomics)
    uint64_t local_nodes    = 0;
    uint64_t local_qnodes   = 0;
    int      local_seldepth = 0;
    uint64_t node_quota     = 0;

    // Published atomics read by Thread 0 for live USI `info` & Lazy SMP depth coordination
    std::atomic<uint64_t> pub_nodes{0};
    std::atomic<uint64_t> pub_qnodes{0};
    std::atomic<int>      pub_seldepth{0};
    std::atomic<int>      active_depth{0};

    // Detailed search statistics (private to owning thread during search)
    uint64_t beta_cutoffs = 0;
    uint64_t threshold_cutoffs = 0;
    uint64_t tt_probes = 0;
    uint64_t tt_hits = 0;
    uint64_t tt_cutoffs = 0;
    uint64_t null_move_verifications = 0;
    uint64_t late_move_prunes = 0;
    uint64_t aspiration_fail_low = 0;
    uint64_t aspiration_fail_high = 0;
    uint64_t countermove_hits = 0;
    uint64_t repetition_sensitive_tt_skips = 0;
    uint64_t checking_move_selectivity_skips = 0;
    uint64_t continuation_history_updates = 0;
    uint64_t continuation_history_ordering_uses = 0;
    uint64_t singular_tt_candidates = 0;
    uint64_t singular_extension_attempts = 0;
    uint64_t singular_extensions = 0;
    uint64_t exclusion_searches = 0;

    // Completed iteration result for Stockfish-style consensus voting
    int               completed_depth     = 0;
    int               completed_score     = -INF;
    Move              completed_best_move = MOVE_NONE;
    std::vector<Move> completed_pv;

    // Per-ply search stacks
    std::array<std::array<Move, MAX_PLY>, MAX_PLY> pv;
    std::array<int, MAX_PLY>                       pv_len;
    std::array<std::array<Move, 2>, MAX_PLY>       killers;
    std::array<int, MAX_PLY>                       eval_stack;
    std::array<int, MAX_PLY>                       move_ctx_stack;

    // Persistent per-thread move-ordering histories
    int16_t                 main_history[COLOR_NB][SQUARE_NB][SQUARE_NB];
    int16_t                 drop_history[COLOR_NB][PT_NB][SQUARE_NB];
    int16_t                 capture_history[PIECE_NB][SQUARE_NB][PT_NB];
    Move                    countermoves[MOVE_CTX_NB];
    std::vector<int16_t>    cont_hist;    // [MOVE_CTX_NB * SIDE_MOVE_CTX_NB]
    std::vector<ScoredMove> ply_move_buf; // [(MAX_PLY + 4) * MoveList::MAX_MOVES]

    uint8_t cacheline_pad[64];

    explicit SearchWorker(int tid)
        : thread_id(tid),
          cont_hist(static_cast<size_t>(MOVE_CTX_NB) * SIDE_MOVE_CTX_NB, 0),
          ply_move_buf(static_cast<size_t>(MAX_PLY + 4) * MoveList::MAX_MOVES) {
        std::memset(&pv, 0, sizeof(pv));
        pv_len.fill(0);
        for (auto& k : killers) k = {MOVE_NONE, MOVE_NONE};
        eval_stack.fill(-INF);
        move_ctx_stack.fill(-1);
        std::memset(main_history, 0, sizeof(main_history));
        std::memset(drop_history, 0, sizeof(drop_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (auto& cm : countermoves) cm = MOVE_NONE;
        std::memset(cacheline_pad, 0, sizeof(cacheline_pad));
    }

    inline void publish_counters() noexcept {
        pub_nodes.store(local_nodes, std::memory_order_relaxed);
        pub_qnodes.store(local_qnodes, std::memory_order_relaxed);
        pub_seldepth.store(local_seldepth, std::memory_order_relaxed);
    }

    void reset_for_search(bool decay_hist) {
        local_nodes    = 0;
        local_qnodes   = 0;
        local_seldepth = 0;
        node_quota     = 0;
        pub_nodes.store(0, std::memory_order_relaxed);
        pub_qnodes.store(0, std::memory_order_relaxed);
        pub_seldepth.store(0, std::memory_order_relaxed);
        active_depth.store(0, std::memory_order_relaxed);

        beta_cutoffs = 0;
        threshold_cutoffs = 0;
        tt_probes = 0;
        tt_hits = 0;
        tt_cutoffs = 0;
        null_move_verifications = 0;
        late_move_prunes = 0;
        aspiration_fail_low = 0;
        aspiration_fail_high = 0;
        countermove_hits = 0;
        repetition_sensitive_tt_skips = 0;
        checking_move_selectivity_skips = 0;
        continuation_history_updates = 0;
        continuation_history_ordering_uses = 0;
        singular_tt_candidates = 0;
        singular_extension_attempts = 0;
        singular_extensions = 0;
        exclusion_searches = 0;

        completed_depth = 0;
        completed_score = -INF;
        completed_best_move = MOVE_NONE;
        completed_pv.clear();

        pv_len.fill(0);
        eval_stack.fill(-INF);
        move_ctx_stack.fill(-1);
        for (auto& k : killers) k = {MOVE_NONE, MOVE_NONE};

        if (decay_hist) {
            for (int c = 0; c < COLOR_NB; ++c) {
                for (int f = 0; f < SQUARE_NB; ++f)
                    for (int t = 0; t < SQUARE_NB; ++t)
                        main_history[c][f][t] /= 2;
                for (int pt = 0; pt < PT_NB; ++pt)
                    for (int t = 0; t < SQUARE_NB; ++t)
                        drop_history[c][pt][t] /= 2;
            }
        }
    }

    void clear_histories() {
        std::memset(main_history, 0, sizeof(main_history));
        std::memset(drop_history, 0, sizeof(drop_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (auto& cm : countermoves) cm = MOVE_NONE;
        std::fill(cont_hist.begin(), cont_hist.end(), static_cast<int16_t>(0));
    }
};

// ============================================================
// Shared Search Coordination State & Persistent Worker Thread Pool
// ============================================================
std::chrono::steady_clock::time_point g_start;
std::atomic<int>      g_num_threads{1};
int                   g_active_threads = 1;
int                   g_hard_ms        = 0;
bool                  g_allow_old_gen_cutoff = true;
uint64_t              g_active_nodelimit = 0;
std::atomic<uint64_t> g_nodes_remaining{0};
std::atomic<int>      g_highest_completed_depth{0};

std::array<std::unique_ptr<SearchWorker>, MAX_SEARCH_THREADS> g_workers{};
SearchStats g_last_stats{};
int lmr_table[MAX_DEPTH][64];

void run_worker_iterative_deepening(
    SearchWorker& w,
    Board board,
    std::vector<ScoredMove> root_moves,
    TimeBudget budget,
    int target_max_depth,
    int active_threads,
    const std::function<void(const SearchInfo&)>& info_cb);

struct PersistentSmpPool {
    std::mutex              mtx;
    std::condition_variable cv_work;
    std::condition_variable cv_done;
    std::vector<std::thread> threads;

    uint64_t job_epoch      = 0;
    int      active_helpers = 0;
    int      running_cnt    = 0;
    bool     shutting_down  = false;

    const Board*                   job_board      = nullptr;
    const std::vector<ScoredMove>* job_root_moves = nullptr;
    TimeBudget                     job_budget{};
    int                            job_max_depth  = MAX_DEPTH;
    int                            job_threads    = 1;

    ~PersistentSmpPool() {
        shutdown();
    }

    void ensure_size(int helper_count) {
        std::lock_guard<std::mutex> lk(mtx);
        if (shutting_down) {
            shutting_down = false;
        }
        while (static_cast<int>(threads.size()) < helper_count) {
            const int tid = static_cast<int>(threads.size()) + 1;
            threads.emplace_back([this, tid]() { worker_loop(tid); });
        }
    }

    void dispatch_helpers(int total_active_threads,
                          const Board& root_board,
                          const std::vector<ScoredMove>& root_moves,
                          TimeBudget budget,
                          int target_max_depth) {
        const int helpers = std::max(0, total_active_threads - 1);
        if (helpers <= 0) return;
        ensure_size(helpers);

        std::lock_guard<std::mutex> lk(mtx);
        job_board      = &root_board;
        job_root_moves = &root_moves;
        job_budget     = budget;
        job_max_depth  = target_max_depth;
        job_threads    = total_active_threads;
        active_helpers = helpers;
        running_cnt    = helpers;
        ++job_epoch;
        cv_work.notify_all();
    }

    void wait_helpers() {
        std::unique_lock<std::mutex> lk(mtx);
        cv_done.wait(lk, [this]() { return running_cnt == 0; });
    }

    void shutdown() {
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (threads.empty()) return;
            shutting_down = true;
            g_stop.store(true, std::memory_order_relaxed);
            cv_work.notify_all();
            to_join.swap(threads);
        }
        for (auto& th : to_join) {
            if (th.joinable()) th.join();
        }
    }

private:
    void worker_loop(int tid) {
        uint64_t seen_epoch = 0;
        while (true) {
            const Board*                   b_ptr  = nullptr;
            const std::vector<ScoredMove>* rm_ptr = nullptr;
            TimeBudget                     bud{};
            int                            max_d  = MAX_DEPTH;
            int                            act_th = 1;
            bool                           participate = false;

            {
                std::unique_lock<std::mutex> lk(mtx);
                cv_work.wait(lk, [&]() { return shutting_down || job_epoch != seen_epoch; });
                if (shutting_down) return;
                seen_epoch  = job_epoch;
                participate = (tid <= active_helpers);
                if (participate) {
                    b_ptr  = job_board;
                    rm_ptr = job_root_moves;
                    bud    = job_budget;
                    max_d  = job_max_depth;
                    act_th = job_threads;
                }
            }

            if (participate && b_ptr && rm_ptr) {
                SearchWorker& w = *g_workers[static_cast<size_t>(tid)];
                run_worker_iterative_deepening(w, *b_ptr, *rm_ptr, bud, max_d, act_th, {});
                w.publish_counters();

                std::lock_guard<std::mutex> lk(mtx);
                if (--running_cnt == 0) {
                    cv_done.notify_all();
                }
            }
        }
    }
};

PersistentSmpPool g_smp_pool;

void init_lmr_table() {
    for (int d = 0; d < MAX_DEPTH; ++d) {
        for (int m = 0; m < 64; ++m) {
            if (d < 2 || m < 1)
                lmr_table[d][m] = 0;
            else
                lmr_table[d][m] = static_cast<int>(0.70 + std::log(d) * std::log(m + 1) / 2.15);
        }
    }
}

void ensure_workers_allocated(int count) {
    const int clamped = std::clamp(count, 1, MAX_SEARCH_THREADS);
    for (int i = 0; i < clamped; ++i) {
        if (!g_workers[static_cast<size_t>(i)]) {
            g_workers[static_cast<size_t>(i)] = std::make_unique<SearchWorker>(i);
        }
    }
}

inline int elapsed_ms() {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_start).count());
}

inline uint64_t total_live_nodes(int active_threads) {
    uint64_t sum = 0;
    for (int i = 0; i < active_threads; ++i) {
        const SearchWorker& w = *g_workers[static_cast<size_t>(i)];
        if (i == 0) {
            sum += w.local_nodes + w.local_qnodes;
        } else {
            sum += w.pub_nodes.load(std::memory_order_relaxed) + w.pub_qnodes.load(std::memory_order_relaxed);
        }
    }
    return sum;
}

inline int max_live_seldepth(int active_threads) {
    int sd = g_workers[0]->local_seldepth;
    for (int i = 1; i < active_threads; ++i) {
        sd = std::max(sd, g_workers[static_cast<size_t>(i)]->pub_seldepth.load(std::memory_order_relaxed));
    }
    return sd;
}

void collect_search_stats(int active_threads) {
    g_last_stats = SearchStats{};
    for (int i = 0; i < active_threads; ++i) {
        const SearchWorker& w = *g_workers[static_cast<size_t>(i)];
        g_last_stats.nodes                              += w.local_nodes;
        g_last_stats.qnodes                             += w.local_qnodes;
        g_last_stats.beta_cutoffs                       += w.beta_cutoffs;
        g_last_stats.threshold_cutoffs                  += w.threshold_cutoffs;
        g_last_stats.tt_probes                          += w.tt_probes;
        g_last_stats.tt_hits                            += w.tt_hits;
        g_last_stats.tt_cutoffs                         += w.tt_cutoffs;
        g_last_stats.null_move_verifications            += w.null_move_verifications;
        g_last_stats.late_move_prunes                   += w.late_move_prunes;
        g_last_stats.aspiration_fail_low                += w.aspiration_fail_low;
        g_last_stats.aspiration_fail_high               += w.aspiration_fail_high;
        g_last_stats.countermove_hits                   += w.countermove_hits;
        g_last_stats.repetition_sensitive_tt_skips      += w.repetition_sensitive_tt_skips;
        g_last_stats.checking_move_selectivity_skips    += w.checking_move_selectivity_skips;
        g_last_stats.continuation_history_updates       += w.continuation_history_updates;
        g_last_stats.continuation_history_ordering_uses += w.continuation_history_ordering_uses;
        g_last_stats.singular_tt_candidates             += w.singular_tt_candidates;
        g_last_stats.singular_extension_attempts        += w.singular_extension_attempts;
        g_last_stats.singular_extensions                += w.singular_extensions;
        g_last_stats.exclusion_searches                 += w.exclusion_searches;
    }
}

// Consumes 1 node token before visiting a search/quiescence node.
// Uses register-resident `local_nodes` / `local_qnodes` in normal mode (zero atomic traffic!),
// and strict token-bucket CAS when `g_active_nodelimit > 0`.
inline bool try_consume_node(SearchWorker& w, bool is_qnode) {
    if (g_stop.load(std::memory_order_relaxed)) return false;

    if (g_active_nodelimit > 0) {
        if (w.node_quota == 0) {
            uint64_t rem = g_nodes_remaining.load(std::memory_order_relaxed);
            while (true) {
                if (rem == 0) {
                    g_stop.store(true, std::memory_order_relaxed);
                    return false;
                }
                const uint64_t batch_cap = (g_active_threads <= 1)
                    ? 64ULL
                    : ((rem > static_cast<uint64_t>(g_active_threads) * 256ULL) ? 32ULL : 1ULL);
                const uint64_t take = std::min<uint64_t>(rem, batch_cap);
                if (g_nodes_remaining.compare_exchange_weak(
                        rem, rem - take,
                        std::memory_order_relaxed, std::memory_order_relaxed)) {
                    w.node_quota = take;
                    break;
                }
            }
        }
        --w.node_quota;
    }

    if (is_qnode) ++w.local_qnodes;
    else          ++w.local_nodes;

    const uint64_t tot = w.local_nodes + w.local_qnodes;
    if ((tot & TIME_CHECK_MASK) == 0) {
        if (w.thread_id != 0) {
            w.publish_counters();
        } else if (time_up(g_hard_ms)) {
            g_stop.store(true, std::memory_order_relaxed);
            return false;
        }
    }
    return true;
}

inline void tt_prefetch(uint64_t key) noexcept {
    __builtin_prefetch(&g_tt[key & g_tt_mask], 0, 1);
}

bool tt_probe(SearchWorker& w, uint64_t key, TTData& out) {
    ++w.tt_probes;
    TTCluster& cluster = g_tt[key & g_tt_mask];
    const uint32_t k32 = tt_key32(key);
    for (int i = 0; i < 4; ++i) {
        const uint64_t w0 = cluster.entry[i].word0.load(std::memory_order_relaxed);
        const uint64_t w1 = cluster.entry[i].word1.load(std::memory_order_relaxed);
        if ((w0 | w1) == 0) continue;
        const TTData d = unpack_tt_words(w0, w1);
        if (d.bound() != BOUND_NONE && d.key32 == k32 &&
            std::abs(d.score) < MATE_VALUE + MAX_PLY) {
            ++w.tt_hits;
            out = d;
            return true;
        }
    }
    return false;
}

void tt_store(uint64_t key, int depth, int score, BoundType bound, Move best_move, int ply, int static_eval, bool is_main = true) {
    TTCluster& cluster = g_tt[key & g_tt_mask];
    const uint32_t k32 = tt_key32(key);
    const int8_t clamped_depth = static_cast<int8_t>(std::clamp(depth, -1, 125));
    int16_t ev16 = (std::abs(static_eval) <= 30000) ? static_cast<int16_t>(static_eval) : EVAL_NONE;

    int replace_idx = -1;
    TTData existing{};
    for (int i = 0; i < 4; ++i) {
        const uint64_t w0 = cluster.entry[i].word0.load(std::memory_order_relaxed);
        const uint64_t w1 = cluster.entry[i].word1.load(std::memory_order_relaxed);
        const TTData d = unpack_tt_words(w0, w1);
        if (d.bound() == BOUND_NONE || d.key32 == k32) {
            replace_idx = i;
            existing = d;
            break;
        }
    }

    if (replace_idx < 0) {
        replace_idx = 0;
        int worst_val = INT_MAX;
        for (int i = 0; i < 4; ++i) {
            const uint64_t w0 = cluster.entry[i].word0.load(std::memory_order_relaxed);
            const uint64_t w1 = cluster.entry[i].word1.load(std::memory_order_relaxed);
            const TTData d = unpack_tt_words(w0, w1);
            const int val = d.depth - 4 * tt_age_diff(g_tt_gen, d.gen());
            if (val < worst_val) {
                worst_val = val;
                replace_idx = i;
                existing = d;
            }
        }
    }

    const bool same_key = (existing.bound() != BOUND_NONE && existing.key32 == k32);
    if (same_key) {
        if (best_move == MOVE_NONE) best_move = existing.best_move;
        // CRITICAL Lazy SMP guard: NEVER let a shallow lagging helper thread (even with BOUND_EXACT)
        // clobber a much deeper current-generation TT entry!
        const bool reject_overwrite = is_main
            ? (bound != BOUND_EXACT && existing.gen() == g_tt_gen && existing.depth > clamped_depth + 2)
            : (existing.gen() == g_tt_gen && existing.depth > clamped_depth + (bound == BOUND_EXACT ? 0 : 2));
        if (reject_overwrite) {
            if (ev16 != EVAL_NONE && existing.eval16 == EVAL_NONE) {
                const uint64_t upd_w1 = pack_tt_word1(existing.best_move, ev16, existing.depth, existing.gen_bound);
                const uint64_t upd_r0 = (static_cast<uint64_t>(existing.key32) << 32) |
                                        static_cast<uint32_t>(existing.score);
                cluster.entry[replace_idx].word0.store(upd_r0 ^ upd_w1, std::memory_order_relaxed);
                cluster.entry[replace_idx].word1.store(upd_w1, std::memory_order_relaxed);
            }
            return;
        }
        if (ev16 == EVAL_NONE) ev16 = existing.eval16;
    }

    const uint8_t gen_bound = static_cast<uint8_t>((g_tt_gen << 2) | bound);
    const int32_t st_score  = tt_store_score(score, ply);
    const uint64_t raw1     = pack_tt_word1(best_move, ev16, clamped_depth, gen_bound);
    const uint64_t raw0     = (static_cast<uint64_t>(k32) << 32) | static_cast<uint32_t>(st_score);
    cluster.entry[replace_idx].word0.store(raw0 ^ raw1, std::memory_order_relaxed);
    cluster.entry[replace_idx].word1.store(raw1, std::memory_order_relaxed);
}

void next_tt_generation() {
    g_tt_gen = static_cast<uint8_t>((g_tt_gen + 1) & 0x3Fu);
    if (g_tt_gen == 0) g_tt_gen = 1;
}

int prepare_search_session(int hard_ms, int configured_threads, bool decay_hist, uint64_t go_nodes_limit) {
    static bool lmr_initialized = false;
    static uint32_t last_eval_cfg_epoch = 0;
    if (!lmr_initialized) {
        init_lmr_table();
        lmr_initialized = true;
    }
    ensure_tt_initialized();

    const uint64_t opt_limit = nodelimit.load(std::memory_order_relaxed);
    uint64_t eff_limit = 0;
    if (opt_limit > 0 && go_nodes_limit > 0) eff_limit = std::min(opt_limit, go_nodes_limit);
    else if (opt_limit > 0)                  eff_limit = opt_limit;
    else                                     eff_limit = go_nodes_limit;

    // Smart Lazy SMP thread scaling when `nodelimit` / `go nodes` is active:
    // Splitting e.g. 50,000 nodes across 16 threads gives each thread only 3,125 nodes (losing 3+ plies!).
    // Ensure every active thread gets at least ~150,000 nodes so `nodelimit` NEVER loses depth!
    const unsigned hw_cores = std::thread::hardware_concurrency();
    const int max_hw = (hw_cores > 0) ? static_cast<int>(hw_cores) : MAX_SEARCH_THREADS;
    int active_threads = std::clamp(std::min(configured_threads, max_hw), 1, MAX_SEARCH_THREADS);
    if (eff_limit > 0 && active_threads > 1) {
        const uint64_t max_useful = std::max<uint64_t>(1ULL, eff_limit / 150000ULL);
        if (max_useful < static_cast<uint64_t>(active_threads)) {
            active_threads = static_cast<int>(max_useful);
        }
    }

    ensure_workers_allocated(active_threads);
    prepare_eval_threads(active_threads);

    const uint32_t cur_eval_cfg_epoch = get_eval_config_epoch();
    if (last_eval_cfg_epoch != cur_eval_cfg_epoch) {
        if (last_eval_cfg_epoch != 0) {
            clear_tt_clusters(active_threads);
            g_tt_gen = 1;
        }
        last_eval_cfg_epoch = cur_eval_cfg_epoch;
    }

    search_init_time();
    g_stop.store(false, std::memory_order_relaxed);
    g_active_threads   = active_threads;
    g_hard_ms          = hard_ms;
    g_active_nodelimit = eff_limit;
    g_highest_completed_depth.store(0, std::memory_order_relaxed);

    const uint64_t token_budget = (eff_limit > 1) ? (eff_limit - 1) : eff_limit;
    g_nodes_remaining.store(token_budget, std::memory_order_relaxed);

    for (int i = 0; i < active_threads; ++i) {
        g_workers[static_cast<size_t>(i)]->reset_for_search(decay_hist);
    }
    next_tt_generation();
    return active_threads;
}

// ============================================================
// Move Classification & Per-Worker History Helpers
// ============================================================
inline bool is_tactical_move(const Board& board, Move m) {
    if (is_promote(m)) return true;
    return !is_drop(m) && board.piece_at(to_sq(m)) != NO_PIECE;
}

inline PieceType moved_after_pt(const Board& board, Move m) {
    if (is_drop(m)) return dropped_pt(m);
    PieceType pt = type_of(board.piece_at(from_sq(m)));
    return is_promote(m) ? promote_pt(pt) : pt;
}

inline int move_ctx_key(const Board& board, Color us, Move m) {
    if (m == MOVE_NONE) return -1;
    const PieceType pt = moved_after_pt(board, m);
    const Square    to = to_sq(m);
    if (pt <= NO_PT || pt >= PT_NB || to < 0 || to >= SQUARE_NB) return -1;
    return (static_cast<int>(us) * PT_NB + static_cast<int>(pt)) * SQUARE_NB + to;
}

inline int side_move_ctx_key(const Board& board, Move m) {
    if (m == MOVE_NONE) return -1;
    const PieceType pt = moved_after_pt(board, m);
    const Square    to = to_sq(m);
    if (pt <= NO_PT || pt >= PT_NB || to < 0 || to >= SQUARE_NB) return -1;
    return static_cast<int>(pt) * SQUARE_NB + to;
}

inline int quiet_history_score(const SearchWorker& w, Color us, Move m) {
    if (is_drop(m)) return w.drop_history[us][dropped_pt(m)][to_sq(m)];
    return w.main_history[us][from_sq(m)][to_sq(m)];
}

inline int cont_history_lookup(const SearchWorker& w, int prev_ctx, int side_ctx) {
    if (prev_ctx < 0 || side_ctx < 0) return 0;
    return w.cont_hist[static_cast<size_t>(prev_ctx) * SIDE_MOVE_CTX_NB + side_ctx];
}

inline void gravity_update(int16_t& entry, int bonus) {
    int v = entry;
    v += bonus - (v * std::abs(bonus)) / MAX_HISTORY;
    entry = static_cast<int16_t>(std::clamp(v, -MAX_HISTORY, MAX_HISTORY));
}

inline void record_killer(SearchWorker& w, int ply, Move m) {
    if (ply < 0 || ply >= MAX_PLY) return;
    if (w.killers[ply][0] == m) return;
    w.killers[ply][1] = w.killers[ply][0];
    w.killers[ply][0] = m;
}

void update_quiet_histories(SearchWorker& w, const Board& board, Color us, Move m, int depth, bool good,
                            int prev1_ctx, int prev2_ctx) {
    int bonus = std::min(2048, depth * depth * 16 + depth * 32);
    if (!good) bonus = -bonus * 3 / 4;

    if (is_drop(m)) {
        gravity_update(w.drop_history[us][dropped_pt(m)][to_sq(m)], bonus);
    } else {
        gravity_update(w.main_history[us][from_sq(m)][to_sq(m)], bonus);
    }

    const int sctx = side_move_ctx_key(board, m);
    if (sctx >= 0) {
        if (prev1_ctx >= 0) {
            gravity_update(w.cont_hist[static_cast<size_t>(prev1_ctx) * SIDE_MOVE_CTX_NB + sctx], bonus);
            ++w.continuation_history_updates;
        }
        if (prev2_ctx >= 0) {
            gravity_update(w.cont_hist[static_cast<size_t>(prev2_ctx) * SIDE_MOVE_CTX_NB + sctx], bonus * 3 / 4);
            ++w.continuation_history_updates;
        }
    }
}

void update_capture_history(SearchWorker& w, const Board& board, Move m, int depth, bool good) {
    if (is_drop(m)) return;
    Piece attacker = board.piece_at(from_sq(m));
    if (attacker == NO_PIECE) return;
    Piece victim = board.piece_at(to_sq(m));
    PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;
    int bonus = std::min(2048, depth * depth * 12 + depth * 24);
    if (!good) bonus = -bonus / 2;
    gravity_update(w.capture_history[attacker][to_sq(m)][vic_pt], bonus);
}

inline int score_tactical_move(const SearchWorker& w, const Board& board, Move m) {
    const Square from = from_sq(m);
    const Square to   = to_sq(m);
    const Piece  attacker = board.piece_at(from);
    const Piece  victim   = board.piece_at(to);
    const PieceType att_pt = type_of(attacker);
    const PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;

    int score = capture_swing_value(vic_pt) * 16 - PIECE_VALUE_BONANZA[att_pt];
    if (is_promote(m)) score += promote_gain_value(att_pt) * 16;
    score += w.capture_history[attacker][to][vic_pt] / 4;
    return score;
}

inline int score_quiet_move(SearchWorker& w, const Board& board, Color us, Move m, int prev1_ctx, int prev2_ctx) {
    const int sctx  = side_move_ctx_key(board, m);
    const int cont1 = cont_history_lookup(w, prev1_ctx, sctx);
    const int cont2 = cont_history_lookup(w, prev2_ctx, sctx);
    if (cont1 != 0 || cont2 != 0) ++w.continuation_history_ordering_uses;
    return quiet_history_score(w, us, m) * 2 + cont1 * 2 + cont2;
}

// ============================================================
// Staged MovePicker (YaneuraOu-style: zero heap allocations!)
// ============================================================
class MovePicker {
public:
    enum Stage {
        ST_MAIN_TT = 0,
        ST_GEN_TACTICAL,
        ST_GOOD_TACTICAL,
        ST_KILLER_1,
        ST_KILLER_2,
        ST_COUNTERMOVE,
        ST_GEN_QUIET,
        ST_QUIET,
        ST_BAD_TACTICAL,
        ST_DONE,

        ST_EVASION_TT,
        ST_GEN_EVASION,
        ST_EVASION
    };

    MovePicker(SearchWorker& w, Board& board, Move tt_move, int ply, bool in_check, int prev1_ctx, int prev2_ctx)
        : w_(w),
          board_(board),
          us_(board.side_to_move()),
          tt_move_(tt_move),
          ply_(ply),
          in_check_(in_check),
          prev1_ctx_(prev1_ctx),
          prev2_ctx_(prev2_ctx),
          scored_(w.ply_move_buf.data() + static_cast<size_t>(std::clamp(ply, 0, MAX_PLY + 3)) * MoveList::MAX_MOVES) {
        if (in_check_) {
            stage_ = ST_EVASION_TT;
        } else {
            stage_ = ST_MAIN_TT;
            if (ply_ >= 0 && ply_ < MAX_PLY) {
                killer1_ = w_.killers[ply_][0];
                killer2_ = w_.killers[ply_][1];
            }
            if (prev1_ctx_ >= 0 && prev1_ctx_ < MOVE_CTX_NB) {
                countermove_ = w_.countermoves[prev1_ctx_];
            }
        }
    }

    Move next_move(bool& out_is_bad_tactical) {
        out_is_bad_tactical = false;
        while (true) {
            switch (stage_) {
                case ST_MAIN_TT: {
                    stage_ = ST_GEN_TACTICAL;
                    if (tt_move_ != MOVE_NONE && is_move_legal(board_, tt_move_, false)) {
                        return tt_move_;
                    }
                    tt_move_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_TACTICAL: {
                    MoveList tlist;
                    generate_tactical_moves(board_, tlist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    bad_end_ = 0;
                    for (int i = 0; i < tlist.size(); ++i) {
                        Move m = tlist[i];
                        if (m == tt_move_) continue;
                        scored_[end_idx_++] = {m, score_tactical_move(w_, board_, m)};
                    }
                    stage_ = ST_GOOD_TACTICAL;
                    break;
                }
                case ST_GOOD_TACTICAL: {
                    while (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        Piece attacker = board_.piece_at(from_sq(m));
                        Piece victim   = board_.piece_at(to_sq(m));
                        PieceType vic_pt = (victim != NO_PIECE) ? type_of(victim) : NO_PT;
                        int chist = w_.capture_history[attacker][to_sq(m)][vic_pt];
                        if (see_ge(board_, m, -chist / 32)) {
                            return m;
                        }
                        if (bad_end_ < 128) bad_tactical_[bad_end_++] = m;
                    }
                    stage_ = ST_KILLER_1;
                    break;
                }
                case ST_KILLER_1: {
                    stage_ = ST_KILLER_2;
                    if (killer1_ != MOVE_NONE && killer1_ != tt_move_ &&
                        !is_tactical_move(board_, killer1_) &&
                        is_move_legal(board_, killer1_, false)) {
                        return killer1_;
                    }
                    killer1_ = MOVE_NONE;
                    break;
                }
                case ST_KILLER_2: {
                    stage_ = ST_COUNTERMOVE;
                    if (killer2_ != MOVE_NONE && killer2_ != tt_move_ && killer2_ != killer1_ &&
                        !is_tactical_move(board_, killer2_) &&
                        is_move_legal(board_, killer2_, false)) {
                        return killer2_;
                    }
                    killer2_ = MOVE_NONE;
                    break;
                }
                case ST_COUNTERMOVE: {
                    stage_ = ST_GEN_QUIET;
                    if (countermove_ != MOVE_NONE && countermove_ != tt_move_ &&
                        countermove_ != killer1_ && countermove_ != killer2_ &&
                        !is_tactical_move(board_, countermove_) &&
                        is_move_legal(board_, countermove_, false)) {
                        ++w_.countermove_hits;
                        return countermove_;
                    }
                    countermove_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_QUIET: {
                    MoveList qlist;
                    generate_quiet_moves(board_, qlist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    for (int i = 0; i < qlist.size(); ++i) {
                        Move m = qlist[i];
                        if (m == tt_move_ || m == killer1_ || m == killer2_ || m == countermove_)
                            continue;
                        scored_[end_idx_++] = {m, score_quiet_move(w_, board_, us_, m, prev1_ctx_, prev2_ctx_)};
                    }
                    stage_ = ST_QUIET;
                    break;
                }
                case ST_QUIET: {
                    if (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        return m;
                    }
                    cur_idx_ = 0;
                    stage_ = ST_BAD_TACTICAL;
                    break;
                }
                case ST_BAD_TACTICAL: {
                    if (cur_idx_ < bad_end_) {
                        out_is_bad_tactical = true;
                        return bad_tactical_[cur_idx_++];
                    }
                    stage_ = ST_DONE;
                    return MOVE_NONE;
                }

                // ---- In-check evasion stages ----
                case ST_EVASION_TT: {
                    stage_ = ST_GEN_EVASION;
                    if (tt_move_ != MOVE_NONE && is_move_legal(board_, tt_move_, true)) {
                        return tt_move_;
                    }
                    tt_move_ = MOVE_NONE;
                    break;
                }
                case ST_GEN_EVASION: {
                    MoveList elist;
                    generate_legal_moves(board_, elist);
                    cur_idx_ = 0;
                    end_idx_ = 0;
                    for (int i = 0; i < elist.size(); ++i) {
                        Move m = elist[i];
                        if (m == tt_move_) continue;
                        int sc;
                        if (is_tactical_move(board_, m)) {
                            sc = 10'000'000 + score_tactical_move(w_, board_, m);
                        } else {
                            sc = score_quiet_move(w_, board_, us_, m, prev1_ctx_, prev2_ctx_);
                        }
                        scored_[end_idx_++] = {m, sc};
                    }
                    stage_ = ST_EVASION;
                    break;
                }
                case ST_EVASION: {
                    if (cur_idx_ < end_idx_) {
                        Move m = pick_best(scored_, cur_idx_, end_idx_).move;
                        ++cur_idx_;
                        return m;
                    }
                    stage_ = ST_DONE;
                    return MOVE_NONE;
                }
                case ST_DONE:
                default:
                    return MOVE_NONE;
            }
        }
    }

    Move validated_tt_move() const { return tt_move_; }

private:
    static ScoredMove pick_best(ScoredMove* arr, int start, int end) {
        int best_i = start;
        int best_s = arr[start].score;
        for (int i = start + 1; i < end; ++i) {
            if (arr[i].score > best_s) {
                best_s = arr[i].score;
                best_i = i;
            }
        }
        if (best_i != start) std::swap(arr[start], arr[best_i]);
        return arr[start];
    }

    SearchWorker& w_;
    Board& board_;
    Color  us_;
    Move   tt_move_     = MOVE_NONE;
    Move   killer1_     = MOVE_NONE;
    Move   killer2_     = MOVE_NONE;
    Move   countermove_ = MOVE_NONE;
    int    ply_         = 0;
    bool   in_check_    = false;
    int    prev1_ctx_   = -1;
    int    prev2_ctx_   = -1;
    Stage  stage_       = ST_DONE;

    ScoredMove* scored_;
    Move        bad_tactical_[128];
    int         cur_idx_ = 0;
    int         end_idx_ = 0;
    int         bad_end_ = 0;
};

int quiescence(SearchWorker& w, Board& board, int alpha, int beta, int ply, int qply);

// ============================================================
// Main Negamax Alpha-Beta Search (PVS + YaneuraOu Selectivity)
// ============================================================
int search(SearchWorker& w, Board& board, int depth, int alpha, int beta, int ply,
           bool no_null = false, Move excluded_move = MOVE_NONE, bool cut_node = false) {
    if (depth <= 0 || ply >= MAX_PLY - 2)
        return quiescence(w, board, alpha, beta, ply, 0);

    if (!try_consume_node(w, /*is_qnode=*/false)) return 0;

    if (ply > w.local_seldepth) w.local_seldepth = ply;
    if (ply < MAX_PLY) w.pv_len[ply] = ply;

    // 1. Repetition & perpetual-check detection
    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS)  return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    // 2. Mate distance pruning (guarded for synthetic test windows outside mate range)
    if (ply > 0 && alpha < MATE_SCORE_THRESHOLD && beta > -MATE_SCORE_THRESHOLD) {
        alpha = std::max(alpha, -(MATE_VALUE - ply));
        beta  = std::min(beta,   (MATE_VALUE - ply - 1));
        if (alpha >= beta) return alpha;
    }

    const int   original_alpha = alpha;
    const int   original_beta  = beta;
    const bool  in_check       = board.in_check();
    const bool  is_pv          = (beta > alpha + 1);
    const Color us             = board.side_to_move();

    Move hash_move = MOVE_NONE;
    int  tt_eval   = EVAL_NONE;
    bool try_singular_extension = false;
    int  singular_tt_score = 0;

    // 3. Lock-Free Transposition Table Probe
    if (excluded_move == MOVE_NONE) {
        TTData entry;
        if (tt_probe(w, board.hash(), entry)) {
            hash_move = entry.best_move;
            tt_eval   = entry.eval16;
            const uint8_t bnd = entry.bound();
            const bool singular_bound = (bnd == BOUND_LOWER || bnd == BOUND_EXACT);

            if (!repetition_sensitive && ply > 0 && !is_pv && !in_check && depth >= 6 &&
                entry.depth >= depth - 3 && singular_bound && hash_move != MOVE_NONE) {
                ++w.singular_tt_candidates;
                singular_tt_score = tt_probe_score(entry.score, ply);
                try_singular_extension = !is_mate_score(singular_tt_score) &&
                    singular_tt_score >= beta - 2 * depth;
            }

            if ((g_allow_old_gen_cutoff || entry.gen() == g_tt_gen) && entry.depth >= depth && !try_singular_extension) {
                if (repetition_sensitive) {
                    ++w.repetition_sensitive_tt_skips;
                } else if (!is_pv || bnd == BOUND_EXACT) {
                    const int tt_score = tt_probe_score(entry.score, ply);
                    if (bnd == BOUND_EXACT) {
                        ++w.tt_cutoffs;
                        return tt_score;
                    }
                    if (!is_pv) {
                        if (bnd == BOUND_LOWER && tt_score >= beta) {
                            ++w.tt_cutoffs;
                            return tt_score;
                        }
                        if (bnd == BOUND_UPPER && tt_score <= alpha) {
                            ++w.tt_cutoffs;
                            return tt_score;
                        }
                    }
                }
            }
        }
    }

    // 4. Fast 1-ply mate detection (`mate1ply`) when not in check at shallow/frontier nodes
    if (excluded_move == MOVE_NONE && !in_check && depth <= 2 &&
        alpha < MATE_VALUE - (ply + 1)) {
        Move mate_m = MOVE_NONE;
        if (find_mate_1ply(board, &mate_m)) {
            const int mscore = MATE_VALUE - (ply + 1);
            if (ply < MAX_PLY) {
                w.pv[ply][ply] = mate_m;
                w.pv_len[ply]  = ply + 1;
            }
            tt_store(board.hash(), depth, mscore, BOUND_EXACT, mate_m, ply, EVAL_NONE, w.thread_id == 0);
            return mscore;
        }
    }

    // 5. Static evaluation & `improving` heuristic
    int static_eval = -INF;
    bool improving  = false;
    if (!in_check) {
        static_eval = (tt_eval != EVAL_NONE) ? tt_eval : evaluate(board, w.thread_id);
        if (ply < MAX_PLY) {
            w.eval_stack[ply] = static_eval;
            improving = (ply >= 2 && w.eval_stack[ply - 2] != -INF &&
                         static_eval > w.eval_stack[ply - 2]);
        }
    } else if (ply < MAX_PLY) {
        w.eval_stack[ply] = -INF;
    }

    // 6. Internal Iterative Reductions (IIR)
    if (excluded_move == MOVE_NONE && !in_check && depth >= 4 && hash_move == MOVE_NONE) {
        depth -= 1;
    }

    // 7. Selective Pre-Move Pruning
    if (ply > 0 && excluded_move == MOVE_NONE && !repetition_sensitive &&
        !is_pv && !in_check && std::abs(beta) < MATE_SCORE_THRESHOLD) {

        // 7a. Reverse Futility Pruning
        if (depth <= 6) {
            const int rfp_margin = (improving ? 80 : 110) * depth;
            if (static_eval - rfp_margin >= beta) {
                ++w.threshold_cutoffs;
                return (static_eval + beta) / 2;
            }
        }

        // 7b. Razoring
        if (depth <= 2 && static_eval + 300 * depth <= alpha) {
            const int qscore = quiescence(w, board, alpha, alpha + 1, ply, 0);
            if (g_stop.load(std::memory_order_relaxed)) return 0;
            if (qscore <= alpha) {
                ++w.threshold_cutoffs;
                return qscore;
            }
        }

        // 7c. Null Move Pruning
        if (!try_singular_extension && !no_null && depth >= 3 && static_eval >= beta) {
            const bool has_major = board.has_non_pawn_material(us);

            if (has_major) {
                const int R = 3 + depth / 4 + std::min(3, (static_eval - beta) / 200);
                if (ply < MAX_PLY) w.move_ctx_stack[ply] = -1;
                board.do_null_move();
                const int null_score = -search(w, board, depth - 1 - R, -beta, -beta + 1,
                                               ply + 1, true, MOVE_NONE, !cut_node);
                board.undo_null_move();

                if (g_stop.load(std::memory_order_relaxed)) return 0;

                if (null_score >= beta && !is_mate_score(null_score)) {
                    if (depth < 8) {
                        ++w.threshold_cutoffs;
                        return null_score;
                    }
                    ++w.null_move_verifications;
                    const int verify_score = search(w, board, depth - R, beta - 1, beta,
                                                    ply, true, MOVE_NONE, false);
                    if (g_stop.load(std::memory_order_relaxed)) return 0;
                    if (verify_score >= beta && !is_mate_score(verify_score)) {
                        ++w.threshold_cutoffs;
                        tt_store(board.hash(), depth, verify_score, BOUND_LOWER,
                                 MOVE_NONE, ply, static_eval, w.thread_id == 0);
                        return verify_score;
                    }
                }
            }
        }
    }

    // 8. Singular Extension Exclusion Search
    int singular_extension = 0;
    if (try_singular_extension && hash_move != MOVE_NONE &&
        is_move_legal(board, hash_move, in_check)) {
        ++w.singular_extension_attempts;
        const int singular_beta = singular_tt_score - 2 * depth;
        ++w.exclusion_searches;
        const int alternative_score = search(
            w, board, std::max(1, (depth - 1) / 2), singular_beta - 1, singular_beta,
            ply, true, hash_move, cut_node);
        if (g_stop.load(std::memory_order_relaxed)) return 0;
        if (alternative_score < singular_beta) {
            singular_extension = 1;
            ++w.singular_extensions;
        } else if (singular_beta >= beta && !is_mate_score(alternative_score)) {
            ++w.threshold_cutoffs;
            return singular_beta;
        }
    }

    // 9. Staged Move Loop
    const int prev1_ctx = (ply >= 1 && ply - 1 < MAX_PLY) ? w.move_ctx_stack[ply - 1] : -1;
    const int prev2_ctx = (ply >= 2 && ply - 2 < MAX_PLY) ? w.move_ctx_stack[ply - 2] : -1;
    if (ply + 1 < MAX_PLY) {
        w.killers[ply + 1] = {MOVE_NONE, MOVE_NONE};
    }

    const bool use_futility   = (excluded_move == MOVE_NONE && !repetition_sensitive &&
                                 !is_pv && !in_check && depth <= 4);
    const int  futility_limit = use_futility ? (static_eval + 100 + 135 * depth) : INF;
    const int  lmp_threshold  = improving ? (4 + depth * depth) : (3 + depth * depth * 3 / 4);

    MovePicker picker(w, board, hash_move, ply, in_check, prev1_ctx, prev2_ctx);
    Move best_move  = MOVE_NONE;
    int  best_score = -INF;
    int  searched_count = 0;
    int  searched_quiet_count = 0;
    int  legal_seen = 0;

    Move searched_quiets[96];
    int  n_searched_quiets = 0;
    Move searched_tacticals[64];
    int  n_searched_tacticals = 0;

    bool is_bad_tactical = false;
    for (Move m = picker.next_move(is_bad_tactical); m != MOVE_NONE;
         m = picker.next_move(is_bad_tactical)) {
        ++legal_seen;
        if (m == excluded_move) continue;

        const bool is_tactical = is_tactical_move(board, m);
        const bool gives_check = move_gives_check(board, m);
        const int  m_ctx       = move_ctx_key(board, us, m);

        if (searched_count >= 1 && excluded_move == MOVE_NONE && !repetition_sensitive &&
            !is_pv && !in_check && best_score > -MATE_SCORE_THRESHOLD) {

            // 9a. Late Move Pruning (LMP)
            if (!is_tactical && depth <= 4 && searched_quiet_count >= lmp_threshold &&
                quiet_history_score(w, us, m) <= 1024) {
                if (gives_check) {
                    ++w.checking_move_selectivity_skips;
                } else {
                    ++w.threshold_cutoffs;
                    ++w.late_move_prunes;
                    continue;
                }
            }

            // 9b. Futility Pruning
            if (!is_tactical && use_futility && futility_limit < alpha) {
                if (gives_check) {
                    ++w.checking_move_selectivity_skips;
                } else {
                    ++w.threshold_cutoffs;
                    continue;
                }
            }

            // 9c. Shallow SEE Pruning
            if (depth <= 4 && !gives_check && alpha < MATE_SCORE_THRESHOLD) {
                const int see_margin = is_tactical ? (-95 * depth * depth) : (-60 * depth);
                if (!see_ge(board, m, see_margin)) {
                    ++w.threshold_cutoffs;
                    continue;
                }
            }
        }

        // 9d. Extensions (Check Extension + Singular Extension)
        int extension = 0;
        if (m == picker.validated_tt_move() && singular_extension > 0) {
            extension = 1;
        } else if (gives_check && ply < MAX_DEPTH &&
                   (in_check || see_ge(board, m, 0))) {
            extension = 1;
        }

        if (ply < MAX_PLY) w.move_ctx_stack[ply] = m_ctx;
        board.do_move(m);

        int score;
        const int new_depth = depth - 1 + extension;

        if (searched_count == 0) {
            score = -search(w, board, new_depth, -beta, -alpha, ply + 1, false, MOVE_NONE, false);
        } else {
            int lmr_depth = new_depth;
            bool did_lmr  = false;
            if (excluded_move == MOVE_NONE && !repetition_sensitive && !in_check &&
                depth >= 3 && searched_count >= 2 && m != picker.validated_tt_move()) {

                if (!is_tactical || is_bad_tactical) {
                    int r = lmr_table[std::min(depth, MAX_DEPTH - 1)][std::min(searched_count, 63)];
                    const bool is_killer = (ply < MAX_PLY) &&
                        (m == w.killers[ply][0] || m == w.killers[ply][1]);
                    const bool is_cm = (prev1_ctx >= 0 && w.countermoves[prev1_ctx] == m);

                    if (!improving) r += 1;
                    if (cut_node)   r += 1;
                    if (is_pv)      r -= 1;
                    if (is_killer || is_cm) r -= 1;

                    if (!is_tactical) {
                        const int sctx = (m_ctx >= 0) ? (m_ctx % SIDE_MOVE_CTX_NB) : -1;
                        const int hist_sum = quiet_history_score(w, us, m)
                            + cont_history_lookup(w, prev1_ctx, sctx)
                            + cont_history_lookup(w, prev2_ctx, sctx) / 2;
                        r -= hist_sum / 6144;
                    }

                    const int adj_r = std::clamp(r, 0, new_depth - 1);
                    if (adj_r > 0 && !(is_killer || is_cm)) {
                        if (gives_check) {
                            ++w.checking_move_selectivity_skips;
                        } else {
                            lmr_depth = std::max(1, new_depth - adj_r);
                            did_lmr   = true;
                        }
                    }
                }
            }

            score = -search(w, board, lmr_depth, -alpha - 1, -alpha, ply + 1,
                            false, MOVE_NONE, true);

            if (!g_stop.load(std::memory_order_relaxed) && did_lmr && score > alpha) {
                score = -search(w, board, new_depth, -alpha - 1, -alpha, ply + 1,
                                false, MOVE_NONE, !cut_node);
            }

            if (!g_stop.load(std::memory_order_relaxed) && is_pv && score > alpha && score < beta) {
                score = -search(w, board, new_depth, -beta, -alpha, ply + 1,
                                false, MOVE_NONE, false);
            }
        }

        board.undo_move(m);
        ++searched_count;
        if (!is_tactical) {
            ++searched_quiet_count;
            if (n_searched_quiets < 96) searched_quiets[n_searched_quiets++] = m;
        } else {
            if (n_searched_tacticals < 64) searched_tacticals[n_searched_tacticals++] = m;
        }

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            if (ply < MAX_PLY) {
                w.pv[ply][ply] = m;
                if (ply + 1 < MAX_PLY) {
                    const int child_len = std::min<int>(w.pv_len[ply + 1], MAX_PLY);
                    for (int i = ply + 1; i < child_len; ++i) w.pv[ply][i] = w.pv[ply + 1][i];
                    w.pv_len[ply] = std::max(ply + 1, child_len);
                } else {
                    w.pv_len[ply] = ply + 1;
                }
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
            if (alpha >= beta) {
                ++w.beta_cutoffs;
                if (!is_tactical) {
                    record_killer(w, ply, m);
                    if (prev1_ctx >= 0) w.countermoves[prev1_ctx] = m;
                    for (int i = 0; i < n_searched_quiets; ++i) {
                        if (searched_quiets[i] != m)
                            update_quiet_histories(w, board, us, searched_quiets[i], depth, false, prev1_ctx, prev2_ctx);
                    }
                    update_quiet_histories(w, board, us, m, depth, true, prev1_ctx, prev2_ctx);
                } else {
                    for (int i = 0; i < n_searched_tacticals; ++i) {
                        if (searched_tacticals[i] != m)
                            update_capture_history(w, board, searched_tacticals[i], depth, false);
                    }
                    update_capture_history(w, board, m, depth, true);
                }
                break;
            }
        }
    }

    if (legal_seen == 0) {
        return in_check ? -(MATE_VALUE - ply) : 0;
    }
    if (excluded_move != MOVE_NONE && searched_count == 0) {
        return -INF;
    }
    if (g_stop.load(std::memory_order_relaxed)) {
        return (best_move == MOVE_NONE) ? 0 : best_score;
    }

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha)     bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    if (excluded_move == MOVE_NONE) {
        tt_store(board.hash(), depth, best_score, bound, best_move, ply, static_eval, w.thread_id == 0);
    }
    return best_score;
}

// ============================================================
// Quiescence Search (Tactical-only MoveGen + SEE + qply==0 Checks + Mate1Ply)
// ============================================================
int quiescence(SearchWorker& w, Board& board, int alpha, int beta, int ply, int qply) {
    if (!try_consume_node(w, /*is_qnode=*/true)) return 0;

    if (ply > w.local_seldepth) w.local_seldepth = ply;
    if (ply < MAX_PLY) w.pv_len[ply] = ply;

    bool repetition_sensitive = false;
    const RepetitionResult repetition = board.repetition_result(&repetition_sensitive);
    if (repetition == RepetitionResult::DRAW) return 0;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_WINS)  return MATE_VALUE - ply;
    if (repetition == RepetitionResult::SIDE_TO_MOVE_LOSES) return -(MATE_VALUE - ply);

    if (ply >= MAX_PLY - 2) return evaluate(board, w.thread_id);

    const bool  in_check       = board.in_check();
    const int   original_alpha = alpha;
    const int   original_beta  = beta;
    const bool  is_pv          = (beta > alpha + 1);
    const Color us             = board.side_to_move();

    Move hash_move = MOVE_NONE;
    int  tt_eval   = EVAL_NONE;
    const int req_tt_depth = (!in_check && qply == 0) ? 0 : -1;

    TTData entry;
    if (tt_probe(w, board.hash(), entry)) {
        hash_move = entry.best_move;
        tt_eval   = entry.eval16;
        const uint8_t bnd = entry.bound();
        if ((g_allow_old_gen_cutoff || entry.gen() == g_tt_gen) && entry.depth >= req_tt_depth) {
            if (repetition_sensitive) {
                ++w.repetition_sensitive_tt_skips;
            } else if (!is_pv || bnd == BOUND_EXACT) {
                const int tt_score = tt_probe_score(entry.score, ply);
                if (bnd == BOUND_EXACT) {
                    ++w.tt_cutoffs;
                    return tt_score;
                }
                if (!is_pv) {
                    if (bnd == BOUND_LOWER && tt_score >= beta) {
                        ++w.tt_cutoffs;
                        return tt_score;
                    }
                    if (bnd == BOUND_UPPER && tt_score <= alpha) {
                        ++w.tt_cutoffs;
                        return tt_score;
                    }
                }
            }
        }
    }

    int stand_pat = -INF;
    if (!in_check) {
        stand_pat = (tt_eval != EVAL_NONE) ? tt_eval : evaluate(board, w.thread_id);
        if (stand_pat >= beta) {
            tt_store(board.hash(), req_tt_depth, stand_pat, BOUND_LOWER, MOVE_NONE, ply, stand_pat, w.thread_id == 0);
            return stand_pat;
        }
        if (stand_pat > alpha) alpha = stand_pat;

        if (qply == 0 && alpha < MATE_VALUE - (ply + 1)) {
            Move mate_m = MOVE_NONE;
            if (find_mate_1ply(board, &mate_m)) {
                const int mscore = MATE_VALUE - (ply + 1);
                if (ply < MAX_PLY) {
                    w.pv[ply][ply] = mate_m;
                    w.pv_len[ply]  = ply + 1;
                }
                tt_store(board.hash(), 0, mscore, BOUND_EXACT, mate_m, ply, stand_pat, w.thread_id == 0);
                return mscore;
            }
        }
    }

    ScoredMove* candidates = w.ply_move_buf.data() + static_cast<size_t>(std::clamp(ply, 0, MAX_PLY + 3)) * MoveList::MAX_MOVES;
    int n_cand = 0;

    if (in_check) {
        MoveList evasions;
        generate_legal_moves(board, evasions);
        if (evasions.empty()) return -(MATE_VALUE - ply);

        const int prev1_ctx = (ply >= 1 && ply - 1 < MAX_PLY) ? w.move_ctx_stack[ply - 1] : -1;
        const int prev2_ctx = (ply >= 2 && ply - 2 < MAX_PLY) ? w.move_ctx_stack[ply - 2] : -1;
        for (int i = 0; i < evasions.size(); ++i) {
            Move m = evasions[i];
            int sc = (m == hash_move) ? 20'000'000
                   : is_tactical_move(board, m) ? (10'000'000 + score_tactical_move(w, board, m))
                   : score_quiet_move(w, board, us, m, prev1_ctx, prev2_ctx);
            candidates[n_cand++] = {m, sc};
        }
    } else {
        MoveList tlist;
        generate_tactical_moves(board, tlist);
        for (int i = 0; i < tlist.size(); ++i) {
            Move m = tlist[i];
            if (m == hash_move) {
                candidates[n_cand++] = {m, 20'000'000};
                continue;
            }
            const Piece victim = board.piece_at(to_sq(m));
            const PieceType att_pt = type_of(board.piece_at(from_sq(m)));
            if (victim == NO_PIECE && is_promote(m) &&
                att_pt != PAWN && att_pt != BISHOP && att_pt != ROOK &&
                !move_gives_check(board, m)) {
                continue;
            }
            const int max_gain = (victim != NO_PIECE ? capture_swing_value(type_of(victim)) : 0)
                               + (is_promote(m) ? promote_gain_value(att_pt) : 0);
            if (stand_pat + max_gain + 220 < alpha && !move_gives_check(board, m))
                continue;

            if (!see_ge(board, m, 0)) continue;

            candidates[n_cand++] = {m, 10'000'000 + score_tactical_move(w, board, m)};
        }

        if (qply == 0) {
            MoveList qchecks;
            generate_quiet_checks(board, qchecks);
            for (int i = 0; i < qchecks.size(); ++i) {
                Move m = qchecks[i];
                if (m == hash_move) {
                    candidates[n_cand++] = {m, 19'000'000};
                    continue;
                }
                if (!see_ge(board, m, 0)) continue;
                candidates[n_cand++] = {m, 1'000'000 + quiet_history_score(w, us, m)};
            }
        }
    }

    if (n_cand == 0) return stand_pat;

    int  best_score = in_check ? -INF : stand_pat;
    Move best_move  = MOVE_NONE;

    for (int idx = 0; idx < n_cand; ++idx) {
        int best_i = idx;
        for (int j = idx + 1; j < n_cand; ++j) {
            if (candidates[j].score > candidates[best_i].score) best_i = j;
        }
        if (best_i != idx) std::swap(candidates[idx], candidates[best_i]);

        const Move m = candidates[idx].move;
        if (ply < MAX_PLY) w.move_ctx_stack[ply] = move_ctx_key(board, us, m);
        board.do_move(m);
        tt_prefetch(board.hash());
        tt_prefetch(board.hash());
        const int score = -quiescence(w, board, -beta, -alpha, ply + 1, qply + 1);
        board.undo_move(m);

        if (g_stop.load(std::memory_order_relaxed)) break;

        if (score > best_score) {
            best_score = score;
            best_move  = m;
            if (ply < MAX_PLY) {
                w.pv[ply][ply] = m;
                if (ply + 1 < MAX_PLY) {
                    const int child_len = std::min<int>(w.pv_len[ply + 1], MAX_PLY);
                    for (int i = ply + 1; i < child_len; ++i) w.pv[ply][i] = w.pv[ply + 1][i];
                    w.pv_len[ply] = std::max(ply + 1, child_len);
                } else {
                    w.pv_len[ply] = ply + 1;
                }
            }
        }

        if (best_score > alpha) {
            alpha = best_score;
            if (alpha >= beta) {
                ++w.beta_cutoffs;
                break;
            }
        }
    }

    if (g_stop.load(std::memory_order_relaxed))
        return (best_move == MOVE_NONE) ? (in_check ? 0 : stand_pat) : best_score;

    BoundType bound = BOUND_EXACT;
    if (best_score <= original_alpha)     bound = BOUND_UPPER;
    else if (best_score >= original_beta) bound = BOUND_LOWER;
    tt_store(board.hash(), req_tt_depth, best_score, bound, best_move, ply,
             in_check ? EVAL_NONE : stand_pat, w.thread_id == 0);
    return best_score;
}

SearchInfo build_search_info(int depth, int seldepth, int score, const std::vector<Move>& pv, int active_threads) {
    SearchInfo info;
    info.depth    = depth;
    info.seldepth = std::max(depth, seldepth);
    info.time_ms  = std::max(1, elapsed_ms());
    info.nodes    = total_live_nodes(active_threads);
    info.nps      = static_cast<uint64_t>((info.nodes * 1000ULL) / static_cast<uint64_t>(info.time_ms));
    info.score_cp = score;
    if (is_mate_score(score)) {
        info.score_is_mate = true;
        const int plies_to_mate = MATE_VALUE - std::abs(score);
        info.score_mate = (score > 0 ? 1 : -1) * std::max(1, (plies_to_mate + 1) / 2);
    }
    info.pv = pv;
    return info;
}

// ============================================================
// Frontier-Synchronized Lazy SMP Iterative Deepening Loop
// ============================================================
void run_worker_iterative_deepening(
    SearchWorker& w,
    Board board,
    std::vector<ScoredMove> root_moves,
    TimeBudget budget,
    int target_max_depth,
    int active_threads,
    const std::function<void(const SearchInfo&)>& info_cb) {

    const bool  is_main       = (w.thread_id == 0);
    const Color us            = board.side_to_move();
    const bool  root_in_check = board.in_check();

    // Seed this worker's root NNUE accumulator slot so all subtrees chain incrementally from root
    (void)evaluate_progress_bucket(board, w.thread_id);

    // Keep TT root move (`20'000'000`) and good captures (`10'000'000+`) strictly first,
    // while diversifying equal-0 quiet moves across helper threads for wider Lazy SMP coverage.
    if (!is_main && root_moves.size() > 1) {
        for (size_t i = 0; i < root_moves.size(); ++i) {
            if (root_moves[i].score < 10'000'000) {
                root_moves[i].score += static_cast<int>(((w.thread_id * 19u + i * 13u) & 31u)) - 15;
            }
        }
        std::stable_sort(root_moves.begin(), root_moves.end(), [](const ScoredMove& a, const ScoredMove& b) {
            return a.score > b.score;
        });
    }

    Move best_move      = root_moves[0].move;
    int  previous_score = 0;
    int  stable_count   = 0;
    w.completed_best_move = best_move;

    for (int depth = 1; depth <= target_max_depth; ++depth) {
        if (g_stop.load(std::memory_order_relaxed)) break;
        if (is_main && time_up(budget.hard_ms)) {
            g_stop.store(true, std::memory_order_relaxed);
            break;
        }

        // Frontier synchronization & Stockfish-style half-density staggering for helper threads:
        // 1. Never waste CPU searching depths that another thread has ALREADY completed!
        // 2. If >= half of active workers are already at `depth`, odd-parity workers jump to `depth + 1`
        //    so half the workers accelerate the frontier depth `D` via shared TT and half pre-fill `D + 1`!
        // Stockfish-style Lazy SMP skip schedule:
        // Warms up every helper thread's private move-ordering histories while staggering
        // helpers 1-2 plies ahead of Thread 0 (and catching up if >2 plies behind frontier).
        if (!is_main && depth > 1 && depth < target_max_depth) {
            const int global_done = g_highest_completed_depth.load(std::memory_order_relaxed);
            if (depth + 1 <= global_done) {
                continue; // already >=2 plies behind completed frontier: advance cleanly
            }
            static constexpr int skip_size[16]  = {1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4};
            static constexpr int start_parity[16] = {0, 1, 0, 1, 2, 3, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3};
            const int idx_s = (w.thread_id - 1) & 15;
            if (((depth + start_parity[idx_s]) % (skip_size[idx_s] * 2)) >= skip_size[idx_s]) {
                continue;
            }
        }

        // Helper threads only: sync root PV move from deeper shared TT entry without disturbing Thread 0!
        if (!is_main && active_threads > 1 && depth > 1) {
            TTData root_tt;
            if (tt_probe(w, board.hash(), root_tt) && root_tt.best_move != MOVE_NONE &&
                root_tt.depth >= w.completed_depth) {
                for (size_t i = 1; i < root_moves.size(); ++i) {
                    if (root_moves[i].move == root_tt.best_move) {
                        ScoredMove promoted = root_moves[i];
                        for (size_t k = i; k > 0; --k) root_moves[k] = root_moves[k - 1];
                        root_moves[0] = promoted;
                        break;
                    }
                }
                if (w.completed_depth == 0 || root_tt.depth > w.completed_depth) {
                    const int tt_sc = tt_probe_score(root_tt.score, 0);
                    if (!is_mate_score(tt_sc)) {
                        previous_score = tt_sc;
                    }
                }
            }
        }

        w.active_depth.store(depth, std::memory_order_relaxed);

        int delta       = 35 + (!is_main ? (w.thread_id & 7) * 4 : 0);
        int alpha_bound = (depth == 1 || w.completed_depth == 0) ? -INF : std::max(-INF, previous_score - delta);
        int beta_bound  = (depth == 1 || w.completed_depth == 0) ?  INF : std::min( INF, previous_score + delta);

        int  current_score = -INF;
        Move current_best  = MOVE_NONE;

        while (!g_stop.load(std::memory_order_relaxed)) {
            int alpha = alpha_bound;
            const int beta = beta_bound;
            current_score = -INF;
            current_best  = MOVE_NONE;
            w.pv_len[0]   = 0;

            for (size_t idx = 0; idx < root_moves.size(); ++idx) {
                if (g_stop.load(std::memory_order_relaxed)) break;
                if (is_main && time_up(budget.hard_ms)) {
                    g_stop.store(true, std::memory_order_relaxed);
                    break;
                }

                const Move m = root_moves[idx].move;
                const bool gives_chk = move_gives_check(board, m);
                const int ext = (gives_chk && (root_in_check || see_ge(board, m, 0))) ? 1 : 0;
                w.move_ctx_stack[0] = move_ctx_key(board, us, m);
                board.do_move(m);

                int score;
                const int child_depth = depth - 1 + ext;
                if (idx == 0) {
                    score = -search(w, board, child_depth, -beta, -alpha, 1, false, MOVE_NONE, false);
                } else {
                    int r = 0;
                    if (!root_in_check && !gives_chk && depth >= 4 && idx >= 4 &&
                        !is_tactical_move(board, m)) {
                        r = 1 + (idx >= 12 ? 1 : 0);
                    }
                    score = -search(w, board, std::max(0, child_depth - r), -alpha - 1, -alpha,
                                    1, false, MOVE_NONE, true);
                    if (!g_stop.load(std::memory_order_relaxed) && r > 0 && score > alpha) {
                        score = -search(w, board, child_depth, -alpha - 1, -alpha,
                                        1, false, MOVE_NONE, true);
                    }
                    if (!g_stop.load(std::memory_order_relaxed) && score > alpha && score < beta) {
                        score = -search(w, board, child_depth, -beta, -alpha,
                                        1, false, MOVE_NONE, false);
                    }
                }

                board.undo_move(m);
                if (g_stop.load(std::memory_order_relaxed)) break;

                if (score > current_score) {
                    current_score = score;
                    current_best  = m;
                    w.pv[0][0]    = m;
                    const int clen = std::min<int>(w.pv_len[1], MAX_PLY);
                    for (int i = 1; i < clen; ++i) w.pv[0][i] = w.pv[1][i];
                    w.pv_len[0] = std::max(1, clen);

                    if (idx > 0) {
                        ScoredMove promoted = root_moves[idx];
                        for (size_t k = idx; k > 0; --k) root_moves[k] = root_moves[k - 1];
                        root_moves[0] = promoted;
                    }
                }

                if (current_score > alpha) {
                    alpha = current_score;
                    if (alpha >= beta) break;
                }
            }

            if (g_stop.load(std::memory_order_relaxed)) {
                if (w.completed_depth == 0 && current_best != MOVE_NONE) {
                    w.completed_best_move = current_best;
                    w.completed_score     = current_score;
                }
                break;
            }
            if (current_best == MOVE_NONE) break;

            if (current_score <= alpha_bound && alpha_bound > -INF) {
                ++w.aspiration_fail_low;
                beta_bound  = (alpha_bound + beta_bound) / 2;
                delta       = std::min(INF, delta * 2 + 20);
                alpha_bound = (delta >= 4000) ? -INF : std::max(-INF, current_score - delta);
                continue;
            }
            if (current_score >= beta_bound && beta_bound < INF) {
                ++w.aspiration_fail_high;
                delta      = std::min(INF, delta * 2 + 20);
                beta_bound = (delta >= 4000) ? INF : std::min(INF, current_score + delta);
                continue;
            }
            break;
        }

        if (g_stop.load(std::memory_order_relaxed) || current_best == MOVE_NONE) break;

        if (current_best == best_move) ++stable_count;
        else                           stable_count = 0;

        best_move = current_best;
        const int score_drop = previous_score - current_score;
        previous_score = current_score;

        w.completed_depth     = depth;
        w.completed_score     = current_score;
        w.completed_best_move = best_move;
        w.completed_pv.clear();
        for (int i = 0; i < w.pv_len[0] && i < MAX_DEPTH; ++i) w.completed_pv.push_back(w.pv[0][i]);
        if (w.completed_pv.empty()) w.completed_pv.push_back(best_move);
        w.publish_counters();

        // Update global frontier depth atomically and only store root TT if at frontier or main thread
        int prev_global_d = g_highest_completed_depth.load(std::memory_order_relaxed);
        while (depth > prev_global_d &&
               !g_highest_completed_depth.compare_exchange_weak(
                   prev_global_d, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {}

        if (is_main || depth >= prev_global_d) {
            tt_store(board.hash(), depth, current_score, BOUND_EXACT, best_move, 0, EVAL_NONE, is_main);
        }

        if (is_main && info_cb) {
            const SearchInfo info = build_search_info(
                depth, max_live_seldepth(active_threads), current_score, w.completed_pv, active_threads);
            info_cb(info);
        }

        if (is_mate_score(current_score)) {
            if (is_main) g_stop.store(true, std::memory_order_relaxed);
            break;
        }

        if (is_main && budget.soft_ms > 0 && budget.soft_ms < budget.hard_ms && depth >= 5) {
            double factor = 1.0;
            if (stable_count >= 4)      factor *= 0.65;
            else if (stable_count >= 2) factor *= 0.80;
            else if (stable_count == 0) factor *= 1.35;
            if (score_drop >= 40)       factor *= 1.40;
            const int adjusted_soft = std::min(budget.hard_ms,
                                               static_cast<int>(budget.soft_ms * factor));
            if (elapsed_ms() >= adjusted_soft) {
                g_stop.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }

    w.active_depth.store(0, std::memory_order_relaxed);
    w.publish_counters();
    if (is_main) {
        g_stop.store(true, std::memory_order_relaxed);
    }
}

} // namespace

// ============================================================
// Public Search API
// ============================================================

int default_hardware_threads() {
#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len > 0) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
            len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(buf.data(), &len)) {
            int phys_cores = 0;
            const size_t cnt = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            for (size_t i = 0; i < cnt; ++i) {
                if (buf[i].Relationship == RelationProcessorCore) ++phys_cores;
            }
            if (phys_cores > 0) return std::clamp(phys_cores, 1, 16);
        }
    }
#endif
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 4;
    if (hw <= 4) return static_cast<int>(hw);
    return std::clamp(static_cast<int>(hw / 2), 4, 16);
}

void set_num_threads(int threads) {
    const int clamped = std::clamp(threads, 1, MAX_SEARCH_THREADS);
    g_num_threads.store(clamped, std::memory_order_relaxed);
}

int get_num_threads() {
    return g_num_threads.load(std::memory_order_relaxed);
}

void search_shutdown_threads() {
    g_smp_pool.shutdown();
}

void set_nodelimit(uint64_t limit) {
    nodelimit.store(limit, std::memory_order_relaxed);
}

uint64_t get_nodelimit() {
    return nodelimit.load(std::memory_order_relaxed);
}

void set_tt_size_mb(size_t mb) {
    size_t target_bytes = std::max<size_t>(1, mb) * 1024ULL * 1024ULL;
    size_t clusters = 1;
    while ((clusters << 1) * sizeof(TTCluster) <= target_bytes) {
        clusters <<= 1;
    }
    g_tt = std::make_unique<TTCluster[]>(clusters);
    g_tt_clusters = clusters;
    g_tt_mask     = clusters - 1;
    g_tt_gen      = 1;
}

void search_clear_game_state() {
    ensure_tt_initialized();
    const int threads = get_num_threads();
    ensure_workers_allocated(threads);
    clear_tt_clusters(threads);
    g_tt_gen = 1;
    for (int i = 0; i < MAX_SEARCH_THREADS; ++i) {
        if (g_workers[static_cast<size_t>(i)]) {
            g_workers[static_cast<size_t>(i)]->clear_histories();
        }
    }
    clear_eval_caches();
}

void search_init_time() {
    g_start = std::chrono::steady_clock::now();
}

bool time_up(int allotted_ms) {
    if (g_stop.load(std::memory_order_relaxed)) return true;
    if (allotted_ms <= 0) return false;
    return elapsed_ms() >= allotted_ms;
}

int compute_allotted_ms(int my_time_ms, int byoyomi_ms) {
    int v = my_time_ms / 40 + byoyomi_ms / 2;
    if (v < 100) v = 100;
    if (v > 5000) v = 5000;
    return v;
}

TimeBudget compute_time_budget(int my_time_ms, int my_inc_ms, int byoyomi_ms,
                               int game_ply, double game_progress) {
    constexpr int SAFETY_MS = 50;
    if (my_time_ms <= 0 && byoyomi_ms > 0) {
        int hard = std::max(20, std::min(byoyomi_ms - SAFETY_MS, byoyomi_ms * 92 / 100));
        return {hard * 85 / 100, hard};
    }
    if (my_time_ms <= 0 && my_inc_ms <= 0) {
        return {1500, 2000};
    }

    int mtg = std::max(24, 44 - game_ply / 6);
    double phase_boost = 1.0;
    if (game_progress >= 0.0 && game_progress <= 1.0) {
        const int prog_mtg = std::clamp(static_cast<int>(std::round(20.0 + 28.0 * (1.0 - game_progress))), 20, 48);
        mtg = (mtg + prog_mtg) / 2;
        if (game_progress >= 0.20 && game_progress <= 0.75) {
            phase_boost = 1.18;
        }
    }

    const int avail = std::max(20, my_time_ms + byoyomi_ms - SAFETY_MS);
    int soft = static_cast<int>((my_time_ms / mtg + my_inc_ms * 3 / 4 + byoyomi_ms * 2 / 3) * phase_boost);
    int hard = std::min(avail, std::max(soft * 3, my_time_ms / 8 + byoyomi_ms * 9 / 10));
    soft = std::clamp(soft, 20, hard);
    return {soft, hard};
}

int negamax(Board& board, int depth, int alpha, int beta, int ply) {
    ensure_tt_initialized();
    ensure_workers_allocated(1);
    prepare_eval_threads(1);
    if (ply == 0) {
        prepare_search_session(/*hard_ms=*/0, /*configured_threads=*/1, /*decay_hist=*/false, /*go_nodes_limit=*/0);
        g_allow_old_gen_cutoff = false;
    }
    SearchWorker& w0 = *g_workers[0];
    const int res = search(w0, board, depth, alpha, beta, ply);
    w0.publish_counters();
    collect_search_stats(1);
    return res;
}

SearchStats last_search_stats() {
    return g_last_stats;
}

Move iterative_deepening_timed(Board& board, TimeBudget budget,
                               const std::function<void(const SearchInfo&)>& info_cb,
                               const std::function<void()>& started_cb,
                               int max_depth_limit,
                               uint64_t go_nodes_limit) {
    const int configured_threads = get_num_threads();
    const int active_threads = prepare_search_session(
        budget.hard_ms, configured_threads, /*decay_hist=*/true, go_nodes_limit);
    g_allow_old_gen_cutoff = true;
    if (started_cb) started_cb();

    MoveList root_moves_list;
    generate_legal_moves(board, root_moves_list);
    if (root_moves_list.empty()) {
        collect_search_stats(active_threads);
        return MOVE_NONE;
    }

    SearchWorker& w0 = *g_workers[0];
    const Color us = board.side_to_move();

    Move tt_root = MOVE_NONE;
    TTData root_tt_entry;
    if (tt_probe(w0, board.hash(), root_tt_entry) &&
        is_move_legal(board, root_tt_entry.best_move, board.in_check())) {
        tt_root = root_tt_entry.best_move;
    }

    std::vector<ScoredMove> root_moves;
    root_moves.reserve(static_cast<size_t>(root_moves_list.size()));
    for (int i = 0; i < root_moves_list.size(); ++i) {
        Move m = root_moves_list[i];
        int sc = (m == tt_root) ? 20'000'000
               : is_tactical_move(board, m) ? (10'000'000 + score_tactical_move(w0, board, m))
               : quiet_history_score(w0, us, m);
        root_moves.push_back({m, sc});
    }
    std::sort(root_moves.begin(), root_moves.end(), [](const ScoredMove& a, const ScoredMove& b) {
        return a.score > b.score;
    });

    if (root_moves.size() == 1 && budget.soft_ms < budget.hard_ms && budget.hard_ms > 0) {
        budget.soft_ms = std::min(budget.soft_ms, 30);
        budget.hard_ms = std::min(budget.hard_ms, 80);
        g_hard_ms      = budget.hard_ms;
    }

    const int target_max_depth = std::clamp(max_depth_limit, 1, MAX_DEPTH);

    if (active_threads > 1) {
        g_smp_pool.dispatch_helpers(active_threads, board, root_moves, budget, target_max_depth);
    }

    run_worker_iterative_deepening(
        w0, board, root_moves, budget, target_max_depth, active_threads, info_cb);

    g_stop.store(true, std::memory_order_relaxed);
    if (active_threads > 1) {
        g_smp_pool.wait_helpers();
    }

    collect_search_stats(active_threads);

    // ============================================================
    // Stockfish / YaneuraOu Consensus Voting Best-Thread Selection
    //   Eliminates single-thread horizon-effect optimism blunders!
    // ============================================================
    int best_tid = 0;
    int best_mate_score = -INF;
    int max_completed_d = 0;

    for (int t = 0; t < active_threads; ++t) {
        const SearchWorker& cand = *g_workers[static_cast<size_t>(t)];
        if (cand.completed_depth <= 0 || cand.completed_best_move == MOVE_NONE) continue;
        max_completed_d = std::max(max_completed_d, cand.completed_depth);
        if (cand.completed_score >= MATE_SCORE_THRESHOLD && cand.completed_score > best_mate_score) {
            best_mate_score = cand.completed_score;
            best_tid        = t;
        }
    }

    if (best_mate_score < MATE_SCORE_THRESHOLD && active_threads > 1 && max_completed_d > 0) {
        const int depth_floor = std::max(1, max_completed_d - 2);
        int min_score = INF;
        for (int t = 0; t < active_threads; ++t) {
            const SearchWorker& cand = *g_workers[static_cast<size_t>(t)];
            if (cand.completed_depth >= depth_floor && cand.completed_best_move != MOVE_NONE) {
                min_score = std::min(min_score, cand.completed_score);
            }
        }
        if (min_score < INF) {
            std::unordered_map<uint32_t, int64_t> move_votes;
            for (int t = 0; t < active_threads; ++t) {
                const SearchWorker& cand = *g_workers[static_cast<size_t>(t)];
                if (cand.completed_depth < depth_floor || cand.completed_best_move == MOVE_NONE) continue;
                const int64_t weight = static_cast<int64_t>(cand.completed_score - min_score + 14 + (t == 0 ? 6 : 0))
                                     * static_cast<int64_t>(cand.completed_depth);
                move_votes[static_cast<uint32_t>(cand.completed_best_move)] += weight;
            }

            Move winning_move = g_workers[0]->completed_best_move;
            int64_t best_vote = (winning_move != MOVE_NONE)
                ? move_votes[static_cast<uint32_t>(winning_move)] : -1;

            for (const auto& kv : move_votes) {
                if (kv.second > best_vote) {
                    best_vote    = kv.second;
                    winning_move = static_cast<Move>(kv.first);
                }
            }

            // Pick the deepest / highest-scoring worker among those backing `winning_move`
            int chosen_t = -1;
            for (int t = 0; t < active_threads; ++t) {
                const SearchWorker& cand = *g_workers[static_cast<size_t>(t)];
                if (cand.completed_depth <= 0 || cand.completed_best_move != winning_move) continue;
                if (chosen_t < 0) {
                    chosen_t = t;
                } else {
                    const SearchWorker& prev = *g_workers[static_cast<size_t>(chosen_t)];
                    if (cand.completed_depth > prev.completed_depth ||
                        (cand.completed_depth == prev.completed_depth && cand.completed_score > prev.completed_score)) {
                        chosen_t = t;
                    }
                }
            }
            if (chosen_t >= 0) best_tid = chosen_t;
        }
    }

    const SearchWorker& chosen = *g_workers[static_cast<size_t>(best_tid)];
    if (best_tid != 0 && info_cb && chosen.completed_depth > 0 && !chosen.completed_pv.empty()) {
        info_cb(build_search_info(
            chosen.completed_depth, max_live_seldepth(active_threads),
            chosen.completed_score, chosen.completed_pv, active_threads));
    }

    return (chosen.completed_best_move != MOVE_NONE) ? chosen.completed_best_move : root_moves[0].move;
}

Move iterative_deepening(Board& board, int allotted_ms,
                         const std::function<void(const SearchInfo&)>& info_cb,
                         const std::function<void()>& started_cb,
                         int max_depth_limit) {
    return iterative_deepening_timed(board, TimeBudget{allotted_ms, allotted_ms},
                                     info_cb, started_cb, max_depth_limit, 0);
}
