// 1-Wire waveform tracer -- prints every transition of the data line with timestamps (us).
// Purpose: tell apart "a real device is pulling the line low" from "a capacitor is draining".
#include <Arduino.h>

#define PIN 17

static inline void owLow() { digitalWrite(PIN, LOW); pinMode(PIN, OUTPUT); }
static inline void owRel() { pinMode(PIN, INPUT_PULLUP); }

// record every edge for totalUs, print as "@t us -> level"
void trace(const char *tag, uint16_t totalUs) {
  uint8_t prev = digitalRead(PIN);
  uint32_t t0 = micros();
  Serial.printf("  %-22s start=%d", tag, prev);
  while (micros() - t0 < totalUs) {
    uint8_t now = digitalRead(PIN);
    if (now != prev) {
      Serial.printf("  @%lu->%d", (unsigned long)(micros() - t0), now);
      prev = now;
    }
  }
  Serial.printf("   [end=%d]\n", prev);
}

bool resetPulse(unsigned long lowUs) {
  owLow(); delayMicroseconds(lowUs); owRel();
  delayMicroseconds(70);
  bool p = (digitalRead(PIN) == LOW);
  delayMicroseconds(410);
  return p;
}
void writeBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(6);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(60); owRel(); delayMicroseconds(10); }
}
void writeByte(uint8_t v) { for (uint8_t i = 0; i < 8; i++) { writeBit(v & 1); v >>= 1; } }
uint8_t readBitSlot() {
  owLow(); delayMicroseconds(3); owRel(); delayMicroseconds(10);
  uint8_t b = (digitalRead(PIN) == HIGH) ? 1 : 0;
  delayMicroseconds(55);
  return b;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== 1-WIRE WAVE TRACER ===\n");

  // 1) static level tests
  pinMode(PIN, INPUT);
  delay(5);  Serial.printf("pin float     = %d  (1 => external pull-up really present)\n", digitalRead(PIN));
  pinMode(PIN, INPUT_PULLUP);
  delay(5);  Serial.printf("pin pullup    = %d\n", digitalRead(PIN));
  pinMode(PIN, INPUT_PULLDOWN);
  delay(5);  Serial.printf("pin pulldown  = %d  (0 => pin can be pulled low, not shorted to 3V3)\n", digitalRead(PIN));
  pinMode(PIN, INPUT_PULLUP);
  delay(50);
}

void loop() {
  Serial.println("\n----------------------------");

  owLow(); delayMicroseconds(480); owRel();
  trace("A reset480 release", 400);

  owLow(); delayMicroseconds(1000); owRel();
  trace("B reset1000 release", 400);

  // C: after a normal reset + READ ROM, look at the very first read slot
  if (resetPulse(480)) {
    writeByte(0x33);
    owLow(); delayMicroseconds(3); owRel();
    trace("C slot after 0x33", 70);
    uint8_t rom[8];
    for (int i = 0; i < 8; i++) rom[i] = readBitSlot();
    Serial.print("  ROM:");
    for (int i = 0; i < 8; i++) Serial.printf(" %02X", rom[i]);
    Serial.println();
  } else Serial.println("  C: no presence");

  // D: same but with SKIP ROM + CONVERT, then look at a slot
  if (resetPulse(480)) {
    writeByte(0xCC);
    owLow(); delayMicroseconds(3); owRel();
    trace("D slot after 0xCC", 70);
  } else Serial.println("  D: no presence");

  delay(3000);
}
