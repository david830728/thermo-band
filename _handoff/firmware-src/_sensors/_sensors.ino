// Multi-sensor probe -- Waveshare ESP32-S3-Touch-LCD-1.69
// 1) boot banner: chip / flash / psram
// 2) I2C scan on GPIO11(SDA)/GPIO10(SCL)  -> looks for MAX30205 @0x48
// 3) 1-Wire search for DS18B20 on GPIO 17 / 18 / 2 / 3 (auto-detect which pin)
// 4) loop: print MAX30205 status + DS18B20 temperature every 2s
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PIN_SDA   11
#define PIN_SCL   10
#define MAX_ADDR  0x48
#define LSB_C     0.00390625f

static const uint8_t kCand[] = {17, 18, 2, 3};
static const uint8_t kCnt    = sizeof(kCand) / sizeof(kCand[0]);
OneWire           *ow[kCnt];
DallasTemperature *ds[kCnt];
int activeBus = -1;

uint8_t i2cProbe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission();
}

void scanI2C() {
  Serial.println("--- i2c scan (SDA=11 SCL=10) ---");
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cProbe(a) == 0) {
      Serial.printf("  found 0x%02X", a);
      if (a == 0x15) Serial.print("  <- CST816T touch");
      if (a == 0x48) Serial.print("  <- *** MAX30205 ***");
      if (a == 0x51) Serial.print("  <- PCF85063 RTC");
      if (a == 0x6B) Serial.print("  <- QMI8658 IMU");
      Serial.println();
      n++;
    }
  }
  Serial.printf("--- %u device(s) ---\n", n);
}

void searchDS18B20() {
  Serial.println("--- DS18B20 1-Wire search on GPIO 17/18/2/3 ---");
  for (uint8_t i = 0; i < kCnt; i++) {
    ow[i] = new OneWire(kCand[i]);
    ds[i] = new DallasTemperature(ow[i]);
    ds[i]->begin();
    uint8_t n = ds[i]->getDeviceCount();
    Serial.printf("  GPIO%-2u : %u device(s)", kCand[i], n);
    if (n > 0) {
      for (uint8_t k = 0; k < n && k < 2; k++) {
        DeviceAddress a;
        if (ds[i]->getAddress(a, k)) {
          Serial.print("  rom=");
          for (uint8_t b = 0; b < 8; b++) Serial.printf("%02X", a[b]);
        }
      }
      if (activeBus < 0) activeBus = (int)i;
    }
    Serial.println();
  }
  if (activeBus < 0) {
    Serial.println("  !! no DS18B20 found");
    Serial.println("     check: VCC->3V3, GND->GND, DO(DQ)->GPIO17, and 4.7k pull-up DO->VCC");
  } else {
    ds[activeBus]->setResolution(10);
    Serial.printf(">> DS18B20 on GPIO%u\n", kCand[activeBus]);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== BOOT OK (multi-sensor) ===");
  Serial.printf("chip=%s cores=%d flash=%dMB psram=%dMB heap=%u\n",
                ESP.getChipModel(), ESP.getChipCores(),
                ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / (1024 * 1024),
                (unsigned)ESP.getFreeHeap());

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
  delay(30);
  scanI2C();
  searchDS18B20();
  Serial.println("--- loop every 2s ---");
}

void loop() {
  unsigned long up = millis() / 1000;

  // --- DS18B20 ---
  if (activeBus >= 0) {
    ds[activeBus]->requestTemperatures();
    float t = ds[activeBus]->getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) {
      Serial.printf("[%5lus] DS18B20(GPIO%u) read FAIL (disconnected / bad pull-up)\n", up, kCand[activeBus]);
    } else {
      Serial.printf("[%5lus] DS18B20(GPIO%u) = %.2f C\n", up, kCand[activeBus], t);
    }
  } else {
    Serial.printf("[%5lus] DS18B20: not found on GPIO 17/18/2/3\n", up);
  }

  // --- MAX30205 ---
  uint8_t e = i2cProbe(MAX_ADDR);
  if (e == 0) {
    Wire.beginTransmission(MAX_ADDR);
    Wire.write(0x00);
    uint8_t e2 = Wire.endTransmission(false);
    uint8_t got = Wire.requestFrom((int)MAX_ADDR, 2);
    if (e2 == 0 && got == 2) {
      uint16_t hi = Wire.read(), lo = Wire.read();
      uint16_t raw = (uint16_t)((hi << 8) | lo);
      Serial.printf("[%5lus] MAX30205 = %.3f C\n", up, (int16_t)raw * LSB_C);
    } else {
      Serial.printf("[%5lus] MAX30205 ACK but read failed (endTx=%u got=%u)\n", up, e2, got);
    }
  } else {
    Serial.printf("[%5lus] MAX30205 0x48 NACK (endTx=%u)\n", up, e);
  }

  delay(2000);
}
