// chess.h -- self-contained chess rules engine for ChessLink
//
// full legal move generation, FEN in/out, make_move, attack tests
// no dynamic allocation, no arduino deps, fits comfortably on ESP32
// squares are 0..63, a1 = 0, b1 = 1, ... h8 = 63 (rank*8 + file)
// bit i of a uint64_t occupancy word == square i occupied

#ifndef CHESSLINK_CHESS_H
#define CHESSLINK_CHESS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- constants --------------------------------------------------------------

#define CL_WHITE 0
#define CL_BLACK 1

// piece codes stored in board[64], 0 = empty
enum {
    CL_EMPTY = 0,
    CL_WP, CL_WN, CL_WB, CL_WR, CL_WQ, CL_WK,   // white 1..6
    CL_BP, CL_BN, CL_BB, CL_BR, CL_BQ, CL_BK    // black 7..12
};

// piece types (color-stripped), used for promotion codes too
enum { CL_PAWN = 1, CL_KNIGHT, CL_BISHOP, CL_ROOK, CL_QUEEN, CL_KING };

// castling right bits
#define CL_CR_WK 1
#define CL_CR_WQ 2
#define CL_CR_BK 4
#define CL_CR_BQ 8

// move flags
#define CL_FLAG_QUIET   0x00
#define CL_FLAG_CAPTURE 0x01
#define CL_FLAG_DOUBLE  0x02   // pawn double push, sets ep target
#define CL_FLAG_EP      0x04   // en passant capture
#define CL_FLAG_CASTLE  0x08   // king-side or queen-side, decode from to-square

#define CL_MAX_MOVES 256

// --- types ------------------------------------------------------------------

typedef struct {
    uint8_t from;    // 0..63
    uint8_t to;      // 0..63
    uint8_t promo;   // 0 = none, else CL_KNIGHT..CL_QUEEN
    uint8_t flags;   // CL_FLAG_*
} Move;

typedef struct {
    int8_t   board[64];     // piece code per square
    uint8_t  side;          // CL_WHITE / CL_BLACK, side to move
    uint8_t  castling;      // CL_CR_* bitmask
    int8_t   ep_square;     // en passant target square, -1 if none
    uint16_t halfmove;      // halfmove clock (50-move rule)
    uint16_t fullmove;      // fullmove number, starts at 1
} GameState;

// --- helpers (inline) -------------------------------------------------------

static inline int cl_color_of(int8_t pc) {
    // caller must ensure pc != CL_EMPTY
    return (pc >= CL_BP) ? CL_BLACK : CL_WHITE;
}

static inline int cl_type_of(int8_t pc) {
    // strip color, returns CL_PAWN..CL_KING
    return (pc >= CL_BP) ? (pc - CL_BP + 1) : pc;
}

static inline int cl_file(int sq) { return sq & 7; }
static inline int cl_rank(int sq) { return sq >> 3; }
static inline int cl_sq(int file, int rank) { return rank * 8 + file; }

// --- api --------------------------------------------------------------------

// set up the standard starting position
void cl_init_startpos(GameState *gs);

// occupancy bitboard for a state (bit set == square holds a piece)
uint64_t cl_occupancy(const GameState *gs);

// parse a FEN string into gs, returns true on success
// used to sync board truth from lichess
bool cl_from_fen(GameState *gs, const char *fen);

// write FEN for gs into out (out must hold at least 90 bytes)
// used to hand the current position back to lichess / a UCI engine
void cl_to_fen(const GameState *gs, char *out);

// is square sq attacked by any piece of color by_side
bool cl_is_attacked(const GameState *gs, int sq, int by_side);

// is the side-to-move's king currently in check
bool cl_in_check(const GameState *gs);

// generate every fully legal move for the side to move
// writes into out[], returns the count (bounded by CL_MAX_MOVES)
int cl_gen_legal(const GameState *gs, Move *out);

// apply a move produced by cl_gen_legal, advancing the state
// (updates side, castling rights, ep square, clocks)
void cl_make_move(GameState *gs, const Move *m);

// occupancy that WOULD result from applying m to gs, without mutating gs
// this is what the sensor board should read once the move is physically done
uint64_t cl_result_occupancy(const GameState *gs, const Move *m);

// convenience: find a legal move matching from/to (and promo), NULL if none
// promo may be 0 to match a non-promotion or "don't care"
const Move *cl_find_move(const Move *list, int count,
                         int from, int to, int promo);

// bitmask of every legal destination for the piece on from_sq
// (0 if the square is empty, not the side to move, or pinned in place)
// used to light the target squares when a player lifts a piece
uint64_t cl_legal_dest_mask(const GameState *gs, int from_sq);

// game status helpers
bool cl_is_checkmate(const GameState *gs);   // in check and no legal moves
bool cl_is_stalemate(const GameState *gs);   // not in check and no legal moves

#ifdef __cplusplus
}
#endif

#endif // CHESSLINK_CHESS_H
