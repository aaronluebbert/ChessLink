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
