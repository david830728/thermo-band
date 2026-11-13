// Orientation finder -- cycles the 4 possible MADCTL rotations, 4s each, forever.
// In the CORRECT rotation you will see: full white border on all 4 sides,
// a triangle arrow pointing UP at the top, and an upright readable digit (1..4).
// Tell me that digit and I lock the rotation in.
#include <SPI.h>

#define PIN_SCK   6
#define PIN_MOSI  7
#define PIN_CS    5
#define PIN_DC    4
#define PIN_RST   8
#define PIN_BL   15
#define W 240
#define H 280

static const uint8_t kSeg[10] = {
  0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
  0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111
};

static inline void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
static inline void dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }

static void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  cmd(0x2A); dat(x0 >> 8); dat(x0 & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);
  cmd(0x2B); dat(y0 >> 8); dat(y0 & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);
  cmd(0x2C);
}
static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  digitalWrite(PIN_CS, HIGH);
}

static void drawDigit(int x, int y, int w, int h, int t, uint16_t col) {
  uint8_t m = kSeg[t % 10];
  int hh = h / 2;
  if (m & 0x01) fillRect(x, y, w, 5, col);
  if (m & 0x02) fillRect(x + w - 5, y, 5, hh, col);
  if (m & 0x04) fillRect(x + w - 5, y + hh, 5, hh, col);
  if (m & 0x08) fillRect(x, y + h - 5, w, 5, col);
  if (m & 0x10) fillRect(x, y + hh, 5, hh, col);
  if (m & 0x20) fillRect(x, y, 5, hh, col);
  if (m & 0x40) fillRect(x, y + hh - 3, w, 6, col);
}

static void st7789Init() {
  digitalWrite(PIN_RST, LOW); delay(20);
  digitalWrite(PIN_RST, HIGH); delay(120);
  cmd(0x01); delay(150);
  cmd(0x11); delay(120);
  cmd(0x3A); dat(0x55);
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

// rotation table: madctl, x offset, y offset
static const uint8_t  kMad[4] = {0x00, 0x60, 0xC0, 0xA0};
static const uint16_t kXoff[4] = {0, 0, 0, 0};
static const uint16_t kYoff[4] = {20, 0, 20, 0};

static void drawPattern(int idx) {
  cmd(0x36); dat(kMad[idx]);

  // clear whole RAM generously
  setWindow(0, 0, 239, 319);
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  for (uint32_t i = 0; i < 240UL * 320UL; i++) { SPI.transfer(0x00); SPI.transfer(0x00); }
  digitalWrite(PIN_CS, HIGH);

  uint16_t y0 = kYoff[idx], x0 = kXoff[idx];

  // border on all four sides of the 240x280 logical area
  fillRect(x0, y0, W, 4, 0xFFFF);
  fillRect(x0, y0 + H - 4, W, 4, 0xFFFF);
  fillRect(x0, y0, 4, H, 0xFFFF);
  fillRect(x0 + W - 4, y0, 4, H, 0xFFFF);

  // upward triangle at top-centre (clear orientation marker)
  int cx = x0 + W / 2;
  for (int i = 0; i < 40; i += 8) {
    fillRect(cx - (40 - i) / 2, y0 + 20 + i, (40 - i) / 2 * 2, 8, 0xF800);
  }

  // rotation index as a big digit, bottom-centre
  drawDigit(cx - 30, y0 + 150, 60, 100, idx + 1, 0x07FF);

  // reference bar: left = red, right = blue  (mirror check)
  fillRect(x0 + 10, y0 + 100, 40, 20, 0xF800);
  fillRect(x0 + W - 50, y0 + 100, 40, 20, 0x001F);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== ROT TEST ===");
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  st7789Init();
  Serial.println("init done, cycling rotations");
}

void loop() {
  for (int i = 0; i < 4; i++) {
    Serial.printf(">>> rotation %d  madctl=0x%02X yoff=%u\n", i + 1, kMad[i], kYoff[i]);
    drawPattern(i);
    delay(5000);
  }
}
