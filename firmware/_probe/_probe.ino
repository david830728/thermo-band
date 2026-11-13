// _probe · T1.1 传感器原始数据探针（纯串口，不碰屏幕）
// 用途：一次性看清 I2C 上到底挂了哪些器件、0x48 在不在、DS18B20 返回的原始字节是什么
// 烧录：source env.sh && flash _probe   （记得按 BOOT+RST 进下载模式）
#include <Arduino.h>
#include <Wire.h>

#define MAX_ADDR 0x48

// ---- 1-Wire 位操作（与 _ui 同一套时序，保证对比公平）----
static inline void owLow(uint8_t p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(uint8_t p) { pinMode(p, INPUT_PULLUP); }

bool owReset(uint8_t p) {
  owLow(p); delayMicroseconds(480);
  owRel(p); delayMicroseconds(70);
  bool pres = (digitalRead(p) == LOW);
  delayMicroseconds(410);
  return pres;
}
void owWriteBit(uint8_t p, uint8_t b) {
  if (b) { owLow(p); delayMicroseconds(6);  owRel(p); delayMicroseconds(64); }
  else   { owLow(p); delayMicroseconds(60); owRel(p); delayMicroseconds(10); }
}
uint8_t owReadBit(uint8_t p) {
  owLow(p); delayMicroseconds(3); owRel(p); delayMicroseconds(10);
  uint8_t b = (digitalRead(p) == HIGH) ? 1 : 0;
  delayMicroseconds(53);
  return b;
}
void owWriteByte(uint8_t p, uint8_t v) { for (uint8_t i = 0; i < 8; i++) { owWriteBit(p, v & 1); v >>= 1; } }
uint8_t owReadByte(uint8_t p) { uint8_t v = 0; for (uint8_t i = 0; i < 8; i++) v |= (owReadBit(p) << i); return v; }

// ---- I2C 扫描 ----
void scanBus(int sda, int scl, const char *tag) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  delay(20);
  Serial.printf("[I2C %s] SDA=%d SCL=%d -> ", tag, sda, scl);
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("0x%02X ", a); n++; }
    delay(2);
  }
  Serial.printf("| %u device(s)\n", n);
}

const char *labelOf(uint8_t a) {
  switch (a) {
    case 0x15: return "CST816T touch";
    case 0x51: return "PCF85063 RTC";
    case 0x6B: return "QMI8658 IMU";
    case 0x48: return "MAX30205 temp";
    default:   return "unknown";
  }
}

void probeMax() {
  Wire.beginTransmission(MAX_ADDR);
  if (Wire.endTransmission() != 0) { Serial.println("  MAX30205: no ACK (0x48 不在总线上)"); return; }
  // 读温度寄存器 0x00
  Wire.beginTransmission(MAX_ADDR); Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) { Serial.println("  MAX30205: ACK 但寄存器写失败"); return; }
  if (Wire.requestFrom((int)MAX_ADDR, 2) != 2) { Serial.println("  MAX30205: ACK 但读不到 2 字节"); return; }
  uint16_t raw = (Wire.read() << 8) | Wire.read();
  float t = (int16_t)raw * 0.00390625f;
  // 顺便读配置寄存器 0x01，确认 chip 真的醒了
  Wire.beginTransmission(MAX_ADDR); Wire.write(0x01);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MAX_ADDR, 1);
  uint8_t cfg = Wire.available() ? Wire.read() : 0xFF;
  Serial.printf("  MAX30205: raw=0x%04X  T=%.4f C  config=0x%02X\n", raw, t, cfg);
}

void probeDS(uint8_t p) {
  pinMode(p, INPUT_PULLUP);
  delay(5);
  Serial.printf("[1-Wire GPIO%d] idle=%d ", p, digitalRead(p));
  bool pres = owReset(p);
  Serial.printf("presence=%d ", pres);
  if (!pres) { Serial.println("(无应答)"); return; }
  owWriteByte(p, 0xCC); owWriteByte(p, 0x44);   // skip ROM + convert
  delay(750);
  if (!owReset(p)) { Serial.println("(第二次复位无应答)"); return; }
  owWriteByte(p, 0xCC); owWriteByte(p, 0xBE);   // skip ROM + read scratchpad
  uint8_t b[9];
  for (uint8_t i = 0; i < 9; i++) b[i] = owReadByte(p);
  Serial.print("bytes=");
  for (uint8_t i = 0; i < 9; i++) Serial.printf("%02X ", b[i]);
  int16_t raw = (int16_t)((b[1] << 8) | b[0]);
  uint8_t crc = 0, crcB = b[8];
  // Dallas CRC8
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t x = crc ^ b[i];
    for (uint8_t k = 0; k < 8; k++) x = (x & 1) ? ((x >> 1) ^ 0x8C) : (x >> 1);
    crc = x;
  }
  Serial.printf("| raw=0x%04X T=%.4fC crc=%s\n", (uint16_t)raw, raw * 0.0625f, (crc == crcB) ? "OK" : "BAD");
  Serial.println("  (bytes 全 FF = 数据线读到的全是 1 → 上拉不足/线太长/接触不良)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== PROBE START ===");
  Serial.printf("chip=%s cores=%d flash=%dMB psram=%dMB\n",
                ESP.getChipModel(), ESP.getChipCores(),
                (int)(ESP.getFlashChipSize() / (1024 * 1024)),
                (int)(ESP.getPsramSize() / (1024 * 1024)));
  Serial.printf("free heap=%u\n", (unsigned)ESP.getFreeHeap());
}

void loop() {
  Serial.println("----------------------------------------");
  scanBus(11, 10, "A 官方顺序");
  scanBus(10, 11, "B 反接顺序");
  // 回到官方顺序做后续探测
  Wire.end(); Wire.begin(11, 10); Wire.setClock(100000); delay(20);

  Serial.println("[探测 0x48]");
  probeMax();

  Serial.println("[探测 DS18B20]");
  probeDS(17);

  Serial.println("=== PROBE END (5s 后重来) ===\n");
  delay(5000);
}
