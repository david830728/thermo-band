// _w1d · 开漏模式 + 精确时序
//
// _w1b 已经证明器件是活的、返回真实数据：
//   ROM 前 6 字节稳定重复 28 9E 7E 7E 2A A0（0x28 = DS18B20 family code）
//   但 CRC 不过、末两字节乱跳
//
// 根因：owLow()/owRel() 用 pinMode() 切换，每次 1~2us 不稳定开销，
// 64 个时隙累积下来，最后几个字节就错位。
//
// 本固件改用 GPIO_MODE_INPUT_OUTPUT_OD（开漏）+ gpio_set_level()，
// 切换开销从 ~1.5us 降到 ~0.1us，时序精度提升一个数量级。
// 同时扫描采样点 3~20us，找出这个芯片真正的最佳采样位置。

#include <Arduino.h>
#include <driver/gpio.h>

#define PIN 17

static uint8_t gSampleUs = 11;

static inline void owLow() { gpio_set_level((gpio_num_t)PIN, 0); }
static inline void owRel() { gpio_set_level((gpio_num_t)PIN, 1); }   // 开漏=高阻，靠外部上拉

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

static void owInit() {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << PIN);
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  owRel();
  delayMicroseconds(500);
}

static unsigned owReset() {
  owLow();
  delayMicroseconds(480);
  owRel();
  unsigned long t0 = micros();
  while (gpio_get_level((gpio_num_t)PIN) == 1 && micros() - t0 < 150) {}
  if (gpio_get_level((gpio_num_t)PIN) == 1) { delayMicroseconds(300); return 0; }
  unsigned long tl = micros();
  while (gpio_get_level((gpio_num_t)PIN) == 0 && micros() - tl < 400) {}
  unsigned w = (unsigned)(micros() - tl);
  delayMicroseconds(700);          // tREC >= 480us
  return w;
}

static void owWriteBit(uint8_t b) {
  if (b) { owLow(); delayMicroseconds(2);  owRel(); delayMicroseconds(64); }
  else   { owLow(); delayMicroseconds(62); owRel(); delayMicroseconds(6);  }
}

static uint8_t owReadBit() {
  owLow();
  delayMicroseconds(2);                                  // 下降沿 2us
  owRel();
  if (gSampleUs > 2) delayMicroseconds(gSampleUs - 2);   // 到采样点
  uint8_t v = (uint8_t)gpio_get_level((gpio_num_t)PIN);
  if (gSampleUs < 66) delayMicroseconds(66 - gSampleUs); // 补足时隙
  return v;
}

static void owWriteByte(uint8_t v) { for (int i = 0; i < 8; i++) { owWriteBit(v & 1); v >>= 1; } }
static uint8_t owReadByte() { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= (owReadBit() << i); return v; }

// 读 ROM；同时可打印位流
static bool readRom(uint8_t *rom, bool dumpBits) {
  char bits[65];
  unsigned w = owReset();
  if (!w) return false;
  owWriteByte(0x33);
  int k = 0;
  for (int i = 0; i < 8; i++) {
    uint8_t v = 0;
    for (int b = 0; b < 8; b++) {
      uint8_t bit = owReadBit();
      v |= (bit << b);
      if (dumpBits && k < 64) bits[k++] = bit ? '1' : '0';
    }
    rom[i] = v;
  }
  if (dumpBits) { bits[64] = 0; Serial.printf("    位流: %s\n", bits); }
  return rom[0] != 0x00 && rom[0] != 0xFF && crc8(rom, 7) == rom[7];
}

// 读温度（跳过 ROM）
static float readTemp(uint16_t *rawOut) {
  uint8_t sp[9];
  if (!owReset()) return NAN;
  owWriteByte(0xCC);
  owWriteByte(0x44);               // 启动转换
  delay(800);
  if (!owReset()) return NAN;
  owWriteByte(0xCC);
  owWriteByte(0xBE);               // 读暂存器
  for (int i = 0; i < 9; i++) sp[i] = owReadByte();
  int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
  if (rawOut) *rawOut = (uint16_t)raw;
  if (crc8(sp, 8) != sp[8]) return NAN;
  return raw * 0.0625f;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1d · 开漏精确时序 ===");
  owInit();
  Serial.printf("空闲电平: %d（1=高，有上拉）\n", gpio_get_level((gpio_num_t)PIN));
}

void loop() {
  static const uint8_t sampleList[] = {3, 5, 8, 11, 14, 17, 20};
  bool found = false;
  uint8_t rom[8];
  uint8_t hitSample = 0;

  Serial.println("\n---------------- sweep (OD mode) ----------------");
  for (uint8_t i = 0; i < 7; i++) {
    gSampleUs = sampleList[i];
    bool ok = readRom(rom, false);
    Serial.printf("smp=%2u ROM:", gSampleUs);
    for (int j = 0; j < 8; j++) Serial.printf(" %02X", rom[j]);
    Serial.print(ok ? "  <<< VALID (CRC OK)" : "");
    Serial.println();
    if (ok) { found = true; hitSample = gSampleUs; break; }
    delay(40);
  }

  if (!found) {
    Serial.println("\n[诊断] 全失败，打印 smp=8 的完整位流：");
    gSampleUs = 8;
    readRom(rom, true);
    Serial.println("[诊断] 再打印 smp=11 的位流：");
    gSampleUs = 11;
    readRom(rom, true);
    delay(6000);
    return;
  }

  // 命中：用同一点位重新读一次拿详情 + 温度
  gSampleUs = hitSample;
  Serial.printf("\n>>> 命中采样点 smp=%u，重读详情：\n", hitSample);
  readRom(rom, true);

  Serial.print(">>> ROM:");
  for (int i = 0; i < 8; i++) Serial.printf(" %02X", rom[i]);
  Serial.println();
  Serial.printf(">>> family=0x%02X\n", rom[0]);

  Serial.println(">>> 读温度：");
  for (int t = 0; t < 5; t++) {
    uint16_t raw = 0;
    float c = readTemp(&raw);
    if (isnan(c)) Serial.printf("  失败 raw=0x%04X\n", raw);
    else Serial.printf("  %.3f C  (raw=0x%04X)\n", c, raw);
    delay(1000);
  }
  delay(15000);
}
