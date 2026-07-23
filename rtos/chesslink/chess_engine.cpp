#include "chess_engine.h"
#include <string.h>
#include <stdlib.h>

// =============================================================================
// attack table generation
//
// knight and king attacks are precomputed at startup into lookup tables.
// sliding piece attacks (bishop, rook, queen) use classical ray casting --
// loop outward along each ray direction, stop at first occupied square.
// no magic bitboards needed at this scale, ray casting is fast enough.
// =============================================================================

static BB knight_attacks[64];
static BB king_attacks[64];

// called once at init -- fills lookup tables
static void init_attack_tables() {
    for (int sq = 0; sq < 64; sq++) {
        int f = FILE_OF(sq);
        int r = RANK_OF(sq);
        BB bb = BB_EMPTY;

        // knight -- 8 possible L-shaped jumps, clamp to board
        const int kn_df[] = {-2,-2,-1,-1, 1, 1, 2, 2};
        const int kn_dr[] = {-1, 1,-2, 2,-2, 2,-1, 1};
        for (int i = 0; i < 8; i++) {
            int nf = f + kn_df[i];
            int nr = r + kn_dr[i];
            if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                bb |= BB_SQ(SQ(nf, nr));
        }
        knight_attacks[sq] = bb;

        // king -- 8 surrounding squares
        bb = BB_EMPTY;
        for (int df = -1; df <= 1; df++) {
            for (int dr = -1; dr <= 1; dr++) {
                if (df == 0 && dr == 0) continue;
                int nf = f + df;
                int nr = r + dr;
                if (nf >= 0 && nf < 8 && nr >= 0 && nr < 8)
                    bb |= BB_SQ(SQ(nf, nr));
            }
        }
        king_attacks[sq] = bb;
    }
}

// ray cast from sq in direction (df, dr), stopping at first blocker.
// if include_blocker is true the blocker square is included in result (captures).
static BB ray(uint8_t sq, int df, int dr, BB blockers, bool include_blocker) {
    BB result = BB_EMPTY;
    int f = FILE_OF(sq);
    int r = RANK_OF(sq);
    for (;;) {
        f += df;
        r += dr;
        if (f < 0 || f >= 8 || r < 0 || r >= 8) break;
        uint8_t s = SQ(f, r);
        if (blockers & BB_SQ(s)) {
            if (include_blocker) result |= BB_SQ(s);
            break;
        }
        result |= BB_SQ(s);
    }
    return result;
}

// rook attack rays from sq given occupancy
static BB rook_attacks(uint8_t sq, BB occ) {
    return ray(sq,  1,  0, occ, true)
         | ray(sq, -1,  0, occ, true)
         | ray(sq,  0,  1, occ, true)
         | ray(sq,  0, -1, occ, true);
}

// bishop attack rays from sq given occupancy
static BB bishop_attacks(uint8_t sq, BB occ) {
    return ray(sq,  1,  1, occ, true)
         | ray(sq, -1,  1, occ, true)
         | ray(sq,  1, -1, occ, true)
         | ray(sq, -1, -1, occ, true);
}

static BB queen_attacks(uint8_t sq, BB occ) {
    return rook_attacks(sq, occ) | bishop_attacks(sq, occ);
}

// =============================================================================
// attack detection
//
// is square sq attacked by any piece of color c in position pos?
// used for check detection and castling legality.
// =============================================================================

static bool sq_attacked_by(const Position *pos, uint8_t sq, Color c) {
    BB occ = pos->all;

    // pawns attack diagonally forward (relative to their color)
    BB pawns = pos->pieces[c][PAWN];
    if (c == WHITE) {
        // white pawns attack upward, so they attack sq from below-left/below-right
        BB pawn_atk = ((pawns << 7) & ~FILE_H) | ((pawns << 9) & ~FILE_A);
        if (pawn_atk & BB_SQ(sq)) return true;
    } else {
        BB pawn_atk = ((pawns >> 7) & ~FILE_A) | ((pawns >> 9) & ~FILE_H);
        if (pawn_atk & BB_SQ(sq)) return true;
    }

    if (knight_attacks[sq] & pos->pieces[c][KNIGHT]) return true;
    if (bishop_attacks(sq, occ) & (pos->pieces[c][BISHOP] | pos->pieces[c][QUEEN])) return true;
    if (rook_attacks(sq, occ)   & (pos->pieces[c][ROOK]   | pos->pieces[c][QUEEN])) return true;
    if (king_attacks[sq]        &  pos->pieces[c][KING])  return true;

    return false;
}

bool in_check(const Position *pos, Color c) {
    uint8_t king_sq = (uint8_t)__builtin_ctzll(pos->pieces[c][KING]);
    return sq_attacked_by(pos, king_sq, OTHER(c));
}

// =============================================================================
// FEN parsing
// =============================================================================

void pos_from_fen(Position *pos, const char *fen) {
    memset(pos, 0, sizeof(Position));
    for (int i = 0; i < 64; i++) {
        pos->board[i]    = NO_PIECE;
        pos->color_at[i] = NO_COLOR;
    }
    pos->ep_sq = -1;

    // --- piece placement ---
    int rank = 7, file = 0;
    const char *p = fen;
    while (*p && *p != ' ') {
        char c = *p++;
        if (c == '/') { rank--; file = 0; continue; }
        if (c >= '1' && c <= '8') { file += c - '0'; continue; }

        Color     col  = (c >= 'a') ? BLACK : WHITE;
        PieceType type;
        switch (c | 0x20) {  // to lowercase
            case 'p': type = PAWN;   break;
            case 'n': type = KNIGHT; break;
            case 'b': type = BISHOP; break;
            case 'r': type = ROOK;   break;
            case 'q': type = QUEEN;  break;
            case 'k': type = KING;   break;
            default:  file++; continue;
        }
        uint8_t sq = SQ(file, rank);
        pos->pieces[col][type] |= BB_SQ(sq);
        pos->board[sq]    = type;
        pos->color_at[sq] = col;
        file++;
    }

    // rebuild combined occupancy
    for (int c = 0; c < 2; c++) {
        pos->occupied[c] = BB_EMPTY;
        for (int t = 0; t < 6; t++)
            pos->occupied[c] |= pos->pieces[c][t];
    }
    pos->all = pos->occupied[WHITE] | pos->occupied[BLACK];

    // --- active color ---
    if (*p) p++;  // skip space
    pos->side = (*p == 'b') ? BLACK : WHITE;
    if (*p) p++;
    if (*p) p++;  // skip space

    // --- castling rights ---
    pos->castling = 0;
    while (*p && *p != ' ') {
        switch (*p++) {
            case 'K': pos->castling |= CASTLE_WK; break;
            case 'Q': pos->castling |= CASTLE_WQ; break;
            case 'k': pos->castling |= CASTLE_BK; break;
            case 'q': pos->castling |= CASTLE_BQ; break;
        }
    }
    if (*p) p++;  // skip space

    // --- en passant square ---
    if (*p && *p != '-') {
        int ep_file = *p++ - 'a';
        int ep_rank = *p++ - '1';
        pos->ep_sq = (int8_t)SQ(ep_file, ep_rank);
    } else {
        if (*p) p++;  // skip '-'
    }
    if (*p) p++;  // skip space

    // --- halfmove clock ---
    pos->halfmove = (uint8_t)atoi(p);
}

// =============================================================================
// FEN serialization
// =============================================================================

void pos_to_fen(const Position *pos, char *out, size_t len) {
    const char piece_chars[2][6] = {
        {'P','N','B','R','Q','K'},   // white
        {'p','n','b','r','q','k'},   // black
    };
    char *o = out;
    char *end = out + len - 1;

    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            uint8_t sq = SQ(file, rank);
            if (pos->board[sq] == NO_PIECE) {
                empty++;
            } else {
                if (empty) { if (o < end) *o++ = '0' + empty; empty = 0; }
                if (o < end) *o++ = piece_chars[pos->color_at[sq]][pos->board[sq]];
            }
        }
        if (empty) { if (o < end) *o++ = '0' + empty; }
        if (rank > 0 && o < end) *o++ = '/';
    }

    if (o < end) *o++ = ' ';
    if (o < end) *o++ = (pos->side == WHITE) ? 'w' : 'b';
    if (o < end) *o++ = ' ';

    bool any_castle = false;
    if (pos->castling & CASTLE_WK) { if (o < end) *o++ = 'K'; any_castle = true; }
    if (pos->castling & CASTLE_WQ) { if (o < end) *o++ = 'Q'; any_castle = true; }
    if (pos->castling & CASTLE_BK) { if (o < end) *o++ = 'k'; any_castle = true; }
    if (pos->castling & CASTLE_BQ) { if (o < end) *o++ = 'q'; any_castle = true; }
    if (!any_castle && o < end) *o++ = '-';

    if (o < end) *o++ = ' ';
    if (pos->ep_sq >= 0) {
        if (o < end) *o++ = 'a' + FILE_OF(pos->ep_sq);
        if (o < end) *o++ = '1' + RANK_OF(pos->ep_sq);
    } else {
        if (o < end) *o++ = '-';
    }

    // halfmove and fullmove (fullmove not tracked so just write 1)
    int written = snprintf(o, end - o, " %d 1", pos->halfmove);
    o += (written > 0) ? written : 0;
    *o = '\0';
}

// =============================================================================
// make / unmake
//
// unmake just restores the full saved Position -- simple and correct.
// we only ever need one ply deep for legality checking.
// =============================================================================

void make_move_pos(Position *pos, Move m, Position *undo) {
    *undo = *pos;  // save full state

    uint8_t   from    = MV_FROM(m);
    uint8_t   to      = MV_TO(m);
    PieceType piece   = MV_PIECE(m);
    PieceType capture = MV_CAPTURE(m);
    PieceType promo   = MV_PROMO(m);
    Color     us      = pos->side;
    Color     them    = OTHER(us);

    // remove captured piece
    if (capture != NO_PIECE && !MV_IS_EP(m)) {
        pos->pieces[them][capture] &= ~BB_SQ(to);
        pos->occupied[them]        &= ~BB_SQ(to);
        pos->board[to]              = NO_PIECE;
        pos->color_at[to]           = NO_COLOR;
    }

    // en passant capture -- remove the pawn on the rank behind the target
    if (MV_IS_EP(m)) {
        uint8_t cap_sq = (us == WHITE) ? to - 8 : to + 8;
        pos->pieces[them][PAWN] &= ~BB_SQ(cap_sq);
        pos->occupied[them]     &= ~BB_SQ(cap_sq);
        pos->board[cap_sq]       = NO_PIECE;
        pos->color_at[cap_sq]    = NO_COLOR;
    }

    // move the piece
    pos->pieces[us][piece] &= ~BB_SQ(from);
    pos->occupied[us]      &= ~BB_SQ(from);
    pos->board[from]        = NO_PIECE;
    pos->color_at[from]     = NO_COLOR;

    PieceType landing = (promo != NO_PIECE) ? promo : piece;
    pos->pieces[us][landing] |= BB_SQ(to);
    pos->occupied[us]        |= BB_SQ(to);
    pos->board[to]            = landing;
    pos->color_at[to]         = us;

    // castling -- also move the rook
    if (MV_IS_CASTLE(m)) {
        uint8_t rook_from, rook_to;
        if (to == SQ(6, 0)) { rook_from = SQ(7,0); rook_to = SQ(5,0); }  // white kingside
        else if (to == SQ(2, 0)) { rook_from = SQ(0,0); rook_to = SQ(3,0); }  // white queenside
        else if (to == SQ(6, 7)) { rook_from = SQ(7,7); rook_to = SQ(5,7); }  // black kingside
        else                     { rook_from = SQ(0,7); rook_to = SQ(3,7); }  // black queenside

        pos->pieces[us][ROOK] &= ~BB_SQ(rook_from);
        pos->occupied[us]     &= ~BB_SQ(rook_from);
        pos->board[rook_from]  = NO_PIECE;
        pos->color_at[rook_from] = NO_COLOR;

        pos->pieces[us][ROOK] |= BB_SQ(rook_to);
        pos->occupied[us]     |= BB_SQ(rook_to);
        pos->board[rook_to]    = ROOK;
        pos->color_at[rook_to] = us;
    }

    // update castling rights when king or rook moves
    if (piece == KING) {
        if (us == WHITE) pos->castling &= ~(CASTLE_WK | CASTLE_WQ);
        else             pos->castling &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (from == SQ(0,0) || to == SQ(0,0)) pos->castling &= ~CASTLE_WQ;
    if (from == SQ(7,0) || to == SQ(7,0)) pos->castling &= ~CASTLE_WK;
    if (from == SQ(0,7) || to == SQ(0,7)) pos->castling &= ~CASTLE_BQ;
    if (from == SQ(7,7) || to == SQ(7,7)) pos->castling &= ~CASTLE_BK;

    // en passant target square for double pawn push
    pos->ep_sq = -1;
    if (piece == PAWN) {
        if (us == WHITE && RANK_OF(from) == 1 && RANK_OF(to) == 3)
            pos->ep_sq = (int8_t)(from + 8);
        else if (us == BLACK && RANK_OF(from) == 6 && RANK_OF(to) == 4)
            pos->ep_sq = (int8_t)(from - 8);
    }

    // halfmove clock
    pos->halfmove = (piece == PAWN || capture != NO_PIECE) ? 0 : pos->halfmove + 1;

    // rebuild all
    pos->all = pos->occupied[WHITE] | pos->occupied[BLACK];

    pos->side = them;
}

void unmake_move_pos(Position *pos, const Position *undo) {
    *pos = *undo;
}

// =============================================================================
// pseudo-legal move generation
//
// generates moves that follow piece movement rules but may leave the king in
// check. the legal filter below applies make/in_check/unmake to discard those.
// =============================================================================

static int gen_pseudo(const Position *pos, Move *moves) {
    int cnt = 0;
    Color us   = pos->side;
    Color them = OTHER(us);
    BB my      = pos->occupied[us];
    BB opp     = pos->occupied[them];
    BB occ     = pos->all;
    BB empty   = ~occ;

    // --- pawns ---
    BB pawns = pos->pieces[us][PAWN];
    if (us == WHITE) {
        // single push
        BB push1 = (pawns << 8) & empty;
        // double push from rank 2
        BB push2 = ((push1 & RANK_3) << 8) & empty;
        // captures
        BB cap_l = ((pawns << 7) & ~FILE_H) & opp;
        BB cap_r = ((pawns << 9) & ~FILE_A) & opp;
        // en passant
        BB ep_bb = (pos->ep_sq >= 0) ? BB_SQ(pos->ep_sq) : BB_EMPTY;
        BB ep_l  = ((pawns << 7) & ~FILE_H) & ep_bb;
        BB ep_r  = ((pawns << 9) & ~FILE_A) & ep_bb;

        // helper to add pawn moves, expanding promotions
        auto add_pawn = [&](uint8_t from, uint8_t to, PieceType cap, bool ep) {
            if (RANK_OF(to) == 7) {
                // promotion -- generate all 4 options
                for (PieceType p : {QUEEN, ROOK, BISHOP, KNIGHT})
                    moves[cnt++] = make_move(from, to, PAWN, cap, p, false, false);
            } else {
                moves[cnt++] = make_move(from, to, PAWN, cap, NO_PIECE, ep, false);
            }
        };

        BB tmp;
        tmp = push1;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to-8, to, NO_PIECE, false); }
        tmp = push2;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; moves[cnt++] = make_move(to-16, to, PAWN); }
        tmp = cap_l;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to-7, to, pos->board[to], false); }
        tmp = cap_r;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to-9, to, pos->board[to], false); }
        if (ep_l) { uint8_t to = __builtin_ctzll(ep_l); moves[cnt++] = make_move(to-7, to, PAWN, PAWN, NO_PIECE, true); }
        if (ep_r) { uint8_t to = __builtin_ctzll(ep_r); moves[cnt++] = make_move(to-9, to, PAWN, PAWN, NO_PIECE, true); }

    } else {
        // black pawns -- mirror of white
        BB push1 = (pawns >> 8) & empty;
        BB push2 = ((push1 & RANK_6) >> 8) & empty;
        BB cap_l = ((pawns >> 7) & ~FILE_A) & opp;
        BB cap_r = ((pawns >> 9) & ~FILE_H) & opp;
        BB ep_bb = (pos->ep_sq >= 0) ? BB_SQ(pos->ep_sq) : BB_EMPTY;
        BB ep_l  = ((pawns >> 7) & ~FILE_A) & ep_bb;
        BB ep_r  = ((pawns >> 9) & ~FILE_H) & ep_bb;

        auto add_pawn = [&](uint8_t from, uint8_t to, PieceType cap, bool ep) {
            if (RANK_OF(to) == 0) {
                for (PieceType p : {QUEEN, ROOK, BISHOP, KNIGHT})
                    moves[cnt++] = make_move(from, to, PAWN, cap, p, false, false);
            } else {
                moves[cnt++] = make_move(from, to, PAWN, cap, NO_PIECE, ep, false);
            }
        };

        BB tmp;
        tmp = push1;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to+8, to, NO_PIECE, false); }
        tmp = push2;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; moves[cnt++] = make_move(to+16, to, PAWN); }
        tmp = cap_l;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to+7, to, pos->board[to], false); }
        tmp = cap_r;
        while (tmp) { uint8_t to = __builtin_ctzll(tmp); tmp &= tmp-1; add_pawn(to+9, to, pos->board[to], false); }
        if (ep_l) { uint8_t to = __builtin_ctzll(ep_l); moves[cnt++] = make_move(to+7, to, PAWN, PAWN, NO_PIECE, true); }
        if (ep_r) { uint8_t to = __builtin_ctzll(ep_r); moves[cnt++] = make_move(to+9, to, PAWN, PAWN, NO_PIECE, true); }
    }

    // --- knights ---
    BB knights = pos->pieces[us][KNIGHT];
    while (knights) {
        uint8_t from = __builtin_ctzll(knights); knights &= knights-1;
        BB atk = knight_attacks[from] & ~my;
        while (atk) {
            uint8_t to = __builtin_ctzll(atk); atk &= atk-1;
            moves[cnt++] = make_move(from, to, KNIGHT, pos->board[to]);
        }
    }

    // --- bishops ---
    BB bishops = pos->pieces[us][BISHOP];
    while (bishops) {
        uint8_t from = __builtin_ctzll(bishops); bishops &= bishops-1;
        BB atk = bishop_attacks(from, occ) & ~my;
        while (atk) {
            uint8_t to = __builtin_ctzll(atk); atk &= atk-1;
            moves[cnt++] = make_move(from, to, BISHOP, pos->board[to]);
        }
    }

    // --- rooks ---
    BB rooks = pos->pieces[us][ROOK];
    while (rooks) {
        uint8_t from = __builtin_ctzll(rooks); rooks &= rooks-1;
        BB atk = rook_attacks(from, occ) & ~my;
        while (atk) {
            uint8_t to = __builtin_ctzll(atk); atk &= atk-1;
            moves[cnt++] = make_move(from, to, ROOK, pos->board[to]);
        }
    }

    // --- queens ---
    BB queens = pos->pieces[us][QUEEN];
    while (queens) {
        uint8_t from = __builtin_ctzll(queens); queens &= queens-1;
        BB atk = queen_attacks(from, occ) & ~my;
        while (atk) {
            uint8_t to = __builtin_ctzll(atk); atk &= atk-1;
            moves[cnt++] = make_move(from, to, QUEEN, pos->board[to]);
        }
    }

    // --- king ---
    {
        uint8_t from = __builtin_ctzll(pos->pieces[us][KING]);
        BB atk = king_attacks[from] & ~my;
        while (atk) {
            uint8_t to = __builtin_ctzll(atk); atk &= atk-1;
            moves[cnt++] = make_move(from, to, KING, pos->board[to]);
        }

        // castling
        if (us == WHITE && from == SQ(4,0)) {
            // kingside: squares 5,6 must be empty and not attacked
            if ((pos->castling & CASTLE_WK)
                && !(occ & (BB_SQ(SQ(5,0)) | BB_SQ(SQ(6,0))))
                && !sq_attacked_by(pos, SQ(4,0), BLACK)
                && !sq_attacked_by(pos, SQ(5,0), BLACK)
                && !sq_attacked_by(pos, SQ(6,0), BLACK))
            {
                moves[cnt++] = make_move(from, SQ(6,0), KING, NO_PIECE, NO_PIECE, false, true);
            }
            // queenside: squares 3,2,1 empty, 4,3,2 not attacked
            if ((pos->castling & CASTLE_WQ)
                && !(occ & (BB_SQ(SQ(3,0)) | BB_SQ(SQ(2,0)) | BB_SQ(SQ(1,0))))
                && !sq_attacked_by(pos, SQ(4,0), BLACK)
                && !sq_attacked_by(pos, SQ(3,0), BLACK)
                && !sq_attacked_by(pos, SQ(2,0), BLACK))
            {
                moves[cnt++] = make_move(from, SQ(2,0), KING, NO_PIECE, NO_PIECE, false, true);
            }
        } else if (us == BLACK && from == SQ(4,7)) {
            if ((pos->castling & CASTLE_BK)
                && !(occ & (BB_SQ(SQ(5,7)) | BB_SQ(SQ(6,7))))
                && !sq_attacked_by(pos, SQ(4,7), WHITE)
                && !sq_attacked_by(pos, SQ(5,7), WHITE)
                && !sq_attacked_by(pos, SQ(6,7), WHITE))
            {
                moves[cnt++] = make_move(from, SQ(6,7), KING, NO_PIECE, NO_PIECE, false, true);
            }
            if ((pos->castling & CASTLE_BQ)
                && !(occ & (BB_SQ(SQ(3,7)) | BB_SQ(SQ(2,7)) | BB_SQ(SQ(1,7))))
                && !sq_attacked_by(pos, SQ(4,7), WHITE)
                && !sq_attacked_by(pos, SQ(3,7), WHITE)
                && !sq_attacked_by(pos, SQ(2,7), WHITE))
            {
                moves[cnt++] = make_move(from, SQ(2,7), KING, NO_PIECE, NO_PIECE, false, true);
            }
        }
    }

    return cnt;
}

// =============================================================================
// legal move generation -- filter pseudo-legal moves by checking for self-check
// =============================================================================

// rank 3 and 6 needed for pawn double push -- define here since not in header
#define RANK_3  0x0000000000FF0000ULL
#define RANK_6  0x0000FF0000000000ULL

int gen_legal_moves(const Position *pos, Move *moves) {
    Move pseudo[MAX_MOVES];
    int  pcnt = gen_pseudo(pos, pseudo);
    int  lcnt = 0;
    Position undo;

    for (int i = 0; i < pcnt; i++) {
        make_move_pos((Position*)pos, pseudo[i], &undo);
        // after the move, pos->side has flipped -- check if the mover's king is safe
        if (!in_check(pos, OTHER(pos->side)))
            moves[lcnt++] = pseudo[i];
        unmake_move_pos((Position*)pos, &undo);
    }
    return lcnt;
}

int gen_legal_moves_from(const Position *pos, uint8_t from_sq, Move *moves) {
    Move all[MAX_MOVES];
    int  cnt  = gen_legal_moves(pos, all);
    int  lcnt = 0;
    for (int i = 0; i < cnt; i++) {
        if (MV_FROM(all[i]) == from_sq)
            moves[lcnt++] = all[i];
    }
    return lcnt;
}

BB legal_destinations(const Position *pos, uint8_t from_sq) {
    Move moves[MAX_MOVES];
    int  cnt = gen_legal_moves_from(pos, from_sq, moves);
    BB   dest = BB_EMPTY;
    for (int i = 0; i < cnt; i++)
        dest |= BB_SQ(MV_TO(moves[i]));
    return dest;
}

bool is_legal(const Position *pos, uint8_t from_sq, uint8_t to_sq) {
    return (legal_destinations(pos, from_sq) & BB_SQ(to_sq)) != 0;
}

// =============================================================================
// UCI helpers
// =============================================================================

void move_to_uci(Move m, char *out) {
    uint8_t from = MV_FROM(m);
    uint8_t to   = MV_TO(m);
    out[0] = 'a' + FILE_OF(from);
    out[1] = '1' + RANK_OF(from);
    out[2] = 'a' + FILE_OF(to);
    out[3] = '1' + RANK_OF(to);
    if (MV_IS_PROMO(m)) {
        const char promo_chars[] = {'p','n','b','r','q','k'};
        out[4] = promo_chars[MV_PROMO(m)];
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

Move uci_to_move(const Position *pos, const char *uci) {
    if (!uci || strlen(uci) < 4) return MOVE_NONE;

    uint8_t from = SQ(uci[0]-'a', uci[1]-'1');
    uint8_t to   = SQ(uci[2]-'a', uci[3]-'1');

    // handle promotion piece
    PieceType promo = NO_PIECE;
    if (uci[4]) {
        switch (uci[4]) {
            case 'q': promo = QUEEN;  break;
            case 'r': promo = ROOK;   break;
            case 'b': promo = BISHOP; break;
            case 'n': promo = KNIGHT; break;
        }
    }

    // find the matching legal move (handles ep/castle flags automatically)
    Move moves[MAX_MOVES];
    int  cnt = gen_legal_moves(pos, moves);
    for (int i = 0; i < cnt; i++) {
        if (MV_FROM(moves[i]) == from && MV_TO(moves[i]) == to) {
            // for promotions, also match piece type
            if (promo != NO_PIECE && MV_PROMO(moves[i]) != promo) continue;
            return moves[i];
        }
    }
    return MOVE_NONE;
}

// =============================================================================
// one-time init -- call from setup() before any engine functions
// =============================================================================

void chess_engine_init() {
    init_attack_tables();
}
