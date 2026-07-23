#include "chesslink.h"

// --- task --------------------------------------------------------------------
//
// GPIO 34 and 35 are input-only on the ESP32 -- no internal pullup available,
// so the PCB must have external pullups (10k to 3.3V recommended).
// buttons are active-low: idle = HIGH, pressed = LOW.
//
// debounce: track how long each pin has been stable, fire event on
// leading edge after BTN_DEBOUNCE_MS of consistent LOW.

void task_Buttons(void *pvParameters) {
    pinMode(BTN_CYCLE,   INPUT);
    pinMode(BTN_CONFIRM, INPUT);

    // state per button
    struct BtnState {
        bool     last_raw;     // last raw digitalRead
        bool     confirmed;    // last debounced state
        uint32_t stable_since; // millis() when current raw state started
    } btns[2] = {};

    // initialize to current pin state so we don't fire on boot
    btns[0].last_raw = btns[0].confirmed = digitalRead(BTN_CYCLE);
    btns[1].last_raw = btns[1].confirmed = digitalRead(BTN_CONFIRM);
    btns[0].stable_since = btns[1].stable_since = millis();

    const int pins[2]            = { BTN_CYCLE, BTN_CONFIRM };
    const ButtonEvent_t evts[2]  = { BTN_EVT_CYCLE, BTN_EVT_CONFIRM };

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint32_t now = millis();

        for (int i = 0; i < 2; i++) {
            bool raw = digitalRead(pins[i]);

            if (raw != btns[i].last_raw) {
                // pin changed -- reset stable timer
                btns[i].last_raw    = raw;
                btns[i].stable_since = now;
            } else if ((now - btns[i].stable_since) >= BTN_DEBOUNCE_MS
                       && raw != btns[i].confirmed) {
                // stable for long enough and state actually changed
                btns[i].confirmed = raw;

                // fire event on falling edge only (HIGH->LOW = press)
                if (raw == LOW) {
                    ButtonEvent_t evt = evts[i];
                    xQueueSend(xQ_ButtonEvent, &evt, 0);
                }
            }
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(BTN_POLL_MS));
    }

    vTaskDelete(NULL);
}
