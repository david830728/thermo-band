// T0.2 smoke test -- Waveshare ESP32-S3-Touch-LCD-1.69
// Acceptance: serial shows "=== BOOT OK ===" and psram=8MB

void setup() {
  Serial.begin(115200);
  delay(1500);                       // wait USB enumeration, do not remove
  Serial.println();
  Serial.println("=== BOOT OK ===");
  Serial.printf("chip=%s cores=%d flash=%dMB psram=%dMB\n",
                ESP.getChipModel(), ESP.getChipCores(),
                ESP.getFlashChipSize() / (1024 * 1024),
                ESP.getPsramSize() / (1024 * 1024));
  Serial.printf("sdk=%s\n", ESP.getSdkVersion());
  Serial.printf("psramfree=%u\n", (unsigned)ESP.getFreePsram());
  uint64_t mac = ESP.getEfuseMac();
  Serial.printf("mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                (uint8_t)(mac >> 40), (uint8_t)(mac >> 32), (uint8_t)(mac >> 24),
                (uint8_t)(mac >> 16), (uint8_t)(mac >> 8), (uint8_t)mac);
}

void loop() {
  static uint32_t n = 0;
  Serial.printf("tick %lu  heap=%u\n", ++n, (unsigned)ESP.getFreeHeap());
  delay(1000);
}
