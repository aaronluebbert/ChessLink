#include "chesslink.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// display setup

// HSPI (separate from VSPI used by shift registers)
static SPIClass hspi(HSPI);
static Adafruit_ST7789 tft = Adafruit_ST7789(&hspi, LCD_CS, LCD_DC, LCD_RST);

// physical display: 170x320 (portrait)
#define D_W    170
#define D_H    320

// color palette 

// RGB565
#define C_BLACK       0x0000
#define C_WHITE       0xFFFF
#define C_DARK_GRAY   0x39C7
#define C_LIGHT_GRAY  0xC618
#define C_GREEN       0x07E0
#define C_RED         0xF800
#define C_YELLOW      0xFFE0
#define C_BLUE        0x001F

// theme
#define C_BG          C_BLACK
#define C_HEADER_BG   0x0390   // dark green
#define C_TEXT        C_WHITE
#define C_ACCENT      C_YELLOW
#define C_DIM         C_DARK_GRAY

// board square colors (RGB565 approximation of tan/brown)
#define C_SQ_LIGHT    0xF7BE
#define C_SQ_DARK     0x9A40

// - layout constants (170x320 portrait) -
//
//  y=0    -
//         -  header bar (28px)           -
//  y=28   -
//         -  turn indicator (20px)       -
//  y=48   -
//         -  last move (18px)            -
//  y=66   -
//         -  status msg (18px)           -
//  y=84   -
//         -                              -
//         -  mini board (160x160, sq=20) -  centered: x=(170-160)/2=5
//         -                              -
//  y=244  -
//         -  padding (16px)              -
//  y=260  -
//         -  eval bar (20px)             -
//  y=280  -
//         -  padding                     -
//  y=320  -

#define HEADER_Y       0
#define HEADER_H       28
#define TURN_Y         28
#define TURN_H         20
#define MOVE_Y         48
#define MOVE_H         18
#define STATUS_Y       66
#define STATUS_H       18
#define BOARD_SQ       20              // pixels per square (8x20 = 160)
#define BOARD_PX       (BOARD_SQ * 8) // 160
#define BOARD_X        ((D_W - BOARD_PX) / 2)  // 5 -- centers on 170px width
#define BOARD_Y        84
#define EVAL_Y         260
#define EVAL_H         20

// draw functions

static void draw_header(void) {
    tft.fillRect(0, HEADER_Y, D_W, HEADER_H, C_HEADER_BG);
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print("ChessLink");
}

static void draw_mode_badge(GameMode_t mode) {
    const char *label;
    uint16_t    color;
    switch (mode) {
        case GAME_MODE_LOCAL:    label = "LOCAL";    color = C_BLUE;       break;
        case GAME_MODE_LICHESS:  label = "LICHESS";  color = C_GREEN;      break;
        case GAME_MODE_ANALYSIS: label = "ANALYSIS"; color = C_YELLOW;     break;
        default:                 label = "IDLE";     color = C_DARK_GRAY;  break;
    }
    // right-align badge in header: 52px wide, 14px tall
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

static void draw_status(const char *status_msg) {
    tft.fillRect(0, STATUS_Y, D_W, STATUS_H, C_BG);
    tft.setTextColor(C_TEXT);
    tft.setTextSize(1);
    tft.setCursor(4, STATUS_Y + 4);
    tft.print(status_msg);
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

    // clamp to +/-500 cp, map to bar width
    int clamped = eval_cp;
    if (clamped >  500) clamped =  500;
    if (clamped < -500) clamped = -500;
    int bar_px = (clamped + 500) * D_W / 1000;

    tft.fillRect(0,      EVAL_Y, bar_px,        EVAL_H, C_WHITE);
    tft.fillRect(bar_px, EVAL_Y, D_W - bar_px,  EVAL_H, C_BLACK);
    tft.drawRect(0,      EVAL_Y, D_W,            EVAL_H, C_LIGHT_GRAY);

    // eval text centered on bar
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

            if (occupied & (1ULL << sq)) {
                // filled circle for piece, radius leaves a 2px border
                tft.fillCircle(x + BOARD_SQ / 2, y + BOARD_SQ / 2,
                               BOARD_SQ / 2 - 2, C_WHITE);
            }
        }
    }
    tft.drawRect(BOARD_X - 1, BOARD_Y - 1, BOARD_PX + 2, BOARD_PX + 2, C_LIGHT_GRAY);
}

// full redraw

static void render_game_state(const GameState_t *gs) {
    tft.fillScreen(C_BG);
    draw_header();
    draw_mode_badge(gs->mode);
    draw_turn_indicator(gs->active_color);
    draw_last_move(gs->last_move);
    draw_status(gs->status_msg);
    draw_mini_board(gs->occupied);
    draw_eval_bar(gs->eval_cp);
}

// task

void task_LcdDisplay(void *pvParameters) {
    hspi.begin(LCD_SCLK, /*MISO=*/-1, LCD_MOSI, LCD_CS);

    // ST7789 init: 170x320, portrait
    tft.init(170, 320, SPI_MODE2);
    tft.setRotation(0);
    tft.fillScreen(C_BLACK);

    if (LCD_BL >= 0) {
        pinMode(LCD_BL, OUTPUT);
        digitalWrite(LCD_BL, HIGH);
    }

    // splash screen
    tft.setTextColor(C_WHITE);
    tft.setTextSize(2);
    tft.setCursor(14, 140);
    tft.print("ChessLink");
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(44, 162);
    tft.print("initializing...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    GameState_t gs = {};

    for (;;) {
        if (xQueueReceive(xQ_GameState, &gs, pdMS_TO_TICKS(250)) == pdTRUE)
            render_game_state(&gs);
    }

    vTaskDelete(NULL);
}
