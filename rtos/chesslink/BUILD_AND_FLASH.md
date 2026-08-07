# Building and flashing ChessLink

## Short answer

Yes -- the whole `ChessLink/` folder is one Arduino sketch. Open the `.ino` in the Arduino IDE and it compiles every `.cpp`/`.h` in the
folder together, then Upload flashes it. WiFi setup afterward is done entirely
from your phone (no serial monitor needed). A few build steps the first time:

## 1. One-time IDE setup

1. Install the **Arduino IDE** (2.x).
2. Add the **ESP32 board package**: File > Preferences > "Additional Boards
   Manager URLs", add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`, then
   Tools > Board > Boards Manager > install **esp32 by Espressif**.
3. Install these libraries (Tools > Manage Libraries):
   - **Adafruit GFX Library** (pulls in Adafruit BusIO)
   - **Adafruit ST7789** (comes with Adafruit ST7735/ST7789)
   - **FastLED**
   - **ArduinoJson** (v6.x)

   `WiFi`, `WiFiClientSecure`, `HTTPClient`, `Preferences`, `SPI` ship with the
   ESP32 core -- nothing to install for those.

## 2. Folder layout

Arduino requires the sketch folder and the main `.ino` to share a name. Your
main file is `chesslink.ino`, so the folder should be named `chesslink`
(Windows is case-insensitive, so `ChessLink` also opens fine -- if the IDE ever
offers to "move into a sketch folder", let it, or just rename the folder to
`chesslink`).

Keep every `.cpp` and `.h` in that one folder:

```
chesslink/
  chesslink.ino     <- open this
  main.cpp  chesslink.h
  task_sensor.cpp  task_led.cpp  task_game.cpp  task_display.cpp
  task_network.cpp  task_input.cpp
  chess.h  chess.cpp  chesslink_tracker.h  chesslink_tracker.cpp
  ui_menu.h  ui_menu.cpp  lichess.h  lichess.cpp  famous_games.h  famous_games.cpp
```

**Delete the `vmock/` folder and any `test_*.cpp` before flashing.** They exist
only for host-side verification (compiling on a PC with fake Arduino headers).
`vmock/` is a subfolder so the IDE won't compile it, but there's no reason to
ship it.

## 3. Board settings (Tools menu)

- **Board:** "ESP32 Dev Module" (or your exact board).
- **Partition Scheme:** "Huge APP (3MB No OTA/1MB SPIFFS)". The firmware plus
  WiFi + TLS is large, and an online seek briefly holds **two** TLS sockets, so
  give it room.
- **PSRAM:** enable it if your module has it -- TLS buffers appreciate the heap.
- **Upload Speed:** 921600 is fine; drop to 115200 if uploads fail.
- **Port:** the COM/tty port that appears when the board is plugged in (you may
  need the CP2102/CH340 USB driver for your board).

## 4. Flash it

1. Plug in the board.
2. Select the port under Tools > Port.
3. Click Upload. If it won't enter bootloader, hold **BOOT**, tap **EN/RST**,
   release BOOT while "Connecting..." shows.

## 5. First boot -- phone only, no serial

The board sets itself up over WiFi from your phone. Nothing needs the serial
monitor.

1. On first boot (no saved WiFi) the board starts its own network:
   **`ChessLink-Setup`**, password **`chesslink12`**. The LCD shows this.
2. Join that network from your phone. A setup page pops up automatically (it's a
   captive portal, like a hotel WiFi login). If it doesn't, open any web page or
   go to `http://192.168.4.1`.
3. On the page: pick your home WiFi from the list (or type it), enter its
   password, and paste your **Lichess API token**, then tap *Save & connect*.
4. The board saves everything to flash, drops its setup network, connects to your
   WiFi, verifies the token, and lands on the menu.

Credentials persist across reboots, so you only do this once. To reconfigure
later, choose **WiFi Setup** on the main menu (buttons only) -- it reopens the
phone portal.

Get a Lichess token at lichess.org > Preferences > API access tokens, with the
**board:play** scope.

## Notes / gotchas

- **Pins** are in `chesslink.h`. LCD on HSPI (SCLK 14 / MOSI 13 / CS 15 / DC 2),
  shift registers on VSPI (SCLK 18 / MISO 19 / LOAD 5), LEDs on GPIO 4, buttons
  on 34/35. GPIO34/35 are input-only with **no internal pull-ups** -- add
  external pull-ups; the code assumes active-low.
- **Case sensitivity:** on the bench (this sketch was verified on Linux, which is
  case-sensitive) the folder/ino name must match exactly; on Windows/Mac it's
  relaxed.
- If compilation complains about a missing library, it's one of the four in
  step 1 -- install it and rebuild.
