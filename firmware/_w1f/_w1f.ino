// _w1f · 多激励响应扫描
//
// 假设：器件可能不是 DS18B20 的"主机读时隙"协议，而是 DHT 类
// "主机发长起始信号 → 器件主动吐一串脉冲编码数据"协议。
//
// 做法：用 5 种不同长度的低电平去激励（100us / 480us / 1ms / 5ms / 18ms），
// 每次释放后连续采样 12ms，把全部边沿连同时间戳打印出来。
// 哪种激励能引出"presence 之后紧跟一串脉冲"，就是它的协议。

#include <Arduino.h>
#include <driver/gpio.h>

#define PIN 17
#define MAXE 96

struct E { uint32_t t; uint8_t v; };
static E es[MAXE];

static void owInit() {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << PIN);
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  gpio_set_level((gpio_num_t)PIN, 1);
  delayMicroseconds(500);
}

static void stim(uint32_t lowUs, const char *tag) {
  gpio_set_level((gpio_num_t)PIN, 0);
  if (lowUs >= 1000) delay(lowUs / 1000);
  else delayMicroseconds(lowUs);

  uint32_t t0 = micros();
  gpio_set_level((gpio_num_t)PIN, 1);

  int n = 0, last = 1;
  while ((uint32_t)(micros() - t0) < 12000 && n < MAXE) {
    uint32_t now = micros();
    int v = gpio_get_level((gpio_num_t)PIN);
    if (v != last) { es[n].t = now - t0; es[n].v = (uint8_t)v; n++; last = v; }
  }

  Serial.printf("\n[%s] 低 %luus -> 释放后 12ms 内边沿数=%d\n",
                tag, (unsigned long)lowUs, n);
  if (n == 0) { Serial.println("   全程恒高：器件完全沉默"); return; }

  // 按行打印，每行 8 个边沿
  for (int i = 0; i < n && i < 48; i++) {
    if (i % 8 == 0) Serial.print("   ");
    Serial.printf(" %s+%lu", es[i].v ? "H" : "L", (unsigned long)es[i].t);
    if (i % 8 == 7) Serial.println();
  }
  Serial.println();

  // 判断是否出现"presence 后紧跟脉冲串"（DHT 类特征）
  if (n >= 6) {
    uint32_t presEnd = 0;
    for (int i = 0; i < n; i++) {
      if (es[i].v == 1) { presEnd = es[i].t; break; }
    }
    if (presEnd > 0 && presEnd < 300) {
      Serial.printf("   >>> presence 约 %luus 后结束，且后续还有 %d 个边沿 —— 疑似 DHT 类数据流\n",
                    (unsigned long)presEnd, n - 1);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1f · 多激励响应扫描 ===");
  owInit();
  Serial.printf("空闲=%d\n", gpio_get_level((gpio_num_t)PIN));
}

void loop() {
  stim(100,   "100us");
  delay(2500);
  stim(480,   "480us 标准1-Wire复位");
  delay(2500);
  stim(1000,  "1ms");
  delay(2500);
  stim(5000,  "5ms");
  delay(2500);
  stim(18000, "18ms DHT风格起始");
  delay(6000);
  Serial.println("\n=========== 一轮结束 ===========");
  delay(3000);
}
