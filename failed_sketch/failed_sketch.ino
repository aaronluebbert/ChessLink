#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4
#define TFT_BLK  32

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

int counter = 0;
unsigned long lastTick = 0;

void drawCounter(int val);

void setup() {
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.init(170, 320);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(50, 70);
  tft.println("ChessLink");

  drawCounter(counter);
}

void loop() {
  unsigned long now = millis();

  if (now - lastTick >= 1000) {
    lastTick = now;
    counter++;
    drawCounter(counter);
  }
}

void drawCounter(int val) {
  tft.fillRect(50, 110, 220, 60, ST77XX_BLACK);

  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(5);
  tft.setCursor(50, 110);
  tft.print(val);
}