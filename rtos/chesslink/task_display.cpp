#include "chesslink.h"
#include <Arduino_GFX_Library.h>

// --- display setup -----------------------------------------------------------
//
// ideaspark 1.9" 170x320 ST7789 (IPS) over VSPI. Arduino_GFX takes the panel
// geometry explicitly -- the visible 170-wide area is offset by 35 columns in
// the controller. `tft` is a reference to a heap object so the render code below
// can keep using `tft.` calls unchanged (Adafruit-GFX-compatible API).
static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCLK, LCD_MOSI, GFX_NOT_DEFINED /*MISO*/);
static Arduino_GFX &tft =
    *(new Arduino_ST7789(bus, LCD_RST, 2 /*rotation: 180, USB-C at top*/, true /*IPS*/, 170, 320, 35, 0, 35, 0));

#define LCD_LOCK()   xSemaphoreTake(xSPI18Mutex, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(xSPI18Mutex)

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
        case GAME_MODE_REPLAY:  label = "STUDY";   color = C_ACCENT;    break;
        default:                label = "IDLE";    color = C_DARK_GRAY; break;
    }
    tft.fillRect(D_W - 54, 7, 52, 14, color);
    tft.setTextColor(C_BLACK);
    tft.setTextSize(1);
    tft.setCursor(D_W - 52, 10);
    tft.print(label);
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
    draw_last_move(gs->last_move);
    draw_status(gs->status_msg);

    // No on-screen board -- the physical board is the board. Just show a big,
    // clear "whose move" prompt where the mini board used to be.
    const char *side = (gs->active_color == 0) ? "WHITE" : "BLACK";
    uint16_t    col  = (gs->active_color == 0) ? C_WHITE : C_ACCENT;
    int w = (int)strlen(side) * 6 * 4;
    tft.setTextSize(4);
    tft.setTextColor(col);
    tft.setCursor((D_W - w) / 2, 150);
    tft.print(side);

    const char *sub = "to move";
    int w2 = (int)strlen(sub) * 6 * 2;
    tft.setTextSize(2);
    tft.setTextColor(C_LIGHT_GRAY);
    tft.setCursor((D_W - w2) / 2, 196);
    tft.print(sub);
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

static const char *MENU_ITEMS[MENU_ITEM_COUNT] =
    { "Local Game", "Online Game", "Play Bot", "Famous Games", "WiFi Setup" };

static void render_menu(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header(gs->mode);

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 8);
    tft.print("select a mode");

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int  y   = 64 + i * 44;
        bool sel = (i == gs->menu_cursor);

        tft.fillRect(8, y, D_W - 16, 36, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 36, sel ? C_ACCENT : C_LIGHT_GRAY);
        if (sel) tft.drawRect(9, y + 1, D_W - 18, 34, C_ACCENT);

        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);
        tft.setTextSize(2);
        tft.setCursor(20, y + 10);
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

static void render_local_cfg(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("Local Game");

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 4);
    tft.print("over-the-board setup");

    for (int i = 0; i < LOCAL_ROW_COUNT; i++) {
        int  y   = 60 + i * 42;
        bool sel = (i == gs->cfg_cursor);

        tft.fillRect(8, y, D_W - 16, 34, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 34, sel ? C_ACCENT : C_LIGHT_GRAY);

        tft.setTextSize(2);
        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);

        if (i == LOCAL_ROW_START) {
            tft.setCursor(20, y + 9);
            tft.print("Start game");
        } else {   // LOCAL_ROW_TIME
            const char *val = LOCAL_TIME_PRESETS[gs->cfg_time_idx].label;
            tft.setCursor(16, y + 9);
            tft.print("Clock");
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

static void render_famous(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    tft.fillRect(0, 0, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("Famous Games");

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, HEADER_H + 4);
    tft.print("step through move by move");

    for (int i = 0; i < FAMOUS_GAME_COUNT; i++) {
        int  y   = 64 + i * 44;
        bool sel = (i == gs->cfg_cursor);
        tft.fillRect(8, y, D_W - 16, 36, sel ? C_HEADER_BG : C_DARK_GRAY);
        tft.drawRect(8, y, D_W - 16, 36, sel ? C_ACCENT : C_LIGHT_GRAY);
        tft.setTextColor(sel ? C_ACCENT : C_LIGHT_GRAY);
        tft.setTextSize(2);
        tft.setCursor(20, y + 10);
        tft.print(FAMOUS_GAMES[i].name);
    }

    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, 292);
    tft.print("UP/DN move  OK start");
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

// --- frame rendering ---------------------------------------------------------

enum DispScreen {
    SCR_NONE, SCR_MENU, SCR_ONLINE_CFG, SCR_BOT_CFG, SCR_LOCAL_CFG, SCR_FAMOUS, SCR_SETUP,
    SCR_NOTICE, SCR_CONFIRM, SCR_GAMEOVER, SCR_BOARD, SCR_MATCH, SCR_PROMO
};

// persistent display state across frames (which screen is up, cursors, and the
// local clock baseline so a live match counts down between server updates)
struct DispState {
    DispScreen shown;
    uint8_t    last_menu_cursor;
    uint8_t    last_promo_cursor;
    uint32_t   base_my_ms;
    uint32_t   base_opp_ms;
    TickType_t base_tick;
    bool       my_turn;
};

// draw the right screen for this frame. the caller holds xSPI18Mutex. got==false
// means no new GameState arrived -- only the live match clock ticks
static void render_frame(const GameState_t *gs, DispState *st, bool got) {
    if (!got) {
        if (st->shown == SCR_MATCH) {
            uint32_t elapsed = (xTaskGetTickCount() - st->base_tick) * portTICK_PERIOD_MS;
            uint32_t my_ms   = st->base_my_ms;
            uint32_t opp_ms  = st->base_opp_ms;
            if (st->my_turn) my_ms  = elapsed < st->base_my_ms  ? st->base_my_ms  - elapsed : 0;
            else             opp_ms = elapsed < st->base_opp_ms ? st->base_opp_ms - elapsed : 0;
            draw_match_clocks(my_ms, opp_ms, st->my_turn);
        }
        return;
    }

    if (gs->ui_screen == UI_SETUP) {
        if (st->shown != SCR_SETUP) render_setup();
        st->shown = SCR_SETUP; return;
    }
    if (gs->ui_screen == UI_NOTICE) {
        if (st->shown != SCR_NOTICE) render_notice(gs, false);
        st->shown = SCR_NOTICE; return;
    }
    if (gs->ui_screen == UI_CONFIRM) {
        if (st->shown != SCR_CONFIRM) render_confirm();
        st->shown = SCR_CONFIRM; return;
    }
    if (gs->ui_screen == UI_GAMEOVER) {
        if (st->shown != SCR_GAMEOVER) render_notice(gs, true);
        st->shown = SCR_GAMEOVER; return;
    }
    if (gs->ui_screen == UI_MENU) {
        if (st->shown != SCR_MENU || gs->menu_cursor != st->last_menu_cursor) render_menu(gs);
        st->shown = SCR_MENU; st->last_menu_cursor = gs->menu_cursor; return;
    }
    if (gs->ui_screen == UI_ONLINE_CFG) { render_online_cfg(gs); st->shown = SCR_ONLINE_CFG; return; }
    if (gs->ui_screen == UI_BOT_CFG)    { render_bot_cfg(gs);    st->shown = SCR_BOT_CFG;    return; }
    if (gs->ui_screen == UI_LOCAL_CFG)  { render_local_cfg(gs);  st->shown = SCR_LOCAL_CFG;  return; }
    if (gs->ui_screen == UI_FAMOUS)     { render_famous(gs);     st->shown = SCR_FAMOUS;     return; }

    if (gs->promo_state == PROMO_SELECTING) {
        if (st->shown != SCR_PROMO)                      render_promo_picker(gs);
        else if (gs->promo_cursor != st->last_promo_cursor) redraw_promo_tiles(gs);
        st->shown = SCR_PROMO; st->last_promo_cursor = gs->promo_cursor; return;
    }

    // a live timed match if either clock is running, otherwise the board
    if (gs->white_clock_ms || gs->black_clock_ms) {
        st->base_my_ms  = gs->my_color == 0 ? gs->white_clock_ms : gs->black_clock_ms;
        st->base_opp_ms = gs->my_color == 0 ? gs->black_clock_ms : gs->white_clock_ms;
        st->base_tick   = xTaskGetTickCount();
        st->my_turn     = (gs->active_color == gs->my_color);
        render_match_static(gs);
        draw_match_clocks(st->base_my_ms, st->base_opp_ms, st->my_turn);
        st->shown = SCR_MATCH;
    } else {
        render_game_state(gs);
        st->shown = SCR_BOARD;
    }
}

// --- task --------------------------------------------------------------------

void task_LcdDisplay(void *pvParameters) {
    // backlight on
    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    // bare-minimum ST7789 bring-up for the 1.9" 170x320 panel
    LCD_LOCK();
    tft.begin();
    tft.setRotation(2);   // 180 -- panel is mounted with USB-C at the top

    // power-on test: a solid red screen. if you see RED, the panel is being
    // driven; if it stays dark, the init/library still isn't right
    tft.fillScreen(C_RED);
    LCD_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(1500));

    LCD_LOCK();
    tft.fillScreen(C_BLACK);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 150);
    tft.print("ChessLink");
    LCD_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(1000));

    GameState_t gs = {};
    DispState   st = { SCR_NONE, 0xFF, 0xFF, 0, 0, 0, false };

    for (;;) {
        bool got = (xQueueReceive(xQ_GameState, &gs, pdMS_TO_TICKS(250)) == pdTRUE);
        LCD_LOCK();
        render_frame(&gs, &st, got);
        LCD_UNLOCK();
    }

    vTaskDelete(NULL);
}
