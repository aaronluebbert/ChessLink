#include "chesslink.h"
#include "chess_engine.h"

// --- queue handles -----------------------------------------------------------

QueueHandle_t xQ_BoardState;
QueueHandle_t xQ_LedCmd;
QueueHandle_t xQ_GameState;
QueueHandle_t xQ_PlayerMove;
QueueHandle_t xQ_OpponentMove;
QueueHandle_t xQ_ButtonEvent;

// --- setup -------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    Serial.println("[chesslink] booting...");

    // build knight/king attack tables before any tasks start
    chess_engine_init();

    xQ_BoardState   = xQueueCreate(Q_BOARD_STATE_DEPTH,  sizeof(BoardState_t));
    xQ_LedCmd       = xQueueCreate(Q_LED_CMD_DEPTH,       sizeof(LedCmd_t));
    xQ_GameState    = xQueueCreate(Q_GAME_STATE_DEPTH,    sizeof(GameState_t));
    xQ_PlayerMove   = xQueueCreate(Q_MOVE_DEPTH,          sizeof(MoveEvent_t));
    xQ_OpponentMove = xQueueCreate(Q_MOVE_DEPTH,          sizeof(MoveEvent_t));
    xQ_ButtonEvent  = xQueueCreate(Q_BUTTON_DEPTH,        sizeof(ButtonEvent_t));

    if (!xQ_BoardState || !xQ_LedCmd || !xQ_GameState ||
        !xQ_PlayerMove || !xQ_OpponentMove || !xQ_ButtonEvent) {
        Serial.println("[chesslink] fatal: queue allocation failed");
        while (1) {}
    }

    // core 1
    xTaskCreatePinnedToCore(task_SensorScan, "SensorScan", STACK_SENSOR,  NULL, PRI_SENSOR,  NULL, CORE_SENSOR);
    xTaskCreatePinnedToCore(task_LedControl, "LedControl", STACK_LED,     NULL, PRI_LED,     NULL, CORE_LED);
    xTaskCreatePinnedToCore(task_GameLogic,  "GameLogic",  STACK_GAME,    NULL, PRI_GAME,    NULL, CORE_GAME);
    xTaskCreatePinnedToCore(task_Buttons,    "Buttons",    STACK_BUTTONS, NULL, PRI_BUTTONS, NULL, CORE_BUTTONS);

    // core 0
    xTaskCreatePinnedToCore(task_LcdDisplay, "LcdDisplay", STACK_DISPLAY, NULL, PRI_DISPLAY, NULL, CORE_DISPLAY);
    xTaskCreatePinnedToCore(task_Network,    "Network",    STACK_NETWORK, NULL, PRI_NETWORK, NULL, CORE_NETWORK);

    Serial.println("[chesslink] all tasks spawned");
}

// loop() runs on core 1 at priority 1 -- park it
void loop() {
    vTaskDelay(portMAX_DELAY);
}
