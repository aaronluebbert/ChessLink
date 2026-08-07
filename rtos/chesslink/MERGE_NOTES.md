# Merge notes: groupmate branches -> ChessLink firmware

Two branches came in. Here's exactly what was taken from each and why.

## ChessLink-Latest (CONNECT_ITALL) -- his WiFi + UI work

His branch was a single-file blocking sketch: a polished captive portal, a
data-driven menu, a Settings screen, and a Stockfish level/time/color picker --
but **no board play** (no sensor tracking, no move engine, no board rendering;
it just started a Lichess game and told you to watch on lichess.org).

We kept our working RTOS firmware (which actually plays chess on the board) and
folded his WiFi/UI work into it.

**Taken from his branch (preserved):**
- **His HTML captive portal** -- the styled amber card with the crown, network
  datalist, password + token fields. Copied verbatim into `task_network.cpp`
  (`portal_handle_root`). His open AP name `ChessLink-Setup` kept.
- **His menu layout** -- Local / Online / Settings, with Settings > Network /
  Delete data, and Online > Play Stockfish / Player / Ranked. Now in `ui_menu`.
- **His Stockfish picker** -- level (1-8), time preset, color, with the edit-mode
  UX. Re-implemented as an event-driven screen in `task_display` (`UI_STOCKFISH`)
  since our display is a task, not a blocking loop. Presets live in `sf_config.*`.
- **His configurable AI challenge** -- `challenge_ai()` now sends the picked
  level / clock / color instead of a hardcoded level 3.

**Where his had bugs / we kept ours instead:**
- **Account check:** his `checkAccount()` parsed the whole `/api/account` reply
  into a 2 KB buffer, which overflows (the response carries all rating perfs) and
  fails login. We kept our fix: HTTP 200 = valid token, username via a small
  filter.
- **Board play:** his online flow ends at "open lichess.org." Ours streams the
  game and drives the physical board (guided opponent moves, move detection,
  color-correct forwarding, real game id). Kept ours entirely.
- **Architecture:** his is one blocking `loop()`. Ours is FreeRTOS tasks on the
  shared SPI bus with the sensor. Kept ours (his portal/menu now run inside it).
- **Pins:** authority is our PCB v3 map. His LCD/button pins already matched;
  his had no LED pin. Ours stands (LED on GPIO25, buttons 36/39/34/35, etc.).

## ChessLink-esp32-game-logic -- the Pi approach (not used)

This branch offloaded chess to a Raspberry Pi: `ChessBoardESP32.ino` is a thin
HTTP client that POSTs UCI moves to a FastAPI + python-chess server
(`main.py`). That's exactly the Pi dependency we agreed to drop -- our on-ESP32
engine already does everything that server does (FEN, turn, check, checkmate,
legal-move list) with no Pi and no network round-trip. **Nothing from the ESP32
or Pi code was used.**

The genuinely valuable artifact in that branch is the **KiCad PCB design**
(`pcb/` -- schematics, footprints, gerbers, BOM). That's hardware, not firmware,
and it's consistent with the pin map we've been building to.

## Net result

One firmware: his portal + menu + Stockfish picker on the front, our engine +
tracker + board rendering + real Lichess play underneath, on our PCB pins. All
host tests pass (perft/legality, tracker, lichess parsing, famous games, menu,
queue-safety).
