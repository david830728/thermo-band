// _probe3 · DS18B20 电气特性判定
// 判定逻辑：
//   A. 爬升时间 riseTime：驱动低 → 松开（不加上拉）→ 测多久回到高
//        < 20µs   → 外部上拉存在（4.7k 级别）
//        > 500µs  → 没有外部上拉（只有分布电容/漏电）
//   B. presence 宽度：真 DS18B20 应答低电平 60~240µs；纯电容假阳性会 >300µs 且呈指数爬升
//   C. 480µs / 960µs 两种复位脉冲对比（部分克隆片需要更长复位）
#include <Arduino.h>

#define PIN 17

static inline void owLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }

// 驱动低 60µs 后完全松开（INPUT，无内部上拉），测爬升到高的时间
uint32_t riseTime(uint8_t p) {
  owLow(p);
  delayMicroseconds(60);
  pinMode(p, INPUT);                 // 高阻，无内部上拉
  uint32_t t0 = micros();
  while (digitalRead(p) == LOW && (micros() - t0) < 3000) {}
  return micros() - t0;
}

// 带指定复位脉宽的 presence 测量；返回低电平宽度（µs），0 = 无应答
uint32_t presenceWidth(uint8_t p, uint16_t resetUs, bool useInternalPullup) {
  owLow(p);
  delayMicroseconds(resetUs);
  if (useInternalPullup) pinMode(p, INPUT_PULLUP); else pinMode(p, INPUT);
  uint32_t t0 = micros();
  // 等它回到高（=presence 结束）
  while (digitalRead(p) == LOW && (micros() - t0) < 2000) {}
  uint32_t w = micros() - t0;
  if (w >= 2000) return 0;           // 一直低 → 异常/短路
  return w;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== PROBE3 · DS18B20 电气判定 ===");
}

void loop() {
  Serial.println("----------------------------------------");

  // 1) 空闲电平
  pinMode(PIN, INPUT);
  delay(3);
  int idleFloat = digitalRead(PIN);
  pinMode(PIN, INPUT_PULLUP);
  delay(3);
  int idlePull = digitalRead(PIN);
  Serial.printf("[%d] idle: float=%d pullup=%d\n", PIN, idleFloat, idlePull);

  // 2) 爬升时间（判断外部上拉是否存在）
  uint32_t r1 = riseTime(PIN), r2 = riseTime(PIN), r3 = riseTime(PIN);
  Serial.printf("  rise time: %lu / %lu / %lu us  -> %s\n",
                (unsigned long)r1, (unsigned long)r2, (unsigned long)r3,
                (r1 < 20 && r2 < 20) ? "外部上拉存在 ✓" :
                (r1 > 500 || r2 > 500) ? "无外部上拉（纯电容）✗" : "临界，可能上拉偏弱");

  // 3) presence 宽度：480µs 复位，内部上拉
  uint32_t w1 = presenceWidth(PIN, 480, true);
  Serial.printf("  presence(480us, 内部上拉): %lu us -> %s\n", (unsigned long)w1,
                w1 == 0 ? "无应答" : (w1 >= 60 && w1 <= 240) ? "真 1-Wire 器件 ✓" : "异常宽度（电容/干扰）");

  // 4) presence 宽度：480µs 复位，仅外部上拉（不加内部）
  uint32_t w2 = presenceWidth(PIN, 480, false);
  Serial.printf("  presence(480us, 仅外部上拉): %lu us\n", (unsigned long)w2);

  // 5) presence 宽度：960µs 复位（部分克隆片要求更长）
  uint32_t w3 = presenceWidth(PIN, 960, true);
  Serial.printf("  presence(960us, 内部上拉): %lu us\n", (unsigned long)w3);

  Serial.println();
  delay(3000);
}
