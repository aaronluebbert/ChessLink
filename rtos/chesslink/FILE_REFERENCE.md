# ChessLink firmware -- file reference

A per-file description of the ChessLink RTOS firmware, detailed enough to
reconstruct any file from scratch. It is an ESP32 (Arduino core) FreeRTOS app: a
physical chessboard with 64 hall sensors (read through an HC165 shift-register
chain), a WS2812B LED under each square, an ST7789 LCD, four buttons, and a
WiFi/Lichess link. It plays local games, online rated human games, and games
against Stockfish.

--------------------------------------------------------------------------------
## Architecture at a glance

Six FreeRTOS tasks talk only through queues -- no shared mutable globals across
tasks except a few file-static caches inside a single task. `task_game` is the
brain; every other task is an I/O edge.

```
 [task_Sensor] --BoardState--> [task_Game] --GameState--> [task_Display]
                                  |   ^  \--LedCmd------> [task_Led]
 [task_Buttons]--ButtonEvent-----/   |
                                     |  NetUpdate (clocks/players/status/result)
 [task_Network] <--NetCmd-- [task_Game]   ^
       |  \--OpponentMove--> [task_Game]  |
       \----------------------------------/
       ^--PlayerMove-- [task_Game]
```

Cores/priorities (from `chesslink.h`): sensor, LED, game, buttons on core 1;
network and display on core 0 (they share the core with WiFi/TCP system tasks).
Priorities high-to-low: sensor 5, LED 4, game 3, buttons 3, network 2, display 1.

Canonical square index used everywhere: `sq = rank*8 + file`, a1=0 .. h8=63
(file 0=a..7=h, rank 0=1st..7=8th). Board occupancy is a `uint64_t` bitmask,
bit `sq` set = a piece is on that square.

--------------------------------------------------------------------------------
## chesslink.ino
Arduino entry stub. Contains only a comment and `#include "chesslink.h"`. The
Arduino IDE compiles every `.cpp` in the folder; the real `setup()`/`loop()` live
in `main.cpp`. The sketch folder and this file must share the name `chesslink`.

--------------------------------------------------------------------------------
## main.cpp
Owns the queue handle definitions and `setup()`/`loop()`.

Defines (one instance each) all eight `QueueHandle_t` globals declared `extern`
in `chesslink.h`: `xQ_BoardState`, `xQ_LedCmd`, `xQ_GameState`, `xQ_PlayerMove`,
`xQ_OpponentMove`, `xQ_ButtonEvent`, `xQ_NetCmd`, `xQ_NetUpdate`.

`setup()`: `Serial.begin(115200)`; `chess_engine_init()` (builds attack tables
before any task runs); create the eight queues with the depths and item sizes
from `chesslink.h` (`xQ_NetCmd` carries `NetCmdMsg_t`, `xQ_NetUpdate` carries
`NetUpdate_t`, etc.); if any queue is null, print fatal and spin forever; then
`xTaskCreatePinnedToCore` the six tasks with the stack/priority/core macros.
Order: sensor, LED, game, buttons on core 1; display, network on core 0.

`loop()`: `vTaskDelay(portMAX_DELAY)` -- everything runs in tasks.

--------------------------------------------------------------------------------
## chesslink.h
The shared contract: pins, RTOS config, all cross-task data types, queue externs,
task prototypes, and the online/bot config tables. This is the most important
file to get right; everything else depends on it.

Pins (`#define`): shift registers on VSPI -- `SR_SCLK 18`, `SR_MISO 19`,
`SR_LOAD 5`. LEDs `LED_DATA_PIN 25` (WS2812B via a 74AHCT125 level shifter). LCD
on HSPI -- `LCD_MOSI 13`, `LCD_SCLK 14`, `LCD_CS 15`, `LCD_DC 2`, `LCD_RST -1`,
`LCD_BL 32`. Buttons (input-only pins, external 10k pull-ups, active-low):
`BTN_UP 36`, `BTN_DOWN 39`, `BTN_CONFIRM 34`, `BTN_CANCEL 35`.

`NUM_SQUARES 64`. Stack sizes (`STACK_SENSOR 4096` ... `STACK_NETWORK 8192`),
priorities (`PRI_*`), cores (`CORE_*`), queue depths (`Q_*_DEPTH`, mostly 4-8),
and timing (`SENSOR_SCAN_MS 20`, `LED_UPDATE_MS 16`, `BTN_POLL_MS 20`,
`BTN_DEBOUNCE_MS 50`, `DEBOUNCE_SCANS 3`).

Enums and structs (exact fields matter -- queues copy by value):
- `BoardState_t { uint64_t occupied; }` -- one sensor scan.
- `LedCmdType_t { LED_CMD_SET_SQUARE, LED_CMD_CLEAR, LED_CMD_PATTERN, LED_CMD_HILITE }`.
- `LedCmd_t { LedCmdType_t type; uint8_t square; uint8_t r,g,b; uint64_t mask; }`.
- `ButtonEvent_t { BTN_EVT_UP, BTN_EVT_DOWN, BTN_EVT_CONFIRM, BTN_EVT_CANCEL }`.
- `PromoState_t { PROMO_NONE, PROMO_SELECTING }`.
- `GameMode_t { GAME_MODE_IDLE, GAME_MODE_LOCAL, GAME_MODE_LICHESS }`.
- `UiScreen_t { UI_MENU, UI_ONLINE_CFG, UI_BOT_CFG, UI_GAME, UI_SETUP, UI_NOTICE, UI_CONFIRM, UI_GAMEOVER }`.
- Menu items: `MENU_ITEM_COUNT 4`, `MENU_ITEM_LOCAL/ONLINE/BOT/SETUP = 0..3`.
- Soft-AP: `WIFI_SETUP_AP_SSID "ChessLink-Setup"`, `WIFI_SETUP_AP_PASS "chesslink12"`.
- `TimePreset_t { uint16_t min; uint8_t inc; const char *label; }`; externs
  `ONLINE_TIME_PRESETS[]`/`ONLINE_TIME_COUNT` and `BOT_TIME_PRESETS[]`/`BOT_TIME_COUNT`
  (defined in task_game.cpp).
- Online config rows: `CFG_ROW_TIME 0`, `CFG_ROW_RATED 1`, `CFG_ROW_START 2`, `CFG_ROW_COUNT 3`.
- Bot config rows: `BOT_ROW_LEVEL 0`, `BOT_ROW_TIME 1`, `BOT_ROW_COLOR 2`, `BOT_ROW_START 3`, `BOT_ROW_COUNT 4`; `BOT_LEVEL_MIN 1`, `BOT_LEVEL_MAX 8`.
- `NetCmd_t { NET_CMD_START_ONLINE, NET_CMD_START_BOT, NET_CMD_RESIGN, NET_CMD_OPEN_SETUP }`.
- `NetCmdMsg_t { NetCmd_t type; uint16_t time_min; uint8_t inc_sec; bool rated; uint8_t level; uint8_t color; }` -- game->network command (color 0=white/1=black/2=random).
- `NetStatus_t { NET_STATUS_SETUP, NET_STATUS_ONLINE }`.
- `NetUpdate_t` -- network->game status/meta/clock/result push. Flags gate each
  block: `has_status`+`status`; `has_result`+`result_text[32]`;
  `has_meta`+`my_name[20]`/`opp_name[20]`/`my_rating`/`opp_rating`/`my_color`;
  `has_clocks`+`white_clock_ms`/`black_clock_ms`.
- `GameState_t` -- game->display snapshot: `mode`, `fen[92]`, `active_color`,
  `last_move[6]`, `status_msg[32]`, `ui_screen`, `menu_cursor`, config fields
  (`cfg_cursor`/`cfg_time_idx`/`cfg_rated`/`cfg_level`/`cfg_color`),
  `white_clock_ms`/`black_clock_ms`, player meta (`my_name`/`opp_name`/
  `my_rating`/`opp_rating`/`my_color`), `promo_state`, `promo_cursor`.
- `MoveEvent_t { uint8_t from_sq, to_sq; char uci[6]; uint32_t white_clock_ms, black_clock_ms; }` -- carried on `xQ_PlayerMove` and `xQ_OpponentMove` (clocks only meaningful on opponent events).

Queue externs and the six task prototypes. Also `void chess_engine_init();`.

Note: clocks are stored as absolute remaining ms; the server value already
includes increment, so increments are intentionally not tracked.

--------------------------------------------------------------------------------
## board_map.h
Two pure inline helpers mapping physical wiring to canonical squares. From the
validated hardware:
- `cl_led_index(sq)` -> WS2812B chain position. Chain order is LED 0 = h8, then
  h..a across each rank, ranks 8..1: `return (7 - (sq>>3))*8 + (7 - (sq&7));`.
- `cl_sensor_sq(readidx)` -> canonical square. The HC165 chain clocks out in
  canonical order a1,b1..h1,a2..h8, so this is the identity: `return readidx;`.

--------------------------------------------------------------------------------
## chess_engine.h / chess_engine.cpp
A self-contained bitboard move generator and rules engine. No Arduino/RTOS deps
beyond `<Arduino.h>` for `uint*` types, so it host-compiles and unit-tests.

Header defines: `BB` (uint64_t); file masks `FILE_A/B/G/H`; rank masks
`RANK_1..RANK_8` (all eight -- `RANK_3`/`RANK_6` must be here, they are used by
pawn double-push generation); `SQ/FILE_OF/RANK_OF/BB_SQ` macros; `PieceType
{ PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING, NO_PIECE }`; `Color { WHITE, BLACK,
NO_COLOR }`, `OTHER(c)`; castling bits `CASTLE_WK/WQ/BK/BQ`; `Move` (uint32_t)
with bit layout from/to/piece/capture/promo/ep/castle and the `MV_*` accessors +
`make_move(...)` builder; `MAX_MOVES 218`; and `Position` (per-color piece
bitboards `pieces[2][6]`, `occupied[2]`, `all`, `side`, `castling`, `ep_sq`,
`halfmove`, plus per-square `board[64]`/`color_at[64]` lookups).

Public API (all defined once in the .cpp): `pos_from_fen`, `pos_to_fen`,
`gen_legal_moves`, `gen_legal_moves_from`, `legal_destinations`, `in_check`,
`make_move_pos(pos,m,undo)` (undo is a full-state scratch save; callers that
don't unmake still must pass one), `unmake_move_pos`, `uci_to_move`, `move_to_uci`,
`chess_engine_init`.

.cpp sections in order: attack tables (`init_attack_tables`, ray/rook/bishop/queen
attacks); `sq_attacked_by`/`in_check`; FEN in/out; `make_move_pos`/`unmake_move_pos`
(make handles captures, en passant removal, promotion, castling rook move,
castling-right updates, ep-target set, halfmove clock, and rebuilds `pos->all`);
pseudo-legal generation `gen_pseudo`; legal filtering (make/`in_check`/unmake) in
`gen_legal_moves`, `gen_legal_moves_from`, `legal_destinations`; UCI helpers;
`chess_engine_init` (calls `init_attack_tables`). Occupancy after any legal move
is authoritative in `Position::all` -- the game task relies on this.

--------------------------------------------------------------------------------
## task_sensor.cpp  (task_SensorScan)
Reads the 64-sensor HC165 chain and posts a debounced occupancy bitmask.

Bit-bangs the chain (no SPI peripheral): set `SR_LOAD` output/idle-high,
`SR_SCLK` output/idle-low, `SR_MISO` `INPUT_PULLUP`. `sr_read_all()` pulses
`SR_LOAD` low->high to latch, then for each of 64 bits reads `SR_MISO` (active-low:
LOW = piece present) into `occupied` at `cl_sensor_sq(bit)`, pulsing `SR_SCLK`
between bits (5us delays). Loop: read; require `DEBOUNCE_SCANS` identical reads in
a row before accepting a change; on an accepted change build `BoardState_t {occupied}`
and `xQueueSend(xQ_BoardState)`, dropping the oldest queued item if full;
`vTaskDelayUntil` every `SENSOR_SCAN_MS`.

--------------------------------------------------------------------------------
## task_led.cpp  (task_LedControl)
Drives the WS2812B strip via FastLED. `CRGB leds[NUM_SQUARES]`; `sq_to_led(sq)`
returns `cl_led_index(sq)`. `apply_led_cmd` handles: `SET_SQUARE` (one square to
rgb), `CLEAR` (`FastLED.clear()`), `PATTERN` (masked squares full rgb, others at
~10% for context), `HILITE` (masked squares rgb, all others off -- used by the
setup-board guide). Task: `addLeds<WS2812B, LED_DATA_PIN, GRB>`,
`setBrightness(48)`, then a loop that drains `xQ_LedCmd`, caps `FastLED.show()` to
~60fps (`LED_UPDATE_MS`), and blocks up to one frame for the next command.

--------------------------------------------------------------------------------
## task_buttons.cpp  (task_Buttons)
Debounces four active-low buttons. Arrays `pins[4] = {BTN_UP,BTN_DOWN,BTN_CONFIRM,
BTN_CANCEL}` and `evts[4] = {BTN_EVT_UP,...}`. Per button tracks last raw / debounced
state / `stable_since`. Init each pin `INPUT` and seed state so nothing fires at
boot. Loop every `BTN_POLL_MS`: on a raw change reset the stable timer; once stable
for `BTN_DEBOUNCE_MS` and the debounced state actually changed, fire the event only
on the falling edge (HIGH->LOW press) via `xQueueSend(xQ_ButtonEvent)`.

--------------------------------------------------------------------------------
## task_game.cpp  (task_GameLogic)  -- the brain
Owns all game/UI state in a local `GameCtx_t` and publishes `GameState_t`. Never
touches hardware directly; talks through queues.

Preset tables (definitions of the header externs): `ONLINE_TIME_PRESETS[]`
(1+0 .. 15+10, default index 4 = 5+3) and `BOT_TIME_PRESETS[]` (Untimed, 3+2, 5+3,
10+0, 15+10; default index 2). `CAND_MAX 40`.

`GameCtx_t` holds: `Position pos`; `mode`; `status_msg`/`last_move`; UI flags
`in_menu`/`in_setup`/`in_notice`/`in_online_cfg`/`in_bot_cfg`/`confirm_exit`;
`menu_cursor`; config `cfg_cursor`/`cfg_time_idx`/`cfg_rated`/`cfg_level`/`cfg_color`;
clocks `white_clock_ms`/`black_clock_ms`; player meta `my_name`/`opp_name`/
`my_rating`/`opp_rating`/`my_color`; `last_occupied` (cached sensor bitmask);
move-detection `phase`, `lifted_sq`, `promo_to_sq`, `legal_dests`, candidate arrays
`cand_moves[CAND_MAX]`/`cand_occ[CAND_MAX]`/`cand_count`, `promo_cursor`.

`MovePhase_t { PHASE_SETUP_BOARD, PHASE_IDLE, PHASE_PIECE_LIFTED, PHASE_PROMO_SELECT,
PHASE_OPP_SYNC, PHASE_GAME_OVER }`.

`publish_game_state(ctx)`: copies ctx into a `GameState_t`, computes `ui_screen`
by priority: setup > confirm_exit > menu > online_cfg > bot_cfg > game_over >
notice > else game; `xQueueOverwrite(xQ_GameState)`.

Mode entry: `enter_menu` (resets all UI flags, mode IDLE); `check_setup(ctx,occ)`
(if `occ == pos.all` go PHASE_IDLE + "your move", else `LED_CMD_HILITE` the
squares still missing a piece = `pos.all & ~occ`); `start_local` (load startpos,
PHASE_SETUP_BOARD, run check against `last_occupied`); `open_online_cfg`/`open_bot_cfg`
(reset the shared `cfg_time_idx` to that mode's default); `start_lichess_game`
(shared: notice "Seeking.../Starting bot...", mode LICHESS, send the `NetCmdMsg`);
`start_online`/`start_bot` (build the `NetCmdMsg` from the config and call it);
`open_setup` (send `NET_CMD_OPEN_SETUP`, show setup screen); `leave_game`
(local -> enter_menu; lichess -> notice + `NET_CMD_RESIGN`).

Game over: `end_game(ctx,text)` sets `status_msg`, clears notice, PHASE_GAME_OVER,
publishes. `check_local_end` (local only, after each move): no legal reply ->
checkmate (names winner) or stalemate; else `halfmove>=100` -> 50-move draw.

Move detection by expected occupancy: `occ_after_move(pos,m)` = `pos.all` after a
scratch make. `begin_move(ctx,from)` gathers `gen_legal_moves_from` and precomputes
each candidate's resulting occupancy. `process_board_change(ctx,bs)` early-returns
for promo/game-over/notice/confirm; `PHASE_SETUP_BOARD` -> `check_setup`;
`PHASE_OPP_SYNC` -> wait until `curr == pos.all` then resume; `PHASE_IDLE` -> a
lifted square of the side to move starts a move (`begin_move`); `PHASE_PIECE_LIFTED`
-> if `curr` equals a candidate occupancy, commit it (or open the promo picker if
`MV_IS_PROMO`), if `curr == prev` cancel, else if a single own piece landed on a
non-legal square flash red. This is what makes captures/castling/en passant work.

`commit_move(ctx,chosen)`: make the move on `pos`, set last move/status, green
flash, if LICHESS send `MoveEvent_t` on `xQ_PlayerMove`, publish, brief delay,
clear LEDs, then `check_local_end` for local games.

`promo_hint`/`commit_promo` support the promotion picker.

`handle_button(ctx,evt)` routes by state: setup/notice ignore; `confirm_exit`
(CONFIRM=leave, CANCEL=resume); game-over (any button -> menu); menu (UP/DOWN
cursor, CONFIRM opens local/online-cfg/bot-cfg/setup); online cfg (UP/DOWN row,
CONFIRM cycles time/toggles rated/starts, CANCEL back); bot cfg (level/time/color
rows + start); promo picker (UP/DOWN cycle piece, CONFIRM commit); otherwise
in-game CANCEL opens the leave prompt.

`apply_opponent_move(ctx,mv)`: make the move on `pos` (now ahead of the board),
store clocks, light from(blue)/to(cyan), enter `PHASE_OPP_SYNC` (player must
replay it). `apply_net_update(ctx,u)`: `has_result` -> `end_game`; `has_status`
SETUP/ONLINE -> setup screen / return to menu; on the first meta/clocks after a
seek, clear the notice and enter `PHASE_SETUP_BOARD`; copy meta/clocks and publish.

`task_GameLogic`: init ctx (default names, config defaults), `enter_menu`. Loop:
read a button -> `handle_button`; always read `xQ_BoardState` and cache
`last_occupied` (even in menu) and, when not in menu, `process_board_change`;
skip the rest while in menu; take an opponent move only when `PHASE_IDLE` and not
`confirm_exit`; always drain `xQ_NetUpdate`.

--------------------------------------------------------------------------------
## task_display.cpp  (task_LcdDisplay)
Renders `GameState_t` on the ST7789 (170x320 portrait, USB-C at top,
`setRotation(0)`). HSPI via `SPIClass hspi(HSPI)`; palette `#define`s (C_BG,
C_ACCENT, etc.). Board layout constants (header/turn/move/status/board rects).

Helpers: `fen_to_squares` (FEN piece-placement -> 64-char array);
`draw_board_square`/`draw_mini_board` (8x8 with piece letters in circles);
`print_centered(s,y,size,color)` (width = len*6*size); `fmt_clock(ms)` -> MM:SS
(0 -> "--:--"); `fmt_rating` (0 -> "----").

Screen renderers: `render_game_state` (header/turn/last-move/status/mini-board);
`render_match_static` + `draw_match_clocks` (live match: opponent name+elo top,
opponent clock, my clock, my name+elo bottom; running side green; clocks count
down locally and resync on updates -- opponent up top, me at bottom, mapped via
`my_color`); `render_menu` (four items); `render_online_cfg`/`render_bot_cfg`
(config rows with a highlighted cursor and right-aligned values);
`render_setup` (soft-AP join instructions); `render_notice(gs,terminal)` (centered
message; terminal adds "press a button for the menu", transient adds "please
wait..."); `render_confirm` ("Leave game? OK=yes CANCEL=no"); promotion picker
(2x2 tiles, UP/DOWN choose, CONFIRM confirm) with `redraw_promo_tiles`.

Task: init hspi + `tft.init(170,320,SPI_MODE2)`, backlight on, splash, then a
loop with a `shown` enum to avoid needless full redraws. On each `GameState`
select by `ui_screen`: SETUP, CONFIRM, NOTICE, GAMEOVER, MENU, ONLINE_CFG,
BOT_CFG, or (in a game) PROMO if `promo_state==SELECTING`, MATCH if either clock
is nonzero, else BOARD. The match screen also ticks locally: on a `GameState`
timeout it decrements the running side from the last server value and redraws
only the clock band.

--------------------------------------------------------------------------------
## task_network.cpp  (task_Network)
Owns WiFi, NVS credentials, and all Lichess Board-API traffic. Uses `WiFi`,
`HTTPClient`, `ArduinoJson`, `Preferences`, `WebServer`, `DNSServer`.

Config/token: `LICHESS_BASE`. `lichess_token[96]` loaded from NVS namespace
"wifi" key "token" by `load_lichess_token()`; `add_auth(http)` builds the
`Bearer` header at runtime (nothing secret is compiled in). `net_report(status)`
and `net_report_result(text)` push `NetUpdate_t`s; `s_game_ended` breaks the
stream loop.

Captive portal: `SETUP_PAGE` (PROGMEM HTML form for ssid/pass/token),
`setup_handle_root`/`setup_handle_save` (writes NVS namespace "wifi"),
`run_setup_portal()` (soft-AP `WIFI_SETUP_AP_SSID`, `DNSServer` wildcard,
`WebServer`, reports `NET_STATUS_SETUP`, blocks until the form is submitted),
`ensure_connected()` (try saved creds via `wifi_connect`, else run the portal;
on success reload token and report `NET_STATUS_ONLINE`).

Lichess: `lichess_post_move` (POST a move UCI); `lichess_game_action`/`lichess_resign`
(POST resign, fall back to abort); `count_moves`; `handle_stream_line` (parses
`gameFull` -> player names/ratings + our color + initial clocks, tagging an
`aiLevel` player as "Stockfish", pushes a meta+clock `NetUpdate`; `gameState` ->
terminal status ends the game with a formatted result, else updates clocks and,
if the opponent just moved, sends a `MoveEvent` on `xQ_OpponentMove`, otherwise
re-syncs clocks); `lichess_stream_game` (opens the board game stream, each loop
checks `xQ_NetCmd` for `NET_CMD_RESIGN`, flushes queued player moves via
`lichess_post_move`, and feeds lines to `handle_stream_line` until game end or
timeout); `lichess_seek_and_get_id` (POST board/seek with the online time control,
reads `gameStart` for id + our color); `lichess_challenge_ai` (POST challenge/ai
with level/color and, if timed, `clock.limit` in seconds + `clock.increment`;
reads the id and our color from the response).

`task_Network`: `ensure_connected()`, then loop reading `xQ_NetCmd`:
`START_ONLINE` (set the seek params, seek, stream, report errors "No token"/"Seek
failed"/"Connection lost"); `START_BOT` (challenge Stockfish, stream, "Bot start
failed"); `OPEN_SETUP` (run the portal, reconnect). Reconnect on a dropped link.

--------------------------------------------------------------------------------
## Other docs in this folder
- `IMPLEMENTATION_ROADMAP.md` -- what's done and what's left.
- `PIN_MAP.md` -- firmware<->PCB pin map (authoritative for wiring).
- `BUILD_AND_FLASH.md` -- Arduino IDE setup and flashing (note: its file list and
  the LED pin are out of date; use this reference and `PIN_MAP.md` for current
  truth).
- `INTEGRATION.md` / `MERGE_NOTES.md` -- historical notes from an earlier merge
  that referenced files since removed; kept only as history, not current.
