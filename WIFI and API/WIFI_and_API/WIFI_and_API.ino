#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BLK  32

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

String ssid;
String password;
String token;

// ---------- Output helper: writes to BOTH serial and LCD ----------
void show(const String& title, const String& body, uint16_t titleColor = ST77XX_CYAN) {
  Serial.println("=== " + title + " ===");
  if (body.length()) Serial.println(body);

  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(4, 6);
  tft.setTextSize(2);
  tft.setTextColor(titleColor);
  tft.println(title);
  tft.setCursor(4, tft.getCursorY() + 2);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.println(body);
}

// Serial-only status (no LCD).
void serialStatus(const String& line) {
  Serial.println(line);
}

// ---------- Serial helpers ----------
String readSerialLine() {
  String input = "";
  while (true) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (input.length() > 0) {
          input.trim();
          return input;
        }
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
      if (buf.length() > 0) {
        buf.trim();
        out = buf;
        buf = "";
        return true;
      }
    } else {
      buf += c;
    }
  }
  return false;
}

// ---------- Prompt with confirm (plain text) ----------
String promptWithConfirm(const String& fieldName) {
  while (true) {
    show("Enter " + fieldName, "Type it in the\nserial monitor,\nthen press Enter.");
    String value = readSerialLine();

    show("Confirm " + fieldName,
         value + "\n\nlength = " + String(value.length()) +
         "\n\ny = yes   n = retry");

    String answer = readSerialLine();
    answer.toLowerCase();
    if (answer == "y" || answer == "yes") {
      show(fieldName + " set", value, ST77XX_GREEN);
      delay(600);
      return value;
    }
  }
}

// ---------- Live network scan ----------
String pickNetworkLive() {
  while (true) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks();

    String list = "";
    Serial.println("=== Networks (2.4 GHz) ===");
    if (n == 0) {
      list = "No networks found.\nESP32 is 2.4 GHz only.";
      Serial.println(list);
    } else {
      for (int i = 0; i < n; i++) {
        String line = String(i + 1) + ": " + WiFi.SSID(i) +
                      " (" + String(WiFi.RSSI(i)) + ")";
        Serial.println(line);
        if (i < 7) list += WiFi.SSID(i) + "\n";
      }
    }

    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(4, 6);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_CYAN);
    tft.println("Networks");
    tft.setCursor(4, tft.getCursorY() + 2);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.println(list);
    tft.setTextColor(ST77XX_YELLOW);
    tft.println("Type SSID + Enter");
    tft.println("(rescans every 4s)");

    Serial.println("Type your SSID and press Enter (rescans every 4s)...");

    unsigned long start = millis();
    String typed;
    while (millis() - start < 4000) {
      if (tryReadSerialLine(typed)) {
        return typed;
      }
      delay(50);
    }
  }
}

// ---------- WiFi connect (retry-based, status only on serial) ----------
void connectWiFi() {
  show("WiFi", "Connecting to:\n" + ssid, ST77XX_YELLOW);
  Serial.println("Connecting to: " + ssid);

  WiFi.mode(WIFI_STA);
  delay(200);

  unsigned long start = millis();
  unsigned long lastTry = 0;

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastTry > 5000) {          // re-issue begin() every 5s
      WiFi.disconnect();
      delay(100);
      WiFi.begin(ssid.c_str(), password.c_str());
      lastTry = millis();
      serialStatus("Issued WiFi.begin()...");
    }
    delay(250);
    serialStatus("status=" + String(WiFi.status()));   // serial only

    if (millis() - start > 30000) {           // 30s total before re-prompt
      serialStatus("Giving up after retries. Last status=" + String(WiFi.status()));
      show("WiFi FAILED",
           "Couldn't connect.\nToggle hotspot\noff/on, then\nre-enter creds.",
           ST77XX_RED);
      delay(2000);
      ssid     = pickNetworkLive();
      password = promptWithConfirm("WiFi password");
      show("WiFi", "Connecting to:\n" + ssid, ST77XX_YELLOW);
      start = millis();
      lastTry = 0;
    }
  }

  Serial.println("Connected. IP: " + WiFi.localIP().toString());
  show("WiFi OK", "IP:\n" + WiFi.localIP().toString(), ST77XX_GREEN);
  delay(1000);
}

// ---------- Lichess account check ----------
void checkAccount() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://lichess.org/api/account");
  https.addHeader("Authorization", String("Bearer ") + token);

  show("Lichess", "Verifying token...", ST77XX_YELLOW);

  int code = https.GET();
  Serial.println("HTTP " + String(code));

  if (code == 200) {
    String body = https.getString();
    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      show("Parse error", err.c_str(), ST77XX_RED);
    } else {
      const char* username = doc["username"] | "(unknown)";
      show("Logged in", String(username), ST77XX_GREEN);
    }
  } else if (code == 401) {
    show("Auth FAILED", "HTTP 401\nToken rejected.\nRe-enter token.", ST77XX_RED);
    delay(1500);
    token = promptWithConfirm("Lichess token");
    checkAccount();
  } else {
    show("HTTP error", "Code: " + String(code), ST77XX_RED);
  }
  https.end();
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  tft.init(170, 320);        // 1.9" 170x320 panel
  tft.setRotation(1);        // landscape: 320 wide x 170 tall

  show("Lichess ESP32", "Starting up...", ST77XX_YELLOW);
  delay(800);
  show("Setup", "Serial Monitor:\n115200 baud,\nNewline ending.");
  delay(1200);

  ssid     = pickNetworkLive();
  password = promptWithConfirm("WiFi password");
  token    = promptWithConfirm("Lichess token");

  connectWiFi();
  checkAccount();

  show("Ready", "Setup complete.\nNext: game loop.", ST77XX_GREEN);
}

void loop() {
}