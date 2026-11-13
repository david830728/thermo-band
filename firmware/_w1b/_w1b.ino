// _w1b · 1-Wire 时序矩阵扫描 + 自动读温度
//
// 背景（_w1 已确认的事实）：
//   GPIO17 上 480us 复位 → +42us 出现 115us 低电平 → 教科书级 presence
//   120us/48us 复位不响应 → 器件要标准速度复位
//   但 READ ROM (0x33) 读回 FF×8 → 读时隙采样点太晚
//
// 本固件：5 种拉低时长 × 9 种采样点 = 45 组矩阵扫描，
// 找到能读出合法 ROM(CRC 通过且非全 0/全 F) 的那一组，然后自动读温度。

#include <Arduino.h>

#define PIN 17

static uint8_t gLowUs = 3;
static uint8_t gSampleUs = 12;

static inline void owLow() { digitalWrite(PIN, LOW); pinMode(PIN, OUTPUT); }
static inline void owRel() { pinMode(PIN, INPUT_PULLUP); }

static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0;
  while (n--) {
    uint8_t b = *d++;
    for (int i = 0; i < 8; i++) {
      uint8_t mix = (c ^ b) & 1;
      c >>= 1;
      if (mix) c ^= 0x8C;
      b >>= 1;
    }
  }
  return c;
}

// 复位，返回 presence 宽度（0 = 无应答）
static unsigned owReset() {
  owLow();
  delayMicroseconds(480);
  owRel();
  unsigned long t0 = micros();
  while (digitalRead(PIN) == HIGH && micros() - t0 < 150) {}     // presence 最晚 60us 到来
  if (digitalRead(PIN) == HIGH) { delayMicroseconds(300); return 0; }
  unsigned long tl = micros();
  while (digitalRead(PIN) == LOW && micros() - tl < 400) {}
  unsigned w = (unsigned)(micros() - tl);
  delayMicroseconds(600);                                        // tREC 恢复时间（DS18B20 要求 >=480us）
  return w;
}

static void owWriteBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(6);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(60); owRel(); delayMicroseconds(10); }
}

static uint8_t owReadBit() {
  owLow();
  delayMicroseconds(gLowUs);
  owRel();
  if (gSampleUs > gLowUs) delayMicroseconds(gSampleUs - gLowUs);
  uint8_t b = (digitalRead(PIN) == HIGH) ? 1 : 0;
  if (gSampleUs < 70) delayMicroseconds(70 - gSampleUs);
  return b;
}

static void owWriteByte(uint8_t v) { for (int i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }
static uint8_t owReadByte() { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= (owReadBit() << i); return v; }

// 用当前时序读 ROM，返回 presence 宽度
static unsigned tryRom(uint8_t *rom) {
  unsigned w = owReset();
  for (int i = 0; i < 8; i++) rom[i] = 0;
  if (!w) return 0;
  owWriteByte(0x33);            // READ ROM
  for (int i = 0; i < 8; i++) rom[i] = owReadByte();
  return w;
}

// 用当前时序读温度（跳过 ROM），返回温度（摄氏度），NAN 表示失败
static float tryTemp(uint16_t *rawOut) {
  uint8_t sp[9];
  if (!owReset()) return NAN;
  owWriteByte(0xCC);            // SKIP ROM
  owWriteByte(0x44);            // CONVERT T
  delay(800);
  if (!owReset()) return NAN;
  owWriteByte(0xCC);
  owWriteByte(0xBE);            // READ SCRATCHPAD
  for (int i = 0; i < 9; i++) sp[i] = owReadByte();
  if (rawOut) *rawOut = (uint16_t)sp[0] | ((uint16_t)sp[1] << 8);
  if (crc8(sp, 8) != sp[8]) return NAN;
  int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
  return raw * 0.0625f;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1b · 时序矩阵扫描 ===");
  pinMode(PIN, INPUT_PULLUP);
  delay(30);
  Serial.printf("空闲电平: %s\n", digitalRead(PIN) ? "HIGH(有上拉)" : "LOW");
}

void loop() {
  static const uint8_t lows[]    = {1, 2, 3};
  static const uint8_t samples[] = {3, 4, 5, 6, 8, 10, 12, 15};
  uint8_t rom[8];
  bool found = false;
  uint8_t bestLow = 0, bestSample = 0;

  Serial.println("\n---------------- sweep ----------------");
  for (uint8_t a = 0; a < 3 && !found; a++) {
    for (uint8_t b = 0; b < 8 && !found; b++) {
      gLowUs = lows[a];
      gSampleUs = samples[b];
      unsigned w = tryRom(rom);
      bool ok = w && rom[0] != 0x00 && rom[0] != 0xFF && crc8(rom, 7) == rom[7];
      Serial.printf("low=%u smp=%2u pres=%3u  ROM:", gLowUs, gSampleUs, w);
      for (int i = 0; i < 8; i++) Serial.printf(" %02X", rom[i]);
      if (ok) { Serial.print("   <<< VALID ROM"); found = true; }
      Serial.println();
      delay(20);
    }
  }

  if (found) {
    bestLow = gLowUs; bestSample = gSampleUs;
    Serial.printf("\n>>> 时序命中: low=%u sample=%u\n", bestLow, bestSample);
    Serial.printf(">>> family = 0x%02X  ", rom[0]);
    if (rom[0] == 0x28) Serial.print("(DS18B20 系列)");
    else if (rom[0] == 0x42) Serial.print("(MAX30207/31820 类体温片)");
    else if (rom[0] == 0x22) Serial.print("(DS1822)");
    else if (rom[0] == 0x10) Serial.print("(DS18S20)");
    else Serial.print("(未知 family)");
    Serial.println();
    Serial.print(">>> 序列号: ");
    for (int i = 6; i >= 1; i--) Serial.printf("%02X", rom[i]);
    Serial.println();

    // 用命中时序读温度
    for (int t = 0; t < 3; t++) {
      uint16_t raw = 0;
      float c = tryTemp(&raw);
      if (isnan(c)) Serial.printf("  温度读取失败 (raw=0x%04X)\n", raw);
      else Serial.printf("  温度 = %.2f C  (raw=0x%04X)\n", c, raw);
      delay(600);
    }
    Serial.println(">>> 完成，10 秒后重新扫描");
    delay(10000);
  } else {
    Serial.println("\n>>> 全部组合都没读出合法 ROM");
    Serial.println(">>> 下一轮改试 SKIP ROM(0xCC)+READ SCRATCHPAD(0xBE) 看能否读到温度");
    delay(2000);

    // 退化路径：不读 ROM，直接跳过 ROM 读暂存器
    for (uint8_t a = 0; a < 3; a++) {
      for (uint8_t b = 0; b < 8; b++) {
        gLowUs = lows[a]; gSampleUs = samples[b];
        uint16_t raw = 0;
        float c = tryTemp(&raw);
        if (!isnan(c)) {
          Serial.printf("  [SKIP-ROM] low=%u smp=%2u -> %.2f C (raw=0x%04X) <<< 成功\n",
                        gLowUs, gSampleUs, c, raw);
          delay(10000);
          return;
        }
      }
    }
    Serial.println("  SKIP-ROM 路径也全失败，10 秒后重试");
    delay(10000);
  }
}
