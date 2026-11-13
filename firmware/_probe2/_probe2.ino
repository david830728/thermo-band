// _probe2 · DS18B20 深度诊断
// 核心验证：
//   1) 读 ROM（0x33）——真 DS18B20 会返回 family=0x28 + 48bit 序列号 + CRC 通过
//   2) 普通上拉转换 vs 强上拉（把数据线主动驱动高）转换，对比 scratchpad
//   3) 扫描 _ui 里全部候选引脚，确认传感器到底在哪个脚上
#include <Arduino.h>

static const uint8_t kPins[] = {17, 18, 2, 3};
static const uint8_t kPinCnt = 4;

static inline void owLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(uint8_t p) { pinMode(p, INPUT_PULLUP); }
static inline void owStrong(uint8_t p) { digitalWrite(p, HIGH); pinMode(p, OUTPUT); }  // 强上拉

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

uint8_t crc8(const uint8_t *d, uint8_t n) {
  uint8_t crc = 0;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t x = crc ^ d[i];
    for (uint8_t k = 0; k < 8; k++) x = (x & 1) ? ((x >> 1) ^ 0x8C) : (x >> 1);
    crc = x;
  }
  return crc;
}

void dumpBytes(const char *tag, const uint8_t *b, uint8_t n) {
  Serial.printf("  %s=", tag);
  for (uint8_t i = 0; i < n; i++) Serial.printf("%02X ", b[i]);
  Serial.println();
}

// 读 ROM：8 字节（family + 6 序列号 + CRC）
bool readRom(uint8_t p, uint8_t *rom) {
  if (!owReset(p)) return false;
  owWriteByte(p, 0x33);
  for (uint8_t i = 0; i < 8; i++) rom[i] = owReadByte(p);
  return true;
}

// 读 scratchpad；strong=true 时转换期间由 MCU 强上拉驱动数据线
bool readScratch(uint8_t p, uint8_t *sp, bool strong) {
  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC); owWriteByte(p, 0x44);   // skip ROM + convert T
  if (strong) { owStrong(p); delay(800); owRel(p); }
  else        { delay(800); }

  if (!owReset(p)) return false;
  owWriteByte(p, 0xCC); owWriteByte(p, 0xBE);
  for (uint8_t i = 0; i < 9; i++) sp[i] = owReadByte(p);
  return true;
}

void testPin(uint8_t p) {
  pinMode(p, INPUT_PULLUP);
  delay(5);
  Serial.printf("\n--- GPIO%d --- idle=%d\n", p, digitalRead(p));
  if (!owReset(p)) { Serial.println("  presence=0  (无器件，跳过)"); return; }
  Serial.println("  presence=1  (有器件应答)");

  uint8_t rom[8] = {0};
  if (readRom(p, rom)) {
    dumpBytes("ROM", rom, 8);
    Serial.printf("  family=0x%02X (%s)  crc=%s\n", rom[0],
                  rom[0] == 0x28 ? "DS18B20 / DS18B20+ ✓" : "非 DS18B20",
                  crc8(rom, 7) == rom[7] ? "OK" : "BAD");
  } else {
    Serial.println("  读 ROM 失败");
  }

  uint8_t sp[9] = {0};
  if (readScratch(p, sp, false)) {
    dumpBytes("普通上拉 scratchpad", sp, 9);
    Serial.printf("  raw=0x%04X T=%.4f C  crc=%s\n",
                  (uint16_t)((sp[1] << 8) | sp[0]),
                  (int16_t)((sp[1] << 8) | sp[0]) * 0.0625f,
                  crc8(sp, 8) == sp[8] ? "OK" : "BAD");
  } else Serial.println("  普通上拉：转换期间复位失败");

  uint8_t sp2[9] = {0};
  if (readScratch(p, sp2, true)) {
    dumpBytes("强上拉 scratchpad", sp2, 9);
    Serial.printf("  raw=0x%04X T=%.4f C  crc=%s\n",
                  (uint16_t)((sp2[1] << 8) | sp2[0]),
                  (int16_t)((sp2[1] << 8) | sp2[0]) * 0.0625f,
                  crc8(sp2, 8) == sp2[8] ? "OK" : "BAD");
  } else Serial.println("  强上拉：转换期间复位失败");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== PROBE2 · DS18B20 深度诊断 ===");
  Serial.printf("chip=%s flash=%dMB psram=%dMB heap=%u\n",
                ESP.getChipModel(),
                (int)(ESP.getFlashChipSize() / 1048576),
                (int)(ESP.getPsramSize() / 1048576),
                (unsigned)ESP.getFreeHeap());
}

void loop() {
  for (uint8_t i = 0; i < kPinCnt; i++) testPin(kPins[i]);
  Serial.println("\n=== 全部引脚扫完，8s 后重来 ===\n");
  delay(8000);
}
