#include "chesslink.h"
#include <SPI.h>

// --- chain topology ----------------------------------------------------------
//
// 8 SN74HC165 shift registers, one per rank
//
// physical layout (sitting at the board, ESP PCB to the right):
//
//   rank 8 SR  ->  rank 7 SR  ->  ...  ->  rank 1 SR  ->  GPIO19 (MISO)
//   (furthest from ESP)                     (QH to ESP)
//
// rank PCBs detect a full rank: each sensor on a given rank connects to
// the SR input for its file. wiring within each SR:
//
//   A-file sensor -> input H (MSB, clocked out first)
//   B-file sensor -> input G
//   ...
//   H-file sensor -> input A (LSB, clocked out last)
//
// so after SPI.transfer x8 with MSBFIRST, the raw uint64 looks like:
//
//   bits 63..56 = rank 8  (bit63=A8, bit62=B8, ... bit56=H8)
//   bits 55..48 = rank 7
//   ...
//   bits  7.. 0 = rank 1  (bit7=A1,  bit6=B1,  ... bit0=H1)
//
// to get canonical sq = rank*8 + file from raw bit position p:
//   rank = p / 8
//   file = 7 - (p % 8)   // H->A maps bits 0->7, so invert
//   sq   = rank*8 + file
//
// A3144 output is active-low, a 0 bit means magnet detected (occupied)

// --- helpers -----------------------------------------------------------------

// pulse SR_LOAD low to latch all parallel inputs at once
// HC165 latches on falling edge of PL, hold 1us before clocking
static inline void sr_latch() {
    digitalWrite(SR_LOAD, LOW);
    delayMicroseconds(1);
    digitalWrite(SR_LOAD, HIGH);
}

// clock out all 64 bits over VSPI
// rank 8 byte comes out first (MSB of result), rank 1 last
static uint64_t sr_read_all() {
    sr_latch();

    uint64_t raw = 0;
    for (int i = 7; i >= 0; i--) {
        uint8_t b = SPI.transfer(0x00);
        raw |= ((uint64_t)b << (i * 8));
    }
    return raw;
}

// convert raw 64-bit SR read to occupied bitmask in canonical sq indexing
// raw bit p -> rank = p/8, file = 7-(p%8), sq = rank*8+file
// A3144 is active-low so invert: bit=0 means occupied
static uint64_t raw_to_occupied(uint64_t raw) {
    uint64_t occupied = 0;
    for (int p = 0; p < 64; p++) {
        bool sensor_low = !((raw >> p) & 1);
        if (sensor_low) {
            int rank = p / 8;
            int file = 7 - (p % 8);
            int sq   = rank * 8 + file;
            occupied |= (1ULL << sq);
        }
    }
    return occupied;
}

// --- task --------------------------------------------------------------------

void task_SensorScan(void *pvParameters) {
    pinMode(SR_LOAD, OUTPUT);
    digitalWrite(SR_LOAD, HIGH);  // idle high, active-low load

    // VSPI, mode 1, 1 MHz (conservative, bump after hardware validation)
    SPI.begin(SR_SCLK, SR_MISO, /*MOSI*/-1, /*SS*/-1);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));

    uint64_t prev_confirmed = 0;
    uint64_t candidate      = 0;
    uint8_t  stable_count   = 0;

    TickType_t xLastWake = xTaskGetTickCount();

    for (;;) {
        uint64_t occupied = raw_to_occupied(sr_read_all());

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

            BoardState_t msg = {
                .occupied     = occupied,
                .timestamp_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS,
            };

            // drop oldest rather than blocking the scan loop
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
