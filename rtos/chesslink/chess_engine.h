#pragma once
#include <Arduino.h>

// --- square and bitboard basics ----------------------------------------------
//
// sq = rank*8 + file  (a1=0, b1=1, ... h1=7, a2=8, ... h8=63)
// bitboard: bit N set means square N is relevant (occupied, attacked, etc.)

typedef uint64_t BB;

// file masks
#define FILE_A  0x0101010101010101ULL
#define FILE_B  0x0202020202020202ULL
#define FILE_G  0x4040404040404040ULL
#define FILE_H  0x8080808080808080ULL

// rank masks
#define RANK_1  0x00000000000000FFULL
#define RANK_2  0x000000000000FF00ULL
#define RANK_4  0x00000000FF000000ULL
#define RANK_5  0x000000FF00000000ULL
#define RANK_7  0x00FF000000000000ULL
#define RANK_8  0xFF00000000000000ULL

#define BB_EMPTY  0ULL

#define SQ(file, rank)   ((uint8_t)((rank)*8 + (file)))
#define FILE_OF(sq)      ((sq) % 8)
#define RANK_OF(sq)      ((sq) / 8)
#define BB_SQ(sq)        (1ULL << (sq))

// --- piece and color ---------------------------------------------------------

typedef enum : uint8_t {
    PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE
} PieceType;

typedef enum : uint8_t {
    WHITE, BLACK, NO_COLOR
} Color;

#define OTHER(c)  ((Color)(1 - (c)))

// --- castling rights ---------------------------------------------------------

#define CASTLE_WK  0x1
#define CASTLE_WQ  0x2
#define CASTLE_BK  0x4
#define CASTLE_BQ  0x8

// --- move encoding -----------------------------------------------------------
//
// 32-bit move word layout:
//   bits  0-5:  from square
//   bits  6-11: to square
//   bits 12-14: moving piece type
//   bits 15-17: captured piece type (NO_PIECE if quiet)
//   bits 18-20: promotion piece type (NO_PIECE if not a promo)
//   bit  21:    en passant capture flag
//   bit  22:    castling flag

typedef uint32_t Move;

#define MOVE_NONE  0

#define MV_FROM(m)       ((uint8_t)((m) & 0x3F))
#define MV_TO(m)         ((uint8_t)(((m) >> 6) & 0x3F))
#define MV_PIECE(m)      ((PieceType)(((m) >> 12) & 0x7))
#define MV_CAPTURE(m)    ((PieceType)(((m) >> 15) & 0x7))
#define MV_PROMO(m)      ((PieceType)(((m) >> 18) & 0x7))
#define MV_IS_EP(m)      (((m) >> 21) & 0x1)
#define MV_IS_CASTLE(m)  (((m) >> 22) & 0x1)
#define MV_IS_PROMO(m)   (MV_PROMO(m) != NO_PIECE)
#define MV_IS_CAPTURE(m) (MV_CAPTURE(m) != NO_PIECE || MV_IS_EP(m))

static inline Move make_move(uint8_t from, uint8_t to, PieceType piece,
                              PieceType capture = NO_PIECE,
                              PieceType promo   = NO_PIECE,
                              bool ep = false, bool castle = false) {
    return (Move)(from
        | ((uint32_t)to      << 6)
        | ((uint32_t)piece   << 12)
        | ((uint32_t)capture << 15)
        | ((uint32_t)promo   << 18)
        | ((uint32_t)ep      << 21)
        | ((uint32_t)castle  << 22));
}

// --- position ----------------------------------------------------------------

#define MAX_MOVES  218   // theoretical max legal moves in any chess position

typedef struct {
    BB        pieces[2][6];   // pieces[color][piece_type]
    BB        occupied[2];    // all pieces of each color
    BB        all;            // union of both

    Color     side;           // who moves next
    uint8_t   castling;       // CASTLE_* bitmask
    int8_t    ep_sq;          // en passant target square, -1 if none
    uint8_t   halfmove;       // 50-move rule counter

    // per-square lookup -- faster than scanning bitboards during make/unmake
    PieceType board[64];
    Color     color_at[64];
} Position;

// --- public API --------------------------------------------------------------

// initialize position from FEN string
void pos_from_fen(Position *pos, const char *fen);

// serialize position to FEN (caller provides buffer, suggest 92 bytes)
void pos_to_fen(const Position *pos, char *out, size_t len);

// generate all legal moves for side to move
// returns move count, fills moves[] -- caller provides array of MAX_MOVES
int  gen_legal_moves(const Position *pos, Move *moves);

// legal moves from one square only (for LED highlighting when piece is lifted)
int  gen_legal_moves_from(const Position *pos, uint8_t from_sq, Move *moves);

// bitmask of all squares the piece on from_sq can legally reach
BB   legal_destinations(const Position *pos, uint8_t from_sq);

// quick yes/no legality check for a from->to pair
bool is_legal(const Position *pos, uint8_t from_sq, uint8_t to_sq);

// is the given color's king currently in check?
bool in_check(const Position *pos, Color c);

// apply/undo a move (undo saves full position state -- one ply deep is enough)
void make_move_pos  (Position *pos, Move m, Position *undo);
void unmake_move_pos(Position *pos, const Position *undo);

// convert UCI string ("e2e4", "e7e8q") to a Move -- returns MOVE_NONE if illegal
Move uci_to_move(const Position *pos, const char *uci);

// write UCI string for a move into out (needs at least 6 bytes)
void move_to_uci(Move m, char *out);
