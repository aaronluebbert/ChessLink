# ChessLink firmware <-> PCB v3 pin map

Firmware pins (chesslink.h) now match `ChessLink_Pin_Map_v3.xlsx` exactly.

| GPIO | PCB net / function | firmware #define | dir | notes |
|---|---|---|---|---|
| 23 | SPI_MOSI (LCD only) | `LCD_MOSI` | out | VSPI MOSI/SDA. not wired to the HC165s |
| 18 | SPI_SCLK (shared) | `LCD_SCLK` / `SR_SCLK` | out | one net drives LCD SCK **and** every HC165 CLK |
| 19 | SR data in (MISO) | `SR_MISO` | in | HC165 QH chain -> ESP. needs the 5V->3.3V divider |
| 5  | SR load (SH/LD) | `SR_LOAD` | out | bit-banged. strapping pin, idle HIGH = shift mode |
| 15 | LCD chip select | `LCD_CS` | out | strapping pin, LCD owns it |
| 2  | LCD data/command | `LCD_DC` | out | strapping pin, must be LOW at boot (LCD doesn't drive it) |
| 4  | LCD reset | `LCD_RST` | out | |
| 32 | LCD backlight | `LCD_BL` | out | |
| 25 | WS2812B data | `LED_DATA_PIN` | out | RMT, 3.3V -> 74AHCT125 -> 5V DIN |
| 36 | Button Up | `BTN_UP` | in | input-only, ext 10k to 3V3, active low |
| 39 | Button Down | `BTN_DOWN` | in | input-only, ext 10k to 3V3, active low |
| 34 | Button Confirm | `BTN_CONFIRM` | in | input-only, ext 10k to 3V3, active low |
| 35 | Button Back | `BTN_BACK` | in | input-only, ext 10k to 3V3, active low |

Spare on the PCB: 21, 22, 26, 27, 33, 13, 14. Reserved: 1/3 (UART debug), 6-11 (flash).

## Shared SPI bus (important)

The LCD and the shift-register chain share VSPI (SCLK on GPIO18). The firmware
handles this by giving each device its **own** SPI transaction:

- `task_sensor` wraps every 64-bit read in `SPI.beginTransaction()` /
  `SPI.endTransaction()` (per read, at 1 MHz, mode 1). It never holds the bus
  across the scan loop.
- The LCD driver (Adafruit) already brackets each draw in its own transaction
  at its own speed/mode.

The Arduino ESP32 `SPIClass` serializes transactions on the shared bus, so the
two tasks interleave safely. (The earlier "black screen" was the sensor task
holding one transaction open forever, which blocked the LCD at `tft.init()`.)

## Bring-up watch items

- **GPIO5 dual role:** it's the HC165 SH/LD (bit-banged) and also VSPI's nominal
  hardware-SS. ESP32 Arduino SPI does not auto-drive SS during transfers, so the
  sensor's `digitalWrite(SR_LOAD, ...)` stays in control. If you ever see the
  latch misbehave, re-assert `pinMode(SR_LOAD, OUTPUT)` after SPI init.
- **Square -> bit order:** `raw_to_occupied()` in `task_sensor.cpp` assumes a
  specific mapping (raw bit p -> rank=p/8, file=7-(p%8), sensors active-low).
  Confirm this against the actual PCB routing of the 8 sensor boards and the
  A3144/reed orientation; adjust that one function if a rank/file comes out
  mirrored or rotated. Everything downstream uses canonical sq = row*8+col.
- **LEDs:** `LED_DATA_PIN` is 25; wire the WS2812B data line there (through the
  level shifter). The strip isn't required for the menu/board UI to work.
