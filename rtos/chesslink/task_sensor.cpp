#include "chesslink.h"
#include "board_map.h"

// --- chain topology ----------------------------------------------------------
//
// all 64 sensors are one HC165 daisy chain on a single data line (SR_MISO)
// read is bit-banged: latch once, then clock out 64 bits in a row
//
// the chain clocks out in canonical order a1,b1..h1, a2..h2, ... a8..h8, so
// read-index i is square i (see cl_sensor_sq in board_map.h, identity map)
//
// A3144 output is active-low, a LOW bit means magnet detected (occupied)
// this replaces the old per-rank SPI read, the LCD keeps HSPI to itself so
// nothing else touches SR_SCLK now

// --- helpers -----------------------------------------------------------------

// pulse SR_LOAD low to latch all parallel inputs at once
// HC165 latches on the falling edge of PL, hold a moment before clocking
static inline void sr_latch() {
    digitalWrite(SR_LOAD, LOW);
    delayMicroseconds(5);
    digitalWrite(SR_LOAD, HIGH);
    delayMicroseconds(5);
}

// latch, then clock out all 64 bits, reading one bit per tick from SR_MISO
// bit is read before the clock pulse, first bit out is QH of the chain
static uint64_t sr_read_all() {
    sr_latch();

    uint64_t occupied = 0;
    for (int bit = 0; bit < NUM_SQUARES; bit++) {
        // active-low, a LOW reading means a piece is on this square
        if (!digitalRead(SR_MISO))
            occupied |= (1ULL << cl_sensor_sq(bit));

        digitalWrite(SR_SCLK, HIGH);
        delayMicroseconds(5);
        digitalWrite(SR_SCLK, LOW);
        delayMicroseconds(5);
    }
    return occupied;
}

// --- task --------------------------------------------------------------------

void task_SensorScan(void *pvParameters) {
    pinMode(SR_LOAD, OUTPUT);
    digitalWrite(SR_LOAD, HIGH);      // idle high, active-low load

    pinMode(SR_SCLK, OUTPUT);
    digitalWrite(SR_SCLK, LOW);       // clock idles low, bit-banged

    // pullup so an unconnected chain reads high (empty) instead of stuck-on
    pinMode(SR_MISO, INPUT_PULLUP);

    uint64_t prev_confirmed = 0;
    uint64_t candidate      = 0;
    uint8_t  stable_count   = 0;

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint64_t occupied = sr_read_all();

        // need DEBOUNCE_SCANS identical reads in a row before accepting a change
        if (occupied == candidate) {
            stable_count++;
        } else {
            candidate    = occupied;
            stable_count = 0;
        }

        if (stable_count >= DEBOUNCE_SCANS && occupied != prev_confirmed) {
            prev_confirmed = occupied;
            stable_count   = 0;

            BoardState_t msg = { .occupied = occupied };

            // drop oldest rather than blocking the scan loop
            if (xQueueSend(xQ_BoardState, &msg, 0) != pdTRUE) {
                BoardState_t discard;
                xQueueReceive(xQ_BoardState, &discard, 0);
                xQueueSend(xQ_BoardState, &msg, 0);
            }
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(SENSOR_SCAN_MS));
    }

    vTaskDelete(NULL);
}
