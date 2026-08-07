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
    PHASE_REPLAY,         // famous-game study -- waiting for the player to make the
                          // scripted move on the physical board
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

// local over-the-board time controls -- untimed plus common OTB controls
const TimePreset_t LOCAL_TIME_PRESETS[] = {
    {  0,  0, "Untimed" },
    {  3,  2, "3+2"     },
    {  5,  3, "5+3"     },
    { 10,  0, "10+0"    },
    { 15, 10, "15+10"   },
    { 30,  0, "30+0"    },
};
const int LOCAL_TIME_COUNT = sizeof(LOCAL_TIME_PRESETS) / sizeof(LOCAL_TIME_PRESETS[0]);
#define LOCAL_DEFAULT_TIME_IDX 0   // Untimed (preserves the old instant-start default)

// --- famous games (step-through study mode) ----------------------------------
// The Immortal Game -- Anderssen vs Kieseritzky, London 1851 -- in UCI.
static const char *IMMORTAL_MOVES[] = {
    "e2e4","e7e5","f2f4","e5f4","f1c4","d8h4","e1f1","b7b5","c4b5","g8f6",
    "g1f3","h4h6","d2d3","f6h5","f3h4","h6g5","h4f5","c7c6","g2g4","h5f6",
    "h1g1","c6b5","h2h4","g5g6","h4h5","g6g5","d1f3","f6g8","c1f4","g5f6",
    "b1c3","f8c5","c3d5","f6b2","f4d6","c5g1","e4e5","b2a1","f1e2","b8a6",
    "f5g7","e8d8","f3f6","g8f6","d6e7",
};
const FamousGame_t FAMOUS_GAMES[] = {
    { "Immortal Game", IMMORTAL_MOVES, (int)(sizeof(IMMORTAL_MOVES)/sizeof(IMMORTAL_MOVES[0])) },
};
const int FAMOUS_GAME_COUNT = (int)(sizeof(FAMOUS_GAMES) / sizeof(FAMOUS_GAMES[0]));

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
    bool        in_local_cfg;  // local over-the-board clock config submenu is showing
    bool        in_famous;     // famous-games list submenu is showing
    bool        confirm_exit;  // "leave game?" prompt is up
    uint8_t     menu_cursor;   // MENU_ITEM_*

    // famous-game replay (study mode)
    const char *const *replay_moves;  // UCI move list of the selected game
    int         replay_count;         // number of moves
    int         replay_idx;           // index of the move being made now
    bool        replay_hints;         // show the next move on the LEDs
    uint64_t    replay_pre_occ;       // board occupancy before the current move
    uint64_t    replay_target;        // board occupancy after the current move
    Move        replay_move;          // the current scripted move (applied on success)
    uint8_t     replay_from;
    uint8_t     replay_to;

    // config submenus
    uint8_t     cfg_cursor;    // CFG_ROW_* / BOT_ROW_*
    uint8_t     cfg_time_idx;  // index into the active time-preset list
    bool        cfg_rated;     // online
    uint8_t     cfg_level;     // bot 1..8
    uint8_t     cfg_color;     // bot 0=white 1=black 2=random

    // clock data (from lichess, or run locally for a timed OTB game)
    uint32_t    white_clock_ms;
    uint32_t    black_clock_ms;
    bool        local_timed;       // local game has clocks running
    uint32_t    local_inc_ms;      // increment added after each local move
    TickType_t  turn_start;        // tick the side-to-move's turn began (local clock)
    bool        untimed;           // no time control -> never show clocks, even if
                                   // the server still sends clock data

    // live-match players (filled from the lichess gameFull event via NetUpdate)
    char        my_name[20];
    char        opp_name[20];
    uint16_t    my_rating;
    uint16_t    opp_rating;
    uint8_t     my_color;      // 0=white 1=black

    // authoritative server-sync (online): a full position from lichess that arrived
    // while the board was still being set up, applied once setup completes
    bool        pending_sync;
    char        pending_fen[92];
    bool        pending_have_last;
    uint8_t     pending_from;
    uint8_t     pending_to;

    // latest board occupancy from the sensors, cached even in the menu so the
    // start-position check can see a static (already set up) board right away
    uint64_t    last_occupied;

    // move detection
    MovePhase_t phase;
    uint8_t     lifted_sq;
    uint8_t     promo_to_sq;   // destination square saved while picker is open
    BB          legal_dests;
    // every square that has been physically empty since the move began (origin
    // plus any captured square). used to disambiguate captures: two captures from
    // the same origin leave identical occupancy, so we require the captured square
    // to have actually been lifted before accepting that capture.
    uint64_t    lifted_mask;
    // true while red error squares are being shown for a board inconsistency
    // (piece moved out of turn / dropped on the wrong square) so idle knows to
    // clear them once the board is put right again.
    bool        board_error;

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
        .type = LED_CMD_HILITE,          // only the legal squares lit, rest dark
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
                    : ctx->in_local_cfg                ? UI_LOCAL_CFG
                    : ctx->in_famous                   ? UI_FAMOUS
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
    if (ctx->untimed || ctx->phase == PHASE_SETUP_BOARD) {
        // untimed game (or still setting up) -> never show a clock screen, even if
        // lichess keeps sending clock data
        gs.white_clock_ms = gs.black_clock_ms = 0;
    } else if (ctx->local_timed &&
        (ctx->phase == PHASE_IDLE || ctx->phase == PHASE_PIECE_LIFTED
         || ctx->phase == PHASE_PROMO_SELECT)) {
        // publish the side-to-move's LIVE remaining time so the display's own
        // tick-down always starts from the accurate value
        uint32_t elapsed = (xTaskGetTickCount() - ctx->turn_start) * portTICK_PERIOD_MS;
        if (ctx->pos.side == WHITE)
            gs.white_clock_ms = (ctx->white_clock_ms > elapsed) ? ctx->white_clock_ms - elapsed : 0;
        else
            gs.black_clock_ms = (ctx->black_clock_ms > elapsed) ? ctx->black_clock_ms - elapsed : 0;
    }
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
    ctx->in_local_cfg  = false;
    ctx->in_famous     = false;
    ctx->mode    = GAME_MODE_IDLE;
    ctx->phase   = PHASE_IDLE;
    ctx->local_timed = false;
    ctx->untimed     = false;
    ctx->white_clock_ms = ctx->black_clock_ms = 0;
    send_led_clear();
    strncpy(ctx->status_msg, "select a mode", sizeof(ctx->status_msg) - 1);
    publish_game_state(ctx);
}

// forward declarations (defined further down, needed by check_setup)
static void end_game(GameCtx_t *ctx, const char *result);
static void replay_present_move(GameCtx_t *ctx);

// Adopt an authoritative position from the server (online play). The board always
// defers to this: ctx->pos becomes the server's truth, and if the physical board
// is behind, we guide the player to catch it up (PHASE_OPP_SYNC).
static void reconcile_to_server(GameCtx_t *ctx, const char *fen, bool have_last,
                                uint8_t from, uint8_t to) {
    Position sp;
    pos_from_fen(&sp, fen);
    ctx->pos = sp;                       // the server is authoritative, always
    ctx->board_error = false;            // any prior disturbance is moot now

    if (have_last)
        snprintf(ctx->last_move, sizeof(ctx->last_move), "%c%c%c%c",
                 'a' + (from & 7), '1' + (from >> 3),
                 'a' + (to   & 7), '1' + (to   >> 3));

    if (ctx->last_occupied == sp.all) {
        // physical board already matches the server -> ready to play or wait
        ctx->phase = PHASE_IDLE;
        send_led_clear();
        snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                 ((uint8_t)sp.side == ctx->my_color) ? "your move" : "opponent to move");
    } else {
        // board is behind the server -> make the shown move on the board
        ctx->phase = PHASE_OPP_SYNC;
        send_led_clear();
        if (have_last) {
            send_led_sq(from, 0, 0, 200);      // source, blue
            send_led_sq(to,   0, 100, 255);    // destination, cyan
        }
        snprintf(ctx->status_msg, sizeof(ctx->status_msg), "play the move shown");
    }
    publish_game_state(ctx);
}

// gate play until the pieces sit in the start position. occ is the current
// sensor reading; ctx->pos is the freshly-loaded start position, so pos.all is
// the target occupancy (we haven't moved yet)
static void check_setup(GameCtx_t *ctx, uint64_t occ) {
    uint64_t missing = ctx->pos.all & ~occ;   // start square with no piece -> green
    uint64_t extra   = occ & ~ctx->pos.all;   // a piece where none belongs -> red

    if (missing == 0 && extra == 0) {         // board matches the start position
        ctx->phase = PHASE_IDLE;
        ctx->turn_start = xTaskGetTickCount();   // white's clock starts now (if timed)
        send_led_clear();

        // an online game may have advanced (opponent moved) while we were setting
        // up -- reconcile to the server's position now that the board is ready
        if (ctx->pending_sync) {
            ctx->pending_sync = false;
            reconcile_to_server(ctx, ctx->pending_fen, ctx->pending_have_last,
                                ctx->pending_from, ctx->pending_to);
            return;
        }

        // famous-game study -- board is set, present the first scripted move
        if (ctx->mode == GAME_MODE_REPLAY) {
            replay_present_move(ctx);
            return;
        }

        strncpy(ctx->status_msg,
                ctx->mode == GAME_MODE_LOCAL ? "white to move" : "your move",
                sizeof(ctx->status_msg) - 1);
        publish_game_state(ctx);
        return;
    }

    // Show what's blocking the start: green = still needs a piece, red = a piece
    // sitting on a square that should be empty (misplaced piece or a stuck sensor).
    LedCmd_t g = { .type = LED_CMD_HILITE, .r = 0, .g = 160, .b = 0, .mask = missing };
    xQueueSend(xQ_LedCmd, &g, 0);
    for (int sq = 0; sq < 64; sq++) {
        if (extra & (1ULL << sq)) {
            LedCmd_t rd = { .type = LED_CMD_SET_SQUARE, .square = (uint8_t)sq,
                            .r = 160, .g = 0, .b = 0 };
            xQueueSend(xQ_LedCmd, &rd, 0);
        }
    }
    Serial.printf("[setup] occ=%016llX target=%016llX missing=%016llX extra=%016llX\n",
                  (unsigned long long)occ,      (unsigned long long)ctx->pos.all,
                  (unsigned long long)missing,  (unsigned long long)extra);
}

// open the local over-the-board config submenu (clock control + start).
// cfg_time_idx is shared between configs, so reset it to this mode's default
static void open_local_cfg(GameCtx_t *ctx) {
    ctx->in_menu      = false;
    ctx->in_local_cfg = true;
    ctx->cfg_cursor   = LOCAL_ROW_TIME;
    ctx->cfg_time_idx = LOCAL_DEFAULT_TIME_IDX;
    publish_game_state(ctx);
}

// start a local, over-the-board game using the selected time control
static void start_local(GameCtx_t *ctx) {
    const TimePreset_t *tp = &LOCAL_TIME_PRESETS[ctx->cfg_time_idx];

    ctx->in_menu       = false;
    ctx->in_notice     = false;
    ctx->in_online_cfg = false;
    ctx->in_bot_cfg    = false;
    ctx->in_local_cfg  = false;
    ctx->mode    = GAME_MODE_LOCAL;
    ctx->phase   = PHASE_SETUP_BOARD;

    ctx->untimed = (tp->min == 0);
    if (tp->min > 0) {                       // timed OTB game -> match/clock screen
        ctx->local_timed    = true;
        ctx->local_inc_ms   = (uint32_t)tp->inc * 1000;
        ctx->white_clock_ms = ctx->black_clock_ms = (uint32_t)tp->min * 60000;
        // label the two sides for the match screen (white at the bottom)
        ctx->my_color = 0;
        strncpy(ctx->my_name,  "White", sizeof(ctx->my_name)  - 1); ctx->my_name[sizeof(ctx->my_name)-1]  = '\0';
        strncpy(ctx->opp_name, "Black", sizeof(ctx->opp_name) - 1); ctx->opp_name[sizeof(ctx->opp_name)-1] = '\0';
        ctx->my_rating = ctx->opp_rating = 0;
    } else {                                 // untimed -> plain board screen
        ctx->local_timed    = false;
        ctx->white_clock_ms = ctx->black_clock_ms = 0;
    }

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
    ctx->pending_sync = false;
    pos_from_fen(&ctx->pos, STARTPOS_FEN);
    strncpy(ctx->status_msg, notice, sizeof(ctx->status_msg) - 1);
    send_led_clear();
    xQueueSend(xQ_NetCmd, cmd, 0);
    publish_game_state(ctx);
}

// start an online human game with the selected time control
static void start_online(GameCtx_t *ctx) {
    const TimePreset_t *tp = &ONLINE_TIME_PRESETS[ctx->cfg_time_idx];
    ctx->untimed = (tp->min == 0);
    NetCmdMsg_t cmd = { .type = NET_CMD_START_ONLINE, .time_min = tp->min,
                        .inc_sec = tp->inc, .rated = ctx->cfg_rated };
    start_lichess_game(ctx, &cmd, "Seeking opponent...");
}

// start a game against Stockfish at the selected level/time/color
static void start_bot(GameCtx_t *ctx) {
    const TimePreset_t *tp = &BOT_TIME_PRESETS[ctx->cfg_time_idx];
    ctx->untimed = (tp->min == 0);   // "Untimed" -> hide clocks even if lichess sends them
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

// --- famous-game study mode --------------------------------------------------

// redraw the LEDs for the current replay step: the scripted move (if hints are
// on) in blue/cyan, plus any squares the player has wrongly disturbed in red
static void replay_render(GameCtx_t *ctx, uint64_t err) {
    send_led_clear();
    if (ctx->replay_hints) {
        send_led_sq(ctx->replay_from, 0, 0, 200);     // source, blue
        send_led_sq(ctx->replay_to,   0, 100, 255);   // destination, cyan
    }
    uint64_t e = err;
    while (e) {
        uint8_t sq = (uint8_t)__builtin_ctzll(e);
        e &= e - 1;
        send_led_sq(sq, 220, 0, 0);                    // wrong piece, red
    }
}

// present the move at ctx->replay_idx: precompute the before/after occupancy and
// wait (PHASE_REPLAY) for the player to make it on the board
static void replay_present_move(GameCtx_t *ctx) {
    if (ctx->replay_idx >= ctx->replay_count) {        // reached the end
        end_game(ctx, "Game complete!");
        return;
    }

    const char *uci = ctx->replay_moves[ctx->replay_idx];
    Move m = uci_to_move(&ctx->pos, uci);
    if (m == MOVE_NONE) { end_game(ctx, "replay data error"); return; }

    ctx->replay_move    = m;
    ctx->replay_pre_occ = ctx->pos.all;
    ctx->replay_from    = (uint8_t)((uci[0] - 'a') + (uci[1] - '1') * 8);
    ctx->replay_to      = (uint8_t)((uci[2] - 'a') + (uci[3] - '1') * 8);

    // occupancy the board should reach once this move is played
    Position after = ctx->pos, undo;
    make_move_pos(&after, m, &undo);
    ctx->replay_target = after.all;

    ctx->phase = PHASE_REPLAY;
    strncpy(ctx->last_move, uci, sizeof(ctx->last_move) - 1);
    ctx->last_move[sizeof(ctx->last_move) - 1] = '\0';
    snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Move %d/%d   Hints:%s",
             ctx->replay_idx + 1, ctx->replay_count, ctx->replay_hints ? "ON" : "OFF");
    replay_render(ctx, 0);
    publish_game_state(ctx);
}

// enter study mode for the chosen famous game -- run the normal board setup first
static void start_replay(GameCtx_t *ctx, int game_idx) {
    if (game_idx < 0 || game_idx >= FAMOUS_GAME_COUNT) return;
    const FamousGame_t *g = &FAMOUS_GAMES[game_idx];

    ctx->in_menu = ctx->in_notice = ctx->in_online_cfg = false;
    ctx->in_bot_cfg = ctx->in_local_cfg = ctx->in_famous = false;
    ctx->mode          = GAME_MODE_REPLAY;
    ctx->phase         = PHASE_SETUP_BOARD;
    ctx->local_timed   = false;
    ctx->untimed       = true;             // never a clock screen in study mode
    ctx->white_clock_ms = ctx->black_clock_ms = 0;
    ctx->replay_moves  = g->moves;
    ctx->replay_count  = g->count;
    ctx->replay_idx    = 0;
    ctx->replay_hints  = true;             // start with the move shown

    pos_from_fen(&ctx->pos, STARTPOS_FEN);
    strncpy(ctx->status_msg, "Set up the board", sizeof(ctx->status_msg) - 1);
    send_led_clear();
    publish_game_state(ctx);
    check_setup(ctx, ctx->last_occupied);  // in case the board is already set up
}

// open the famous-games list submenu
static void open_famous(GameCtx_t *ctx) {
    ctx->in_menu   = false;
    ctx->in_famous = true;
    ctx->cfg_cursor = 0;                    // selected game index
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

    // local clock: charge the mover for their think time, add the increment, then
    // hand the clock to the other side (make_move_pos flips ctx->pos.side below)
    if (ctx->local_timed) {
        Color    mover   = ctx->pos.side;
        uint32_t elapsed = (xTaskGetTickCount() - ctx->turn_start) * portTICK_PERIOD_MS;
        uint32_t *clk    = (mover == WHITE) ? &ctx->white_clock_ms : &ctx->black_clock_ms;
        *clk = (*clk > elapsed) ? (*clk - elapsed) : 0;
        if (*clk > 0) *clk += ctx->local_inc_ms;
        ctx->turn_start = xTaskGetTickCount();
    }

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
    ctx->board_error = false;        // a real move is starting -- drop any error hint
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

    // famous-game study: wait for the scripted move to be made on the board.
    if (ctx->phase == PHASE_REPLAY) {
        if (curr == ctx->replay_target) {
            Position undo;
            make_move_pos(&ctx->pos, ctx->replay_move, &undo);  // advance the truth
            ctx->replay_idx++;
            replay_present_move(ctx);                            // present the next move
            return;
        }
        // the two squares of the scripted move may legitimately be in flux; any
        // OTHER square that differs from the target holds a wrongly-placed piece
        uint64_t move_sq = (ctx->replay_pre_occ ^ ctx->replay_target)
                         | BB_SQ(ctx->replay_from) | BB_SQ(ctx->replay_to);
        uint64_t err = (curr ^ ctx->replay_target) & ~move_sq;
        replay_render(ctx, err);
        return;
    }

    if (ctx->phase == PHASE_IDLE) {
        uint64_t missing = prev & ~curr;   // truth says occupied, board is empty
        uint64_t extra   = curr & ~prev;   // board occupied, truth says empty

        // board matches the game again -> clear any lingering error indicator
        if (missing == 0 && extra == 0) {
            if (ctx->board_error) {
                ctx->board_error = false;
                send_led_clear();
                const char *msg =
                    (ctx->mode == GAME_MODE_LOCAL)
                        ? (ctx->pos.side == WHITE ? "white to move" : "black to move")
                    : ((uint8_t)ctx->pos.side != ctx->my_color)
                        ? "wait for opponent's move"
                        : "your move";
                strncpy(ctx->status_msg, msg, sizeof(ctx->status_msg) - 1);
                ctx->status_msg[sizeof(ctx->status_msg) - 1] = '\0';
                publish_game_state(ctx);
            }
            return;
        }

        // ONLINE/BOT: while it's the opponent's turn (we've moved, the server
        // hasn't sent their reply yet) it is NOT our move. Don't start a move --
        // any piece the player disturbs is flagged red until the board is put back
        // or the opponent's move arrives. This also keeps us in PHASE_IDLE so the
        // incoming move still gets shown (a lifted piece would have hidden it).
        if (ctx->mode == GAME_MODE_LICHESS && (uint8_t)ctx->pos.side != ctx->my_color) {
            LedCmd_t cmd = { .type = LED_CMD_HILITE, .r = 200, .g = 0, .b = 0,
                             .mask = missing | extra };
            xQueueSend(xQ_LedCmd, &cmd, 0);
            ctx->board_error = true;
            snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                     "wait for opponent's move");
            publish_game_state(ctx);
            return;
        }

        // a move starts when a piece of the side to move is lifted. (an opponent
        // piece lifted first during a capture is fine -- ignored here until the
        // mover's own piece comes up.)
        uint64_t t = missing;
        while (t) {
            uint8_t sq = (uint8_t)__builtin_ctzll(t);
            t &= t - 1;
            if (ctx->pos.color_at[sq] == ctx->pos.side) {
                ctx->lifted_mask = missing;   // origin (+ any enemy already lifted)
                begin_move(ctx, sq);
                return;
            }
        }

        // no piece of the side to move has been lifted, yet something is out of
        // place. if a piece has been set down somewhere (extra), the board is
        // inconsistent -- a piece was moved out of turn, or dropped on the wrong
        // square. light where it wrongly sits AND where it should go back, in red.
        // (if only opponent pieces are lifted with nothing placed, stay quiet --
        // that's the captured piece being removed ahead of a capture.)
        if (extra) {
            LedCmd_t cmd = { .type = LED_CMD_HILITE, .r = 200, .g = 0, .b = 0,
                             .mask = missing | extra };
            xQueueSend(xQ_LedCmd, &cmd, 0);
            ctx->board_error = true;
            snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                     "not your turn - put the red piece back");
            publish_game_state(ctx);
        }
        return;
    }

    // PHASE_PIECE_LIFTED -- has the board settled onto a legal move's result?
    ctx->lifted_mask |= (prev & ~curr);   // remember every square lifted so far

    for (int i = 0; i < ctx->cand_count; i++) {
        if (curr != ctx->cand_occ[i]) continue;

        Move m = ctx->cand_moves[i];
        // A capture leaves the same occupancy as any other capture from this
        // origin, so occupancy alone can't tell them apart -- and an illegal
        // capture (e.g. one that leaves the king in check) can collide with a
        // legal one and commit the wrong move. Require the captured square to
        // have physically been lifted before accepting a capture.
        if ((ctx->pos.all & BB_SQ(MV_TO(m))) && !(ctx->lifted_mask & BB_SQ(MV_TO(m))))
            continue;

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

    // Any piece now sitting on a square that isn't a legal destination of the
    // lifted piece is a problem: either the mover was dropped on an illegal
    // square, or a stray piece is out of place and blocking the move. Redraw the
    // legal targets (green) + source (yellow) and mark every offending square red
    // so the player can see exactly what to fix. Multi-step moves (captures,
    // castling, en passant) pass through here mid-way with nothing newly placed
    // (placed == 0) and are simply left to complete.
    uint64_t missing = prev & ~curr;
    uint64_t placed  = curr & ~prev;
    // squares that don't belong to the move in progress get flagged red:
    //  - placed pieces on a non-legal square (the wrong-colour piece's TO, or the
    //    mover dropped somewhere illegal), and
    //  - lifted pieces that are neither the mover's own piece nor a legal target
    //    (the wrong-colour piece's FROM).
    // a real capture's target square is a legal destination, so it's excluded and
    // never flags -- the captured square just goes quietly empty then filled.
    uint64_t bad = (placed  &  ~ctx->legal_dests)
                 | (missing &  ~ctx->legal_dests & ~BB_SQ(ctx->lifted_sq));
    if (bad) {
        show_legal_moves(ctx->lifted_sq, ctx->legal_dests);   // green targets + yellow source
        uint64_t b = bad;
        while (b) {
            uint8_t sq = (uint8_t)__builtin_ctzll(b);
            b &= b - 1;
            send_led_sq(sq, 220, 0, 0);                        // red: doesn't belong here
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
                else if (ctx->menu_cursor == MENU_ITEM_FAMOUS) open_famous(ctx);
                else if (ctx->menu_cursor == MENU_ITEM_SETUP)  open_setup(ctx);
                else                                           open_local_cfg(ctx);
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

    // local over-the-board config: Time row + Start
    if (ctx->in_local_cfg) {
        switch (evt) {
            case BTN_EVT_UP:
                ctx->cfg_cursor = (ctx->cfg_cursor + LOCAL_ROW_COUNT - 1) % LOCAL_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_DOWN:
                ctx->cfg_cursor = (ctx->cfg_cursor + 1) % LOCAL_ROW_COUNT;
                publish_game_state(ctx); break;
            case BTN_EVT_CONFIRM:
                if (ctx->cfg_cursor == LOCAL_ROW_TIME) {
                    ctx->cfg_time_idx = (ctx->cfg_time_idx + 1) % LOCAL_TIME_COUNT;
                    publish_game_state(ctx);
                } else {
                    start_local(ctx);   // LOCAL_ROW_START
                }
                break;
            case BTN_EVT_CANCEL: enter_menu(ctx); break;
        }
        return;
    }

    // famous-games list: UP/DOWN pick a game, CONFIRM starts it, CANCEL backs out
    if (ctx->in_famous) {
        switch (evt) {
            case BTN_EVT_UP:
                ctx->cfg_cursor = (uint8_t)((ctx->cfg_cursor + FAMOUS_GAME_COUNT - 1) % FAMOUS_GAME_COUNT);
                publish_game_state(ctx); break;
            case BTN_EVT_DOWN:
                ctx->cfg_cursor = (uint8_t)((ctx->cfg_cursor + 1) % FAMOUS_GAME_COUNT);
                publish_game_state(ctx); break;
            case BTN_EVT_CONFIRM: start_replay(ctx, ctx->cfg_cursor); break;
            case BTN_EVT_CANCEL:  enter_menu(ctx); break;
        }
        return;
    }

    // famous-game study: CONFIRM toggles the on-board move hint, CANCEL leaves
    if (ctx->phase == PHASE_REPLAY) {
        if (evt == BTN_EVT_CANCEL) {
            ctx->confirm_exit = true;
            publish_game_state(ctx);
        } else if (evt == BTN_EVT_CONFIRM) {
            ctx->replay_hints = !ctx->replay_hints;
            snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Move %d/%d   Hints:%s",
                     ctx->replay_idx + 1, ctx->replay_count, ctx->replay_hints ? "ON" : "OFF");
            uint64_t move_sq = (ctx->replay_pre_occ ^ ctx->replay_target)
                             | BB_SQ(ctx->replay_from) | BB_SQ(ctx->replay_to);
            uint64_t err = (ctx->last_occupied ^ ctx->replay_target) & ~move_sq;
            replay_render(ctx, err);
            publish_game_state(ctx);
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

// authoritative full-position sync from the server (online play only)
static void apply_server_state(GameCtx_t *ctx, const NetUpdate_t *u) {
    // still placing the pieces at the start position -- stash the server's state
    // and reconcile the moment setup completes (see check_setup)
    if (ctx->phase == PHASE_SETUP_BOARD) {
        ctx->pending_sync = true;
        strncpy(ctx->pending_fen, u->sync_fen, sizeof(ctx->pending_fen) - 1);
        ctx->pending_fen[sizeof(ctx->pending_fen) - 1] = '\0';
        ctx->pending_have_last = u->sync_have_last;
        ctx->pending_from      = u->sync_from;
        ctx->pending_to        = u->sync_to;
        return;
    }
    // don't yank the truth out from under a move in progress; lichess resends the
    // full state, so we'll reconcile on the next packet
    if (ctx->phase == PHASE_PIECE_LIFTED || ctx->phase == PHASE_PROMO_SELECT)
        return;
    reconcile_to_server(ctx, u->sync_fen, u->sync_have_last, u->sync_from, u->sync_to);
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
            ctx->in_setup  = true;            // portal up -- show instructions
            ctx->in_notice = false;
            publish_game_state(ctx);
        } else if (u->status == NET_STATUS_CONNECTING) {
            ctx->in_setup  = false;           // creds submitted -- joining WiFi
            ctx->in_notice = true;
            strncpy(ctx->status_msg, "Connecting to WiFi...", sizeof(ctx->status_msg) - 1);
            publish_game_state(ctx);
        } else if (u->status == NET_STATUS_ONLINE) {
            if (ctx->in_setup || ctx->in_notice) {
                ctx->in_setup = false;        // connected -- back to the menu
                enter_menu(ctx);              // also clears in_notice
            }
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

    // authoritative position from the server -- the board defers to it every time
    if (u->has_sync) {
        apply_server_state(ctx, u);
        return;
    }

    publish_game_state(ctx);
}

// local timed game: end the game the moment the side to move runs out of time
static void check_local_flag(GameCtx_t *ctx) {
    if (!ctx->local_timed) return;
    if (ctx->phase != PHASE_IDLE && ctx->phase != PHASE_PIECE_LIFTED
        && ctx->phase != PHASE_PROMO_SELECT) return;
    uint32_t clk     = (ctx->pos.side == WHITE) ? ctx->white_clock_ms : ctx->black_clock_ms;
    uint32_t elapsed = (xTaskGetTickCount() - ctx->turn_start) * portTICK_PERIOD_MS;
    if (elapsed >= clk) {
        if (ctx->pos.side == WHITE) ctx->white_clock_ms = 0; else ctx->black_clock_ms = 0;
        ctx->local_timed = false;   // stop the clock
        end_game(ctx, ctx->pos.side == WHITE ? "Black wins: time" : "White wins: time");
    }
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
            // only track pieces during an actual game/setup -- not in the menu or
            // any submenu (all of which leave the mode at GAME_MODE_IDLE)
            if (ctx.mode != GAME_MODE_IDLE) process_board_change(&ctx, &bs);
        }

        // network updates are handled in EVERY state (before the menu skip below)
        // so a setup/connected/error status can update the screen -- e.g. clear
        // the WiFi-setup screen once the board connects, or in-game clocks/results
        if (xQueueReceive(xQ_NetUpdate, &net_upd, 0) == pdTRUE)
            apply_net_update(&ctx, &net_upd);

        check_local_flag(&ctx);   // end a local timed game on flag-fall

        if (ctx.in_menu) continue;

        // only take an opponent move when idle and not mid-prompt -- otherwise
        // it waits in the queue
        if (ctx.phase == PHASE_IDLE && !ctx.confirm_exit
            && xQueueReceive(xQ_OpponentMove, &opp_mv, 0) == pdTRUE)
            apply_opponent_move(&ctx, &opp_mv);
    }

    vTaskDelete(NULL);
}
