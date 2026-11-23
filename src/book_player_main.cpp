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
#include <cctype>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::string to_lower_str(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int default_threads_count() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(static_cast<int>(hw > 0 ? hw : 4), 1, 64);
}

} // namespace

// ============================================================
// `winrate_book_player` — USI Win-Rate Book + External Engine Proxy
//   Register THIS executable directly in ShogiGUI!
//   1. If current `board.hash()` is in `winrate_book.db` (with >=1 book move),
//      performs minimax over `winrate_book.db`, prints the winrate, and plays the best move.
//   2. Otherwise, delegates `position` + `go` seamlessly to `ChildEnginePath`
//      (`shogi_engine.exe` OR Suisho 5 / YaneuraOu `.exe` — plug-and-play with zero extra setup!).
// ============================================================
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Zobrist::init();

    std::string book_file_opt       = "winrate_book.db";
    int         book_minimax_depth  = 24;
#if defined(_WIN32)
    std::string child_engine_opt    = "shogi_engine.exe";
#else
    std::string child_engine_opt    = "./shogi_engine";
#endif
    bool        child_own_book_opt  = false;
    int         child_threads_opt   = default_threads_count();
    int         child_hash_opt      = 256;
    uint64_t    child_nodelimit_opt = 0;
    std::string child_eval_dir_opt  = "";
    std::string child_eval_file_opt = "";
    std::string child_prog_file_opt = "";
    int         child_fv_scale_opt  = 0; // 0 = preserve child engine's own tuned default (Suisho 5 or shogi_engine)
    std::vector<std::pair<std::string, std::string>> custom_passthrough_opts;

    bool        book_dirty          = true;
    bool        child_dirty         = true;
    WinrateBook book;
    ExternalUsiEngine child_engine;

    Board board;
    board.set_startpos();
    std::string last_position_cmd = "position startpos";

    std::atomic<bool> proxy_searching{false};
    std::thread go_thread;

    auto ensure_book_and_child_loaded = [&]() {
        if (book_dirty) {
            book_dirty = false;
            const std::string resolved_book = resolve_sibling_or_cwd_path(book_file_opt);
            std::string b_err;
            if (book.load(resolved_book, &b_err)) {
                std::cout << "info string [WinrateBook] Loaded " << book.size()
                          << " entries from " << resolved_book << "\n";
            } else {
                book.clear();
                std::cout << "info string [WinrateBook] Book not loaded (" << b_err
                          << ") -> will use ChildEnginePath directly\n";
            }
        }

        if (child_dirty || !child_engine.is_running()) {
            child_dirty = false;
            std::string c_err;
            if (child_engine.start(child_engine_opt, {}, &c_err, /*defer_isready=*/true)) {
                child_engine.configure_own_book(child_own_book_opt);
                child_engine.configure_threads(child_threads_opt);
                child_engine.configure_hash_mb(child_hash_opt);
                child_engine.configure_node_limit(child_nodelimit_opt);

                if (!child_eval_dir_opt.empty())  child_engine.set_option_if_supported("EvalDir", child_eval_dir_opt);
                if (!child_eval_file_opt.empty()) child_engine.set_option_if_supported("EvalFile", child_eval_file_opt);
                if (!child_prog_file_opt.empty()) child_engine.set_option_if_supported("ProgressFile", child_prog_file_opt);
                if (child_fv_scale_opt > 0) {
                    child_engine.set_option_if_supported("FV_SCALE", std::to_string(child_fv_scale_opt));
                }
                for (const auto& kv : custom_passthrough_opts) {
                    child_engine.set_option_if_supported(kv.first, kv.second);
                }

                if (child_engine.ensure_ready()) {
                    const std::string label = child_engine.engine_id_name().empty()
                        ? child_engine.exe_path()
                        : (child_engine.engine_id_name() + " (" + child_engine.exe_path() + ")");
                    std::cout << "info string [WinrateBook] Connected child engine: " << label << "\n";
                } else {
                    std::cout << "info string [WinrateBook] WARNING: Child engine failed during isready: "
                              << child_engine.exe_path() << "\n";
                }
            } else {
                std::cout << "info string [WinrateBook] WARNING: Child engine failed to start ("
                          << c_err << ")\n";
            }
        } else {
            child_engine.ensure_ready();
        }
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd.empty()) continue;

        if (go_thread.joinable() && cmd != "stop") {
            if (proxy_searching.load(std::memory_order_relaxed)) {
                child_engine.send_stop();
            }
            go_thread.join();
        }

        if (cmd == "usi") {
            std::cout << "id name ShogiEngine_WinrateBook\n";
            std::cout << "id author ShogiAI\n";
            std::cout << "option name BookFile type string default " << book_file_opt << "\n";
            std::cout << "option name BookMinimaxDepth type spin default " << book_minimax_depth << " min 1 max 200\n";
            std::cout << "option name ChildEnginePath type string default " << child_engine_opt << "\n";
            std::cout << "option name ChildOwnBook type check default false\n";
            std::cout << "option name Threads type spin default " << child_threads_opt << " min 1 max 256\n";
            std::cout << "option name USI_Hash type spin default " << child_hash_opt << " min 4 max 65536\n";
            std::cout << "option name nodelimit type spin default 0 min 0 max 2147483647\n";
            std::cout << "option name EvalDir type string default \n";
            std::cout << "option name EvalFile type string default \n";
            std::cout << "option name ProgressFile type string default \n";
            std::cout << "option name FV_SCALE type spin default 0 min 0 max 256\n";
            std::cout << "usiok\n";
            std::cout.flush();

        } else if (cmd == "isready") {
            ensure_book_and_child_loaded();
            std::cout << "readyok\n";
            std::cout.flush();

        } else if (cmd == "setoption") {
            std::string tok, name, value;
            iss >> tok; // "name"
            while (iss >> tok && tok != "value") {
                if (!name.empty()) name += ' ';
                name += tok;
            }
            while (iss >> tok) {
                if (!value.empty()) value += ' ';
                value += tok;
            }
            const std::string lname = to_lower_str(name);
            if (lname == "bookfile") {
                book_file_opt = value;
                book_dirty    = true;
            } else if (lname == "bookminimaxdepth" && !value.empty()) {
                try { book_minimax_depth = std::clamp(std::stoi(value), 1, 200); } catch (...) {}
            } else if (lname == "childenginepath") {
                if (!value.empty() && value != child_engine_opt) {
                    child_engine_opt = value;
                    child_dirty      = true;
                }
            } else if (lname == "childownbook") {
                child_own_book_opt = (to_lower_str(value) == "true" || value == "1");
                if (child_engine.is_running()) child_engine.configure_own_book(child_own_book_opt);
            } else if ((lname == "threads" || lname == "usi_threads") && !value.empty()) {
                try {
                    child_threads_opt = std::clamp(std::stoi(value), 1, 256);
                    if (child_engine.is_running()) child_engine.configure_threads(child_threads_opt);
                } catch (...) {}
            } else if ((lname == "usi_hash" || lname == "hash") && !value.empty()) {
                try {
                    child_hash_opt = std::clamp(std::stoi(value), 4, 65536);
                    if (child_engine.is_running()) child_engine.configure_hash_mb(child_hash_opt);
                } catch (...) {}
            } else if ((lname == "nodelimit" || lname == "nodeslimit") && !value.empty()) {
                try {
                    child_nodelimit_opt = (!value.empty() && value[0] == '-') ? 0ULL : std::stoull(value);
                    if (child_engine.is_running()) child_engine.configure_node_limit(child_nodelimit_opt);
                } catch (...) {}
            } else if (lname == "evaldir") {
                child_eval_dir_opt = value;
                if (!value.empty() && child_engine.is_running()) {
                    child_engine.set_option_if_supported("EvalDir", value);
                }
            } else if (lname == "evalfile") {
                child_eval_file_opt = value;
                if (!value.empty() && child_engine.is_running()) {
                    child_engine.set_option_if_supported("EvalFile", value);
                }
            } else if (lname == "progressfile") {
                child_prog_file_opt = value;
                if (!value.empty() && child_engine.is_running()) {
                    child_engine.set_option_if_supported("ProgressFile", value);
                }
            } else if (lname == "fv_scale" && !value.empty()) {
                try {
                    child_fv_scale_opt = std::clamp(std::stoi(value), 0, 256);
                    if (child_fv_scale_opt > 0 && child_engine.is_running()) {
                        child_engine.set_option_if_supported("FV_SCALE", std::to_string(child_fv_scale_opt));
                    }
                } catch (...) {}
            } else if (!name.empty()) {
                custom_passthrough_opts.emplace_back(name, value);
                if (child_engine.is_running()) {
                    child_engine.set_option_if_supported(name, value);
                }
            }

        } else if (cmd == "usinewgame") {
            board.set_startpos();
            last_position_cmd = "position startpos";
            if (child_engine.is_running()) {
                child_engine.send_usinewgame();
            }

        } else if (cmd == "gameover") {
            if (child_engine.is_running()) {
                child_engine.send_raw_line(line);
            }

        } else if (cmd == "position") {
            last_position_cmd = line;
            std::string type;
            iss >> type;

            if (type == "startpos") {
                board.set_startpos();
                std::string moves_tok;
                if (iss >> moves_tok && moves_tok == "moves") {
                    std::string mv;
                    while (iss >> mv) board.do_move(usi_to_move(mv));
                }
            } else if (type == "sfen") {
                std::string sfen, tok;
                int fields = 0;
                while (fields < 4 && iss >> tok) {
                    if (tok == "moves") break;
                    if (fields > 0) sfen += ' ';
                    sfen += tok;
                    ++fields;
                }
                board.parse_sfen(sfen);
                if (tok != "moves") iss >> tok;
                if (tok == "moves") {
                    std::string mv;
                    while (iss >> mv) board.do_move(usi_to_move(mv));
                }
            }

        } else if (cmd == "go") {
            ensure_book_and_child_loaded();

            // 1. Check `winrate_book.db` first!
            const auto t0 = std::chrono::steady_clock::now();
            Board book_board = board;
            const BookMinimaxResult bres = book.search_minimax(book_board, book_minimax_depth);

            if (bres.in_book && bres.best_move != MOVE_NONE) {
                const int elapsed = std::max(1, static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count()));
                const uint64_t nps = (bres.visited_nodes * 1000ULL) / static_cast<uint64_t>(elapsed);

                std::ostringstream msg;
                msg << std::fixed << std::setprecision(1)
                    << "info string [勝率定跡ヒット] 先手勝率=" << (bres.black_winrate * 100.0) << "%"
                    << " / 手番勝率=" << (bres.stm_winrate * 100.0) << "%"
                    << " (winrate=" << std::setprecision(4) << bres.black_winrate
                    << ", minimax_depth=" << bres.reached_depth
                    << ", book_nodes=" << bres.visited_nodes << ")";
                std::cout << msg.str() << "\n";

                std::cout << "info depth " << bres.reached_depth
                          << " seldepth " << bres.reached_depth
                          << " time " << elapsed
                          << " nodes " << bres.visited_nodes
                          << " nps " << nps
                          << " score cp " << bres.score_cp_stm
                          << " pv";
                for (Move pm : bres.pv) std::cout << ' ' << move_to_usi(pm);
                std::cout << "\n";
                std::cout << "bestmove " << move_to_usi(bres.best_move) << "\n";
                std::cout.flush();
                continue;
            }

            // 2. Out of book (or book leaf reached): delegate asynchronously to ChildEnginePath!
            const std::string engine_tag = child_engine.engine_id_name().empty()
                ? child_engine.exe_path()
                : child_engine.engine_id_name();

            if (bres.hash_present) {
                std::cout << "info string [勝率定跡終端] 現在ノード先手勝率="
                          << std::fixed << std::setprecision(1) << (bres.node_black_wr * 100.0)
                          << "% -> 通常将棋AI (" << engine_tag << ") に指し継ぎます\n";
            } else {
                std::cout << "info string [定跡外] 通常将棋AI (" << engine_tag << ") で探索します\n";
            }
            std::cout.flush();

            if (!child_engine.is_running()) {
                MoveList legal;
                generate_legal_moves(board, legal);
                if (legal.empty()) std::cout << "bestmove resign\n";
                else               std::cout << "bestmove " << move_to_usi(legal[0]) << "\n";
                std::cout.flush();
                continue;
            }

            const std::string pos_cmd_copy = last_position_cmd;
            const std::string go_cmd_copy  = line;
            std::promise<void> started;
            std::future<void> fut = started.get_future();

            proxy_searching.store(true, std::memory_order_relaxed);
            go_thread = std::thread([&child_engine, &proxy_searching, pos_cmd_copy, go_cmd_copy,
                                     started = std::move(started)]() mutable {
                started.set_value();
                const UsiEvalResult r = child_engine.run_search(
                    pos_cmd_copy, go_cmd_copy,
                    [](const std::string& info_line) {
                        std::cout << info_line << "\n";
                        std::cout.flush();
                    });
                proxy_searching.store(false, std::memory_order_relaxed);
                std::cout << "bestmove " << (r.ok ? r.best_usi : "resign") << "\n";
                std::cout.flush();
            });
            fut.wait();

        } else if (cmd == "stop") {
            if (proxy_searching.load(std::memory_order_relaxed)) {
                child_engine.send_stop();
            }
            if (go_thread.joinable()) go_thread.join();

        } else if (cmd == "quit") {
            break;
        }
    }

    if (go_thread.joinable()) {
        if (proxy_searching.load(std::memory_order_relaxed)) {
            child_engine.send_stop();
        }
        go_thread.join();
    }
    child_engine.stop_process();
    return 0;
}
