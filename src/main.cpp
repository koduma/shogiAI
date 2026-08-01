#include "board.hpp"
#include "movegen.hpp"
#include "search.hpp"
#include "eval.hpp"
#include <iostream>
#include <sstream>
#include <string>
#include <algorithm>

// ============================================================
// USI main loop
// ============================================================
int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Zobrist::init();

    Board board;
    board.set_startpos();

    std::string line;
    while (std::getline(std::cin, line)) {
        // Strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        // ---- usi ----
        if (cmd == "usi") {
            std::cout << "id name ShogiEngine\n";
            std::cout << "id author ShogiAI\n";
            std::cout << "option name EvalFile type string default eval/kpp_weights.txt\n";
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
            }

        // ---- usinewgame ----
        } else if (cmd == "usinewgame") {
            board.set_startpos();

        // ---- position ----
        } else if (cmd == "position") {
            std::string type;
            iss >> type;

            if (type == "startpos") {
                board.set_startpos();
            } else if (type == "sfen") {
                // Collect the 4 SFEN fields (stop early if "moves" is encountered)
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
                // If we didn't break on "moves", read the next token
                if (tok != "moves") iss >> tok;
                // Apply any moves that follow
                if (tok == "moves") {
                    std::string mv;
                    while (iss >> mv)
                        board.do_move(usi_to_move(mv));
                }
                // Position fully set; nothing more to do for this command.
                continue;
            }

            // For "startpos": apply optional "moves" section
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
            int btime = -1, wtime = -1, byoyomi = 0, movetime = -1;
            bool infinite = false;

            std::string tok;
            while (iss >> tok) {
                if      (tok == "btime"   ) iss >> btime;
                else if (tok == "wtime"   ) iss >> wtime;
                else if (tok == "byoyomi" ) iss >> byoyomi;
                else if (tok == "movetime") iss >> movetime;
                else if (tok == "infinite") infinite = true;
            }

            // Time management
            int allotted_ms;
            if (movetime > 0) {
                allotted_ms = std::max(1, movetime - 10);  // small safety margin
            } else if (infinite) {
                allotted_ms = 60 * 1000;      // search for up to 60 s then return best
            } else {
                int my_time = (board.side_to_move() == BLACK) ? btime : wtime;
                if (my_time > 0) {
                    allotted_ms = compute_allotted_ms(my_time, byoyomi);
                } else if (byoyomi > 0) {
                    allotted_ms = byoyomi * 9 / 10;
                } else {
                    allotted_ms = 2000;        // fallback: 2 seconds
                }
            }

            std::cout << "info string " << eval_status_message() << "\n";
            std::cout.flush();

            Move best = iterative_deepening(board, allotted_ms, [](const SearchInfo& info) {
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
            });

            if (best == MOVE_NONE) {
                std::cout << "bestmove resign\n";
            } else {
                std::cout << "bestmove " << move_to_usi(best) << "\n";
            }
            std::cout.flush();

        // ---- stop ----
        } else if (cmd == "stop") {
            g_stop.store(true);

        // ---- quit ----
        } else if (cmd == "quit") {
            break;
        }
        // Ignore unknown commands (USI spec says to ignore them)
    }

    return 0;
}
