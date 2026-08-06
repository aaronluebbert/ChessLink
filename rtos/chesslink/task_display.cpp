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
//  y=244  padding to 320

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

static void render_game_state(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);
    draw_turn_indicator(gs->active_color);
    draw_last_move(gs->last_move);
    draw_status(gs->status_msg);
    draw_mini_board(gs->fen);
}

// --- live match screen -------------------------------------------------------
//
// portrait, USB-C at the top. opponent up top, me at the bottom, the two
// clocks stacked in the middle (opponent's above mine). the side to move gets
// a green clock, the idle side stays white. names and elo share one text size
//
//  y=16    opponent name   (size 2)
//  y=40    opponent elo     (size 2)
//  y=88    opponent clock   (size 5)
//  y=150   divider
//  y=176   my clock         (size 5)
//  y=254   my name          (size 2)
//  y=278   my elo           (size 2)

#define M_OPP_NAME_Y  16
#define M_OPP_ELO_Y   40
#define M_OPP_CLK_Y   88
#define M_MY_CLK_Y    176
#define M_MY_NAME_Y   254
#define M_MY_ELO_Y    278
#define M_CLK_SIZE    5
#define M_NAME_SIZE   2

// each GFX char cell is 6px wide per size step, so center on that
static void print_centered(const char *s, int y, uint8_t size, uint16_t color) {
    int w = (int)strlen(s) * 6 * size;
    int x = (D_W - w) / 2;
    if (x < 0) x = 0;
    tft.setTextSize(size);
    tft.setTextColor(color);
    tft.setCursor(x, y);
    tft.print(s);
}

// ms remaining -> MM:SS, minutes clamped to 99, 0 shows as a blank clock
static void fmt_clock(uint32_t ms, char *buf, size_t n) {
    if (ms == 0) { strncpy(buf, "--:--", n); return; }
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    s %= 60;
    if (m > 99) m = 99;
    snprintf(buf, n, "%02u:%02u", (unsigned)m, (unsigned)s);
}

// rating -> string, 0 (unknown) shows as dashes
static void fmt_rating(uint16_t r, char *buf, size_t n) {
    if (r) snprintf(buf, n, "%u", (unsigned)r);
    else   strncpy(buf, "----", n);
}

// the parts that only change on a real state update: names, elo, divider
static void render_match_static(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    char elo[12];

    print_centered(gs->opp_name[0] ? gs->opp_name : "Opponent",
                   M_OPP_NAME_Y, M_NAME_SIZE, C_WHITE);
    fmt_rating(gs->opp_rating, elo, sizeof(elo));
    print_centered(elo, M_OPP_ELO_Y, M_NAME_SIZE, C_ACCENT);

    tft.drawFastHLine(12, 150, D_W - 24, C_DARK_GRAY);

    print_centered(gs->my_name[0] ? gs->my_name : "You",
                   M_MY_NAME_Y, M_NAME_SIZE, C_WHITE);
    fmt_rating(gs->my_rating, elo, sizeof(elo));
    print_centered(elo, M_MY_ELO_Y, M_NAME_SIZE, C_ACCENT);
}

// just the two clocks, redrawn on every tick so the running side counts down
static void draw_match_clocks(uint32_t my_ms, uint32_t opp_ms, bool my_turn) {
    char clk[8];
    int band = 8 * M_CLK_SIZE;   // size-5 glyphs are 40px tall

    tft.fillRect(0, M_OPP_CLK_Y, D_W, band, C_BG);
    fmt_clock(opp_ms, clk, sizeof(clk));
    print_centered(clk, M_OPP_CLK_Y, M_CLK_SIZE, my_turn ? C_WHITE : C_GREEN);

    tft.fillRect(0, M_MY_CLK_Y, D_W, band, C_BG);
    fmt_clock(my_ms, clk, sizeof(clk));
    print_centered(clk, M_MY_CLK_Y, M_CLK_SIZE, my_turn ? C_GREEN : C_WHITE);
}

// --- mode-select menu --------------------------------------------------------
//
// shown at boot and whenever no game is running. UP/DOWN move the highlight,
// CONFIRM enters the mode. labels line up with the MENU_ITEM_* indices

static const char *MENU_ITEMS[MENU_ITEM_COUNT] = { "Local Game", "Online Game", "Play Bot", "WiFi Setup" };

static void render_menu(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 8);
    tft.print("select a mode");

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int  y   = 74 + i * 46;
        bool sel = (i == gs->menu_cursor);

        tft.fillRect(8, y, D_W - 16, 38, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 38, sel ? C_ACCENT : C_LIGHT_GRAY);
        if (sel) tft.drawRect(9, y + 1, D_W - 18, 36, C_ACCENT);

        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);
        tft.setTextSize(2);
        tft.setCursor(20, y + 11);
        tft.print(MENU_ITEMS[i]);
    }

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, 294);
    tft.print("UP/DN move");
    tft.setCursor(6, 306);
    tft.print("OK select");
}

// --- online rated-match config -----------------------------------------------
//
// rows: time control (cycles on OK), rated toggle, and a start row. UP/DN move
// the highlight, OK changes/starts, CANCEL backs out

static void render_online_cfg(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("Online");

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 8);
    tft.print("rated match setup");

    for (int i = 0; i < CFG_ROW_COUNT; i++) {
        int  y   = 70 + i * 42;
        bool sel = (i == gs->cfg_cursor);

        tft.fillRect(8, y, D_W - 16, 34, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 34, sel ? C_ACCENT : C_LIGHT_GRAY);

        tft.setTextSize(2);
        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);

        if (i == CFG_ROW_START) {
            tft.setCursor(20, y + 9);
            tft.print("Start game");
        } else {
            const char *label = (i == CFG_ROW_TIME) ? "Time" : "Rated";
            const char *val   = (i == CFG_ROW_TIME)
                              ? ONLINE_TIME_PRESETS[gs->cfg_time_idx].label
                              : (gs->cfg_rated ? "Yes" : "No");
            tft.setCursor(16, y + 9);
            tft.print(label);
            int vw = (int)strlen(val) * 12;   // right-align the value
            tft.setTextColor(C_WHITE);
            tft.setCursor(D_W - 16 - vw, y + 9);
            tft.print(val);
        }
    }

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, 292);
    tft.print("UP/DN move  OK change");
    tft.setCursor(6, 304);
    tft.print("CANCEL = back");
}

// --- play-computer (Stockfish) config ----------------------------------------
//
// rows: level (1-8), time control, color, and a start row. same controls as the
// online config

static void render_bot_cfg(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("Play Bot");

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 4);
    tft.print("stockfish setup");

    char lvl[6];
    snprintf(lvl, sizeof(lvl), "%u", gs->cfg_level);
    static const char *COLORS[3] = { "White", "Black", "Random" };

    for (int i = 0; i < BOT_ROW_COUNT; i++) {
        int  y   = 60 + i * 42;
        bool sel = (i == gs->cfg_cursor);

        tft.fillRect(8, y, D_W - 16, 34, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 34, sel ? C_ACCENT : C_LIGHT_GRAY);

        tft.setTextSize(2);
        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);

        if (i == BOT_ROW_START) {
            tft.setCursor(20, y + 9);
            tft.print("Start game");
        } else {
            const char *label = (i == BOT_ROW_LEVEL) ? "Level"
                              : (i == BOT_ROW_TIME)  ? "Time" : "Color";
            const char *val   = (i == BOT_ROW_LEVEL) ? lvl
                              : (i == BOT_ROW_TIME)  ? BOT_TIME_PRESETS[gs->cfg_time_idx].label
                              : COLORS[gs->cfg_color % 3];
            tft.setCursor(16, y + 9);
            tft.print(label);
            int vw = (int)strlen(val) * 12;
            tft.setTextColor(C_WHITE);
            tft.setCursor(D_W - 16 - vw, y + 9);
            tft.print(val);
        }
    }

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, 292);
    tft.print("UP/DN move  OK change");
    tft.setCursor(6, 304);
    tft.print("CANCEL = back");
}

// --- WiFi / token setup screen -----------------------------------------------
//
// static instructions shown while the captive portal is up. all the details are
// constants (AP name/password, portal IP), so nothing needs to be passed in

static void render_setup(void) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("WiFi Setup");

    tft.setTextSize(1);
    int y = HEADER_H + 12;

    tft.setTextColor(C_ACCENT); tft.setCursor(6, y);  tft.print("1. Join this WiFi:");    y += 16;
    tft.setTextColor(C_WHITE);  tft.setCursor(14, y); tft.print(WIFI_SETUP_AP_SSID);      y += 14;
    tft.setTextColor(C_DIM);    tft.setCursor(14, y); tft.print("pw: ");
                                                      tft.print(WIFI_SETUP_AP_PASS);      y += 26;

    tft.setTextColor(C_ACCENT); tft.setCursor(6, y);  tft.print("2. Open in browser:");   y += 16;
    tft.setTextColor(C_WHITE);  tft.setCursor(14, y); tft.print("192.168.4.1");           y += 26;

    tft.setTextColor(C_ACCENT); tft.setCursor(6, y);  tft.print("3. Enter WiFi + token"); y += 16;
    tft.setTextColor(C_DIM);    tft.setCursor(14, y); tft.print("then Save & connect");

    tft.setTextColor(C_DIM);    tft.setCursor(6, 300); tft.print("waiting for phone...");
}

// --- notice / result screen --------------------------------------------------
//
// one screen for centered messages: transient notices ("Seeking opponent...")
// and terminal ones (game results, errors). the message lives in status_msg and
// is auto-fit (big if it fits the width, else small). terminal notices invite a
// button press to return to the menu

static void render_notice(const GameState_t *gs, bool terminal) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("ChessLink");

    uint8_t sz = ((int)strlen(gs->status_msg) * 6 * 2 <= D_W) ? 2 : 1;
    print_centered(gs->status_msg, 150, sz, C_ACCENT);

    if (terminal) {
        print_centered("press a button", 286, 1, C_DIM);
        print_centered("for the menu",   300, 1, C_DIM);
    } else {
        print_centered("please wait...", 300, 1, C_DIM);
    }
}

// --- leave-game confirm ------------------------------------------------------

static void render_confirm(void) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("ChessLink");

    print_centered("Leave game?", 140, 2, C_ACCENT);
    print_centered("OK = yes", 286, 1, C_GREEN);
    print_centered("CANCEL = no", 300, 1, C_DIM);
}

// --- promotion picker screen -------------------------------------------------
//
// 2x2 tile grid, UP/DOWN choose the piece, CONFIRM locks it in
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
    tft.print("UP/DN choose piece");
    tft.setCursor(4, 256);
    tft.print("OK confirm");
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
    uint8_t      last_promo_cursor = 0xFF;

    // which screen is currently up, so we only full-redraw on a real change
    enum { SCR_NONE, SCR_MENU, SCR_ONLINE_CFG, SCR_BOT_CFG, SCR_SETUP, SCR_NOTICE, SCR_CONFIRM, SCR_GAMEOVER, SCR_BOARD, SCR_MATCH, SCR_PROMO } shown = SCR_NONE;
    uint8_t last_menu_cursor = 0xFF;

    // local clock tracking -- lichess only sends fresh times on a move, so we
    // count the running side down locally between updates for a live feel
    uint32_t   base_my_ms  = 0;
    uint32_t   base_opp_ms = 0;
    TickType_t base_tick   = 0;
    bool       my_turn     = false;

    for (;;) {
        bool got = (xQueueReceive(xQ_GameState, &gs, pdMS_TO_TICKS(250)) == pdTRUE);

        if (got) {
            if (gs.ui_screen == UI_SETUP) {
                if (shown != SCR_SETUP) render_setup();
                shown = SCR_SETUP;
                continue;
            }

            if (gs.ui_screen == UI_NOTICE) {
                if (shown != SCR_NOTICE) render_notice(&gs, false);
                shown = SCR_NOTICE;
                continue;
            }

            if (gs.ui_screen == UI_CONFIRM) {
                if (shown != SCR_CONFIRM) render_confirm();
                shown = SCR_CONFIRM;
                continue;
            }

            if (gs.ui_screen == UI_GAMEOVER) {
                if (shown != SCR_GAMEOVER) render_notice(&gs, true);
                shown = SCR_GAMEOVER;
                continue;
            }

            if (gs.ui_screen == UI_MENU) {
                if (shown != SCR_MENU || gs.menu_cursor != last_menu_cursor)
                    render_menu(&gs);
                shown = SCR_MENU;
                last_menu_cursor = gs.menu_cursor;
                continue;
            }

            if (gs.ui_screen == UI_ONLINE_CFG) {
                render_online_cfg(&gs);   // published only on changes -> redraw
                shown = SCR_ONLINE_CFG;
                continue;
            }

            if (gs.ui_screen == UI_BOT_CFG) {
                render_bot_cfg(&gs);
                shown = SCR_BOT_CFG;
                continue;
            }

            if (gs.promo_state == PROMO_SELECTING) {
                if (shown != SCR_PROMO)                 render_promo_picker(&gs);
                else if (gs.promo_cursor != last_promo_cursor) redraw_promo_tiles(&gs);
                shown = SCR_PROMO;
                last_promo_cursor = gs.promo_cursor;
                continue;
            }

            // a live timed match if either clock is running
            if (gs.white_clock_ms || gs.black_clock_ms) {
                base_my_ms  = gs.my_color == 0 ? gs.white_clock_ms : gs.black_clock_ms;
                base_opp_ms = gs.my_color == 0 ? gs.black_clock_ms : gs.white_clock_ms;
                base_tick   = xTaskGetTickCount();
                my_turn     = (gs.active_color == gs.my_color);

                render_match_static(&gs);
                draw_match_clocks(base_my_ms, base_opp_ms, my_turn);
                shown = SCR_MATCH;
            } else {
                render_game_state(&gs);
                shown = SCR_BOARD;
            }
            continue;
        }

        // no new state -- tick the running clock down while a match is on screen
        if (shown == SCR_MATCH) {
            uint32_t elapsed = (xTaskGetTickCount() - base_tick) * portTICK_PERIOD_MS;
            uint32_t my_ms   = base_my_ms;
            uint32_t opp_ms  = base_opp_ms;
            if (my_turn) my_ms  = elapsed < base_my_ms  ? base_my_ms  - elapsed : 0;
            else         opp_ms = elapsed < base_opp_ms ? base_opp_ms - elapsed : 0;
            draw_match_clocks(my_ms, opp_ms, my_turn);
        }
    }

    vTaskDelete(NULL);
}
