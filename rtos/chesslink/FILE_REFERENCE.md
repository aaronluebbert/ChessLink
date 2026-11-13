# ChessLink firmware file reference

A guide to each source file in `rtos/chesslink/`. The firmware is an ESP32 (Arduino core) FreeRTOS application for a physical chessboard with 64 Hall-effect sensors read through a shift-register chain, a WS2812B LED under each square, an ST7789 LCD, four buttons, and a WiFi link to Lichess. It plays local games, online games against humans, games against Stockfish, and a step-through study mode for famous games.

Related docs: [`PIN_MAP.md`](PIN_MAP.md) for wiring, [`BUILD_AND_FLASH.md`](BUILD_AND_FLASH.md) for building.

---

## Architecture

Six FreeRTOS tasks communicate only through queues. `task_game` is the brain, and every other task is an I/O edge.

```
 SensorScan --BoardState--> GameLogic --GameState--> LcdDisplay
                              |  ^  \---LedCmd-----> LedControl
 Buttons ---ButtonEvent------/   |
                                 |  NetUpdate (clocks, players, status, result, server position)
 Network <---NetCmd--- GameLogic ^
    |  \--OpponentMove--> GameLogic
    ^----PlayerMove------ GameLogic
```

| Task | Core | Priority | Stack |
|---|---|---|---|
| SensorScan | 1 | 5 | 4096 |
| LedControl | 1 | 4 | 4096 |
| GameLogic | 1 | 3 | 8192 |
| Buttons | 1 | 3 | 2048 |
| Network | 0 | 2 | 16384 |
| LcdDisplay | 0 | 1 | 8192 |

Sensor, LED, game, and button tasks run on core 1. Network and display run on core 0, which also hosts the ESP32 WiFi and TCP system tasks.

Canonical square index, used everywhere: `sq = rank * 8 + file`, with a1 = 0 and h8 = 63. Board occupancy is a `uint64_t` bitmask where bit `sq` set means a piece is on that square.

---

## chesslink.ino

Arduino entry stub. It includes `chesslink.h` and nothing else. The real `setup()` and `loop()` are in `main.cpp`.

## main.cpp

Defines the eight queue handles and the shared SPI mutex, then starts everything.

`setup()` starts the serial port at 115200, calls `chess_engine_init()` to build the attack tables, creates the eight queues with the depths and item sizes from `chesslink.h`, halts with a message if any allocation fails, and pins the six tasks to their cores. `loop()` just sleeps, since all work happens in tasks.

Queues: `xQ_BoardState`, `xQ_LedCmd`, `xQ_GameState`, `xQ_PlayerMove`, `xQ_OpponentMove`, `xQ_ButtonEvent`, `xQ_NetCmd`, `xQ_NetUpdate`. `xQ_GameState` has depth 1 and is written with `xQueueOverwrite`, so the display always sees the latest state.

## chesslink.h

The shared contract: pins, task configuration, cross-task types, queue externs, and the preset tables.

- **Pins and timing.** See `PIN_MAP.md`. Sensor scan every 20 ms, LED refresh every 16 ms, button poll every 20 ms with a 50 ms debounce, and 3 identical sensor scans required to accept a change.
- **Modes.** `GameMode_t`: idle, local, lichess (online and bot), and replay (study mode).
- **Screens.** `UiScreen_t`: menu, online config, bot config, local config, famous games list, game, WiFi setup, notice, leave-game confirm, game over.
- **Menu.** Five items in order: Local Game, Online Game, Play Bot, Famous Games, WiFi Setup.
- **Config rows.** Online: time, rated, start. Bot: level (1 to 8), time, color, start. Local: time, start.
- **Presets.** `TimePreset_t` tables for online (1+0 to 15+10), bot (untimed to 15+10), and local (untimed to 30+0).
- **Famous games.** `FamousGame_t` holds a short name, a UCI move list, a move count, and a one-line players description.
- **Network messages.** `NetCmd_t`: start online, start bot, resign, open setup, import. `NetCmdMsg_t` carries the game parameters. `NetStatus_t`: setup, connecting, online. `NetUpdate_t` carries status, result text, player names and ratings, clocks, and an authoritative position (FEN) with the last move, each gated by a `has_*` flag.
- **State snapshots.** `GameState_t` is what the display renders: mode, FEN, side to move, last move, status text, current screen, menu and config cursors, clocks, player info, and promotion picker state. `MoveEvent_t` carries a move (and clocks, on opponent events) between the game and network tasks.
- **Setup access point.** `WIFI_SETUP_AP_SSID` and `WIFI_SETUP_AP_PASS`.

## board_map.h

`cl_led_index(sq)` maps a canonical square to its position in the WS2812B chain. LED 0 is h8, and the chain runs h to a across each rank, ranks 8 down to 1: `(7 - rank) * 8 + (7 - file)`. The sensor read order is handled inside `task_sensor.cpp`.

---

## chess_engine.h and chess_engine.cpp

A self-contained bitboard rules engine with no RTOS dependencies.

- **Position.** Per-color piece bitboards, occupancy, side to move, castling rights, en passant square, halfmove clock, and per-square lookup arrays.
- **Move encoding.** A `Move` packs from, to, piece, capture, promotion, en passant, and castling into 32 bits.
- **API.** `pos_from_fen`, `pos_to_fen`, `gen_legal_moves`, `gen_legal_moves_from`, `legal_destinations`, `in_check`, `make_move_pos`, `unmake_move_pos`, `uci_to_move`, `move_to_uci`, `move_to_san`, and `chess_engine_init`.
- **Generation.** Pseudo-legal moves are generated per piece, then filtered by making each move and checking whether the mover's king is attacked.
- **Make move.** Handles captures, en passant removal, promotion, the castling rook move, castling-right updates, the en passant target, and the halfmove clock, and rebuilds `pos->all`.

`move_to_san` is used to build the movetext when a finished game is sent to Lichess for analysis.

---

## task_sensor.cpp

Reads the 64-sensor shift-register chain and publishes a debounced occupancy bitmask.

A scan pulses `SR_LOAD` to latch the sensors, reads the first bit (a1) directly from the data line, then clocks the remaining bits in over VSPI at 1 MHz and shifts them into place. Sensors are active-low. The read holds `xSPI18Mutex` so an LCD write cannot interleave with it. A change must appear in 3 consecutive scans before it is sent on `xQ_BoardState`, and if the queue is full the oldest entry is dropped.

## task_led.cpp

Drives the WS2812B strip with FastLED. It handles four commands: set one square to a color, clear, pattern (masked squares at full color and the rest dimmed), and highlight (masked squares lit, all others off). It drains pending commands before each refresh and caps `FastLED.show()` at about 60 fps. Brightness is set to 48 out of 255.

## task_buttons.cpp

Debounces four active-low buttons (up, down, confirm, cancel) and posts a `ButtonEvent_t` on each press, on the falling edge, once the level has been stable for the debounce time.

---

## task_game.cpp

Owns all game and UI state in one context struct and publishes `GameState_t` snapshots. It never touches hardware directly.

**Phases.** `PHASE_SETUP_BOARD` (waiting for the start position), `PHASE_IDLE`, `PHASE_PIECE_LIFTED`, `PHASE_PROMO_SELECT`, `PHASE_OPP_SYNC` (waiting for the player to replay an opponent move), `PHASE_REPLAY` (study mode), and `PHASE_GAME_OVER`.

**Start position check.** Before every game, squares that should hold a piece but do not are lit green. Play starts when the sensors match the starting occupancy.

**Move detection by expected occupancy.** When a piece of the side to move is lifted, `begin_move` generates the legal moves from that square, lights the destinations green and the source yellow, and precomputes the board occupancy each candidate move would produce. As the sensors change, the move is committed when the occupancy matches one candidate. Captures, castling, en passant, and promotion all fall out of this, because the whole board is compared. Pieces placed on squares that do not belong to the move are flagged red until corrected. Promotion opens a picker on the LCD.

**Board error highlighting.** If a piece is moved out of turn, dropped on the wrong square, or disturbed while waiting for an online opponent, the offending squares light red, along with where the piece should go back, and the status line explains what to fix.

**Online play.** Player moves are sent to the network task. Opponent moves arrive on `xQ_OpponentMove`, are applied to the position, lit blue (from) and cyan (to), and the game waits in `PHASE_OPP_SYNC` until the physical board matches. The server's position, rebuilt into a FEN by the network task, is adopted as truth through `reconcile_to_server`, so the on-device state cannot drift from the game. Syncs that arrive while a piece is lifted or during setup are deferred and applied afterward.

**Game end.** Local games detect checkmate, stalemate, the 50-move rule, and running out of time on timed games. Online games take the result from Lichess. A result screen shows the outcome, and any button returns to the menu. Finished games can be sent to Lichess for analysis (the game task builds the SAN movetext and stages it with `net_stage_import`).

**Leave game.** Cancel mid-game opens a confirm prompt. Confirming resigns an online or bot game and ends a local game.

**Study mode.** `start_replay` loads a game from `FAMOUS_GAMES`, runs the normal board setup, then presents one scripted move at a time. The move is shown on the LEDs (blue and cyan) and the game waits until the board reaches the expected occupancy. Confirm toggles the hint, and cancel leaves. Wrongly disturbed squares light red.

**Tables defined here.** The online, bot, and local time presets, and the famous games list (Immortal, Evergreen, Opera Game, Century, Deep Blue).

Main loop: read a button, read the board state (always cached, even in the menu), take opponent moves only when the board is idle, always drain network updates, and check the local clock.

---

## task_display.cpp

Renders `GameState_t` on the 170 by 320 ST7789. It uses the `Arduino_GFX_Library` (`Arduino_ESP32SPI` bus and `Arduino_ST7789` panel) on the shared VSPI bus, rotated 180 degrees so the USB-C port is at the top. Every render holds `xSPI18Mutex`. The backlight is switched on before the panel is initialized.

Screens: main menu, online config, bot config, local config, famous games list (name plus players line), WiFi setup instructions, transient notice (for example "Seeking opponent..."), leave-game confirm, game over, board view (mini board with piece letters in circles), live match view (opponent name and rating, both clocks, my name and rating), and the promotion picker.

`render_frame` picks the screen from `ui_screen`, avoids redrawing static screens that are already shown, and while a match is showing it counts the running clock down locally between server updates.

---

## task_network.cpp

Owns WiFi, stored credentials, and all Lichess traffic.

- **Credentials.** SSID, password, and token live in NVS (`Preferences`, namespace `wifi`). Nothing secret is compiled in. The token is read at request time and sent as a bearer header.
- **Setup portal.** With no saved WiFi, or on demand, the task starts the `ChessLink-Setup` access point with a wildcard DNS server and a web page that takes the SSID, password, and token, then connects and reports its status to the game task.
- **Lichess calls.** Seek (`POST /api/board/seek`), stream a game (`GET /api/board/game/stream/{id}`), send a move, resign or abort, challenge Stockfish (`POST /api/challenge/ai`), and import a finished game (`POST /api/import`).
- **Stream handling.** The first stream message gives player names, ratings, our color, and clocks, and tags a Stockfish opponent by name. Each later message carries the full move list. The task rebuilds the position from it with the engine, sends it up as an authoritative FEN with the last move, and forwards a move event when the opponent has just moved. Terminal statuses become a formatted result string from the player's point of view.
- **Commands.** Start online, start bot, resign (read inside the stream loop), open setup, and import. Errors surface on the LCD as "No token", "Seek failed", "Connection lost", or "Bot start failed".
