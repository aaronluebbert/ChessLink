# Building and flashing ChessLink

The whole `rtos/chesslink/` folder is one Arduino sketch. Open `chesslink.ino` in the Arduino IDE and it compiles every `.cpp` and `.h` in the folder together.

## 1. One-time IDE setup

1. Install **Arduino IDE 2.x**.
2. Add the ESP32 board package. In File > Preferences, add this URL to "Additional Boards Manager URLs":
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   Then open Tools > Board > Boards Manager and install **esp32 by Espressif**.
3. Install these libraries from Tools > Manage Libraries:
   - **GFX Library for Arduino** (provides `Arduino_GFX_Library.h`, drives the ST7789)
   - **FastLED**
   - **ArduinoJson**, version 6.x

   `WiFi`, `HTTPClient`, `WebServer`, `DNSServer`, `Preferences`, and `SPI` ship with the ESP32 core, so there is nothing to install for those.

## 2. Folder layout

The sketch folder and the main `.ino` must share a name. The folder is `chesslink` and the sketch is `chesslink.ino`, which already matches. Keep every source file in that one folder:

```
chesslink/
  chesslink.ino      <- open this
  main.cpp           chesslink.h        board_map.h
  chess_engine.h     chess_engine.cpp
  task_sensor.cpp    task_led.cpp       task_buttons.cpp
  task_game.cpp      task_display.cpp   task_network.cpp
```

## 3. Board settings (Tools menu)

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Partition Scheme | Huge APP (3MB No OTA/1MB SPIFFS) |
| PSRAM | Enabled, if your module has it |
| Upload Speed | 921600, or 115200 if uploads fail |
| Port | The COM or tty port that appears when the board is plugged in |

The firmware plus WiFi and TLS is large, and an online seek briefly holds two TLS connections, so the Huge APP partition and PSRAM both help.

## 4. Flash

1. Plug the board in over USB and select the port.
2. Click Upload.
3. If it will not enter the bootloader, hold **BOOT**, tap **EN**, and release BOOT once the IDE shows "Connecting...".

Your computer may need the CP2102 or CH340 USB driver for the dev board.

## 5. First boot

Setup is done entirely from a phone, with no serial monitor.

1. With no saved WiFi, the board starts an access point named **`ChessLink-Setup`** (password `chesslink12`, set in `chesslink.h`). The LCD shows instructions.
2. Join it from a phone. A setup page opens like a hotel WiFi login. If it does not, browse to `http://192.168.4.1`.
3. Choose your WiFi network, enter its password, and paste a Lichess API token. Create one at lichess.org under Preferences, API access tokens, with the **`board:play`** scope.
4. Tap **Save & connect**. The board stores everything in flash, joins your network, and lands on the main menu.

Credentials survive reboots. To change them later, pick **WiFi Setup** from the main menu, which reopens the phone page.

## Wiring notes

The full table is in [`PIN_MAP.md`](PIN_MAP.md). Two points that matter when bringing up a board:

- The LCD and the shift-register chain share the ESP32 VSPI bus (SCK 18, MISO 19, MOSI 23). The sensor and display tasks take a mutex around bus access.
- GPIO 34, 35, 36, and 39 are input-only and have no internal pull-ups. The buttons need external pull-ups to 3.3 V and are active-low.

## Troubleshooting

| Symptom | Check |
|---|---|
| Sketch too large | Partition Scheme set to Huge APP |
| Upload fails or times out | Lower the upload speed, use the BOOT and EN sequence above |
| LCD backlight on but screen blank | Display init or SPI wiring, since the backlight (GPIO 32) is switched on before the panel is initialized |
| Whole board reads as occupied | Shift-register data line (GPIO 19) and the 5 V to 3.3 V divider on the return line |
| Setup page does not appear | Open `http://192.168.4.1` manually, or forget the network and rejoin |
| "No token" or "Seek failed" on the LCD | Re-run **WiFi Setup** and re-enter the token, which needs the `board:play` scope |
