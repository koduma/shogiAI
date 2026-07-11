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
Board::Board() { reset(); }

void Board::reset() {
    std::memset(board_, 0, sizeof(board_));
    std::memset(hand_,  0, sizeof(hand_));
    stm_  = BLACK;
    ply_  = 0;
    hash_ = 0;
    king_sq_[BLACK] = SQ_NONE;
    king_sq_[WHITE] = SQ_NONE;
    history_.clear();
    pos_hashes_.clear();
}

// ============================================================
// Zobrist hash recomputation from scratch
// ============================================================
void Board::compute_hash() {
    hash_ = 0;
    for (int s = 0; s < SQUARE_NB; s++) {
        if (board_[s] != NO_PIECE)
            hash_ ^= Zobrist::piece_sq[board_[s]][s];
    }
    for (int c = 0; c < COLOR_NB; c++)
        for (int pt = 0; pt < PT_NB; pt++)
            hash_ ^= Zobrist::hand[c][pt][hand_[c][pt]];
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

    compute_hash();
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
// do_move
// ============================================================
void Board::do_move(Move m) {
    StateInfo si;
    si.hash     = hash_;
    si.captured = NO_PIECE;

    Color us   = stm_;
    Color them = ~us;

    if (is_drop(m)) {
        PieceType pt = dropped_pt(m);
        Square    to = to_sq(m);

        // remove from hand – update hash
        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];
        hand_[us][pt]--;
        hash_ ^= Zobrist::hand[us][pt][hand_[us][pt]];

        // place on board
        Piece p = make_piece(us, pt);
        board_[to] = p;
        hash_ ^= Zobrist::piece_sq[p][to];
    } else {
        Square from     = from_sq(m);
        Square to       = to_sq(m);
        Piece  moving   = board_[from];
        Piece  captured = board_[to];

        si.captured = captured;

        // remove moving piece from source
        board_[from] = NO_PIECE;
        hash_ ^= Zobrist::piece_sq[moving][from];

        // handle capture
        if (captured != NO_PIECE) {
            hash_ ^= Zobrist::piece_sq[captured][to];
            PieceType cap_base = to_hand_pt(type_of(captured));
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
            hand_[us][cap_base]++;
            hash_ ^= Zobrist::hand[us][cap_base][hand_[us][cap_base]];
        }

        // handle promotion
        if (is_promote(m)) {
            moving = make_piece(us, promote_pt(type_of(moving)));
        }

        // place on destination
        board_[to] = moving;
        hash_ ^= Zobrist::piece_sq[moving][to];

        // Update king position cache.
        // Recover the original piece type (before promotion) to identify king moves.
        PieceType orig_type = is_promote(m) ? unpromote_pt(type_of(moving)) : type_of(moving);
        if (orig_type == KING) king_sq_[us] = to;
    }

    // switch side
    hash_ ^= Zobrist::side;
    stm_  = them;
    ply_++;

    history_.push_back(si);
    pos_hashes_.push_back(hash_);
}

// ============================================================
// undo_move
// ============================================================
void Board::undo_move(Move m) {
    Color them = stm_;         // side that WILL move next (= side that just moved = ~them below)
    Color us   = ~them;        // side that made the move we are undoing

    stm_  = us;
    ply_--;

    StateInfo si = history_.back();
    history_.pop_back();
    pos_hashes_.pop_back();
    hash_ = si.hash;  // restore hash wholesale

    if (is_drop(m)) {
        PieceType pt = dropped_pt(m);
        Square    to = to_sq(m);

        board_[to] = NO_PIECE;
        hand_[us][pt]++;
    } else {
        Square from = from_sq(m);
        Square to   = to_sq(m);

        Piece    moved_now  = board_[to];
        PieceType moved_type = type_of(moved_now);

        // undo promotion
        if (is_promote(m)) moved_type = unpromote_pt(moved_type);

        // restore piece at source
        board_[from] = make_piece(us, moved_type);
        if (moved_type == KING) king_sq_[us] = from;

        // restore destination
        board_[to] = si.captured;

        // remove captured piece from hand
        if (si.captured != NO_PIECE) {
            PieceType cap_base = to_hand_pt(type_of(si.captured));
            hand_[us][cap_base]--;
        }
    }
}

// ============================================================
// is_attacked(sq, by)
//   Returns true if any piece of color 'by' can capture on square 'sq'.
// ============================================================
bool Board::is_attacked(Square sq, Color by) const {
    const int f   = file_of(sq);
    const int r   = rank_of(sq);
    const int fwd = (by == BLACK) ? -1 : 1;   // rank-delta for 'by's forward

    // ---- Pawn ----
    // by's pawn at (f, r - fwd) attacks sq by moving forward (+fwd).
    {
        int pr = r - fwd;
        if (on_board(f, pr)) {
            Piece p = board_[make_sq(f, pr)];
            if (p == make_piece(by, PAWN)) return true;
        }
    }

    // ---- Lance (sliding forward) ----
    // by's lance attacks from behind sq (in the -fwd direction from sq).
    {
        for (int nr = r - fwd; on_board(f, nr); nr -= fwd) {
            Piece p = board_[make_sq(f, nr)];
            if (p != NO_PIECE) {
                if (p == make_piece(by, LANCE)) return true;
                break;
            }
        }
    }

    // ---- Knight ----
    // by's knight at (f ± 1, r - 2*fwd) attacks sq.
    for (int df : {-1, 1}) {
        int nf = f + df, nr = r - 2 * fwd;
        if (on_board(nf, nr) && board_[make_sq(nf, nr)] == make_piece(by, KNIGHT))
            return true;
    }

    // ---- Silver ----
    // Positions where by's silver can be to attack sq:
    //   (f,    r - fwd),   (f±1, r - fwd),   (f±1, r + fwd)
    {
        Piece sv = make_piece(by, SILVER);
        auto chk = [&](int df, int dr) {
            int nf = f+df, nr = r+dr;
            return on_board(nf,nr) && board_[make_sq(nf,nr)] == sv;
        };
        if (chk( 0, -fwd) || chk( 1, -fwd) || chk(-1, -fwd) ||
            chk( 1,  fwd) || chk(-1,  fwd)) return true;
    }

    // ---- Gold & gold-equivalent promoted pieces ----
    // Positions: (f, r-fwd), (f±1, r-fwd), (f±1, r), (f, r+fwd)
    {
        auto is_gold_mover = [&](Piece p) -> bool {
            if (p == NO_PIECE || color_of(p) != by) return false;
            PieceType t = type_of(p);
            return t == GOLD || t == PROM_PAWN || t == PROM_LANCE ||
                   t == PROM_KNIGHT || t == PROM_SILVER;
        };
        int cks[6][2] = {{0,-fwd},{1,-fwd},{-1,-fwd},{1,0},{-1,0},{0,fwd}};
        for (auto& ck : cks) {
            int nf = f+ck[0], nr = r+ck[1];
            if (on_board(nf,nr) && is_gold_mover(board_[make_sq(nf,nr)])) return true;
        }
    }

    // ---- Bishop / Promoted-Bishop (diagonal sliding) ----
    // Promoted-bishop also has a 1-step orthogonal move (caught in rook scan below).
    {
        const int DDF[] = { 1,  1, -1, -1};
        const int DDR[] = { 1, -1,  1, -1};
        for (int i = 0; i < 4; i++) {
            bool first = true;
            for (int nf = f+DDF[i], nr = r+DDR[i]; on_board(nf,nr);
                 nf += DDF[i], nr += DDR[i]) {
                Piece p = board_[make_sq(nf,nr)];
                if (p != NO_PIECE) {
                    if (color_of(p) == by) {
                        PieceType t = type_of(p);
                        if (t == BISHOP || t == PROM_BISHOP) return true;
                        // Prom-Rook can step 1 diagonal
                        if (first && t == PROM_ROOK) return true;
                    }
                    break;
                }
                first = false;
            }
        }
    }

    // ---- Rook / Promoted-Rook (orthogonal sliding) ----
    // Promoted-rook also has a 1-step diagonal move (caught in bishop scan above).
    {
        const int ODF[] = { 1, -1,  0,  0};
        const int ODR[] = { 0,  0,  1, -1};
        for (int i = 0; i < 4; i++) {
            bool first = true;
            for (int nf = f+ODF[i], nr = r+ODR[i]; on_board(nf,nr);
                 nf += ODF[i], nr += ODR[i]) {
                Piece p = board_[make_sq(nf,nr)];
                if (p != NO_PIECE) {
                    if (color_of(p) == by) {
                        PieceType t = type_of(p);
                        if (t == ROOK || t == PROM_ROOK) return true;
                        // Prom-Bishop can step 1 orthogonal
                        if (first && t == PROM_BISHOP) return true;
                    }
                    break;
                }
                first = false;
            }
        }
    }

    // ---- King ----
    for (int df = -1; df <= 1; df++) for (int dr = -1; dr <= 1; dr++) {
        if (df == 0 && dr == 0) continue;
        int nf = f+df, nr = r+dr;
        if (on_board(nf,nr) && board_[make_sq(nf,nr)] == make_piece(by, KING))
            return true;
    }

    return false;
}

bool Board::in_check() const {
    return is_attacked(king_sq_[stm_], ~stm_);
}

// ============================================================
// repetition_count
//   Count how many times the current position has appeared.
// ============================================================
int Board::repetition_count() const {
    int cnt = 0;
    for (uint64_t h : pos_hashes_)
        if (h == hash_) cnt++;
    return cnt;
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
            if (prom) ch = static_cast<char>(std::tolower(ch)); // lowercase = promoted
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
        // Drop: "P*5e"
        PieceType pt = char_to_pt(static_cast<char>(std::toupper(s[0])));
        Square    to = usi_to_sq(s[2], s[3]);
        return make_drop_move(pt, to);
    }
    // Normal / promote: "7g7f" or "7g7f+"
    Square from  = usi_to_sq(s[0], s[1]);
    Square to    = usi_to_sq(s[2], s[3]);
    bool   promo = (s.size() >= 5 && s[4] == '+');
    return promo ? make_promote_move(from, to) : make_normal_move(from, to);
}
