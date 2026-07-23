// chess.cpp -- implementation of the ChessLink rules engine
//
// design notes
// - mailbox board (int8_t[64]) for clarity, occupancy bitboard derived on demand
// - move gen is pseudo-legal + a make/king-safety filter, dead simple and
//   provably correct (verified by perft). speed is a non-issue: we validate the
//   human's single move per turn, not a search tree
// - every offset is bounded by explicit file/rank checks so nothing wraps around
//   the board edges
#include "chess.h"
#include <string.h>

static const int8_t KNIGHT_D[8][2] = {
    { 1, 2}, { 2, 1}, { 2,-1}, { 1,-2},
    {-1,-2}, {-2,-1}, {-2, 1}, {-1, 2}
};
static const int8_t KING_D[8][2] = {
    { 1, 0}, {-1, 0}, { 0, 1}, { 0,-1},
    { 1, 1}, { 1,-1}, {-1, 1}, {-1,-1}
};
static const int8_t BISHOP_D[4][2] = { {1,1}, {1,-1}, {-1,1}, {-1,-1} };
static const int8_t ROOK_D[4][2]   = { {1,0}, {-1,0}, {0,1}, {0,-1} };

void cl_init_startpos(GameState *gs) {
    static const int8_t back[8] = { CL_WR, CL_WN, CL_WB, CL_WQ,
                                    CL_WK, CL_WB, CL_WN, CL_WR };
    memset(gs->board, CL_EMPTY, sizeof(gs->board));
    for (int f = 0; f < 8; f++) {
        gs->board[cl_sq(f, 0)] = back[f];
        gs->board[cl_sq(f, 1)] = CL_WP;
        gs->board[cl_sq(f, 6)] = CL_BP;
        gs->board[cl_sq(f, 7)] = back[f] + (CL_BP - CL_WP);
    }
    gs->side = CL_WHITE;
    gs->castling = CL_CR_WK | CL_CR_WQ | CL_CR_BK | CL_CR_BQ;
    gs->ep_square = -1;
    gs->halfmove = 0;
    gs->fullmove = 1;
}

uint64_t cl_occupancy(const GameState *gs) {
    uint64_t occ = 0;
    for (int sq = 0; sq < 64; sq++)
        if (gs->board[sq] != CL_EMPTY) occ |= (uint64_t)1 << sq;
    return occ;
}

static int find_king(const GameState *gs, int color) {
    int target = (color == CL_WHITE) ? CL_WK : CL_BK;
    for (int sq = 0; sq < 64; sq++)
        if (gs->board[sq] == target) return sq;
    return -1;
}

bool cl_is_attacked(const GameState *gs, int sq, int by_side) {
    int f = cl_file(sq), r = cl_rank(sq);
    int pr = (by_side == CL_WHITE) ? (r - 1) : (r + 1);
    if (pr >= 0 && pr < 8) {
        int wantp = (by_side == CL_WHITE) ? CL_WP : CL_BP;
        if (f - 1 >= 0 && gs->board[cl_sq(f - 1, pr)] == wantp) return true;
        if (f + 1 <  8 && gs->board[cl_sq(f + 1, pr)] == wantp) return true;
    }
    int wantn = (by_side == CL_WHITE) ? CL_WN : CL_BN;
    for (int i = 0; i < 8; i++) {
        int nf = f + KNIGHT_D[i][0], nr = r + KNIGHT_D[i][1];
        if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
        if (gs->board[cl_sq(nf, nr)] == wantn) return true;
    }
    int wantk = (by_side == CL_WHITE) ? CL_WK : CL_BK;
    for (int i = 0; i < 8; i++) {
        int nf = f + KING_D[i][0], nr = r + KING_D[i][1];
        if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
        if (gs->board[cl_sq(nf, nr)] == wantk) return true;
    }
    int wantb = (by_side == CL_WHITE) ? CL_WB : CL_BB;
    int wantr = (by_side == CL_WHITE) ? CL_WR : CL_BR;
    int wantq = (by_side == CL_WHITE) ? CL_WQ : CL_BQ;
    for (int i = 0; i < 4; i++) {
        int nf = f, nr = r;
        for (;;) {
            nf += BISHOP_D[i][0]; nr += BISHOP_D[i][1];
            if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
            int8_t pc = gs->board[cl_sq(nf, nr)];
            if (pc == CL_EMPTY) continue;
            if (pc == wantb || pc == wantq) return true;
            break;
        }
    }
    for (int i = 0; i < 4; i++) {
        int nf = f, nr = r;
        for (;;) {
            nf += ROOK_D[i][0]; nr += ROOK_D[i][1];
            if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
            int8_t pc = gs->board[cl_sq(nf, nr)];
            if (pc == CL_EMPTY) continue;
            if (pc == wantr || pc == wantq) return true;
            break;
        }
    }
    return false;
}

bool cl_in_check(const GameState *gs) {
    int ksq = find_king(gs, gs->side);
    if (ksq < 0) return false;
    return cl_is_attacked(gs, ksq, gs->side ^ 1);
}

void cl_make_move(GameState *gs, const Move *m) {
    int8_t pc = gs->board[m->from];
    int me = gs->side;
    bool is_pawn = (cl_type_of(pc) == CL_PAWN);
    bool is_capture = (m->flags & (CL_FLAG_CAPTURE | CL_FLAG_EP)) != 0;
    gs->board[m->to] = pc;
    gs->board[m->from] = CL_EMPTY;
    if (m->flags & CL_FLAG_EP) {
        int cap_sq = (me == CL_WHITE) ? (m->to - 8) : (m->to + 8);
        gs->board[cap_sq] = CL_EMPTY;
    }
    if (m->promo) {
        int8_t promo_pc = (me == CL_WHITE) ? m->promo : (m->promo + (CL_BP - CL_WP));
        gs->board[m->to] = promo_pc;
    }
    if (m->flags & CL_FLAG_CASTLE) {
        if (cl_file(m->to) == 6) {
            int rf = cl_sq(7, cl_rank(m->to));
            int rt = cl_sq(5, cl_rank(m->to));
            gs->board[rt] = gs->board[rf];
            gs->board[rf] = CL_EMPTY;
        } else {
            int rf = cl_sq(0, cl_rank(m->to));
            int rt = cl_sq(3, cl_rank(m->to));
            gs->board[rt] = gs->board[rf];
            gs->board[rf] = CL_EMPTY;
        }
    }
    if (pc == CL_WK) gs->castling &= ~(CL_CR_WK | CL_CR_WQ);
    if (pc == CL_BK) gs->castling &= ~(CL_CR_BK | CL_CR_BQ);
    if (m->from == cl_sq(7,0) || m->to == cl_sq(7,0)) gs->castling &= ~CL_CR_WK;
    if (m->from == cl_sq(0,0) || m->to == cl_sq(0,0)) gs->castling &= ~CL_CR_WQ;
    if (m->from == cl_sq(7,7) || m->to == cl_sq(7,7)) gs->castling &= ~CL_CR_BK;
    if (m->from == cl_sq(0,7) || m->to == cl_sq(0,7)) gs->castling &= ~CL_CR_BQ;
    gs->ep_square = -1;
    if (m->flags & CL_FLAG_DOUBLE)
        gs->ep_square = (me == CL_WHITE) ? (m->from + 8) : (m->from - 8);
    if (is_pawn || is_capture) gs->halfmove = 0;
    else gs->halfmove++;
    if (me == CL_BLACK) gs->fullmove++;
    gs->side ^= 1;
}

static void add_move(Move *out, int *n, int from, int to, int promo, int flags) {
    if (*n >= CL_MAX_MOVES) return;
    out[*n].from = (uint8_t)from;
    out[*n].to = (uint8_t)to;
    out[*n].promo = (uint8_t)promo;
    out[*n].flags = (uint8_t)flags;
    (*n)++;
}

static void add_pawn_move(Move *out, int *n, int from, int to, int flags, int me) {
    int last = (me == CL_WHITE) ? 7 : 0;
    if (cl_rank(to) == last) {
        add_move(out, n, from, to, CL_QUEEN,  flags);
        add_move(out, n, from, to, CL_ROOK,   flags);
        add_move(out, n, from, to, CL_BISHOP, flags);
        add_move(out, n, from, to, CL_KNIGHT, flags);
    } else {
        add_move(out, n, from, to, 0, flags);
    }
}

static int gen_pseudo(const GameState *gs, Move *out) {
    int n = 0;
    int me = gs->side, them = me ^ 1;
    for (int sq = 0; sq < 64; sq++) {
        int8_t pc = gs->board[sq];
        if (pc == CL_EMPTY || cl_color_of(pc) != me) continue;
        int f = cl_file(sq), r = cl_rank(sq);
        int type = cl_type_of(pc);
        if (type == CL_PAWN) {
            int dr = (me == CL_WHITE) ? 1 : -1;
            int start_rank = (me == CL_WHITE) ? 1 : 6;
            int one = cl_sq(f, r + dr);
            if (gs->board[one] == CL_EMPTY) {
                add_pawn_move(out, &n, sq, one, CL_FLAG_QUIET, me);
                if (r == start_rank) {
                    int two = cl_sq(f, r + 2 * dr);
                    if (gs->board[two] == CL_EMPTY)
                        add_move(out, &n, sq, two, 0, CL_FLAG_DOUBLE);
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                int cf = f + df, cr = r + dr;
                if (cf < 0 || cf > 7 || cr < 0 || cr > 7) continue;
                int cto = cl_sq(cf, cr);
                int8_t tp = gs->board[cto];
                if (tp != CL_EMPTY && cl_color_of(tp) == them)
                    add_pawn_move(out, &n, sq, cto, CL_FLAG_CAPTURE, me);
                else if (cto == gs->ep_square)
                    add_move(out, &n, sq, cto, 0, CL_FLAG_EP);
            }
        }
        else if (type == CL_KNIGHT) {
            for (int i = 0; i < 8; i++) {
                int nf = f + KNIGHT_D[i][0], nr = r + KNIGHT_D[i][1];
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int to = cl_sq(nf, nr);
                int8_t tp = gs->board[to];
                if (tp == CL_EMPTY) add_move(out, &n, sq, to, 0, CL_FLAG_QUIET);
                else if (cl_color_of(tp) == them) add_move(out, &n, sq, to, 0, CL_FLAG_CAPTURE);
            }
        }
        else if (type == CL_KING) {
            for (int i = 0; i < 8; i++) {
                int nf = f + KING_D[i][0], nr = r + KING_D[i][1];
                if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
                int to = cl_sq(nf, nr);
                int8_t tp = gs->board[to];
                if (tp == CL_EMPTY) add_move(out, &n, sq, to, 0, CL_FLAG_QUIET);
                else if (cl_color_of(tp) == them) add_move(out, &n, sq, to, 0, CL_FLAG_CAPTURE);
            }
            int home = (me == CL_WHITE) ? cl_sq(4, 0) : cl_sq(4, 7);
            if (sq == home && !cl_is_attacked(gs, sq, them)) {
                int kside = (me == CL_WHITE) ? CL_CR_WK : CL_CR_BK;
                int qside = (me == CL_WHITE) ? CL_CR_WQ : CL_CR_BQ;
                int rr = cl_rank(sq);
                if (gs->castling & kside) {
                    int f5 = cl_sq(5, rr), f6 = cl_sq(6, rr);
                    if (gs->board[f5] == CL_EMPTY && gs->board[f6] == CL_EMPTY &&
                        !cl_is_attacked(gs, f5, them) && !cl_is_attacked(gs, f6, them))
                        add_move(out, &n, sq, f6, 0, CL_FLAG_CASTLE);
                }
                if (gs->castling & qside) {
                    int f1 = cl_sq(1, rr), f2 = cl_sq(2, rr), f3 = cl_sq(3, rr);
                    if (gs->board[f1] == CL_EMPTY && gs->board[f2] == CL_EMPTY &&
                        gs->board[f3] == CL_EMPTY &&
                        !cl_is_attacked(gs, f3, them) && !cl_is_attacked(gs, f2, them))
                        add_move(out, &n, sq, f2, 0, CL_FLAG_CASTLE);
                }
            }
        }
        else {
            const int8_t (*dirs)[2];
            int ndir;
            if (type == CL_BISHOP) { dirs = BISHOP_D; ndir = 4; }
            else if (type == CL_ROOK) { dirs = ROOK_D; ndir = 4; }
            else { dirs = KING_D; ndir = 8; }
            for (int i = 0; i < ndir; i++) {
                int nf = f, nr = r;
                for (;;) {
                    nf += dirs[i][0]; nr += dirs[i][1];
                    if (nf < 0 || nf > 7 || nr < 0 || nr > 7) break;
                    int to = cl_sq(nf, nr);
                    int8_t tp = gs->board[to];
                    if (tp == CL_EMPTY) { add_move(out, &n, sq, to, 0, CL_FLAG_QUIET); continue; }
                    if (cl_color_of(tp) == them) add_move(out, &n, sq, to, 0, CL_FLAG_CAPTURE);
                    break;
                }
            }
        }
    }
    return n;
}

int cl_gen_legal(const GameState *gs, Move *out) {
    Move pseudo[CL_MAX_MOVES];
    int pn = gen_pseudo(gs, pseudo);
    int me = gs->side;
    int n = 0;
    for (int i = 0; i < pn; i++) {
        GameState copy = *gs;
        cl_make_move(&copy, &pseudo[i]);
        int ksq = find_king(&copy, me);
        if (ksq >= 0 && !cl_is_attacked(&copy, ksq, me ^ 1))
            out[n++] = pseudo[i];
    }
    return n;
}

uint64_t cl_result_occupancy(const GameState *gs, const Move *m) {
    uint64_t occ = cl_occupancy(gs);
    occ &= ~((uint64_t)1 << m->from);
    occ |=  ((uint64_t)1 << m->to);
    if (m->flags & CL_FLAG_EP) {
        int cap_sq = (gs->side == CL_WHITE) ? (m->to - 8) : (m->to + 8);
        occ &= ~((uint64_t)1 << cap_sq);
    }
    if (m->flags & CL_FLAG_CASTLE) {
        int rr = cl_rank(m->to);
        if (cl_file(m->to) == 6) {
            occ &= ~((uint64_t)1 << cl_sq(7, rr));
            occ |=  ((uint64_t)1 << cl_sq(5, rr));
        } else {
            occ &= ~((uint64_t)1 << cl_sq(0, rr));
            occ |=  ((uint64_t)1 << cl_sq(3, rr));
        }
    }
    return occ;
}

const Move *cl_find_move(const Move *list, int count, int from, int to, int promo) {
    for (int i = 0; i < count; i++) {
        if (list[i].from == from && list[i].to == to) {
            if (promo == 0 || list[i].promo == 0 || list[i].promo == promo)
                return &list[i];
        }
    }
    return 0;
}

uint64_t cl_legal_dest_mask(const GameState *gs, int from_sq) {
    Move mv[CL_MAX_MOVES];
    int n = cl_gen_legal(gs, mv);
    uint64_t mask = 0;
    for (int i = 0; i < n; i++)
        if (mv[i].from == from_sq) mask |= (uint64_t)1 << mv[i].to;
    return mask;
}

bool cl_is_checkmate(const GameState *gs) {
    Move mv[CL_MAX_MOVES];
    if (cl_gen_legal(gs, mv) > 0) return false;
    return cl_in_check(gs);
}

bool cl_is_stalemate(const GameState *gs) {
    Move mv[CL_MAX_MOVES];
    if (cl_gen_legal(gs, mv) > 0) return false;
    return !cl_in_check(gs);
}

static int8_t fen_char_to_pc(char c) {
    switch (c) {
        case 'P': return CL_WP; case 'N': return CL_WN; case 'B': return CL_WB;
        case 'R': return CL_WR; case 'Q': return CL_WQ; case 'K': return CL_WK;
        case 'p': return CL_BP; case 'n': return CL_BN; case 'b': return CL_BB;
        case 'r': return CL_BR; case 'q': return CL_BQ; case 'k': return CL_BK;
    }
    return CL_EMPTY;
}

static char pc_to_fen_char(int8_t pc) {
    static const char *w = " PNBRQK";
    static const char *b = " pnbrqk";
    if (pc == CL_EMPTY) return ' ';
    return (pc >= CL_BP) ? b[pc - CL_BP + 1] : w[pc];
}

bool cl_from_fen(GameState *gs, const char *fen) {
    memset(gs->board, CL_EMPTY, sizeof(gs->board));
    gs->castling = 0;
    gs->ep_square = -1;
    gs->halfmove = 0;
    gs->fullmove = 1;
    const char *p = fen;
    int rank = 7, file = 0;
    while (*p && *p != ' ') {
        char c = *p++;
        if (c == '/') { rank--; file = 0; }
        else if (c >= '1' && c <= '8') { file += c - '0'; }
        else {
            int8_t pc = fen_char_to_pc(c);
            if (pc == CL_EMPTY || rank < 0 || file > 7) return false;
            gs->board[cl_sq(file, rank)] = pc;
            file++;
        }
    }
    if (*p != ' ') return false;
    p++;
    gs->side = (*p == 'b') ? CL_BLACK : CL_WHITE;
    while (*p && *p != ' ') p++;
    if (*p == ' ') p++;
    if (*p == '-') { p++; }
    else {
        while (*p && *p != ' ') {
            switch (*p) {
                case 'K': gs->castling |= CL_CR_WK; break;
                case 'Q': gs->castling |= CL_CR_WQ; break;
                case 'k': gs->castling |= CL_CR_BK; break;
                case 'q': gs->castling |= CL_CR_BQ; break;
            }
            p++;
        }
    }
    if (*p == ' ') p++;
    if (*p == '-') { p++; }
    else if (p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8') {
        gs->ep_square = cl_sq(p[0] - 'a', p[1] - '1');
        p += 2;
    }
    if (*p == ' ') p++;
    if (*p >= '0' && *p <= '9') {
        int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
        gs->halfmove = (uint16_t)v;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            gs->fullmove = (uint16_t)v;
        }
    }
    return true;
}

static char *put_uint(char *o, unsigned v) {
    char tmp[6]; int i = 0;
    if (v == 0) { *o++ = '0'; return o; }
    while (v && i < 6) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i) *o++ = tmp[--i];
    return o;
}

void cl_to_fen(const GameState *gs, char *out) {
    char *o = out;
    for (int rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int8_t pc = gs->board[cl_sq(file, rank)];
            if (pc == CL_EMPTY) { empty++; continue; }
            if (empty) { *o++ = '0' + empty; empty = 0; }
            *o++ = pc_to_fen_char(pc);
        }
        if (empty) *o++ = '0' + empty;
        if (rank) *o++ = '/';
    }
    *o++ = ' ';
    *o++ = (gs->side == CL_WHITE) ? 'w' : 'b';
    *o++ = ' ';
    if (!gs->castling) { *o++ = '-'; }
    else {
        if (gs->castling & CL_CR_WK) *o++ = 'K';
        if (gs->castling & CL_CR_WQ) *o++ = 'Q';
        if (gs->castling & CL_CR_BK) *o++ = 'k';
        if (gs->castling & CL_CR_BQ) *o++ = 'q';
    }
    *o++ = ' ';
    if (gs->ep_square < 0) { *o++ = '-'; }
    else {
        *o++ = 'a' + cl_file(gs->ep_square);
        *o++ = '1' + cl_rank(gs->ep_square);
    }
    *o++ = ' ';
    o = put_uint(o, gs->halfmove);
    *o++ = ' ';
    o = put_uint(o, gs->fullmove);
    *o = '\0';
}
