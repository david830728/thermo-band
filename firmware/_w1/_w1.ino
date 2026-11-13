// _w1 · 通用 1-Wire 侦察器（不预设型号）
//
// 为什么重写：probe3 的 presenceWidth() 在释放后"等低电平"，而 1-Wire 器件
// 是释放后 15~60us 才拉低。线从低爬升的那 1~2us 就被误判成"应答结束"，
// 于是返回 1~2us。真正 presence 脉冲被漏掉。
//
// 本固件做法：释放后以微秒级轮询，记录 3ms 内所有电平边沿及时间戳。
// 无论对方是 DS18B20(0x28)、MAX30207(0x42)、还是任意克隆片，
// 只要它拉低过线，这里一定会看到。
//
// 依次测试：空闲电平 → 480us 复位全程记录 → 读 ROM(0x33) → 120/250/750us
// 复位对比 → overdrive(48us) 复位 → ADC 电压（兜底判断是否为模拟 NTC）

#include <Arduino.h>

static const int PINS[] = {17, 18, 2, 3};
static const int NPINS = sizeof(PINS) / sizeof(PINS[0]);
static const int MAXEDGE = 48;

struct Edge { uint32_t t; uint8_t v; };

static inline void owLow(int p) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
static inline void owRel(int p) { pinMode(p, INPUT_PULLUP); }

// 复位 + 全程记录边沿。返回边沿数
static int capture(int p, uint16_t lowUs, Edge *es, uint32_t spanUs, bool relPullup) {
  owLow(p);
  delayMicroseconds(lowUs);
  uint32_t t0 = micros();
  if (relPullup) pinMode(p, INPUT_PULLUP); else pinMode(p, INPUT);
  int n = 0, last = 1;   // 记录起点视为 HIGH（释放后应有上拉）
  while ((uint32_t)(micros() - t0) < spanUs && n < MAXEDGE) {
    uint32_t now = micros();
    int v = digitalRead(p);
    if (v != last) {
      es[n].t = now - t0;
      es[n].v = (uint8_t)v;
      n++; last = v;
    }
  }
  return n;
}

// 打印边沿，并给出"最长低电平段"（= presence 候选）
static void report(int p, uint16_t lowUs, bool relPullup, const char *tag) {
  static Edge es[MAXEDGE];
  int n = capture(p, lowUs, es, 3000, relPullup);

  Serial.printf("  [%s] reset=%uus pullup=%d edges=%d", tag, lowUs, relPullup ? 1 : 0, n);
  if (n == 0) { Serial.println("  -> 全程恒高，无任何拉低"); return; }

  // 找最长低电平段
  uint32_t bestDur = 0, bestStart = 0;
  for (int i = 0; i < n; i++) {
    if (es[i].v == 0) {
      uint32_t end = (i + 1 < n) ? es[i + 1].t : 3000;
      uint32_t dur = end - es[i].t;
      if (dur > bestDur) { bestDur = dur; bestStart = es[i].t; }
    }
  }
  Serial.printf("  | 最长低电平: 起始+%luus 宽度%luus", (unsigned long)bestStart, (unsigned long)bestDur);
  if (bestDur >= 50 && bestDur <= 300 && bestStart < 120)
    Serial.print("  <= 真 1-Wire presence! ✓");
  else if (bestDur < 10)
    Serial.print("  <= 只是放电极短脉冲 ✗");
  Serial.println();

  // 边沿明细（前 12 个足够看清形状）
  Serial.print("        ");
  for (int i = 0; i < n && i < 12; i++)
    Serial.printf("%s+%lu ", es[i].v ? "H" : "L", (unsigned long)es[i].t);
  Serial.println();
}

static void owWriteBit(int p, int b) {
  owLow(p);
  if (b) { delayMicroseconds(5); owRel(p); delayMicroseconds(60); }
  else   { delayMicroseconds(60); owRel(p); delayMicroseconds(5); }
}
static int owReadBit(int p) {
  owLow(p);
  delayMicroseconds(3);
  owRel(p);
  delayMicroseconds(9);          // 采样点 ~12us，在 15us 读窗口内
  int v = digitalRead(p);
  delayMicroseconds(50);
  return v;
}
static void owWriteByte(int p, uint8_t b) {
  for (int i = 0; i < 8; i++) { owWriteBit(p, b & 1); b >>= 1; }
}
static uint8_t owReadByte(int p) {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) { b >>= 1; if (owReadBit(p)) b |= 0x80; }
  return b;
}
static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0;
  while (n--) {
    uint8_t b = *d++;
    for (int i = 0; i < 8; i++) {
      uint8_t mix = (c ^ b) & 1;
      c >>= 1;
      if (mix) c ^= 0x8C;
      b >>= 1;
    }
  }
  return c;
}

// 标准复位（返回 true 表示检测到 presence，按 50~300us 低电平判定）
static bool owReset(int p, uint16_t lowUs) {
  static Edge es[MAXEDGE];
  int n = capture(p, lowUs, es, 600, true);
  for (int i = 0; i < n; i++) {
    if (es[i].v == 0) {
      uint32_t end = (i + 1 < n) ? es[i + 1].t : 600;
      uint32_t dur = end - es[i].t;
      if (dur >= 50 && dur <= 300 && es[i].t < 120) return true;
    }
  }
  return false;
}

static void probePin(int p) {
  Serial.printf("\n========== GPIO%d ==========\n", p);

  // 1. 空闲电平
  pinMode(p, INPUT);
  delay(3);
  int f = digitalRead(p);
  pinMode(p, INPUT_PULLUP);
  delay(3);
  int u = digitalRead(p);
  Serial.printf("  空闲: 无上拉=%d 内部上拉=%d\n", f, u);

  // 2. 标准复位，全程记录
  report(p, 480, true, "标准480us");

  // 3. 其它复位宽度（有些克隆片/体温片要短或长）
  report(p, 120, true, "短复位120us");
  report(p, 250, true, "中复位250us");
  report(p, 750, true, "长复位750us");
  report(p, 48,  true, "过驱48us");

  // 4. 若标准复位成功，读 ROM
  if (owReset(p, 480)) {
    Serial.println("  >>> 检测到 presence，尝试 READ ROM (0x33)");
    owWriteByte(p, 0x33);
    uint8_t rom[8];
    for (int i = 0; i < 8; i++) rom[i] = owReadByte(p);
    Serial.print("  ROM:");
    for (int i = 0; i < 8; i++) Serial.printf(" %02X", rom[i]);
    Serial.println();
    uint8_t c = crc8(rom, 7);
    Serial.printf("  CRC: 计算=%02X 芯片=%02X -> %s   family=0x%02X\n",
                  c, rom[7], (c == rom[7]) ? "OK ✓" : "BAD ✗", rom[0]);
    if (rom[0] == 0x28) Serial.println("  ^^ family 0x28 = DS18B20 系列");
    else if (rom[0] == 0x42) Serial.println("  ^^ family 0x42 = MAX30207/31820 类体温片");
    else if (rom[0] == 0x00 || rom[0] == 0xFF) Serial.println("  ^^ ROM 全 0/全 F，读时隙时序不对，不是真 ROM");
  }

  // 5. ADC 兜底：万一是模拟 NTC 分压输出
  int mv = analogReadMilliVolts(p);
  Serial.printf("  ADC: %d mV（若为 0 或 3300 基本可排除模拟量输出）\n", mv);
}

void setup() {
  Serial.begin(115200);
  delay(1800);
  Serial.println("\n\n=== _w1 通用 1-Wire 侦察器 ===");
  Serial.println("不预设型号：记录复位后 3ms 内全部电平边沿");
  for (int i = 0; i < NPINS; i++) probePin(PINS[i]);
  Serial.println("\n=== 扫描结束，3 秒后重复 ===");
}

void loop() {
  delay(3000);
  Serial.println("\n--- 第二轮 ---");
  for (int i = 0; i < NPINS; i++) probePin(PINS[i]);
}
