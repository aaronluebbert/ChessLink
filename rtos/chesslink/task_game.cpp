#include "chesslink.h"

// internal state

typedef enum {
    PHASE_IDLE,
    PHASE_PIECE_LIFTED,
} MovePhase_t;

typedef struct {
    GameMode_t   mode;
    uint64_t     piece_positions;  // last confirmed occupancy bitmask
    uint8_t      active_color;     // 0=white, 1=black
    char         fen[92];
    char         last_move[6];
    char         status_msg[32];
    int16_t      eval_cp;

    // move detection state
    MovePhase_t  phase;
    uint8_t      lifted_sq;
} GameCtx_t;

// helpers

static void send_led_clear(void) {
    LedCmd_t cmd = { .type = LED_CMD_CLEAR };
    xQueueSend(xQ_LedCmd, &cmd, 0);
}

static void send_led_highlight(uint8_t sq, uint8_t r, uint8_t g, uint8_t b) {
    LedCmd_t cmd = { .type = LED_CMD_SET_SQUARE, .square = sq, .r = r, .g = g, .b = b };
    xQueueSend(xQ_LedCmd, &cmd, 0);
}

// source square in yellow, legal targets in green via PATTERN
// TODO: replace legal_mask with real move gen
static void show_legal_moves(uint8_t from_sq, uint64_t legal_mask) {
    LedCmd_t cmd = {
        .type = LED_CMD_PATTERN, .square = from_sq,
        .r = 0, .g = 200, .b = 0,
        .mask = legal_mask,
    };
    xQueueSend(xQ_LedCmd, &cmd, 0);
    send_led_highlight(from_sq, 200, 200, 0);  // yellow source
}

static void publish_game_state(const GameCtx_t *ctx) {
    GameState_t gs;
    gs.mode         = ctx->mode;
    gs.active_color = ctx->active_color;
    gs.eval_cp      = ctx->eval_cp;
    gs.occupied     = ctx->piece_positions;
    strncpy(gs.fen,        ctx->fen,        sizeof(gs.fen)        - 1);
    strncpy(gs.last_move,  ctx->last_move,  sizeof(gs.last_move)  - 1);
    strncpy(gs.status_msg, ctx->status_msg, sizeof(gs.status_msg) - 1);

    xQueueOverwrite(xQ_GameState, &gs);  // LCD drops stale frames
}

// move detection

// stub: every square is a legal destination
static uint64_t get_legal_destinations(uint8_t from_sq, const GameCtx_t *ctx) {
    (void)from_sq; (void)ctx;
    return 0xFFFFFFFFFFFFFFFFULL;
}

// stub: all moves legal
static bool is_legal_move(uint8_t from_sq, uint8_t to_sq, const GameCtx_t *ctx) {
    (void)from_sq; (void)to_sq; (void)ctx;
    return true;
}

static void sq_to_uci(uint8_t sq, char *out) {
    out[0] = 'a' + (sq % 8);
    out[1] = '1' + (sq / 8);
    out[2] = '\0';
}

static void process_board_change(GameCtx_t *ctx, const BoardState_t *bs) {
    uint64_t prev   = ctx->piece_positions;
    uint64_t curr   = bs->occupied;
    uint64_t lifted = prev & ~curr;
    uint64_t placed  = curr & ~prev;

    if (ctx->phase == PHASE_IDLE) {
        if (lifted && !placed) {
            ctx->lifted_sq = (uint8_t)__builtin_ctzll(lifted);
            ctx->phase     = PHASE_PIECE_LIFTED;
            show_legal_moves(ctx->lifted_sq, get_legal_destinations(ctx->lifted_sq, ctx));
        }

    } else if (ctx->phase == PHASE_PIECE_LIFTED) {

        if (placed) {
            uint8_t to_sq = (uint8_t)__builtin_ctzll(placed);

            if (is_legal_move(ctx->lifted_sq, to_sq, ctx)) {
                char from_str[3], to_str[3];
                sq_to_uci(ctx->lifted_sq, from_str);
                sq_to_uci(to_sq,           to_str);
                snprintf(ctx->last_move, sizeof(ctx->last_move), "%s%s", from_str, to_str);

                snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                         "move: %s  %s to move",
                         ctx->last_move,
                         ctx->active_color == 0 ? "black" : "white");

                ctx->active_color   ^= 1;
                ctx->piece_positions = curr;
                ctx->phase           = PHASE_IDLE;

                send_led_highlight(to_sq, 0, 255, 0);  // brief green flash

                if (ctx->mode == GAME_MODE_LICHESS) {
                    MoveEvent_t mv = { .src = MOVE_SRC_PLAYER, .from_sq = ctx->lifted_sq, .to_sq = to_sq };
                    strncpy(mv.uci, ctx->last_move, sizeof(mv.uci) - 1);
                    xQueueSend(xQ_PlayerMove, &mv, 0);
                }

                publish_game_state(ctx);
                vTaskDelay(pdMS_TO_TICKS(500));
                send_led_clear();

            } else {
                // illegal -- flash red, return to legal move hints
                send_led_highlight(to_sq, 255, 0, 0);
                vTaskDelay(pdMS_TO_TICKS(300));
                show_legal_moves(ctx->lifted_sq, get_legal_destinations(ctx->lifted_sq, ctx));
            }

        } else if (!lifted && !placed) {
            // piece returned to origin -- cancel
            ctx->phase           = PHASE_IDLE;
            ctx->piece_positions = curr;
            send_led_clear();
        }
    }
}

static void apply_opponent_move(GameCtx_t *ctx, const MoveEvent_t *mv) {
    ctx->piece_positions &= ~(1ULL << mv->from_sq);
    ctx->piece_positions |=  (1ULL << mv->to_sq);
    ctx->active_color    ^=  1;

    strncpy(ctx->last_move, mv->uci, sizeof(ctx->last_move) - 1);
    snprintf(ctx->status_msg, sizeof(ctx->status_msg), "opp: %s  your turn", mv->uci);

    // blue from, light blue to
    LedCmd_t cmd = { .type = LED_CMD_SET_SQUARE, .square = mv->from_sq, .r = 0, .g = 0, .b = 200 };
    xQueueSend(xQ_LedCmd, &cmd, 0);
    send_led_highlight(mv->to_sq, 0, 100, 255);

    publish_game_state(ctx);
}

// task

void task_GameLogic(void *pvParameters) {
    GameCtx_t ctx = {};
    ctx.mode           = GAME_MODE_LOCAL;
    ctx.active_color   = 0;
    ctx.phase          = PHASE_IDLE;
    ctx.piece_positions = 0;
    ctx.eval_cp        = INT16_MIN;
    strncpy(ctx.fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", sizeof(ctx.fen) - 1);
    strncpy(ctx.status_msg, "place pieces to begin", sizeof(ctx.status_msg) - 1);

    publish_game_state(&ctx);

    BoardState_t bs;
    MoveEvent_t  opp_mv;

    for (;;) {
        // block on board state, short timeout so opponent queue stays responsive
        if (xQueueReceive(xQ_BoardState, &bs, pdMS_TO_TICKS(50)) == pdTRUE)
            process_board_change(&ctx, &bs);

        if (xQueueReceive(xQ_OpponentMove, &opp_mv, 0) == pdTRUE)
            apply_opponent_move(&ctx, &opp_mv);
    }

    vTaskDelete(NULL);
}
