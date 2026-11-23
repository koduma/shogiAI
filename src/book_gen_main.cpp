// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "board.hpp"
#include "movegen.hpp"
#include "usi_proc.hpp"
#include "winrate_book.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct GenConfig {
#if defined(_WIN32)
    std::string engine_path = "shogi_engine.exe";
#else
    std::string engine_path = "./shogi_engine";
#endif
    std::string book_path             = "winrate_book.db";
    std::string eval_dir              = "";
    std::string eval_file             = "";
    int         beam_width            = 8;
    int         max_ply               = 200;
    int         num_runs              = 1;      // number of consecutive 200-ply Beam Search runs
    int         eval_threshold_cp     = 600;
    int         deep_eval_ms          = 150;    // deep search time per beam node (0 = use deep_eval_nodes)
    uint64_t    deep_eval_nodes       = 0;      // optional fixed node budget for deep node eval
    int         selfplay_games        = 100;    // 100 self-play games per [-600, +600] node
    int         game_limit_ms         = 10000;  // <= 10 seconds wall-clock limit per self-play game
    uint64_t    rollout_nodes         = 1200;   // per-move nodes in self-play rollout (fast + strong NNUE)
    int         max_rollout_plies     = 220;    // max additional plies per self-play game before adjudication
    uint64_t    branch_eval_nodes     = 2500;   // search nodes to evaluate each candidate child for beam pruning
    int         max_children_per_node = 0;      // 0 = evaluate ALL legal moves (recommended for multi-run beam search)
    int         workers               = 0;      // 0 -> hardware_concurrency()
    int         threads_per_worker    = 1;      // Threads per worker process
    int         hash_mb_per_worker    = 32;     // USI_Hash / Hash MB per worker process (MB)
    bool        resume                = true;   // load existing winrate_book.db so unregistered nodes are prioritized!
};

struct BeamNode {
    std::vector<Move> moves_from_start;
    uint64_t          hash            = 0;
    int               black_eval      = 0;    // Black-POV evaluation (先手視点評価値)
    bool              is_unregistered = true; // true iff NOT yet in `winrate_book.db` at selection time
};

std::string build_position_cmd(const std::vector<Move>& history) {
    std::ostringstream oss;
    oss << "position startpos";
    if (!history.empty()) {
        oss << " moves";
        for (Move m : history) oss << ' ' << move_to_usi(m);
    }
    return oss.str();
}

Board replay_from_startpos(const std::vector<Move>& history) {
    Board b;
    b.set_startpos();
    for (Move m : history) b.do_move(m);
    return b;
}

// Runs a single self-play game from `start_history` using `eng` (`shogi_engine.exe` or Suisho 5 / YaneuraOu),
// strictly bounded by `cfg.game_limit_ms` (10 seconds wall-clock).
// Returns Black's game points: `1.0` = Black win, `0.5` = draw, `0.0` = White win.
double run_single_selfplay_game(ExternalUsiEngine& eng,
                                const std::vector<Move>& start_history,
                                const GenConfig& cfg,
                                uint64_t seed) {
    const auto game_t0 = std::chrono::steady_clock::now();
    std::mt19937_64 rng(seed);

    eng.send_usinewgame();
    std::vector<Move> hist = start_history;
    Board b = replay_from_startpos(hist);

    int last_black_cp = 0;

    for (int step = 0; step < cfg.max_rollout_plies; ++step) {
        // 1. Check terminal Shogi rules (repetition / perpetual check / checkmate)
        const RepetitionResult rep = b.repetition_result();
        if (rep == RepetitionResult::DRAW) return 0.5;
        if (rep == RepetitionResult::SIDE_TO_MOVE_WINS) {
            return (b.side_to_move() == BLACK) ? 1.0 : 0.0;
        }
        if (rep == RepetitionResult::SIDE_TO_MOVE_LOSES) {
            return (b.side_to_move() == BLACK) ? 0.0 : 1.0;
        }

        MoveList legal;
        generate_legal_moves(b, legal);
        if (legal.empty()) {
            return (b.side_to_move() == BLACK) ? 0.0 : 1.0;
        }

        // 2. Enforce strict <= `game_limit_ms` (10 seconds) wall-clock per game!
        const int elapsed_game_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - game_t0).count());
        const int remaining_ms = cfg.game_limit_ms - elapsed_game_ms;
        if (remaining_ms <= 25) {
            if (last_black_cp >= cfg.eval_threshold_cp)  return 1.0;
            if (last_black_cp <= -cfg.eval_threshold_cp) return 0.0;
            const double p = 1.0 / (1.0 + std::exp(-static_cast<double>(last_black_cp) / 600.0));
            return (p >= 0.65) ? 1.0 : (p <= 0.35 ? 0.0 : 0.5);
        }

        Move chosen = MOVE_NONE;
        const Color stm = b.side_to_move();

        // Diversify the opening 2 plies of each self-play game via temperature-weighted root evaluation
        // (always including the engine's true top move so tactical forced replies are never missed!)
        if (step < 2 && legal.size() > 1 && remaining_ms > 300) {
            const uint64_t probe_nodes = std::max<uint64_t>(150, cfg.rollout_nodes / 4);
            std::ostringstream root_go;
            root_go << "go nodes " << probe_nodes;
            const int probe_timeout = std::max(30, std::min(remaining_ms / 4, 500));
            const UsiEvalResult root_r = eng.run_search(build_position_cmd(hist), root_go.str(), {}, probe_timeout);

            if (root_r.is_declare_win) {
                return (stm == BLACK) ? 1.0 : 0.0;
            }
            if (!root_r.ok || root_r.is_resign) {
                return (stm == BLACK) ? 0.0 : 1.0;
            }

            std::vector<Move> cands;
            if (root_r.best_move != MOVE_NONE && is_move_legal(b, root_r.best_move, b.in_check())) {
                cands.push_back(root_r.best_move);
            }

            std::vector<Move> others;
            others.reserve(static_cast<size_t>(legal.size()));
            for (int i = 0; i < legal.size(); ++i) {
                if (legal[i] != root_r.best_move) others.push_back(legal[i]);
            }
            std::shuffle(others.begin(), others.end(), rng);
            for (Move om : others) {
                if (cands.size() >= 5) break;
                cands.push_back(om);
            }

            const size_t sample_k = cands.size();
            std::vector<double> scores(sample_k, 0.0);
            double best_s = -1e18;
            for (size_t k = 0; k < sample_k; ++k) {
                const int now_elapsed = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - game_t0).count());
                const int rem_k = cfg.game_limit_ms - now_elapsed;
                if (rem_k <= 40 && k > 0) {
                    scores[k] = best_s - 1000.0;
                    continue;
                }
                hist.push_back(cands[k]);
                const UsiEvalResult r = eng.run_search(
                    build_position_cmd(hist), root_go.str(), {}, std::max(25, std::min(rem_k - 10, 400)));
                hist.pop_back();

                double mover_cp = 0.0;
                if (r.is_declare_win)      mover_cp = -30000.0;
                else if (r.is_resign)      mover_cp = +30000.0;
                else if (r.ok)             mover_cp = static_cast<double>(-r.score_stm_cp);
                scores[k] = mover_cp;
                if (mover_cp > best_s) best_s = mover_cp;
            }

            std::vector<double> weights(sample_k, 0.0);
            for (size_t k = 0; k < sample_k; ++k) {
                weights[k] = std::exp(std::max(-20.0, (scores[k] - best_s) / 35.0));
            }
            std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
            const size_t pick = dist(rng);
            chosen = cands[pick];
            last_black_cp = (stm == BLACK) ? static_cast<int>(scores[pick])
                                           : -static_cast<int>(scores[pick]);
        } else {
            uint64_t move_nodes = cfg.rollout_nodes;
            if (remaining_ms < 800) {
                move_nodes = std::min<uint64_t>(move_nodes, 250);
            } else {
                const uint64_t jitter = (cfg.rollout_nodes / 3) + 1;
                move_nodes = (cfg.rollout_nodes * 5 / 6) + (rng() % jitter);
            }
            std::ostringstream go_oss;
            go_oss << "go nodes " << move_nodes;
            const int step_timeout_ms = std::max(20, std::min(remaining_ms - 5, 600));

            const UsiEvalResult r = eng.run_search(build_position_cmd(hist), go_oss.str(), {}, step_timeout_ms);
            if (r.is_declare_win) {
                return (stm == BLACK) ? 1.0 : 0.0;
            }
            if (!r.ok || r.is_resign || r.best_move == MOVE_NONE || !is_move_legal(b, r.best_move, b.in_check())) {
                return (stm == BLACK) ? 0.0 : 1.0;
            }
            chosen = r.best_move;
            last_black_cp = (stm == BLACK) ? r.score_stm_cp : -r.score_stm_cp;

            if (step >= 8 && std::abs(last_black_cp) >= 2800) {
                return (last_black_cp > 0) ? 1.0 : 0.0;
            }
        }

        hist.push_back(chosen);
        b.do_move(chosen);
    }

    if (last_black_cp >= cfg.eval_threshold_cp)  return 1.0;
    if (last_black_cp <= -cfg.eval_threshold_cp) return 0.0;
    return 0.5;
}

void print_usage(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  --engine <path>         Path to USI Shogi AI executable (default: shogi_engine.exe; Suisho 5 / YaneuraOu directly supported)\n"
        << "  --book <path>           Output winrate book DB file (default: winrate_book.db)\n"
        << "  --runs <R>              Number of consecutive 200-ply Beam Search runs (default: 1)\n"
        << "  --eval-dir <dir>        Optional EvalDir override passed to child engine\n"
        << "  --eval-file <path>      Optional EvalFile override passed to child engine\n"
        << "  --beam-width <N>        Beam width per ply (default: 8)\n"
        << "  --max-ply <N>           Maximum plies from Hirate startpos (default: 200)\n"
        << "  --threshold <cp>        Prune threshold in Black-POV cp -> 1.0 / 0.0 (default: 600)\n"
        << "  --eval-ms <ms>          Deep evaluation movetime per node in ms (default: 150)\n"
        << "  --eval-nodes <N>        Deep evaluation node cap per node (0 = use --eval-ms)\n"
        << "  --games <N>             Self-play games per [-600, +600] node (default: 100)\n"
        << "  --game-limit-ms <ms>    Max wall-clock ms per self-play game (default: 10000 = 10s)\n"
        << "  --rollout-nodes <N>     Per-move node cap inside self-play games (default: 1200)\n"
        << "  --branch-nodes <N>      Node cap for scoring branched child candidates (default: 2500)\n"
        << "  --max-children <K>      Max pre-screened children per node to score (default: 0 = all legal moves)\n"
        << "  --workers <W>           Parallel child engine processes (default: CPU cores)\n"
        << "  --threads <T>           Threads per worker process (default: 1)\n"
        << "  --hash-mb <MB>          USI_Hash / Hash MB per worker process (default: 32)\n"
        << "  --no-resume             Ignore existing DB file and start from an empty DB\n";
}

} // namespace

int main(int argc, char** argv) {
    Zobrist::init();
    GenConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next_str = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : "";
        };
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--engine")                              cfg.engine_path   = next_str();
        else if (arg == "--book" || arg == "--out")                cfg.book_path     = next_str();
        else if (arg == "--runs")                                  cfg.num_runs      = std::max(1, std::stoi(next_str()));
        else if (arg == "--eval-dir")                              cfg.eval_dir      = next_str();
        else if (arg == "--eval-file")                             cfg.eval_file     = next_str();
        else if (arg == "--beam-width")                            cfg.beam_width    = std::max(1, std::stoi(next_str()));
        else if (arg == "--max-ply" || arg == "--max-plies")       cfg.max_ply       = std::clamp(std::stoi(next_str()), 1, 1000);
        else if (arg == "--threshold" || arg == "--prune-cp")      cfg.eval_threshold_cp = std::max(1, std::stoi(next_str()));
        else if (arg == "--eval-ms")                               cfg.deep_eval_ms  = std::max(1, std::stoi(next_str()));
        else if (arg == "--eval-nodes")                            cfg.deep_eval_nodes = std::stoull(next_str());
        else if (arg == "--games")                                 cfg.selfplay_games = std::max(1, std::stoi(next_str()));
        else if (arg == "--game-limit-ms")                         cfg.game_limit_ms = std::max(50, std::stoi(next_str()));
        else if (arg == "--rollout-nodes" || arg == "--game-nodes") cfg.rollout_nodes = std::max<uint64_t>(50, std::stoull(next_str()));
        else if (arg == "--branch-nodes")                          cfg.branch_eval_nodes = std::max<uint64_t>(50, std::stoull(next_str()));
        else if (arg == "--max-children")                          cfg.max_children_per_node = std::max(0, std::stoi(next_str()));
        else if (arg == "--workers")                               cfg.workers       = std::max(1, std::stoi(next_str()));
        else if (arg == "--threads")                               cfg.threads_per_worker = std::max(1, std::stoi(next_str()));
        else if (arg == "--hash-mb")                               cfg.hash_mb_per_worker = std::max(4, std::stoi(next_str()));
        else if (arg == "--no-resume")                             cfg.resume        = false;
        else {
            std::cerr << "[WinrateBookGen] Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.workers <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        cfg.workers = std::clamp(static_cast<int>(hw > 0 ? hw : 4), 1, 32);
    }

    WinrateBook book;
    if (cfg.resume && file_exists_nonempty_path(cfg.book_path)) {
        std::string err;
        if (book.load(cfg.book_path, &err)) {
            std::cout << "[WinrateBookGen] Loaded existing DB '" << cfg.book_path
                      << "' (" << book.size() << " registered entries — unregistered nodes will be prioritized first)\n";
        }
    }

    // Spawn worker pool sequentially so Suisho 5 / YaneuraOu `EvalShare` shared memory
    // initializes cleanly on worker 0 and shares NNUE weights across all workers!
    std::vector<std::unique_ptr<ExternalUsiEngine>> pool;
    pool.reserve(static_cast<size_t>(cfg.workers));
    for (int w = 0; w < cfg.workers; ++w) {
        auto eng = std::make_unique<ExternalUsiEngine>();
        std::string err;
        if (!eng->start(cfg.engine_path, {}, &err, /*defer_isready=*/true)) {
            std::cerr << "[WinrateBookGen] ERROR starting engine '" << cfg.engine_path
                      << "': " << err << "\n";
            return 1;
        }
        eng->configure_own_book(false);
        eng->configure_zero_overhead_local_pipe();
        eng->configure_threads(cfg.threads_per_worker);
        eng->configure_hash_mb(cfg.hash_mb_per_worker);
        eng->configure_node_limit(0);
        if (!cfg.eval_dir.empty())  eng->set_option_if_supported("EvalDir", cfg.eval_dir);
        if (!cfg.eval_file.empty()) eng->set_option_if_supported("EvalFile", cfg.eval_file);

        if (!eng->ensure_ready()) {
            std::cerr << "[WinrateBookGen] ERROR: Engine '" << eng->exe_path()
                      << "' did not respond with 'readyok' (check eval/nn.bin placement).\n";
            return 1;
        }
        pool.push_back(std::move(eng));
    }

    const std::string id_label = pool[0]->engine_id_name().empty()
        ? pool[0]->exe_path()
        : (pool[0]->engine_id_name() + " [" + pool[0]->exe_path() + "]");

    std::cout << "[WinrateBookGen] Started " << cfg.workers << " engine worker(s): "
              << id_label << "\n"
              << "[WinrateBookGen] Beam width=" << cfg.beam_width
              << ", max_ply=" << cfg.max_ply
              << ", runs=" << cfg.num_runs
              << ", threshold=+-" << cfg.eval_threshold_cp << " cp"
              << ", selfplay_games=" << cfg.selfplay_games
              << " (limit=" << cfg.game_limit_ms << "ms/game)"
              << " -> DB='" << cfg.book_path << "'\n";
    std::cout.flush();

    for (int run_idx = 0; run_idx < cfg.num_runs; ++run_idx) {
        const size_t db_before_run = book.size();
        if (cfg.num_runs > 1) {
            std::cout << "\n=== [Beam Search Run " << (run_idx + 1) << "/" << cfg.num_runs
                      << " | Existing DB=" << db_before_run << " entries] ===\n";
            std::cout.flush();
        }

        // Initialize beam at Ply 0 with Hirate root node
        Board start_board;
        start_board.set_startpos();
        std::vector<BeamNode> beam;
        beam.push_back(BeamNode{{}, start_board.hash(), 0, !book.contains(start_board.hash())});

        std::unordered_set<uint64_t> expanded_hashes;

        for (int ply = 0; ply <= cfg.max_ply && !beam.empty(); ++ply) {
            const auto ply_t0 = std::chrono::steady_clock::now();
            size_t cnt_pruned_high = 0;
            size_t cnt_pruned_low  = 0;
            size_t cnt_selfplay    = 0;
            size_t cnt_cached_pass = 0;

            std::vector<size_t> expandable_indices;

            // ------------------------------------------------------------------
            // Step A: Deep-evaluate each node in current beam (`beam`)
            //         >= +600 -> (hash, 1.0) & prune
            //         <= -600 -> (hash, 0.0) & prune
            //         (-600, +600) -> 100 self-play games (<=10s/game) -> (hash, black_winrate) & branch
            // ------------------------------------------------------------------
            struct NodeEvalInfo {
                int    black_cp     = 0;
                bool   terminal     = false;
                double terminal_bwr = 0.5;
            };
            std::vector<NodeEvalInfo> eval_infos(beam.size());

            {
                std::atomic<size_t> next_idx{0};
                std::vector<std::thread> threads;
                const int use_w = std::min<int>(cfg.workers, static_cast<int>(beam.size()));
                for (int w = 0; w < use_w; ++w) {
                    threads.emplace_back([&, w]() {
                        ExternalUsiEngine& eng = *pool[static_cast<size_t>(w)];
                        while (true) {
                            const size_t idx = next_idx.fetch_add(1);
                            if (idx >= beam.size()) break;

                            Board b = replay_from_startpos(beam[idx].moves_from_start);
                            const RepetitionResult rep = b.repetition_result();
                            if (rep == RepetitionResult::DRAW) {
                                eval_infos[idx] = {0, true, 0.5};
                                continue;
                            }
                            if (rep == RepetitionResult::SIDE_TO_MOVE_WINS) {
                                eval_infos[idx] = {0, true, (b.side_to_move() == BLACK) ? 1.0 : 0.0};
                                continue;
                            }
                            if (rep == RepetitionResult::SIDE_TO_MOVE_LOSES) {
                                eval_infos[idx] = {0, true, (b.side_to_move() == BLACK) ? 0.0 : 1.0};
                                continue;
                            }
                            MoveList legal;
                            generate_legal_moves(b, legal);
                            if (legal.empty()) {
                                eval_infos[idx] = {0, true, (b.side_to_move() == BLACK) ? 0.0 : 1.0};
                                continue;
                            }

                            std::ostringstream go_oss;
                            int eval_watchdog_ms = 0;
                            if (cfg.deep_eval_nodes > 0) {
                                go_oss << "go nodes " << cfg.deep_eval_nodes;
                                eval_watchdog_ms = 10000;
                            } else {
                                go_oss << "go movetime " << cfg.deep_eval_ms;
                                eval_watchdog_ms = std::max(500, cfg.deep_eval_ms * 4 + 250);
                            }
                            const UsiEvalResult r = eng.run_search(
                                build_position_cmd(beam[idx].moves_from_start), go_oss.str(), {}, eval_watchdog_ms);
                            if (r.is_declare_win) {
                                eval_infos[idx] = {0, true, (b.side_to_move() == BLACK) ? 1.0 : 0.0};
                                continue;
                            }
                            if (r.is_resign) {
                                eval_infos[idx] = {0, true, (b.side_to_move() == BLACK) ? 0.0 : 1.0};
                                continue;
                            }
                            const int black_cp = (b.side_to_move() == BLACK) ? r.score_stm_cp : -r.score_stm_cp;
                            eval_infos[idx] = {black_cp, false, 0.5};
                        }
                    });
                }
                for (auto& th : threads) th.join();
            }

            // Process deep evaluations & run 100 self-play games for unregistered (-600, +600) nodes
            for (size_t idx = 0; idx < beam.size(); ++idx) {
                const BeamNode& node = beam[idx];
                const NodeEvalInfo& ei = eval_infos[idx];
                const bool already_registered = book.contains(node.hash);

                if (ei.terminal) {
                    if (!already_registered) book.put(node.hash, ei.terminal_bwr);
                    continue;
                }

                if (ei.black_cp >= cfg.eval_threshold_cp) {
                    if (!already_registered) book.put(node.hash, 1.0);
                    ++cnt_pruned_high;
                    continue;
                }
                if (ei.black_cp <= -cfg.eval_threshold_cp) {
                    if (!already_registered) book.put(node.hash, 0.0);
                    ++cnt_pruned_low;
                    continue;
                }

                if (!already_registered) {
                    std::atomic<int> game_counter{0};
                    std::mutex score_mtx;
                    double total_black_pts = 0.0;

                    std::vector<std::thread> sp_threads;
                    const int use_w = std::min<int>(cfg.workers, cfg.selfplay_games);
                    for (int w = 0; w < use_w; ++w) {
                        sp_threads.emplace_back([&, w]() {
                            ExternalUsiEngine& eng = *pool[static_cast<size_t>(w)];
                            double local_pts = 0.0;
                            while (true) {
                                const int g = game_counter.fetch_add(1);
                                if (g >= cfg.selfplay_games) break;
                                const uint64_t seed = node.hash ^
                                    (static_cast<uint64_t>(g + 1) * 0x9E3779B97F4A7C15ULL) ^
                                    (static_cast<uint64_t>(ply) << 32) ^
                                    (static_cast<uint64_t>(run_idx) * 0xBF58476D1CE4E5B9ULL);
                                local_pts += run_single_selfplay_game(eng, node.moves_from_start, cfg, seed);
                            }
                            std::lock_guard<std::mutex> lk(score_mtx);
                            total_black_pts += local_pts;
                        });
                    }
                    for (auto& th : sp_threads) th.join();

                    const double black_wr = std::clamp(
                        total_black_pts / static_cast<double>(cfg.selfplay_games), 0.0, 1.0);
                    book.put(node.hash, black_wr);
                    ++cnt_selfplay;
                } else {
                    ++cnt_cached_pass;
                }

                if (ply < cfg.max_ply && expanded_hashes.insert(node.hash).second) {
                    expandable_indices.push_back(idx);
                }
            }

            // ------------------------------------------------------------------
            // Step B: Update `winrate_book.db` atomically at every ply!
            // ------------------------------------------------------------------
            {
                std::string save_err;
                if (!book.save_atomic(cfg.book_path, &save_err)) {
                    std::cerr << "[WinrateBookGen] ERROR saving '" << cfg.book_path << "': " << save_err << "\n";
                    return 1;
                }
            }

            if (ply >= cfg.max_ply || expandable_indices.empty()) {
                const int ply_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - ply_t0).count());
                std::cout << "[Ply " << std::setw(3) << ply << "/" << cfg.max_ply << "] "
                          << "beam=" << beam.size()
                          << " (+600_pruned=" << cnt_pruned_high
                          << ", -600_pruned=" << cnt_pruned_low
                          << ", selfplay=" << cnt_selfplay
                          << ", db_cached=" << cnt_cached_pass
                          << ") -> DB_total=" << book.size()
                          << " (" << ply_ms << " ms) [DONE]\n";
                std::cout.flush();
                break;
            }

            // ------------------------------------------------------------------
            // Step C: Branch surviving nodes and select `<= beam_width` next-generation nodes:
            //   1st Priority: Unregistered in `winrate_book.db` (`!book.contains(child_hash)`)
            //   2nd Priority: Black-POV evaluation (`black_eval`):
            //                   - 先手番 (`mover == BLACK`): 評価値降順ソート (`descending`)
            //                   - 後手番 (`mover == WHITE`): 評価値昇順ソート (`ascending`)
            // ------------------------------------------------------------------
            const Color mover_turn = (ply % 2 == 0) ? BLACK : WHITE;
            const Color child_stm  = ~mover_turn;

            struct RawCandidate {
                std::vector<Move> moves_from_start;
                uint64_t          hash            = 0;
                bool              is_unregistered = true;
            };
            std::unordered_map<uint64_t, size_t> seen_child_hashes;
            std::vector<RawCandidate> candidates;

            for (size_t idx : expandable_indices) {
                const BeamNode& parent = beam[idx];
                Board b = replay_from_startpos(parent.moves_from_start);
                MoveList legal;
                generate_legal_moves(b, legal);

                struct LocalMove {
                    Move     m;
                    uint64_t child_hash;
                    bool     unreg;
                    int      prio;
                };
                std::vector<LocalMove> lms;
                lms.reserve(static_cast<size_t>(legal.size()));
                for (int i = 0; i < legal.size(); ++i) {
                    const Move m = legal[i];
                    int p = 0;
                    if (!is_drop(m) && b.piece_at(to_sq(m)) != NO_PIECE) {
                        p += see_ge(b, m, 0) ? 5000 : -2000;
                    }
                    if (is_promote(m)) p += 2500;
                    if (!is_drop(m)) {
                        const PieceType pt = type_of(b.piece_at(from_sq(m)));
                        if (pt == PAWN || pt == ROOK || pt == BISHOP || pt == SILVER || pt == GOLD) p += 200;
                    }
                    b.do_move(m, false);
                    const uint64_t ch = b.hash();
                    b.undo_move(m);
                    lms.push_back({m, ch, !book.contains(ch), p});
                }

                // Always keep unregistered moves ahead of already-registered moves even if `--max-children` is set!
                std::stable_sort(lms.begin(), lms.end(), [](const LocalMove& a, const LocalMove& b2) {
                    if (a.unreg != b2.unreg) return a.unreg > b2.unreg;
                    return a.prio > b2.prio;
                });

                const size_t limit_k = (cfg.max_children_per_node > 0)
                    ? std::min<size_t>(lms.size(), static_cast<size_t>(std::max(cfg.beam_width, cfg.max_children_per_node)))
                    : lms.size();

                for (size_t k = 0; k < limit_k; ++k) {
                    const uint64_t ch = lms[k].child_hash;
                    if (seen_child_hashes.find(ch) == seen_child_hashes.end()) {
                        seen_child_hashes[ch] = candidates.size();
                        std::vector<Move> next_hist = parent.moves_from_start;
                        next_hist.push_back(lms[k].m);
                        candidates.push_back(RawCandidate{std::move(next_hist), ch, lms[k].unreg});
                    }
                }
            }

            // Evaluate all candidate children in parallel and convert score to Black-POV (`black_eval`)
            std::vector<BeamNode> scored_children(candidates.size());
            {
                std::atomic<size_t> next_c{0};
                std::vector<std::thread> c_threads;
                const int use_w = std::min<int>(cfg.workers, static_cast<int>(candidates.size()));
                for (int w = 0; w < use_w; ++w) {
                    c_threads.emplace_back([&, w]() {
                        ExternalUsiEngine& eng = *pool[static_cast<size_t>(w)];
                        std::ostringstream go_oss;
                        go_oss << "go nodes " << cfg.branch_eval_nodes;
                        const std::string go_str = go_oss.str();

                        while (true) {
                            const size_t ci = next_c.fetch_add(1);
                            if (ci >= candidates.size()) break;
                            const UsiEvalResult r = eng.run_search(
                                build_position_cmd(candidates[ci].moves_from_start), go_str, {}, 5000);

                            // `r.score_stm_cp` is from `child_stm` perspective (`~mover_turn`).
                            // Convert cleanly to Black-POV evaluation (`black_eval`):
                            int child_stm_cp = 0;
                            if (r.is_declare_win) child_stm_cp = +29999; // `child_stm` wins immediately
                            else if (r.is_resign) child_stm_cp = -29999; // `child_stm` is mated/resigns
                            else if (r.ok)        child_stm_cp = r.score_stm_cp;
                            else                  child_stm_cp = (mover_turn == BLACK) ? +29999 : -29999; // fallback penalizes failed move

                            const int black_eval = (child_stm == BLACK) ? child_stm_cp : -child_stm_cp;
                            scored_children[ci] = BeamNode{
                                candidates[ci].moves_from_start,
                                candidates[ci].hash,
                                black_eval,
                                candidates[ci].is_unregistered
                            };
                        }
                    });
                }
                for (auto& th : c_threads) th.join();
            }

            // Sort next-generation candidates:
            //   1. Unregistered in `winrate_book.db` first!
            //   2. Black-POV evaluation: descending on Black's turn (`mover_turn == BLACK`),
            //                            ascending  on White's turn (`mover_turn == WHITE`)!
            std::sort(scored_children.begin(), scored_children.end(),
                      [mover_turn](const BeamNode& a, const BeamNode& b) {
                          return beam_candidate_better(
                              mover_turn,
                              a.is_unregistered, a.black_eval, a.hash,
                              b.is_unregistered, b.black_eval, b.hash);
                      });

            if (scored_children.size() > static_cast<size_t>(cfg.beam_width)) {
                scored_children.resize(static_cast<size_t>(cfg.beam_width));
            }

            size_t picked_unreg = 0;
            for (const auto& bn : scored_children) {
                if (bn.is_unregistered) ++picked_unreg;
            }

            const int ply_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - ply_t0).count());
            std::cout << "[Ply " << std::setw(3) << ply << "/" << cfg.max_ply
                      << " " << (mover_turn == BLACK ? "先手:降順" : "後手:昇順") << "] "
                      << "beam=" << beam.size()
                      << " (+600_pruned=" << cnt_pruned_high
                      << ", -600_pruned=" << cnt_pruned_low
                      << ", selfplay=" << cnt_selfplay
                      << ", db_cached=" << cnt_cached_pass
                      << ", branched=" << candidates.size()
                      << " -> next_beam=" << scored_children.size()
                      << " [unreg=" << picked_unreg << "/" << scored_children.size() << "])"
                      << " DB_total=" << book.size()
                      << " (" << ply_ms << " ms)\n";
            std::cout.flush();

            beam = std::move(scored_children);
        }

        if (cfg.num_runs > 1) {
            std::cout << "=== [Run " << (run_idx + 1) << " Summary: +"
                      << (book.size() - db_before_run) << " new entries -> DB_total="
                      << book.size() << "] ===\n";
            std::cout.flush();
        }
    }

    std::cout << "[WinrateBookGen] Completed. Final '" << cfg.book_path
              << "' contains " << book.size() << " positions.\n";
    return 0;
}
