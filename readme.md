# ChessLink

**A smart chessboard built from scratch: custom PCBs, 64 Hall-effect sensors, 64 RGB LEDs, and an ESP32 running FreeRTOS. It plays over the board against a friend, against Stockfish, or against real opponents on Lichess.**

![ESP32](https://img.shields.io/badge/MCU-ESP32-blue)
![FreeRTOS](https://img.shields.io/badge/RTOS-FreeRTOS-green)
![KiCad](https://img.shields.io/badge/PCB-KiCad%2010-orange)
![Language](https://img.shields.io/badge/firmware-C%2B%2B%20(Arduino%20core)-lightgrey)
![Status](https://img.shields.io/badge/status-working%20prototype-yellow)

Built for the Microcontroller Lab at Texas Tech University, Summer 2026, as a three-person group project. Inspired by commercial boards like ChessUp 2.

You move a real piece. Magnets in the piece bases trigger a sensor under each square, the firmware works out which legal move you made, and the LEDs under the board guide you. Lift a piece and its legal destinations light up. When an online opponent moves, the from and to squares light up for you to replay. Put a piece somewhere illegal and the square flashes red.

---

## Contents

- [What it does](#what-it-does)
- [A game, start to finish](#a-game-start-to-finish)
- [Hardware](#hardware)
- [Firmware](#firmware)
- [Lichess integration](#lichess-integration)
- [Repository layout](#repository-layout)
- [Build and flash](#build-and-flash)
- [First boot](#first-boot)
- [Known limitations](#known-limitations)
- [Engineering notes](#engineering-notes)
- [Team](#team)

---

## What it does

| Mode | What happens |
|---|---|
| **Local Game** | Two people play over the board. Untimed or with a clock preset (up to 30+0). Detects checkmate, stalemate, and the 50-move draw. |
| **Online Game** | Seeks a human opponent on Lichess (1+0 up to 15+10, rated or casual). Your moves are sent for you, and the opponent's moves are lit on the board for you to replay. |
| **Play Bot** | Challenges Lichess Stockfish, level 1 to 8, with a chosen clock (or untimed) and color (white, black, random). |
| **Famous Games** | Step-through study mode. One game is stored on the device: the Immortal Game (Anderssen vs Kieseritzky, 1851). |
| **WiFi Setup** | Re-opens the phone-based setup page to change WiFi credentials or the Lichess token. |

Also in the box:

- **Full rules on the device.** Castling, en passant, promotion (with an on-screen piece picker), and captures are all detected from sensor occupancy alone. There is no per-piece identification.
- **Start-position check.** Before each game the LEDs light every square that is still missing a piece.
- **Live match screen.** Opponent name and rating, both clocks, and a running-side indicator. Clocks tick locally and resync with the server.
- **Leave-game prompt.** Cancel mid-game opens a confirm. On a Lichess game that resigns (or aborts, early on). On a local game it just drops back to the menu.
- **Analyze on Lichess.** After a game the result screen can import the game to Lichess for analysis.
- **No credentials in the source.** WiFi and the Lichess token are entered from a phone through a captive portal and stored in flash (NVS).

---

## A game, start to finish

1. **Pick a mode** from the LCD menu with the four buttons (up, down, confirm, cancel).
2. **Set up the board.** The LEDs light green on every square that is still missing a piece for the starting position. The game begins once the sensors see the standard setup.
3. **Lift a piece.** The square lights yellow and every legal destination lights green.
4. **Put it down.** The firmware matches the new board occupancy against the legal moves from that square and commits the one that fits. The destination flashes green. A pawn reaching the last rank opens a promotion picker on the LCD.
5. **Opponent's turn.**
   - *Local:* the other player moves.
   - *Online or bot:* the move arrives from Lichess, the from square lights blue and the to square cyan, and the board waits until you have physically replayed it.
6. **Game over.** The result is shown on the LCD ("You won: mate", "Draw: stalemate", and so on). Any button returns to the menu, and online games can be sent to Lichess for analysis.

---

## Hardware

### System overview

```
                    3.7 V LiPo --> USB-C charger module (off-board)
                                        |
                        +---------------+----------------+
                        |                                |
              TLV62568 buck (3.3 V)             TLV61048 boost (5 V)
                        |                                |
          +-------------+-------------+                  |
          |   ESP32 dev board         |                  |
          |   (1.9" ST7789 LCD,       |   LED data --> 74AHCT125 --> 5 V DIN
          |    170 x 320, onboard)    |   SCLK / SR load -> 74AHCT125 -> 5 V
          |                           |   <-- 1k/2k divider <-- SR_QH (5 V)
          |   4 buttons (GPIO 34-39)  |
          +---------------------------+
                        |
        8 x Sensors+LEDs strips, one per rank, chained together:
        each has 8 x A3144 Hall sensor, 8 x WS2812B, 1 x SN74HC165
```

### Boards

Two custom two-layer PCBs, designed in KiCad 10, with JLCPCB production files (Gerbers, BOM, placement) generated in the repo.

| Board | What is on it |
|---|---|
| **ESP + Power** (35 parts) | ESP32 dev-board footprint, TLV62568 buck (3.3 V rail), TLV61048 boost (5 V rail), SN74AHCT125N level shifter, four navigation buttons, power switch, charger-module footprint, header connectors to the sensor strips. |
| **Sensors + LEDs** (41 parts, one per rank) | 8 x A3144 Hall sensors, 8 x WS2812B LEDs, 1 x SN74HC165N parallel-in shift register, decoupling, pull-ups. Eight of these make up the 8x8 board. |

Total across the board: **64 sensors and 64 LEDs**.

Custom footprints and symbols live in `pcb/ChessLink_Footprints.pretty` and `pcb/ChessLink_Symbols`.

### How a square is sensed

1. Each piece has an N52 disc magnet embedded in its base. Pieces and squares are 3D printed; the print is paused mid-way so the magnet can be dropped in and fully enclosed.
2. An A3144 Hall sensor under each square pulls its input low when a magnet is present.
3. The firmware pulses the shift-register load line, then clocks the whole 64-bit chain back in over the ESP32's VSPI bus at 1 MHz. A scan runs every 20 ms, and a change must appear in 3 consecutive scans before it is accepted.

Design-stage calculations put the field from a 10 x 3 mm N52 magnet at roughly 1,100 to 1,300 gauss at a 4.5 mm gap, about 2.5 to 3 times the A3144 trigger threshold, with adjacent-square crosstalk well below threshold at the 57 mm square pitch (within the 50 to 60 mm range in the FIDE handbook). These are calculated margins, not a measured characterization.

### Pin map

The full table is in [`rtos/chesslink/PIN_MAP.md`](rtos/chesslink/PIN_MAP.md). The short version:

| Function | GPIO |
|---|---|
| Shared SPI clock (LCD and shift registers) | 18 |
| Shift-register data in (MISO) / LCD data (MOSI) | 19 / 23 |
| Shift-register load | 5 |
| LCD CS / DC / RST / backlight | 15 / 2 / 4 / 32 |
| WS2812B data (through 74AHCT125) | 25 |
| Buttons: up / down / confirm / cancel | 36 / 39 / 34 / 35 |

---

## Firmware

About 4,200 lines of C++ on the Arduino ESP32 core, structured as six FreeRTOS tasks that communicate only through queues. No task reaches into another task's state.

```
 SensorScan --BoardState--> GameLogic --GameState--> LcdDisplay
                              |  ^  \---LedCmd-----> LedControl
 Buttons ---ButtonEvent------/   |
                                 |  NetUpdate (clocks, players, status, result)
 Network <---NetCmd--- GameLogic ^
    |  \--OpponentMove--> GameLogic
    ^----PlayerMove------ GameLogic
```

| Task | Core | Priority | Stack | Job |
|---|---|---|---|---|
| SensorScan | 1 | 5 | 4 KB | Latch and read 64 sensors, debounce, publish an occupancy bitmask |
| LedControl | 1 | 4 | 4 KB | Drive the WS2812B strip with FastLED, capped at about 60 fps |
| GameLogic | 1 | 3 | 8 KB | Rules, move detection, UI state, game flow |
| Buttons | 1 | 3 | 2 KB | Debounce four active-low buttons |
| Network | 0 | 2 | 16 KB | WiFi, captive portal, Lichess HTTP and streaming |
| LcdDisplay | 0 | 1 | 8 KB | Render every screen to the ST7789 |

Real-time I/O and game logic sit on core 1. The WiFi stack and anything that can block on the network sit on core 0.

### Design decisions worth calling out

- **Move detection by expected occupancy.** When a piece is lifted, the engine precomputes the board occupancy that each legal move from that square would produce. The move is committed when the sensors settle on one of those patterns. That makes captures, castling, en passant, and promotion fall out naturally, since the board is compared as a whole rather than square by square.
- **Opponent-move reconciliation.** When a Lichess opponent moves, the firmware updates its own position, lights the from and to squares, and holds until the physical board matches before handing the turn back.
- **Server position wins.** Online, the move list from Lichess is rebuilt into a FEN and adopted as truth, so the on-device position cannot drift from the game.
- **On-device chess engine.** A bitboard move generator with legal-move filtering, make and unmake, FEN in and out, and UCI conversion (`chess_engine.cpp`, about 700 lines). Move legality does not depend on a server or a Raspberry Pi.
- **One SPI bus, two devices.** The LCD and the shift-register chain share SCK. Both tasks hold a mutex around bus access so a display write cannot interleave with a sensor read.
- **Canonical square index.** `sq = rank * 8 + file` (a1 = 0, h8 = 63) everywhere. Physical wiring order is isolated in `board_map.h` and one function in `task_sensor.cpp`.

---

## Lichess integration

Uses the Lichess Board API over HTTPS. Authentication is a personal API token (`board:play` scope) entered at setup and stored in flash.

| Purpose | Endpoint |
|---|---|
| Find an online opponent | `POST /api/board/seek` |
| Stream a game (moves, clocks, result) | `GET /api/board/game/stream/{id}` |
| Send your move | `POST /api/board/game/{id}/move/{uci}` |
| Resign or abort | `POST /api/board/game/{id}/resign` or `/abort` |
| Play Stockfish | `POST /api/challenge/ai` |
| Export a finished game for analysis | `POST /api/import` |

---

## Repository layout

```
ChessLink/
  rtos/chesslink/              Arduino sketch (open chesslink.ino)
    main.cpp                   queue and task creation
    chesslink.h                pins, task config, shared types
    chess_engine.{h,cpp}       bitboard rules engine
    task_sensor.cpp            shift-register read and debounce
    task_led.cpp               WS2812B control
    task_game.cpp              game logic, move detection, menus
    task_display.cpp           ST7789 rendering
    task_network.cpp           WiFi, captive portal, Lichess
    task_buttons.cpp           button debounce
    board_map.h                square-to-LED mapping
    PIN_MAP.md                 firmware to PCB pin map
    FILE_REFERENCE.md          per-file design notes
  pcb/
    ChessLink_Footprints.pretty/   custom KiCad footprints
    ChessLink_Symbols/             custom KiCad symbols
    Microcontroller Lab Summer 2026/
      ESP+Power_Only/          control and power board (KiCad + production files)
      Sensors+LEDs/            per-rank sensor and LED strip (KiCad + production files)
```

---

## Build and flash

Requirements: Arduino IDE 2.x and the **esp32 by Espressif** board package.

Libraries (Library Manager):

- GFX Library for Arduino (`Arduino_GFX_Library`)
- FastLED
- ArduinoJson 6.x

`WiFi`, `HTTPClient`, `WebServer`, `DNSServer`, `Preferences`, and `SPI` ship with the ESP32 core.

Steps:

1. Open `rtos/chesslink/chesslink.ino`. The IDE compiles every `.cpp` in the folder.
2. Board: **ESP32 Dev Module**.
3. Partition scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**. WiFi, TLS, and the HTTP client need the room.
4. Enable **PSRAM** if your module has it.
5. Upload. If it will not enter the bootloader, hold BOOT, tap EN, and release BOOT once "Connecting..." appears.

---

## First boot

1. With no saved WiFi, the board starts an access point called `ChessLink-Setup` and shows instructions on the LCD.
2. Join it from a phone. A setup page opens automatically, or go to `192.168.4.1`.
3. Pick your WiFi network, enter its password, and paste a Lichess API token. Create one at lichess.org under Preferences, API access tokens, with the `board:play` scope.
4. The board saves everything to flash, connects, and lands on the menu. You only do this once; **WiFi Setup** in the menu reopens it. A bad token is not validated at setup, so it only shows up as a failed seek or bot start.

The setup access point uses a fixed default password, defined in `chesslink.h`. Change it before leaving a board running anywhere untrusted.

---

## Known limitations

This is a working prototype, not a finished product.

- **No automated tests are committed.** The engine and move-detection logic were checked during development, but no test suite is in this repo.
- **No board-desync recovery.** If a piece is knocked over or misplaced mid-game, there is no "restore the board" state yet. If Lichess rejects a move, the failure is logged but not reverted.
- **One stored study game.** Famous Games contains only the Immortal Game.
- **Game-end detection is partial.** Local games detect checkmate, stalemate, and the 50-move rule. Threefold repetition and insufficient material are not detected on the device. Online games take the result from Lichess.
- **LED brightness is conservative.** It is set to 48 of 255 pending a thermal check of the full 64-LED load.
- **Piece magnets can interact.** Prototype pieces with several stacked magnets showed interference at roughly 50 to 60 mm separation. Using a single magnet per piece was being evaluated.
- **Some docs in `rtos/chesslink/` are stale.** `BUILD_AND_FLASH.md`, `INTEGRATION.md`, and `MERGE_NOTES.md` describe earlier iterations (older libraries, older pin assignments, files that no longer exist). The code, `PIN_MAP.md`, and this README are current.
- **Not implemented, but sketched in `IMPLEMENTATION_ROADMAP.md`:** resume a game after reboot, persistent settings, king-in-check highlight, draw offers and takebacks, puzzle mode, OTA updates.

---

## Engineering notes

- **A software bug that looked like a hardware fault.** One rank of the board kept misbehaving. I replaced that entire sensor strip before finding the cause was a mapping error in the LED and sensor index tables. I moved to explicit, editable lookup tables during bring-up so a single wrong rank is a one-line fix.
- **Shared-clock bring-up bug.** With four parallel segments sharing one clock, reading them one after another corrupted the later segments, because the shared clock had already shifted their data out. The fix was to sample all lines on the same clock edges, and later to move to a single 64-bit daisy chain.
- **Off-by-one on the first square.** The '165 presents square a1 on the data line right after the load pulse, before any clock edge. The SPI master misses it, which shifts the whole board by one square. `sr_read_all()` reads that first bit directly and shifts the rest into place.
- **Input-only GPIOs have no internal pull-ups.** The four buttons on GPIO 34, 35, 36, and 39 need external 10 k pull-ups, which are on the main board.
- **Identical-looking pieces on the LCD.** An early display bug drew every piece as the same white circle, so the board view was unreadable. Rendering the piece letter inside each circle, taken from the FEN, fixed it.

---

## Team

ChessLink was a group project by **Gabriel Cooper**, **Aaron Luebbert**, and **Nnamdi Onyejekwe**.

My part (Aaron): the PCB design in KiCad (schematics, custom footprints and symbols, layout, production files and ordering), the FreeRTOS firmware architecture and integration, and hardware bring-up and debugging. The WiFi captive-portal page and the menu and Stockfish-picker layout began as teammates' prototypes and were folded into this firmware.

Thanks to Jordie Bellar, a lab student, for hands-on help along the way.
