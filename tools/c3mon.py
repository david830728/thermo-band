#!/usr/bin/env python3
"""c3mon.py · 在不复位 ESP32-C3 的前提下读写串口

为什么需要它：这块 C3 板用 CH343 转串口，DTR/RTS 接在 EN/GPIO9 上，
用 pyserial 打开端口会触发一次复位，芯片 RAM 里的录制数据就被清空。
这里直接 os.open + termios，完全不碰 modem 控制线。

用法:
  python3 tools/c3mon.py --send p --secs 6          # 发命令 p 并读取 6 秒
  python3 tools/c3mon.py --secs 5                   # 只读，不发命令
  python3 tools/c3mon.py --send n --secs 3 --quiet  # 读但不打印（用来看是否复位）
"""
import argparse
import os
import select
import sys
import termios
import time
import tty

PORT_DEFAULT = "/dev/cu.usbmodem5CFF0557041"


def open_raw(port):
    """打开串口并设为 raw 模式，不触碰 DTR/RTS。"""
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)                      # 关掉回显/行缓冲/信号处理
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=PORT_DEFAULT)
    ap.add_argument("--send", default=None, help="要发送的命令字符串")
    ap.add_argument("--secs", type=float, default=5.0)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--pre", type=float, default=0.3, help="发送前静默等待秒数")
    a = ap.parse_args()

    fd = open_raw(a.port)

    # 只读取当前可能在缓冲里的残留，不做 reset_input_buffer（那会额外碰 ioctl）
    time.sleep(a.pre)

    if a.send:
        os.write(fd, a.send.encode())

    t0 = time.time()
    out = bytearray()
    while time.time() - t0 < a.secs:
        r, _, _ = select.select([fd], [], [], 0.2)
        if not r:
            continue
        try:
            chunk = os.read(fd, 8192)
        except BlockingIOError:
            continue
        if chunk:
            out += chunk

    os.close(fd)
    if not a.quiet:
        sys.stdout.write(out.decode("utf-8", "replace"))
        sys.stdout.write("\n=== LEN %d\n" % len(out))
    else:
        print("LEN %d" % len(out))


if __name__ == "__main__":
    main()
