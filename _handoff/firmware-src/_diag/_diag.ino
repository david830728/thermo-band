// Display bring-up diagnostic -- pinpoints the exact step that resets the chip
// Prints esp_reset_reason() at every boot:
//   1=POWERON 2=EXT 3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 7=WDT 8=DEEPSLEEP 9=BROWNOUT
// Then walks through: control wait -> backlight -> tft.init() -> rotation -> fills,
// printing a marker and pausing 3s around each one.
#include <SPI.h>
#include <TFT_eSPI.h>
#include <esp_system.h>

TFT_eSPI tft = TFT_eSPI();

static void mark(const char *s) {
  Serial.printf("[DIAG] %s  (uptime %lus)\n", s, (unsigned long)(millis() / 1000));
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
    case ESP_RST_PANIC:     why = "PANIC/CRASH"; break;
    case ESP_RST_INT_WDT:   why = "INT_WDT";   break;
    case ESP_RST_TASK_WDT:  why = "TASK_WDT";  break;
    case ESP_RST_WDT:       why = "WDT";       break;
    case ESP_RST_DEEPSLEEP: why = "DEEPSLEEP"; break;
    case ESP_RST_BROWNOUT:  why = "BROWNOUT(low power)"; break;
    case ESP_RST_SDIO:      why = "SDIO";      break;
    default:                why = "UNKNOWN";   break;
  }
  Serial.println();
  Serial.printf("=== DIAG BOOT  reset_reason=%d (%s) ===\n", (int)esp_reset_reason(), why);
  Serial.printf("chip=%s flash=%dMB psram=%dMB heap=%u\n", ESP.getChipModel(),
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / (1024 * 1024),
                (unsigned)ESP.getFreeHeap());

  mark("step0 control: display untouched, idle 6s");
  delay(6000);

  mark("step1 backlight pin HIGH (GPIO15)");
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  delay(3000);

  mark("step2 tft.init() BEGIN");
  tft.init();
  mark("step3 tft.init() DONE");
  delay(1500);

  tft.setRotation(0);
  mark("step4 setRotation(0) ok");
  delay(1000);

  tft.fillScreen(TFT_RED);
  mark("step5 fillScreen(RED) ok");
  delay(1200);
  tft.fillScreen(TFT_GREEN);
  mark("step6 fillScreen(GREEN) ok");
  delay(1200);
  tft.fillScreen(TFT_BLUE);
  mark("step7 fillScreen(BLUE) ok");
  delay(1200);

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("DIAG OK", 10, 10, 4);
  tft.drawRect(0, 0, 240, 280, TFT_WHITE);
  mark("step8 text+box drawn  -> DISPLAY WORKS");
}

void loop() {
  static uint32_t n = 0;
  mark("loop alive");
  if (n++ == 3) {
    tft.fillRect(120, 60, 110, 24, TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(String(millis() / 1000), 122, 62, 2);
  }
  delay(2000);
}
