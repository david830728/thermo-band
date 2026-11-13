// _w1e · 用 OneWire 官方库（PaulStoffregen）读传感器
//
// 目的：排除手写时序的嫌疑。
// _w1b/_w1d 的读时隙数据看着像"电容爬升的假位"（smp>=8 全 FF），
// 怀疑 0x33 命令根本没写进器件。OneWire 库是业界标准实现，
// ESP32 上有直接寄存器优化，如果它也读不出来，说明器件协议不是标准 1-Wire。

#include <OneWire.h>
#include <Arduino.h>

#define PIN 17

OneWire ds(PIN);

static void dumpScratchpad(const uint8_t *sp, int n) {
  Serial.print("  sp:");
  for (int i = 0; i < n; i++) Serial.printf(" %02X", sp[i]);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== _w1e · OneWire 官方库 ===");
  Serial.printf("引脚 GPIO%d\n", PIN);
  delay(200);
}

void loop() {
  Serial.println("--------------------------------");

  // 1) 复位 + presence
  uint8_t pres = ds.reset();
  Serial.printf("reset() -> 0x%02X  (%s)\n", pres,
                pres == 0 ? "有器件应答 ✓" : "无应答 ✗");

  uint8_t addr[8];
  memset(addr, 0, 8);

  if (pres == 0) {
    // 2) READ ROM
    ds.write(0x33, 0);
    for (int i = 0; i < 8; i++) addr[i] = ds.read();
    Serial.print("READ ROM:");
    for (int i = 0; i < 8; i++) Serial.printf(" %02X", addr[i]);
    uint8_t c = OneWire::crc8(addr, 7);
    Serial.printf("   CRC=%02X %s\n", c, c == addr[7] ? "OK ✓" : "BAD ✗");

    // 3) SEARCH（另一条路径，检验是否能枚举器件）
    ds.reset_search();
    uint8_t a2[8];
    int found = 0;
    while (ds.search(a2) && found < 4) {
      Serial.print("SEARCH 找到:");
      for (int i = 0; i < 8; i++) Serial.printf(" %02X", a2[i]);
      Serial.println();
      found++;
    }
    if (!found) Serial.println("SEARCH: 未枚举到任何器件");

    // 4) 读温度（有合法 ROM 才做）
    if (c == addr[7]) {
      Serial.println(">>> ROM 合法，开始读温度");
      ds.reset();
      ds.select(addr);
      ds.write(0x44, 1);          // 启动转换，寄生供电强上拉
      delay(800);

      ds.reset();
      ds.select(addr);
      ds.write(0xBE);             // 读暂存器
      uint8_t sp[9];
      for (int i = 0; i < 9; i++) sp[i] = ds.read();
      dumpScratchpad(sp, 9);
      int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
      Serial.printf("  raw=0x%04X  T=%.3f C  spCRC=%s\n",
                    (uint16_t)raw, raw * 0.0625f,
                    OneWire::crc8(sp, 8) == sp[8] ? "OK" : "BAD");
    }
  }

  // 5) 跳过 ROM 直接读（有些克隆片不支持 0x33）
  Serial.println("[备选] SKIP ROM 路径：");
  if (ds.reset() == 0) {
    ds.skip();
    ds.write(0x44, 1);
    delay(800);
    if (ds.reset() == 0) {
      ds.skip();
      ds.write(0xBE);
      uint8_t sp[9];
      for (int i = 0; i < 9; i++) sp[i] = ds.read();
      dumpScratchpad(sp, 9);
      int16_t raw = (int16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8));
      Serial.printf("  raw=0x%04X  T=%.3f C  spCRC=%s\n",
                    (uint16_t)raw, raw * 0.0625f,
                    OneWire::crc8(sp, 8) == sp[8] ? "OK" : "BAD");
    }
  }

  delay(4000);
}
