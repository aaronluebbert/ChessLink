// ChessLink RTOS -- combined source
// generated from individual task files
//
// files included in order:
//   chesslink.h        -- shared types, pin defs, queue handles, task declarations
//   chess_engine.h     -- bitboard types, move encoding, position struct, engine API
//   chess_engine.cpp   -- attack tables, FEN parser, move gen, make/unmake, UCI helpers
//   main.cpp           -- queue creation, task spawning, setup/loop
//   task_sensor.cpp    -- SN74HC165 chain read, debounce, BoardState queue
//   task_led.cpp       -- WS2812B via FastLED, LedCmd queue consumer
//   task_buttons.cpp   -- debounced button polling, ButtonEvent queue
//   task_game.cpp      -- game logic, move detection, promo picker state machine
//   task_display.cpp   -- ST7789 rendering, game screen, promotion picker screen
//   task_network.cpp   -- WiFi, lichess HTTP/stream, move posting
//
// to flash: rename to chesslink.ino (or keep as .cpp alongside a thin .ino stub)
// partition scheme: No OTA (2MB APP / 2MB SPIFFS)

// =============================================================================
// chesslink.h        -- shared types, pin defs, queue handles, task declarations
// =============================================================================

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// --- pin definitions ---------------------------------------------------------

// SN74HC165 shift registers (VSPI)
#define SR_SCLK       18   // VSPI CLK
#define SR_MISO       19   // VSPI MISO (QH from rank 1 SR)
#define SR_LOAD       5    // active-low parallel load, bit-banged

// WS2812B via 74AHCT125 level shifter
#define LED_DATA_PIN  4    // RMT output -> level shifter -> LED_DATA_5V

// ST7789 LCD (HSPI) -- 170x320
#define LCD_MOSI      13
#define LCD_SCLK      14
#define LCD_CS        15
#define LCD_DC        2
#define LCD_RST       -1
#define LCD_BL        32

// tactile buttons (input-only pins, no pullup available -- use external)
#define BTN_CYCLE     34   // cycle through promotion choices
#define BTN_CONFIRM   35   // confirm selection

// --- board geometry ----------------------------------------------------------

#define NUM_SQUARES   64
// sq = rank*8 + file  (a1=0, h8=63)

// --- RTOS task config --------------------------------------------------------

// stack sizes in words
#define STACK_SENSOR    4096
#define STACK_LED       4096
#define STACK_GAME      6144
#define STACK_DISPLAY   4096
#define STACK_NETWORK   8192
#define STACK_BUTTONS   2048

// priorities -- higher = more urgent
#define PRI_SENSOR      5   // must hit 50 Hz, nothing preempts it
#define PRI_LED         4   // RMT timing sensitive
#define PRI_GAME        3   // pure compute, no I/O
#define PRI_BUTTONS     3   // same as game -- button response needs to be snappy
#define PRI_NETWORK     2   // above display so incoming moves aren't starved
#define PRI_DISPLAY     1   // late redraw is fine, missed move is not

// core assignments
//
// core 1 -- sensor, LED, game logic, buttons
//   all pure compute or dedicated peripherals (VSPI, RMT, GPIO)
//   no WiFi stack interference
//
// core 0 -- network, display
//   ESP32 WiFi/TCP stack is a system task on core 0
//   network must live here, display stays here too (HSPI, infrequent redraws)
#define CORE_SENSOR     1
#define CORE_LED        1
#define CORE_GAME       1
#define CORE_BUTTONS    1
#define CORE_DISPLAY    0
#define CORE_NETWORK    0

// queue depths
#define Q_BOARD_STATE_DEPTH   4
#define Q_LED_CMD_DEPTH       8
#define Q_GAME_STATE_DEPTH    4
#define Q_MOVE_DEPTH          4
#define Q_BUTTON_DEPTH        8

// timing
#define SENSOR_SCAN_MS    20    // 50 Hz
#define LED_UPDATE_MS     16    // ~60 fps
#define BTN_POLL_MS       20    // 50 Hz button poll
#define BTN_DEBOUNCE_MS   50    // ms of stable state before registering press
#define DEBOUNCE_SCANS    3     // sensor debounce: consecutive identical reads

// --- data types --------------------------------------------------------------

// raw 64-bit occupancy bitmask (bit N = square N occupied)
typedef struct {
    uint64_t occupied;
    uint32_t timestamp_ms;
} BoardState_t;

// LED command: game logic -> LED control
typedef enum {
    LED_CMD_SET_SQUARE,
    LED_CMD_SET_ALL,
    LED_CMD_CLEAR,
    LED_CMD_PATTERN,   // mask squares lit with color, rest dimmed
} LedCmdType_t;

typedef struct {
    LedCmdType_t type;
    uint8_t  square;
    uint8_t  r, g, b;
    uint64_t mask;
} LedCmd_t;

// button events: button task -> game logic
typedef enum {
    BTN_EVT_CYCLE,    // BTN_CYCLE pressed
    BTN_EVT_CONFIRM,  // BTN_CONFIRM pressed
} ButtonEvent_t;

// promotion picker state -- embedded in GameState_t so display knows what to show
typedef enum {
    PROMO_NONE,       // not in a promotion
    PROMO_SELECTING,  // player is choosing a piece
} PromoState_t;

// game state snapshot: game logic -> LCD display
typedef enum {
    GAME_MODE_IDLE,
    GAME_MODE_LOCAL,
    GAME_MODE_LICHESS,
    GAME_MODE_ANALYSIS,
} GameMode_t;

typedef struct {
    GameMode_t  mode;
    char        fen[92];
    uint64_t    occupied;
    uint8_t     active_color;   // 0=white, 1=black
    int16_t     eval_cp;        // centipawn eval, INT16_MIN if unknown
    char        last_move[6];
    char        status_msg[32];

    // promotion picker -- display shows picker when promo_state == PROMO_SELECTING
    PromoState_t promo_state;
    uint8_t      promo_cursor;  // 0=queen 1=rook 2=bishop 3=knight
} GameState_t;

// move events between tasks
typedef enum {
    MOVE_SRC_PLAYER,
    MOVE_SRC_OPPONENT,
} MoveSrc_t;

typedef struct {
    MoveSrc_t src;
    uint8_t   from_sq;
    uint8_t   to_sq;
    char      uci[6];
} MoveEvent_t;

// --- queue handles (defined in main.cpp) -------------------------------------

extern QueueHandle_t xQ_BoardState;
extern QueueHandle_t xQ_LedCmd;
extern QueueHandle_t xQ_GameState;
extern QueueHandle_t xQ_PlayerMove;
extern QueueHandle_t xQ_OpponentMove;
extern QueueHandle_t xQ_ButtonEvent;

// --- task declarations -------------------------------------------------------

void task_SensorScan  (void *pvParameters);
void task_LedControl  (void *pvParameters);
void task_GameLogic   (void *pvParameters);
void task_LcdDisplay  (void *pvParameters);
void task_Network     (void *pvParameters);
void task_Buttons     (void *pvParameters);

// --- chess engine ------------------------------------------------------------
// call once from setup() before any tasks start
void chess_engine_init();


// =============================================================================
// chess_engine.h     -- bitboard types, move encoding, position struct, engine API
// =============================================================================

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


// =============================================================================
// chess_engine.cpp   -- attack tables, FEN parser, move gen, make/unmake, UCI helpers
// =============================================================================

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


// =============================================================================
// main.cpp           -- queue creation, task spawning, setup/loop
// =============================================================================

#include "chesslink.h"
#include "chess_engine.h"

// --- queue handles -----------------------------------------------------------

QueueHandle_t xQ_BoardState;
QueueHandle_t xQ_LedCmd;
QueueHandle_t xQ_GameState;
QueueHandle_t xQ_PlayerMove;
QueueHandle_t xQ_OpponentMove;
QueueHandle_t xQ_ButtonEvent;

// --- setup -------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.println("[chesslink] booting...");

    // build knight/king attack tables before any tasks start
    chess_engine_init();

    xQ_BoardState   = xQueueCreate(Q_BOARD_STATE_DEPTH,  sizeof(BoardState_t));
    xQ_LedCmd       = xQueueCreate(Q_LED_CMD_DEPTH,       sizeof(LedCmd_t));
    xQ_GameState    = xQueueCreate(Q_GAME_STATE_DEPTH,    sizeof(GameState_t));
    xQ_PlayerMove   = xQueueCreate(Q_MOVE_DEPTH,          sizeof(MoveEvent_t));
    xQ_OpponentMove = xQueueCreate(Q_MOVE_DEPTH,          sizeof(MoveEvent_t));
    xQ_ButtonEvent  = xQueueCreate(Q_BUTTON_DEPTH,        sizeof(ButtonEvent_t));

    if (!xQ_BoardState || !xQ_LedCmd || !xQ_GameState ||
        !xQ_PlayerMove || !xQ_OpponentMove || !xQ_ButtonEvent) {
        Serial.println("[chesslink] fatal: queue allocation failed");
        while (1) {}
    }

    // core 1
    xTaskCreatePinnedToCore(task_SensorScan, "SensorScan", STACK_SENSOR,  NULL, PRI_SENSOR,  NULL, CORE_SENSOR);
    xTaskCreatePinnedToCore(task_LedControl, "LedControl", STACK_LED,     NULL, PRI_LED,     NULL, CORE_LED);
    xTaskCreatePinnedToCore(task_GameLogic,  "GameLogic",  STACK_GAME,    NULL, PRI_GAME,    NULL, CORE_GAME);
    xTaskCreatePinnedToCore(task_Buttons,    "Buttons",    STACK_BUTTONS, NULL, PRI_BUTTONS, NULL, CORE_BUTTONS);

    // core 0
    xTaskCreatePinnedToCore(task_LcdDisplay, "LcdDisplay", STACK_DISPLAY, NULL, PRI_DISPLAY, NULL, CORE_DISPLAY);
    xTaskCreatePinnedToCore(task_Network,    "Network",    STACK_NETWORK, NULL, PRI_NETWORK, NULL, CORE_NETWORK);

    Serial.println("[chesslink] all tasks spawned");
}

// loop() runs on core 1 at priority 1 -- park it
void loop() {
    vTaskDelay(portMAX_DELAY);
}


// =============================================================================
// task_sensor.cpp    -- SN74HC165 chain read, debounce, BoardState queue
// =============================================================================

#include "chesslink.h"
#include <SPI.h>

// --- chain topology ----------------------------------------------------------
//
// 8 SN74HC165 shift registers, one per rank
//
// physical layout (sitting at the board, ESP PCB to the right):
//
//   rank 8 SR  ->  rank 7 SR  ->  ...  ->  rank 1 SR  ->  GPIO19 (MISO)
//   (furthest from ESP)                     (QH to ESP)
//
// rank PCBs detect a full rank: each sensor on a given rank connects to
// the SR input for its file. wiring within each SR:
//
//   A-file sensor -> input H (MSB, clocked out first)
//   B-file sensor -> input G
//   ...
//   H-file sensor -> input A (LSB, clocked out last)
//
// so after SPI.transfer x8 with MSBFIRST, the raw uint64 looks like:
//
//   bits 63..56 = rank 8  (bit63=A8, bit62=B8, ... bit56=H8)
//   bits 55..48 = rank 7
//   ...
//   bits  7.. 0 = rank 1  (bit7=A1,  bit6=B1,  ... bit0=H1)
//
// to get canonical sq = rank*8 + file from raw bit position p:
//   rank = p / 8
//   file = 7 - (p % 8)   // H->A maps bits 0->7, so invert
//   sq   = rank*8 + file
//
// A3144 output is active-low, a 0 bit means magnet detected (occupied)

// --- helpers -----------------------------------------------------------------

// pulse SR_LOAD low to latch all parallel inputs at once
// HC165 latches on falling edge of PL, hold 1us before clocking
static inline void sr_latch() {
    digitalWrite(SR_LOAD, LOW);
    delayMicroseconds(1);
    digitalWrite(SR_LOAD, HIGH);
}

// clock out all 64 bits over VSPI
// rank 8 byte comes out first (MSB of result), rank 1 last
static uint64_t sr_read_all() {
    sr_latch();

    uint64_t raw = 0;
    for (int i = 7; i >= 0; i--) {
        uint8_t b = SPI.transfer(0x00);
        raw |= ((uint64_t)b << (i * 8));
    }
    return raw;
}

// convert raw 64-bit SR read to occupied bitmask in canonical sq indexing
// raw bit p -> rank = p/8, file = 7-(p%8), sq = rank*8+file
// A3144 is active-low so invert: bit=0 means occupied
static uint64_t raw_to_occupied(uint64_t raw) {
    uint64_t occupied = 0;
    for (int p = 0; p < 64; p++) {
        bool sensor_low = !((raw >> p) & 1);
        if (sensor_low) {
            int rank = p / 8;
            int file = 7 - (p % 8);
            int sq   = rank * 8 + file;
            occupied |= (1ULL << sq);
        }
    }
    return occupied;
}

// --- task --------------------------------------------------------------------

void task_SensorScan(void *pvParameters) {
    pinMode(SR_LOAD, OUTPUT);
    digitalWrite(SR_LOAD, HIGH);  // idle high, active-low load

    // VSPI, mode 1, 1 MHz (conservative, bump after hardware validation)
    SPI.begin(SR_SCLK, SR_MISO, /*MOSI*/-1, /*SS*/-1);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));

    uint64_t prev_confirmed = 0;
    uint64_t candidate      = 0;
    uint8_t  stable_count   = 0;

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint64_t occupied = raw_to_occupied(sr_read_all());

        // need DEBOUNCE_SCANS identical reads in a row before accepting a change
        if (occupied == candidate) {
            stable_count++;
        } else {
            candidate    = occupied;
            stable_count = 0;
        }

        if (stable_count >= DEBOUNCE_SCANS && occupied != prev_confirmed) {
            prev_confirmed = occupied;
            stable_count   = 0;

            BoardState_t msg = {
                .occupied     = occupied,
                .timestamp_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS,
            };

            // drop oldest rather than blocking the scan loop
            if (xQueueSend(xQ_BoardState, &msg, 0) != pdTRUE) {
                BoardState_t discard;
                xQueueReceive(xQ_BoardState, &discard, 0);
                xQueueSend(xQ_BoardState, &msg, 0);
            }
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(SENSOR_SCAN_MS));
    }

    SPI.endTransaction();
    vTaskDelete(NULL);
}


// =============================================================================
// task_led.cpp       -- WS2812B via FastLED, LedCmd queue consumer
// =============================================================================

#include "chesslink.h"
#include <FastLED.h>

// - LED buffer -

static CRGB leds[NUM_SQUARES];

// TODO: remap if physical strip routing is serpentine or column-major
static inline uint8_t sq_to_led(uint8_t sq) {
    return sq;
}

// - command handlers -

static void apply_led_cmd(const LedCmd_t *cmd) {
    switch (cmd->type) {

        case LED_CMD_SET_SQUARE: {
            uint8_t idx = sq_to_led(cmd->square);
            if (idx < NUM_SQUARES)
                leds[idx] = CRGB(cmd->r, cmd->g, cmd->b);
            break;
        }

        case LED_CMD_SET_ALL: {
            CRGB color(cmd->r, cmd->g, cmd->b);
            for (int i = 0; i < NUM_SQUARES; i++) leds[i] = color;
            break;
        }

        case LED_CMD_CLEAR:
            FastLED.clear();
            break;

        case LED_CMD_PATTERN: {
            // lit squares get full color, unlit squares get ~10% for context
            CRGB on_color(cmd->r, cmd->g, cmd->b);
            CRGB off_color = on_color;
            off_color.nscale8(26);

            for (int sq = 0; sq < NUM_SQUARES; sq++) {
                leds[sq_to_led(sq)] = (cmd->mask & (1ULL << sq)) ? on_color : off_color;
            }
            break;
        }
    }
}

// - task -

void task_LedControl(void *pvParameters) {
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, NUM_SQUARES);
    FastLED.setBrightness(64);  // ~25%, increase after thermal check
    FastLED.clear(true);

    LedCmd_t cmd;
    TickType_t xLastShow = xTaskGetTickCount();

    for (;;) {
        // drain all pending commands before pushing to strip
        while (xQueueReceive(xQ_LedCmd, &cmd, 0) == pdTRUE)
            apply_led_cmd(&cmd);

        // cap FastLED.show() to ~60 fps regardless of command rate
        TickType_t now = xTaskGetTickCount();
        if ((now - xLastShow) >= pdMS_TO_TICKS(LED_UPDATE_MS)) {
            FastLED.show();
            xLastShow = now;
        }

        // block up to one frame period waiting for next command
        if (xQueueReceive(xQ_LedCmd, &cmd, pdMS_TO_TICKS(LED_UPDATE_MS)) == pdTRUE)
            apply_led_cmd(&cmd);
    }

    vTaskDelete(NULL);
}


// =============================================================================
// task_buttons.cpp   -- debounced button polling, ButtonEvent queue
// =============================================================================

#include "chesslink.h"

// --- task --------------------------------------------------------------------
//
// GPIO 34 and 35 are input-only on the ESP32 -- no internal pullup available,
// so the PCB must have external pullups (10k to 3.3V recommended).
// buttons are active-low: idle = HIGH, pressed = LOW.
//
// debounce: track how long each pin has been stable, fire event on
// leading edge after BTN_DEBOUNCE_MS of consistent LOW.

void task_Buttons(void *pvParameters) {
    pinMode(BTN_CYCLE,   INPUT);
    pinMode(BTN_CONFIRM, INPUT);

    // state per button
    struct BtnState {
        bool     last_raw;     // last raw digitalRead
        bool     confirmed;    // last debounced state
        uint32_t stable_since; // millis() when current raw state started
    } btns[2] = {};

    // initialize to current pin state so we don't fire on boot
    btns[0].last_raw = btns[0].confirmed = digitalRead(BTN_CYCLE);
    btns[1].last_raw = btns[1].confirmed = digitalRead(BTN_CONFIRM);
    btns[0].stable_since = btns[1].stable_since = millis();

    const int pins[2]            = { BTN_CYCLE, BTN_CONFIRM };
    const ButtonEvent_t evts[2]  = { BTN_EVT_CYCLE, BTN_EVT_CONFIRM };

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint32_t now = millis();

        for (int i = 0; i < 2; i++) {
            bool raw = digitalRead(pins[i]);

            if (raw != btns[i].last_raw) {
                // pin changed -- reset stable timer
                btns[i].last_raw    = raw;
                btns[i].stable_since = now;
            } else if ((now - btns[i].stable_since) >= BTN_DEBOUNCE_MS
                       && raw != btns[i].confirmed) {
                // stable for long enough and state actually changed
                btns[i].confirmed = raw;

                // fire event on falling edge only (HIGH->LOW = press)
                if (raw == LOW) {
                    ButtonEvent_t evt = evts[i];
                    xQueueSend(xQ_ButtonEvent, &evt, 0);
                }
            }
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(BTN_POLL_MS));
    }

    vTaskDelete(NULL);
}


// =============================================================================
// task_game.cpp      -- game logic, move detection, promo picker state machine
// =============================================================================

#include "chesslink.h"
#include "chess_engine.h"

// --- internal state ----------------------------------------------------------

typedef enum {
    PHASE_IDLE,
    PHASE_PIECE_LIFTED,
    PHASE_PROMO_SELECT,   // pawn reached back rank, waiting for piece choice
} MovePhase_t;

// promotion piece options in cycle order
static const PieceType PROMO_PIECES[4] = { QUEEN, ROOK, BISHOP, KNIGHT };
static const char      PROMO_NAMES[4]  = { 'q',   'r',  'b',    'n'   };

typedef struct {
    Position    pos;
    GameMode_t  mode;
    char        status_msg[32];
    char        last_move[6];
    int16_t     eval_cp;

    // move detection
    MovePhase_t phase;
    uint8_t     lifted_sq;
    uint8_t     promo_to_sq;   // destination square saved while picker is open
    BB          legal_dests;

    // promotion picker
    uint8_t     promo_cursor;  // 0=queen 1=rook 2=bishop 3=knight
} GameCtx_t;

// --- helpers -----------------------------------------------------------------

static void send_led_clear(void) {
    LedCmd_t cmd = { .type = LED_CMD_CLEAR };
    xQueueSend(xQ_LedCmd, &cmd, 0);
}

static void send_led_sq(uint8_t sq, uint8_t r, uint8_t g, uint8_t b) {
    LedCmd_t cmd = { .type = LED_CMD_SET_SQUARE, .square = sq, .r = r, .g = g, .b = b };
    xQueueSend(xQ_LedCmd, &cmd, 0);
}

static void show_legal_moves(uint8_t from_sq, BB dests) {
    LedCmd_t cmd = {
        .type = LED_CMD_PATTERN,
        .square = from_sq,
        .r = 0, .g = 200, .b = 0,
        .mask = dests,
    };
    xQueueSend(xQ_LedCmd, &cmd, 0);
    send_led_sq(from_sq, 200, 200, 0);  // yellow source
}

static void publish_game_state(const GameCtx_t *ctx) {
    GameState_t gs = {};
    gs.mode         = ctx->mode;
    gs.active_color = (uint8_t)ctx->pos.side;
    gs.eval_cp      = ctx->eval_cp;
    gs.occupied     = ctx->pos.all;
    gs.promo_state  = (ctx->phase == PHASE_PROMO_SELECT) ? PROMO_SELECTING : PROMO_NONE;
    gs.promo_cursor = ctx->promo_cursor;
    pos_to_fen(&ctx->pos, gs.fen, sizeof(gs.fen));
    strncpy(gs.last_move,  ctx->last_move,  sizeof(gs.last_move)  - 1);
    strncpy(gs.status_msg, ctx->status_msg, sizeof(gs.status_msg) - 1);
    xQueueOverwrite(xQ_GameState, &gs);
}

// commit a fully resolved move (piece and promo piece already known)
static void commit_move(GameCtx_t *ctx, Move chosen) {
    char uci[6];
    move_to_uci(chosen, uci);

    Position undo;
    make_move_pos(&ctx->pos, chosen, &undo);

    strncpy(ctx->last_move, uci, sizeof(ctx->last_move) - 1);
    snprintf(ctx->status_msg, sizeof(ctx->status_msg),
             "move: %s  %s to move", uci,
             ctx->pos.side == WHITE ? "white" : "black");

    ctx->phase = PHASE_IDLE;

    send_led_sq(MV_TO(chosen), 0, 255, 0);  // green flash on destination

    if (ctx->mode == GAME_MODE_LICHESS) {
        MoveEvent_t mv = {
            .src     = MOVE_SRC_PLAYER,
            .from_sq = MV_FROM(chosen),
            .to_sq   = MV_TO(chosen),
        };
        strncpy(mv.uci, uci, sizeof(mv.uci) - 1);
        xQueueSend(xQ_PlayerMove, &mv, 0);
    }

    publish_game_state(ctx);
    vTaskDelay(pdMS_TO_TICKS(500));
    send_led_clear();
}

// --- board change handling ---------------------------------------------------

static void process_board_change(GameCtx_t *ctx, const BoardState_t *bs) {
    // ignore board changes while player is picking a promotion piece
    if (ctx->phase == PHASE_PROMO_SELECT) return;

    uint64_t prev   = ctx->pos.all;
    uint64_t curr   = bs->occupied;
    uint64_t lifted = prev & ~curr;
    uint64_t placed  = curr & ~prev;

    if (ctx->phase == PHASE_IDLE) {
        if (lifted && !placed) {
            uint8_t sq = (uint8_t)__builtin_ctzll(lifted);
            if (ctx->pos.color_at[sq] != ctx->pos.side) return;

            ctx->lifted_sq   = sq;
            ctx->legal_dests = legal_destinations(&ctx->pos, sq);
            ctx->phase       = PHASE_PIECE_LIFTED;
            show_legal_moves(sq, ctx->legal_dests);
        }

    } else if (ctx->phase == PHASE_PIECE_LIFTED) {
        if (placed) {
            uint8_t to_sq = (uint8_t)__builtin_ctzll(placed);

            if (ctx->legal_dests & BB_SQ(to_sq)) {
                // check if this is a promotion (pawn reaching back rank)
                bool is_promo = (ctx->pos.board[ctx->lifted_sq] == PAWN)
                    && ((ctx->pos.side == WHITE && RANK_OF(to_sq) == 7)
                     || (ctx->pos.side == BLACK && RANK_OF(to_sq) == 0));

                if (is_promo) {
                    // pause and ask the player what piece they want
                    ctx->promo_to_sq  = to_sq;
                    ctx->promo_cursor = 0;  // default to queen
                    ctx->phase        = PHASE_PROMO_SELECT;

                    // pulse the destination square purple to indicate waiting
                    send_led_sq(to_sq, 180, 0, 200);

                    snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                             "choose promotion piece");
                    publish_game_state(ctx);

                } else {
                    // normal move -- find and commit it
                    Move moves[MAX_MOVES];
                    int  cnt    = gen_legal_moves_from(&ctx->pos, ctx->lifted_sq, moves);
                    Move chosen = MOVE_NONE;
                    for (int i = 0; i < cnt; i++) {
                        if (MV_TO(moves[i]) == to_sq) { chosen = moves[i]; break; }
                    }
                    if (chosen != MOVE_NONE) commit_move(ctx, chosen);
                }

            } else {
                // illegal destination -- flash red, re-show hints
                send_led_sq(to_sq, 255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
                show_legal_moves(ctx->lifted_sq, ctx->legal_dests);
            }

        } else if (!lifted && !placed) {
            // piece returned to origin -- cancel
            ctx->phase = PHASE_IDLE;
            send_led_clear();
        }
    }
}

// --- promotion picker --------------------------------------------------------
//
// BTN_CYCLE rotates through Q -> R -> B -> N -> Q
// BTN_CONFIRM locks in the current choice and commits the move

static void handle_button(GameCtx_t *ctx, ButtonEvent_t evt) {
    if (ctx->phase != PHASE_PROMO_SELECT) return;

    if (evt == BTN_EVT_CYCLE) {
        ctx->promo_cursor = (ctx->promo_cursor + 1) % 4;

        // update LED color to hint the current choice:
        //   queen=gold, rook=cyan, bishop=orange, knight=magenta
        static const uint8_t colors[4][3] = {
            {220, 180,   0},   // queen -- gold
            {  0, 200, 200},   // rook -- cyan
            {220, 100,   0},   // bishop -- orange
            {160,   0, 200},   // knight -- magenta
        };
        const uint8_t *c = colors[ctx->promo_cursor];
        send_led_sq(ctx->promo_to_sq, c[0], c[1], c[2]);

        snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                 "promote to: %c  (confirm?)",
                 PROMO_NAMES[ctx->promo_cursor]);
        publish_game_state(ctx);

    } else if (evt == BTN_EVT_CONFIRM) {
        // find the legal move matching from, to, and chosen promo piece
        PieceType chosen_piece = PROMO_PIECES[ctx->promo_cursor];
        Move moves[MAX_MOVES];
        int  cnt = gen_legal_moves_from(&ctx->pos, ctx->lifted_sq, moves);
        Move chosen = MOVE_NONE;
        for (int i = 0; i < cnt; i++) {
            if (MV_TO(moves[i]) == ctx->promo_to_sq
                && MV_PROMO(moves[i]) == chosen_piece) {
                chosen = moves[i];
                break;
            }
        }
        if (chosen != MOVE_NONE) commit_move(ctx, chosen);
    }
}

// --- opponent move -----------------------------------------------------------

static void apply_opponent_move(GameCtx_t *ctx, const MoveEvent_t *mv) {
    Move m = uci_to_move(&ctx->pos, mv->uci);
    if (m == MOVE_NONE) {
        snprintf(ctx->status_msg, sizeof(ctx->status_msg), "bad opp move: %s", mv->uci);
        publish_game_state(ctx);
        return;
    }

    Position undo;
    make_move_pos(&ctx->pos, m, &undo);

    strncpy(ctx->last_move, mv->uci, sizeof(ctx->last_move) - 1);
    snprintf(ctx->status_msg, sizeof(ctx->status_msg),
             "opp: %s  your turn", mv->uci);

    LedCmd_t cmd = { .type = LED_CMD_SET_SQUARE, .square = mv->from_sq,
                     .r = 0, .g = 0, .b = 200 };
    xQueueSend(xQ_LedCmd, &cmd, 0);
    send_led_sq(mv->to_sq, 0, 100, 255);

    publish_game_state(ctx);
}

// --- task --------------------------------------------------------------------

void task_GameLogic(void *pvParameters) {
    GameCtx_t ctx = {};
    ctx.mode    = GAME_MODE_LOCAL;
    ctx.eval_cp = INT16_MIN;
    ctx.phase   = PHASE_IDLE;

    pos_from_fen(&ctx.pos,
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    strncpy(ctx.status_msg, "place pieces to begin", sizeof(ctx.status_msg) - 1);

    publish_game_state(&ctx);

    BoardState_t  bs;
    MoveEvent_t   opp_mv;
    ButtonEvent_t btn_evt;

    for (;;) {
        if (xQueueReceive(xQ_BoardState, &bs, pdMS_TO_TICKS(50)) == pdTRUE)
            process_board_change(&ctx, &bs);

        if (xQueueReceive(xQ_OpponentMove, &opp_mv, 0) == pdTRUE)
            apply_opponent_move(&ctx, &opp_mv);

        if (xQueueReceive(xQ_ButtonEvent, &btn_evt, 0) == pdTRUE)
            handle_button(&ctx, btn_evt);
    }

    vTaskDelete(NULL);
}


// =============================================================================
// task_display.cpp   -- ST7789 rendering, game screen, promotion picker screen
// =============================================================================

#include "chesslink.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// --- display setup -----------------------------------------------------------

static SPIClass hspi(HSPI);
static Adafruit_ST7789 tft = Adafruit_ST7789(&hspi, LCD_CS, LCD_DC, LCD_RST);

// physical display: 170x320 (portrait)
#define D_W  170
#define D_H  320

// --- color palette -----------------------------------------------------------

#define C_BLACK       0x0000
#define C_WHITE       0xFFFF
#define C_DARK_GRAY   0x39C7
#define C_LIGHT_GRAY  0xC618
#define C_GREEN       0x07E0
#define C_RED         0xF800
#define C_YELLOW      0xFFE0
#define C_BLUE        0x001F
#define C_PURPLE      0x780F

// theme
#define C_BG          C_BLACK
#define C_HEADER_BG   0x0390
#define C_TEXT        C_WHITE
#define C_ACCENT      C_YELLOW
#define C_DIM         C_DARK_GRAY

// board square colors
#define C_SQ_LIGHT    0xF7BE
#define C_SQ_DARK     0x9A40

// --- layout (170x320 portrait) -----------------------------------------------
//
//  y=0    header bar (28px)
//  y=28   turn indicator (20px)
//  y=48   last move (18px)
//  y=66   status msg (18px)
//  y=84   mini board (160x160, 20px/sq, x=5)
//  y=244  padding (16px)
//  y=260  eval bar (20px)
//  y=280  padding to 320

#define HEADER_Y    0
#define HEADER_H    28
#define TURN_Y      28
#define TURN_H      20
#define MOVE_Y      48
#define MOVE_H      18
#define STATUS_Y    66
#define STATUS_H    18
#define BOARD_SQ    20
#define BOARD_PX    (BOARD_SQ * 8)   // 160
#define BOARD_X     ((D_W - BOARD_PX) / 2)   // 5
#define BOARD_Y     84
#define EVAL_Y      260
#define EVAL_H      20

// --- normal game screen draw functions ---------------------------------------

static void draw_header(GameMode_t mode) {
    tft.fillRect(0, HEADER_Y, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("ChessLink");

    const char *label;
    uint16_t    color;
    switch (mode) {
        case GAME_MODE_LOCAL:    label = "LOCAL";    color = C_BLUE;      break;
        case GAME_MODE_LICHESS:  label = "LICHESS";  color = C_GREEN;     break;
        case GAME_MODE_ANALYSIS: label = "ANALYSIS"; color = C_YELLOW;    break;
        default:                 label = "IDLE";     color = C_DARK_GRAY; break;
    }
    tft.fillRect(D_W - 54, 7, 52, 14, color);
    tft.setTextColor(C_BLACK);
    tft.setTextSize(1);
    tft.setCursor(D_W - 52, 10);
    tft.print(label);
}

static void draw_turn_indicator(uint8_t active_color) {
    uint16_t bg = active_color == 0 ? C_WHITE : C_BLACK;
    uint16_t fg = active_color == 0 ? C_BLACK : C_WHITE;
    tft.fillRect(0, TURN_Y, D_W, TURN_H, bg);
    if (active_color == 1)
        tft.drawRect(0, TURN_Y, D_W, TURN_H, C_LIGHT_GRAY);
    tft.setTextColor(fg);
    tft.setTextSize(1);
    tft.setCursor(34, TURN_Y + 6);
    tft.print(active_color == 0 ? "WHITE to move" : "BLACK to move");
}

static void draw_last_move(const char *move_str) {
    tft.fillRect(0, MOVE_Y, D_W, MOVE_H, C_BG);
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(4, MOVE_Y + 4);
    tft.print("last: ");
    tft.setTextColor(C_ACCENT);
    tft.setTextSize(2);
    tft.print(move_str[0] ? move_str : "---");
}

static void draw_status(const char *msg) {
    tft.fillRect(0, STATUS_Y, D_W, STATUS_H, C_BG);
    tft.setTextColor(C_TEXT);
    tft.setTextSize(1);
    tft.setCursor(4, STATUS_Y + 4);
    tft.print(msg);
}

static void draw_eval_bar(int16_t eval_cp) {
    tft.fillRect(0, EVAL_Y, D_W, EVAL_H, C_DARK_GRAY);
    if (eval_cp == INT16_MIN) {
        tft.setTextColor(C_DIM);
        tft.setTextSize(1);
        tft.setCursor(60, EVAL_Y + 6);
        tft.print("eval: ---");
        return;
    }
    int clamped = eval_cp < -500 ? -500 : eval_cp > 500 ? 500 : eval_cp;
    int bar_px  = (clamped + 500) * D_W / 1000;
    tft.fillRect(0,      EVAL_Y, bar_px,       EVAL_H, C_WHITE);
    tft.fillRect(bar_px, EVAL_Y, D_W - bar_px, EVAL_H, C_BLACK);
    tft.drawRect(0,      EVAL_Y, D_W,           EVAL_H, C_LIGHT_GRAY);
    char buf[12];
    if (abs(eval_cp) >= 10000)
        snprintf(buf, sizeof(buf), "M%d", (abs(eval_cp) - 9000) / 100);
    else
        snprintf(buf, sizeof(buf), "%+.1f", eval_cp / 100.0f);
    tft.setTextSize(1);
    tft.setTextColor(bar_px > D_W / 2 ? C_BLACK : C_WHITE);
    tft.setCursor(D_W / 2 - 14, EVAL_Y + 6);
    tft.print(buf);
}

static void draw_mini_board(uint64_t occupied) {
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            uint8_t sq  = (uint8_t)(row * 8 + col);
            bool    lit = (row + col) % 2 == 0;
            int x = BOARD_X + col * BOARD_SQ;
            int y = BOARD_Y + (7 - row) * BOARD_SQ;  // rank 1 at bottom
            tft.fillRect(x, y, BOARD_SQ, BOARD_SQ, lit ? C_SQ_LIGHT : C_SQ_DARK);
            if (occupied & (1ULL << sq))
                tft.fillCircle(x + BOARD_SQ / 2, y + BOARD_SQ / 2,
                               BOARD_SQ / 2 - 2, C_WHITE);
        }
    }
    tft.drawRect(BOARD_X - 1, BOARD_Y - 1, BOARD_PX + 2, BOARD_PX + 2, C_LIGHT_GRAY);
}

static void render_game_state(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);
    draw_turn_indicator(gs->active_color);
    draw_last_move(gs->last_move);
    draw_status(gs->status_msg);
    draw_mini_board(gs->occupied);
    draw_eval_bar(gs->eval_cp);
}

// --- promotion picker screen -------------------------------------------------
//
// replaces the normal game view while PROMO_SELECTING.
// shows 4 piece options in a 2x2 grid with the cursor highlighted.
// BTN_CYCLE cycles the highlight, BTN_CONFIRM commits.
//
// layout (centered in 170x320):
//
//   y=0    header (28px, reused)
//   y=40   "PROMOTE PAWN" title
//   y=70   2x2 grid of piece tiles (each 70x80px)
//            col0 x=5,  col1 x=90
//            row0 y=70  (queen, rook)
//            row1 y=160 (bishop, knight)
//   y=260  hint text
//   y=290  button legend

// piece tile colors (RGB565)
static const uint16_t PROMO_COLORS[4] = {
    0xC600,   // queen -- gold
    0x07FF,   // rook -- cyan
    0xFC00,   // bishop -- orange
    0x780F,   // knight -- magenta
};
static const char *PROMO_LABELS[4] = { "QUEEN", "ROOK", "BISHOP", "KNIGHT" };

#define TILE_W   72
#define TILE_H   78
#define TILE_X0  5
#define TILE_X1  93
#define TILE_Y0  62
#define TILE_Y1  150

static void draw_promo_tile(int idx, bool selected) {
    int x = (idx % 2 == 0) ? TILE_X0 : TILE_X1;
    int y = (idx < 2)      ? TILE_Y0 : TILE_Y1;

    uint16_t bg     = selected ? PROMO_COLORS[idx] : C_DARK_GRAY;
    uint16_t border = selected ? C_WHITE            : C_LIGHT_GRAY;
    uint16_t fg     = selected ? C_BLACK            : C_LIGHT_GRAY;

    tft.fillRect(x, y, TILE_W, TILE_H, bg);
    tft.drawRect(x, y, TILE_W, TILE_H, border);
    if (selected)
        tft.drawRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, border);  // double border on selected

    // piece initial letter -- large, centered in tile
    tft.setTextColor(fg);
    tft.setTextSize(4);
    tft.setCursor(x + TILE_W / 2 - 12, y + 14);
    tft.print(PROMO_LABELS[idx][0]);

    // piece name -- small, below letter
    tft.setTextSize(1);
    tft.setCursor(x + 4, y + TILE_H - 14);
    tft.print(PROMO_LABELS[idx]);
}

static void render_promo_picker(const GameState_t *gs) {
    tft.fillScreen(C_BG);

    // reuse header
    draw_header(gs->mode);

    // title bar
    tft.fillRect(0, 30, D_W, 28, C_PURPLE);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(1);
    tft.setCursor(28, 40);
    tft.print("PROMOTE PAWN -- choose piece");

    // 2x2 tile grid
    for (int i = 0; i < 4; i++)
        draw_promo_tile(i, i == gs->promo_cursor);

    // button legend at bottom
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(4, 240);
    tft.print("[CYCLE] next piece");
    tft.setCursor(4, 256);
    tft.print("[CONFIRM] lock in choice");
}

// partial redraw for picker -- only re-draws the tiles, not the full screen.
// called when promo_cursor changes so we don't flicker the whole display.
static void redraw_promo_tiles(const GameState_t *gs) {
    for (int i = 0; i < 4; i++)
        draw_promo_tile(i, i == gs->promo_cursor);
}

// --- task --------------------------------------------------------------------

void task_LcdDisplay(void *pvParameters) {
    hspi.begin(LCD_SCLK, /*MISO*/-1, LCD_MOSI, LCD_CS);
    tft.init(170, 320, SPI_MODE2);
    tft.setRotation(0);
    tft.fillScreen(C_BLACK);

    if (LCD_BL >= 0) {
        pinMode(LCD_BL, OUTPUT);
        digitalWrite(LCD_BL, HIGH);
    }

    // splash
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(14, 140);
    tft.print("ChessLink");
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(44, 162);
    tft.print("initializing...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    GameState_t  gs       = {};
    PromoState_t last_promo_state  = PROMO_NONE;
    uint8_t      last_promo_cursor = 0xFF;  // force initial draw

    for (;;) {
        if (xQueueReceive(xQ_GameState, &gs, pdMS_TO_TICKS(250)) != pdTRUE)
            continue;

        if (gs.promo_state == PROMO_SELECTING) {
            if (last_promo_state != PROMO_SELECTING) {
                // just entered picker -- full redraw
                render_promo_picker(&gs);
            } else if (gs.promo_cursor != last_promo_cursor) {
                // cursor moved -- only redraw tiles to avoid flicker
                redraw_promo_tiles(&gs);
            }
        } else {
            // normal game screen
            render_game_state(&gs);
        }

        last_promo_state  = gs.promo_state;
        last_promo_cursor = gs.promo_cursor;
    }

    vTaskDelete(NULL);
}


// =============================================================================
// task_network.cpp   -- WiFi, lichess HTTP/stream, move posting
// =============================================================================

#include "chesslink.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// - config (move to NVS in production) -

#define WIFI_SSID      "YOUR_SSID"
#define WIFI_PASS      "YOUR_PASS"
#define LICHESS_TOKEN  "YOUR_LICHESS_API_TOKEN"
#define LICHESS_BASE   "https://lichess.org"

// - internal state -

typedef enum {
    NET_STATE_DISCONNECTED,
    NET_STATE_IDLE,
    NET_STATE_IN_GAME,
} NetState_t;

static NetState_t net_state   = NET_STATE_DISCONNECTED;
static char       game_id[16] = {};

// - WiFi -

static bool wifi_connect(void) {
    Serial.printf("[net] connecting to %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    for (int elapsed = 0; elapsed < 15000 && WiFi.status() != WL_CONNECTED; elapsed += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[net] connected -- IP %s\n", WiFi.localIP().toString().c_str());
        return true;
    }
    Serial.println("\n[net] WiFi failed");
    return false;
}

// - lichess API helpers -

// POST /api/board/game/{gameId}/move/{uci}
static bool lichess_post_move(const char *game, const char *uci) {
    if (WiFi.status() != WL_CONNECTED) return false;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/%s/move/%s", LICHESS_BASE, game, uci);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Length", "0");

    int code = http.POST("");
    if (code != 200) Serial.printf("[net] post move failed: HTTP %d\n", code);
    http.end();
    return code == 200;
}

// POST /api/board/seek -- 5+3 blitz, rated
static bool lichess_seek_game(void) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(LICHESS_BASE "/api/board/seek");
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int code = http.POST("rated=true&time=5&increment=3&variant=standard");
    Serial.printf("[net] seek result: HTTP %d\n", code);
    http.end();
    return code == 200 || code == 201;
}

// GET /api/board/game/stream/{gameId} -- NDJSON long-poll.
// reads until connection drops or a player move arrives to post.
// TODO: replace with proper streaming HTTP client for robustness
static void lichess_stream_game(const char *game) {
    if (WiFi.status() != WL_CONNECTED) return;

    char url[128];
    snprintf(url, sizeof(url), "%s/api/board/game/stream/%s", LICHESS_BASE, game);

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", "Bearer " LICHESS_TOKEN);
    http.setTimeout(30000);

    if (http.GET() != 200) { http.end(); return; }

    WiFiClient *stream = http.getStreamPtr();
    char line_buf[256];
    int  line_len = 0;

    MoveEvent_t  player_mv;
    TickType_t   last_data = xTaskGetTickCount();
    const TickType_t STREAM_TIMEOUT = pdMS_TO_TICKS(30000);

    while (WiFi.status() == WL_CONNECTED) {
        // post player moves without breaking the read loop
        if (xQueueReceive(xQ_PlayerMove, &player_mv, 0) == pdTRUE)
            lichess_post_move(game, player_mv.uci);

        if (stream->available()) {
            char c = (char)stream->read();
            last_data = xTaskGetTickCount();

            if (c == '\n') {
                line_buf[line_len] = '\0';
                line_len = 0;

                if (strlen(line_buf) == 0) continue;  // keep-alive newline

                StaticJsonDocument<512> doc;
                if (deserializeJson(doc, line_buf)) continue;

                const char *type = doc["type"];
                if (!type) continue;

                if (strcmp(type, "gameState") == 0) {
                    const char *moves = doc["moves"];
                    if (!moves || !strlen(moves)) continue;

                    // last space-delimited token is the most recent move
                    const char *last = strrchr(moves, ' ');
                    const char *uci  = last ? last + 1 : moves;

                    if (strlen(uci) < 4) continue;

                    // TODO: track color assignment properly, this blindly forwards
                    // every state update as an opponent move
                    MoveEvent_t opp = { .src = MOVE_SRC_OPPONENT };
                    strncpy(opp.uci, uci, sizeof(opp.uci) - 1);
                    opp.from_sq = (uint8_t)((opp.uci[0] - 'a') + (opp.uci[1] - '1') * 8);
                    opp.to_sq   = (uint8_t)((opp.uci[2] - 'a') + (opp.uci[3] - '1') * 8);
                    xQueueSend(xQ_OpponentMove, &opp, 0);
                }

            } else if (line_len < (int)sizeof(line_buf) - 2) {
                line_buf[line_len++] = c;
            }

        } else {
            if ((xTaskGetTickCount() - last_data) > STREAM_TIMEOUT) {
                Serial.println("[net] stream timeout");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    http.end();
}

// - task -

void task_Network(void *pvParameters) {
    MoveEvent_t player_mv;

    while (!wifi_connect())
        vTaskDelay(pdMS_TO_TICKS(30000));
    net_state = NET_STATE_IDLE;

    for (;;) {
        if (xQueueReceive(xQ_PlayerMove, &player_mv, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (net_state == NET_STATE_IN_GAME) {
                lichess_post_move(game_id, player_mv.uci);

            } else if (net_state == NET_STATE_IDLE) {
                Serial.println("[net] seeking lichess game...");
                if (lichess_seek_game()) {
                    // TODO: extract real game_id from seek response
                    strncpy(game_id, "stubgameid1", sizeof(game_id) - 1);
                    net_state = NET_STATE_IN_GAME;
                    lichess_stream_game(game_id);
                    net_state = NET_STATE_IDLE;
                }
            }
        }

        if (WiFi.status() != WL_CONNECTED) {
            net_state = NET_STATE_DISCONNECTED;
            Serial.println("[net] dropped, reconnecting...");
            wifi_connect();
            net_state = NET_STATE_IDLE;
        }
    }

    vTaskDelete(NULL);
}


