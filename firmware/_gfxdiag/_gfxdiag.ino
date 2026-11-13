// Arduino_GFX display bring-up -- Waveshare ESP32-S3-Touch-LCD-1.69 (ST7789V2 240x280)
// Same step markers + esp_reset_reason() as the failing TFT_eSPI build, so we can compare.
#include <Arduino_GFX_Library.h>
#include <esp_system.h>

#define TFT_DC   4
#define TFT_CS   5
#define TFT_SCK  6
#define TFT_MOSI 7
#define TFT_RST  8
#define TFT_BL   15

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, GFX_NOT_DEFINED);
Arduino_GFX     *gfx = new Arduino_ST7789(bus, TFT_RST, 0, true, 240, 280);

static void mark(const char *s) {
  Serial.printf("[GFX] %s  (uptime %lus)\n", s, (unsigned long)(millis() / 1000));
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  const char *why;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   why = "POWERON";   break;
    case ESP_RST_EXT:       why = "EXT_PIN";   break;
    case ESP_RST_SW:        why = "SOFTWARE";  break;
    case ESP_RST_PANIC:     why = "PANIC";     break;
    case ESP_RST_INT_WDT:   why = "INT_WDT";   break;
    case ESP_RST_TASK_WDT:  why = "TASK_WDT";  break;
    case ESP_RST_WDT:       why = "WDT";       break;
    case ESP_RST_BROWNOUT:  why = "BROWNOUT";  break;
    default:                why = "OTHER";     break;
  }
  Serial.println();
  Serial.printf("=== GFX DIAG BOOT  reset_reason=%d (%s) ===\n", (int)esp_reset_reason(), why);

  mark("step0 control: idle 5s");
  delay(5000);

  mark("step1 backlight HIGH");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  delay(1500);

  mark("step2 gfx->begin() BEGIN");
  bool ok = gfx->begin();
  mark(ok ? "step3 begin() returned TRUE" : "step3 begin() returned FALSE");
  delay(800);

  mark("step4 fillScreen RED");
  gfx->fillScreen(RGB565_RED);
  delay(900);
  mark("step5 fillScreen GREEN");
  gfx->fillScreen(RGB565_GREEN);
  delay(900);
  mark("step6 fillScreen BLUE");
  gfx->fillScreen(RGB565_BLUE);
  delay(900);

  mark("step7 text");
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(10, 20);
  gfx->println("GFX OK");
  gfx->setTextSize(2);
  gfx->setCursor(10, 70);
  gfx->printf("%dx%d", gfx->width(), gfx->height());
  gfx->drawRect(0, 0, gfx->width(), gfx->height(), RGB565_WHITE);
  mark("step8 all drawn -> DISPLAY WORKS");
}

void loop() {
  static uint32_t n = 0;
  mark("loop alive");
  gfx->fillRect(10, 110, 120, 30, RGB565_BLACK);
  gfx->setTextColor(RGB565_YELLOW);
  gfx->setTextSize(2);
  gfx->setCursor(10, 110);
  gfx->printf("t=%lus", (unsigned long)(millis() / 1000));
  delay(2000);
}
