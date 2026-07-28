#include "chesslink.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// --- display setup -----------------------------------------------------------

static SPIClass hspi(HSPI);
static Adafruit_ST7789 tft = Adafruit_ST7789(&hspi, LCD_CS, LCD_DC, LCD_RST);

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

#define C_BG          C_BLACK
#define C_HEADER_BG   0x0390
#define C_TEXT        C_WHITE
#define C_ACCENT      C_YELLOW
#define C_DIM         C_DARK_GRAY
#define C_SQ_LIGHT    0xF7BE
#define C_SQ_DARK     0x9A40

// --- layout ------------------------------------------------------------------
//
//  y=0    header (28px)
//  y=28   turn indicator (20px)
//  y=48   last move (18px)
//  y=66   status (18px)
//  y=84   board (160x160, 20px/sq, x=5)
//  y=244  padding
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
#define BOARD_PX    (BOARD_SQ * 8)
#define BOARD_X     ((D_W - BOARD_PX) / 2)
#define BOARD_Y     84
#define EVAL_Y      260
#define EVAL_H      20

// --- FEN board parser --------------------------------------------------------
//
// parses the piece-placement section of a FEN string into a 64-byte array
// where each entry is a piece char ('P','n','K', etc.) or 0 for empty.
// used by draw_mini_board to render the right piece on each square.

static void fen_to_squares(const char *fen, char *squares) {
    memset(squares, 0, 64);
    int rank = 7, file = 0;
    for (const char *p = fen; *p && *p != ' '; p++) {
        char c = *p;
        if (c == '/') { rank--; file = 0; }
        else if (c >= '1' && c <= '8') { file += c - '0'; }
        else {
            if (file < 8 && rank >= 0)
                squares[rank * 8 + file] = c;
            file++;
        }
    }
}

// --- board drawing -----------------------------------------------------------

// draw one square at pixel coords (px, py) with the given piece char (0 = empty)
// white pieces = uppercase, black pieces = lowercase
static void draw_board_square(int px, int py, bool light_sq, char piece) {
    uint16_t sq_color = light_sq ? C_SQ_LIGHT : C_SQ_DARK;
    tft.fillRect(px, py, BOARD_SQ, BOARD_SQ, sq_color);

    if (!piece) return;

    bool is_white = (piece >= 'A' && piece <= 'Z');
    char upper    = (piece >= 'a') ? piece - 32 : piece;

    // draw piece circle -- white pieces: white fill, dark outline
    //                       black pieces: dark fill, white outline
    uint16_t fill    = is_white ? C_WHITE     : 0x2945;
    uint16_t outline = is_white ? C_DARK_GRAY : C_WHITE;
    int cx = px + BOARD_SQ / 2;
    int cy = py + BOARD_SQ / 2;
    tft.fillCircle(cx, cy, BOARD_SQ / 2 - 2, fill);
    tft.drawCircle(cx, cy, BOARD_SQ / 2 - 2, outline);

    // letter inside circle -- identifies piece type
    // setTextSize(1) gives 6x8px characters, center them in the circle
    tft.setTextSize(1);
    tft.setTextColor(is_white ? C_BLACK : C_WHITE);
    tft.setCursor(cx - 3, cy - 4);
    tft.print(upper);
}

static void draw_mini_board(const char *fen) {
    char squares[64];
    fen_to_squares(fen, squares);

    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            int px = BOARD_X + col * BOARD_SQ;
            int py = BOARD_Y + (7 - row) * BOARD_SQ;  // rank 1 at bottom
            draw_board_square(px, py, (row + col) % 2 == 0,
                              squares[row * 8 + col]);
        }
    }
    tft.drawRect(BOARD_X - 1, BOARD_Y - 1, BOARD_PX + 2, BOARD_PX + 2, C_LIGHT_GRAY);
}

// --- game screen draw functions ----------------------------------------------

static void draw_header(GameMode_t mode) {
    tft.fillRect(0, HEADER_Y, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("ChessLink");

    const char *label;
    uint16_t    color;
    switch (mode) {
        case GAME_MODE_LOCAL:   label = "LOCAL";   color = C_BLUE;      break;
        case GAME_MODE_LICHESS: label = "LICHESS"; color = C_GREEN;     break;
        default:                label = "IDLE";    color = C_DARK_GRAY; break;
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

static void render_game_state(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);
    draw_turn_indicator(gs->active_color);
    draw_last_move(gs->last_move);
    draw_status(gs->status_msg);
    draw_mini_board(gs->fen);
    draw_eval_bar(gs->eval_cp);
}

// --- promotion picker screen -------------------------------------------------
//
// 2x2 tile grid, cycle with BTN_CYCLE, confirm with BTN_CONFIRM
//
//  y=0    header (reused)
//  y=30   purple title bar
//  y=62   row 0 tiles: queen (x=5), rook (x=93)
//  y=150  row 1 tiles: bishop (x=5), knight (x=93)
//  y=240  button legend

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
        tft.drawRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, border);

    tft.setTextColor(fg);
    tft.setTextSize(4);
    tft.setCursor(x + TILE_W / 2 - 12, y + 14);
    tft.print(PROMO_LABELS[idx][0]);

    tft.setTextSize(1);
    tft.setCursor(x + 4, y + TILE_H - 14);
    tft.print(PROMO_LABELS[idx]);
}

static void render_promo_picker(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);

    tft.fillRect(0, 30, D_W, 28, C_PURPLE);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(1);
    tft.setCursor(28, 40);
    tft.print("PROMOTE PAWN -- choose piece");

    for (int i = 0; i < 4; i++)
        draw_promo_tile(i, i == gs->promo_cursor);

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(4, 240);
    tft.print("[CYCLE] next piece");
    tft.setCursor(4, 256);
    tft.print("[CONFIRM] lock in choice");
}

// only redraws the tiles when cursor moves -- avoids full-screen flicker
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

    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(14, 140);
    tft.print("ChessLink");
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(44, 162);
    tft.print("initializing...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    GameState_t  gs                = {};
    PromoState_t last_promo_state  = PROMO_NONE;
    uint8_t      last_promo_cursor = 0xFF;

    for (;;) {
        if (xQueueReceive(xQ_GameState, &gs, pdMS_TO_TICKS(250)) != pdTRUE)
            continue;

        if (gs.promo_state == PROMO_SELECTING) {
            if (last_promo_state != PROMO_SELECTING)
                render_promo_picker(&gs);
            else if (gs.promo_cursor != last_promo_cursor)
                redraw_promo_tiles(&gs);
        } else {
            render_game_state(&gs);
        }

        last_promo_state  = gs.promo_state;
        last_promo_cursor = gs.promo_cursor;
    }

    vTaskDelete(NULL);
}
