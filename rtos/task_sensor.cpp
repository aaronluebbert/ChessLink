#include "chesslink.h"
#include <SPI.h>

// helpers

// pulse SR_LOAD low to latch parallel inputs, HC165 samples on falling edge of PL
static inline void sr_latch() {
    digitalWrite(SR_LOAD, LOW);
    delayMicroseconds(1);
    digitalWrite(SR_LOAD, HIGH);
}

// clock out 64 bits (8 chips x 8 bits) over VSPI.
// SPI mode 1 (CPOL=0, CPHA=1) -- HC165 QH presents data on falling CLK edge
static uint64_t sr_read_all() {
    sr_latch();

    uint64_t raw = 0;
    // chip 0 (col a) arrives first in the chain -> placed in MSB position
    for (int byte_idx = 7; byte_idx >= 0; byte_idx--) {
        uint8_t b = SPI.transfer(0x00);
        raw |= ((uint64_t)b << (byte_idx * 8));
    }
    return raw;
}

// map chip+bit -> canonical square index.
// chip = column (0=a ... 7=h), bit = row within chip (0=rank1 ... 7=rank8)
static inline uint8_t bit_to_square(int chip, int bit) {
    return (uint8_t)(bit * 8 + chip);  // sq = row*8 + col
}

// A3144 output goes LOW when field > Bop -- so a '0' bit means occupied
static uint64_t raw_to_occupied(uint64_t raw) {
    uint64_t occupied = 0;
    for (int chip = 0; chip < 8; chip++) {
        uint8_t byte_val = (uint8_t)(raw >> (chip * 8));
        for (int bit = 0; bit < 8; bit++) {
            bool sensor_low = !(byte_val & (0x80 >> bit));
            if (sensor_low) {
                occupied |= (1ULL << bit_to_square(chip, bit));
            }
        }
    }
    return occupied;
}

// task

void task_SensorScan(void *pvParameters) {
    pinMode(SR_LOAD, OUTPUT);
    digitalWrite(SR_LOAD, HIGH);  // idle high, active-low load

    // VSPI, mode 1, 1 MHz (conservative first pass)
    SPI.begin(SR_SCLK, SR_MISO, /*MOSI=*/-1, /*SS=*/-1);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));

    uint64_t prev_confirmed = 0;
    uint64_t candidate      = 0;
    uint8_t  stable_count   = 0;

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint64_t occupied = raw_to_occupied(sr_read_all());

        // require DEBOUNCE_SCANS consecutive identical reads before accepting change
        if (occupied == candidate) {
            stable_count++;
        } else {
            candidate    = occupied;
            stable_count = 0;
        }

        if (stable_count >= DEBOUNCE_SCANS && occupied != prev_confirmed) {
            prev_confirmed = occupied;
            stable_count   = 0;

            BoardState_t msg = {
                .occupied     = occupied,
                .timestamp_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS,
            };

            // drop oldest if full rather than blocking the scan loop
            if (xQueueSend(xQ_BoardState, &msg, 0) != pdTRUE) {
                BoardState_t discard;
                xQueueReceive(xQ_BoardState, &discard, 0);
                xQueueSend(xQ_BoardState, &msg, 0);
            }
        }

        vTaskDelayUntil(&xLastWake, pdMS_TO_TICKS(SENSOR_SCAN_MS));
    }

    SPI.endTransaction();
    vTaskDelete(NULL);
}
