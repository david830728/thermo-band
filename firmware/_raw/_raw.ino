// Raw ST7789V2 driver -- no graphics library at all.
// Waveshare ESP32-S3-Touch-LCD-1.69: DC=4 CS=5 SCK=6 MOSI=7 RST=8 BL=15, 240x280
// Step markers + esp_reset_reason() so a crash is easy to localise.
#include <SPI.h>
#include <esp_system.h>

#define PIN_SCK   6
#define PIN_MOSI  7
#define PIN_CS    5
#define PIN_DC    4
#define PIN_RST   8
#define PIN_BL   15

#define W 240
#define H 280

// ---- 7-segment digit table (a=bit0 b=1 c=2 d=3 e=4 f=5 g=6) ----
static const uint8_t kSeg[10] = {
  0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
  0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111
};

static void mark(const char *s) {
  Serial.printf("[RAW] %s  (uptime %lus)\n", s, (unsigned long)(millis() / 1000));
  Serial.flush();
}

static inline void cmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}
static inline void dat(uint8_t d) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  SPI.transfer(d);
  digitalWrite(PIN_CS, HIGH);
}
static void datN(const uint8_t *p, size_t n) {
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < n; i++) SPI.transfer(p[i]);
  digitalWrite(PIN_CS, HIGH);
}

static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  cmd(0x2A); { uint8_t b[4] = {(uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1}; datN(b, 4); }
  cmd(0x2B); { uint8_t b[4] = {(uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1}; datN(b, 4); }
  cmd(0x2C);
}

static void fillRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = color >> 8, lo = color & 0xFF;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  digitalWrite(PIN_CS, HIGH);
}
static void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }

static void drawDigit(int x, int y, int w, int h, int t, uint16_t color) {
  uint8_t m = kSeg[t % 10];
  int hw = w / 2, hh = h / 2;
  if (m & 0x01) fillRect(x, y, w, t ? 4 : 4, color);                      // a
  if (m & 0x02) fillRect(x + w - 4, y, 4, hh, color);                     // b
  if (m & 0x04) fillRect(x + w - 4, y + hh, 4, hh, color);                // c
  if (m & 0x08) fillRect(x, y + h - 4, w, 4, color);                      // d
  if (m & 0x10) fillRect(x, y + hh, 4, hh, color);                        // e
  if (m & 0x20) fillRect(x, y, 4, hh, color);                             // f
  if (m & 0x40) fillRect(x, y + hh - 2, w, 4, color);                     // g
}
static void drawNumber(int x, int y, int w, int h, long v, uint16_t color) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%ld", v < 0 ? -v : v);
  for (char *p = buf; *p; p++) {
    if (v < 0 && p == buf) fillRect(x - 10, y + h / 2, 6, 4, color);   // minus sign
    drawDigit(x, y, w, h, *p - '0', color);
    x += w + 8;
  }
}

static void st7789Init() {
  digitalWrite(PIN_RST, LOW); delay(20);
  digitalWrite(PIN_RST, HIGH); delay(120);

  cmd(0x01); delay(150);              // SWRESET
  cmd(0x11); delay(120);              // SLPOUT
  cmd(0x3A); dat(0x55);               // COLMOD 16bit
  cmd(0x36); dat(0x00);               // MADCTL
  cmd(0xB2); { uint8_t p[] = {0x0C, 0x0C, 0x00, 0x33, 0x33}; datN(p, 5); }  // PORCTRL
  cmd(0xB7); dat(0x35);               // GCTRL
  cmd(0xBB); dat(0x19);               // VCOMS
  cmd(0xC0); dat(0x2C);               // LCMCTRL
  cmd(0xC2); dat(0x01);               // VDVVRHEN
  cmd(0xC3); dat(0x12);               // VRHS
  cmd(0xC4); dat(0x20);               // VDVS
  cmd(0xC6); dat(0x0F);               // FRCTRL2
  cmd(0xD0); { uint8_t p[] = {0xA4, 0xA1}; datN(p, 2); }                    // PWCTRL1
  cmd(0xE0); { uint8_t p[] = {0xD0,0x04,0x0D,0x11,0x13,0x2B,0x3F,0x54,0x4C,0x18,0x0D,0x0B,0x1F,0x23}; datN(p, 14); }
  cmd(0xE1); { uint8_t p[] = {0xD0,0x04,0x0C,0x11,0x13,0x2C,0x3F,0x44,0x51,0x2F,0x1F,0x1F,0x20,0x23}; datN(p, 14); }
  cmd(0x21);                          // INVON
  cmd(0x13);                          // NORON
  cmd(0x29); delay(20);               // DISPON
}

void setup() {
  Serial.begin(115200);
  delay(1200);

  const char *why;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  why = "POWERON";  break;
    case ESP_RST_EXT:      why = "EXT_PIN";  break;
    case ESP_RST_SW:       why = "SOFTWARE"; break;
    case ESP_RST_PANIC:    why = "PANIC";    break;
    case ESP_RST_INT_WDT:  why = "INT_WDT";  break;
    case ESP_RST_TASK_WDT: why = "TASK_WDT"; break;
    case ESP_RST_WDT:      why = "WDT";      break;
    case ESP_RST_BROWNOUT: why = "BROWNOUT"; break;
    default:               why = "OTHER";    break;
  }
  Serial.printf("\n=== RAW BOOT  reset_reason=%d (%s) ===\n", (int)esp_reset_reason(), why);

  mark("step0 control idle 4s");
  delay(4000);

  mark("step1 pins + backlight");
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  delay(800);

  mark("step2 SPI.begin");
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  delay(200);

  mark("step3 st7789Init()");
  st7789Init();
  mark("step4 init done");

  mark("step5 fill RED");
  fillScreen(0xF800); delay(700);
  mark("step6 fill GREEN");
  fillScreen(0x07E0); delay(700);
  mark("step7 fill BLUE");
  fillScreen(0x001F); delay(700);

  mark("step8 layout");
  fillScreen(0x0000);
  fillRect(0, 0, W, 3, 0xFFFF);
  fillRect(0, H - 3, W, 3, 0xFFFF);
  fillRect(0, 0, 3, H, 0xFFFF);
  fillRect(W - 3, 0, 3, H, 0xFFFF);
  fillRect(20, 20, 60, 60, 0xF800);
  fillRect(90, 20, 60, 60, 0x07E0);
  fillRect(160, 20, 60, 60, 0x001F);
  mark("step9 all drawn -> DISPLAY WORKS");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 900) {
    last = millis();
    long s = millis() / 1000;
    fillRect(30, 120, 180, 70, 0x0000);
    drawNumber(40, 130, 26, 50, s, 0x07FF);
    Serial.printf("[RAW] alive %lus\n", (unsigned long)s);
  }
}
