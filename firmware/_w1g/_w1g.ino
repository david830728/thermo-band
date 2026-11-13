// _w1g · 敏源 M601 全事务读取（关中断 + 双换算）
//
// 器件确认：敏源传感 M601（1-wire 单总线数字温度芯片，DFN8，家族码 28h）
//   复位：低 >=480us → 15~60us 后器件拉低 60~240us   （_w1f 实测 +31us 拉低、持续 116us，完全吻合）
//   指令集与 DS18B20 相同：33h读ROM / CCh跳过ROM / 44h转换 / BEh读暂存器 / 4Eh写 / 48h拷贝 / B8h召回
//   ⚠ 温度格式不同：16bit 有符号补码，LSB = 1/256 ℃  => T = raw / 256.0
//      （DS18B20 是 12bit、LSB=1/256*16=0.0625℃，这就是之前换算会错的地方）
//   暂存器：b0=temp_lsb b1=temp_msb b2,b3=保留 b4=TH b5=TL b6=cfg b7=status b8=CRC8(0x8C)
//
// 官方时序表（M601 系列 202503-V4.5 · 11.10 单总线时序特性）:
//   tRSTL  复位低电平       min 480 / typ 960 us   ← 用 960
//   tPDHIGH presence 高     15 / 30 / 60 us        ← 本板实测 31us  ✔
//   tPDLOW  presence 低     60 / 115 / 240 us      ← 本板实测 116us ✔✔ 完全命中典型值
//   tSLOT  时隙长度         60 / 120 us
//   tLOW1  写1低电平        1 / 5 / 15 us
//   tLOW0  写0低电平        60 / 60 / 120 us
//   tREC   时隙间恢复       1 / 5 us
//   tRDV   读数据有效       5 / 15 us   ← ★关键：主机必须在下降沿后 5~15us 内采样
//
// 之前失败的根因：读时隙采样点取到 18~21us，超出 tRDV 的 15us 上限 → 读回全 1。
// 本固件：采样点定在 12us（窗口正中），并用 noInterrupts() 防止中断拉长时隙。

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

// 复位，返回 presence 低电平宽度(us)，0 = 无应答
static uint32_t owReset() {
  lo(); du(960);                 // tRSTL 用典型值 960us（手册 min 480）
  hi();
  du(32);                        // tPDHIGH 典型 30us
  if (rd()) { du(400); return 0; }
  uint32_t t = micros();
  while (!rd() && (uint32_t)(micros() - t) < 400) {}
  uint32_t w = micros() - t;
  du(400);                       // 收尾，保证 tREC >= 480us
  return w;
}

static void owWrite(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 1) { lo(); du(6);  hi(); du(60); }   // 写1：tLOW1 落在 1~15us
    else       { lo(); du(70); hi(); du(8);  }   // 写0：tLOW0 落在 60~120us
    b >>= 1;
  }
}

static uint32_t gSample = 12;    // 读时隙采样点(us)，可在 8/12/15 之间切换

static uint8_t owRead() {
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    lo(); du(2); hi();           // 下降沿后至少 1us 释放
    du(gSample - 2);             // 采样点默认 12us，tRDV(5~15us) 窗口正中
    int bit = rd();
    du(58 - gSample);            // 补齐 tSLOT >= 60us
    if (bit) v |= (1 << i);
  }
  return v;
}

static void dump(const char *tag, uint8_t *d, int n, uint8_t crcIdx) {
  Serial.printf("  %s:", tag);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  uint8_t c = crc8(d, crcIdx);
  Serial.printf("   CRC算=%02X 器件=%02X %s\n", c, d[crcIdx],
                c == d[crcIdx] ? "✔通过" : "✘不匹配");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1g · 敏源 M601 读取 ===");
  owInit();
  Serial.printf("空闲电平=%d (1=正常)\n", rd());
  Serial.println("温度换算: M601 用 raw/256  |  DS18B20 用 raw*0.0625（两者都会打印，看哪个合理）\n");
}

void loop() {
  // ---------- 0) 采样点扫描：定位真实有效的 tRDV 窗口 ----------
  const uint32_t pts[3] = {8, 12, 15};
  Serial.println("[采样点扫描] 三个采样点各读一次 ROM，看哪个 CRC 通过");
  for (int k = 0; k < 3; k++) {
    gSample = pts[k];
    uint8_t r[8];
    noInterrupts();
    uint32_t pw = owReset();
    if (pw) { owWrite(0x33); for (int i = 0; i < 8; i++) r[i] = owRead(); }
    interrupts();
    if (!pw) { Serial.printf("  采样%luus: 无 presence\n", (unsigned long)pts[k]); continue; }
    Serial.printf("  采样%luus: " , (unsigned long)pts[k]);
    for (int i = 0; i < 8; i++) Serial.printf("%02X ", r[i]);
    uint8_t c = crc8(r, 7);
    Serial.printf(" CRC算=%02X 器件=%02X %s\n", c, r[7], c == r[7] ? "✔" : "✘");
  }
  gSample = 12;
  Serial.println();

  // ---------- 1) 读 ROM ----------
  uint8_t rom[8];
  noInterrupts();
  uint32_t pw = owReset();
  if (pw) {
    owWrite(0x33);
    for (int i = 0; i < 8; i++) rom[i] = owRead();
  }
  interrupts();

  if (pw == 0) {
    Serial.println("[复位] 无 presence —— 检查接线");
    delay(2000);
    return;
  }
  Serial.printf("[复位] presence=%luus\n", (unsigned long)pw);
  dump("ROM  ", rom, 8, 7);
  Serial.printf("  family=0x%02X %s\n", rom[0],
                rom[0] == 0x28 ? "(=28h，敏源与 DS18B20 共用家族码，正常)" : "(异常)");

  // ---------- 2) 启动转换 ----------
  noInterrupts();
  owReset();
  owWrite(0xCC);
  owWrite(0x44);
  interrupts();
  Serial.println("[转换] 0xCC 0x44 已发，等 30ms（M601 默认高重复性 tCONV=10.5ms）");
  delay(30);

  // ---------- 3) 连读 3 次暂存器 ----------
  for (int k = 0; k < 3; k++) {
    uint8_t sp[9];
    noInterrupts();
    owReset();
    owWrite(0xCC);
    owWrite(0xBE);
    for (int i = 0; i < 9; i++) sp[i] = owRead();
    interrupts();

    Serial.printf("\n--- 第 %d 次读暂存器 ---\n", k + 1);
    dump("SP   ", sp, 9, 8);

    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    uint16_t uraw = (uint16_t)raw;
    Serial.printf("  b0=%02X b1=%02X  raw(hex)=%04X  raw(dec)=%d\n", sp[0], sp[1], uraw, raw);
    Serial.printf("  b4=TH=%d b5=TL=%d b6=cfg=%02X b7=status=%02X\n",
                  (int8_t)sp[4], (int8_t)sp[5], sp[6], sp[7]);
    Serial.printf("  >>> M601 换算: %.4f C      DS18B20 换算: %.4f C\n",
                  raw / 256.0f, raw * 0.0625f);
  }

  Serial.println("=========================================\n");
  delay(3000);
}
