// _ir_learn · 空调遥控器红外码录制器（ESP32-C3）
//
// 设计要点：录到的码保存在芯片 RAM 里（最近 8 条），
// 所以"什么时候读串口"完全不重要——你可以先随便按遥控器，我事后再取。
//
// 接线（直插，无需杜邦线）：
//   红外发射模块  DAT -> GPIO2    VCC -> 3.3V   GND -> GND
//   1838 接收模块  OUT -> GPIO5    VCC -> 3.3V   GND -> GND
//
// 串口命令：
//   p  打印已录制的全部记录（协议 / 位数 / 数值 / 完整 µs 时序）
//   c  清空记录
//   1~8 重放第 n 条记录（用 sendRaw，原样重发）
//   n  发一次 NEC 0x00FFA25D 测试码
//
// 判读：
//   按遥控器后出现 [REC k] ... 行 = 已成功录到；
//   若 type 显示 UNKNOWN 但 rawlen 很大，说明协议不在库里（空调很常见），
//   这时靠 RAW 时序重放即可，不需要认识协议。

#include <Arduino.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>

#define PIN_IR_TX 2
#define PIN_IR_RX 5

#define kSlots   8      // 保留最近 8 条
#define kMaxRaw  700    // 单条最多 700 个边沿（约 350 bit，足够覆盖长帧空调协议）

IRsend irsend(PIN_IR_TX);
IRrecv irrecv(PIN_IR_RX, 2048, 15, true);
decode_results results;

struct Rec {
  uint16_t      n;              // 边沿个数
  decode_type_t type;
  uint64_t      val;
  uint16_t      bits;
  bool          overflow;
  uint16_t      raw[kMaxRaw];   // 单位 µs
};

static Rec     recs[kSlots];
static uint8_t totalRec = 0;    // 累计录制条数
static uint8_t slot     = 0;    // 环形写入位置
static bool    pending  = false;// 有新记录待打印摘要

static void printOne(uint8_t idx) {
  Rec &r = recs[idx];
  Serial.printf("\n[%u] %s  bits=%u  value=0x%016llX  rawlen=%u%s\n",
                (unsigned)(idx + 1),
                typeToString(r.type, false).c_str(),
                (unsigned)r.bits,
                (unsigned long long)r.val,
                (unsigned)r.n,
                r.overflow ? "  (overflow, 已截断)" : "");
  Serial.print("RAW:");
  for (uint16_t i = 0; i < r.n; i++) {
    Serial.print(r.raw[i]);
    Serial.print(i + 1 < r.n ? ',' : '\n');
  }
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println("\n=== _ir_learn · 空调遥控器录制器 ===");
  Serial.printf("TX=GPIO%d  RX=GPIO%d  容量=%d 条 x %d 边沿\n", PIN_IR_TX, PIN_IR_RX, kSlots, kMaxRaw);

  pinMode(PIN_IR_RX, INPUT);
  delay(30);
  int idle = 0;
  for (int i = 0; i < 20; i++) { idle += digitalRead(PIN_IR_RX); delay(2); }
  Serial.printf("RX 静态电平 = %d/20 (%s)\n", idle,
                idle >= 18 ? "接收头就绪" : "接收头异常：未接/未供电/接反");

  irsend.begin();
  irrecv.enableIRIn();
  Serial.println("命令: p=打印全部  c=清空  1~8=重放  n=测试码");
  Serial.println("现在对着接收头按遥控器，每个键间隔 1 秒左右，可以按很多个\n");
}

void loop() {
  // ---- 串口命令 ----
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'p' || c == 'P') {
      if (!totalRec) { Serial.println("(还没有记录)"); continue; }
      uint8_t cnt = totalRec < kSlots ? totalRec : kSlots;
      Serial.printf("\n===== 共 %u 条（按录制顺序）=====\n", (unsigned)cnt);
      for (uint8_t i = 0; i < cnt; i++) {
        uint8_t idx = (totalRec <= kSlots) ? i : (uint8_t)((slot + i) % kSlots);
        printOne(idx);
      }
      Serial.println("===== END =====");
    } else if (c == 'c' || c == 'C') {
      totalRec = 0; slot = 0; pending = false;
      Serial.println("已清空");
    } else if (c >= '1' && c <= '8') {
      uint8_t want = (uint8_t)(c - '1');
      uint8_t cnt = totalRec < kSlots ? totalRec : kSlots;
      if (want >= cnt) { Serial.printf("没有第 %d 条\n", c - '0'); continue; }
      uint8_t idx = (totalRec <= kSlots) ? want : (uint8_t)((slot + want) % kSlots);
      Serial.printf("重放 [%u] %u 边沿 ...\n", (unsigned)(want + 1), (unsigned)recs[idx].n);
      irsend.sendRaw(recs[idx].raw, recs[idx].n, 38);
    } else if (c == 'n' || c == 'N') {
      Serial.println("发送 NEC 0x00FFA25D");
      irsend.sendNEC(0x00FFA25DUL, 32);
    }
  }

  // ---- 红外接收 ----
  if (irrecv.decode(&results)) {
    if (results.rawlen > 1) {
      uint16_t *raw = resultToRawArray(&results);
      if (raw) {
        Rec &r = recs[slot];
        r.n    = results.rawlen - 1;             // rawlen 含结束标记
        if (r.n > kMaxRaw) { r.n = kMaxRaw; }
        for (uint16_t i = 0; i < r.n; i++) r.raw[i] = raw[i];
        r.type     = results.decode_type;
        r.val      = results.value;
        r.bits     = results.bits;
        r.overflow = results.overflow;

        totalRec++;
        slot = (uint8_t)((slot + 1) % kSlots);
        pending = true;
      }
      delete[] raw;
    }
    irrecv.resume();
  }

  // ---- 新记录摘要（实时提示，方便边按边看）----
  if (pending) {
    pending = false;
    uint8_t idx = (totalRec <= kSlots) ? (uint8_t)(totalRec - 1)
                                       : (uint8_t)((slot + kSlots - 1) % kSlots);
    Rec &r = recs[idx];
    Serial.printf("[REC %u] %s bits=%u value=0x%016llX rawlen=%u%s\n",
                  (unsigned)totalRec,
                  typeToString(r.type, false).c_str(),
                  (unsigned)r.bits,
                  (unsigned long long)r.val,
                  (unsigned)r.n,
                  r.overflow ? " overflow" : "");
  }
}
