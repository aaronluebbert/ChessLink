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

## Shared VSPI bus (LCD + shift registers)

The ideaspark board wires the LCD and the HC165 chain onto the ESP32 default VSPI
pins: **SCK 18, MISO 19, MOSI 23**. `SPI.begin(18, 19, 23, -1)` runs once in
`setup()` (SS left off so GPIO5 stays free for the SR latch). The LCD is driven by
hardware SPI (`Adafruit_ST7789(&SPI, ...)`); the shift registers are read on the
same bus with `SPI.transfer` (MODE0, MSBFIRST) after a `SR_LOAD` (GPIO5) latch.

The sensor (`task_sensor`) and display (`task_display`) each hold `xSPI18Mutex`
around their bus access so a sensor read can't be clocked apart by an LCD write.
The SR shifting during an LCD write is harmless -- we re-latch before every read,
and the LCD's CS (GPIO15) is de-asserted while the sensor reads, so the panel
ignores those clocks.

**Square -> bit order:** the chain clocks out in canonical order
(a1,b1..h8). Read over SPI, byte `b` holds rank `b` with file a in the MSB and
file h in the LSB; active-low (0 = occupied). This reproduces the validated
identity map (host-tested in `srmap.c`). If a rank/file comes out mirrored,
adjust the unpack loop in `sr_read_all`; if the whole read is wrong, try
`SPI_MODE0` -> `SPI_MODE2` (clock idle polarity).

## Bring-up watch items

- **GPIO5 (SH/LD):** bit-banged parallel load, idle HIGH = shift mode. If the
  latch ever misbehaves, re-assert `pinMode(SR_LOAD, OUTPUT)` at task start.
- **Square -> bit order:** the chain clocks out in canonical order
  (a1,b1..h1, a2..h2, ... a8..h8), so `cl_sensor_sq()` in `board_map.h` is the
  identity map and sensors are active-low. This came straight from the validated
  sketch. If a rank/file comes out mirrored, adjust `cl_sensor_sq()` only;
  everything downstream uses canonical sq = rank*8+file.
- **LEDs:** `LED_DATA_PIN` is 25; wire the WS2812B data line there (through the
  level shifter). Chain order is LED 0 = h8, files h..a across each rank, ranks
  8..1, encoded in `cl_led_index()`. The strip isn't required for the menu/board
  UI to work.
