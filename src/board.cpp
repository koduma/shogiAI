#include "board.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cctype>

// ============================================================
// Zobrist tables
// ============================================================
namespace Zobrist {
    uint64_t piece_sq[PIECE_NB][SQUARE_NB];
    uint64_t hand   [COLOR_NB][PT_NB][MAX_HAND + 1];
    uint64_t side;

    static uint64_t rng_state = 0xdeadbeefcafe1234ULL;
    static uint64_t rng64() {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        return rng_state;
    }

    void init() {
        rng_state = 0xdeadbeefcafe1234ULL;
        for (int p = 0; p < PIECE_NB; p++)
            for (int s = 0; s < SQUARE_NB; s++)
                piece_sq[p][s] = rng64();

        for (int c = 0; c < COLOR_NB; c++)
            for (int pt = 0; pt < PT_NB; pt++) {
                hand[c][pt][0] = 0; // empty hand = 0 contribution
                for (int n = 1; n <= MAX_HAND; n++)
                    hand[c][pt][n] = rng64();
            }

        side = rng64();
    }
}

// ============================================================
// Board constructor / reset
// ============================================================
Board::Board() {
    history_.reserve(256);
    pos_hashes_.reserve(512);
    reset();
}

void Board::reset() {
    std::memset(board_, 0, sizeof(board_));
    std::memset(hand_,  0, sizeof(hand_));
    stm_            = BLACK;
    ply_            = 0;
    hash_           = 0;
    material_black_ = 0;
    king_sq_[BLACK] = SQ_NONE;
    king_sq_[WHITE] = SQ_NONE;
    history_.clear();
    pos_hashes_.clear();
}

// ============================================================
// Zobrist hash & material recomputation from scratch
// ============================================================
void Board::compute_hash_and_material() {
    hash_ = 0;
    material_black_ = 0;
    for (int s = 0; s < SQUARE_NB; s++) {
        Piece p = board_[s];
        if (p != NO_PIECE) {
            hash_ ^= Zobrist::piece_sq[p][s];
            int v = PIECE_VALUE_BONANZA[type_of(p)];
            material_black_ += (color_of(p) == BLACK) ? v : -v;
        }
    }
    for (int c = 0; c < COLOR_NB; c++) {
        for (int pt = 0; pt < PT_NB; pt++) {
            int cnt = hand_[c][pt];
            if (cnt >= 0 && cnt <= Zobrist::MAX_HAND)
                hash_ ^= Zobrist::hand[c][pt][cnt];
            if (pt >= PAWN && pt <= ROOK && cnt != 0) {
                int v = PIECE_VALUE_BONANZA[pt] * cnt;
                material_black_ += (c == BLACK) ? v : -v;
            }
        }
    }
    if (stm_ == WHITE)
        hash_ ^= Zobrist::side;
}

// ============================================================
// set_startpos  –  standard shogi starting position
// SFEN: lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1
// ============================================================
void Board::set_startpos() {
    parse_sfen("lnsgkgsnl/1r5b1/ppppppppp/9/9/9/PPPPPPPPP/1B5R1/LNSGKGSNL b - 1");
}

// ============================================================
// parse_sfen
// ============================================================
bool Board::parse_sfen(const std::string& sfen) {
    reset();

    std::istringstream ss(sfen);
    std::string board_str, stm_str, hand_str, move_cnt_str;
    ss >> board_str >> stm_str >> hand_str >> move_cnt_str;

    // --- board ---
    int r = 0, f = 0;
    bool promo = false;
    for (char c : board_str) {
        if (c == '/') { r++; f = 0; promo = false; continue; }
        if (c == '+') { promo = true; continue; }
        if (c >= '1' && c <= '9') {
            f += (c - '0'); promo = false; continue;
        }
        Color    pc = std::isupper(c) ? BLACK : WHITE;
        PieceType pt = char_to_pt(static_cast<char>(std::toupper(c)));
        if (promo) pt = promote_pt(pt);
        promo = false;
        Square sq = make_sq(f, r);
        board_[sq] = make_piece(pc, pt);
        if (type_of(board_[sq]) == KING) king_sq_[pc] = sq;
        f++;
    }

    // --- side to move ---
    stm_ = (stm_str == "b") ? BLACK : WHITE;

    // --- hand pieces ---
    if (hand_str != "-") {
        int cnt = 0;
        for (char c : hand_str) {
            if (c >= '0' && c <= '9') {
                cnt = cnt * 10 + (c - '0');
            } else {
                if (cnt == 0) cnt = 1;
                Color    hc = std::isupper(c) ? BLACK : WHITE;
                PieceType pt = char_to_pt(static_cast<char>(std::toupper(c)));
                hand_[hc][pt] += cnt;
                cnt = 0;
            }
        }
    }

    // --- move count ---
    if (!move_cnt_str.empty()) {
        try {
            int mc = std::stoi(move_cnt_str);
            ply_ = (mc > 0) ? (mc - 1) : 0;
        } catch (...) {}
    }

    compute_hash_and_material();
    pos_hashes_.push_back(hash_);
    return true;
}

// ============================================================
// to_sfen
// ============================================================
std::string Board::to_sfen() const {
    std::string out;

    // board
    for (int rank = 0; rank < RANK_NB; rank++) {
        if (rank > 0) out += '/';
        int empty = 0;
        for (int file = 0; file < FILE_NB; file++) {
            Piece p = board_[make_sq(file, rank)];
            if (p == NO_PIECE) { empty++; continue; }
            if (empty > 0) { out += static_cast<char>('0' + empty); empty = 0; }
            PieceType pt = type_of(p);
            Color     pc = color_of(p);
            if (is_promoted(pt)) { out += '+'; pt = unpromote_pt(pt); }
            char ch = pt_to_char(pt);
            out += (pc == BLACK) ? static_cast<char>(std::toupper(ch))
                                 : static_cast<char>(std::tolower(ch));
        }
        if (empty > 0) out += static_cast<char>('0' + empty);
    }

    // side to move
    out += ' ';
    out += (stm_ == BLACK) ? 'b' : 'w';

    // hand
    out += ' ';
    static const PieceType HAND_ORDER[] = {ROOK, BISHOP, GOLD, SILVER, KNIGHT, LANCE, PAWN};
    bool any = false;
    for (Color c : {BLACK, WHITE}) {
        for (PieceType pt : HAND_ORDER) {
            int cnt = hand_[c][pt];
            if (cnt == 0) continue;
            any = true;
            if (cnt > 1) out += std::to_string(cnt);
            char ch = pt_to_char(pt);
            out += (c == BLACK) ? static_cast<char>(std::toupper(ch))
                                : static_cast<char>(std::tolower(ch));
        }
    }
    if (!any) out += '-';

    // move count
    out += ' ';
    out += std::to_string(ply_ + 1);

    return out;
}

// ============================================================
// do_move / undo_move / null_move
// ============================================================
void Board::do_move(Move m, bool record_check) {
    StateInfo si;
    si.hash           = hash_;
    si.material_black = material_black_;
    si.captured       = NO_PIECE;
    si.move           = m;

    Color us   = stm_;
    Color them = ~us;
    si.mover   = us;
    const int sign = (us == BLACK) ? 1 : -1;

    if (is_drop(m)) {
        PieceType pt = dropped_pt(m);
        Square    to = to_sq(m);

        si.hand_cnt_before = static_cast<uint8_t>(hand_[us][pt]);

        // remove from hand – update hash (material_black_ unchanged on drop)
        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];
        hand_[us][pt]--;
        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];

        // place on board
        Piece p = make_piece(us, pt);
        board_[to] = p;
        hash_ ^= Zobrist::piece_sq[p][to];
        si.moved_piece = p;
    } else {
        Square from     = from_sq(m);
        Square to       = to_sq(m);
        Piece  moving   = board_[from];
        Piece  captured = board_[to];
        PieceType orig_type = type_of(moving);

        si.captured = captured;

        // remove moving piece from source
        board_[from] = NO_PIECE;
        hash_ ^= Zobrist::piece_sq[moving][from];

        // handle capture
        if (captured != NO_PIECE) {
            hash_ ^= Zobrist::piece_sq[captured][to];
            PieceType cap_pt   = type_of(captured);
            PieceType cap_base = to_hand_pt(cap_pt);
            si.hand_cnt_before = static_cast<uint8_t>(hand_[us][cap_base]);
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
            hand_[us][cap_base]++;
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
            material_black_ += sign * (PIECE_VALUE_BONANZA[cap_pt] + PIECE_VALUE_BONANZA[cap_base]);
        }

        // handle promotion
        if (is_promote(m)) {
            PieceType prom_type = promote_pt(orig_type);
            moving = make_piece(us, prom_type);
            material_black_ += sign * (PIECE_VALUE_BONANZA[prom_type] - PIECE_VALUE_BONANZA[orig_type]);
        }

        // place on destination
        board_[to] = moving;
        hash_ ^= Zobrist::piece_sq[moving][to];
        si.moved_piece = moving;

        // Update king position cache
        if (orig_type == KING) king_sq_[us] = to;
    }

    // switch side
    hash_ ^= Zobrist::side;
    stm_  = them;
    ply_++;

    si.gave_check = record_check && in_check();
    history_.push_back(si);
    pos_hashes_.push_back(hash_);
}

void Board::undo_move(Move m) {
    Color them = stm_;
    Color us   = ~them;

    stm_  = us;
    ply_--;

    const StateInfo si = history_.back();
    history_.pop_back();
    pos_hashes_.pop_back();
    hash_           = si.hash;
    material_black_ = si.material_black;

    if (is_drop(m)) {
        PieceType pt = dropped_pt(m);
        Square    to = to_sq(m);

        board_[to] = NO_PIECE;
        hand_[us][pt]++;
    } else {
        Square from = from_sq(m);
        Square to   = to_sq(m);

        Piece     moved_now  = board_[to];
        PieceType moved_type = type_of(moved_now);

        if (is_promote(m)) moved_type = unpromote_pt(moved_type);

        board_[from] = make_piece(us, moved_type);
        if (moved_type == KING) king_sq_[us] = from;

        board_[to] = si.captured;

        if (si.captured != NO_PIECE) {
            PieceType cap_base = to_hand_pt(type_of(si.captured));
            hand_[us][cap_base]--;
        }
    }
}

void Board::do_null_move() {
    StateInfo si;
    si.hash           = hash_;
    si.material_black = material_black_;
    si.mover          = stm_;
    si.null_move      = true;

    stm_  = ~stm_;
    hash_ ^= Zobrist::side;
    ply_++;
    history_.push_back(si);
    pos_hashes_.push_back(hash_);
}

void Board::undo_null_move() {
    const StateInfo si = history_.back();
    history_.pop_back();
    pos_hashes_.pop_back();
    ply_--;
    hash_           = si.hash;
    material_black_ = si.material_black;
    stm_            = ~stm_;
}

// ============================================================
// is_attacked_ignoring(sq, by, ignore_sq)
// ============================================================
bool Board::is_attacked_ignoring(Square sq, Color by, Square ignore_sq) const {
    if (sq < 0 || sq >= SQUARE_NB) return false;
    const int f   = file_of(sq);
    const int r   = rank_of(sq);
    const int fwd = (by == BLACK) ? -1 : 1;

    // ---- Pawn ----
    {
        int pr = r - fwd;
        if (on_board(f, pr)) {
            Square s = make_sq(f, pr);
            if (s != ignore_sq && board_[s] == make_piece(by, PAWN)) return true;
        }
    }

    // ---- Knight ----
    for (int df : {-1, 1}) {
        int nf = f + df, nr = r - 2 * fwd;
        if (on_board(nf, nr)) {
            Square s = make_sq(nf, nr);
            if (s != ignore_sq && board_[s] == make_piece(by, KNIGHT)) return true;
        }
    }

    // ---- Silver / Gold / Promoted minors / King (adjacent 8 squares) ----
    const Piece by_silver = make_piece(by, SILVER);
    const Piece by_king   = make_piece(by, KING);
    for (int dr = -1; dr <= 1; ++dr) {
        int nr = r + dr;
        if (static_cast<unsigned>(nr) >= static_cast<unsigned>(RANK_NB)) continue;
        for (int df = -1; df <= 1; ++df) {
            if (df == 0 && dr == 0) continue;
            int nf = f + df;
            if (static_cast<unsigned>(nf) >= static_cast<unsigned>(FILE_NB)) continue;
            Square s = make_sq(nf, nr);
            if (s == ignore_sq) continue;
            Piece p = board_[s];
            if (p == NO_PIECE || color_of(p) != by) continue;
            if (p == by_king) return true;
            PieceType t = type_of(p);
            if (p == by_silver) {
                if (dr == -fwd || (df != 0 && dr == fwd)) return true;
            } else if (t == GOLD || t == PROM_PAWN || t == PROM_LANCE ||
                       t == PROM_KNIGHT || t == PROM_SILVER) {
                if (!(df != 0 && dr == fwd)) return true;
            } else if (t == PROM_BISHOP) {
                if (df == 0 || dr == 0) return true;
            } else if (t == PROM_ROOK) {
                if (df != 0 && dr != 0) return true;
            }
        }
    }

    // ---- Diagonal sliders (Bishop / Promoted Bishop) ----
    {
        static constexpr int DDF[4] = { 1,  1, -1, -1};
        static constexpr int DDR[4] = { 1, -1,  1, -1};
        for (int i = 0; i < 4; i++) {
            for (int nf = f + DDF[i], nr = r + DDR[i]; on_board(nf, nr);
                 nf += DDF[i], nr += DDR[i]) {
                Square s = make_sq(nf, nr);
                if (s == ignore_sq) continue;
                Piece p = board_[s];
                if (p != NO_PIECE) {
                    if (color_of(p) == by) {
                        PieceType t = type_of(p);
                        if (t == BISHOP || t == PROM_BISHOP) return true;
                    }
                    break;
                }
            }
        }
    }

    // ---- Orthogonal sliders (Rook / Promoted Rook / Lance) ----
    {
        static constexpr int ODF[4] = { 1, -1,  0,  0};
        static constexpr int ODR[4] = { 0,  0,  1, -1};
        for (int i = 0; i < 4; i++) {
            const bool lance_ray = (ODF[i] == 0 && ODR[i] == -fwd);
            for (int nf = f + ODF[i], nr = r + ODR[i]; on_board(nf, nr);
                 nf += ODF[i], nr += ODR[i]) {
                Square s = make_sq(nf, nr);
                if (s == ignore_sq) continue;
                Piece p = board_[s];
                if (p != NO_PIECE) {
                    if (color_of(p) == by) {
                        PieceType t = type_of(p);
                        if (t == ROOK || t == PROM_ROOK) return true;
                        if (lance_ray && t == LANCE) return true;
                    }
                    break;
                }
            }
        }
    }

    return false;
}

bool Board::is_attacked(Square sq, Color by) const {
    return is_attacked_ignoring(sq, by, SQ_NONE);
}

bool Board::in_check() const {
    Square ksq = king_sq_[stm_];
    if (ksq == SQ_NONE) return false;
    return is_attacked(ksq, ~stm_);
}

// ============================================================
// repetition_count / repetition_result
// ============================================================
int Board::repetition_count() const {
    int cnt = 0;
    for (uint64_t h : pos_hashes_)
        if (h == hash_) cnt++;
    return cnt;
}

RepetitionResult Board::repetition_result(bool* repetition_sensitive) const {
    if (repetition_sensitive) *repetition_sensitive = false;
    size_t occurrences[4] = {};
    size_t count = 0;
    for (size_t i = pos_hashes_.size(); i > 0 && count < 4; --i) {
        if (pos_hashes_[i - 1] == hash_) {
            if (repetition_sensitive && count > 0) *repetition_sensitive = true;
            occurrences[3 - count++] = i - 1;
        }
    }
    if (count < 4)
        return RepetitionResult::NONE;

    const size_t first = occurrences[0];
    bool moved[COLOR_NB] = {false, false};
    bool checked_every_move[COLOR_NB] = {true, true};

    for (size_t i = first; i + 1 < pos_hashes_.size(); ++i) {
        if (i >= history_.size() || history_[i].null_move)
            return RepetitionResult::NONE;
        const StateInfo& state = history_[i];
        moved[state.mover] = true;
        checked_every_move[state.mover] =
            checked_every_move[state.mover] && state.gave_check;
    }

    Color checker = BLACK;
    if (moved[BLACK] && checked_every_move[BLACK] &&
        !(moved[WHITE] && checked_every_move[WHITE])) {
        checker = BLACK;
    } else if (moved[WHITE] && checked_every_move[WHITE] &&
               !(moved[BLACK] && checked_every_move[BLACK])) {
        checker = WHITE;
    } else {
        return RepetitionResult::DRAW;
    }

    return checker == stm_ ? RepetitionResult::SIDE_TO_MOVE_LOSES
                           : RepetitionResult::SIDE_TO_MOVE_WINS;
}

// ============================================================
// print  (debug)
// ============================================================
void Board::print() const {
    std::cout << "  9 8 7 6 5 4 3 2 1\n";
    for (int rank = 0; rank < RANK_NB; rank++) {
        std::cout << static_cast<char>('a' + rank) << ' ';
        for (int file = 0; file < FILE_NB; file++) {
            Piece p = board_[make_sq(file, rank)];
            if (p == NO_PIECE) { std::cout << ". "; continue; }
            PieceType pt = type_of(p);
            bool prom = is_promoted(pt);
            if (prom) pt = unpromote_pt(pt);
            char ch = pt_to_char(pt);
            if (prom) ch = static_cast<char>(std::tolower(ch));
            if (color_of(p) == BLACK) ch = static_cast<char>(std::toupper(ch));
            std::cout << ch << ' ';
        }
        std::cout << '\n';
    }
    std::cout << "Side: " << (stm_ == BLACK ? "Black" : "White") << '\n';
}

// ============================================================
// move_to_usi / usi_to_move
// ============================================================
std::string move_to_usi(Move m) {
    if (is_drop(m)) {
        std::string s;
        s += static_cast<char>(std::toupper(pt_to_char(dropped_pt(m))));
        s += '*';
        s += sq_to_usi(to_sq(m));
        return s;
    }
    std::string s = sq_to_usi(from_sq(m)) + sq_to_usi(to_sq(m));
    if (is_promote(m)) s += '+';
    return s;
}

Move usi_to_move(const std::string& s) {
    if (s.size() >= 4 && s[1] == '*') {
        PieceType pt = char_to_pt(static_cast<char>(std::toupper(s[0])));
        Square    to = usi_to_sq(s[2], s[3]);
        return make_drop_move(pt, to);
    }
    Square from  = usi_to_sq(s[0], s[1]);
    Square to    = usi_to_sq(s[2], s[3]);
    bool   promo = (s.size() >= 5 && s[4] == '+');
    return promo ? make_promote_move(from, to) : make_normal_move(from, to);
}
