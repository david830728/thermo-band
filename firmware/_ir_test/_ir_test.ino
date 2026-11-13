// _ir_test · ESP32-C3 红外收发自检（发射 + 1838 接收）
//
// 目的：不依赖空调，先证明「发得出、收得到、码正确」这条链路成立。
//
// 接线：
//   IR Transmitter 模块： VCC->3V3  GND->GND  DAT->GPIO3
//   IR Receiver  模块： VCC->3V3  GND->GND  OUT->GPIO4
//   两个模块相距 10~30cm 面对面（太近接收头会饱和）
//
// 串口命令：
//   n = 立即发一次 NEC 0x00FFA25D
//   t = 持续输出高电平 1 秒（用手机摄像头看 LED 是否亮，判断极性）
//   s = 输出 38kHz 载波 1 秒（同上，用来分辨模块是否需要外部调制）
//   a = 每 3 秒自动发码（默认开启）
//
// 判读：
//   ① 手机摄像头对准发射管，发码时应看到紫色/白色闪烁 → 发射正常
//   ② 串口出现 [RX] NEC 0x00FFA25D → 接收与整个链路正常
//   ③ 摄像头看不到任何闪烁 → 极性反了或引脚错，用 t 命令逐项排查

#include <Arduino.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

#define PIN_IR_TX 2      // 与 3V3/GND 相邻（板子下组 IO3 IO2 3V3 GND），模块可直接插
#define PIN_IR_RX 5      // 与 3V3/GND 相邻（板上组 IO4 IO5 3V3 GND）

IRsend irsend(PIN_IR_TX);
IRrecv irrecv(PIN_IR_RX, 1024, 15, true);
decode_results results;

static bool autoSend = true;
static uint32_t lastAuto = 0;
static uint32_t txN = 0, rxN = 0;
static uint32_t lastRxMs = 0;

void sendTest() {
  txN++;
  Serial.printf("[TX %lu] NEC 0x00FFA25D\n", (unsigned long)txN);
  irsend.sendNEC(0x00FFA25DUL, 32);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== _ir_test · ESP32-C3 红外收发自检 ===");
  Serial.printf("芯片: %s  rev %d\n", ESP.getChipModel(), (int)ESP.getChipRevision());

  irsend.begin();
  Serial.printf("IRsend 就绪  TX=GPIO%d\n", PIN_IR_TX);

  // 先看 RX 引脚的静态电平：1838 接收头通电且空闲时 OUT 应为高电平(≈3.3V)
  // 读成 0 或乱跳 -> 要么没接、要么没供电、要么接错脚
  pinMode(PIN_IR_RX, INPUT);
  delay(50);
  int idle = 0;
  for (int i = 0; i < 20; i++) { idle += digitalRead(PIN_IR_RX); delay(2); }
  Serial.printf("RX 静态电平采样(20次高电平计数) = %d/20  -> %s\n", idle,
                idle >= 18 ? "高电平，接收头已供电" :
                idle <= 2  ? "低电平，未接/未供电/接错" : "抖动，线路不稳");

  irrecv.enableIRIn();
  Serial.printf("IRrecv 已启动  RX=GPIO%d\n", PIN_IR_RX);

  Serial.println("命令: n=发码  t=直流高1s  s=38k载波1s  a=切换自动发送");
  Serial.println("请用手机摄像头对准发射管观察\n");
}

void loop() {
  if (Serial.available()) {
    int c = Serial.read();
    if (c == 'n' || c == 'N') sendTest();
    if (c == 'a' || c == 'A') { autoSend = !autoSend; Serial.printf("自动发送 = %s\n", autoSend ? "开" : "关"); }
    if (c == 't' || c == 'T') {
      Serial.println("直流高电平 1s ...");
      digitalWrite(PIN_IR_TX, HIGH);
      pinMode(PIN_IR_TX, OUTPUT);
      digitalWrite(PIN_IR_TX, HIGH);
      delay(1000);
      pinMode(PIN_IR_TX, INPUT);
      Serial.println("结束");
    }
    if (c == 's' || c == 'S') {
      Serial.println("38kHz 载波 1s ...");
      irsend.enableIROut(38);
      irsend.mark(1000 * 1000);
      irsend.space(1);
      Serial.println("结束");
    }
  }

  if (autoSend && (uint32_t)(millis() - lastAuto) >= 3000) {
    lastAuto = millis();
    sendTest();
  }

  if (irrecv.decode(&results)) {
    // 即使解不出协议也要打印：rawlen>1 说明接收头确实看到了红外脉冲
    rxN++;
    lastRxMs = millis();
    Serial.printf("[RX %lu] %-12s 0x%016llX  %u bits  rawlen=%u\n",
                  (unsigned long)rxN,
                  typeToString(results.decode_type, false).c_str(),
                  (unsigned long long)results.value,
                  (unsigned)results.bits,
                  (unsigned)results.rawlen);
    irrecv.resume();
  }
}
