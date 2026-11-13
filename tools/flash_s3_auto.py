#!/usr/bin/env python3
"""flash_s3_auto.py · 等手环进下载模式 → 自动烧录 → 再等它启动 → 抓 25 秒日志

用法:
    python3 tools/flash_s3_auto.py [sketch名，默认 _wb]

你只需要做两步：
  1) 按住 BOOT → 短按 RST → 松开 BOOT      （脚本自动开始烧录）
  2) 烧完后按一下 RST（不要按 BOOT）        （脚本自动抓日志）

原理：S3 的 EN 没接 RTS，esptool 无法自己复位进下载模式，所以第 1 步必须人手。
其余全自动。
"""
import os
import subprocess
import sys
import time
import glob
import termios
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, ".arduinodata")
ESPTOOL = os.path.join(DATA, "packages/esp32/tools/esptool_py/5.3.1/esptool")
FQBN = ("esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=default,"
        "CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,"
        "PSRAM=opi,LoopCore=1,EventsCore=1,DebugLevel=none")
C3_PORT_HINT = "5CFF0557041"
DL_HINT = "11301"


def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def app_port():
    for p in ports():
        if C3_PORT_HINT in p or DL_HINT in p:
            continue
        return p
    return None


def wait_for(pred, timeout, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        r = pred()
        if r:
            return r
        time.sleep(0.4)
    print("超时：%s（等了 %.0f 秒）" % (label, timeout))
    return None


def main():
    sketch = sys.argv[1] if len(sys.argv) > 1 else "_wb"
    wait_s = float(sys.argv[2]) if len(sys.argv) > 2 else 300.0
    build = os.path.join(ROOT, "build", sketch)
    files = {
        0x0: os.path.join(build, "%s.ino.bootloader.bin" % sketch),
        0x8000: os.path.join(build, "%s.ino.partitions.bin" % sketch),
        0xE000: os.path.join(build, "boot_app0_flashed.bin"),
        0x10000: os.path.join(build, "%s.ino.bin" % sketch),
    }
    for a, f in files.items():
        if not os.path.exists(f):
            print("缺少文件 %s，请先 build %s" % (f, sketch))
            return 1

    if app_port():
        print("当前是应用模式（%s），请按 BOOT→RST→松 BOOT 进入下载模式…" % app_port())
    else:
        print("等待进入下载模式…")

    dl = wait_for(lambda: next((p for p in ports() if DL_HINT in p), None), wait_s,
                  "没等到下载模式端口，按键没生效？")
    if not dl:
        return 1
    print("检测到下载模式 %s，开始烧录…" % dl)

    cmd = [ESPTOOL, "--chip", "esp32s3", "-p", dl, "-b", "921600",
           "--before", "default-reset", "--after", "hard-reset",
           "write-flash", "--flash-mode", "dio", "--flash-size", "16MB"]
    for addr, f in files.items():
        cmd += ["0x%X" % addr, f]
    r = subprocess.run(cmd, capture_output=True, text=True)
    ok = r.returncode == 0
    for line in r.stdout.splitlines() + r.stderr.splitlines():
        if any(k in line for k in ("Wrote ", "Hash of data", "error", "Error", "Failed")):
            print("   " + line.strip())
    if not ok:
        print("!! 烧录失败，请重新 BOOT→RST 再试")
        return 1

    print("\n烧录完成。现在按一下 RST（不要按 BOOT），等 25 秒抓日志…\n")
    t0 = time.time()
    app = wait_for(app_port, 180, "没等到应用口，是否忘了按 RST？")
    if not app:
        return 1
    time.sleep(2.0)

    fd = os.open(app, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    a[2] |= termios.CLOCAL | termios.CREAD
    a[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, a)

    print("===== 抓 %s 25s =====" % app)
    n = 0
    t0 = time.time()
    try:
        while time.time() - t0 < 25:
            try:
                d = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            except OSError:
                print("!! 端口断开")
                break
            if d:
                n += len(d)
                sys.stdout.write(d.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        os.close(fd)
    print("\n===== 共 %d 字节 =====" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
