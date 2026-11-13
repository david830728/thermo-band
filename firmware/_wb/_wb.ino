// Wristband dashboard v2 -- M601 integrated (2026-11-12)
//
// 相比 _ui 的三处关键改动：
//  1) 1-Wire 层从 digitalWrite/pinMode 换成寄存器级 GPIO_MODE_INPUT_OUTPUT_OD
//     （digitalWrite 在 S3 上要过 IDF 层，调用开销抖动几 us，正是 presence 漏检的根源）
//  2) presence 判定改为 200us 轮询窗口（本板实测器件在释放后 36~40us 才拉低）
//  3) 温度公式改为 T = 40 + raw/256（敏源 M601，不是 DS18B20 的 raw*0.0625）
//
// 显示：M601 主测温 (大字号, 按温度分级着色) / MAX30205 对照 / I2C 数 / 1-Wire 脚 / uptime
#include <SPI.h>
#include <Wire.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>

// ---------------- ESP-NOW 上报 ----------------
// 手环作为「传感器节点」，每秒把皮温广播出去；C3 中枢负责接收。
// 两边都用 WiFi 信道 1（ESP-NOW 不需要路由器，但信道必须一致）。
#define ESPNOW_CH 1

typedef struct __attribute__((packed)) {
  uint8_t  magic;      // 0x5A 固定
  uint8_t  ver;        // 协议版本
  uint8_t  seq;        // 序号，丢包可见
  uint8_t  flags;      // bit0 = M601 读数有效
  float    skinT;      // 皮温 ℃
  int16_t  raw;        // M601 原始值（调试用）
  uint16_t crc;        // 校验
} WbMsg;

static WbMsg    gMsg;
static uint8_t  gBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool     gNowReady = false;
static bool     gLastQueued = false;
static uint32_t gTxOk = 0, gTxFail = 0;

// ---------------- 下行包（C3 → 手环）----------------
// 手环上没有任何环境传感器，也没有联网校时的条件（一联网就会改 WiFi 信道、
// 把 ESP-NOW 打断），所以室温/湿度/空调状态/时间全部靠中枢下发。
// 字段顺序必须与 _c3_hub 里的 HubMsg 完全一致，改一边就要改另一边。
#define HUB_MAGIC 0x5B

typedef struct __attribute__((packed)) {
  uint8_t  magic;      // 0x5B
  uint8_t  ver;
  uint8_t  seq;
  uint8_t  acMode;
  float    roomT;
  float    roomRH;
  uint8_t  acSet;
  uint8_t  acOn;
  uint32_t epochSec;   // 0 = 中枢尚未校时
  uint16_t crc;
} HubMsg;

static HubMsg  gHubRx;
static bool    gHubOk    = false;     // 是否至少收到过一包
static uint32_t gHubMs   = 0;         // 最后一包的时刻
static uint32_t gHubCnt  = 0, gHubBad = 0;
#define HUB_TIMEOUT_MS 8000UL         // 超过 8s 没收到 → 视为下行断链

static uint16_t msgCrc(const uint8_t *p, int n) {
  uint16_t c = 0x1D0F;
  for (int i = 0; i < n; i++) { c ^= p[i]; c = (uint16_t)((c << 1) | (c >> 15)); }
  return c;
}

// 下行回调：只做「拷贝 + 校验 + 记时间」，绝不在这里画屏
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onHubMsg(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
#else
static void onHubMsg(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
#endif
  if (len != (int)sizeof(HubMsg)) { gHubBad++; return; }
  HubMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != HUB_MAGIC) { gHubBad++; return; }
  if (m.crc != msgCrc((const uint8_t *)&m, sizeof(m) - 2)) { gHubBad++; return; }
  gHubRx  = m;
  gHubMs  = millis();
  gHubCnt++;
  gHubOk  = true;
}

// 下行是否在线（8s 没包就当断链）
static bool hubAlive() { return gHubOk && (millis() - gHubMs < HUB_TIMEOUT_MS); }

// epoch → 本地时间字符串（东八区）
static void fmtClock(uint32_t e, char *buf, size_t n) {
  if (e == 0) { snprintf(buf, n, "--:--:--"); return; }
  uint32_t s = (e + 8 * 3600UL) % 86400UL;
  snprintf(buf, n, "%02u:%02u:%02u", (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

static void espNowBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(50);
  esp_wifi_set_channel(ESPNOW_CH, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init FAILED");
    gNowReady = false;
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, gBcast, 6);
  peer.channel = ESPNOW_CH;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(gBcast)) esp_now_add_peer(&peer);

  esp_now_register_recv_cb(onHubMsg);        // 接收中枢下发的状态快照

  gNowReady = true;
  Serial.printf("ESP-NOW ready  ch=%d  MAC=%s\n",
                ESPNOW_CH, WiFi.macAddress().c_str());
}

static void espNowReport(float t, int16_t raw) {
  if (!gNowReady) return;
  gMsg.magic = 0x5A;
  gMsg.ver   = 1;
  gMsg.seq++;
  gMsg.flags = (!isnan(t)) ? 0x01 : 0x00;
  gMsg.skinT = isnan(t) ? 0.0f : t;
  gMsg.raw   = raw;
  gMsg.crc   = msgCrc((const uint8_t *)&gMsg, sizeof(gMsg) - 2);
  gLastQueued = (esp_now_send(gBcast, (uint8_t *)&gMsg, sizeof(gMsg)) == ESP_OK);
  if (gLastQueued) gTxOk++; else gTxFail++;
}

#define PIN_SCK   6
#define PIN_MOSI  7
#define PIN_CS    5
#define PIN_DC    4
#define PIN_RST   8
#define PIN_BL   15

#define W 240
#define H 280
#define YOFF 20               // 240x280 panel: visible area starts at RAM row 20

#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREY   0x8410
#define C_DKGREY 0x4208
#define C_RED    0xF800
#define C_GREEN  0x07E0
#define C_BLUE   0x001F
#define C_CYAN   0x07FF
#define C_YELLOW 0xFFE0
#define C_ORANGE 0xFD20

// ---------------- 5x7 font (cols, LSB = top row) ----------------
struct Glyph { char c; uint8_t col[5]; };
static const Glyph kFont[] = {
  {' ', {0x00,0x00,0x00,0x00,0x00}},
  {'-', {0x08,0x08,0x08,0x08,0x08}},
  {'.', {0x00,0x00,0x00,0x60,0x00}},
  {':', {0x00,0x36,0x00,0x36,0x00}},
  {'/', {0x20,0x10,0x08,0x04,0x02}},
  {'0', {0x3E,0x51,0x49,0x45,0x3E}},
  {'1', {0x00,0x42,0x7F,0x40,0x00}},
  {'2', {0x42,0x61,0x51,0x49,0x46}},
  {'3', {0x21,0x41,0x45,0x4B,0x31}},
  {'4', {0x18,0x14,0x12,0x7F,0x10}},
  {'5', {0x27,0x45,0x45,0x45,0x39}},
  {'6', {0x3C,0x4A,0x49,0x49,0x30}},
  {'7', {0x01,0x71,0x09,0x05,0x03}},
  {'8', {0x36,0x49,0x49,0x49,0x36}},
  {'9', {0x06,0x49,0x49,0x29,0x1E}},
  {'A', {0x7E,0x11,0x11,0x11,0x7E}},
  {'B', {0x7F,0x49,0x49,0x49,0x36}},
  {'C', {0x3E,0x41,0x41,0x41,0x22}},
  {'D', {0x7F,0x41,0x41,0x22,0x1C}},
  {'E', {0x7F,0x49,0x49,0x49,0x41}},
  {'F', {0x7F,0x09,0x09,0x09,0x01}},
  {'G', {0x3E,0x41,0x49,0x49,0x7A}},
  {'H', {0x7F,0x08,0x08,0x08,0x7F}},
  {'I', {0x00,0x41,0x7F,0x41,0x00}},
  {'J', {0x20,0x40,0x41,0x3F,0x01}},
  {'K', {0x7F,0x08,0x14,0x22,0x41}},
  {'L', {0x7F,0x40,0x40,0x40,0x40}},
  {'M', {0x7F,0x02,0x0C,0x02,0x7F}},
  {'N', {0x7F,0x04,0x08,0x10,0x7F}},
  {'O', {0x3E,0x41,0x41,0x41,0x3E}},
  {'P', {0x7F,0x09,0x09,0x09,0x06}},
  {'Q', {0x3E,0x41,0x51,0x21,0x5E}},
  {'R', {0x7F,0x09,0x19,0x29,0x46}},
  {'S', {0x26,0x49,0x49,0x49,0x32}},
  {'T', {0x01,0x01,0x7F,0x01,0x01}},
  {'U', {0x3F,0x40,0x40,0x40,0x3F}},
  {'V', {0x1F,0x20,0x40,0x20,0x1F}},
  {'W', {0x3F,0x40,0x38,0x40,0x3F}},
  {'X', {0x63,0x14,0x08,0x14,0x63}},
  {'Y', {0x07,0x08,0x70,0x08,0x07}},
  {'Z', {0x61,0x51,0x49,0x45,0x43}},
  {'%', {0x62,0x64,0x08,0x13,0x23}},
  {'?', {0x02,0x01,0x51,0x09,0x06}},
};
static const uint8_t kFontN = sizeof(kFont) / sizeof(kFont[0]);

// ---------------- display low level ----------------
static inline void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
static inline void dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }

static void setWindow(int x0, int y0, int x1, int y1) {
  y0 += YOFF; y1 += YOFF;
  cmd(0x2A); dat(x0 >> 8); dat(x0 & 0xFF); dat(x1 >> 8); dat(x1 & 0xFF);
  cmd(0x2B); dat(y0 >> 8); dat(y0 & 0xFF); dat(y1 >> 8); dat(y1 & 0xFF);
  cmd(0x2C);
}
static void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (w <= 0 || h <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  setWindow(x, y, x + w - 1, y + h - 1);
  uint8_t hi = c >> 8, lo = c & 0xFF;
  digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW);
  uint32_t n = (uint32_t)w * h;
  for (uint32_t i = 0; i < n; i++) { SPI.transfer(hi); SPI.transfer(lo); }
  digitalWrite(PIN_CS, HIGH);
}
static void fillScreen(uint16_t c) { fillRect(0, 0, W, H, c); }

static void drawChar(int x, int y, char ch, int s, uint16_t col) {
  const uint8_t *g = kFont[kFontN - 1].col;   // '?' default
  for (uint8_t i = 0; i < kFontN; i++) if (kFont[i].c == ch) { g = kFont[i].col; break; }
  for (int c = 0; c < 5; c++) {
    uint8_t bits = g[c];
    for (int r = 0; r < 7; r++) {
      if (bits & (1 << r)) fillRect(x + c * s, y + r * s, s, s, col);
    }
  }
}
static int textWidth(const char *s, int scale) { return strlen(s) * 6 * scale; }
static void drawText(int x, int y, const char *s, int scale, uint16_t col) {
  while (*s) { drawChar(x, y, *s++, scale, col); x += 6 * scale; }
}
static void drawTextC(int y, const char *s, int scale, uint16_t col) {
  drawText((W - textWidth(s, scale)) / 2, y, s, scale, col);
}

// ---------------- ST7789 ----------------
static void st7789Init() {
  digitalWrite(PIN_RST, LOW); delay(20);
  digitalWrite(PIN_RST, HIGH); delay(120);
  cmd(0x01); delay(150);
  cmd(0x11); delay(120);
  cmd(0x3A); dat(0x55);
  cmd(0x36); dat(0x00);
  cmd(0xB2); dat(0x0C); dat(0x0C); dat(0x00); dat(0x33); dat(0x33);
  cmd(0xB7); dat(0x35);
  cmd(0xBB); dat(0x19);
  cmd(0xC0); dat(0x2C);
  cmd(0xC2); dat(0x01);
  cmd(0xC3); dat(0x12);
  cmd(0xC4); dat(0x20);
  cmd(0xC6); dat(0x0F);
  cmd(0xD0); dat(0xA4); dat(0xA1);
  cmd(0xE0); dat(0xD0); dat(0x04); dat(0x0D); dat(0x11); dat(0x13); dat(0x2B); dat(0x3F);
             dat(0x54); dat(0x4C); dat(0x18); dat(0x0D); dat(0x0B); dat(0x1F); dat(0x23);
  cmd(0xE1); dat(0xD0); dat(0x04); dat(0x0C); dat(0x11); dat(0x13); dat(0x2C); dat(0x3F);
             dat(0x44); dat(0x51); dat(0x2F); dat(0x1F); dat(0x1F); dat(0x20); dat(0x23);
  cmd(0x21);
  cmd(0x13);
  cmd(0x29); delay(20);
}

// ================= M601 1-Wire (寄存器级, 已实机验证) =================
static inline void owLo(uint8_t p) { gpio_set_level((gpio_num_t)p, 0); }
static inline void owHi(uint8_t p) { gpio_set_level((gpio_num_t)p, 1); }
static inline int  owRd(uint8_t p) { return gpio_get_level((gpio_num_t)p); }
static inline void owDu(uint32_t us) { esp_rom_delay_us(us); }

static void owInitPin(uint8_t p) {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << p);
  io.mode         = GPIO_MODE_INPUT_OUTPUT_OD;
  io.pull_up_en   = GPIO_PULLUP_ENABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io);
  owHi(p);
  owDu(200);
}

// 返回 presence 低电平宽度(us)，0 = 无应答
static uint32_t owReset(uint8_t p) {
  owLo(p); owDu(960);                       // tRSTL 典型值
  uint32_t t0 = micros();
  owHi(p);
  // ★ 本板实测：器件在释放后 36~40us 才拉低。窗口给 200us，绝不能只等 30us。
  bool got = false;
  while ((uint32_t)(micros() - t0) < 200) { if (!owRd(p)) { got = true; break; } }
  if (!got) { owDu(400); return 0; }
  uint32_t t1 = micros();
  while (!owRd(p) && (uint32_t)(micros() - t1) < 400) {}
  uint32_t w = micros() - t1;
  owDu(400);                                // tREC >= 480us
  return w;
}

static void owWrite(uint8_t p, uint8_t b) {
  for (int i = 0; i < 8; i++) {
    if (b & 1) { owLo(p); owDu(6);  owHi(p); owDu(60); }   // 写 1：tLOW1 6us
    else       { owLo(p); owDu(70); owHi(p); owDu(8);  }   // 写 0：tLOW0 70us
    b >>= 1;
  }
}

static uint8_t owRead(uint8_t p) {
  uint8_t v = 0;
  for (int i = 0; i < 8; i++) {
    owLo(p); owDu(2); owHi(p);
    owDu(10);                        // 采样点 = 下降沿后 12us，tRDV 5~15us
    int bit = owRd(p);
    owDu(50);                        // 补齐 tSLOT >= 60us
    if (bit) v |= (1 << i);
  }
  return v;
}

static uint8_t m601Crc8(const uint8_t *d, uint8_t n) {
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

// 成功返回 true，t = 摄氏温度（T = 40 + raw/256）
static bool m601Read(uint8_t p, float &t, int16_t &rawOut, uint8_t *crcOut) {
  uint8_t sp[9];
  noInterrupts();
  uint32_t pw1 = owReset(p);
  if (pw1) { owWrite(p, 0xCC); owWrite(p, 0x44); }     // Skip ROM + Convert T
  interrupts();
  if (crcOut) *crcOut = 0;
  if (!pw1) return false;

  delay(20);                                            // tCONV 最大 10.5ms

  noInterrupts();
  uint32_t pw2 = owReset(p);
  if (pw2) {
    owWrite(p, 0xCC); owWrite(p, 0xBE);                 // Skip ROM + Read Scratchpad
    for (int i = 0; i < 9; i++) sp[i] = owRead(p);
  }
  interrupts();
  if (!pw2) return false;

  uint8_t c = m601Crc8(sp, 8);
  if (crcOut) *crcOut = (c == sp[8]) ? 1 : 2;
  if (c != sp[8]) return false;

  int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
  rawOut = raw;
  t = 40.0f + raw / 256.0f;                             // ★ M601 核心公式
  return true;
}

// ---------------- MAX30205 ----------------
#define MAX_ADDR 0x48
#define LSB_C    0.00390625f

bool  maxOk = false;
float maxT  = NAN;
int   owPin = -1;
float m601T = NAN;
int16_t m601Raw = 0;
uint8_t i2cN = 0;

static const uint8_t kOwPins[] = {17, 18, 2, 3};   // 17 是实际接线，其余为备用
static const uint8_t kOwCnt = 4;

void rescan() {
  Wire.begin(11, 10);
  Wire.setClock(100000);
  delay(5);

  i2cN = 0; bool f48 = false;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { i2cN++; if (a == MAX_ADDR) f48 = true; }
  }
  maxOk = f48;

  bool alive = false;
  if (owPin >= 0) { owInitPin((uint8_t)owPin); alive = (owReset((uint8_t)owPin) != 0); }
  if (!alive) {
    owPin = -1;
    for (uint8_t i = 0; i < kOwCnt; i++) {
      owInitPin(kOwPins[i]);
      if (owReset(kOwPins[i])) { owPin = kOwPins[i]; break; }
    }
  }
  Serial.printf("[P] i2c=%u 0x48=%d owPin=%d\n", i2cN, maxOk, owPin);
}

bool readMax(float *out) {
  Wire.beginTransmission(MAX_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX_ADDR, 2) != 2) return false;
  uint16_t hi = Wire.read(), lo = Wire.read();
  *out = (int16_t)((hi << 8) | lo) * LSB_C;
  return true;
}

// ---------------- UI ----------------
static uint16_t tempColor(float t) {
  if (isnan(t))    return C_RED;
  if (t < 25.0f)   return C_CYAN;
  if (t < 32.0f)   return C_GREEN;
  if (t < 37.5f)   return C_YELLOW;
  return C_RED;
}

void drawFrame() {
  fillScreen(C_BLACK);
  fillRect(0, 0, W, 36, 0x18E3);              // title bar
  drawTextC(8, "WRISTBAND", 3, C_GREEN);
  fillRect(0, 36, W, 2, C_DKGREY);

  drawText(10, 44, "M601", 2, C_GREY);
  drawText(10, 92, "ROOM", 2, C_GREY);      // 室温来自中枢下行包，手环自己没有环境传感器
  drawText(10, 140, "HUMI", 2, C_GREY);     // 湿度同样来自下行包

  fillRect(0, 182, W, 2, C_DKGREY);
  drawText(10, 188, "I2C", 2, C_GREY);
  drawText(10, 208, "1WIRE", 2, C_GREY);
  drawText(10, 228, "UPTIME", 2, C_GREY);
}

void drawValue(int y, float v, bool ok, uint16_t col) {
  char buf[12];
  if (!ok || isnan(v)) snprintf(buf, sizeof(buf), "NO");
  else                 snprintf(buf, sizeof(buf), "%.1f", v);
  fillRect(110, y, 118, 26, C_BLACK);
  drawText(114, y + 2, buf, 3, col);
  if (ok && !isnan(v)) drawText(114 + textWidth(buf, 3) + 4, y + 2, "C", 3, C_GREY);
}

// 湿度：不带小数（DHT11 本来就是 1% 分辨率），单位 %
void drawHum(int y, float v, bool ok, uint16_t col) {
  char buf[12];
  if (!ok || isnan(v)) snprintf(buf, sizeof(buf), "NO");
  else                 snprintf(buf, sizeof(buf), "%.0f", v);
  fillRect(110, y, 118, 26, C_BLACK);
  drawText(114, y + 2, buf, 3, col);
  if (ok && !isnan(v)) drawText(114 + textWidth(buf, 3) + 4, y + 2, "%", 3, C_GREY);
}

void drawStatus() {
  static float lastM = 1e9f;
  static uint8_t lastN = 0xFF;
  static int lastPin = -999;
  static uint32_t lastUp = 0xFFFFFFFF;
  char buf[16];

  // 主测温：M601
  bool chM = (isnan(m601T) != isnan(lastM)) || (!isnan(m601T) && fabsf(m601T - lastM) >= 0.05f);
  if (chM) {
    lastM = m601T;
    drawValue(60, m601T, (owPin >= 0) && !isnan(m601T), tempColor(m601T));
  }
  // 室温 / 湿度：来自中枢下行包（手环自己没有环境传感器）
  static bool  lastHa = false;
  static float lastRoom = 1e9f, lastRH = 1e9f;
  bool ha = hubAlive();
  bool downChanged = (ha != lastHa);
  if (downChanged || (ha && fabsf(gHubRx.roomT - lastRoom) >= 0.05f)) {
    lastRoom = gHubRx.roomT;
    drawValue(108, ha ? gHubRx.roomT : NAN, ha, ha ? C_CYAN : C_RED);
  }
  if (downChanged || (ha && fabsf(gHubRx.roomRH - lastRH) >= 0.5f)) {
    lastRH = gHubRx.roomRH;
    drawHum(156, ha ? gHubRx.roomRH : NAN, ha, ha ? C_CYAN : C_RED);
  }
  lastHa = ha;
  if (i2cN != lastN) {
    lastN = i2cN;
    snprintf(buf, sizeof(buf), "%u", i2cN);
    fillRect(110, 188, 118, 20, C_BLACK);
    drawText(114, 188, buf, 2, C_CYAN);
  }
  if (owPin != lastPin) {
    lastPin = owPin;
    fillRect(110, 208, 118, 20, C_BLACK);
    if (owPin >= 0) { snprintf(buf, sizeof(buf), "GPIO%d", owPin); drawText(114, 208, buf, 2, C_CYAN); }
    else            drawText(114, 208, "NONE", 2, C_RED);
  }
  uint32_t up = millis() / 1000;
  if (up != lastUp) {
    lastUp = up;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(up / 60), (unsigned long)(up % 60));
    fillRect(110, 228, 118, 20, C_BLACK);
    drawText(114, 228, buf, 2, C_WHITE);
  }
  // 右上角 ESP-NOW 状态灯：绿=数据已发出，红=未就绪/排队失败
  static int lastLink = -1;
  int link = (!gNowReady) ? 0 : (gLastQueued ? 2 : 1);
  if (link != lastLink) {
    lastLink = link;
    fillRect(212, 10, 18, 18, 0x18E3);
    fillRect(214, 12, 14, 14, link == 2 ? C_GREEN : (link == 1 ? C_YELLOW : C_RED));
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.printf("\n=== WRISTBAND v2 BOOT  reset_reason=%d ===\n", (int)esp_reset_reason());

  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_DC, OUTPUT); digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL, OUTPUT); digitalWrite(PIN_BL, HIGH);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  SPI.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  st7789Init();
  Serial.println("display init done");

  fillScreen(C_RED);   delay(200);
  fillScreen(C_GREEN); delay(200);
  fillScreen(C_BLUE);  delay(200);

  drawFrame();

  owInitPin(17);                    // 先按实际接线初始化，再交给 rescan 确认
  rescan();

  espNowBegin();                    // 起 ESP-NOW（不依赖路由器，仅需信道一致）
  drawStatus();
  Serial.println("--- loop ---");
}

void loop() {
  static uint32_t n = 0;
  n++;

  // 串口 'B'：软件进入 ROM 下载模式（下次复位由 RTC 强拉，无需按 BOOT/RST）
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'B') {
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
      Serial.println("→ 写入 FORCE_DOWNLOAD_BOOT，3 秒后重启进下载模式");
      delay(300);
      REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
      delay(100);
      esp_restart();
#else
      Serial.println("本内核未提供 FORCE_DOWNLOAD_BOOT，需手动 BOOT+RST");
#endif
    }
  }

  if (owPin >= 0) {
    float t; int16_t raw; uint8_t crc = 0;
    if (m601Read((uint8_t)owPin, t, raw, &crc)) { m601T = t; m601Raw = raw; }
    else                                       { m601T = NAN; }
    Serial.printf("[%5lus] m601=%.3fC raw=0x%04X crc=%u  max=%s  i2c=%u  pin=%d\n",
                  (unsigned long)(millis() / 1000), m601T, (uint16_t)m601Raw, crc,
                  maxOk ? "ok" : "no", i2cN, owPin);
  } else {
    m601T = NAN;
    Serial.printf("[%5lus] m601=NO-DEVICE\n", (unsigned long)(millis() / 1000));
  }

  if (maxOk) { float t; maxT = readMax(&t) ? t : NAN; } else maxT = NAN;

  espNowReport(m601T, m601Raw);     // 每秒把皮温广播给 C3 中枢
  Serial.printf("      ESP-NOW tx=%lu fail=%lu seq=%u queued=%d\n",
                (unsigned long)gTxOk, (unsigned long)gTxFail,
                (unsigned)gMsg.seq, (int)gLastQueued);

  // 每 5 秒汇报一次下行包内容（证明中枢状态已经进到手腕上）
  if (n % 5 == 0) {
    if (hubAlive()) {
      char clk[12];
      fmtClock(gHubRx.epochSec, clk, sizeof(clk));
      Serial.printf("      HUB rx=%lu bad=%lu 室温=%.1fC RH=%.0f%% 空调=%uC/%s 时间=%s\n",
                    (unsigned long)gHubCnt, (unsigned long)gHubBad,
                    gHubRx.roomT, gHubRx.roomRH, (unsigned)gHubRx.acSet,
                    gHubRx.acOn ? "开" : "关", clk);
    } else {
      Serial.printf("      HUB 下行断链 rx=%lu bad=%lu（检查 C3 是否上电、同信道）\n",
                    (unsigned long)gHubCnt, (unsigned long)gHubBad);
    }
  }

  drawStatus();
  if (n % 8 == 0) rescan();
  delay(1000);
}
