// _m601e · presence 窗口加宽验证
//
// 诊断结论：pw1=115 正常，pw2 全是 0。不是数据错，是"第二次复位收不到 presence"。
// 推断：手册 tPDHIGH = 15~60us，我旧代码只等 32us 就判无应答，第二次响应偏晚就被漏掉。
//
// 本固件把 presence 等待窗口拉到 200us，并打印"释放后多少 us 才拉低"（delay），
// 三个变体各测一遍，确认到底是漏检还是器件真的不应答。

#include <Arduino.h>
#include <driver/gpio.h>

#define PIN 17

static inline void lo() { gpio_set_level((gpio_num_t)PIN, 0); }
static inline void hi() { gpio_set_level((gpio_num_t)PIN, 1); }
static inline int rd() { return gpio_get_level((gpio_num_t)PIN); }
static inline void du(uint32_t us) { esp_rom_delay_us(us); }

static uint8_t crc8(const uint8_t *d, uint8_t n) {
  uint8_t c = 0;
  while (n--) {
    uint8_t b = *d++;
    for (int i = 0; i < 8; i++) {
      uint8_t m = (c ^ b) & 1;
      c >>= 1;
      if (m) c ^= 0x8C;
      b >>= 1;
    }
  }
  return c;
}

static void owInit() {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << PIN);
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  hi();
  du(1000);
}

// 返回 presence 低电平宽度；*delayUs = 释放后到器件拉低的延迟（0 表示没等到）
static uint32_t owResetEx(uint32_t *delayUs) {
  lo(); du(960);
  uint32_t t0 = micros();
  hi();
  uint32_t d = 0;
  bool got = false;
  while ((uint32_t)(micros() - t0) < 200) {     // ★ 窗口 32us -> 200us
    if (!rd()) { d = micros() - t0; got = true; break; }
  }
  if (!got) { if (delayUs) *delayUs = 0; du(400); return 0; }
  uint32_t t1 = micros();
  while (!rd() && (uint32_t)(micros() - t1) < 400) {}
  uint32_t w = micros() - t1;
  if (delayUs) *delayUs = d;
  du(400);
  return w;
}

static void owWrite(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 1) { lo(); du(6);  hi(); du(60); }
    else       { lo(); du(62); hi(); du(10); }
    b >>= 1;
  }
}

static uint8_t owRead() {
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    lo(); du(2); hi();
    du(10);
    int bit = rd();
    du(46);
    if (bit) v |= (1 << i);
  }
  return v;
}

static void variant(const char *name, uint32_t waitMs, bool romFirst) {
  uint8_t sp[9];
  uint32_t d1 = 0, d2 = 0;

  noInterrupts();
  uint32_t pw1 = owResetEx(&d1);
  if (pw1) {
    if (romFirst) { owWrite(0x33); for (int i = 0; i < 8; i++) owRead(); }
    owWrite(0xCC); owWrite(0x44);
  }
  interrupts();
  if (!pw1) { Serial.printf("  [%s] ✘ 复位1 无 presence\n", name); return; }
  delay(waitMs);

  noInterrupts();
  uint32_t pw2 = owResetEx(&d2);
  if (pw2) {
    owWrite(0xCC); owWrite(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = owRead();
  }
  interrupts();

  Serial.printf("  [%s] 复位1: 延迟%luus 宽%luus | 复位2: ", name,
                (unsigned long)d1, (unsigned long)pw1);
  if (!pw2) { Serial.println("✘ 仍无 presence"); return; }
  Serial.printf("延迟%luus 宽%luus\n", (unsigned long)d2, (unsigned long)pw2);

  uint8_t c = crc8(sp, 8);
  Serial.print("        SP:");
  for (int i = 0; i < 9; i++) Serial.printf(" %02X", sp[i]);
  Serial.printf("   CRC算=%02X 器件=%02X %s", c, sp[8], c == sp[8] ? "✔" : "✘");
  if (c == sp[8]) {
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    Serial.printf("   T = 40 + %d/256 = %.4f C", raw, 40.0f + raw / 256.0f);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== _m601e · presence 窗口 200us ===");
  owInit();
  Serial.printf("空闲=%d\n", rd());
}

void loop() {
  Serial.println("--- 新轮 ---");
  variant("V1 等30ms", 30, false);
  variant("V2 等100ms", 100, false);
  variant("V3 先读ROM", 30, true);
  Serial.println();
  delay(2000);
}
