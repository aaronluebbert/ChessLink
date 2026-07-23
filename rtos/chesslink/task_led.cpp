#include "chesslink.h"
#include <FastLED.h>

// - LED buffer -

static CRGB leds[NUM_SQUARES];

// TODO: remap if physical strip routing is serpentine or column-major
static inline uint8_t sq_to_led(uint8_t sq) {
    return sq;
}

// - command handlers -

static void apply_led_cmd(const LedCmd_t *cmd) {
    switch (cmd->type) {

        case LED_CMD_SET_SQUARE: {
            uint8_t idx = sq_to_led(cmd->square);
            if (idx < NUM_SQUARES)
                leds[idx] = CRGB(cmd->r, cmd->g, cmd->b);
            break;
        }

        case LED_CMD_SET_ALL: {
            CRGB color(cmd->r, cmd->g, cmd->b);
            for (int i = 0; i < NUM_SQUARES; i++) leds[i] = color;
            break;
        }

        case LED_CMD_CLEAR:
            FastLED.clear();
            break;

        case LED_CMD_PATTERN: {
            // lit squares get full color, unlit squares get ~10% for context
            CRGB on_color(cmd->r, cmd->g, cmd->b);
            CRGB off_color = on_color;
            off_color.nscale8(26);

            for (int sq = 0; sq < NUM_SQUARES; sq++) {
                leds[sq_to_led(sq)] = (cmd->mask & (1ULL << sq)) ? on_color : off_color;
            }
            break;
        }
    }
}

// - task -

void task_LedControl(void *pvParameters) {
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, NUM_SQUARES);
    FastLED.setBrightness(64);  // ~25%, increase after thermal check
    FastLED.clear(true);

    LedCmd_t cmd;
    TickType_t xLastShow = xTaskGetTickCount();

    for (;;) {
        // drain all pending commands before pushing to strip
        while (xQueueReceive(xQ_LedCmd, &cmd, 0) == pdTRUE)
            apply_led_cmd(&cmd);

        // cap FastLED.show() to ~60 fps regardless of command rate
        TickType_t now = xTaskGetTickCount();
        if ((now - xLastShow) >= pdMS_TO_TICKS(LED_UPDATE_MS)) {
            FastLED.show();
            xLastShow = now;
        }

        // block up to one frame period waiting for next command
        if (xQueueReceive(xQ_LedCmd, &cmd, pdMS_TO_TICKS(LED_UPDATE_MS)) == pdTRUE)
            apply_led_cmd(&cmd);
    }

    vTaskDelete(NULL);
}
