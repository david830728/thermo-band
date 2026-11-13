// _c3_hello · ESP32-C3 启动 + LED 心跳（用来肉眼判断应用是否真的在跑）
//
// 为什么要有 LED：串口可能因为 CDC 配置或复位问题看不到，但 LED 闪不闪是硬的。
// 同时驱动 GPIO12/13/2/8，覆盖常见 C3 开发板的用户 LED 位置。
#include <Arduino.h>

static const uint8_t kLedPins[] = {12, 13, 2, 8};
static const uint8_t kLedN = sizeof(kLedPins) / sizeof(kLedPins[0]);

void setup() {
  for (uint8_t i = 0; i < kLedN; i++) { pinMode(kLedPins[i], OUTPUT); digitalWrite(kLedPins[i], LOW); }

  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=================================");
  Serial.println("HELLO C3  -- boot + led heartbeat");
  Serial.printf("chip=%s rev=%d cores=%d\n", ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores());
  Serial.printf("flash=%lu bytes  cpu=%lu MHz\n", (unsigned long)ESP.getFlashChipSize(), (unsigned long)getCpuFrequencyMhz());
  Serial.println("=================================");

  // 开机三连闪：即便串口看不到，也能用眼睛确认应用跑起来了
  for (int k = 0; k < 3; k++) {
    for (uint8_t i = 0; i < kLedN; i++) digitalWrite(kLedPins[i], HIGH);
    delay(120);
    for (uint8_t i = 0; i < kLedN; i++) digitalWrite(kLedPins[i], LOW);
    delay(120);
  }
}

void loop() {
  static uint32_t n = 0;
  static bool on = false;
  n++;
  on = !on;
  for (uint8_t i = 0; i < kLedN; i++) digitalWrite(kLedPins[i], on ? HIGH : LOW);
  Serial.printf("tick %lu  uptime=%lu ms  led=%d\n", (unsigned long)n, (unsigned long)millis(), (int)on);
  delay(500);
}
