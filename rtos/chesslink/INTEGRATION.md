# ChessLink -- merged firmware

Your groupmate's three branches were three iterations of the same thing: a
single-file, blocking, `loop()`-based sketch for WiFi + Lichess login and an
on-screen menu. This folds the best of them into your FreeRTOS architecture so
everything runs as tasks on your queues -- one buildable Arduino sketch.

## What each branch contributed

| branch | what it was | what got kept |
|---|---|---|
| WIFI-and-API | earliest: serial WiFi/token entry + account check, no persistence | the Lichess account-check flow |
| Menu_Connectivity | added NVS credential saving + a hand-coded menu (boolean flags) | NVS persistence via `Preferences` |
| AI-Optimized-GUI-State-Machine | clean rewrite of the menu as a `ScreenDef` table | the menu model (now `ui_menu.*`) |

The three sketches were ~90% duplicate. AI-Optimized superseded the other two,
so it's the base for the menu; NVS came from Menu_Connectivity; the account
check is common to all.

## The problem it solved

Those sketches can't just be dropped in: they own the LCD on the default SPI
bus (SCLK18 -- which collides with your shift-register VSPI clock), block on
`readSerialLine()`, and `delay()` everywhere. Your firmware already has a
`task_Network` and a `task_LcdDisplay` on queues. So the connectivity and menu
became responsibilities *inside* those tasks, not a second program.

## New / changed files

| file | change |
|---|---|
| `ui_menu.h` / `ui_menu.cpp` | **new** -- the menu state machine, hardware-free and host-tested |
| `task_input.cpp` | **new** -- owns Serial + the two buttons, emits `UiEvent_t` |
| `task_display.cpp` | **rewritten** -- UI state machine: renders setup / menu / board |
| `task_network.cpp` | **rewritten** -- NVS creds, event-driven setup, Lichess, status reporting |
| `task_game.cpp` | takes a `GameCmd` from the menu to start/stop a game |
| `chesslink.h` | new UI/connectivity types, queues, `task_Input`, `g_conn` |
| `main.cpp` | creates the new queues, spawns `task_Input` |
| `task_sensor.cpp`, `task_led.cpp`, chess engine, tracker | unchanged |

## How it fits together

Three new queues wire the UI in without disturbing the game path:

```
 buttons/serial ->[task_Input]--UiEvent-->[task_Display]--NetCmd-->[task_Network]
                                              |  ^                       |
                                     GameCmd  |  | UiStatus / g_conn     | WiFi + Lichess
                                              v  |                       |
                                          [task_GameLogic] <--OpponentMove--
                                              |  ^
                                     GameState|  | BoardState
                                              v  |
                                          [task_Display]   [task_Sensor]
```

- **task_Input** is the only reader of Serial and the buttons. It classifies
  each line/press into a `UiEvent_t` (nav `w/s/e/q`, commands `wifi/clear/help`,
  or typed setup text) and posts to `xQ_UiEvent`. Buttons: MODE short = down,
  MODE long = back, CONFIRM = select.
- **task_Display** is the UI brain and the only LCD owner. It runs three modes
  (setup / menu / board), renders the menu from `ui_menu`, the board from
  `GameState_t`, and setup screens from `UiStatus_t`. On a menu leaf it sends a
  `GameCmd` to the game task; `wifi`/typed text it forwards to the network task.
- **task_Network** owns WiFi + Lichess. Credentials live in NVS. Setup is
  event-driven (SSID -> password -> token arrive as `NET_CMD_TEXT`), so it never
  blocks on Serial. Progress is reported as `UiStatus` messages; live
  username/SSID go in `g_conn` for the status bar.
- **task_GameLogic** starts `IDLE` and waits for a `GameCmd` from the menu, then
  runs the presence-only tracker exactly as before.

## Menu -> game mapping

| menu path | GameCmd |
|---|---|
| Local > Local Match | `GAME_MODE_LOCAL` |
| Local > Famous Games > MAGvsGABRIEL | `GAME_MODE_ANALYSIS` (TODO: load a stored FEN) |
| Online > Play Bot / Player / Ranked | `GAME_MODE_LICHESS` (detail = bot/player/ranked) |

## Decisions worth reviewing

- **Pins:** kept your dual-SPI map (LCD on HSPI 14/13, shift registers on VSPI
  18/19). The groupmate's LCD pins (SCLK18/MOSI23) are dropped -- they clashed
  with the sensor bus.
- **Buttons on GPIO34/35** are input-only with no internal pull-ups; wire
  external pull-ups (active-low assumed in `task_input.cpp`).
- **Per-field y/n confirm** from the groupmate's setup was dropped for a simpler
  linear flow. Re-run `wifi` if a value is wrong. Easy to add back as extra
  setup states.
- **Two open TODOs carried over** from the network code: the seek returns a stub
  game id (needs parsing from the seek response), and the game stream still
  echoes your own moves as opponent moves (needs color tracking). Both flagged
  in `task_network.cpp`.

## Verification

- `ui_menu` navigation unit-tested on host (all transitions + leaves).
- Engine + tracker perft/tracker tests still pass unchanged.
- Every task compiled against the real `chesslink.h` with mock
  Arduino/FreeRTOS/WiFi/HTTPClient/ArduinoJson/Preferences/Adafruit/FastLED/SPI
  headers, and the whole firmware linked -- so all types, struct fields and
  queue signatures line up across the merge.

Build in the Arduino IDE: open `chesslink.ino`, it compiles every `.cpp` in the
folder. Requires the ESP32 core plus Adafruit GFX/ST7789, FastLED, ArduinoJson.

## Update: real Lichess game handling + famous-game replay

Three fixes landed on top of the merge.

**Real game id + color.** The stub `stubgameid1` is gone. `Play Bot` uses
`POST /api/challenge/ai`, which returns the real game id in its response (we
request white, so our color is known). `Play Player` / `Play Ranked` seek a game
and then read the real id + color from the `gameStart` event on
`GET /api/stream/event`. The pure parsing/color logic lives in `lichess.h/.cpp`
so it is host-tested independently of the HTTP layer.

**No more echoing your own moves.** The game stream sends the full move list from
the start on every update. `lichess_next_opponent_move()` walks that list with a
persistent `applied` cursor, skips the plies that are yours (by color parity),
and forwards only new opponent moves to the game task. Color is taken from
`gameStart` and re-confirmed from the `gameFull` player ids. Unit-tested for
both colors and incremental delivery.

**Famous-game replay.** `famous_games.h/.cpp` holds a table of games as UCI move
lists (currently Morphy's 1858 Opera Game under the `MAGvsGABRIEL` label). Choosing
`Local > Famous Games` starts a guided replay: the game task applies each stored
move to the committed position and lights it on the board, exactly like an
opponent move, so the player reconstructs the whole game move by move. Every move
in the table is verified legal by replaying it through the engine, ending in the
real checkmate.

### New files

| file | role |
|---|---|
| `lichess.h` / `lichess.cpp` | pure board-api parsing: move extraction, opponent-move iterator, color-from-ids. host-tested |
| `famous_games.h` / `famous_games.cpp` | famous-game table (UCI move lists), legality-checked |

### Still open

- Seek-based `Play Player` / `Play Ranked`: `POST /api/board/seek` holds its
  connection open, so on a single task the seek + event-stream read is
  best-effort. A production build wants a non-blocking client to do both at once.
  `Play Bot` (challenge/ai) is fully clean since the id comes back immediately.
- Famous games: one game in the table; add rows to `FAMOUS_GAMES` (a real submenu
  picker is a small follow-up).

## Update 2: robust seek + more famous games

**Non-blocking seek.** `POST /api/board/seek` holds its connection open until a
game is found, so it can't share a single blocking `HTTPClient`. The streaming
endpoints now use a small raw-socket reader, `NdjsonStream`, built on
`WiFiClientSecure`: it sends the request, skips the response headers once, then
reads the body line-by-line without ever blocking (`ndjson_feed`, host-tested).
`seek_and_wait()` opens the event stream and issues the seek on two separate TLS
sockets at once, then reads the real game id + color from the `gameStart` event.
`Play Bot` still uses `challenge/ai` (id comes straight back). Heads up: two live
TLS sockets during a seek is real memory pressure on the ESP32 -- use a "Huge
APP" partition and watch free heap (see BUILD_AND_FLASH.md).

**More famous games.** `FAMOUS_GAMES` now has four, each verified legal and ending
in a real checkmate: the Opera Game, Scholar's Mate, Legal's Mate, and the 1851
Immortal Game (45 plies). Add more by appending rows; a `ply_count` and
`ends_mate` flag come with each. A proper submenu picker (instead of always game 0)
is a small follow-up in `ui_menu` + `task_display`.

## Update 3: phone-only WiFi setup (captive portal)

Credential entry no longer touches the serial monitor. On boot with no saved
WiFi (or when a connection fails), the network task brings up a soft-AP
`ChessLink-Setup` and a captive portal: a `DNSServer` answers every lookup with
the board's IP so the setup page auto-opens on the phone, and a `WebServer`
serves a mobile form for WiFi SSID + password + Lichess token. On submit the
values are saved to NVS and the board connects. `WebServer` and `DNSServer` are
part of the ESP32 core, so no new libraries.

A **WiFi Setup** entry was added to the main menu (`ui_menu`), so the portal can
be re-opened later with the buttons alone -- fully self-contained, phone + board
only. The old serial `wifi`/`clear` commands and the `NET_CMD_TEXT` path are now
vestigial (harmless, unused).

Note: this replaces the serial setup flow the groupmate's branches used; their
captive-portal HTML was not present in the three zips provided, so this is a
fresh implementation.
