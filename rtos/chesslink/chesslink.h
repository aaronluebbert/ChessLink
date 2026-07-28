#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// --- pin definitions ---------------------------------------------------------

// SN74HC165 shift registers (VSPI)
#define SR_SCLK       18
#define SR_MISO       19   // QH from rank 1 SR
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

// tactile buttons (input-only pins, no internal pullup -- needs external 10k to 3.3V)
#define BTN_CYCLE     34   // cycle through promotion choices
#define BTN_CONFIRM   35   // confirm selection

// --- board geometry ----------------------------------------------------------

#define NUM_SQUARES   64
// sq = rank*8 + file  (a1=0, h8=63)

// --- RTOS task config --------------------------------------------------------

#define STACK_SENSOR    4096
#define STACK_LED       4096
#define STACK_GAME      6144
#define STACK_DISPLAY   4096
#define STACK_NETWORK   8192
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
#define Q_GAME_STATE_DEPTH    4
#define Q_MOVE_DEPTH          4
#define Q_BUTTON_DEPTH        8

// timing
#define SENSOR_SCAN_MS    20
#define LED_UPDATE_MS     16
#define BTN_POLL_MS       20
#define BTN_DEBOUNCE_MS   50
#define DEBOUNCE_SCANS    3

// --- data types --------------------------------------------------------------

typedef struct {
    uint64_t occupied;
    uint32_t timestamp_ms;
} BoardState_t;

typedef enum {
    LED_CMD_SET_SQUARE,
    LED_CMD_SET_ALL,
    LED_CMD_CLEAR,
    LED_CMD_PATTERN,
} LedCmdType_t;

typedef struct {
    LedCmdType_t type;
    uint8_t  square;
    uint8_t  r, g, b;
    uint64_t mask;
} LedCmd_t;

typedef enum {
    BTN_EVT_CYCLE,
    BTN_EVT_CONFIRM,
} ButtonEvent_t;

typedef enum {
    PROMO_NONE,
    PROMO_SELECTING,
} PromoState_t;

typedef enum {
    GAME_MODE_IDLE,
    GAME_MODE_LOCAL,
    GAME_MODE_LICHESS,
} GameMode_t;

typedef struct {
    GameMode_t  mode;
    char        fen[92];
    uint8_t     active_color;     // 0=white, 1=black
    int16_t     eval_cp;          // centipawn eval, INT16_MIN if unknown
    char        last_move[6];
    char        status_msg[32];

    // clock data from lichess (ms remaining for each side, 0 if not in a timed game)
    uint32_t    white_clock_ms;
    uint32_t    black_clock_ms;
    uint32_t    white_inc_ms;
    uint32_t    black_inc_ms;

    // promotion picker
    PromoState_t promo_state;
    uint8_t      promo_cursor;    // 0=queen 1=rook 2=bishop 3=knight
} GameState_t;

typedef enum {
    MOVE_SRC_PLAYER,
    MOVE_SRC_OPPONENT,
} MoveSrc_t;

typedef struct {
    MoveSrc_t src;
    uint8_t   from_sq;
    uint8_t   to_sq;
    char      uci[6];
    // clock data piggy-backed on opponent move events (0 if not a timed game)
    uint32_t  white_clock_ms;
    uint32_t  black_clock_ms;
    uint32_t  white_inc_ms;
    uint32_t  black_inc_ms;
} MoveEvent_t;

// --- queue handles -----------------------------------------------------------

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

void chess_engine_init();
