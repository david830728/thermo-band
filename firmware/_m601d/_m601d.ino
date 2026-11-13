// _m601d · M601 读取诊断（一次烧录定位失败环节）
//
// _w1g 能读到（CRC 连续通过），重写成 _m601 后全失败。差异点逐个测：
//   A 策略：reset -> CC,44 -> wait30 -> reset -> CC,BE -> read9
//   B 策略：先发一次 Read ROM(0x33) 再走 A（_w1g 的真实顺序）
//   C 策略：wait 拉长到 80ms 再读
// 每个策略打印 presence 宽度 + 9 字节 + CRC，直接看出卡在哪一步。

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

static uint32_t owReset() {
  lo(); du(960);
  hi();
  du(32);
  if (rd()) { du(400); return 0; }
  uint32_t t = micros();
  while (!rd() && (uint32_t)(micros() - t) < 400) {}
  uint32_t w = micros() - t;
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

static void hexline(const char *tag, uint8_t *d, int n) {
  Serial.printf("    %s:", tag);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

// A/B/C 共用：转换 -> 读暂存器
static bool probe(const char *name, uint32_t waitMs, bool preRom) {
  uint8_t sp[9];

  noInterrupts();
  uint32_t pw1 = owReset();
  if (pw1 && preRom) { owWrite(0x33); for (int i = 0; i < 8; i++) owRead(); }
  if (pw1) { owWrite(0xCC); owWrite(0x44); }
  interrupts();

  if (!pw1) { Serial.printf("  [%s] ✘ 复位1 无 presence\n", name); return false; }
  delay(waitMs);

  noInterrupts();
  uint32_t pw2 = owReset();
  if (pw2) {
    owWrite(0xCC); owWrite(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = owRead();
  }
  interrupts();

  if (!pw2) { Serial.printf("  [%s] ✘ 复位2 无 presence (pw1=%lu)\n", name, (unsigned long)pw1); return false; }

  uint8_t c = crc8(sp, 8);
  bool ok = (c == sp[8]);
  Serial.printf("  [%s] pw1=%lu pw2=%lu  %s\n", name, (unsigned long)pw1, (unsigned long)pw2,
                ok ? "CRC ✔" : "CRC ✘");
  hexline("SP", sp, 9);
  Serial.printf("        CRC算=%02X 器件=%02X", c, sp[8]);
  if (ok) {
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    Serial.printf("   T = 40 + %d/256 = %.4f C", raw, 40.0f + raw / 256.0f);
  }
  Serial.println();
  return ok;
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== _m601d · 诊断 ===");
  owInit();
  Serial.printf("空闲=%d\n", rd());
}

void loop() {
  Serial.println("--- 新轮 ---");
  bool a = probe("A 标准", 30, false);
  bool b = probe("B 先读ROM", 30, true);
  bool c = probe("C 等80ms", 80, false);
  Serial.println(a || b || c ? ">>> 至少一种成功\n" : ">>> 三种全失败\n");
  delay(2000);
}
