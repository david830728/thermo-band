// DS18B20 timing sweep -- finds the read-slot timing that a clone chip actually needs.
// A genuine DS18B20 ROM ID starts with 0x28. If any combination prints 28 .. we found it.
#include <Arduino.h>

#define PIN 17

static inline void owLow() { digitalWrite(PIN, LOW); pinMode(PIN, OUTPUT); }
static inline void owRel() { pinMode(PIN, INPUT_PULLUP); }

unsigned owResetTimed() {
  owLow(); delayMicroseconds(480);
  owRel();
  unsigned long t0 = micros();
  while (digitalRead(PIN) == HIGH && micros() - t0 < 200) {}
  if (digitalRead(PIN) == HIGH) { delayMicroseconds(200); return 0; }
  unsigned long tl = micros();
  while (digitalRead(PIN) == LOW && micros() - tl < 300) {}
  unsigned w = (unsigned)(micros() - tl);
  delayMicroseconds(400);
  return w;
}

// configurable slot timing
static uint8_t gLowUs = 3;      // master pulls low this long at slot start
static uint8_t gSampleUs = 13;  // sample the line at this point (from slot start)

void owWriteBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(6);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(60); owRel(); delayMicroseconds(10); }
}
uint8_t owReadBit() {
  owLow();
  delayMicroseconds(gLowUs);
  owRel();
  if (gSampleUs > gLowUs) delayMicroseconds(gSampleUs - gLowUs);
  uint8_t b = (digitalRead(PIN) == HIGH) ? 1 : 0;
  if (70 > gSampleUs) delayMicroseconds(70 - gSampleUs);
  return b;
}
void owWriteByte(uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }
uint8_t owReadByte() { uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (owReadBit() << i); return v; }

void tryReadRom(uint8_t lowUs, uint8_t sampleUs) {
  gLowUs = lowUs; gSampleUs = sampleUs;
  unsigned w = owResetTimed();
  uint8_t rom[8];
  for (int i = 0; i < 8; i++) rom[i] = 0;
  if (w) {
    owWriteByte(0x33);                 // READ ROM
    for (int i = 0; i < 8; i++) rom[i] = owReadByte();
  }
  Serial.printf("low=%2u sample=%2u pres=%3u us  ROM:", lowUs, sampleUs, w);
  for (int i = 0; i < 8; i++) Serial.printf(" %02X", rom[i]);
  bool ok = w && rom[0] == 0x28;
  Serial.println(ok ? "   <<< VALID DS18B20 !" : "");
  delay(30);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== DS18B20 TIMING SWEEP ===");
  pinMode(PIN, INPUT_PULLUP);
  delay(50);
  Serial.printf("idle=%s\n", digitalRead(PIN) ? "HIGH" : "LOW");
}

void loop() {
  static const uint8_t lows[]    = {1, 2, 3, 5};
  static const uint8_t samples[] = {10, 13, 16, 20, 24, 28, 32, 36};
  Serial.println("\n-------------------- sweep --------------------");
  for (uint8_t a = 0; a < 4; a++)
    for (uint8_t b = 0; b < 8; b++)
      tryReadRom(lows[a], samples[b]);
  Serial.println("-------------------- done --------------------");
  delay(4000);
}
