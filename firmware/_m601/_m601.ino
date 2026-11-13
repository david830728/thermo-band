// _m601 · 敏源 M601 单总线温度读取（已实机验证跑通 2026-11-12）
//
// 器件：敏源传感 M601（DFN8，1-wire 单总线，家族码 28h，与 DS18B20 共用）
// 接线：VCC=3V3  GND=GND  DO(GPIO17)，DQ 需 2~10kΩ 上拉（模块 R1 + 外部 4.7k 并联可用）
//
// 【关键公式】T[°C] = 40 + S_T / 256
//   S_T = int16(temp_msb << 8 | temp_lsb)，16bit 补码，LSB=1/256℃
//   ⚠ 那个 +40 是内部 ADC 的基准偏置，手册里公式是图片，纯文字提取会漏掉，极易踩坑。
//   自检：0x0000=40℃   0x6E00=150℃   0x9200=-70℃
//
// 【关键时序】(手册 202503-V4.5 §11.10)
//   tRSTL  480/960us | tPDHIGH 15/30/60 | tPDLOW 60/115/240
//   tSLOT  60/120us  | tLOW1 1/5/15     | tLOW0 60/60/120
//   tRDV   读数据有效 5~15us —— 必须在下降沿后 15us 内采样，取 12us
//
// 串口指令：B = 重启进入下载模式（免按键烧录）  R = 普通重启

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>

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
  lo(); du(960);                    // tRSTL 典型值
  uint32_t t0 = micros();
  hi();
  // ★ 等 presence 起始：本板实测 36~40us 才拉低（手册 tPDHIGH 15~60us）
  //   窗口必须给到 200us。之前卡 32us 是致命 bug，会导致"复位1 有应答、复位2 无应答"的假象。
  bool got = false;
  while ((uint32_t)(micros() - t0) < 200) {
    if (!rd()) { got = true; break; }
  }
  if (!got) { du(400); return 0; }
  uint32_t t1 = micros();
  while (!rd() && (uint32_t)(micros() - t1) < 400) {}
  uint32_t w = micros() - t1;
  du(400);
  return w;
}

static void owWrite(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 1) { lo(); du(6);  hi(); du(60); }
    else       { lo(); du(70); hi(); du(8);  }
    b >>= 1;
  }
}

static uint8_t owRead() {
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    lo(); du(2); hi();
    du(10);                      // 采样点 12us，tRDV(5~15us)
    int bit = rd();
    du(50);
    if (bit) v |= (1 << i);
  }
  return v;
}

// 返回 true 表示成功；t 为摄氏度。失败时打印详细诊断
static bool m601Read(float &t, int16_t &rawOut) {
  uint8_t sp[9];
  noInterrupts();
  uint32_t pw1 = owReset();
  if (pw1) { owWrite(0xCC); owWrite(0x44); }
  interrupts();
  if (!pw1) { Serial.println("  ✘ 第1次复位无 presence"); return false; }

  delay(20);                              // tCONV 最大 10.5ms，留余量

  noInterrupts();
  uint32_t pw2 = owReset();
  if (pw2) {
    owWrite(0xCC); owWrite(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = owRead();
  }
  interrupts();
  if (!pw2) { Serial.printf("  ✘ 第2次复位无 presence (pw1=%luus)\n", (unsigned long)pw1); return false; }

  uint8_t c = crc8(sp, 8);
  if (c != sp[8]) {
    Serial.printf("  ✘ CRC: ");
    for (int i = 0; i < 9; i++) Serial.printf("%02X ", sp[i]);
    Serial.printf(" 算=%02X 器件=%02X\n", c, sp[8]);
    return false;
  }

  int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
  rawOut = raw;
  t = 40.0f + raw / 256.0f;               // ★ 核心公式
  return true;
}

static void goBootloader() {
  Serial.println("\n[指令] 拉低 GPIO0 并重启 -> 下载模式，可直接烧录");
  Serial.flush();
  delay(100);
  gpio_set_direction((gpio_num_t)0, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)0, 0);
  delay(150);
  esp_restart();
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== _m601 · 敏源 M601 温度读取 ===");
  owInit();
  Serial.printf("空闲电平=%d | 指令: B=进下载模式 R=重启\n\n", rd());
}

void loop() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'B' || c == 'b') goBootloader();
    if (c == 'R' || c == 'r') esp_restart();
  }

  float t;
  int16_t raw;
  if (m601Read(t, raw)) {
    Serial.printf("温度 = %.4f C   (raw=0x%04X = %d)\n", t, (uint16_t)raw, raw);
  } else {
    Serial.println("读取失败（CRC 或 presence）");
  }
  delay(1000);
}
