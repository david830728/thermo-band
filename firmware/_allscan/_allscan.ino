// Full-port scanner -- Waveshare ESP32-S3-Touch-LCD-1.69
// Purpose: find out WHERE the external sensor is actually wired, without guessing.
//   * 1-Wire (DS18B20) presence check on GPIO 17/18/2/3/43/44 using the ESP32 INTERNAL pull-up
//     (works even if the module has no 4.7k resistor)
//   * I2C scan on several pin pairs (11/10, 17/18, 2/3, 43/44 and swapped variants)
//   * rescans every 20 s, so you can re-plug wires and watch the result appear
#include <Wire.h>

static const uint8_t kOwPins[] = {17, 18, 2, 3, 43, 44};
static const uint8_t kOwCnt = sizeof(kOwPins) / sizeof(kOwPins[0]);

static const uint8_t kPairs[][2] = {
  {11, 10}, {10, 11},   // official board I2C
  {17, 18}, {18, 17},
  {2, 3},   {3, 2},
  {43, 44}, {44, 43}
};
static const uint8_t kPairCnt = sizeof(kPairs) / sizeof(kPairs[0]);

int  foundOwPin = -1;
int  foundMaxPair = -1;

// ---------------- 1-Wire bit-bang with internal pull-up ----------------
static inline void owLow(uint8_t p)  { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(uint8_t p)  { pinMode(p, INPUT_PULLUP); }

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
  owLow(p); delayMicroseconds(3);
  owRel(p); delayMicroseconds(10);
  uint8_t b = (digitalRead(p) == HIGH) ? 1 : 0;
  delayMicroseconds(53);
  return b;
}
void owWriteByte(uint8_t p, uint8_t v) {
  for (uint8_t i = 0; i < 8; i++) { owWriteBit(p, v & 1); v >>= 1; }
}
uint8_t owReadByte(uint8_t p) {
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) { v |= (owReadBit(p) << i); }
  return v;
}

bool dsConvert(uint8_t p) {
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC);
  owWriteByte(p, 0x44);
  return true;
}
bool dsReadC(uint8_t p, float *out) {
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC);
  owWriteByte(p, 0xBE);
  uint8_t b[9];
  for (uint8_t i = 0; i < 9; i++) b[i] = owReadByte(p);
  int16_t raw = (int16_t)((b[1] << 8) | b[0]);
  if (raw == -2048) return false;           // not a real reading
  *out = raw * 0.0625f;
  return true;
}

// ---------------- scans ----------------
void scanOneWire() {
  Serial.print("--- 1-Wire presence (internal pull-up) ---\n");
  foundOwPin = -1;
  for (uint8_t i = 0; i < kOwCnt; i++) {
    uint8_t p = kOwPins[i];
    bool pres = owReset(p);
    Serial.printf("  GPIO%-2u : %s\n", p, pres ? "DS18B20 PRESENT" : "-");
    if (pres && foundOwPin < 0) foundOwPin = p;
  }
  if (foundOwPin < 0) Serial.println("  !! no 1-Wire device on any of these pins");
}

void i2cScanPair(uint8_t sda, uint8_t scl, uint8_t *foundMax) {
  Wire.end();
  delay(5);
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  delay(20);
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("    SDA=%u SCL=%u : found 0x%02X", sda, scl, a);
      if (a == 0x15) Serial.print(" (touch)");
      if (a == 0x51) Serial.print(" (RTC)");
      if (a == 0x6B) Serial.print(" (IMU)");
      if (a == 0x48) { Serial.print(" (*** MAX30205 ***)"); if (foundMax) *foundMax = 1; }
      Serial.println();
      n++;
    }
  }
  if (n == 0) Serial.printf("    SDA=%u SCL=%u : nothing\n", sda, scl);
}

void scanI2CAll() {
  Serial.println("--- I2C scan on candidate pin pairs ---");
  uint8_t hit = 0;
  for (uint8_t i = 0; i < kPairCnt; i++) {
    i2cScanPair(kPairs[i][0], kPairs[i][1], &hit);
  }
  // restore official bus for the MAX30205 reader
  Wire.end(); delay(5);
  Wire.begin(11, 10); Wire.setClock(100000); delay(10);
  if (!hit) Serial.println("  !! no device ACKed 0x48 on any pair");
}

void fullScan() {
  Serial.println();
  Serial.println("=== FULL SCAN ===");
  scanOneWire();
  scanI2CAll();
  if (foundOwPin >= 0) Serial.printf(">>> DS18B20 pin = GPIO%d\n", foundOwPin);
  Serial.println("=== END SCAN ===");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== BOOT OK (full-port scanner) ===");
  Serial.printf("chip=%s flash=%dMB psram=%dMB\n", ESP.getChipModel(),
                ESP.getFlashChipSize() / (1024 * 1024), ESP.getPsramSize() / (1024 * 1024));
  fullScan();
  Serial.println("--- loop: rescan every 20s (re-plug wires and watch) ---");
}

void loop() {
  static uint32_t n = 0;
  n++;
  unsigned long up = millis() / 1000;

  if (foundOwPin >= 0) {
    if (dsConvert(foundOwPin)) {
      delay(750);
      float t;
      if (dsReadC(foundOwPin, &t)) Serial.printf("[%5lus] DS18B20(GPIO%d) = %.2f C\n", up, foundOwPin, t);
      else                          Serial.printf("[%5lus] DS18B20(GPIO%d) read failed\n", up, foundOwPin);
    }
  }

  if (n % 10 == 0) fullScan();
  delay(1800);
}
