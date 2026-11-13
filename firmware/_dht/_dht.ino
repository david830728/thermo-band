// _dht · DHT11 读取探针（GPIO17）
// DHT11 协议：MCU 拉低 >=18ms → 释放 → DHT11 应答 80us 低 + 80us 高 → 40bit 数据
//   每位：50us 低 + （26~28us 高 = 0 / 70us 高 = 1）
//   5 字节：湿度整数 湿度小数 温度整数 温度小数 校验和
// 注意：DHT11 采样间隔必须 >= 1s，本程序 2s 一次
#include <Arduino.h>

#define PIN 17

int failCount = 0, okCount = 0;

bool readDHT(uint8_t pin, uint8_t *d) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);                       // 起始信号：拉低 20ms（>=18ms）
  pinMode(pin, INPUT_PULLUP);      // 释放，等 DHT11 应答
  delayMicroseconds(30);

  uint32_t t0 = micros();
  while (digitalRead(pin) == HIGH) {                 // 等应答低电平
    if (micros() - t0 > 200) return false;
  }
  t0 = micros();
  while (digitalRead(pin) == LOW) {                  // 应答低 80us
    if (micros() - t0 > 150) return false;
  }
  t0 = micros();
  while (digitalRead(pin) == HIGH) {                 // 应答高 80us
    if (micros() - t0 > 150) return false;
  }

  for (uint8_t i = 0; i < 5; i++) {
    d[i] = 0;
    for (uint8_t b = 0; b < 8; b++) {
      t0 = micros();
      while (digitalRead(pin) == LOW) {              // 每位起始 50us 低
        if (micros() - t0 > 100) return false;
      }
      t0 = micros();
      while (digitalRead(pin) == HIGH) {             // 高电平长度决定 0/1
        if (micros() - t0 > 100) return false;
      }
      uint32_t hi = micros() - t0;
      d[i] = (d[i] << 1) | (hi > 45 ? 1 : 0);
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== DHT11 PROBE (GPIO17) ===");
}

void loop() {
  uint8_t d[5] = {0};
  if (readDHT(PIN, d)) {
    uint8_t sum = d[0] + d[1] + d[2] + d[3];
    bool crcOk = (sum == d[4]);
    okCount++;
    Serial.printf("[OK %d/%d] raw=%02X %02X %02X %02X %02X | T=%d.%d C  RH=%d.%d %%  checksum=%s\n",
                  okCount, okCount + failCount,
                  d[0], d[1], d[2], d[3], d[4],
                  d[2], d[3], d[0], d[1], crcOk ? "OK" : "BAD");
    if (!crcOk) Serial.println("  !! 校验失败：数据位有错，检查线长/上拉/接触");
  } else {
    failCount++;
    Serial.printf("[FAIL %d/%d] 无应答 —— 检查 VDD(3V3)/GND/DATA 三根线，或换 DHT11\n",
                  okCount, okCount + failCount);
  }
  delay(2000);
}
