#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BLK  32

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;

String ssid;
String password;
String token;
String username = "";          // filled after account check

const int STATUS_H = 18;       // height of the top status bar

// ---------- small helpers ----------
String truncate(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 1) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 1) + "~";
}

// ---------- top status bar ----------
// Left: Lichess user. Right: WiFi network (or "unconnected").
void drawStatusBar() {
  tft.fillRect(0, 0, tft.width(), STATUS_H, ST77XX_BLACK);
  tft.drawFastHLine(0, STATUS_H, tft.width(), ST77XX_BLUE);
  tft.setTextSize(1);

  // Left: username
  String left = (username.length() ? username : "no user");
  left = truncate(left, 13);
  tft.setCursor(2, 5);
  tft.setTextColor(ST77XX_GREEN);
  tft.print(left);

  // Right: wifi network, right-aligned
  bool up = (WiFi.status() == WL_CONNECTED);
  String right = up ? truncate(WiFi.SSID(), 13) : "unconnected";
  int rightPixels = right.length() * 6;             // ~6px per char at size 1
  int x = tft.width() - rightPixels - 2;
  if (x < 80) x = 80;                               // don't overlap left field
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

// Redraw both regions
void screen(const String& title, const String& body, uint16_t titleColor = ST77XX_CYAN) {
  drawStatusBar();
  showBody(title, body, titleColor);
}

// ---------- serial line readers ----------
String readSerialLine() {
  String input = "";
  while (true) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (input.length() > 0) { input.trim(); return input; }
      } else {
        input += c;
      }
    }
    delay(10);
  }
}

bool tryReadSerialLine(String& out) {
  static String buf = "";
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (buf.length() > 0) { buf.trim(); out = buf; buf = ""; return true; }
    } else {
      buf += c;
    }
  }
  return false;
}

// ---------- prompt + confirm ----------
String promptWithConfirm(const String& fieldName) {
  while (true) {
    screen("Enter " + fieldName, "Type in serial,\nthen press Enter.");
    Serial.println("Enter " + fieldName + ":");
    String value = readSerialLine();

    screen("Confirm " + fieldName, value + "\n\ny = yes   n = retry");
    Serial.println("You entered: " + value + "   confirm? (y/n)");
    String answer = readSerialLine();
    answer.toLowerCase();
    if (answer == "y" || answer == "yes") return value;
  }
}

// ---------- live network picker ----------
String pickNetworkLive() {
  while (true) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks();

    String list = "";
    Serial.println("Networks:");
    for (int i = 0; i < n; i++) {
      Serial.println("  " + WiFi.SSID(i));
      if (i < 8) list += truncate(WiFi.SSID(i), 26) + "\n";
    }
    if (n == 0) list = "No networks found.";

    screen("Pick WiFi", list + "\nType SSID + Enter");
    Serial.println("Type your SSID and press Enter...");

    unsigned long start = millis();
    String typed;
    while (millis() - start < 4000) {
      if (tryReadSerialLine(typed)) return typed;
      delay(50);
    }
  }
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
      screen("WiFi failed", "Couldn't connect.\nToggle hotspot,\nthen type 'wifi'.", ST77XX_RED);
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
      drawMenu(1);
    } else {
      screen("Parse error", "Bad response.", ST77XX_RED);
    }
  } else if (code == 401) {
    username = "";
    screen("Auth failed", "Token rejected.\nType 'wifi' to\nre-enter token.", ST77XX_RED);
  } else {
    username = "";
    screen("HTTP error", "Code: " + String(code), ST77XX_RED);
  }
  https.end();
}

// ---------- full (re)configuration flow ----------
void runSetupFlow() {
  ssid     = pickNetworkLive();
  password = promptWithConfirm("WiFi password");
  token    = promptWithConfirm("Lichess token");
  if (connectWiFi()) {
    checkAccount();
    saveCredentials();
  }
}

// ---------- serial command handling ----------
void printHelp() {
  Serial.println("\n--- Commands ---");
  Serial.println("wifi  : set / change WiFi + Lichess token");
  Serial.println("clear : delete saved WiFi + token");
  Serial.println("help  : show this list");
  Serial.println("----------------\n");
}

void handleCommand(const String& cmd) {
  String c = cmd; c.toLowerCase();
  if (c == "wifi") {
    runSetupFlow();
  } else if (c == "clear") {
    clearCredentials();
    WiFi.disconnect();
    Serial.println("Saved credentials deleted.");
    screen("Cleared", "Saved WiFi + token\ndeleted.\nType 'wifi' to set.", ST77XX_YELLOW);
  } else if (c == "help") {
    printHelp();
  } else if(c == "w"){
    drawMenu(1);
  } else if (c == "s") {
    drawMenu(2);
  }else {
    Serial.println("Unknown command. Type 'help'.");
  }
}

void drawMenu(int selection) {
  tft.fillRect(0, 18, tft.width(), 40, ST77XX_RED);
  tft.setTextSize(3);

  // Project Name 
  String nm = ("CHESSLINK");
  tft.setCursor(2, 29);
  tft.setTextColor(ST77XX_WHITE);
  tft.print(nm);

  // Online Play 
  tft.fillRect(10, 80, tft.width()-20, 20, ST77XX_WHITE);
  tft.setTextSize(2);
  String na = ("Local");
  tft.setCursor(12, 85);
  tft.setTextColor(ST77XX_BLACK);
  tft.print(na);

  // Local Play Button 
  tft.fillRect(10, 120, tft.width()-20, 20, ST77XX_WHITE);
  tft.setTextSize(2);
  String nb = ("Online");
  tft.setCursor(12, 125);
  tft.setTextColor(ST77XX_BLACK);
  tft.print(nb);

  // Selection Marker
  if (selection == 1){
    tft.drawRect(5, 75, tft.width()-10, 30, ST77XX_YELLOW);
    tft.drawRect(5, 115, tft.width()-10, 30, ST77XX_BLACK);
  }else if (selection == 2){
    tft.drawRect(5, 75, tft.width()-10, 30, ST77XX_BLACK);
    tft.drawRect(5, 115, tft.width()-10, 30, ST77XX_YELLOW);
  }else{

  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.init(170, 320);
  tft.setRotation(0);          // portrait: 170 wide x 320 tall

  screen("Lichess ESP32", "Starting up...", ST77XX_YELLOW);
  delay(700);
  printHelp();

  if (loadCredentials()) {
    if (connectWiFi()) checkAccount();
    else screen("Idle", "Type 'wifi' to\nreconfigure.", ST77XX_YELLOW);
  } else {
    screen("First setup", "No saved creds.\nStarting setup...", ST77XX_YELLOW);
    delay(800);
    runSetupFlow();
  }

  drawStatusBar();
}

// ---------- loop ----------
void loop() {
  // Listen for serial commands at any time.
  String line;
  if (tryReadSerialLine(line)) {
    handleCommand(line);
  }

  // Keep the status bar fresh (e.g. if WiFi drops).
  static unsigned long lastBar = 0;
  if (millis() - lastBar > 3000) {
    drawStatusBar();
    lastBar = millis();
  }
}
