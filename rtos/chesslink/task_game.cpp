#include "chesslink.h"
#include "chess_engine.h"

// --- internal state ----------------------------------------------------------

typedef enum {
    PHASE_SETUP_BOARD,    // waiting for the pieces to be set in the start position
    PHASE_IDLE,
    PHASE_PIECE_LIFTED,
    PHASE_PROMO_SELECT,   // pawn reached back rank, waiting for piece choice
    PHASE_OPP_SYNC,       // opponent moved online -- waiting for the player to
                          // replay that move on the physical board
    PHASE_GAME_OVER,      // game finished -- showing the result, any button = menu
} MovePhase_t;

// promotion piece options in cycle order
static const PieceType PROMO_PIECES[4] = { QUEEN, ROOK, BISHOP, KNIGHT };
static const char      PROMO_NAMES[4]  = { 'q',   'r',  'b',    'n'   };

// max legal moves from a single square (a centralized queen tops out at 27)
#define CAND_MAX 40

// online rated-match time controls (minutes + increment seconds). shared with
// the display via the externs in chesslink.h
const TimePreset_t ONLINE_TIME_PRESETS[] = {
    {  1,  0, "1+0"   },
    {  3,  0, "3+0"   },
    {  3,  2, "3+2"   },
    {  5,  0, "5+0"   },
    {  5,  3, "5+3"   },
    { 10,  0, "10+0"  },
    { 15, 10, "15+10" },
};
const int ONLINE_TIME_COUNT = sizeof(ONLINE_TIME_PRESETS) / sizeof(ONLINE_TIME_PRESETS[0]);
#define CFG_DEFAULT_TIME_IDX 4   // 5+3

// bot time controls -- same idea plus an untimed option (min == 0)
const TimePreset_t BOT_TIME_PRESETS[] = {
    {  0,  0, "Untimed" },
    {  3,  2, "3+2"     },
    {  5,  3, "5+3"     },
    { 10,  0, "10+0"    },
    { 15, 10, "15+10"   },
};
const int BOT_TIME_COUNT = sizeof(BOT_TIME_PRESETS) / sizeof(BOT_TIME_PRESETS[0]);
#define BOT_DEFAULT_TIME_IDX 2   // 5+3
#define BOT_DEFAULT_LEVEL    3

typedef struct {
    Position    pos;
    GameMode_t  mode;
    char        status_msg[32];
    char        last_move[6];

    // top-level UI: sit in the menu until the user picks a mode
    bool        in_menu;
    bool        in_setup;      // captive-portal setup screen is showing
    bool        in_notice;     // transient status notice (e.g. "seeking...")
    bool        in_online_cfg; // online rated-match config submenu is showing
    bool        in_bot_cfg;    // play-computer config submenu is showing
    bool        confirm_exit;  // "leave game?" prompt is up
    uint8_t     menu_cursor;   // MENU_ITEM_*

    // config submenus
    uint8_t     cfg_cursor;    // CFG_ROW_* / BOT_ROW_*
    uint8_t     cfg_time_idx;  // index into the active time-preset list
    bool        cfg_rated;     // online
    uint8_t     cfg_level;     // bot 1..8
    uint8_t     cfg_color;     // bot 0=white 1=black 2=random

    // clock data (from lichess, 0 if local game)
    uint32_t    white_clock_ms;
    uint32_t    black_clock_ms;

    // live-match players (filled from the lichess gameFull event via NetUpdate)
    char        my_name[20];
    char        opp_name[20];
    uint16_t    my_rating;
    uint16_t    opp_rating;
    uint8_t     my_color;      // 0=white 1=black

    // latest board occupancy from the sensors, cached even in the menu so the
    // start-position check can see a static (already set up) board right away
    uint64_t    last_occupied;

    // move detection
    MovePhase_t phase;
    uint8_t     lifted_sq;
    uint8_t     promo_to_sq;   // destination square saved while picker is open
    BB          legal_dests;

    // candidate legal moves from lifted_sq + the occupancy each would leave.
    // a physical move is recognized when the sensors match one of these, which
    // is how captures, castling and en passant get detected
    Move        cand_moves[CAND_MAX];
    uint64_t    cand_occ[CAND_MAX];
    uint8_t     cand_count;

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
    gs.ui_screen    = ctx->in_setup                    ? UI_SETUP
                    : ctx->confirm_exit                ? UI_CONFIRM
                    : ctx->in_menu                     ? UI_MENU
                    : ctx->in_online_cfg               ? UI_ONLINE_CFG
                    : ctx->in_bot_cfg                  ? UI_BOT_CFG
                    : ctx->phase == PHASE_GAME_OVER    ? UI_GAMEOVER
                    : ctx->in_notice                   ? UI_NOTICE
                                                       : UI_GAME;
    gs.menu_cursor  = ctx->menu_cursor;
    gs.cfg_cursor   = ctx->cfg_cursor;
    gs.cfg_time_idx = ctx->cfg_time_idx;
    gs.cfg_rated    = ctx->cfg_rated;
    gs.cfg_level    = ctx->cfg_level;
    gs.cfg_color    = ctx->cfg_color;
    gs.active_color = (uint8_t)ctx->pos.side;
    gs.promo_state  = (ctx->phase == PHASE_PROMO_SELECT) ? PROMO_SELECTING : PROMO_NONE;
    gs.promo_cursor = ctx->promo_cursor;
    pos_to_fen(&ctx->pos, gs.fen, sizeof(gs.fen));
    strncpy(gs.last_move,  ctx->last_move,  sizeof(gs.last_move)  - 1);
    strncpy(gs.status_msg, ctx->status_msg, sizeof(gs.status_msg) - 1);
    gs.white_clock_ms = ctx->white_clock_ms;
    gs.black_clock_ms = ctx->black_clock_ms;
    strncpy(gs.my_name,  ctx->my_name,  sizeof(gs.my_name)  - 1);
    strncpy(gs.opp_name, ctx->opp_name, sizeof(gs.opp_name) - 1);
    gs.my_rating  = ctx->my_rating;
    gs.opp_rating = ctx->opp_rating;
    gs.my_color   = ctx->my_color;
    xQueueOverwrite(xQ_GameState, &gs);
}

// --- mode entry --------------------------------------------------------------

static const char *STARTPOS_FEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// show the mode-select menu, no game running
static void enter_menu(GameCtx_t *ctx) {
    ctx->in_menu       = true;
    ctx->in_notice     = false;
    ctx->in_setup      = false;
    ctx->in_online_cfg = false;
    ctx->in_bot_cfg    = false;
    ctx->mode    = GAME_MODE_IDLE;
    ctx->phase   = PHASE_IDLE;
    ctx->white_clock_ms = ctx->black_clock_ms = 0;
    send_led_clear();
    strncpy(ctx->status_msg, "select a mode", sizeof(ctx->status_msg) - 1);
    publish_game_state(ctx);
}

// gate play until the pieces sit in the start position. occ is the current
// sensor reading; ctx->pos is the freshly-loaded start position, so pos.all is
// the target occupancy (we haven't moved yet)
static void check_setup(GameCtx_t *ctx, uint64_t occ) {
    if (occ == ctx->pos.all) {
        ctx->phase = PHASE_IDLE;
        send_led_clear();
        strncpy(ctx->status_msg,
                ctx->mode == GAME_MODE_LOCAL ? "white to move" : "your move",
                sizeof(ctx->status_msg) - 1);
        publish_game_state(ctx);
    } else {
        // light the squares that still need a piece placed on them
        uint64_t missing = ctx->pos.all & ~occ;
        LedCmd_t cmd = { .type = LED_CMD_HILITE, .r = 0, .g = 160, .b = 0, .mask = missing };
        xQueueSend(xQ_LedCmd, &cmd, 0);
    }
}

// start a local, over-the-board game
static void start_local(GameCtx_t *ctx) {
    ctx->in_menu       = false;
    ctx->in_notice     = false;
    ctx->in_online_cfg = false;
    ctx->in_bot_cfg    = false;
    ctx->mode    = GAME_MODE_LOCAL;
    ctx->phase   = PHASE_SETUP_BOARD;
    ctx->white_clock_ms = ctx->black_clock_ms = 0;   // no clocks -> board screen
    pos_from_fen(&ctx->pos, STARTPOS_FEN);
    strncpy(ctx->status_msg, "Set up the board", sizeof(ctx->status_msg) - 1);
    send_led_clear();
    publish_game_state(ctx);
    check_setup(ctx, ctx->last_occupied);   // pass instantly if already set up
}

// open the online rated-match config submenu (time control + rated toggle).
// cfg_time_idx is shared between configs, so reset it to this mode's default
static void open_online_cfg(GameCtx_t *ctx) {
    ctx->in_menu       = false;
    ctx->in_online_cfg = true;
    ctx->cfg_cursor    = CFG_ROW_TIME;
    ctx->cfg_time_idx  = CFG_DEFAULT_TIME_IDX;
    publish_game_state(ctx);
}

// open the play-computer config submenu (level, time, color). level and color
// are remembered across opens; the time resets to the bot default
static void open_bot_cfg(GameCtx_t *ctx) {
    ctx->in_menu      = false;
    ctx->in_bot_cfg   = true;
    ctx->cfg_cursor   = BOT_ROW_LEVEL;
    ctx->cfg_time_idx = BOT_DEFAULT_TIME_IDX;
    publish_game_state(ctx);
}

// enter the seeking/notice state shared by the online and bot start paths
static void start_lichess_game(GameCtx_t *ctx, const NetCmdMsg_t *cmd, const char *notice) {
    ctx->in_menu       = false;
    ctx->in_online_cfg = false;
    ctx->in_bot_cfg    = false;
    ctx->in_notice     = true;
    ctx->mode    = GAME_MODE_LICHESS;
    ctx->phase   = PHASE_IDLE;
    pos_from_fen(&ctx->pos, STARTPOS_FEN);
    strncpy(ctx->status_msg, notice, sizeof(ctx->status_msg) - 1);
    send_led_clear();
    xQueueSend(xQ_NetCmd, cmd, 0);
    publish_game_state(ctx);
}

// start an online human game with the selected time control
static void start_online(GameCtx_t *ctx) {
    const TimePreset_t *tp = &ONLINE_TIME_PRESETS[ctx->cfg_time_idx];
    NetCmdMsg_t cmd = { .type = NET_CMD_START_ONLINE, .time_min = tp->min,
                        .inc_sec = tp->inc, .rated = ctx->cfg_rated };
    start_lichess_game(ctx, &cmd, "Seeking opponent...");
}

// start a game against Stockfish at the selected level/time/color
static void start_bot(GameCtx_t *ctx) {
    const TimePreset_t *tp = &BOT_TIME_PRESETS[ctx->cfg_time_idx];
    NetCmdMsg_t cmd = { .type = NET_CMD_START_BOT, .time_min = tp->min,
                        .inc_sec = tp->inc, .level = ctx->cfg_level, .color = ctx->cfg_color };
    start_lichess_game(ctx, &cmd, "Starting bot game...");
}

// open the WiFi/token captive portal -- the network task brings up the soft-AP
// and reports back when it's up (NET_STATUS_SETUP) and again once connected
static void open_setup(GameCtx_t *ctx) {
    ctx->in_setup = true;   // show the setup screen right away
    NetCmdMsg_t cmd = { .type = NET_CMD_OPEN_SETUP };
    xQueueSend(xQ_NetCmd, &cmd, 0);
    publish_game_state(ctx);
}

// leave the current game: resign an online/bot game (lichess sends the result
// back, which shows a result screen), or just drop a local game to the menu
static void leave_game(GameCtx_t *ctx) {
    ctx->confirm_exit = false;
    if (ctx->mode == GAME_MODE_LICHESS) {
        ctx->in_notice = true;
        strncpy(ctx->status_msg, "Leaving game...", sizeof(ctx->status_msg) - 1);
        send_led_clear();
        NetCmdMsg_t cmd = { .type = NET_CMD_RESIGN };
        xQueueSend(xQ_NetCmd, &cmd, 0);
        publish_game_state(ctx);
    } else {
        enter_menu(ctx);
    }
}

// --- game over ---------------------------------------------------------------

// show a result and stop play; any button returns to the menu from here
static void end_game(GameCtx_t *ctx, const char *result) {
    strncpy(ctx->status_msg, result, sizeof(ctx->status_msg) - 1);
    ctx->status_msg[sizeof(ctx->status_msg) - 1] = '\0';
    ctx->in_notice = false;
    ctx->phase = PHASE_GAME_OVER;
    send_led_clear();
    publish_game_state(ctx);
}

// local end-of-game test after a move: no legal reply means mate or stalemate;
// also the 50-move rule. online results come from lichess instead
static bool check_local_end(GameCtx_t *ctx) {
    Move moves[MAX_MOVES];
    if (gen_legal_moves(&ctx->pos, moves) == 0) {
        if (in_check(&ctx->pos, ctx->pos.side))
            end_game(ctx, ctx->pos.side == WHITE ? "Black wins: mate"
                                                 : "White wins: mate");
        else
            end_game(ctx, "Draw: stalemate");
        return true;
    }
    if (ctx->pos.halfmove >= 100) {
        end_game(ctx, "Draw: 50-move rule");
        return true;
    }
    return false;
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
        MoveEvent_t mv = { .from_sq = MV_FROM(chosen), .to_sq = MV_TO(chosen) };
        strncpy(mv.uci, uci, sizeof(mv.uci) - 1);
        xQueueSend(xQ_PlayerMove, &mv, 0);
    }

    publish_game_state(ctx);
    vTaskDelay(pdMS_TO_TICKS(500));
    send_led_clear();

    // local games decide their own result; online results come from lichess
    if (ctx->mode == GAME_MODE_LOCAL) check_local_end(ctx);
}

// --- board change handling ---------------------------------------------------
//
// physical moves are recognized by occupancy, not by piece identity. when a
// piece is lifted we precompute the board each of its legal moves would leave,
// then commit whichever one the sensors settle onto. that is what lets captures
// (destination stays occupied), castling (two pieces move) and en passant
// (a pawn vanishes off the target file) be detected -- a plain lifted/placed
// diff sees none of those correctly.

// board occupancy that playing m would leave, without disturbing ctx->pos
static uint64_t occ_after_move(const Position *pos, Move m) {
    Position tmp = *pos;
    Position undo;
    make_move_pos(&tmp, m, &undo);
    return tmp.all;
}

// a piece of the side to move left `from` -- gather its legal moves and the
// occupancy each would produce, then wait for the board to match one
static void begin_move(GameCtx_t *ctx, uint8_t from) {
    ctx->lifted_sq   = from;
    ctx->legal_dests = legal_destinations(&ctx->pos, from);

    Move buf[MAX_MOVES];
    int  n = gen_legal_moves_from(&ctx->pos, from, buf);

    ctx->cand_count = 0;
    for (int i = 0; i < n && ctx->cand_count < CAND_MAX; i++) {
        ctx->cand_moves[ctx->cand_count] = buf[i];
        ctx->cand_occ[ctx->cand_count]   = occ_after_move(&ctx->pos, buf[i]);
        ctx->cand_count++;
    }

    ctx->phase = PHASE_PIECE_LIFTED;
    show_legal_moves(from, ctx->legal_dests);
}

static void process_board_change(GameCtx_t *ctx, const BoardState_t *bs) {
    // ignore board changes while picking a promotion piece, on the leave prompt,
    // showing a transient notice (e.g. seeking), or after the game ends
    if (ctx->phase == PHASE_PROMO_SELECT || ctx->phase == PHASE_GAME_OVER
        || ctx->in_notice || ctx->confirm_exit) return;

    uint64_t prev = ctx->pos.all;
    uint64_t curr = bs->occupied;

    // waiting for the pieces to be placed in the start position before playing
    if (ctx->phase == PHASE_SETUP_BOARD) {
        check_setup(ctx, curr);
        return;
    }

    // an online opponent move is already applied to our truth, but the board is
    // still in the old position -- wait for the player to physically make it.
    // matching the full occupancy handles opponent captures/castling/en passant,
    // and ignores any half-finished intermediate states along the way
    if (ctx->phase == PHASE_OPP_SYNC) {
        if (curr == ctx->pos.all) {
            ctx->phase = PHASE_IDLE;
            send_led_clear();
            snprintf(ctx->status_msg, sizeof(ctx->status_msg), "your move");
            publish_game_state(ctx);
        }
        return;
    }

    if (ctx->phase == PHASE_IDLE) {
        // a move starts when a piece of the side to move leaves its square.
        // (an opponent piece lifted first during a capture is ignored until
        // the mover's own piece comes up)
        uint64_t lifted = prev & ~curr;
        while (lifted) {
            uint8_t sq = (uint8_t)__builtin_ctzll(lifted);
            lifted &= lifted - 1;
            if (ctx->pos.color_at[sq] == ctx->pos.side) {
                begin_move(ctx, sq);
                return;
            }
        }
        return;
    }

    // PHASE_PIECE_LIFTED -- has the board settled onto a legal move's result?
    for (int i = 0; i < ctx->cand_count; i++) {
        if (curr != ctx->cand_occ[i]) continue;

        Move m = ctx->cand_moves[i];
        if (MV_IS_PROMO(m)) {
            // Q/R/B/N all leave the same occupancy -- ask which piece
            ctx->promo_to_sq  = MV_TO(m);
            ctx->promo_cursor = 0;
            ctx->phase        = PHASE_PROMO_SELECT;
            send_led_sq(ctx->promo_to_sq, 180, 0, 200);
            snprintf(ctx->status_msg, sizeof(ctx->status_msg), "choose promotion piece");
            publish_game_state(ctx);
        } else {
            commit_move(ctx, m);
        }
        return;
    }

    // board back to the pre-move position -- the player changed their mind
    if (curr == prev) {
        ctx->phase = PHASE_IDLE;
        send_led_clear();
        return;
    }

    // a single own piece dropped on a non-legal square -- flag it and re-hint.
    // multi-step moves (captures, castling) pass through here mid-way with an
    // empty `placed` and are simply left to complete
    uint64_t lifted = prev & ~curr;
    uint64_t placed = curr & ~prev;
    if (placed && (placed & (placed - 1)) == 0
        && lifted == BB_SQ(ctx->lifted_sq)) {
        uint8_t to_sq = (uint8_t)__builtin_ctzll(placed);
        if (!(ctx->legal_dests & BB_SQ(to_sq))) {
            send_led_sq(to_sq, 255, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(300));
            show_legal_moves(ctx->lifted_sq, ctx->legal_dests);
        }
    }
}

// --- promotion picker helpers ------------------------------------------------

// light the destination in the current choice's color and update the status
static void promo_hint(GameCtx_t *ctx) {
    //   queen=gold, rook=cyan, bishop=orange, knight=magenta
    static const uint8_t colors[4][3] = {
        {220, 180,   0}, {  0, 200, 200}, {220, 100,   0}, {160,   0, 200},
    };
    const uint8_t *c = colors[ctx->promo_cursor];
    send_led_sq(ctx->promo_to_sq, c[0], c[1], c[2]);
    snprintf(ctx->status_msg, sizeof(ctx->status_msg),
             "promote to: %c", PROMO_NAMES[ctx->promo_cursor]);
    publish_game_state(ctx);
}

// commit the move with the chosen promotion piece
static void commit_promo(GameCtx_t *ctx) {
    PieceType chosen_piece = PROMO_PIECES[ctx->promo_cursor];
    Move moves[MAX_MOVES];
    int  cnt = gen_legal_moves_from(&ctx->pos, ctx->lifted_sq, moves);
    for (int i = 0; i < cnt; i++)
        if (MV_TO(moves[i]) == ctx->promo_to_sq && MV_PROMO(moves[i]) == chosen_piece) {
            commit_move(ctx, moves[i]);
            return;
        }
}

// --- buttons -----------------------------------------------------------------
//
// four buttons: UP / DOWN navigate, CONFIRM selects, CANCEL goes back. they
// drive the menus and the promotion picker; gameplay itself is on the board

static void handle_button(GameCtx_t *ctx, ButtonEvent_t evt) {
    // captive portal is phone-driven; transient notices clear themselves
    if (ctx->in_setup || ctx->in_notice) return;

    // "leave game?" prompt -- CONFIRM leaves, CANCEL resumes
    if (ctx->confirm_exit) {
        if (evt == BTN_EVT_CONFIRM)      leave_game(ctx);
        else if (evt == BTN_EVT_CANCEL) { ctx->confirm_exit = false; publish_game_state(ctx); }
        return;
    }

    // result screen -- any button returns to the menu
    if (ctx->phase == PHASE_GAME_OVER) { enter_menu(ctx); return; }

    // main mode-select menu
    if (ctx->in_menu) {
        switch (evt) {
            case BTN_EVT_UP:
                ctx->menu_cursor = (ctx->menu_cursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_DOWN:
                ctx->menu_cursor = (ctx->menu_cursor + 1) % MENU_ITEM_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_CONFIRM:
                if      (ctx->menu_cursor == MENU_ITEM_ONLINE) open_online_cfg(ctx);
                else if (ctx->menu_cursor == MENU_ITEM_BOT)    open_bot_cfg(ctx);
                else if (ctx->menu_cursor == MENU_ITEM_SETUP)  open_setup(ctx);
                else                                           start_local(ctx);
                break;
            case BTN_EVT_CANCEL: break;   // nothing above the top menu
        }
        return;
    }

    // online rated-match config: UP/DOWN pick a row, CONFIRM changes it or
    // starts the game, CANCEL backs out to the menu
    if (ctx->in_online_cfg) {
        switch (evt) {
            case BTN_EVT_UP:
                ctx->cfg_cursor = (ctx->cfg_cursor + CFG_ROW_COUNT - 1) % CFG_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_DOWN:
                ctx->cfg_cursor = (ctx->cfg_cursor + 1) % CFG_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_CONFIRM:
                if (ctx->cfg_cursor == CFG_ROW_TIME) {
                    ctx->cfg_time_idx = (ctx->cfg_time_idx + 1) % ONLINE_TIME_COUNT;
                    publish_game_state(ctx);
                } else if (ctx->cfg_cursor == CFG_ROW_RATED) {
                    ctx->cfg_rated = !ctx->cfg_rated;
                    publish_game_state(ctx);
                } else {
                    start_online(ctx);   // CFG_ROW_START
                }
                break;
            case BTN_EVT_CANCEL: enter_menu(ctx); break;
        }
        return;
    }

    // play-computer config: Level / Time / Color rows + Start
    if (ctx->in_bot_cfg) {
        switch (evt) {
            case BTN_EVT_UP:
                ctx->cfg_cursor = (ctx->cfg_cursor + BOT_ROW_COUNT - 1) % BOT_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_DOWN:
                ctx->cfg_cursor = (ctx->cfg_cursor + 1) % BOT_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_CONFIRM:
                if (ctx->cfg_cursor == BOT_ROW_LEVEL) {
                    ctx->cfg_level = (ctx->cfg_level >= BOT_LEVEL_MAX)
                                   ? BOT_LEVEL_MIN : ctx->cfg_level + 1;
                    publish_game_state(ctx);
                } else if (ctx->cfg_cursor == BOT_ROW_TIME) {
                    ctx->cfg_time_idx = (ctx->cfg_time_idx + 1) % BOT_TIME_COUNT;
                    publish_game_state(ctx);
                } else if (ctx->cfg_cursor == BOT_ROW_COLOR) {
                    ctx->cfg_color = (ctx->cfg_color + 1) % 3;   // white/black/random
                    publish_game_state(ctx);
                } else {
                    start_bot(ctx);   // BOT_ROW_START
                }
                break;
            case BTN_EVT_CANCEL: enter_menu(ctx); break;
        }
        return;
    }

    // promotion picker
    if (ctx->phase == PHASE_PROMO_SELECT) {
        switch (evt) {
            case BTN_EVT_UP:      ctx->promo_cursor = (ctx->promo_cursor + 3) % 4; promo_hint(ctx); break;
            case BTN_EVT_DOWN:    ctx->promo_cursor = (ctx->promo_cursor + 1) % 4; promo_hint(ctx); break;
            case BTN_EVT_CONFIRM: commit_promo(ctx); break;
            case BTN_EVT_CANCEL:  break;   // piece already placed -- keep picking
        }
        return;
    }

    // otherwise we're in an active game -- CANCEL asks to leave / resign
    if (evt == BTN_EVT_CANCEL) {
        ctx->confirm_exit = true;
        publish_game_state(ctx);
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
    make_move_pos(&ctx->pos, m, &undo);   // pos is now the post-opponent-move truth

    if (mv->white_clock_ms || mv->black_clock_ms) {
        ctx->white_clock_ms = mv->white_clock_ms;
        ctx->black_clock_ms = mv->black_clock_ms;
    }

    strncpy(ctx->last_move, mv->uci, sizeof(ctx->last_move) - 1);
    snprintf(ctx->status_msg, sizeof(ctx->status_msg), "opp: %s -- play it", mv->uci);

    // the board still shows the pre-move position -- light the opponent's move
    // and wait (PHASE_OPP_SYNC) until the player has made it for real
    ctx->phase = PHASE_OPP_SYNC;
    send_led_clear();
    send_led_sq(mv->from_sq, 0, 0, 200);    // source, blue
    send_led_sq(mv->to_sq,   0, 100, 255);  // destination, cyan

    publish_game_state(ctx);
}

// --- network status update ---------------------------------------------------
//
// clocks and player identities come from the network task, not the board.
// clocks re-sync here on every packet that carries them, the display counts
// down locally between updates

static void apply_net_update(GameCtx_t *ctx, const NetUpdate_t *u) {
    if (u->has_result) {          // lichess says the game is over
        end_game(ctx, u->result_text);
        return;
    }

    if (u->has_status) {
        if (u->status == NET_STATUS_SETUP) {
            ctx->in_setup = true;             // portal up -- show instructions
            publish_game_state(ctx);
        } else if (u->status == NET_STATUS_ONLINE && ctx->in_setup) {
            ctx->in_setup = false;            // connected -- back to the menu
            enter_menu(ctx);
        }
        return;
    }

    // the online game is starting -- drop the seeking notice and verify the
    // board sits in the start position before the first move
    if ((u->has_meta || u->has_clocks) && ctx->in_notice) {
        ctx->in_notice = false;
        ctx->phase = PHASE_SETUP_BOARD;
        strncpy(ctx->status_msg, "Set up the board", sizeof(ctx->status_msg) - 1);
        check_setup(ctx, ctx->last_occupied);
    }

    if (u->has_meta) {
        strncpy(ctx->my_name,  u->my_name,  sizeof(ctx->my_name)  - 1);
        ctx->my_name[sizeof(ctx->my_name)  - 1] = '\0';
        strncpy(ctx->opp_name, u->opp_name, sizeof(ctx->opp_name) - 1);
        ctx->opp_name[sizeof(ctx->opp_name) - 1] = '\0';
        ctx->my_rating  = u->my_rating;
        ctx->opp_rating = u->opp_rating;
        ctx->my_color   = u->my_color;
    }
    if (u->has_clocks) {
        ctx->white_clock_ms = u->white_clock_ms;
        ctx->black_clock_ms = u->black_clock_ms;
    }
    publish_game_state(ctx);
}

// --- task --------------------------------------------------------------------

void task_GameLogic(void *pvParameters) {
    GameCtx_t ctx = {};

    // placeholder names until a network game fills real ones
    // ratings and my_color stay 0 (white) from the {} init above
    strncpy(ctx.my_name,  "You",      sizeof(ctx.my_name)  - 1);
    strncpy(ctx.opp_name, "Opponent", sizeof(ctx.opp_name) - 1);

    // config defaults (remembered across the session)
    ctx.cfg_time_idx = CFG_DEFAULT_TIME_IDX;
    ctx.cfg_rated    = true;
    ctx.cfg_level    = BOT_DEFAULT_LEVEL;
    ctx.cfg_color    = 0;   // white

    // boot into the mode-select menu
    enter_menu(&ctx);

    BoardState_t  bs;
    MoveEvent_t   opp_mv;
    ButtonEvent_t btn_evt;
    NetUpdate_t   net_upd;

    for (;;) {
        // buttons drive both the menu and the promotion picker
        if (xQueueReceive(xQ_ButtonEvent, &btn_evt, 0) == pdTRUE)
            handle_button(&ctx, btn_evt);

        // always cache the latest board occupancy (even in the menu / notices) so
        // the start-position check can see an already-set-up board immediately.
        // the 20ms read timeout also paces this loop
        if (xQueueReceive(xQ_BoardState, &bs, pdMS_TO_TICKS(20)) == pdTRUE) {
            ctx.last_occupied = bs.occupied;
            if (!ctx.in_menu) process_board_change(&ctx, &bs);
        }

        if (ctx.in_menu) continue;

        // only take an opponent move when idle and not mid-prompt -- otherwise
        // it waits in the queue
        if (ctx.phase == PHASE_IDLE && !ctx.confirm_exit
            && xQueueReceive(xQ_OpponentMove, &opp_mv, 0) == pdTRUE)
            apply_opponent_move(&ctx, &opp_mv);

        // clocks + player names/ratings from the lichess stream
        if (xQueueReceive(xQ_NetUpdate, &net_upd, 0) == pdTRUE)
            apply_net_update(&ctx, &net_upd);
    }

    vTaskDelete(NULL);
}
