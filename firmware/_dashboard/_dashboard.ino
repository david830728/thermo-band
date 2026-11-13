// T0.3 + sensor dashboard -- Waveshare ESP32-S3-Touch-LCD-1.69
// screen: T0.3 color test, then live sensor status
// serial: same info + full port scan every 20s
#include <SPI.h>
#include <Wire.h>
#include <TFT_eSPI.h>

#define W 240
#define H 280
#define MAX_ADDR 0x48
#define LSB_C    0.00390625f

static const uint8_t kOwPins[] = {17, 18, 2, 3, 43, 44};
static const uint8_t kOwCnt    = sizeof(kOwPins) / sizeof(kOwPins[0]);

TFT_eSPI tft = TFT_eSPI();

int   owPin   = -1;
bool  maxOk   = false;
float maxT    = NAN;
float dsT     = NAN;
uint8_t i2cN  = 0;

// ---------- 1-Wire bit-bang (internal pull-up) ----------
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
  owWriteByte(p, 0xCC); owWriteByte(p, 0x44);      // skip rom, convert
  delay(750);
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC); owWriteByte(p, 0xBE);      // skip rom, read scratchpad
  uint8_t b[9];
  for (uint8_t i = 0; i < 9; i++) b[i] = owReadByte(p);
  int16_t raw = (int16_t)((b[1] << 8) | b[0]);
  if (raw == -2048 || raw == 0x0550) return false; // 0x0550 = power-on default, not a reading
  *out = raw * 0.0625f;
  return true;
}

// ---------- I2C ----------
void scanBus() {
  Wire.begin(11, 10);
  Wire.setClock(100000);
  delay(10);
  i2cN = 0;
  bool found48 = false;
  Serial.println("--- i2c scan (SDA=11 SCL=10) ---");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  found 0x%02X\n", a);
      i2cN++;
      if (a == MAX_ADDR) found48 = true;
    }
  }
  maxOk = found48;
  Serial.printf("--- %u device(s), 0x48=%s ---\n", i2cN, found48 ? "YES" : "no");
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

// ---------- screen ----------
void drawStatic() {
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, W, H, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("T0.3 OK", 10, 10, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("1.69\" 240x280", 10, 42, 2);
  tft.drawFastHLine(0, 62, W, TFT_DARKGREY);
  tft.drawString("MAX30205 @0x48", 10, 72, 2);
  tft.drawString("DS18B20 1-Wire", 10, 100, 2);
  tft.drawString("I2C devices", 10, 128, 2);
  tft.drawFastHLine(0, 152, W, TFT_DARKGREY);
  tft.drawString("i2c: 0x15 0x51 0x6B", 10, 162, 2);
  tft.drawString("pin SDA=11 SCL=10", 10, 180, 2);
}

void updateScreen() {
  char buf[32];
  tft.fillRect(120, 68, 110, 24, TFT_BLACK);
  tft.fillRect(120, 96, 110, 24, TFT_BLACK);
  tft.fillRect(120, 124, 110, 24, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  if (maxOk && !isnan(maxT)) {
    snprintf(buf, sizeof(buf), "%.2f C", maxT);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
  } else {
    snprintf(buf, sizeof(buf), maxOk ? "read err" : "no device");
    tft.setTextColor(TFT_RED, TFT_BLACK);
  }
  tft.drawString(buf, 122, 72, 2);

  if (owPin >= 0 && !isnan(dsT)) {
    snprintf(buf, sizeof(buf), "%.2f C", dsT);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
  } else {
    snprintf(buf, sizeof(buf), owPin >= 0 ? "read err" : "no device");
    tft.setTextColor(TFT_RED, TFT_BLACK);
  }
  tft.drawString(buf, 122, 100, 2);

  snprintf(buf, sizeof(buf), "%u", i2cN);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(buf, 122, 128, 2);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== BOOT OK (dashboard) ===");
  Serial.printf("chip=%s flash=%dMB psram=%dMB\n", ESP.getChipModel(),
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / (1024 * 1024));

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(0);

  tft.fillScreen(TFT_RED);   delay(260);
  tft.fillScreen(TFT_GREEN); delay(260);
  tft.fillScreen(TFT_BLUE);  delay(260);
  Serial.println("display init done");

  drawStatic();

  scanBus();
  for (uint8_t i = 0; i < kOwCnt; i++) {
    if (owReset(kOwPins[i])) { owPin = kOwPins[i]; break; }
  }
  Serial.printf("1-Wire: %s\n", owPin >= 0 ? "found" : "none");
  if (owPin >= 0) Serial.printf("  on GPIO%d\n", owPin);

  updateScreen();
  Serial.println("--- loop: sensors every 2s, rescan every 20s ---");
}

void loop() {
  static uint32_t n = 0;
  n++;
  unsigned long up = millis() / 1000;

  if (maxOk) {
    if (!readMax(&maxT)) maxT = NAN;
  }
  if (owPin >= 0) {
    float t;
    dsT = dsRead(owPin, &t) ? t : NAN;
  }

  Serial.printf("[%5lus] max30205=%s%s  ds18b20=%s%s\n", up,
                maxOk ? "" : "NO ",
                (maxOk && !isnan(maxT)) ? String(maxT, 2).c_str() : "err",
                owPin >= 0 ? "" : "NO ",
                (owPin >= 0 && !isnan(dsT)) ? String(dsT, 2).c_str() : "err");

  updateScreen();

  if (n % 10 == 0) {            // rescan every 20s
    scanBus();
    if (owPin < 0) {
      for (uint8_t i = 0; i < kOwCnt; i++) {
        if (owReset(kOwPins[i])) { owPin = kOwPins[i]; Serial.printf("1-Wire found on GPIO%d\n", owPin); break; }
      }
    }
    updateScreen();
  }
  delay(1200);
}
