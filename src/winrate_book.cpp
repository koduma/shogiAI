// Workaround for MinGW-w64 GCC Bug 65782 / 54412:
// `-march=native` on AVX-512 CPUs emits `.seh_savexmm %xmm16..31` (rejected by GNU as on Win64 SEH)
// and 64-byte ZMM stack spills. Disable AVX-512 on MinGW GCC while keeping full AVX2/FMA/BMI2 active.
#if (defined(_WIN32) && defined(__GNUC__) && !defined(__clang__)) || defined(SHOGIAI_FORCE_NO_AVX512_TEST)
#pragma GCC target("no-avx512f,no-avx512vl,no-avx512bw,no-avx512dq,no-avx512cd")
#ifndef SHOGIAI_NO_AVX512
#define SHOGIAI_NO_AVX512 1
#endif
#endif
#include "winrate_book.hpp"
#include "movegen.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

bool WinrateBook::contains(uint64_t hash) const {
    return table_.find(hash) != table_.end();
}

double WinrateBook::get_black_winrate(uint64_t hash, double fallback) const {
    const auto it = table_.find(hash);
    return (it != table_.end()) ? it->second : fallback;
}

void WinrateBook::put(uint64_t hash, double black_winrate) {
    table_[hash] = std::clamp(black_winrate, 0.0, 1.0);
}

bool WinrateBook::load(const std::string& path, std::string* err_out) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        if (err_out) *err_out = "Cannot open book file: " + path;
        return false;
    }

    table_.clear();
    std::string line;
    size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Strip leading/trailing whitespace and optional '(' / ')'
        size_t s = 0;
        while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s]))) ++s;
        if (s >= line.size() || line[s] == '#') continue;
        if (line[s] == '(') ++s;

        size_t e = line.size();
        while (e > s && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
        if (e > s && line[e - 1] == ')') --e;
        if (e <= s) continue;

        std::string body = line.substr(s, e - s);
        for (char& c : body) {
            if (c == ',' || c == '\t') c = ' ';
        }

        std::istringstream iss(body);
        std::string h_tok;
        double wr = 0.5;
        if (!(iss >> h_tok >> wr)) continue;

        try {
            uint64_t h = 0;
            if (h_tok.size() > 2 && h_tok[0] == '0' && (h_tok[1] == 'x' || h_tok[1] == 'X')) {
                h = std::stoull(h_tok.substr(2), nullptr, 16);
            } else {
                h = std::stoull(h_tok, nullptr, 0);
            }
            table_[h] = std::clamp(wr, 0.0, 1.0);
        } catch (...) {
            if (err_out) {
                std::ostringstream oss;
                oss << "Parse error in " << path << " at line " << line_no;
                *err_out = oss.str();
            }
            return false;
        }
    }
    return true;
}

bool WinrateBook::save_atomic(const std::string& path, std::string* err_out) const {
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            if (err_out) *err_out = "Cannot write temporary book file: " + tmp_path;
            return false;
        }
        ofs << "# ShogiAI Winrate Book DB (hash,black_winrate)\n";
        std::vector<std::pair<uint64_t, double>> sorted_rows(table_.begin(), table_.end());
        std::sort(sorted_rows.begin(), sorted_rows.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        ofs << std::fixed << std::setprecision(6);
        for (const auto& kv : sorted_rows) {
            std::ostringstream hs;
            hs << "0x" << std::hex << std::setw(16) << std::setfill('0') << kv.first;
            ofs << hs.str() << "," << kv.second << "\n";
        }
        ofs.flush();
        if (!ofs.good()) {
            if (err_out) *err_out = "Flush failed on: " + tmp_path;
            return false;
        }
    }

#if defined(_WIN32)
    if (!MoveFileExA(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (err_out) *err_out = "MoveFileExA failed replacing: " + path;
        return false;
    }
#else
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        if (err_out) *err_out = "rename() failed replacing: " + path;
        return false;
    }
#endif
    return true;
}

bool WinrateBook::append_record(const std::string& path, uint64_t hash, double black_winrate) {
    std::ofstream ofs(path, std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return false;
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(16) << std::setfill('0') << hash
        << "," << std::fixed << std::setprecision(6) << std::clamp(black_winrate, 0.0, 1.0) << "\n";
    ofs << oss.str();
    ofs.flush();
    return ofs.good();
}

int WinrateBook::winrate_to_cp(double stm_winrate) {
    const double p = std::clamp(stm_winrate, 0.0005, 0.9995);
    const double cp = 600.0 * std::log(p / (1.0 - p));
    return std::clamp(static_cast<int>(std::round(cp)), -9900, 9900);
}

namespace {

struct MemoKey {
    uint64_t hash;
    int      rem_depth;
    bool operator==(const MemoKey& o) const {
        return hash == o.hash && rem_depth == o.rem_depth;
    }
};

struct MemoKeyHash {
    size_t operator()(const MemoKey& k) const {
        return static_cast<size_t>(k.hash ^ (static_cast<uint64_t>(k.rem_depth) * 0x9E3779B97F4A7C15ULL));
    }
};

struct MemoVal {
    double            black_wr = 0.5;
    std::vector<Move> pv;
};

struct MinimaxCtx {
    const WinrateBook& book;
    uint64_t           visited_nodes = 0;
    std::vector<uint64_t> path_stack{};
    std::unordered_map<MemoKey, MemoVal, MemoKeyHash> exact_memo{};
    explicit MinimaxCtx(const WinrateBook& b) : book(b) {}
};

// Minimax with DAG memoization & cycle protection over `book`
MemoVal book_minimax_rec(MinimaxCtx& ctx, Board& board, int rem_depth) {
    ++ctx.visited_nodes;
    const uint64_t h = board.hash();

    // Repetition / cycle check along active search path
    const RepetitionResult rep = board.repetition_result();
    if (rep == RepetitionResult::DRAW) {
        return {0.5, {}};
    } else if (rep == RepetitionResult::SIDE_TO_MOVE_WINS) {
        return {board.side_to_move() == BLACK ? 1.0 : 0.0, {}};
    } else if (rep == RepetitionResult::SIDE_TO_MOVE_LOSES) {
        return {board.side_to_move() == BLACK ? 0.0 : 1.0, {}};
    }
    if (std::find(ctx.path_stack.begin(), ctx.path_stack.end(), h) != ctx.path_stack.end()) {
        return {0.5, {}};
    }

    const MemoKey mk{h, rem_depth};
    const auto mit = ctx.exact_memo.find(mk);
    if (mit != ctx.exact_memo.end()) {
        return mit->second;
    }

    const double self_bwr = ctx.book.get_black_winrate(h, 0.5);
    if (rem_depth <= 0) {
        return {self_bwr, {}};
    }

    MoveList legal;
    generate_legal_moves(board, legal);
    if (legal.empty()) {
        const double mate_bwr = (board.side_to_move() == BLACK) ? 0.0 : 1.0;
        return {mate_bwr, {}};
    }

    // Collect legal moves whose destination hash exists in `winrate_book.db`
    std::vector<std::pair<Move, double>> book_children;
    book_children.reserve(static_cast<size_t>(legal.size()));
    for (int i = 0; i < legal.size(); ++i) {
        const Move m = legal[i];
        board.do_move(m, false);
        const uint64_t ch = board.hash();
        if (ctx.book.contains(ch)) {
            book_children.push_back({m, ctx.book.get_black_winrate(ch, 0.5)});
        }
        board.undo_move(m);
    }

    // Leaf node of `winrate_book.db` (e.g. +600->1.0 pruned, -600->0.0 pruned, or ply-200 leaf)
    if (book_children.empty()) {
        return {self_bwr, {}};
    }

    const bool is_black = (board.side_to_move() == BLACK);
    std::sort(book_children.begin(), book_children.end(),
              [is_black](const auto& a, const auto& b) {
                  return is_black ? (a.second > b.second) : (a.second < b.second);
              });

    ctx.path_stack.push_back(h);

    double best_bwr = is_black ? -1.0 : 2.0;
    double best_imm = is_black ? -1.0 : 2.0;
    std::vector<Move> best_pv;

    for (const auto& cand : book_children) {
        const Move   m      = cand.first;
        const double imm_wr = cand.second;

        board.do_move(m);
        MemoVal child_val = book_minimax_rec(ctx, board, rem_depth - 1);
        board.undo_move(m);

        bool better = false;
        constexpr double EPS = 1e-9;
        if (is_black) {
            if (child_val.black_wr > best_bwr + EPS) {
                better = true;
            } else if (std::abs(child_val.black_wr - best_bwr) <= EPS) {
                if (imm_wr > best_imm + EPS) {
                    better = true;
                } else if (std::abs(imm_wr - best_imm) <= EPS && !best_pv.empty()) {
                    // Prefer faster win when winning (>0.5), longer resistance when losing (<0.5)
                    better = (child_val.black_wr >= 0.5)
                           ? (child_val.pv.size() + 1 < best_pv.size())
                           : (child_val.pv.size() + 1 > best_pv.size());
                }
            }
        } else {
            if (child_val.black_wr < best_bwr - EPS) {
                better = true;
            } else if (std::abs(child_val.black_wr - best_bwr) <= EPS) {
                if (imm_wr < best_imm - EPS) {
                    better = true;
                } else if (std::abs(imm_wr - best_imm) <= EPS && !best_pv.empty()) {
                    better = (child_val.black_wr <= 0.5)
                           ? (child_val.pv.size() + 1 < best_pv.size())
                           : (child_val.pv.size() + 1 > best_pv.size());
                }
            }
        }

        if (better) {
            best_bwr = child_val.black_wr;
            best_imm = imm_wr;
            best_pv.clear();
            best_pv.push_back(m);
            for (Move pm : child_val.pv) best_pv.push_back(pm);
        }
    }

    ctx.path_stack.pop_back();

    MemoVal out{best_bwr, std::move(best_pv)};
    if (ctx.path_stack.empty() || ctx.exact_memo.size() < 200'000) {
        ctx.exact_memo.emplace(mk, out);
    }
    return out;
}

} // namespace

BookMinimaxResult WinrateBook::search_minimax(Board& board,
                                              int max_depth,
                                              double max_transposition_drop) const {
    BookMinimaxResult res{};
    const uint64_t root_hash = board.hash();
    if (!contains(root_hash)) {
        return res;
    }

    res.hash_present  = true;
    res.node_black_wr = get_black_winrate(root_hash, 0.5);

    const int clamped_depth = std::clamp(max_depth, 1, 256);
    MinimaxCtx ctx(*this);
    MemoVal mv = book_minimax_rec(ctx, board, clamped_depth);
    res.visited_nodes = ctx.visited_nodes;

    if (mv.pv.empty()) {
        // Current hash is a leaf in `winrate_book.db` with 0 children in DB -> hand off to engine!
        return res;
    }

    const Color  stm          = board.side_to_move();
    const double root_stm_wr  = (stm == BLACK) ? res.node_black_wr : (1.0 - res.node_black_wr);
    const double child_stm_wr = (stm == BLACK) ? mv.black_wr       : (1.0 - mv.black_wr);

    // Guard against unexpanded beam-boundary leaves that only accidentally transpose into
    // a much worse unrelated book node:
    if (max_transposition_drop > 0.0 && child_stm_wr + max_transposition_drop < root_stm_wr) {
        return res;
    }

    res.in_book       = true;
    res.best_move     = mv.pv.front();
    res.black_winrate = mv.black_wr;
    res.stm_winrate   = child_stm_wr;
    res.score_cp_stm  = winrate_to_cp(child_stm_wr);
    res.reached_depth = static_cast<int>(mv.pv.size());
    res.pv            = std::move(mv.pv);
    return res;
}
