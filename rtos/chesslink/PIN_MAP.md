# ChessLink firmware to PCB pin map

This table is the reference for how the firmware pins in `chesslink.h` connect to the PCB nets.

| GPIO | PCB net / function | firmware `#define` | dir | notes |
|---|---|---|---|---|
| 23 | SPI_MOSI (LCD only) | `LCD_MOSI` | out | VSPI MOSI/SDA, not wired to the HC165s |
| 18 | SPI_SCLK (shared) | `LCD_SCLK` / `SR_SCLK` | out | one net drives the LCD SCK and every HC165 CLK |
| 19 | SR data in (MISO) | `SR_MISO` | in | HC165 QH chain back to the ESP32, through the 5 V to 3.3 V divider |
| 5  | SR load (SH/LD) | `SR_LOAD` | out | strapping pin, idle HIGH = shift mode |
| 15 | LCD chip select | `LCD_CS` | out | strapping pin, owned by the LCD |
| 2  | LCD data/command | `LCD_DC` | out | strapping pin, must be LOW at boot (the LCD does not drive it) |
| 4  | LCD reset | `LCD_RST` | out | |
| 32 | LCD backlight | `LCD_BL` | out | |
| 25 | WS2812B data | `LED_DATA_PIN` | out | RMT, 3.3 V through the 74AHCT125 to 5 V DIN |
| 36 | Button Up | `BTN_UP` | in | input-only, external 10k to 3V3, active low |
| 39 | Button Down | `BTN_DOWN` | in | input-only, external 10k to 3V3, active low |
| 34 | Button Confirm | `BTN_CONFIRM` | in | input-only, external 10k to 3V3, active low |
| 35 | Button Cancel | `BTN_CANCEL` | in | input-only, external 10k to 3V3, active low |

Spare on the PCB: 21, 22, 26, 27, 33, 13, 14. Reserved: 1 and 3 (UART debug), 6 to 11 (flash).

## Shared VSPI bus (LCD and shift registers)

The ideaspark dev board wires the LCD and the HC165 chain onto the ESP32 default VSPI pins: SCK 18, MISO 19, MOSI 23.

- The LCD is driven with `Arduino_GFX` over `Arduino_ESP32SPI` on these pins.
- The sensor task calls `SPI.begin(SR_SCLK, SR_MISO, LCD_MOSI, -1)` and reads the shift registers on the same bus, in `SPI_MODE0`, MSB first, after a latch pulse on `SR_LOAD` (GPIO 5).
- `task_sensor` and `task_display` each hold `xSPI18Mutex` around their bus access, so a sensor read cannot be clocked apart by an LCD write.
- Shifting during an LCD write is harmless. The shift registers are re-latched before every read, and the LCD chip select is de-asserted while the sensor reads, so the panel ignores those clocks.

## Sensor read order

The 8 HC165s form one 64-bit chain on a single data line. After a latch, bits come out in this order: a1, b1 through h1, a2 through h2, and so on up to h8 (rank 1 first). Sensors are active-low, so 0 means a piece is present.

The HC165 puts the first bit (a1) on the data line right after the load pulse, before any clock edge. The SPI master would miss it, so `sr_read_all()` samples that bit directly from MISO, clocks in the remaining bits, and shifts them into place. If a rank or file ever comes out mirrored or shifted, `sr_read_all()` in `task_sensor.cpp` is the only place to adjust. Everything downstream uses the canonical square index `sq = rank * 8 + file`.

## LED chain order

`LED_DATA_PIN` is GPIO 25, wired through the level shifter. LED 0 is h8, and the chain runs h to a across each rank, ranks 8 down to 1:

```
led = (7 - rank) * 8 + (7 - file)
```

This is implemented once, in `cl_led_index()` in `board_map.h`.
