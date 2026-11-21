#include "board.hpp"
#include "movegen.hpp"
#include "search.hpp"
#include "eval.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>
#include <future>
#include <thread>
#include <utility>

// ============================================================
// USI main loop
// ============================================================
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Zobrist::init();

    Board board;
    board.set_startpos();
    std::thread search_thread;

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (search_thread.joinable() && cmd != "stop") {
            g_stop.store(true);
            search_thread.join();
        }

        // ---- usi ----
        if (cmd == "usi") {
            std::cout << "id name ShogiEngine\n";
            std::cout << "id author ShogiAI\n";
            std::cout << "option name USI_Hash type spin default 64 min 4 max 2048\n";
            std::cout << "option name EvalFile type string default eval/fv.bin\n";
            std::cout << "option name FV_SCALE type spin default 32 min 1 max 256\n";
            std::cout << "usiok\n";
            std::cout.flush();

        // ---- isready ----
        } else if (cmd == "isready") {
            (void)eval_status_message();
            std::cout << "readyok\n";
            std::cout.flush();

        // ---- setoption ----
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
            if (name == "EvalFile") {
                set_eval_file_path(value);
            } else if (name == "USI_Hash" && !value.empty()) {
                try { set_tt_size_mb(static_cast<size_t>(std::stoul(value))); } catch (...) {}
            } else if (name == "FV_SCALE" && !value.empty()) {
                try { set_fv_scale(std::stoi(value)); } catch (...) {}
            }

        // ---- usinewgame ----
        } else if (cmd == "usinewgame") {
            board.set_startpos();
            search_clear_game_state();

        // ---- position ----
        } else if (cmd == "position") {
            std::string type;
            iss >> type;

            if (type == "startpos") {
                board.set_startpos();
            } else if (type == "sfen") {
                std::string sfen;
                std::string tok;
                int fields = 0;
                while (fields < 4 && iss >> tok) {
                    if (tok == "moves") break;
                    if (fields > 0) sfen += ' ';
                    sfen += tok;
                    fields++;
                }
                board.parse_sfen(sfen);
                if (tok != "moves") iss >> tok;
                if (tok == "moves") {
                    std::string mv;
                    while (iss >> mv)
                        board.do_move(usi_to_move(mv));
                }
                continue;
            }

            {
                std::string moves_tok;
                if (iss >> moves_tok && moves_tok == "moves") {
                    std::string mv;
                    while (iss >> mv)
                        board.do_move(usi_to_move(mv));
                }
            }

        // ---- go ----
        } else if (cmd == "go") {
            int btime = -1, wtime = -1, binc = 0, winc = 0, byoyomi = 0, movetime = -1;
            int depth_limit = MAX_DEPTH;
            bool infinite = false;

            std::string tok;
            while (iss >> tok) {
                if      (tok == "btime"   ) iss >> btime;
                else if (tok == "wtime"   ) iss >> wtime;
                else if (tok == "binc"    ) iss >> binc;
                else if (tok == "winc"    ) iss >> winc;
                else if (tok == "byoyomi" ) iss >> byoyomi;
                else if (tok == "movetime") iss >> movetime;
                else if (tok == "depth"   ) iss >> depth_limit;
                else if (tok == "infinite") infinite = true;
            }

            TimeBudget budget;
            if (movetime > 0) {
                int ms = std::max(1, movetime - 15);
                budget = {ms, ms};
            } else if (infinite) {
                budget = {0, 0}; // run until explicit `stop` or max depth
            } else if (btime >= 0 || wtime >= 0 || byoyomi > 0) {
                const bool is_black = (board.side_to_move() == BLACK);
                const int my_time = is_black ? btime : wtime;
                const int my_inc  = is_black ? binc  : winc;
                budget = compute_time_budget(std::max(0, my_time), std::max(0, my_inc),
                                             std::max(0, byoyomi), board.ply_);
            } else {
                budget = {1500, 2000};
            }

            std::cout << "info string " << eval_status_message() << "\n";
            std::cout.flush();

            Board search_board = board;
            std::promise<void> started;
            std::future<void> search_started = started.get_future();
            search_thread = std::thread([search_board = std::move(search_board), budget, depth_limit,
                                         started = std::move(started)]() mutable {
                Move best = iterative_deepening_timed(search_board, budget, [](const SearchInfo& info) {
                    std::cout << "info depth " << info.depth
                              << " seldepth " << info.seldepth
                              << " time " << info.time_ms
                              << " nodes " << info.nodes
                              << " nps " << info.nps << " score ";
                    if (info.score_is_mate) std::cout << "mate " << info.score_mate;
                    else                    std::cout << "cp " << info.score_cp;
                    std::cout << " pv";
                    for (Move m : info.pv) std::cout << ' ' << move_to_usi(m);
                    std::cout << "\n";
                    std::cout.flush();
                }, [&started] { started.set_value(); }, depth_limit);

                if (best == MOVE_NONE) {
                    std::cout << "bestmove resign\n";
                } else {
                    std::cout << "bestmove " << move_to_usi(best) << "\n";
                }
                std::cout.flush();
            });
            search_started.wait();

        // ---- stop ----
        } else if (cmd == "stop") {
            g_stop.store(true);
            if (search_thread.joinable()) search_thread.join();

        // ---- quit ----
        } else if (cmd == "quit") {
            break;
        }
    }

    if (search_thread.joinable()) {
        g_stop.store(true);
        search_thread.join();
    }

    return 0;
}
