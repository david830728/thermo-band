#!/bin/zsh
# 智能温控手环 · macOS 环境脚本（2026-11-12 搭建，实测可用）
# 用法:
#   source env.sh                     # 加载环境变量
#   flash  _smoke                     # 编译+烧录（自动找 firmware/<name> 或工作区内任意 sketch）
#   flash  firmware/_smoke            # 也可给相对/绝对路径
#   build  _smoke                     # 只编译，产物在 build/<name>/
#   mon                               # 串口监视（默认 20 秒）
#   mon 60                            # 监视 60 秒
#   port                              # 查看当前串口设备
#
# ⚠️ 本板烧录必须手动进 Boot：按住 BOOT → 短按 RST → 松开 BOOT
#    （USBMode=default 下应用占用了 USB，esptool 无法自动复位进下载模式，实测确认）

export ARDUINO_DIRECTORIES_DATA="/Users/david/Documents/ESP32/.arduinodata"
export ARDUINO_DIRECTORIES_USER="/Users/david/Documents/ESP32/.arduino-user"
export NO_PROXY="127.0.0.1,localhost"
export no_proxy="127.0.0.1,localhost"
# 网络备忘: 本机代理(127.0.0.1:59637)对 GitHub 系 502；git 协议直连可用；乐鑫 CDN(dl.espressif.cn) 通
export ROOT="/Users/david/Documents/ESP32"

export FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,LoopCore=1,EventsCore=1,DebugLevel=none"
# ⚠️ FlashMode 必须 dio（qio 无限重启）；PSRAM 必须 opi；逐字照抄交接文档

# ===== ESP32-C3（红外控制器）=====
# ⚠️ 关键：C3 板的 USB 口接的是 WCH 转串口芯片（idVendor=0x1A86 "USB Single Serial"），
#    走 UART0(GPIO20/21)，不是原生 USB。所以 **CDCOnBoot 必须是 default**，
#    一旦设成 cdc，Serial 会被映射到 GPIO18/19 原生 USB 引脚 → 串口永远静默（踩过坑）。
#    FlashMode 同样必须 dio。
export FQBN_C3="esp32:esp32:esp32c3:CDCOnBoot=default,CPUFreq=160,FlashMode=dio,FlashSize=4M,PartitionScheme=default,UploadSpeed=921600"
# C3 走 USB-UART 桥，DTR/RTS 已接 EN/GPIO9，烧录全程真正自动化（无需按键）
export C3_PORT="/dev/cu.usbmodem5CFF0557041"     # C3 应用/烧录同一个口
export S3_PORT="/dev/cu.usbmodemB81F3FD775701"   # S3 应用模式口（下载模式会变成 usbmodem11x01）

AC="$HOME/bin/arduino-cli"
ESPTOOL="$ARDUINO_DIRECTORIES_DATA/packages/esp32/tools/esptool_py/5.3.1/esptool"

# 串口: 应用模式 = usbmodem<MAC序列号>，Boot 模式 = usbmodem11101（会变，统一用通配）
port() { ls /dev/cu.usbmodem* 2>/dev/null || echo "未找到板子（检查数据线 / ls /dev/cu.*）"; }

# 把参数解析成存在的 sketch 目录
_resolve_sketch() {
  local s="$1"
  for c in "$s" "$ROOT/$s" "$ROOT/firmware/$s"; do
    [ -d "$c" ] && { echo "$c"; return 0; }
  done
  return 1
}

build() {
  local SKETCH="$1"; [ -z "$SKETCH" ] && { echo "用法: build <sketch>"; return 1; }
  local DIR=$(_resolve_sketch "$SKETCH") || { echo "找不到 sketch: $SKETCH"; return 1; }
  local NAME=$(basename "$DIR")
  echo "== 编译 $DIR =="
  "$AC" compile --fqbn "$FQBN" --output-dir "$ROOT/build/$NAME" "$DIR" 2>&1 | grep -vE "discovery not found" | grep -E "Sketch uses|Global variables|error|Error|warning: unused" | head -12
}

flash() {
  local SKETCH="$1"; [ -z "$SKETCH" ] && { echo "用法: flash <sketch>"; return 1; }
  local DIR=$(_resolve_sketch "$SKETCH") || { echo "找不到 sketch: $SKETCH"; return 1; }
  local NAME=$(basename "$DIR")
  local OUT="$ROOT/build/$NAME"

  echo "== 编译 $NAME =="
  "$AC" compile --fqbn "$FQBN" --output-dir "$OUT" "$DIR" 2>&1 | grep -vE "discovery not found" | grep -E "Sketch uses|Global variables|error|Error" | head -8 || return 1

  local P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  [ -z "$P" ] && { echo "未找到串口，插好板子"; return 1; }

  echo "== 烧录 -> $P =="
  if "$AC" upload -p "$P" --fqbn "$FQBN" --input-dir "$OUT" 2>&1 | grep -vE "discovery not found" | tail -5; then
    echo "== 完成 =="
    echo "   若串口无输出：按一下 RST（不要按 BOOT）让应用启动"
  else
    echo "!! 上传失败，通常是要先进 Boot 模式："
    echo "   按住 BOOT → 短按 RST → 松开 BOOT，然后重新执行 flash $NAME"
    echo "   端口此时会变成 /dev/cu.usbmodem11101，脚本会自动选到它"
  fi
}

# ===== C3 专用：编译 / 烧录 / 监视（全程免按键）=====
buildc3() {
  local SKETCH="$1"; [ -z "$SKETCH" ] && { echo "用法: buildc3 <sketch>"; return 1; }
  local DIR=$(_resolve_sketch "$SKETCH") || { echo "找不到 sketch: $SKETCH"; return 1; }
  local NAME=$(basename "$DIR")
  echo "== [C3] 编译 $DIR =="
  # 注入编译时刻的 UNIX 时间戳，作为中枢的时间基准（手环靠下行包拿时间，
  # 手环自己绝不能连 WiFi 校时——那会改信道，把 ESP-NOW 打断）
  "$AC" compile --fqbn "$FQBN_C3" --output-dir "$ROOT/build/$NAME" \
      --build-property "compiler.cpp.extra_flags=-DESP32_BUILD_EPOCH=$(date +%s)" \
      "$DIR" 2>&1 | grep -vE "discovery not found" | grep -E "Sketch uses|Global variables|error|Error" | head -12
}

# 给 C3 校时（不碰 DTR/RTS，不会复位板子）
settime() {
  local E=${1:-$(date +%s)}
  python3 -c "
import os,termios,tty,time
p='${C3_PORT}'
fd=os.open(p, os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
tty.setraw(fd)
a=termios.tcgetattr(fd); a[4]=a[5]=termios.B115200; a[2]|=termios.CLOCAL|termios.CREAD
termios.tcsetattr(fd,termios.TCSANOW,a)
time.sleep(0.2)
os.write(fd,b'time $E\n')
time.sleep(0.6)
try: print(os.read(fd,4096).decode('utf-8','replace').strip())
except BlockingIOError: pass
os.close(fd)
"
  echo "== [C3] 时间已设为 epoch=$E =="
}

flashc3() {
  local SKETCH="$1"; [ -z "$SKETCH" ] && { echo "用法: flashc3 <sketch>"; return 1; }
  local DIR=$(_resolve_sketch "$SKETCH") || { echo "找不到 sketch: $SKETCH"; return 1; }
  local NAME=$(basename "$DIR")
  local OUT="$ROOT/build/$NAME"
  buildc3 "$NAME" || return 1
  local P="${C3_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v B81F3FD775701 | head -1)}"
  [ -z "$P" ] && { echo "未找到 C3 串口"; return 1; }
  echo "== [C3] 烧录 -> $P =="
  "$AC" upload -p "$P" --fqbn "$FQBN_C3" --input-dir "$OUT" 2>&1 | grep -vE "discovery not found" | grep -E "Wrote|Hash|error|Error" | tail -6
  # 烧完顺手校时（烧录会重启，等它起来）
  sleep 3
  settime
}

monc3() {
  local SECS=${1:-15}
  local P="${C3_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | grep -v B81F3FD775701 | head -1)}"
  [ -z "$P" ] && { echo "未找到 C3 串口"; return 1; }
  echo "== [C3] 监视 $P ${SECS}s =="
  python3 -c "
import serial,time,sys
s=serial.Serial(port='$P',baudrate=115200,timeout=0.3)
time.sleep(0.3)
s.dtr=False; s.rts=False
time.sleep(0.3); s.reset_input_buffer()
t=time.time()
try:
    while time.time()-t<$SECS:
        d=s.read(4096)
        if d: sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
finally:
    s.close()
"
}

mon() {
  local SECS=${1:-20}
  local P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  [ -z "$P" ] && { echo "未找到串口"; return 1; }
  echo "== 监视 $P ${SECS}s =="
  python3 -c "
import serial,time,sys
s=serial.Serial(port='$P',baudrate=115200,timeout=0.3)
time.sleep(0.2)
s.dtr=True; s.rts=False
t=time.time()
try:
    while time.time()-t<$SECS:
        d=s.read(4096)
        if d: sys.stdout.write(d.decode('utf-8','replace')); sys.stdout.flush()
finally:
    s.close()
"
}

echo "环境已加载: arduino-cli=$($AC version 2>/dev/null | awk '{print $2}')  FQBN=esp32s3@3.3.12(dio/opi)"
