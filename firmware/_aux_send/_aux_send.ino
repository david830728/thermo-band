// _aux_send · 奥克斯（AUX）空调红外发射固件  ESP32-C3
//
// 协议要点（已由实机录制 + 公开规范双向验证）：
//   13 字节，104 bit，NEC 风格时序（低比特在先 LSB first）
//   头: 9000µs mark / 4500µs space
//   bit: mark 560µs + space(0:560µs / 1:1690µs)
//   b0  = 0xC3 固定
//   b1  = (设定温度 - 8) << 3 | 上下风向(0~7)
//   b2  = 左右摆风 (0x00 开 / 0xE0 关)
//   b4  = 风速 (0xE0 自动 / 0x60 低 / 0x20 中 / 0xA0 高)
//   b6  = 模式 (0x20 制冷 / 0x80 制热 / 0xC0 送风 / 0x00 自动)
//   b9  = 开关 (0x20 开 / 0x00 关)
//   b11 = 按键 ID（沿用实测 0x06）
//   b12 = 前 12 字节累加和 & 0xFF
//
// 串口命令（换行结束）:
//   26 / 27 / 28 ...  设定制冷温度并发送（范围 16~30）
//   on                按当前温度开机（制冷）
//   off               关机
//   heat <温度>       制热（例: heat 26）
//   fan0/1/2/3        风速 自动/低/中/高
//   p                 打印当前 13 字节帧（不发送）
//   s                 重新发送当前帧

#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

#define PIN_IR_TX 2
#define PIN_IR_RX 5

IRsend irsend(PIN_IR_TX);
IRrecv irrecv(PIN_IR_RX, 1024, 15, true);
decode_results res;

static uint8_t gTemp = 26;      // 设定温度
static uint8_t gFan  = 0xA0;    // 默认高速
static uint8_t gMode = 0x20;    // 默认制冷
static bool    gPower = true;   // 默认开

static uint8_t frame[13];
static uint16_t raw[224];
static char cmd[16];
static uint8_t cmdLen = 0;

// 按当前状态生成 13 字节帧
static void buildFrame() {
  frame[0]  = 0xC3;
  frame[1]  = (uint8_t)(((gTemp - 8) << 3) & 0xF8);  // 温度 + 上下风向=0
  frame[2]  = 0xE0;                                   // 左右摆风关
  frame[3]  = 0x00;
  frame[4]  = gFan;
  frame[5]  = 0x00;
  frame[6]  = gMode;
  frame[7]  = 0x00;
  frame[8]  = 0x00;
  frame[9]  = gPower ? 0x20 : 0x00;
  frame[10] = 0x00;
  frame[11] = 0x06;                                   // 按键 ID
  uint8_t sum = 0;
  for (int i = 0; i < 12; i++) sum += frame[i];
  frame[12] = sum;
}

static void printFrame() {
  Serial.print("FRAME: ");
  for (int i = 0; i < 13; i++) {
    Serial.printf("%02X%s", frame[i], i < 12 ? " " : "");
  }
  Serial.printf("   [temp=%u fan=%02X mode=%02X power=%d]\n", gTemp, gFan, gMode, (int)gPower);
}

// 用 sendRaw 精确还原时序（库里没有 AUX 协议，只能自己拼）
static void sendFrame() {
  buildFrame();
  int n = 0;
  raw[n++] = 9000;
  raw[n++] = 4500;
  for (int i = 0; i < 13; i++) {
    for (int b = 0; b < 8; b++) {
      raw[n++] = 560;
      raw[n++] = ((frame[i] >> b) & 1) ? 1690 : 560;
    }
  }
  raw[n++] = 560;                       // 结束 mark
  uint32_t t0 = micros();
  irsend.sendRaw(raw, n, 38);
  uint32_t dt = micros() - t0;
  printFrame();
  Serial.printf("已发送 (%d 段时序, 耗时 %lu µs)\n", n, (unsigned long)dt);
  delay(40);
  verifyFrame();
}

// 自检：用板载 1838 接收头把刚发出的波形读回来，按位还原成字节与 frame[] 比对
static void verifyFrame() {
  if (!irrecv.decode(&res)) {
    Serial.println("自检: 未收到自己的信号（接收头没对着发射管？）");
    return;
  }
  uint16_t *a = resultToRawArray(&res);
  uint16_t n = res.rawlen - 1;
  uint16_t hMark = (n > 0) ? a[0] : 0;
  uint16_t hSpace = (n > 1) ? a[1] : 0;
  uint8_t got[13] = {0};
  int bi = 0;
  for (uint16_t i = 2; i + 1 < n && bi < 104; i += 2) {
    bool one = a[i + 1] > 1100;          // 间隔 >1.1ms 记为 1
    if (one) got[bi / 8] |= (uint8_t)(1 << (bi % 8));
    bi++;
  }
  delete[] a;
  irrecv.resume();

  Serial.printf("自检: 收到 %d bit, 头 %u/%u µs\n", bi, hMark, hSpace);
  Serial.print("      回读: ");
  for (int i = 0; i < 13; i++) Serial.printf("%02X%s", got[i], i < 12 ? " " : "");
  bool ok = (bi == 104);
  for (int i = 0; i < 13 && ok; i++) if (got[i] != frame[i]) ok = false;
  Serial.println(ok ? "  ✔ 与发送一致" : "  ✘ 不一致");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== _aux_send · 奥克斯空调发射器 ===");
  Serial.printf("TX=GPIO%d  RX=GPIO%d(自检)  默认: 制冷 %u℃ 风速高\n", PIN_IR_TX, PIN_IR_RX, gTemp);
  irsend.begin();
  irrecv.enableIRIn();
  buildFrame();
  printFrame();
  Serial.println("命令: 26/27/28=设温度  on  off  heat<空格>26  fan0~3  p  s");
}

static void handleCmd() {
  cmd[cmdLen] = 0;
  if (cmdLen == 0) return;

  // 纯数字 = 设定温度
  bool allDigit = true;
  for (uint8_t i = 0; i < cmdLen; i++) if (cmd[i] < '0' || cmd[i] > '9') allDigit = false;
  if (allDigit) {
    int t = atoi(cmd);
    if (t < 16 || t > 30) { Serial.println("温度范围 16~30"); return; }
    gTemp = t; gPower = true;
    sendFrame();
    return;
  }
  if (!strcmp(cmd, "on"))  { gPower = true;  sendFrame(); return; }
  if (!strcmp(cmd, "off")) { gPower = false; sendFrame(); return; }
  if (!strcmp(cmd, "p"))   { buildFrame(); printFrame(); return; }
  if (!strcmp(cmd, "s"))   { sendFrame(); return; }
  if (!strncmp(cmd, "heat ", 5)) { gMode = 0x80; gPower = true; gTemp = atoi(cmd + 5); sendFrame(); return; }
  if (!strcmp(cmd, "fan0")) { gFan = 0xE0; sendFrame(); return; }
  if (!strcmp(cmd, "fan1")) { gFan = 0x60; sendFrame(); return; }
  if (!strcmp(cmd, "fan2")) { gFan = 0x20; sendFrame(); return; }
  if (!strcmp(cmd, "fan3")) { gFan = 0xA0; sendFrame(); return; }
  Serial.printf("未知命令: %s\n", cmd);
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      handleCmd();
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }
}
