#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// --- pin definitions ---------------------------------------------------------

// SN74HC165 shift registers (VSPI)
#define SR_SCLK       18   // VSPI CLK
#define SR_MISO       19   // VSPI MISO (QH from rank 1 SR)
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

// tactile buttons (input-only pins, no pullup available -- use external)
#define BTN_CYCLE     34   // cycle through promotion choices
#define BTN_CONFIRM   35   // confirm selection

// --- board geometry ----------------------------------------------------------

#define NUM_SQUARES   64
// sq = rank*8 + file  (a1=0, h8=63)

// --- RTOS task config --------------------------------------------------------

// stack sizes in words
#define STACK_SENSOR    4096
#define STACK_LED       4096
#define STACK_GAME      6144
#define STACK_DISPLAY   4096
#define STACK_NETWORK   8192
#define STACK_BUTTONS   2048

// priorities -- higher = more urgent
#define PRI_SENSOR      5   // must hit 50 Hz, nothing preempts it
#define PRI_LED         4   // RMT timing sensitive
#define PRI_GAME        3   // pure compute, no I/O
#define PRI_BUTTONS     3   // same as game -- button response needs to be snappy
#define PRI_NETWORK     2   // above display so incoming moves aren't starved
#define PRI_DISPLAY     1   // late redraw is fine, missed move is not

// core assignments
//
// core 1 -- sensor, LED, game logic, buttons
//   all pure compute or dedicated peripherals (VSPI, RMT, GPIO)
//   no WiFi stack interference
//
// core 0 -- network, display
//   ESP32 WiFi/TCP stack is a system task on core 0
//   network must live here, display stays here too (HSPI, infrequent redraws)
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
#define SENSOR_SCAN_MS    20    // 50 Hz
#define LED_UPDATE_MS     16    // ~60 fps
#define BTN_POLL_MS       20    // 50 Hz button poll
#define BTN_DEBOUNCE_MS   50    // ms of stable state before registering press
#define DEBOUNCE_SCANS    3     // sensor debounce: consecutive identical reads

// --- data types --------------------------------------------------------------

// raw 64-bit occupancy bitmask (bit N = square N occupied)
typedef struct {
    uint64_t occupied;
    uint32_t timestamp_ms;
} BoardState_t;

// LED command: game logic -> LED control
typedef enum {
    LED_CMD_SET_SQUARE,
    LED_CMD_SET_ALL,
    LED_CMD_CLEAR,
    LED_CMD_PATTERN,   // mask squares lit with color, rest dimmed
} LedCmdType_t;

typedef struct {
    LedCmdType_t type;
    uint8_t  square;
    uint8_t  r, g, b;
    uint64_t mask;
} LedCmd_t;

// button events: button task -> game logic
typedef enum {
    BTN_EVT_CYCLE,    // BTN_CYCLE pressed
    BTN_EVT_CONFIRM,  // BTN_CONFIRM pressed
} ButtonEvent_t;

// promotion picker state -- embedded in GameState_t so display knows what to show
typedef enum {
    PROMO_NONE,       // not in a promotion
    PROMO_SELECTING,  // player is choosing a piece
} PromoState_t;

// game state snapshot: game logic -> LCD display
typedef enum {
    GAME_MODE_IDLE,
    GAME_MODE_LOCAL,
    GAME_MODE_LICHESS,
    GAME_MODE_ANALYSIS,
} GameMode_t;

typedef struct {
    GameMode_t  mode;
    char        fen[92];
    uint64_t    occupied;
    uint8_t     active_color;   // 0=white, 1=black
    int16_t     eval_cp;        // centipawn eval, INT16_MIN if unknown
    char        last_move[6];
    char        status_msg[32];

    // promotion picker -- display shows picker when promo_state == PROMO_SELECTING
    PromoState_t promo_state;
    uint8_t      promo_cursor;  // 0=queen 1=rook 2=bishop 3=knight
} GameState_t;

// move events between tasks
typedef enum {
    MOVE_SRC_PLAYER,
    MOVE_SRC_OPPONENT,
} MoveSrc_t;

typedef struct {
    MoveSrc_t src;
    uint8_t   from_sq;
    uint8_t   to_sq;
    char      uci[6];
} MoveEvent_t;

// --- queue handles (defined in main.cpp) -------------------------------------

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
// call once from setup() before any tasks start
void chess_engine_init();
