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
String username = "";          // filled after account check

const int STATUS_H = 18;       // height of the top status bar

// ---------- buttons ----------
const int bttnup    = 36;   // S_VP  (input-only, external pull-up)
const int bttndwn   = 39;   // S_VN  (input-only, external pull-up)
const int bttncnfrm = 34;   // input-only, external pull-up
const int bttnbck   = 35;   // input-only, external pull-up

int bttnupS = HIGH, bttndwnS = HIGH, bttncnfrmS = HIGH, bttnbckS = HIGH;
int bttnupP = HIGH, bttndwnP = HIGH, bttncnfrmP = HIGH, bttnbckP = HIGH;
unsigned long tUp = 0, tDwn = 0, tCnf = 0, tBck = 0;
const unsigned long DEBOUNCE_MS = 30;

// ---------- navigation state ----------
enum Screen {
  SCR_MENU, SCR_LOCAL, SCR_ONLINE, SCR_FAME, SCR_LPVP,
  SCR_Bot, SCR_Player, SCR_Ranked, SCR_SETTINGS, SCR_DELETE
};
const Screen SAME = (Screen)-1;   // sentinel: "no transition / leaf action"

Screen current = SCR_MENU;
int selection = 1;                // 1-based index of highlighted item

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
                     {"Play Bot", "Play Player", "Play Ranked"}, 3,
                     {SCR_Bot, SCR_Player, SCR_Ranked}, SCR_MENU },

  /* SCR_FAME   */ { "Fame Game", ST77XX_BLUE,
                     {"MAGvsGABRIEL", nullptr, nullptr}, 1,
                     {SAME, SAME, SAME}, SCR_LOCAL },

  /* SCR_LPVP   */ { "Loc PVP", ST77XX_BLUE,
                     {nullptr, nullptr, nullptr}, 0,
                     {SAME, SAME, SAME}, SCR_LOCAL },

  /* SCR_Bot   */ { "Bot", ST77XX_BLUE,
                   {"Finding Match", "Probably", nullptr}, 2,
                   {SAME, SAME, SAME}, SCR_ONLINE },

  /* SCR_Player*/ { "Casual", ST77XX_BLUE,
                   {"Is it", "working", nullptr}, 2,
                   {SAME, SAME, SAME}, SCR_ONLINE },

  /* SCR_Ranked*/ { "Ranked", ST77XX_BLUE,
                   {"200 ELO", "headass", "play the bot"}, 3,
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

// Build the form page. SSID is a typed text field, with scanned
// networks offered as suggestions via <datalist>.
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

// Tells the phone "got it" before the AP drops, so the page doesn't hang.
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
  delay(300);                 // let the response flush to the phone

  portalShouldClose = true;   // handled back in the portal loop
}

void startPortal() {
  screen("Setup mode", "On your phone:\n\n1) Join WiFi\n   " + String(AP_NAME) +
         "\n\n2) Open browser to\n   192.168.4.1\n\n3) Enter network +\n   API token",
         ST77XX_YELLOW);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME);
  delay(200);

  dns.start(DNS_PORT, "*", WiFi.softAPIP());   // captive portal redirect
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);               // any URL -> the form
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

// Runs the portal until the user submits, then connects. Blocks here on purpose.
void runPortal() {
  startPortal();
  while (!portalShouldClose) {
    dns.processNextRequest();
    server.handleClient();

    // Allow Back button to cancel setup without submitting
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
//                       LEAF ACTIONS & NAV
// ====================================================================

void runLeafAction(Screen from, int item) {
  if (from == SCR_SETTINGS) {
    switch (item) {
      case 1: runPortal();   break;   // "Network"
      // item 2 ("Delete save data") is a submenu (SCR_DELETE), not a leaf
    }
  } else if (from == SCR_DELETE) {
    if (item == 1) {                  // "Delete"
      clearCredentials();
      WiFi.disconnect();
      screen("Deleted", "Saved data cleared.", ST77XX_GREEN);
      delay(1000);
      goToScreen(SCR_SETTINGS);
    }
    // item 2 ("Go back") is handled as a submenu transition
  } else if (from == SCR_ONLINE) {
    switch (item) {
      case 1: Serial.println("[action] Online: Play Bot");    break;
      case 2: Serial.println("[action] Online: Play Player"); break;
      case 3: Serial.println("[action] Online: Play Ranked"); break;
    }
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
    if (reading == LOW) fired = true;     // active-low
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

  pinMode(bttnup,    INPUT);   // external pull-ups on these input-only pins
  pinMode(bttndwn,   INPUT);
  pinMode(bttncnfrm, INPUT);
  pinMode(bttnbck,   INPUT);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.init(170, 320);
  tft.setRotation(0);

  screen("ChessLink", "Starting up...", ST77XX_YELLOW);
  delay(700);

  // Startup still auto-connects with saved credentials.
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