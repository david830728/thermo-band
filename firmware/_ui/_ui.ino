// Wristband dashboard -- raw ST7789 driver (no graphics library), fixed Y offset 20.
// Shows: title, MAX30205 temp, DS18B20 temp, I2C device count, uptime.
// Rescans both sensor buses every 20s so the screen updates the moment wiring is fixed.
#include <SPI.h>
#include <Wire.h>
#include <esp_system.h>

#define PIN_SCK   6
#define PIN_MOSI  7
#define PIN_CS    5
#define PIN_DC    4
#define PIN_RST   8
#define PIN_BL   15

#define W 240
#define H 280
#define YOFF 20               // 240x280 panel: visible area starts at RAM row 20

#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
#define C_DKGREY 0x4208
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BLUE   0x001F
#define C_CYAN   0x07FF
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20

// ---------------- 5x7 font ----------------
struct Glyph { char c; uint8_t col[5]; };
static const Glyph kFont[] = {
  {' ', {0x00,0x00,0x00,0x00,0x00}},
  {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'.', {0x00,0x00,0x00,0x60,0x00}},
  {':', {0x00,0x36,0x00,0x36,0x00}},
  {'0', {0x3E,0x51,0x49,0x45,0x3E}},
  {'1', {0x00,0x42,0x7F,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}},
  {'3', {0x21,0x41,0x45,0x4B,0x31}},
  {'4', {0x18,0x14,0x12,0x7F,0x10}},
  {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3C,0x4A,0x49,0x49,0x30}},
  {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}},
  {'9', {0x06,0x49,0x49,0x29,0x1E}},
  {'A', {0x7E,0x11,0x11,0x11,0x7E}},
  {'B', {0x7F,0x49,0x49,0x49,0x36}},
  {'C', {0x3E,0x41,0x41,0x41,0x22}},
  {'D', {0x7F,0x41,0x41,0x22,0x1C}},
  {'E', {0x7F,0x49,0x49,0x49,0x41}},
  {'G', {0x3E,0x41,0x49,0x49,0x7A}},
  {'I', {0x00,0x41,0x7F,0x41,0x00}},
  {'K', {0x7F,0x08,0x14,0x22,0x41}},
  {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
  {'N', {0x7F,0x04,0x08,0x10,0x7F}},
  {'O', {0x3E,0x41,0x41,0x41,0x3E}},
  {'P', {0x7F,0x09,0x09,0x09,0x06}},
  {'R', {0x7F,0x09,0x19,0x29,0x46}},
  {'S', {0x26,0x49,0x49,0x49,0x32}},
  {'T', {0x01,0x01,0x7F,0x01,0x01}},
  {'U', {0x3F,0x40,0x40,0x40,0x3F}},
  {'V', {0x1F,0x20,0x40,0x20,0x1F}},
  {'X', {0x63,0x14,0x08,0x14,0x63}},
  {'Y', {0x07,0x08,0x70,0x08,0x07}},
  {'?', {0x02,0x01,0x51,0x09,0x06}},
};
static const uint8_t kFontN = sizeof(kFont) / sizeof(kFont[0]);

// ---------------- low level ----------------
static inline void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
static inline void dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }

static void setWindow(int x0, int y0, int x1, int y1) {
  y0 += YOFF; y1 += YOFF;
  cmd(0x2A); dat(x0 >> 8); dat(x0 & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);
  cmd(0x2B); dat(y0 >> 8); dat(y0 & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);
  cmd(0x2C);
}
static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  digitalWrite(PIN_CS, HIGH);
}
static void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }

static void drawChar(int x, int y, char ch, int s, uint16_t col) {
  const uint8_t *g = kFont[kFontN - 1].col;   // '?' default
  for (uint8_t i = 0; i < kFontN; i++) if (kFont[i].c == ch) { g = kFont[i].col; break; }
  for (int c = 0; c < 5; c++) {
    uint8_t bits = g[c];
    for (int r = 0; r < 7; r++) {
      if (bits & (1 << r)) fillRect(x + c * s, y + r * s, s, s, col);
    }
  }
}
static int textWidth(const char *s, int scale) { return strlen(s) * 6 * scale; }
static void drawText(int x, int y, const char *s, int scale, uint16_t col) {
  while (*s) { drawChar(x, y, *s++, scale, col); x += 6 * scale; }
}
static void drawTextC(int y, const char *s, int scale, uint16_t col) {
  drawText((W - textWidth(s, scale)) / 2, y, s, scale, col);
}

// ---------------- ST7789 ----------------
static void st7789Init() {
  digitalWrite(PIN_RST, LOW); delay(20);
  digitalWrite(PIN_RST, HIGH); delay(120);
  cmd(0x01); delay(150);
  cmd(0x11); delay(120);
  cmd(0x3A); dat(0x55);
  cmd(0x36); dat(0x00);
  cmd(0xB2); dat(0x0C); dat(0x0C); dat(0x00); dat(0x33); dat(0x33);
  cmd(0xB7); dat(0x35);
  cmd(0xBB); dat(0x19);
  cmd(0xC0); dat(0x2C);
  cmd(0xC2); dat(0x01);
  cmd(0xC3); dat(0x12);
  cmd(0xC4); dat(0x20);
  cmd(0xC6); dat(0x0F);
  cmd(0xD0); dat(0xA4); dat(0xA1);
  cmd(0xE0); dat(0xD0); dat(0x04); dat(0x0D); dat(0x11); dat(0x13); dat(0x2B); dat(0x3F);
             dat(0x54); dat(0x4C); dat(0x18); dat(0x0D); dat(0x0B); dat(0x1F); dat(0x23);
  cmd(0xE1); dat(0xD0); dat(0x04); dat(0x0C); dat(0x11); dat(0x13); dat(0x2C); dat(0x3F);
             dat(0x44); dat(0x51); dat(0x2F); dat(0x1F); dat(0x1F); dat(0x20); dat(0x23);
  cmd(0x21);
  cmd(0x13);
  cmd(0x29); delay(20);
}

// ---------------- sensors ----------------
#define MAX_ADDR 0x48
#define LSB_C    0.00390625f
static const uint8_t kOwPins[] = {17, 18, 2, 3};
static const uint8_t kOwCnt = 4;

bool  maxOk = false;
float maxT = NAN;
int   owPin = -1;
float dsT = NAN;
uint8_t i2cN = 0;

static inline void owLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(uint8_t p) { pinMode(p, INPUT_PULLUP); }
bool owReset(uint8_t p) {
  owLow(p); delayMicroseconds(480);
  owRel(p); delayMicroseconds(70);
  bool pres = (digitalRead(p) == LOW);
  delayMicroseconds(410);
  return pres;
}
void owWriteBit(uint8_t p, uint8_t b) {
  if (b) { owLow(p); delayMicroseconds(6);  owRel(p); delayMicroseconds(64); }
  else   { owLow(p); delayMicroseconds(60); owRel(p); delayMicroseconds(10); }
}
uint8_t owReadBit(uint8_t p) {
  owLow(p); delayMicroseconds(3); owRel(p); delayMicroseconds(10);
  uint8_t b = (digitalRead(p) == HIGH) ? 1 : 0;
  delayMicroseconds(53);
  return b;
}
void owWriteByte(uint8_t p, uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(p, v & 1); v >>= 1; } }
uint8_t owReadByte(uint8_t p) { uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (owReadBit(p) << i); return v; }

bool dsRead(uint8_t p, float *out) {
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC); owWriteByte(p, 0x44);
  delay(750);
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC); owWriteByte(p, 0xBE);
  uint8_t b[9];
  for (uint8_t i = 0; i < 9; i++) b[i] = owReadByte(p);
  int16_t raw = (int16_t)((b[1] << 8) | b[0]);
  if (raw == -2048 || raw == 0x0550) return false;
  *out = raw * 0.0625f;
  return true;
}

void rescan() {
  Wire.begin(11, 10);
  Wire.setClock(100000);
  delay(10);
  i2cN = 0; bool f48 = false;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { i2cN++; if (a == MAX_ADDR) f48 = true; }
  }
  maxOk = f48;
  if (owPin < 0 || !owReset(owPin)) {
    owPin = -1;
    for (uint8_t i = 0; i < kOwCnt; i++) if (owReset(kOwPins[i])) { owPin = kOwPins[i]; break; }
  }
  Serial.printf("[P] i2c=%u 0x48=%d owPin=%d\n", i2cN, maxOk, owPin);
}

bool readMax(float *out) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX_ADDR, 2) != 2) return false;
  uint16_t hi = Wire.read(), lo = Wire.read();
  *out = (int16_t)((hi << 8) | lo) * LSB_C;
  return true;
}

// ---------------- UI ----------------
void drawFrame() {
  fillScreen(C_BLACK);
  fillRect(0, 0, W, 36, 0x18E3);              // title bar
  drawText(10, 9, "T0.3 OK", 3, C_GREEN);
  fillRect(0, 36, W, 2, C_DKGREY);

  drawText(10, 46, "MAX30205", 2, C_GREY);
  drawText(10, 104, "DS18B20", 2, C_GREY);

  fillRect(0, 176, W, 2, C_DKGREY);
  drawText(10, 186, "I2C 0X48", 2, C_GREY);
  drawText(10, 210, "1WIRE", 2, C_GREY);
  drawText(10, 234, "UPTIME", 2, C_GREY);
}

void drawValue(int y, float v, bool ok, uint16_t col) {
  char buf[12];
  if (!ok || isnan(v)) snprintf(buf, sizeof(buf), "NO");
  else                 snprintf(buf, sizeof(buf), "%.1f", v);
  fillRect(110, y, 120, 26, C_BLACK);
  drawText(114, y + 2, buf, 3, col);
  if (ok && !isnan(v)) drawText(114 + textWidth(buf, 3) + 4, y + 2, "C", 3, C_GREY);
}

void drawStatus() {
  char buf[16];

  drawValue(62, maxT, maxOk, maxOk ? C_GREEN : C_RED);

  drawValue(120, dsT, owPin >= 0, owPin >= 0 ? C_GREEN : C_RED);

  snprintf(buf, sizeof(buf), "%u", i2cN);
  fillRect(110, 186, 120, 20, C_BLACK);
  drawText(114, 186, buf, 2, C_CYAN);

  snprintf(buf, sizeof(buf), "GPIO%d", owPin);
  fillRect(110, 210, 120, 20, C_BLACK);
  drawText(114, 210, owPin >= 0 ? buf : "NONE", 2, owPin >= 0 ? C_CYAN : C_RED);

  snprintf(buf, sizeof(buf), "%lus", (unsigned long)(millis() / 1000));
  fillRect(110, 234, 120, 20, C_BLACK);
  drawText(114, 234, buf, 2, C_WHITE);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.printf("\n=== DASHBOARD BOOT  reset_reason=%d ===\n", (int)esp_reset_reason());

  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  st7789Init();
  Serial.println("display init done");

  fillScreen(C_RED);   delay(200);
  fillScreen(C_GREEN); delay(200);
  fillScreen(C_BLUE);  delay(200);

  drawFrame();
  rescan();
  drawStatus();
  Serial.println("--- loop ---");
}

void loop() {
  static uint32_t n = 0;
  n++;

  if (maxOk) { float t; if (readMax(&t)) maxT = t; else maxT = NAN; }
  if (owPin >= 0) { float t; dsT = dsRead(owPin, &t) ? t : NAN; }

  Serial.printf("[%5lus] max=%d %.2f  ds=%d %.2f  i2c=%u\n",
                (unsigned long)(millis() / 1000), maxOk, maxT, owPin >= 0, dsT, i2cN);

  drawStatus();
  if (n % 8 == 0) rescan();
  delay(1500);
}
