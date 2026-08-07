# ChessLink -- what's left before the final version

Audit of the current firmware (the lean, single-version tree). The build is
coherent and compiles: boot lands on a Local/Online menu, both local and online
play handle every move type (captures, castling, en passant, promotion), online
opponent moves are guided and reconciled on the physical board, games end with a
result screen that returns to the menu, online play seeks a lichess game and
streams it, the match screen shows clocks + opponent name/rating, a first-boot
captive portal writes WiFi + token to NVS, and network states/errors are surfaced
on the LCD, four buttons (up/down/confirm/cancel) drive the UI, and an online
rated-match submenu picks the time control, a Play Bot mode challenges Stockfish
(level / time / color), and CANCEL leaves/resigns a game back to the menu. All
the gameplay and product features are done -- what remains is cleanup and
hardware bring-up.

## Cleanup and docs

**1. Stale docs.** `BUILD_AND_FLASH.md` lists deleted files and the old LED pin
(GPIO4, now 25) -- needs a file-list and pin refresh. `INTEGRATION.md` and
`MERGE_NOTES.md` describe the earlier merge and reference removed files; keep as
history or delete. `FILE_REFERENCE.md` (new) is the authoritative per-file guide.

**2. Move rejected by lichess** -- if `POST /move` returns non-200 (rare: race /
not-your-turn), the board and our truth are a move ahead of the server. Currently
logged only; a full fix would revert the local move (see "board-desync recovery"
below, which would also cover this). Low priority.

## Hardware bring-up (gates "final," but not code)

Validate on the real board: the sensor square map (currently the identity map),
the LED chain order, the level shifter on GPIO25, external pull-ups on the
buttons, and LED brightness/thermals. Build with the "Huge APP" partition and
PSRAM so the dual-TLS online seek has heap.

## Future features (appraisal)

The product is complete; these are worth-considering enhancements, roughly
ordered by value-for-effort. None are required to ship.

**High value**
- **Board-desync recovery.** If the physical board ever diverges from our truth
  mid-game (a piece knocked over, a misplacement, or a lichess-rejected move),
  enter a "restore the board" state that `LED_CMD_HILITE`s the wrong squares
  (`occupied XOR pos.all`) and resumes only when they match. Reuses the setup-check
  machinery; the single biggest robustness win and also fixes the move-rejected case.
- **Resume an in-progress game after reboot.** On boot, `GET /api/account/playing`;
  if a game is live, offer to rejoin and stream it. Saves a game after a brown-out.
- **Persist settings in NVS.** Remember last time control, bot level/color, and LED
  brightness across reboots (namespace "wifi" or a new "cfg"). Low effort, nice QoL.

**Medium value**
- **King-in-check LED.** Flash the checked king's square red; the engine already has
  `in_check`. Cheap, good feedback.
- **Rematch on the result screen.** A "play again" option that restarts with the same
  settings instead of returning to the menu.
- **Draw offer / takeback (online).** Board API supports both
  (`/api/board/game/{id}/draw/yes`, `/takeback/yes`); add as in-game button actions.
- **Flip the mini-board when playing black.** Render from the player's perspective.
- **Famous-game guided replay.** Re-add the removed feature; it can reuse
  `PHASE_OPP_SYNC` guidance to walk the player through a stored game move by move.
- **Settings menu.** A menu entry for LED brightness and similar, backed by NVS.

**Larger / longer-term**
- **Lichess puzzle mode.** Fetch a puzzle, guide the solution on the board.
- **OTA firmware update** over WiFi, so the board can be updated without a cable.
- **Move list / PGN view** on a screen or exported over serial/WiFi.
- **Correspondence (days-based) games** in addition to real-time.

---

### Suggested order

1. Doc refresh (`BUILD_AND_FLASH.md`, decide on the history docs) + hardware bring-up.
2. Board-desync recovery (highest-value robustness feature).
3. Resume-after-reboot, persist settings, then the medium-value polish items.

---

### Done

- **Full dead-code + wiring audit.** Confirmed all 8 queues, 6 tasks, engine API,
  and preset tables have exactly one definition and are produced/consumed; no
  unused functions, enums, `#define`s, or colors. Removed the only dead code found:
  the write-only clock-increment fields (server clock times already include
  increment) and the never-read `MoveEvent.src` / `MoveSrc_t` enum; fixed a stale
  TODO. Wrote `FILE_REFERENCE.md` (reconstruction-grade per-file guide).
- **Resign / exit during a game** (was the last usability item). CANCEL mid-game
  opens a "Leave game?" confirm; Confirm resigns an online/bot game (lichess
  `resign`, falling back to `abort` early) and drops a local game, both returning
  to the menu. The resign is read inside the game stream (where the network task
  is parked), reports "You resigned", and board/opponent input is frozen while the
  prompt is up so nothing changes underneath the decision.
- **Play Computer (Stockfish) mode** (was the top optional item). A "Play Bot"
  menu item opens a config submenu (level 1-8, time control incl. untimed, color
  white/black/random) and starts a game via `POST /api/challenge/ai`, then streams
  and plays it through the same path as human games. Our color is taken from the
  request (or the response for random), and the AI player shows as "Stockfish" on
  the match screen. Errors surface as "Bot start failed".
- **Four-button scheme + online rated-match submenu** (was the button-scheme and
  online-options items). The final control set is Up / Down / Confirm / Cancel on
  GPIO 36 / 39 / 34 / 35, wired across the menu, config submenu, promotion picker,
  and result screen. Picking Online now opens a config screen (matching the menu
  style) to choose a time control (1+0 through 15+10) and rated/casual before
  seeking; the choice is passed to the network task and used in the seek request.
  This also reconciled the firmware with `PIN_MAP.md`'s four-button layout.
- **Start-position check** (was item 2). Each game now enters `PHASE_SETUP_BOARD`
  and waits until the sensor occupancy matches the standard start
  (`0xFFFF00000000FFFF`) before the first move; squares still missing a piece are
  lit green via a new `LED_CMD_HILITE`. The latest board occupancy is cached every
  scan (even in the menu) so an already-set-up board passes instantly. Host-checked
  the start occupancy and missing-square math.
- **On-screen network status/error feedback** (was item 1). A shared notice
  screen shows a transient "Seeking opponent..." during a seek and terminal
  notices for errors -- "No token: run setup", "Seek failed", "Connection lost"
  -- which return to the menu on any button, reusing the game-over path. Board
  input and buttons are gated while a notice is up.
- **Game-over handling + return to menu** (was item 1). Local games detect
  checkmate/stalemate/50-move through the engine after each move; online games
  take the authoritative result from lichess's terminal `gameState` (mate,
  resign, timeout, draw, aborted), formatted from our perspective ("You won:
  mate", "Draw: stalemate"). A `PHASE_GAME_OVER` result screen (`UI_GAMEOVER`)
  shows the outcome and any button returns to the menu. End detection host-tested
  against the engine (fool's mate, back-rank mate, stalemate, 50-move, ongoing).
- **On-device provisioning (captive portal)** (was item 1). First boot with no
  saved WiFi (or on demand via the new "WiFi Setup" menu item) brings up the
  `ChessLink-Setup` soft-AP + a DNS/web captive portal; the phone form writes
  ssid/pass/token to NVS ("wifi" namespace), then the board connects and reloads
  the token. The network task reports `NET_STATUS_SETUP` / `NET_STATUS_ONLINE`
  up to the display, which shows a setup-instructions screen while the portal is
  up. This also resolved the old "confirm NVS keys" item -- the same code writes
  and reads `ssid`/`pass`/`token`.
- **Opponent-move physical reconciliation** (was the top critical item).
  `apply_opponent_move()` applies the move to our truth, lights the from/to
  squares, and holds in `PHASE_OPP_SYNC` until the sensors match the post-move
  occupancy before handing the turn back. Full-occupancy matching handles opponent
  captures/castling/en passant; opponent-move intake is gated on an idle board so
  moves queue instead of racing. Host-checked that a partial (king-only) castle
  does not false-complete the sync.
- **Expected-occupancy move detection** (was item 1). `process_board_change()`
  precomputes the occupancy each legal move from the lifted square would leave and
  commits whichever the sensors settle onto. Captures, castling, en passant, and
  promotion are all handled. Verified against the real engine with a host test
  (quiet / capture / O-O / O-O-O / en passant / promotion / promotion-capture).
- Fixed a pre-existing build breaker found along the way: `chess_engine.cpp` used
  `RANK_3` / `RANK_6` before they were defined -- moved them into the header's rank
  masks so the engine actually compiles.
