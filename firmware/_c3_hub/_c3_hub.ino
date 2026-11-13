// _c3_hub · 环境温湿度采集 + 奥克斯空调红外中枢  ESP32-C3
//
// 这一版完成两件事：
//   1) DHT11 读室内温湿度（GPIO4），每 2 秒打印一次
//   2) 奥克斯空调红外发射（GPIO2），保留完整控制能力
//
// 硬件接线：
//   DHT11  VCC -> 3V3      GND -> GND      DATA -> GPIO6
//   红外发射模块 VCC -> 5V(必须 5V，3V3 射程不够)  GND -> GND  DAT -> GPIO2
//   红外接收模块 VCC -> 3V3  GND -> GND  OUT -> GPIO5（可选，用于自检/监听遥控器）
//
// 串口命令（换行结束）：
//   26 / 27 / 28 ...   设定制冷温度并发送（16~30）
//   on / off           开机 / 关机
//   heat <温度>        制热
//   fan0/1/2/3         风速 自动/低/中/高
//   ac <温度>          等价于直接设温度（给学生代码调用更直观的名字）
//   env                立即读一次温湿度
//   cal / cal -3       查看 / 设置温度校准偏移
//   p                  打印当前 13 字节帧（不发送）
//   s                  重发当前帧
//   h                  打印本帮助
//
// ★ 给写温控逻辑的同学：直接调用下面这两个函数即可，不用关心红外细节
//     float getRoomTemp();       // 返回室内温度 ℃，读取失败返回 NAN
//     float getRoomHumidity();   // 返回室内湿度 %
//     void  acSet(uint8_t t);    // 设成制冷 + 温度 t 并立即发送
//     void  acPower(bool on);    // 开/关机
//   详见文件末尾“对外接口”一节。

#include <Arduino.h>
#include <driver/gpio.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

// ================= 引脚 =================
#define PIN_DHT   6     // DHT11 DATA（右列 IO6，避开被红外模块占用的 IO2/IO4/IO5）
#define PIN_IR_TX 2     // 红外发射（模块 VCC 必须接 5V）
#define PIN_IR_RX 5     // 红外接收（自检/监听，可不用）

IRsend irsend(PIN_IR_TX);
IRrecv irrecv(PIN_IR_RX, 1024, 15, true);
decode_results res;

// ================= ESP-NOW（接收手环上报） =================
// 手环广播 {magic=0x5A, ver, seq, flags, skinT, raw, crc}，这里校验后缓存。
// 回调里只做"存值"，绝不做耗时操作（C3 单核，回调与 loop 同核）。
#define ESPNOW_CH 1

typedef struct __attribute__((packed)) {
  uint8_t  magic;
  uint8_t  ver;
  uint8_t  seq;
  uint8_t  flags;      // bit0 = 手环 M601 读数有效
  float    skinT;
  int16_t  raw;
  uint16_t crc;
} WbMsg;

static volatile float    gPeerT   = NAN;
static volatile uint8_t  gPeerFlags = 0;
static volatile uint8_t  gPeerSeq = 0;
static volatile uint32_t gPeerMs  = 0;      // 收到时刻
static volatile uint32_t gPeerCnt = 0, gPeerBad = 0, gPeerMiss = 0;
static bool gNowReady = false;

static uint16_t msgCrc(const uint8_t *p, int n) {
  uint16_t c = 0x1D0F;
  for (int i = 0; i < n; i++) { c ^= p[i]; c = (uint16_t)((c << 1) | (c >> 15)); }
  return c;
}

// ================= 下行包（C3 → 手环） =================
// 上行是「手环报皮温」，下行是「中枢报全局」：室温/湿度/空调状态/时间。
// 手环上没有任何环境传感器，也没有联网校时的条件（连 WiFi 会改信道、断掉 ESP-NOW），
// 所以这些都只能由 C3 主动告诉它。
#define HUB_MAGIC 0x5B

typedef struct __attribute__((packed)) {
  uint8_t  magic;      // 0x5B（与上行的 0x5A 区分）
  uint8_t  ver;        // 协议版本
  uint8_t  seq;
  uint8_t  acMode;     // 0x20 制冷 / 0x80 制热 / 0xC0 送风
  float    roomT;      // 室温 ℃（已含校准偏移）
  float    roomRH;     // 相对湿度 %
  uint8_t  acSet;      // 空调设定温度
  uint8_t  acOn;       // 1 开机 / 0 关机
  uint32_t epochSec;   // UNIX 时间戳，0 = 尚未校时
  uint16_t crc;
} HubMsg;              // packed，实测 20 字节

static uint8_t gBcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static HubMsg  gHub;
static uint32_t gHubTxOk = 0, gHubTxFail = 0;

// 时间源：编译时刻打底（env.sh 编译时注入 -DESP32_BUILD_EPOCH=...），
// 上电后用它 + millis() 外推；也可用串口 `time <epoch>` 手动校准。
#ifndef ESP32_BUILD_EPOCH
#define ESP32_BUILD_EPOCH 0
#endif
static uint32_t gEpochBase = ESP32_BUILD_EPOCH;
static uint32_t gEpochMs   = 0;
static bool     gEpochOk   = (ESP32_BUILD_EPOCH > 0);

static uint32_t nowEpoch() {
  if (!gEpochOk) return 0;
  return gEpochBase + (millis() - gEpochMs) / 1000;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
#else
static void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
#endif
  if (len != (int)sizeof(WbMsg)) { gPeerBad++; return; }
  WbMsg m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != 0x5A) { gPeerBad++; return; }
  if (m.crc != msgCrc((const uint8_t *)&m, sizeof(m) - 2)) { gPeerBad++; return; }
  if (gPeerCnt > 0) {
    uint8_t gap = (uint8_t)(m.seq - gPeerSeq - 1);
    if (gap > 0 && gap < 100) gPeerMiss += gap;
  }
  gPeerT     = (m.flags & 0x01) ? m.skinT : NAN;
  gPeerFlags = m.flags;
  gPeerSeq   = m.seq;
  gPeerMs    = millis();
  gPeerCnt++;
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
  esp_now_register_recv_cb(onRecv);

  // 下行也要发，所以得先把广播地址注册成 peer（只在发送时必需）
  esp_now_peer_info_t bpeer = {};
  memcpy(bpeer.peer_addr, gBcast, 6);
  bpeer.channel = ESPNOW_CH;
  bpeer.encrypt = false;
  if (!esp_now_is_peer_exist(gBcast)) esp_now_add_peer(&bpeer);

  gNowReady = true;
  Serial.printf("ESP-NOW ready  ch=%d  MAC=%s（手环需同一信道）\n",
                ESPNOW_CH, WiFi.macAddress().c_str());
}

// ================= DHT11 =================
// 协议：MCU 拉低 >=18ms → 释放 → 应答 80us 低 + 80us 高 → 40bit
//       每位 50us 低 + (26~28us 高=0 / 70us 高=1)
//       5 字节：湿度整数 湿度小数 温度整数 温度小数 校验和
// ★ 关键：整段时序必须关中断。C3 的 tick 中断 / RMT(红外接收)中断会插进
//   26us 的位宽里，把 0 误判成 1（表现为小数位乱跳、校验偶发失败）。
//   40 位最长约 4.8ms，UART 有 128 字节硬件 FIFO，关中断不会丢串口数据。
#define DR(p) gpio_get_level((gpio_num_t)(p))

static bool readDHT(uint8_t pin, uint8_t *d) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delay(20);                       // 起始信号：拉低 >=18ms
  pinMode(pin, INPUT_PULLUP);      // 释放，等应答
  delayMicroseconds(30);

  noInterrupts();

  uint32_t t0 = micros();
  while (DR(pin) == HIGH) {                          // 等应答低电平
    if (micros() - t0 > 200) { interrupts(); return false; }
  }
  t0 = micros();
  while (DR(pin) == LOW) {                           // 应答低 80us
    if (micros() - t0 > 150) { interrupts(); return false; }
  }
  t0 = micros();
  while (DR(pin) == HIGH) {                          // 应答高 80us
    if (micros() - t0 > 150) { interrupts(); return false; }
  }

  for (uint8_t i = 0; i < 5; i++) {
    d[i] = 0;
    for (uint8_t b = 0; b < 8; b++) {
      t0 = micros();
      while (DR(pin) == LOW) {                       // 每位起始 50us 低
        if (micros() - t0 > 100) { interrupts(); return false; }
      }
      t0 = micros();
      while (DR(pin) == HIGH) {                      // 高电平长度决定 0/1
        if (micros() - t0 > 100) { interrupts(); return false; }
      }
      uint32_t hi = micros() - t0;
      d[i] = (d[i] << 1) | (hi > 45 ? 1 : 0);
    }
  }

  interrupts();

  uint8_t sum = d[0] + d[1] + d[2] + d[3];
  return (sum == d[4]);
}

// 最近一次成功读数（失败时保留旧值，标记 valid）
// DHT11 自身分辨率 1℃ / 1%RH，且读数有 ±0.5℃ 的抖动，
// 这里做 5 点滑动平均，让输出适合做控制输入（短时噪声被压下去）
static float gRoomT   = NAN;
static float gRoomRH  = NAN;
static bool  gEnvOk   = false;
static uint32_t gEnvCount = 0, gEnvFail = 0;
static uint8_t gRaw[5] = {0};

#define ENV_WIN 5
static float bufT[ENV_WIN], bufRH[ENV_WIN];
static uint8_t bufN = 0, bufPos = 0;

static void pushEnv(float t, float rh) {
  bufT[bufPos] = t;
  bufRH[bufPos] = rh;
  bufPos = (bufPos + 1) % ENV_WIN;
  if (bufN < ENV_WIN) bufN++;
  float st = 0, sr = 0;
  for (uint8_t i = 0; i < bufN; i++) { st += bufT[i]; sr += bufRH[i]; }
  gRoomT  = st / bufN;
  gRoomRH = sr / bufN;
}

static bool sampleEnv() {
  uint8_t d[5] = {0};
  if (!readDHT(PIN_DHT, d)) { gEnvFail++; gEnvOk = false; return false; }
  memcpy(gRaw, d, 5);
  // ★ 只用整数位：实测本模块小数位在 0/3/4/5/7 之间随机跳（噪声，非真实分辨率），
  //   DHT11 的真实分辨率就是 1℃ / 1%RH。取整数位再滑动平均，才是有效数据。
  pushEnv((float)d[2], (float)d[0]);
  gEnvOk   = true;
  gEnvCount++;
  return true;
}

// ================= 奥克斯空调协议 =================
// 13 字节 / 104 bit，NEC 风格时序（LSB first），校验 = 前 12 字节累加和
//   b1  = (温度-8)<<3 | 上下风向
//   b4  = 风速 (E0 自动 / 60 低 / 20 中 / A0 高)
//   b6  = 模式 (20 制冷 / 80 制热 / C0 送风 / 00 自动)
//   b9  = 开关 (20 开 / 00 关)
//   b11 = 按键 ID（实测 0x06）
static uint8_t gTemp  = 26;
static uint8_t gFan   = 0xA0;
static uint8_t gMode  = 0x20;
static bool    gPower = true;

static uint8_t  frame[13];
static uint16_t raw[224];

static void buildFrame() {
  frame[0]  = 0xC3;
  frame[1]  = (uint8_t)(((gTemp - 8) << 3) & 0xF8);
  frame[2]  = 0xE0;
  frame[3]  = 0x00;
  frame[4]  = gFan;
  frame[5]  = 0x00;
  frame[6]  = gMode;
  frame[7]  = 0x00;
  frame[8]  = 0x00;
  frame[9]  = gPower ? 0x20 : 0x00;
  frame[10] = 0x00;
  frame[11] = 0x06;
  uint8_t sum = 0;
  for (int i = 0; i < 12; i++) sum += frame[i];
  frame[12] = sum;
}

static void printFrame() {
  Serial.print("FRAME: ");
  for (int i = 0; i < 13; i++) Serial.printf("%02X%s", frame[i], i < 12 ? " " : "");
  Serial.printf("   [temp=%u fan=%02X mode=%02X power=%d]\n", gTemp, gFan, gMode, (int)gPower);
}

static void sendFrame() {
  buildFrame();
  int n = 0;
  raw[n++] = 9000;
  raw[n++] = 4500;
  for (int i = 0; i < 13; i++)
    for (int b = 0; b < 8; b++) {
      raw[n++] = 560;
      raw[n++] = ((frame[i] >> b) & 1) ? 1690 : 560;
    }
  raw[n++] = 560;
  irsend.sendRaw(raw, n, 38);
  Serial.printf("IR-TX > 已发送 ");
  printFrame();
}

// ================= 对外接口（给温控逻辑用） =================
// 校准偏移：DHT11 是 ±2℃ 的器件。实测本模块在室温下比 M601（±0.1℃）高 1.77℃，
// 这里默认按 -1.8 修正，让 getRoomTemp() 与标准温度计一致。
// 换位置/换模块后重新标定即可：串口 `cal -1.5` 试算，定下来改这个宏。
#define ENV_T_OFFSET  (-1.8f)
static float gTempOffset = ENV_T_OFFSET;

float getRoomTemp()     { return isnan(gRoomT) ? NAN : (gRoomT + gTempOffset); }
float getRoomHumidity() { return gRoomRH; }

// 手环皮温：超过 LINK_TIMEOUT_MS 没收到新包就返回 NAN（断链保护）
#define LINK_TIMEOUT_MS 8000UL
float getSkinTemp() {
  if (!gNowReady || gPeerCnt == 0) return NAN;
  if (millis() - gPeerMs > LINK_TIMEOUT_MS) return NAN;
  return gPeerT;
}
bool linkAlive() { return !isnan(getSkinTemp()); }

void acSet(uint8_t t) {
  if (t < 16) t = 16;
  if (t > 30) t = 30;
  gTemp = t; gMode = 0x20; gPower = true;
  sendFrame();
}

void acPower(bool on) { gPower = on; sendFrame(); }

// ================= 下行发送 =================
// 每秒发一包。字段取自本文件的实时状态，手环把整包当成「中枢状态快照」直接显示。
// 注意：这里读的是 getRoomTemp()（含校准偏移），不是原始 gRoomT。
static void hubSend() {
  if (!gNowReady) return;

  gHub.magic   = HUB_MAGIC;
  gHub.ver     = 1;
  gHub.seq++;
  gHub.acMode  = gMode;
  gHub.roomT   = getRoomTemp();
  gHub.roomRH  = getRoomHumidity();
  gHub.acSet   = gTemp;
  gHub.acOn    = gPower ? 1 : 0;
  gHub.epochSec = nowEpoch();
  gHub.crc     = msgCrc((const uint8_t *)&gHub, sizeof(gHub) - 2);

  if (esp_now_send(gBcast, (uint8_t *)&gHub, sizeof(gHub)) == ESP_OK) gHubTxOk++;
  else                                                               gHubTxFail++;
}

static void printHub() {
  char clk[12] = "--:--:--";
  if (gEpochOk) {
    uint32_t s = (gEpochBase + (millis() - gEpochMs) / 1000 + 8 * 3600) % 86400;
    snprintf(clk, sizeof(clk), "%02u:%02u:%02u",
             (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
  }
  Serial.printf("HUB > 室温=%.1fC RH=%.0f%%  空调=%uC/%s  时间=%s  tx=%lu fail=%lu\n",
                gHub.roomT, gHub.roomRH, (unsigned)gHub.acSet, gHub.acOn ? "开" : "关",
                clk, (unsigned long)gHubTxOk, (unsigned long)gHubTxFail);
}

// ================= 串口命令 =================
static char cmd[24];
static uint8_t cmdLen = 0;

static void printEnv() {
  if (gEnvOk)
    Serial.printf("ENV: T=%.1f C (修正 %+.1f → %.1f C)  RH=%.1f %%  [raw %02X %02X %02X %02X %02X]  ok=%lu fail=%lu\n",
                  gRoomT, gTempOffset, getRoomTemp(), gRoomRH,
                  gRaw[0], gRaw[1], gRaw[2], gRaw[3], gRaw[4],
                  (unsigned long)gEnvCount, (unsigned long)gEnvFail);
  else
    Serial.printf("ENV: 读取失败 (ok=%lu fail=%lu) —— 检查 DHT11 VCC/GND/DATA(GPIO%d)\n",
                  (unsigned long)gEnvCount, (unsigned long)gEnvFail, PIN_DHT);
}

static void handleCmd() {
  cmd[cmdLen] = 0;
  if (cmdLen == 0) return;

  bool allDigit = true;
  for (uint8_t i = 0; i < cmdLen; i++) if (cmd[i] < '0' || cmd[i] > '9') allDigit = false;
  if (allDigit) {
    int t = atoi(cmd);
    if (t < 16 || t > 30) { Serial.println("温度范围 16~30"); return; }
    acSet((uint8_t)t);
    return;
  }
  if (!strcmp(cmd, "on"))   { acPower(true);  return; }
  if (!strcmp(cmd, "off"))  { acPower(false); return; }
  if (!strcmp(cmd, "p"))    { buildFrame(); printFrame(); return; }
  if (!strcmp(cmd, "s"))    { sendFrame(); return; }
  if (!strcmp(cmd, "env")) { sampleEnv(); printEnv(); return; }
  if (!strcmp(cmd, "time")) {
    if (!gEpochOk) Serial.println("时间: 未校时（用 time <epoch> 设置）");
    else {
      uint32_t s = (nowEpoch() + 8 * 3600) % 86400;
      Serial.printf("时间: %02u:%02u:%02u  epoch=%lu  (基准 %lu + %lus)\n",
                    (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60),
                    (unsigned long)nowEpoch(), (unsigned long)gEpochBase,
                    (unsigned long)((millis() - gEpochMs) / 1000));
    }
    return;
  }
  if (!strncmp(cmd, "time ", 5)) {
    gEpochBase = (uint32_t)strtoul(cmd + 5, NULL, 10);
    gEpochMs   = millis();
    gEpochOk   = true;
    Serial.printf("时间已校准: epoch=%lu\n", (unsigned long)gEpochBase);
    return;
  }
  if (!strcmp(cmd, "cal")) {
    Serial.printf("校准偏移 = %+.1f C   （用 cal -3 设置）\n", gTempOffset);
    return;
  }
  if (!strncmp(cmd, "cal ", 4)) {
    gTempOffset = atof(cmd + 4);
    Serial.printf("校准偏移已设为 %+.1f C → 修正后室温 %.1f C\n", gTempOffset, getRoomTemp());
    return;
  }
  if (!strncmp(cmd, "ac ", 3)) { acSet((uint8_t)atoi(cmd + 3)); return; }
  if (!strncmp(cmd, "heat ", 5)) { gMode = 0x80; gPower = true; gTemp = atoi(cmd + 5); sendFrame(); return; }
  if (!strcmp(cmd, "fan0")) { gFan = 0xE0; sendFrame(); return; }
  if (!strcmp(cmd, "fan1")) { gFan = 0x60; sendFrame(); return; }
  if (!strcmp(cmd, "fan2")) { gFan = 0x20; sendFrame(); return; }
  if (!strcmp(cmd, "fan3")) { gFan = 0xA0; sendFrame(); return; }
  if (!strcmp(cmd, "h")) {
    Serial.println("命令: <16~30>=设温度  on  off  heat <t>  fan0~3  env  ac <t>  cal  time  p  s  h");
    return;
  }
  Serial.printf("未知命令: %s  （h 看帮助）\n", cmd);
}

void setup() {
  Serial.begin(115200);
  gEpochMs = millis();              // 时间外推基准点（越早取越准）
  delay(1000);
  Serial.println("\n=== _c3_hub · 温湿度采集 + 空调红外中枢 ===");
  Serial.printf("DHT11=GPIO%d   IR-TX=GPIO%d(需5V供电)   IR-RX=GPIO%d\n", PIN_DHT, PIN_IR_TX, PIN_IR_RX);
  irsend.begin();
  irrecv.enableIRIn();
  buildFrame();
  printFrame();

  // 上电先读一次环境
  delay(1200);
  sampleEnv();
  printEnv();

  espNowBegin();                    // 起 ESP-NOW 接收（手环每秒广播皮温）
  Serial.println("命令: 26/27/28  on  off  heat <t>  fan0~3  env  cal  time  p  s  h");
  if (gEpochOk)
    Serial.printf("时间基准: 编译时刻 epoch=%lu，可用 time <epoch> 精确校准\n",
                  (unsigned long)gEpochBase);
  else
    Serial.println("时间基准: 未设置，手环将显示 --:--:--（用 time <epoch> 设置）");
}

void loop() {
  // 串口命令
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handleCmd(); cmdLen = 0; }
    else if (cmdLen < sizeof(cmd) - 1) cmd[cmdLen++] = c;
  }

  // DHT11 每 2 秒采一次（DHT11 要求间隔 >=1s）
  static uint32_t lastEnv = 0;
  if (millis() - lastEnv >= 2000) {
    lastEnv = millis();
    bool ok = sampleEnv();
    if (ok) printEnv();
    else if (gEnvFail % 10 == 1) printEnv();   // 失败时不要刷屏
  }

  // 手环链路：每 2 秒报一次，断链后每 5 秒提醒一次
  static uint32_t lastLink = 0;
  static bool wasAlive = false;
  bool alive = linkAlive();
  if (millis() - lastLink >= (alive ? 2000 : 5000)) {
    lastLink = millis();
    if (alive) {
      Serial.printf("LINK: 皮温=%.2f C  seq=%u  rx=%lu miss=%lu bad=%lu\n",
                    gPeerT, (unsigned)gPeerSeq,
                    (unsigned long)gPeerCnt, (unsigned long)gPeerMiss, (unsigned long)gPeerBad);
    } else {
      Serial.printf("LINK: 无手环数据  rx=%lu bad=%lu  （检查手环是否上电、同信道 %d）\n",
                    (unsigned long)gPeerCnt, (unsigned long)gPeerBad, ESPNOW_CH);
    }
    wasAlive = alive;
  }

  // 下行：每秒把「全局状态快照」发给手环（室温/湿度/空调/时间）
  static uint32_t lastHub = 0;
  if (millis() - lastHub >= 1000) {
    lastHub = millis();
    hubSend();
    if (gHub.seq % 5 == 0) printHub();       // 5 秒打一行，别刷屏
  }
}
