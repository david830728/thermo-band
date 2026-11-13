// _w1c · 读时隙波形观察
//
// 现象总结（_w1b 45 组矩阵）：
//   low=1~5, smp=5  → 读到非 FF 的乱码（线还在低）
//   low=任意, smp>=8 → 全 FF（读到高）
// presence 却稳定 115us
//
// 若器件真在驱动数据，读时隙应出现"明显低脉冲"；若只是电容爬升，
// 则应是"释放后 1~3us 立刻回升"。本固件把每个时隙的电平边沿全打印出来。
//
// 同时做对照：不发任何命令时读时隙长什么样（排除纯电容假象）。

#include <Arduino.h>

#define PIN 17

static inline void owLow() { digitalWrite(PIN, LOW); pinMode(PIN, OUTPUT); }
static inline void owRel() { pinMode(PIN, INPUT_PULLUP); }

static unsigned owReset() {
  owLow();
  delayMicroseconds(480);
  owRel();
  unsigned long t0 = micros();
  while (digitalRead(PIN) == HIGH && micros() - t0 < 150) {}
  if (digitalRead(PIN) == HIGH) { delayMicroseconds(300); return 0; }
  unsigned long tl = micros();
  while (digitalRead(PIN) == LOW && micros() - tl < 400) {}
  unsigned w = (unsigned)(micros() - tl);
  delayMicroseconds(250);
  return w;
}

static void owWriteBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(6);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(60); owRel(); delayMicroseconds(10); }
}
static void owWriteByte(uint8_t v) { for (int i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }

// 一个读时隙：拉低 2us → 释放 → 连续采样 120us，记录全部边沿
static void dumpSlot(int idx) {
  owLow();
  delayMicroseconds(2);
  unsigned long t0 = micros();
  owRel();
  int last = 1, n = 0;
  unsigned long ts[16];
  int vs[16];
  while ((micros() - t0) < 120 && n < 16) {
    unsigned long now = micros();
    int v = digitalRead(PIN);
    if (v != last) { ts[n] = now - t0; vs[n] = v; n++; last = v; }
  }
  Serial.printf("  #%02d ", idx);
  if (!n) Serial.print("恒高（器件完全未驱动）");
  for (int i = 0; i < n; i++) Serial.printf("%s+%lu ", vs[i] ? "H" : "L", ts[i]);
  Serial.println();
  delayMicroseconds(20);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1c · 读时隙波形观察 ===");
  pinMode(PIN, INPUT_PULLUP);
  delay(30);
}

void loop() {
  Serial.println("\n========== 有命令（0x33 READ ROM）==========");
  unsigned w = owReset();
  Serial.printf("[reset] presence=%uus\n", w);
  if (w) {
    owWriteByte(0x33);
    Serial.println("[已发 0x33] 观察 12 个读时隙：");
    for (int i = 0; i < 12; i++) dumpSlot(i);
  }

  delay(400);

  Serial.println("\n========== 对照：不发命令 ==========");
  w = owReset();
  Serial.printf("[reset] presence=%uus\n", w);
  Serial.println("观察 6 个读时隙（无命令，应仅见电容爬升）：");
  for (int i = 0; i < 6; i++) dumpSlot(i);

  delay(3000);
}
