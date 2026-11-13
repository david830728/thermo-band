// _linecheck — 验证 SDA(GPIO11) / SCL(GPIO10) 排针是否真的连到模块
// 循环：拉低 20s → 释放 10s
// 拉低期间测模块 SDA/SCL 对 GND 电压：0V = 线通，3V = 线没接在这两个脚上

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== LINECHECK ===");
  Serial.println("循环: LOW 20s -> HIGH 10s");
}

void loop() {
  pinMode(11, OUTPUT);
  pinMode(10, OUTPUT);
  digitalWrite(11, LOW);
  digitalWrite(10, LOW);
  Serial.println("[LOW 20s] GPIO11=0V GPIO10=0V  <-- 现在测模块 SDA/SCL 对 GND");
  delay(20000);

  pinMode(11, INPUT);
  pinMode(10, INPUT);
  Serial.println("[HIGH 10s] 已释放，总线回升 3V");
  delay(10000);
}
