// DS18B20 raw diagnostic -- serial only, no display.
// Prints: presence pulse width, ROM ID (0x33), scratchpad (0xBE) as raw hex.
// Uses strong push-pull pull-up during temperature conversion (rescues weak/parasitic setups).
#include <Arduino.h>

#define PIN 17

static inline void owLow() { digitalWrite(PIN, LOW); pinMode(PIN, OUTPUT); }
static inline void owRel() { pinMode(PIN, INPUT_PULLUP); }

// returns presence pulse width in us, 0 = no device
unsigned owResetTimed() {
  owLow();
  delayMicroseconds(480);
  owRel();
  unsigned long t0 = micros();
  // wait up to 200us for the sensor to pull the line low
  while (digitalRead(PIN) == HIGH && micros() - t0 < 200) {}
  if (digitalRead(PIN) == HIGH) { delayMicroseconds(200); return 0; }
  unsigned long tl = micros();
  while (digitalRead(PIN) == LOW && micros() - tl < 300) {}
  unsigned w = (unsigned)(micros() - tl);
  delayMicroseconds(400);
  return w;
}

void owWriteBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(6);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(60); owRel(); delayMicroseconds(10); }
}
uint8_t owReadBit() {
  owLow(); delayMicroseconds(3);
  owRel(); delayMicroseconds(10);
  uint8_t b = (digitalRead(PIN) == HIGH) ? 1 : 0;
  delayMicroseconds(55);
  return b;
}
void owWriteByte(uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }
uint8_t owReadByte() { uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (owReadBit() << i); return v; }

void dump(const char *tag, uint8_t *b, int n) {
  Serial.printf("  %s:", tag);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", b[i]);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== DS18B20 RAW DIAG ===");
  pinMode(PIN, INPUT_PULLUP);
  delay(50);
  Serial.printf("idle level on GPIO%d = %s\n", PIN, digitalRead(PIN) ? "HIGH (good)" : "LOW (line stuck!)");
}

void loop() {
  Serial.println("\n--------------------");
  unsigned w = owResetTimed();
  Serial.printf("presence: %s (%u us)\n", w ? "YES" : "NO", w);
  if (!w) { delay(2000); return; }

  uint8_t rom[8];
  if (owResetTimed()) {
    owWriteByte(0x33);                 // READ ROM (single device only)
    for (int i = 0; i < 8; i++) rom[i] = owReadByte();
    dump("ROM", rom, 8);
  } else Serial.println("  ROM read: no presence on 2nd reset");

  if (owResetTimed()) {
    owWriteByte(0xCC); owWriteByte(0x44);   // SKIP ROM + CONVERT
    pinMode(PIN, OUTPUT); digitalWrite(PIN, HIGH);   // strong pull-up during conversion
    delay(760);
    pinMode(PIN, INPUT_PULLUP);
    Serial.println("  convert done (strong pull-up)");
  }

  if (owResetTimed()) {
    owWriteByte(0xCC); owWriteByte(0xBE);   // SKIP ROM + READ SCRATCHPAD
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) sp[i] = owReadByte();
    dump("SCRATCH", sp, 9);
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    Serial.printf("  raw=0x%04X  temp=%.4f C\n", (uint16_t)raw, raw * 0.0625f);
  } else Serial.println("  scratchpad read: no presence on 3rd reset");

  delay(1500);
}
