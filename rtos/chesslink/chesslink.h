#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// - pin definitions -

// SN74HC165 shift registers (VSPI)
#define SR_SCLK       18   // VSPI CLK
#define SR_MISO       19   // VSPI MISO (QH from chain)
#define SR_LOAD       5    // bit-banged LOAD (active-low parallel load)
// CLK INH tied to GND on each column board

// WS2812B via 74AHCT125 level shifter
#define LED_DATA_PIN  4    // RMT output -> level shifter -> LED_DATA_5V

// ST7789 LCD (HSPI) -- 170x320
#define LCD_MOSI      13
#define LCD_SCLK      14
#define LCD_CS        15
#define LCD_DC        2
#define LCD_RST       -1
#define LCD_BL        32

// tactile buttons
#define BTN_MODE      34
#define BTN_CONFIRM   35

// - board geometry -

#define NUM_SQUARES   64
#define NUM_COLS      8
#define NUM_ROWS      8
// sq = row*8 + col  (a1=0, h8=63)

// - RTOS task config -

// stack sizes in words
#define STACK_SENSOR    4096
#define STACK_LED       4096
#define STACK_GAME      6144
#define STACK_DISPLAY   4096
#define STACK_NETWORK   8192

// priorities -- higher number = more urgent
#define PRI_SENSOR      5   // must hit 50 Hz deadline, nothing should preempt it
#define PRI_LED         4   // RMT timing sensitive
#define PRI_GAME        3   // pure compute, no I/O
#define PRI_NETWORK     2   // higher than display so incoming moves aren't starved
#define PRI_DISPLAY     1   // lowest -- a late redraw is fine, a missed move is not

// core assignments
//
// core 1 -- sensor, LED, game logic
//   all three are pure compute or dedicated peripheral (VSPI, RMT)
//   no WiFi stack interference, game logic gets clean uncontested cycles
//
// core 0 -- network, display
//   ESP32 WiFi/TCP stack runs as a system task on core 0, so network
//   must live here to avoid cross-core contention on the IP stack
//   display (HSPI) stays here too since redraws are infrequent and short
#define CORE_SENSOR     1
#define CORE_LED        1
#define CORE_GAME       1
#define CORE_DISPLAY    0
#define CORE_NETWORK    0

// queue depths
#define Q_BOARD_STATE_DEPTH   4
#define Q_LED_CMD_DEPTH       8
#define Q_GAME_STATE_DEPTH    4
#define Q_MOVE_DEPTH          4

// timing
#define SENSOR_SCAN_MS        20    // 50 Hz
#define LED_UPDATE_MS         16    // ~60 fps
#define DEBOUNCE_SCANS        3     // consecutive identical reads to confirm change

// - data types -

// raw 64-bit occupancy bitmask (bit N = square N occupied)
typedef struct {
    uint64_t occupied;
    uint32_t timestamp_ms;
} BoardState_t;

// LED command: game logic -> LED control
typedef enum {
    LED_CMD_SET_SQUARE,   // set one square's color
    LED_CMD_SET_ALL,      // fill entire board
    LED_CMD_CLEAR,        // all off
    LED_CMD_PATTERN,      // highlight mask with color, dimmed elsewhere
} LedCmdType_t;

typedef struct {
    LedCmdType_t type;
    uint8_t  square;      // used by SET_SQUARE
    uint8_t  r, g, b;
    uint64_t mask;        // used by PATTERN
} LedCmd_t;

// game state snapshot: game logic -> LCD display
typedef enum {
    GAME_MODE_IDLE,
    GAME_MODE_LOCAL,      // two local players
    GAME_MODE_LICHESS,    // lichess API
    GAME_MODE_ANALYSIS,
} GameMode_t;

typedef struct {
    GameMode_t mode;
    char       fen[92];
    uint64_t   occupied;         // mirror of board bitmask for display rendering
    uint8_t    active_color;     // 0=white, 1=black
    int16_t    eval_cp;          // centipawn eval, INT16_MIN if unknown
    char       last_move[6];     // e.g. "e2e4"
    char       status_msg[32];
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

// - queue handles (defined in main.cpp) -

extern QueueHandle_t xQ_BoardState;    // sensor -> game logic
extern QueueHandle_t xQ_LedCmd;        // game logic -> LED control
extern QueueHandle_t xQ_GameState;     // game logic -> LCD display
extern QueueHandle_t xQ_PlayerMove;    // game logic -> network
extern QueueHandle_t xQ_OpponentMove;  // network -> game logic

// - task declarations -

void task_SensorScan  (void *pvParameters);
void task_LedControl  (void *pvParameters);
void task_GameLogic   (void *pvParameters);
void task_LcdDisplay  (void *pvParameters);
void task_Network     (void *pvParameters);