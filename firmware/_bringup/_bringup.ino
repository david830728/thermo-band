// T0.2 + T1.1 bring-up (v2) -- Waveshare ESP32-S3-Touch-LCD-1.69 + MAX30205
// - boot banner: chip / flash / psram
// - full I2C scan every 20s  (not only at boot, so it can be read at any time)
// - detailed I2C error codes for 0x48 (0=ACK, 2=addr NACK, 3=data NACK)
// - temperature read every 2s
#include <Wire.h>

#define PIN_SDA   11
#define PIN_SCL   10
#define MAX_ADDR  0x48
#define LSB_C     0.00390625f

uint8_t i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission();
}

void scanBus() {
  Serial.println("--- i2c scan ---");
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cProbe(a) == 0) {
      Serial.printf("  found 0x%02X", a);
      switch (a) {
        case 0x15: Serial.print("  <- CST816T touch"); break;
        case 0x48: Serial.print("  <- *** MAX30205 temp ***"); break;
        case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
          Serial.print("  <- MAX30205 variant (A0/A1/A2 strapped)"); break;
        case 0x51: Serial.print("  <- PCF85063 RTC"); break;
        case 0x6B: Serial.print("  <- QMI8658 IMU"); break;
      }
      Serial.println();
      n++;
    }
  }
  Serial.printf("--- %u device(s) on bus ---\n", n);
  if (i2cProbe(MAX_ADDR) != 0) {
    Serial.println("!! 0x48 no ACK -> check VCC=3V3, SDA=GPIO11, SCL=GPIO10, GND, A0-A2 floating");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== BOOT OK ===");
  Serial.printf("chip=%s cores=%d rev=%d\n", ESP.getChipModel(), ESP.getChipCores(), ESP.getChipRevision());
  Serial.printf("flash=%dMB psram=%dMB (free %u) heap=%u\n",
                ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / (1024 * 1024),
                (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeHeap());
  Serial.printf("sdk=%s\n", ESP.getSdkVersion());
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(50);
  scanBus();
  Serial.println("--- temp every 2s, i2c scan every 20s ---");
}

void loop() {
  static uint32_t n = 0;
  n++;
  unsigned long up = millis() / 1000;

  uint8_t e = i2cProbe(MAX_ADDR);
  if (e == 0) {
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(0x00);
    uint8_t e2 = Wire.endTransmission(false);
    uint8_t got = Wire.requestFrom((int)MAX_ADDR, 2);
    if (e2 == 0 && got == 2) {
      uint16_t hi = Wire.read(), lo = Wire.read();
      uint16_t raw = (uint16_t)((hi << 8) | lo);
      Serial.printf("[%5lus] t=%u raw=0x%04X  %.3f C\n", up, (unsigned)raw, raw, (int16_t)raw * LSB_C);
    } else {
      Serial.printf("[%5lus] 0x48 ACK but read failed (endTx=%u requestFrom=%u)\n", up, e2, got);
    }
  } else {
    Serial.printf("[%5lus] 0x48 NACK (endTx=%u)  0=ok 2=addrNACK 3=dataNACK 5=timeout\n", up, e);
  }

  if (n % 10 == 0) scanBus();
  delay(2000);
}
