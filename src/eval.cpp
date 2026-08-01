#include "eval.hpp"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

constexpr const char* DEFAULT_EVAL_FILE = "eval/kpp_weights.txt";

struct EvalParams {
    std::array<int, PT_NB> piece_value{
        0, 100, 300, 300, 500, 600, 800, 1000, 0, 600, 600, 600, 600, 1100, 1300
    };
    int kpp_weight = 12;
    int king_zone_bonus = 6;
};

EvalParams g_params;
bool g_loaded_once = false;
std::string g_eval_file_path = DEFAULT_EVAL_FILE;
std::string g_status = "built-in KPP fallback";

inline int manhattan_dist(Square a, Square b) {
    return std::abs(file_of(a) - file_of(b)) + std::abs(rank_of(a) - rank_of(b));
}

// Lightweight KPP-style interaction:
// king + piece1 + piece2
int kpp_term_for_side(const Board& board, Color c, const EvalParams& params) {
    std::array<Square, 40> feat_sq{};
    int n = 0;

    const Square king = board.king_sq(c);
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE || color_of(p) != c || type_of(p) == KING) continue;
        if (n < static_cast<int>(feat_sq.size())) feat_sq[n++] = s;
    }

    // Hand pieces are modeled as "virtual nearby pieces" so holdings affect KPP.
    constexpr int VIRTUAL_HAND_DIST = 3;
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        int count = board.hand(c, static_cast<PieceType>(pt));
        while (count-- > 0 && n < static_cast<int>(feat_sq.size())) {
            int f = std::clamp(file_of(king) + ((pt % 2) ? 1 : -1), 0, 8);
            int r = std::clamp(rank_of(king) + ((pt % 3) ? 1 : -1), 0, 8);
            feat_sq[n++] = make_sq(f, r);
            (void)VIRTUAL_HAND_DIST;
        }
    }

    int acc = 0;
    for (int i = 0; i < n; ++i) {
        const int d1 = manhattan_dist(king, feat_sq[i]);
        acc += std::max(0, 6 - d1) * params.king_zone_bonus;
        for (int j = i + 1; j < n; ++j) {
            const int d2 = manhattan_dist(king, feat_sq[j]);
            // 3-piece relation: K + Pi + Pj
            acc += std::max(0, 8 - (d1 + d2)) * params.kpp_weight;
        }
    }
    return acc;
}

void try_load_eval_file_once() {
    if (g_loaded_once) return;
    g_loaded_once = true;

    const char* env_path = std::getenv("SHOGIAI_EVAL_FILE");
    if (env_path && *env_path) g_eval_file_path = env_path;

    std::ifstream ifs(g_eval_file_path);
    if (!ifs) {
        g_status = std::string("built-in KPP fallback (eval file not found: ") + g_eval_file_path + ")";
        return;
    }

    EvalParams loaded = g_params;
    std::string line;
    int loaded_values = 0;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        int parsed = 0;
        std::istringstream iss(val);
        if (!(iss >> parsed)) continue;

        if (key == "kpp_weight") { loaded.kpp_weight = parsed; loaded_values++; continue; }
        if (key == "king_zone_bonus") { loaded.king_zone_bonus = parsed; loaded_values++; continue; }
        if (key.rfind("piece_", 0) == 0) {
            const std::string pt = key.substr(6);
            PieceType t = NO_PT;
            if      (pt == "pawn") t = PAWN;
            else if (pt == "lance") t = LANCE;
            else if (pt == "knight") t = KNIGHT;
            else if (pt == "silver") t = SILVER;
            else if (pt == "gold") t = GOLD;
            else if (pt == "bishop") t = BISHOP;
            else if (pt == "rook") t = ROOK;
            else if (pt == "prom_pawn") t = PROM_PAWN;
            else if (pt == "prom_lance") t = PROM_LANCE;
            else if (pt == "prom_knight") t = PROM_KNIGHT;
            else if (pt == "prom_silver") t = PROM_SILVER;
            else if (pt == "prom_bishop") t = PROM_BISHOP;
            else if (pt == "prom_rook") t = PROM_ROOK;
            if (t != NO_PT) {
                loaded.piece_value[t] = parsed;
                loaded_values++;
            }
        }
    }

    if (loaded_values == 0) {
        g_status = std::string("built-in KPP fallback (invalid eval file: ") + g_eval_file_path + ")";
        return;
    }
    g_params = loaded;
    g_status = std::string("external eval loaded from ") + g_eval_file_path;
}

} // namespace

void set_eval_file_path(const std::string& path) {
    g_eval_file_path = path.empty() ? DEFAULT_EVAL_FILE : path;
    g_loaded_once = false;
    g_status = "built-in KPP fallback";
}

std::string eval_status_message() {
    try_load_eval_file_once();
    return g_status;
}

int evaluate(const Board& board) {
    try_load_eval_file_once();

    const Color us = board.side_to_move();
    int score = 0;

    // Material (board + hand)
    for (int s = 0; s < SQUARE_NB; ++s) {
        Piece p = board.piece_at(s);
        if (p == NO_PIECE) continue;
        int v = g_params.piece_value[type_of(p)];
        score += (color_of(p) == us) ? v : -v;
    }
    for (int pt = PAWN; pt <= ROOK; ++pt) {
        int v = g_params.piece_value[pt];
        score += board.hand(us,  static_cast<PieceType>(pt)) * v;
        score -= board.hand(~us, static_cast<PieceType>(pt)) * v;
    }

    // KPP-style king-piece-piece interaction (symmetric)
    int us_kpp  = kpp_term_for_side(board, us,  g_params);
    int opp_kpp = kpp_term_for_side(board, ~us, g_params);
    score += us_kpp - opp_kpp;

    return score;
}
