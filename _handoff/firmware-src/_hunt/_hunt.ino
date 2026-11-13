// Sensor hunter -- scans I2C in BOTH wire orders + 1-Wire with retries, beeps on success.
// Order A = SDA11/SCL10 (official), Order B = SDA10/SCL11 (catches swapped SDA/SCL wiring).
// Shows a live summary on screen; beeps (GPIO42) whenever 0x48 appears.
#include <SPI.h>
#include <Wire.h>

#define PIN_SCK 6
#define PIN_MOSI 7
#define PIN_CS 5
#define PIN_DC 4
#define PIN_RST 8
#define PIN_BL 15
#define PIN_BUZZ 42

#define W 240
#define H 280
#define YOFF 20

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_GREY  0x8410
#define C_DKGREY 0x4208
#define C_RED   0xF800
#define C_GREEN 0x07E0
#define C_CYAN  0x07FF
#define C_YELLOW 0xFFE0

struct Glyph { char c; uint8_t col[5]; };
static const Glyph kFont[] = {
  {' ', {0x00,0x00,0x00,0x00,0x00}}, {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'0', {0x3E,0x51,0x49,0x45,0x3E}}, {'1', {0x00,0x42,0x7F,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}}, {'3', {0x21,0x41,0x45,0x4B,0x31}},
  {'4', {0x18,0x14,0x12,0x7F,0x10}}, {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3C,0x4A,0x49,0x49,0x30}}, {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}}, {'9', {0x06,0x49,0x49,0x29,0x1E}},
  {'A', {0x7E,0x11,0x11,0x11,0x7E}}, {'B', {0x7F,0x49,0x49,0x49,0x36}},
  {'C', {0x3E,0x41,0x41,0x41,0x22}}, {'D', {0x7F,0x41,0x41,0x22,0x1C}},
  {'E', {0x7F,0x49,0x49,0x49,0x41}}, {'F', {0x7F,0x09,0x09,0x09,0x01}},
  {'G', {0x3E,0x41,0x49,0x49,0x7A}}, {'I', {0x00,0x41,0x7F,0x41,0x00}},
  {'K', {0x7F,0x08,0x14,0x22,0x41}}, {'N', {0x7F,0x04,0x08,0x10,0x7F}},
  {'O', {0x3E,0x41,0x41,0x41,0x3E}}, {'R', {0x7F,0x09,0x19,0x29,0x46}},
  {'S', {0x26,0x49,0x49,0x49,0x32}}, {'T', {0x01,0x01,0x7F,0x01,0x01}},
  {'U', {0x3F,0x40,0x40,0x40,0x3F}}, {'X', {0x7F,0x41,0x41,0x41,0x3E}},
  {'W', {0x3F,0x40,0x38,0x40,0x3F}}, {'?', {0x02,0x01,0x51,0x09,0x06}},
};
static const uint8_t kFontN = sizeof(kFont) / sizeof(kFont[0]);

static inline void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
static inline void dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }
static void setWindow(int x0, int y0, int x1, int y1) {
  y0 += YOFF; y1 += YOFF;
  cmd(0x2A); dat(x0 >> 8); dat(x0 & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);
  cmd(0x2B); dat(y0 >> 8); dat(y0 & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);
  cmd(0x2C);
}
static void fillRect(int x, int y, int w, int h, uint16_t c) {
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
static void drawChar(int x, int y, char ch, int s, uint16_t col) {
  const uint8_t *g = kFont[kFontN - 1].col;
  for (uint8_t i = 0; i < kFontN; i++) if (kFont[i].c == ch) { g = kFont[i].col; break; }
  for (int c = 0; c < 5; c++) for (int r = 0; r < 7; r++)
    if (g[c] & (1 << r)) fillRect(x + c * s, y + r * s, s, s, col);
}
static void drawText(int x, int y, const char *s, int sc, uint16_t col) {
  while (*s) { drawChar(x, y, *s++, sc, col); x += 6 * sc; }
}
static void st7789Init() {
  digitalWrite(PIN_RST, LOW); delay(20); digitalWrite(PIN_RST, HIGH); delay(120);
  cmd(0x01); delay(150); cmd(0x11); delay(120);
  cmd(0x3A); dat(0x55); cmd(0x36); dat(0x00);
  cmd(0xB2); dat(0x0C); dat(0x0C); dat(0x00); dat(0x33); dat(0x33);
  cmd(0xB7); dat(0x35); cmd(0xBB); dat(0x19); cmd(0xC0); dat(0x2C);
  cmd(0xC2); dat(0x01); cmd(0xC3); dat(0x12); cmd(0xC4); dat(0x20); cmd(0xC6); dat(0x0F);
  cmd(0xD0); dat(0xA4); dat(0xA1);
  cmd(0xE0); dat(0xD0); dat(0x04); dat(0x0D); dat(0x11); dat(0x13); dat(0x2B); dat(0x3F);
             dat(0x54); dat(0x4C); dat(0x18); dat(0x0D); dat(0x0B); dat(0x1F); dat(0x23);
  cmd(0xE1); dat(0xD0); dat(0x04); dat(0x0C); dat(0x11); dat(0x13); dat(0x2C); dat(0x3F);
             dat(0x44); dat(0x51); dat(0x2F); dat(0x1F); dat(0x1F); dat(0x20); dat(0x23);
  cmd(0x21); cmd(0x13); cmd(0x29); delay(20);
}

// ---------- 1-Wire ----------
static const uint8_t kOw[] = {17, 18, 2, 3};
static const uint8_t kOwN = 4;
static inline void owLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(uint8_t p) { pinMode(p, INPUT_PULLUP); }
bool owPresence(uint8_t p) {
  owLow(p); delayMicroseconds(480);
  owRel(p); delayMicroseconds(80);
  bool pres = (digitalRead(p) == LOW);
  delayMicroseconds(400);
  return pres;
}
void owWriteBit(uint8_t p, uint8_t b) {
  if (b) { owLow(p); delayMicroseconds(6);  owRel(p); delayMicroseconds(64); }
  else   { owLow(p); delayMicroseconds(60); owRel(p); delayMicroseconds(10); }
}
uint8_t owReadBit(uint8_t p) {
  owLow(p); delayMicroseconds(3); owRel(p); delayMicroseconds(12);
  uint8_t b = (digitalRead(p) == HIGH) ? 1 : 0;
  delayMicroseconds(50);
  return b;
}
void owWriteByte(uint8_t p, uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(p, v & 1); v >>= 1; } }
uint8_t owReadByte(uint8_t p) { uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (owReadBit(p) << i); return v; }
bool dsRead(uint8_t p, float *out) {
  for (uint8_t att = 0; att < 3; att++) {
    if (!owPresence(p)) { delay(20); continue; }
    owWriteByte(p, 0xCC); owWriteByte(p, 0x44);
    delay(760);
    if (!owPresence(p)) { delay(20); continue; }
    owWriteByte(p, 0xCC); owWriteByte(p, 0xBE);
    uint8_t b[9];
    for (uint8_t i = 0; i < 9; i++) b[i] = owReadByte(p);
    int16_t raw = (int16_t)((b[1] << 8) | b[0]);
    if (raw == -2048 || raw == 0x0550) { delay(20); continue; }
    *out = raw * 0.0625f;
    return true;
  }
  return false;
}

// ---------- state ----------
uint8_t cntA = 0, cntB = 0;
bool hasA = false, hasB = false;
int  owPin = -1, owMiss = 0;
float dsT = NAN;
bool beeped = false;
const char *bestOrder = "-";

void beep(int times) {
  for (int i = 0; i < times; i++) { tone(PIN_BUZZ, 2500, 90); delay(140); }
}

void scanOrder(uint8_t sda, uint8_t scl, uint8_t *cnt, bool *has48) {
  Wire.end(); delay(2);
  Wire.begin(sda, scl); Wire.setClock(100000); delay(3);
  *cnt = 0; *has48 = false;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { (*cnt)++; if (a == 0x48) *has48 = true; }
  }
}

bool readMax(float *out) {
  Wire.beginTransmission(0x48); Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(0x48, 2) != 2) return false;
  uint16_t hi = Wire.read(), lo = Wire.read();
  *out = (int16_t)((hi << 8) | lo) * 0.00390625f;
  return true;
}

void showLine(int y, const char *label, const char *val, uint16_t vc) {
  fillRect(0, y, W, 24, C_BLACK);
  drawText(8, y + 2, label, 2, C_GREY);
  drawText(130, y + 2, val, 2, vc);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== HUNTER BOOT ===");
  pinMode(PIN_BUZZ, OUTPUT);
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  st7789Init();

  fillRect(0, 0, W, H, C_BLACK);
  fillRect(0, 0, W, 30, 0x18E3);
  drawText(8, 6, "SENSOR HUNT", 2, C_YELLOW);
  showLine(40, "I2C A", "--", C_GREY);
  showLine(66, "I2C B", "--", C_GREY);
  showLine(92, "48A", "--", C_GREY);
  showLine(118, "48B", "--", C_GREY);
  showLine(144, "1W", "--", C_GREY);
  showLine(170, "TEMP", "--", C_GREY);
  showLine(196, "BEST", "-", C_GREY);
  Serial.println("hunting...");
}

void loop() {
  static uint32_t up = 0;
  char buf[16];

  scanOrder(11, 10, &cntA, &hasA);
  scanOrder(10, 11, &cntB, &hasB);

  // restore official bus for the MAX30205 reader
  Wire.end(); delay(2); Wire.begin(11, 10); Wire.setClock(100000); delay(2);

  if ((hasA || hasB) && !beeped) {
    beeped = true;
    bestOrder = hasA ? "SDA11 SCL10" : "SDA10 SCL11";
    Serial.printf("*** 0x48 FOUND on %s ***\n", bestOrder);
    beep(3);
  }
  if (!hasA && !hasB) beeped = false;

  // 1-Wire
  if (owPin < 0) {
    for (uint8_t i = 0; i < kOwN; i++) if (owPresence(kOw[i])) { owPin = kOw[i]; beep(1); break; }
  } else {
    if (owPresence(owPin)) owMiss = 0;
    else if (++owMiss >= 3) { owPin = -1; owMiss = 0; }
  }
  float t = NAN;
  if (owPin >= 0 && dsRead(owPin, &t)) dsT = t;

  // screen
  snprintf(buf, sizeof(buf), "%u", cntA);  showLine(40, "I2C A", buf, cntA >= 5 ? C_GREEN : C_CYAN);
  snprintf(buf, sizeof(buf), "%u", cntB);  showLine(66, "I2C B", buf, cntB >= 5 ? C_GREEN : C_CYAN);
  showLine(92,  "48A", hasA ? "YES" : "NO", hasA ? C_GREEN : C_RED);
  showLine(118, "48B", hasB ? "YES" : "NO", hasB ? C_GREEN : C_RED);
  if (owPin >= 0) { snprintf(buf, sizeof(buf), "G%d", owPin); showLine(144, "1W", buf, C_GREEN); }
  else            showLine(144, "1W", "NONE", C_RED);
  if (!isnan(dsT)) { snprintf(buf, sizeof(buf), "%.1fC", dsT); showLine(170, "TEMP", buf, C_YELLOW); }
  else             showLine(170, "TEMP", "--", C_GREY);
  showLine(196, "BEST", hasA || hasB ? bestOrder : "-", C_GREEN);

  if (++up % 5 == 0)
    Serial.printf("[H] A=%u(48=%d) B=%u(48=%d) ow=%d temp=%.2f\n",
                  cntA, hasA, cntB, hasB, owPin, dsT);
  delay(900);
}
