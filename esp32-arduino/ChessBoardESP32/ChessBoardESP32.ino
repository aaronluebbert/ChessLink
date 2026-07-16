#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "YOUR_WIFI_NAME";
const char* password = "YOUR_WIFI_PASSWORD";

String moveURL = "http://192.168.1.82:8000/move";
String resetURL = "http://192.168.1.82:8000/reset";
String boardURL = "http://192.168.1.82:8000/board";

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Connected to WiFi");
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());

  Serial.println();
  Serial.println("Type a chess move in UCI format, like e2e4");
  Serial.println("Type reset to reset the board");
  Serial.println("Type board to see current board state");
}

void loop() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.length() > 0) {
      if (input == "reset") {
        resetBoard();
      }
      else if (input == "board") {
        getBoard();
      }
      else {
        sendMove(input);
      }

      Serial.println();
      Serial.println("Type next move:");
    }
  }
}

void sendMove(String move) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    http.begin(moveURL);
    http.addHeader("Content-Type", "application/json");

    String jsonData = "{\"move\":\"" + move + "\"}";

    int httpResponseCode = http.POST(jsonData);

    Serial.print("HTTP response code: ");
    Serial.println(httpResponseCode);

    String response = http.getString();
    Serial.println("Server response:");
    Serial.println(response);

    http.end();
  } else {
    Serial.println("WiFi disconnected");
  }
}

void resetBoard() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    http.begin(resetURL);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST("");

    Serial.print("HTTP response code: ");
    Serial.println(httpResponseCode);

    String response = http.getString();
    Serial.println("Reset response:");
    Serial.println(response);

    http.end();
  } else {
    Serial.println("WiFi disconnected");
  }
}

void getBoard() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    http.begin(boardURL);

    int httpResponseCode = http.GET();

    Serial.print("HTTP response code: ");
    Serial.println(httpResponseCode);

    String response = http.getString();
    Serial.println("Board response:");
    Serial.println(response);

    http.end();
  } else {
    Serial.println("WiFi disconnected");
  }
}