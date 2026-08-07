#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// --- pin definitions ---------------------------------------------------------

// SN74HC165 shift registers -- read over the VSPI hardware bus (SR_SCLK/SR_MISO
// are VSPI SCK/MISO, shared with the LCD; see below). SR_LOAD is a plain GPIO
// latch. the ideaspark board wires these to the ESP32 default VSPI pins
#define SR_SCLK       18   // VSPI SCK, shared with the LCD
#define SR_MISO       19   // VSPI MISO -- QH from the SR chain
#define SR_LOAD       5    // parallel load (PL), active-low, bit-banged latch

// WS2812B via 74AHCT125 level shifter
#define LED_DATA_PIN  25   // RMT output -> level shifter -> LED_DATA_5V

// ST7789 LCD (170x320) on the SAME VSPI bus as the shift registers. the LCD
// writes on MOSI, the SR reads on MISO, both clocked by SCLK 18. the sensor and
// display tasks hold xSPI18Mutex around their transactions so a sensor read
// isn't clocked apart by an LCD write (and the SR shifting during LCD writes is
// harmless -- we re-latch before every read, and the LCD's CS is high while we read)
#define LCD_MOSI      23   // VSPI MOSI, shared with the SR bus
#define LCD_SCLK      18   // = SR_SCLK
#define LCD_CS        15
#define LCD_DC        2
#define LCD_RST       4
#define LCD_BL        32

// tactile buttons (input-only pins, no internal pullup -- needs external 10k to 3.3V)
#define BTN_UP        36   // menu up / previous
#define BTN_DOWN      39   // menu down / next
#define BTN_CONFIRM   34   // confirm / select
#define BTN_CANCEL    35   // cancel / back

// --- board geometry ----------------------------------------------------------

#define NUM_SQUARES   64
// sq = rank*8 + file  (a1=0, h8=63)

// --- RTOS task config --------------------------------------------------------

#define STACK_SENSOR    4096
#define STACK_LED       4096
#define STACK_GAME      8192    // engine + move-gen buffers + big GameCtx local
#define STACK_DISPLAY   8192    // Arduino_GFX needs more than 4k (test task has 8k)
#define STACK_NETWORK   16384   // WiFi + captive portal (WebServer/DNS) + TLS
#define STACK_BUTTONS   2048

// higher = more urgent
#define PRI_SENSOR      5
#define PRI_LED         4
#define PRI_GAME        3
#define PRI_BUTTONS     3
#define PRI_NETWORK     2
#define PRI_DISPLAY     1

// core 1: sensor, LED, game logic, buttons -- pure compute, no WiFi interference
// core 0: network, display -- must share with ESP32 WiFi/TCP system tasks
#define CORE_SENSOR     1
#define CORE_LED        1
#define CORE_GAME       1
#define CORE_BUTTONS    1
#define CORE_DISPLAY    0
#define CORE_NETWORK    0

// queue depths
#define Q_BOARD_STATE_DEPTH   4
#define Q_LED_CMD_DEPTH       8
#define Q_GAME_STATE_DEPTH    1    // must be 1: publish_game_state uses xQueueOverwrite
#define Q_MOVE_DEPTH          4
#define Q_BUTTON_DEPTH        8
#define Q_NET_CMD_DEPTH       4
#define Q_NET_UPDATE_DEPTH    4

// timing
#define SENSOR_SCAN_MS    20
#define LED_UPDATE_MS     16
#define BTN_POLL_MS       20
#define BTN_DEBOUNCE_MS   50
#define DEBOUNCE_SCANS    3

// --- data types --------------------------------------------------------------

typedef struct {
    uint64_t occupied;
} BoardState_t;

typedef enum {
    LED_CMD_SET_SQUARE,
    LED_CMD_CLEAR,
    LED_CMD_PATTERN,
    LED_CMD_HILITE,     // light the masked squares, all others off
} LedCmdType_t;

typedef struct {
    LedCmdType_t type;
    uint8_t  square;
    uint8_t  r, g, b;
    uint64_t mask;
} LedCmd_t;

typedef enum {
    BTN_EVT_UP,
    BTN_EVT_DOWN,
    BTN_EVT_CONFIRM,
    BTN_EVT_CANCEL,
} ButtonEvent_t;

typedef enum {
    PROMO_NONE,
    PROMO_SELECTING,
} PromoState_t;

typedef enum {
    GAME_MODE_IDLE,       // sitting in the menu, no game running
    GAME_MODE_LOCAL,
    GAME_MODE_LICHESS,
    GAME_MODE_REPLAY,     // stepping through a famous game move by move
} GameMode_t;

// top-level screen the display shows
typedef enum {
    UI_MENU,       // mode-select menu
    UI_ONLINE_CFG, // online rated-match config submenu
    UI_BOT_CFG,    // play-computer (Stockfish) config submenu
    UI_LOCAL_CFG,  // local over-the-board config (clock) submenu
    UI_FAMOUS,     // famous-games list submenu
    UI_GAME,       // in a game -- board / match / promo screens
    UI_SETUP,      // WiFi/token captive-portal instructions
    UI_NOTICE,     // transient status ("seeking...") -- no button prompt
    UI_CONFIRM,    // "leave game?" yes/no prompt
    UI_GAMEOVER,   // terminal result / error -- any button returns to the menu
} UiScreen_t;

// mode-select menu items, in cursor order
#define MENU_ITEM_COUNT  5
#define MENU_ITEM_LOCAL  0
#define MENU_ITEM_ONLINE 1
#define MENU_ITEM_BOT    2
#define MENU_ITEM_FAMOUS 3
#define MENU_ITEM_SETUP  4

// famous games (step-through study mode)
typedef struct { const char *name; const char *const *moves; int count; } FamousGame_t;
extern const FamousGame_t FAMOUS_GAMES[];
extern const int          FAMOUS_GAME_COUNT;

// time-control preset (min = 0 means untimed). two lists: the online rated one
// (all timed) and the bot one (adds untimed)
typedef struct { uint16_t min; uint8_t inc; const char *label; } TimePreset_t;
extern const TimePreset_t ONLINE_TIME_PRESETS[];
extern const int          ONLINE_TIME_COUNT;
extern const TimePreset_t BOT_TIME_PRESETS[];
extern const int          BOT_TIME_COUNT;
extern const TimePreset_t LOCAL_TIME_PRESETS[];
extern const int          LOCAL_TIME_COUNT;

// online rated-match config rows
#define CFG_ROW_TIME   0
#define CFG_ROW_RATED  1
#define CFG_ROW_START  2
#define CFG_ROW_COUNT  3

// play-computer config rows + level range (lichess Stockfish is 1..8)
#define BOT_ROW_LEVEL  0
#define BOT_ROW_TIME   1
#define BOT_ROW_COLOR  2
#define BOT_ROW_START  3
#define BOT_ROW_COUNT  4
#define BOT_LEVEL_MIN  1
#define BOT_LEVEL_MAX  8

// local over-the-board config rows (clock control + start)
#define LOCAL_ROW_TIME   0
#define LOCAL_ROW_START  1
#define LOCAL_ROW_COUNT  2

// soft-AP shown during first-boot / on-demand WiFi + token setup
#define WIFI_SETUP_AP_SSID "ChessLink-Setup"
#define WIFI_SETUP_AP_PASS "chesslink12"

// commands from the game task to the network task
typedef enum {
    NET_CMD_START_ONLINE,   // seek a human lichess game, then stream it
    NET_CMD_START_BOT,      // challenge Stockfish, then stream it
    NET_CMD_RESIGN,         // resign/abort the current game (read mid-stream)
    NET_CMD_OPEN_SETUP,     // (re)open the captive portal to set WiFi + token
} NetCmd_t;

// message on xQ_NetCmd -- the command plus game parameters
typedef struct {
    NetCmd_t type;
    uint16_t time_min;      // initial clock in minutes (0 = untimed, bot only)
    uint8_t  inc_sec;       // increment, seconds
    bool     rated;         // online human match
    uint8_t  level;         // bot Stockfish level 1..8
    uint8_t  color;         // bot color: 0=white 1=black 2=random
} NetCmdMsg_t;

// coarse network state the network task reports up for the display
typedef enum {
    NET_STATUS_SETUP,       // captive portal is up, waiting for the phone
    NET_STATUS_CONNECTING,  // creds submitted, joining WiFi
    NET_STATUS_ONLINE,      // connected to WiFi, ready
} NetStatus_t;

// status pushed from the network task to the game task during an online game:
// player identities (once, at game start) and/or fresh clocks (on every packet
// that carries them) so the match screen stays in sync with the server
typedef struct {
    bool        has_status;    // the status field below is valid
    NetStatus_t status;

    bool        has_result;    // game finished -- result_text is valid
    char        result_text[32];

    bool     has_meta;      // my_/opp_ name, rating and color are valid
    char     my_name[20];
    char     opp_name[20];
    uint16_t my_rating;
    uint16_t opp_rating;
    uint8_t  my_color;      // 0=white 1=black

    bool     has_clocks;    // the clock fields below are valid
    uint32_t white_clock_ms;
    uint32_t black_clock_ms;

    // authoritative full-position sync: the server's move list rebuilt into a
    // FEN. the game task adopts this as truth, so online play can never drift.
    bool     has_sync;
    char     sync_fen[92];
    bool     sync_have_last;    // last-move squares below are valid (LED guidance)
    uint8_t  sync_from;
    uint8_t  sync_to;
} NetUpdate_t;

typedef struct {
    GameMode_t  mode;
    char        fen[92];
    uint8_t     active_color;     // 0=white, 1=black
    char        last_move[6];
    char        status_msg[32];

    // top-level UI
    UiScreen_t  ui_screen;     // menu vs in-game
    uint8_t     menu_cursor;   // highlighted item when ui_screen == UI_MENU

    // config submenus (UI_ONLINE_CFG / UI_BOT_CFG)
    uint8_t     cfg_cursor;    // CFG_ROW_* / BOT_ROW_*
    uint8_t     cfg_time_idx;  // index into the active time-preset list
    bool        cfg_rated;     // online
    uint8_t     cfg_level;     // bot: 1..8
    uint8_t     cfg_color;     // bot: 0=white 1=black 2=random

    // clock data from lichess (ms remaining for each side, 0 if not in a timed game)
    uint32_t    white_clock_ms;
    uint32_t    black_clock_ms;

    // live-match players, filled for online games (blank/0 = unknown)
    // my_color says which physical clock (white/black) is mine, so the match
    // screen can map the white/black clocks onto the "me" and "opp" rows
    char        my_name[20];
    char        opp_name[20];
    uint16_t    my_rating;
    uint16_t    opp_rating;
    uint8_t     my_color;         // 0=white, 1=black

    // promotion picker
    PromoState_t promo_state;
    uint8_t      promo_cursor;    // 0=queen 1=rook 2=bishop 3=knight
} GameState_t;

typedef struct {
    uint8_t   from_sq;
    uint8_t   to_sq;
    char      uci[6];
    // clock data piggy-backed on opponent move events (0 if not a timed game)
    uint32_t  white_clock_ms;
    uint32_t  black_clock_ms;
} MoveEvent_t;

// --- queue handles -----------------------------------------------------------

extern QueueHandle_t xQ_BoardState;
extern QueueHandle_t xQ_LedCmd;
extern QueueHandle_t xQ_GameState;
extern QueueHandle_t xQ_PlayerMove;
extern QueueHandle_t xQ_OpponentMove;
extern QueueHandle_t xQ_ButtonEvent;
extern QueueHandle_t xQ_NetCmd;
extern QueueHandle_t xQ_NetUpdate;

// serializes access to the shared SR/LCD clock net (GPIO18): the sensor and the
// display each hold it while bit-banging so their edges never interleave
extern SemaphoreHandle_t xSPI18Mutex;

// --- task declarations -------------------------------------------------------

void task_SensorScan  (void *pvParameters);
void task_LedControl  (void *pvParameters);
void task_GameLogic   (void *pvParameters);
void task_LcdDisplay  (void *pvParameters);
void task_Network     (void *pvParameters);
void task_Buttons     (void *pvParameters);

// --- chess engine ------------------------------------------------------------

void chess_engine_init();
