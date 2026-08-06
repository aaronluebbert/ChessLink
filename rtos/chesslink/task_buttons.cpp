#include "chesslink.h"

// --- task --------------------------------------------------------------------
//
// GPIO 34 and 35 are input-only on the ESP32 -- no internal pullup available,
// so the PCB must have external pullups (10k to 3.3V recommended).
// buttons are active-low: idle = HIGH, pressed = LOW.
//
// debounce: track how long each pin has been stable, fire event on
// leading edge after BTN_DEBOUNCE_MS of consistent LOW.

#define NUM_BTNS 4

void task_Buttons(void *pvParameters) {
    const int         pins[NUM_BTNS] = { BTN_UP,     BTN_DOWN,     BTN_CONFIRM,     BTN_CANCEL     };
    const ButtonEvent_t evts[NUM_BTNS] = { BTN_EVT_UP, BTN_EVT_DOWN, BTN_EVT_CONFIRM, BTN_EVT_CANCEL };

    // state per button
    struct BtnState {
        bool     last_raw;     // last raw digitalRead
        bool     confirmed;    // last debounced state
        uint32_t stable_since; // millis() when current raw state started
    } btns[NUM_BTNS] = {};

    // initialize to current pin state so we don't fire on boot
    for (int i = 0; i < NUM_BTNS; i++) {
        pinMode(pins[i], INPUT);
        btns[i].last_raw = btns[i].confirmed = digitalRead(pins[i]);
        btns[i].stable_since = millis();
    }

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint32_t now = millis();

        for (int i = 0; i < NUM_BTNS; i++) {
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
