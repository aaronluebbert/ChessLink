#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BLK  32

#define CL_AMBER 0xFD20
#define CL_GREY  0x8410
#define CL_DGREEN 0x03E0
#define CL_DISABLED 0x2124
#define CL_EDIT   ST77XX_RED     // edit-mode highlight color (was amber)
#define CL_LIGHTSQ 0xC618        // light board square
#define CL_DARKSQ  0x5AEB        // dark board square

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;

WebServer server(80);
DNSServer dns;
const byte DNS_PORT = 53;
const char* AP_NAME = "ChessLink-Setup";
bool portalActive = false;
bool portalShouldClose = false;

String ssid;
String password;
String token;
String username = "";

const int STATUS_H = 18;

// ---------- buttons ----------
const int bttnup    = 36;
const int bttndwn   = 39;
const int bttncnfrm = 34;
const int bttnbck   = 35;

int bttnupS = HIGH, bttndwnS = HIGH, bttncnfrmS = HIGH, bttnbckS = HIGH;
int bttnupP = HIGH, bttndwnP = HIGH, bttncnfrmP = HIGH, bttnbckP = HIGH;
unsigned long tUp = 0, tDwn = 0, tCnf = 0, tBck = 0;
const unsigned long DEBOUNCE_MS = 30;

// ---------- navigation state ----------
enum Screen {
  SCR_MENU, SCR_LOCAL, SCR_ONLINE, SCR_FAME, SCR_LPVP,
  SCR_PVP_CAT, SCR_SETTINGS, SCR_DELETE
};
const Screen SAME = (Screen)-1;

Screen current = SCR_MENU;
int selection = 1;

struct ScreenDef {
  const char* title;
  uint16_t    headerColor;
  const char* items[3];
  int         itemCount;
  Screen      onSelect[3];
  Screen      onBack;
};

const ScreenDef screens[] = {
  /* SCR_MENU   */ { "CHESSLINK", ST77XX_RED,
                     {"Local", "Online", "Settings"}, 3,
                     {SCR_LOCAL, SCR_ONLINE, SCR_SETTINGS}, SCR_MENU },

  /* SCR_LOCAL  */ { "Local", ST77XX_BLUE,
                     {"Famous Games", "Local Match", nullptr}, 2,
                     {SCR_FAME, SCR_LPVP, SAME}, SCR_MENU },

  /* SCR_ONLINE */ { "Online", ST77XX_BLUE,
                     {"Play Stockfish", "Play Player", nullptr}, 2,
                     {SAME, SCR_PVP_CAT, SAME}, SCR_MENU },

  /* SCR_FAME   */ { "Fame Game", ST77XX_BLUE,
                     {"MAGvsGABRIEL", nullptr, nullptr}, 1,
                     {SAME, SAME, SAME}, SCR_LOCAL },

  /* SCR_LPVP   */ { "Loc PVP", ST77XX_BLUE,
                     {nullptr, nullptr, nullptr}, 0,
                     {SAME, SAME, SAME}, SCR_LOCAL },

  /* SCR_PVP_CAT*/ { "Find Game", ST77XX_BLUE,
                     {"Rapid", "Classical", nullptr}, 2,
                     {SAME, SAME, SAME}, SCR_ONLINE },

  /* SCR_SETTINGS*/ { "Settings", ST77XX_BLUE,
                   {"Network", "Delete save data", nullptr}, 2,
                   {SAME, SCR_DELETE, SAME}, SCR_MENU },

  /* SCR_DELETE */ { "Delete data?", ST77XX_RED,
                   {"Delete", "Go back", nullptr}, 2,
                   {SAME, SCR_SETTINGS, SAME}, SCR_SETTINGS },
};

// ---------- forward declarations ----------
void drawStatusBar();
void drawScreen();
void startPortal();
void checkAccount();
bool runStockfishSettings();
String startAIGame();
void runFindGame(int category);
bool pressed(int reading, int& prev, unsigned long& tStamp);
void playGame(const String& gameId, const String& modeLabel, const String& timeLabel);

// ---------- small helpers ----------
String truncate(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 1) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 1) + "~";
}

// ---------- top status bar ----------
void drawStatusBar() {
  tft.fillRect(0, 0, tft.width(), STATUS_H, ST77XX_BLACK);
  tft.drawFastHLine(0, STATUS_H, tft.width(), ST77XX_BLUE);
  tft.setTextSize(1);

  String left = (username.length() ? username : "no user");
  left = truncate(left, 13);
  tft.setCursor(2, 5);
  tft.setTextColor(ST77XX_GREEN);
  tft.print(left);

  bool up = (WiFi.status() == WL_CONNECTED);
  String right = up ? truncate(WiFi.SSID(), 13) : "unconnected";
  int rightPixels = right.length() * 6;
  int x = tft.width() - rightPixels - 2;
  if (x < 80) x = 80;
  tft.setCursor(x, 5);
  tft.setTextColor(up ? ST77XX_CYAN : ST77XX_RED);
  tft.print(right);
}

// ---------- body text below the status bar ----------
void showBody(const String& title, const String& body, uint16_t titleColor = ST77XX_CYAN) {
  tft.fillRect(0, STATUS_H + 1, tft.width(), tft.height() - STATUS_H - 1, ST77XX_BLACK);
  tft.setCursor(4, STATUS_H + 8);
  tft.setTextSize(2);
  tft.setTextColor(titleColor);
  tft.println(title);
  tft.setCursor(4, tft.getCursorY() + 2);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.println(body);
}

void screen(const String& title, const String& body, uint16_t titleColor = ST77XX_CYAN) {
  drawStatusBar();
  showBody(title, body, titleColor);
}

// ---------- the one menu renderer ----------
void drawScreen() {
  const ScreenDef& s = screens[current];

  tft.fillScreen(ST77XX_BLACK);
  drawStatusBar();

  tft.fillRect(0, STATUS_H, tft.width(), 40, s.headerColor);
  tft.setTextSize(3);
  tft.setCursor(2, 29);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(s.title);

  for (int i = 0; i < s.itemCount; i++) {
    int y = 80 + i * 40;
    tft.fillRect(10, y, tft.width() - 20, 20, ST77XX_WHITE);
    tft.setTextSize(2);
    tft.setCursor(12, y + 5);
    tft.setTextColor(ST77XX_BLACK);
    tft.print(s.items[i]);

    uint16_t box = (selection == i + 1) ? ST77XX_YELLOW : ST77XX_BLACK;
    tft.drawRect(5, y - 5, tft.width() - 10, 30, box);
  }
}

void goToScreen(Screen s) {
  current = s;
  selection = 1;
  drawScreen();
}

// ---------- persistence ----------
void saveCredentials() {
  prefs.begin("lichess", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.putString("token", token);
  prefs.end();
}

bool loadCredentials() {
  prefs.begin("lichess", true);
  ssid     = prefs.getString("ssid", "");
  password = prefs.getString("pass", "");
  token    = prefs.getString("token", "");
  prefs.end();
  return ssid.length() && password.length() && token.length();
}

void clearCredentials() {
  prefs.begin("lichess", false);
  prefs.clear();
  prefs.end();
  ssid = ""; password = ""; token = ""; username = "";
}

// ---------- WiFi connect (retry-based) ----------
bool connectWiFi() {
  screen("WiFi", "Connecting to:\n" + ssid, ST77XX_YELLOW);
  WiFi.mode(WIFI_STA);
  delay(200);

  unsigned long start = millis();
  unsigned long lastTry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastTry > 5000) {
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid.c_str(), password.c_str());
      lastTry = millis();
    }
    delay(250);
    if (millis() - start > 30000) {
      screen("WiFi failed", "Couldn't connect.\nOpen Settings >\nNetwork to retry.", ST77XX_RED);
      delay(1500);
      return false;
    }
  }
  screen("WiFi OK", "IP:\n" + WiFi.localIP().toString(), ST77XX_GREEN);
  delay(800);
  return true;
}

// ---------- Lichess account check ----------
void checkAccount() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://lichess.org/api/account");
  https.addHeader("Authorization", String("Bearer ") + token);

  screen("Lichess", "Verifying token...", ST77XX_YELLOW);
  int code = https.GET();

  if (code == 200) {
    String body = https.getString();
    StaticJsonDocument<2048> doc;
    if (!deserializeJson(doc, body)) {
      username = String(doc["username"] | "(unknown)");
      screen("Logged in", "as " + username, ST77XX_GREEN);
      delay(800);
    } else {
      screen("Parse error", "Bad response.", ST77XX_RED);
      delay(800);
    }
  } else if (code == 401) {
    username = "";
    screen("Auth failed", "Token rejected.", ST77XX_RED);
    delay(800);
  } else {
    username = "";
    screen("HTTP error", "Code: " + String(code), ST77XX_RED);
    delay(800);
  }
  https.end();
}

// ====================================================================
//                       PHONE SETUP PORTAL
// ====================================================================

String buildPortalPage() {
  int n = WiFi.scanNetworks();

  String options = "";
  for (int i = 0; i < n; i++) {
    String nm = WiFi.SSID(i);
    options += "<option value=\"" + nm + "\">";
  }

  String page =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='utf-8'>"
    "<title>ChessLink Setup</title>"
    "<style>"
    ":root{--bg:#11131a;--panel:#1b1f2b;--ink:#f2ede3;--muted:#8a92a6;"
    "--amber:#e0a13c;--amber-d:#b97f25;--line:#2c3242;}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:"
    "repeating-linear-gradient(45deg,#11131a 0 22px,#141722 22px 44px);"
    "color:var(--ink);font-family:'Segoe UI',system-ui,sans-serif;"
    "min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
    ".card{background:var(--panel);border:1px solid var(--line);border-radius:14px;"
    "padding:26px 22px;width:100%;max-width:380px;"
    "box-shadow:0 18px 50px rgba(0,0,0,.45)}"
    ".crown{font-size:30px;line-height:1}"
    "h1{font-family:Georgia,'Times New Roman',serif;font-size:25px;margin:6px 0 2px;"
    "letter-spacing:.5px}"
    ".sub{color:var(--muted);font-size:13px;margin:0 0 22px}"
    "label{display:block;font-size:12px;text-transform:uppercase;letter-spacing:1.4px;"
    "color:var(--amber);margin:16px 0 6px}"
    "input{width:100%;padding:12px 13px;background:#0d0f15;color:var(--ink);"
    "border:1px solid var(--line);border-radius:9px;font-size:15px}"
    "input:focus{outline:none;border-color:var(--amber)}"
    ".hint{color:var(--muted);font-size:11px;margin:6px 2px 0}"
    "button{width:100%;margin-top:24px;padding:14px;border:0;border-radius:9px;"
    "background:linear-gradient(180deg,var(--amber),var(--amber-d));"
    "color:#1a1206;font-size:15px;font-weight:700;letter-spacing:.3px;cursor:pointer}"
    "button:active{transform:translateY(1px)}"
    ".foot{color:var(--muted);font-size:11px;text-align:center;margin-top:16px}"
    "</style></head><body>"
    "<form class='card' action='/save' method='POST'>"
    "<div class='crown'>&#9818;</div>"
    "<h1>ChessLink</h1>"
    "<p class='sub'>Connect your board to play on Lichess.</p>"

    "<label>Network</label>"
    "<input type='text' name='ssid' list='nets' autocomplete='off' "
    "autocapitalize='off' spellcheck='false' placeholder='Type or pick a network'>"
    "<datalist id='nets'>" + options + "</datalist>"
    "<p class='hint'>Type it in, or choose from networks in range.</p>"

    "<label>WiFi password</label>"
    "<input type='password' name='pass' autocomplete='off'>"

    "<label>Lichess API token</label>"
    "<input type='text' name='token' autocomplete='off' spellcheck='false' "
    "autocapitalize='off'>"
    "<p class='hint'>From lichess.org &rarr; Preferences &rarr; API tokens.</p>"

    "<button type='submit'>Save and connect</button>"
    "<p class='foot'>Your board will leave this setup network and join yours.</p>"
    "</form></body></html>";

  return page;
}

void handleRoot() {
  server.send(200, "text/html", buildPortalPage());
}

void handleSave() {
  ssid     = server.arg("ssid");
  password = server.arg("pass");
  token    = server.arg("token");

  String done =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<style>body{margin:0;background:#11131a;color:#f2ede3;"
    "font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;display:flex;"
    "align-items:center;justify-content:center;text-align:center;padding:24px}"
    "h1{font-family:Georgia,serif;color:#e0a13c}p{color:#8a92a6}</style>"
    "</head><body><div><h1>Saving&hellip;</h1>"
    "<p>The board is joining your network.<br>You can close this page.</p>"
    "</div></body></html>";
  server.send(200, "text/html", done);
  delay(300);

  portalShouldClose = true;
}

void startPortal() {
  screen("Setup mode", "On your phone:\n\n1) Join WiFi\n   " + String(AP_NAME) +
         "\n\n2) Open browser to\n   192.168.4.1\n\n3) Enter network +\n   API token",
         ST77XX_YELLOW);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME);
  delay(200);

  dns.start(DNS_PORT, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);
  server.begin();

  portalActive = true;
  portalShouldClose = false;
}

void stopPortal() {
  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
}

void runPortal() {
  startPortal();
  while (!portalShouldClose) {
    dns.processNextRequest();
    server.handleClient();

    if (digitalRead(bttnbck) == LOW) {
      delay(50);
      stopPortal();
      goToScreen(SCR_SETTINGS);
      return;
    }
    delay(5);
  }

  stopPortal();

  if (connectWiFi()) {
    checkAccount();
    saveCredentials();
  }
  goToScreen(SCR_SETTINGS);
}

// ====================================================================
//                    STOCKFISH SETTINGS SCREEN
// ====================================================================

int    selLevel    = 3;
int    selTimeIdx  = 2;
int    selColorIdx = 0;
String selColor    = "white";

struct TimePreset { int limit; int inc; const char* label; };
const TimePreset TIME_PRESETS[] = {
  {  60, 0, "1+0"  },
  { 180, 0, "3+0"  },
  { 300, 0, "5+0"  },
  { 300, 3, "5+3"  },
  { 600, 0, "10+0" },
  { 600, 5, "10+5" },
  { 900,10, "15+10"},
};
const int NUM_TIME = sizeof(TIME_PRESETS) / sizeof(TIME_PRESETS[0]);

const char* COLOR_LABELS[] = { "White", "Black", "Random" };
const char* COLOR_VALUES[] = { "white", "black", "random" };

enum { ROW_LEVEL, ROW_TIME, ROW_COLOR, ROW_START, NUM_ROWS };

int  setCursor = 0;
bool editing   = false;
int  editBackup = 0;

const int SET_TOP   = STATUS_H + 44;
const int SET_ROWH  = 34;
const int VAL_X     = 105;
const int VAL_W     = 62;
int rowY(int r) { return SET_TOP + r * SET_ROWH; }

String rowValue(int r) {
  if (r == ROW_LEVEL) return String(selLevel);
  if (r == ROW_TIME)  return TIME_PRESETS[selTimeIdx].label;
  return COLOR_LABELS[selColorIdx];
}

void drawValue(int r) {
  int y = rowY(r);
  bool active = (setCursor == r);
  bool isEditingThis = (active && editing);

  tft.fillRect(VAL_X, y, VAL_W, 16, ST77XX_BLACK);

  String v = rowValue(r);
  tft.setTextSize(2);
  if (isEditingThis) {
    tft.setTextColor(CL_EDIT);           // RED in edit mode
    tft.setCursor(VAL_X + 1, y);
    tft.print(v);
  } else {
    tft.setTextColor(active ? ST77XX_YELLOW : ST77XX_CYAN);
  }
  tft.setCursor(VAL_X, y);
  tft.print(v);
}

void drawRowLabel(int r) {
  int y = rowY(r);
  bool active = (setCursor == r);
  const char* labels[3] = { "Level", "Time", "Color" };

  tft.fillRect(2, y, 14, 16, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(2, y);
  tft.setTextColor(active ? ST77XX_YELLOW : ST77XX_BLACK);
  tft.print(active ? ">" : " ");

  tft.setCursor(18, y);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(labels[r]);
}

void drawStartBar() {
  int by = tft.height() - 40;
  bool startActive = (setCursor == ROW_START);

  tft.fillRect(4, by - 4, tft.width() - 8, 40, ST77XX_BLACK);

  uint16_t barCol = startActive ? ST77XX_GREEN : CL_DGREEN;
  tft.fillRoundRect(10, by, tft.width() - 20, 30, 6, barCol);
  if (startActive) tft.drawRoundRect(8, by - 2, tft.width() - 16, 34, 7, ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_BLACK);
  tft.setCursor(tft.width()/2 - 48, by + 8);
  tft.print("PLAY NOW");
}

void drawHint() {
  tft.fillRect(0, tft.height() - 10, tft.width(), 10, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(CL_GREY);
  tft.setCursor(6, tft.height() - 8);
  tft.print(editing ? "up/dn adjust  sel ok  back cancel"
                    : "up/dn move  sel choose  back exit");
}

void drawSettings() {
  tft.fillScreen(ST77XX_BLACK);
  drawStatusBar();

  tft.fillRect(0, STATUS_H, tft.width(), 34, ST77XX_BLUE);
  tft.setTextSize(2);
  tft.setCursor(6, STATUS_H + 9);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Stockfish");

  for (int r = 0; r < 3; r++) {
    drawRowLabel(r);
    drawValue(r);
  }
  drawStartBar();
  drawHint();
}

void editAdjust(int delta) {
  switch (setCursor) {
    case ROW_LEVEL:
      selLevel += delta;
      if (selLevel < 1) selLevel = 1;
      if (selLevel > 8) selLevel = 8;
      break;
    case ROW_TIME:
      selTimeIdx += delta;
      if (selTimeIdx < 0) selTimeIdx = 0;
      if (selTimeIdx > NUM_TIME - 1) selTimeIdx = NUM_TIME - 1;
      break;
    case ROW_COLOR:
      selColorIdx += delta;
      if (selColorIdx < 0) selColorIdx = 0;
      if (selColorIdx > 2) selColorIdx = 2;
      break;
  }
}

void snapshotRow() {
  switch (setCursor) {
    case ROW_LEVEL: editBackup = selLevel;    break;
    case ROW_TIME:  editBackup = selTimeIdx;  break;
    case ROW_COLOR: editBackup = selColorIdx; break;
  }
}
void restoreRow() {
  switch (setCursor) {
    case ROW_LEVEL: selLevel    = editBackup; break;
    case ROW_TIME:  selTimeIdx  = editBackup; break;
    case ROW_COLOR: selColorIdx = editBackup; break;
  }
}

bool runStockfishSettings() {
  setCursor = 0;
  editing = false;
  drawSettings();

  while (true) {
    int up = digitalRead(bttnup);
    int dn = digitalRead(bttndwn);
    int ok = digitalRead(bttncnfrm);
    int bk = digitalRead(bttnbck);

    if (pressed(up, bttnupP, tUp)) {
      if (editing) {
        editAdjust(+1);
        drawValue(setCursor);
      } else {
        int prev = setCursor;
        setCursor--; if (setCursor < 0) setCursor = 0;
        if (setCursor != prev) {
          drawRowLabel(prev); if (prev < 3) drawValue(prev);
          if (setCursor < 3) { drawRowLabel(setCursor); drawValue(setCursor); }
          drawStartBar();
        }
      }
    }
    if (pressed(dn, bttndwnP, tDwn)) {
      if (editing) {
        editAdjust(-1);
        drawValue(setCursor);
      } else {
        int prev = setCursor;
        setCursor++; if (setCursor > NUM_ROWS - 1) setCursor = NUM_ROWS - 1;
        if (setCursor != prev) {
          drawRowLabel(prev); if (prev < 3) drawValue(prev);
          if (setCursor < 3) { drawRowLabel(setCursor); drawValue(setCursor); }
          drawStartBar();
        }
      }
    }
    if (pressed(ok, bttncnfrmP, tCnf)) {
      if (setCursor == ROW_START) {
        selColor = COLOR_VALUES[selColorIdx];
        return true;
      } else if (editing) {
        editing = false;
        drawValue(setCursor);
        drawHint();
      } else {
        snapshotRow();
        editing = true;
        drawValue(setCursor);
        drawHint();
      }
    }
    if (pressed(bk, bttnbckP, tBck)) {
      if (editing) {
        restoreRow();
        editing = false;
        drawValue(setCursor);
        drawHint();
      } else {
        return false;
      }
    }
    delay(5);
  }
}

// ---------- start a game vs Stockfish AI; returns game id ----------
String startAIGame() {
  if (WiFi.status() != WL_CONNECTED) return "";

  int limit = TIME_PRESETS[selTimeIdx].limit;
  int inc   = TIME_PRESETS[selTimeIdx].inc;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://lichess.org/api/challenge/ai");
  https.addHeader("Authorization", String("Bearer ") + token);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body =
      "level="            + String(selLevel) +
      "&clock.limit="     + String(limit) +
      "&clock.increment=" + String(inc) +
      "&color="           + selColor +
      "&variant=standard";

  int code = https.POST(body);
  String resp = https.getString();
  https.end();

  Serial.printf("AI challenge HTTP %d\n", code);
  Serial.println(resp);

  if (code == 200 || code == 201) {
    StaticJsonDocument<3072> doc;
    if (!deserializeJson(doc, resp)) {
      return String((const char*)(doc["id"] | ""));
    }
  }
  return "";
}

// ====================================================================
//                 PLAY PLAYER — FIND GAME (Board seek)
//   Board API supports RAPID and CLASSICAL only (no bullet/blitz).
// ====================================================================

struct SeekPreset { int minutes; int inc; const char* label; };

const SeekPreset RAPID[] = {
  {10,0,"10+0"}, {10,5,"10+5"}, {15,10,"15+10"},
};
const SeekPreset CLASSICAL[] = {
  {30,0,"30+0"}, {30,20,"30+20"},
};

const SeekPreset* CAT_LIST[2]  = { RAPID, CLASSICAL };
const int         CAT_COUNT[2] = { 3, 2 };
const char*       CAT_NAME[2]  = { "Rapid", "Classical" };

// Fire a Board seek. time in MINUTES, increment in SECONDS. Random color.
bool startSeek(int minutes, int inc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  https.begin(client, "https://lichess.org/api/board/seek");
  https.addHeader("Authorization", String("Bearer ") + token);
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body =
      "rated=false"
      "&time="      + String(minutes) +
      "&increment=" + String(inc) +
      "&variant=standard";

  int code = https.POST(body);
  String resp = https.getString();
  https.end();

  Serial.printf("Board seek HTTP %d\n", code);
  Serial.println(resp);

  return (code == 200 || code == 201);
}

void drawFindList(int category, int cursor, int chosen) {
  tft.fillScreen(ST77XX_BLACK);
  drawStatusBar();

  tft.fillRect(0, STATUS_H, tft.width(), 34, ST77XX_BLUE);
  tft.setTextSize(2);
  tft.setCursor(6, STATUS_H + 9);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(CAT_NAME[category]);

  int n = CAT_COUNT[category];
  const SeekPreset* list = CAT_LIST[category];

  int top = STATUS_H + 44;
  int rowH = 26;
  for (int i = 0; i < n; i++) {
    int y = top + i * rowH;
    bool active   = (cursor == i);
    bool isChosen = (chosen == i);

    if (isChosen) {
      tft.fillRoundRect(18, y - 3, tft.width() - 40, 22, 4, CL_DGREEN);
    }

    tft.setTextSize(2);
    tft.setCursor(2, y);
    tft.setTextColor(active ? ST77XX_YELLOW : ST77XX_BLACK);
    tft.print(active ? ">" : " ");

    tft.setCursor(24, y);
    if (isChosen)     tft.setTextColor(ST77XX_WHITE);
    else if (active)  tft.setTextColor(ST77XX_YELLOW);
    else              tft.setTextColor(ST77XX_CYAN);
    tft.print(list[i].label);
  }

  int by = tft.height() - 40;
  bool goActive = (cursor == n);
  bool ready    = (chosen >= 0);
  uint16_t barCol;
  if (!ready)  barCol = CL_DISABLED;
  else         barCol = goActive ? ST77XX_GREEN : CL_DGREEN;

  tft.fillRoundRect(10, by, tft.width() - 20, 30, 6, barCol);
  if (goActive && ready) tft.drawRoundRect(8, by - 2, tft.width() - 16, 34, 7, ST77XX_YELLOW);
  tft.setTextSize(2);
  tft.setTextColor(ready ? ST77XX_BLACK : CL_GREY);
  tft.setCursor(tft.width()/2 - 54, by + 8);
  tft.print("FIND GAME");

  tft.setTextSize(1);
  tft.setTextColor(CL_GREY);
  tft.setCursor(6, tft.height() - 8);
  tft.print(chosen < 0 ? "sel to choose a time first"
                       : "up/dn move   sel ok   back exit");
}

void runFindGame(int category) {
  int n = CAT_COUNT[category];
  int cursor = 0;
  int chosen = -1;
  drawFindList(category, cursor, chosen);

  while (true) {
    int up = digitalRead(bttnup);
    int dn = digitalRead(bttndwn);
    int ok = digitalRead(bttncnfrm);
    int bk = digitalRead(bttnbck);

    if (pressed(up, bttnupP, tUp)) {
      cursor--; if (cursor < 0) cursor = 0;
      drawFindList(category, cursor, chosen);
    }
    if (pressed(dn, bttndwnP, tDwn)) {
      cursor++; if (cursor > n) cursor = n;
      drawFindList(category, cursor, chosen);
    }
    if (pressed(ok, bttncnfrmP, tCnf)) {
      if (cursor == n) {
        if (chosen >= 0) {
          const SeekPreset& p = CAT_LIST[category][chosen];
          if (WiFi.status() != WL_CONNECTED) {
            screen("No WiFi", "Connect first via\nSettings > Network.", ST77XX_RED);
            delay(1400);
            drawFindList(category, cursor, chosen);
          } else {
            screen("Searching", String(CAT_NAME[category]) + "  " + p.label +
                   "\n\nSeeking a random\nopponent...\n\n(Game-start detect\nis a TODO: watch\nlichess.org)", ST77XX_YELLOW);
            bool okSeek = startSeek(p.minutes, p.inc);
            if (okSeek) {
              screen("Seek sent", "Waiting for a\nmatch on " + String(p.label) +
                     "\n\nGame appears on\nlichess.org", ST77XX_GREEN);
              delay(2500);
            } else {
              screen("Seek failed", "Lichess refused.\nSee serial log.", ST77XX_RED);
              delay(2000);
            }
            return;
          }
        }
      } else {
        chosen = cursor;
        drawFindList(category, cursor, chosen);
      }
    }
    if (pressed(bk, bttnbckP, tBck)) {
      return;
    }
    delay(5);
  }
}

// ====================================================================
//                        IN-GAME SCREEN
// ====================================================================
// Maintains an 8x8 board model, applies UCI moves from the game stream,
// renders players/ratings/board, and offers Abort / Resign / Draw.

// Board model: uppercase = white, lowercase = black, '.' = empty.
char board[8][8];

// Game info parsed from the stream
String gWhiteName = "?", gBlackName = "?";
int    gWhiteRating = 0, gBlackRating = 0;
bool   gIAmWhite = true;         // is the logged-in user White?
String gModeLabel = "", gTimeLabel = "";
String gStatus = "started";      // started, mate, resign, draw, aborted...
String lastMovesApplied = "";    // to detect new moves

// selection among the 3 action buttons
int gBtn = 0;                    // 0=Abort 1=Resign 2=Draw

void boardInit() {
  const char* back = "rnbqkbnr";
  for (int c = 0; c < 8; c++) {
    board[0][c] = back[c];        // rank 8 (black back rank), row 0 = top
    board[1][c] = 'p';
    for (int r = 2; r < 6; r++) board[r][c] = '.';
    board[6][c] = 'P';
    board[7][c] = (char)toupper(back[c]);
  }
}

// Convert a UCI square like "e2" to row/col in our array (row 0 = rank 8).
void sqToRC(const String& sq, int& r, int& c) {
  c = sq.charAt(0) - 'a';
  int rank = sq.charAt(1) - '1';      // 0..7 where 0 = rank 1
  r = 7 - rank;                       // row 0 = rank 8
}

// Apply one UCI move (e.g. "e2e4", "e7e8q", "e1g1" castle) to the board.
void applyUci(const String& mv) {
  if (mv.length() < 4) return;
  int fr, fc, tr, tc;
  sqToRC(mv.substring(0, 2), fr, fc);
  sqToRC(mv.substring(2, 4), tr, tc);
  char piece = board[fr][fc];

  // en passant: pawn moves diagonally to an empty square
  if ((piece == 'P' || piece == 'p') && fc != tc && board[tr][tc] == '.') {
    board[fr][tc] = '.';              // captured pawn is on from-row, to-col
  }

  // castling: king moves two files -> move the rook too
  if (piece == 'K' || piece == 'k') {
    if (fc == 4 && tc == 6) {         // king side
      board[tr][5] = board[tr][7]; board[tr][7] = '.';
    } else if (fc == 4 && tc == 2) {  // queen side
      board[tr][3] = board[tr][0]; board[tr][0] = '.';
    }
  }

  board[tr][tc] = piece;
  board[fr][fc] = '.';

  // promotion: 5th char is the new piece
  if (mv.length() >= 5) {
    char promo = mv.charAt(4);
    board[tr][tc] = (piece == 'P') ? (char)toupper(promo) : (char)tolower(promo);
  }
}

// Rebuild the board from the full move list (space-separated UCI).
void applyMoveList(const String& moves) {
  boardInit();
  int start = 0;
  while (start < moves.length()) {
    int sp = moves.indexOf(' ', start);
    String mv = (sp < 0) ? moves.substring(start) : moves.substring(start, sp);
    mv.trim();
    if (mv.length() >= 4) applyUci(mv);
    if (sp < 0) break;
    start = sp + 1;
  }
}

// ---------- drawing the game screen ----------
const int BOARD_PX = 160;                 // 8 * 20
const int SQ = BOARD_PX / 8;              // 20
int boardTop;                             // computed in layout

// piece letter color: white pieces bright, black pieces dark
void drawPieceLetter(int r, int c) {
  char p = board[r][c];
  if (p == '.') return;
  int x = 5 + c * SQ;
  int y = boardTop + r * SQ;
  bool whitePiece = (p >= 'A' && p <= 'Z');
  char letter = (char)toupper(p);
  tft.setTextSize(2);
  // outline-ish: draw dark behind for contrast then the letter
  tft.setTextColor(whitePiece ? ST77XX_WHITE : ST77XX_BLACK);
  tft.setCursor(x + 4, y + 3);
  tft.print(letter);
}

void drawBoard() {
  for (int r = 0; r < 8; r++) {
    for (int c = 0; c < 8; c++) {
      int x = 5 + c * SQ;
      int y = boardTop + r * SQ;
      uint16_t sqcol = ((r + c) % 2 == 0) ? CL_LIGHTSQ : CL_DARKSQ;
      tft.fillRect(x, y, SQ, SQ, sqcol);
      drawPieceLetter(r, c);
    }
  }
  tft.drawRect(5, boardTop, BOARD_PX, BOARD_PX, ST77XX_WHITE);
}

void drawPlayers() {
  // opponent row (top) and me row (bottom of info area)
  int oppTop = STATUS_H + 2;
  int oppRating = gIAmWhite ? gBlackRating : gWhiteRating;
  String oppName = gIAmWhite ? gBlackName : gWhiteName;
  int myRating  = gIAmWhite ? gWhiteRating : gBlackRating;
  String myName = gIAmWhite ? gWhiteName : gBlackName;

  // opponent line
  tft.fillRect(0, oppTop, tft.width(), 12, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(2, oppTop + 2);
  tft.print(truncate(oppName, 16));
  String orat = String(oppRating);
  tft.setCursor(tft.width() - orat.length()*6 - 2, oppTop + 2);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(orat);

  // mode + time line
  int infoTop = oppTop + 12;
  tft.fillRect(0, infoTop, tft.width(), 12, ST77XX_BLACK);
  tft.setTextColor(CL_GREY);
  tft.setCursor(2, infoTop + 2);
  tft.print(gModeLabel + "  " + gTimeLabel);

  // my line (just under the board)
  int myTop = boardTop + BOARD_PX + 2;
  tft.fillRect(0, myTop, tft.width(), 12, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(2, myTop + 2);
  tft.print(truncate(myName, 16));
  String mrat = String(myRating);
  tft.setCursor(tft.width() - mrat.length()*6 - 2, myTop + 2);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(mrat);
}

void drawActionButtons() {
  int by = tft.height() - 34;
  const char* labels[3] = { "Abort", "Resign", "Draw" };
  int bw = (tft.width() - 8) / 3;
  for (int i = 0; i < 3; i++) {
    int x = 2 + i * bw;
    bool active = (gBtn == i);
    uint16_t bg = active ? ST77XX_YELLOW : 0x2124;
    tft.fillRoundRect(x + 1, by, bw - 2, 28, 4, bg);
    tft.setTextSize(1);
    tft.setTextColor(active ? ST77XX_BLACK : ST77XX_WHITE);
    int tw = strlen(labels[i]) * 6;
    tft.setCursor(x + (bw - tw)/2, by + 10);
    tft.print(labels[i]);
  }
}

void drawGameScreen() {
  tft.fillScreen(ST77XX_BLACK);
  drawStatusBar();
  drawPlayers();
  drawBoard();
  drawActionButtons();
}

// ---------- game action POSTs ----------
bool gameAction(const String& gameId, const String& path) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.begin(client, "https://lichess.org/api/board/game/" + gameId + "/" + path);
  https.addHeader("Authorization", String("Bearer ") + token);
  int code = https.POST("");
  String resp = https.getString();
  https.end();
  Serial.printf("action %s -> HTTP %d\n", path.c_str(), code);
  Serial.println(resp);
  return (code == 200);
}

// Parse a gameFull / gameState json line and update model. Returns true if
// something changed that needs a redraw.
bool handleStreamLine(const String& line) {
  if (line.length() < 2) return false;
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) { Serial.println("json err in stream line"); return false; }

  const char* type = doc["type"] | "";

  if (strcmp(type, "gameFull") == 0) {
    gWhiteName   = String((const char*)(doc["white"]["name"] | doc["white"]["id"] | "White"));
    gBlackName   = String((const char*)(doc["black"]["name"] | doc["black"]["id"] | "Black"));
    gWhiteRating = doc["white"]["rating"] | 0;
    gBlackRating = doc["black"]["rating"] | 0;
    // am I white? compare my username to white name (case-insensitive-ish)
    String wl = gWhiteName; wl.toLowerCase();
    String un = username;   un.toLowerCase();
    gIAmWhite = (wl == un);
    String moves = String((const char*)(doc["state"]["moves"] | ""));
    applyMoveList(moves);
    lastMovesApplied = moves;
    gStatus = String((const char*)(doc["state"]["status"] | "started"));
    return true;
  }
  else if (strcmp(type, "gameState") == 0) {
    String moves = String((const char*)(doc["moves"] | ""));
    if (moves != lastMovesApplied) {
      applyMoveList(moves);
      lastMovesApplied = moves;
    }
    gStatus = String((const char*)(doc["status"] | "started"));
    return true;
  }
  return false;
}

// Main in-game routine: opens the game stream, renders, handles buttons.
void playGame(const String& gameId, const String& modeLabel, const String& timeLabel) {
  gModeLabel = modeLabel;
  gTimeLabel = timeLabel;
  gBtn = 0;
  boardInit();
  lastMovesApplied = "";

  // layout
  boardTop = STATUS_H + 28;

  // loading screen
  screen("Loading", "Opening game...\n" + gameId, ST77XX_YELLOW);

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect("lichess.org", 443)) {
    screen("Stream fail", "Couldn't open\ngame stream.", ST77XX_RED);
    delay(1500);
    return;
  }

  // Send the streaming GET request manually so we can read line-by-line.
  String req =
      "GET /api/board/game/stream/" + gameId + " HTTP/1.1\r\n"
      "Host: lichess.org\r\n"
      "Authorization: Bearer " + token + "\r\n"
      "Accept: application/x-ndjson\r\n"
      "Connection: keep-alive\r\n\r\n";
  client.print(req);

  // Skip HTTP response headers (until blank line)
  unsigned long hstart = millis();
  while (client.connected()) {
    String h = client.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
    if (millis() - hstart > 8000) break;
  }

  bool drawn = false;
  unsigned long lastData = millis();

  while (true) {
    // read any available ndjson lines
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      lastData = millis();
      if (line.length() > 0) {
        if (handleStreamLine(line)) {
          drawGameScreen();
          drawn = true;
        }
      }
    }

    if (!drawn && millis() - lastData > 1500) {
      // still waiting on first data
      screen("Loading", "Waiting for game\ndata...\n" + gameId, ST77XX_YELLOW);
      drawn = false;
      lastData = millis();
    }

    // stream ended / game over?
    if (gStatus != "started" && gStatus != "created" && drawn) {
      drawGameScreen();
      tft.setTextSize(2);
      tft.setTextColor(ST77XX_YELLOW);
      tft.setCursor(10, boardTop + BOARD_PX/2 - 8);
      tft.print(gStatus);
      delay(2500);
      break;
    }

    if (!client.connected()) {
      // stream closed by server (game ended)
      break;
    }

    // ---- buttons ----
    int lft = digitalRead(bttnup);     // up  = move selection left
    int rgt = digitalRead(bttndwn);    // down= move selection right
    int ok  = digitalRead(bttncnfrm);
    int bk  = digitalRead(bttnbck);

    if (pressed(lft, bttnupP, tUp)) {
      gBtn--; if (gBtn < 0) gBtn = 0;
      drawActionButtons();
    }
    if (pressed(rgt, bttndwnP, tDwn)) {
      gBtn++; if (gBtn > 2) gBtn = 2;
      drawActionButtons();
    }
    if (pressed(ok, bttncnfrmP, tCnf)) {
      if (gBtn == 0) { gameAction(gameId, "abort");  }
      if (gBtn == 1) { gameAction(gameId, "resign"); }
      if (gBtn == 2) { gameAction(gameId, "draw/yes"); }
    }
    if (pressed(bk, bttnbckP, tBck)) {
      // leave the game screen (does not resign; game continues on lichess)
      break;
    }

    delay(10);
  }

  client.stop();
}

// ====================================================================
//                       LEAF ACTIONS & NAV
// ====================================================================

void runLeafAction(Screen from, int item) {
  if (from == SCR_SETTINGS) {
    switch (item) {
      case 1: runPortal();   break;
    }
  } else if (from == SCR_DELETE) {
    if (item == 1) {
      clearCredentials();
      WiFi.disconnect();
      screen("Deleted", "Saved data cleared.", ST77XX_GREEN);
      delay(1000);
      goToScreen(SCR_SETTINGS);
    }
  } else if (from == SCR_ONLINE) {
    if (item == 1) {                          // "Play Stockfish"
      bool go = runStockfishSettings();
      if (go) {
        if (WiFi.status() != WL_CONNECTED) {
          screen("No WiFi", "Connect first via\nSettings > Network.", ST77XX_RED);
          delay(1400);
        } else {
          screen("Starting", "Lvl " + String(selLevel) + "  " +
                 TIME_PRESETS[selTimeIdx].label + "\n" + selColor + "\n\nContacting Lichess...",
                 ST77XX_YELLOW);
          String id = startAIGame();
          if (id.length()) {
            // straight into the game screen with the returned id
            playGame(id, "Stockfish " + String(selLevel),
                     TIME_PRESETS[selTimeIdx].label);
          } else {
            screen("Start failed", "Lichess refused.\nSee serial log for\nthe response.", ST77XX_RED);
            delay(2000);
          }
        }
      }
      goToScreen(SCR_ONLINE);
    }
  } else if (from == SCR_PVP_CAT) {
    runFindGame(item - 1);
    goToScreen(SCR_PVP_CAT);
  } else if (from == SCR_FAME) {
    Serial.println("[action] Famous game viewer");
  } else if (from == SCR_LPVP) {
    Serial.println("[action] Local PvP match");
  }
}

void moveSelection(int delta) {
  int count = screens[current].itemCount;
  if (count == 0) return;
  selection += delta;
  if (selection < 1)     selection = 1;
  if (selection > count) selection = count;
  drawScreen();
}

void confirm() {
  const ScreenDef& s = screens[current];
  if (s.itemCount == 0) return;
  Screen dest = s.onSelect[selection - 1];
  if (dest == SAME) {
    runLeafAction(current, selection);
  } else {
    goToScreen(dest);
  }
}

void goBack() {
  Screen dest = screens[current].onBack;
  if (dest != current) goToScreen(dest);
}

// ---------- button edge detection (debounced) ----------
bool pressed(int reading, int& prev, unsigned long& tStamp) {
  bool fired = false;
  if (reading != prev && (millis() - tStamp) > DEBOUNCE_MS) {
    if (reading == LOW) fired = true;
    prev = reading;
    tStamp = millis();
  }
  return fired;
}

// ====================================================================
//                            SETUP / LOOP
// ====================================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(bttnup,    INPUT);
  pinMode(bttndwn,   INPUT);
  pinMode(bttncnfrm, INPUT);
  pinMode(bttnbck,   INPUT);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.init(170, 320);
  tft.setRotation(0);

  screen("ChessLink", "Starting up...", ST77XX_YELLOW);
  delay(700);

  if (loadCredentials()) {
    if (connectWiFi()) checkAccount();
    else screen("Offline", "No connection.\nSettings > Network\nto set up.", ST77XX_YELLOW);
  } else {
    screen("Welcome", "No saved data.\nSettings > Network\nto connect a phone.", ST77XX_YELLOW);
  }
  delay(1200);

  goToScreen(SCR_MENU);
}

void loop() {
  bttnupS    = digitalRead(bttnup);
  bttndwnS   = digitalRead(bttndwn);
  bttncnfrmS = digitalRead(bttncnfrm);
  bttnbckS   = digitalRead(bttnbck);

  if (pressed(bttnupS,    bttnupP,    tUp))  moveSelection(-1);
  if (pressed(bttndwnS,   bttndwnP,   tDwn)) moveSelection(+1);
  if (pressed(bttncnfrmS, bttncnfrmP, tCnf)) confirm();
  if (pressed(bttnbckS,   bttnbckP,   tBck)) goBack();

  static unsigned long lastBar = 0;
  if (millis() - lastBar > 3000) {
    drawStatusBar();
    lastBar = millis();
  }
}
