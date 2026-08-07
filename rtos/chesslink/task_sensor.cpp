#include "chesslink.h"
#include <SPI.h>

// --- chain topology ----------------------------------------------------------
//
// The 8 HC165s read over the same VSPI bus as the LCD (shared SCK 18, MOSI 23;
// the SR data comes back on MISO 19, which the LCD doesn't use). To read the
// board we pulse SR_LOAD (GPIO5) to parallel-load the sensors, then clock 8
// bytes out over SPI. byte b -> rank b; MSB..LSB = file a..h; active-low
// (0 = occupied). The whole read is wrapped in xSPI18Mutex so an LCD write can't
// toggle GPIO18 in the middle of it; the LCD's CS is de-asserted meanwhile, so
// the panel ignores these clocks.

// --- helpers -----------------------------------------------------------------

static inline void sr_latch() {
    digitalWrite(SR_LOAD, LOW);       // parallel-load the current sensor state
    delayMicroseconds(5);
    digitalWrite(SR_LOAD, HIGH);      // back to shift mode
    delayMicroseconds(5);
}

static uint64_t sr_read_all() {
    sr_latch();                           // parallel-load; QH now holds square a1

    // Off-by-one fix: the '165 presents the first square (a1) on the data line
    // right after the load, BEFORE any clock, but the SPI master shifts it out
    // on the first edge before it samples -- so a plain 8-byte read misses a1 and
    // slides the whole board up by one square. Read that first bit straight off
    // MISO, then clock the rest and shift them back down into place.
    uint8_t a1_occ = (digitalRead(SR_MISO) == LOW) ? 1 : 0;   // active-low

    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    uint64_t shifted = 0;                  // bit k here = physical square (k+1)
    for (int b = 0; b < 8; b++) {
        uint8_t byte = SPI.transfer(0x00);
        for (int f = 0; f < 8; f++) {     // MSB = file a .. LSB = file h
            if (!((byte >> (7 - f)) & 0x01))   // active-low: 0 = piece present
                shifted |= (1ULL << (b * 8 + f));
        }
    }
    SPI.endTransaction();

    // square 0 (a1) from the direct read, squares 1..63 from the clocked bits;
    // the 64th clocked bit is a phantom 65th square and drops off the top.
    return (shifted << 1) | a1_occ;
}

// --- task --------------------------------------------------------------------

void task_SensorScan(void *pvParameters) {
    pinMode(SR_LOAD, OUTPUT);
    digitalWrite(SR_LOAD, HIGH);      // idle high, active-low load

    // Let the display bring up the shared VSPI bus first, then add MISO (19) so
    // we can clock the shift-register chain back in over the same bus.
    vTaskDelay(pdMS_TO_TICKS(500));
    SPI.begin(SR_SCLK, SR_MISO, LCD_MOSI, -1);
    // NOTE: do NOT call pinMode(SR_MISO, ...) here -- it detaches the pin from the
    // SPI peripheral's MISO input and every byte reads back 0x00 (whole board
    // reads "occupied"). SPI.begin already routes MISO; digitalRead still works.

    uint64_t prev_confirmed = 0;
    uint64_t candidate      = 0;
    uint8_t  stable_count   = 0;

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        // hold the shared-clock lock for the whole 64-bit read so the LCD can't
        // toggle GPIO18 in the middle of it
        xSemaphoreTake(xSPI18Mutex, portMAX_DELAY);
        uint64_t occupied = sr_read_all();
        xSemaphoreGive(xSPI18Mutex);

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
