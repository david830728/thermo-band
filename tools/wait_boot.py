#!/usr/bin/env python3
"""wait_boot.py · 等 S3 从下载模式复位进应用模式，然后自动抓 20 秒输出

用法: python3 tools/wait_boot.py [--secs 120] [--watch 20]
不触碰 DTR/RTS，纯被动监听。
"""
import argparse
import glob
import os
import sys
import time

import termios
import tty


def open_raw(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    a = termios.tcgetattr(fd)
    a[4] = a[5] = termios.B115200
    a[2] |= termios.CLOCAL | termios.CREAD
    a[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def ports():
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--secs", type=float, default=120.0, help="等复位的最长时间")
    ap.add_argument("--watch", type=float, default=20.0, help="进应用后抓多久")
    args = ap.parse_args()

    boot = [p for p in ports() if "11301" in p]
    if not boot:
        print("当前不在下载模式，直接监视应用口")
    t0 = time.time()
    target = None
    while time.time() - t0 < args.secs:
        cur = ports()
        # S3 应用口 = 非 11301 且非 C3 的口
        cand = [p for p in cur if "11301" not in p and "5CFF0557041" not in p]
        if cand:
            target = cand[0]
            print("[%5.1fs] 检测到应用口 %s" % (time.time() - t0, target))
            break
        time.sleep(0.5)
    if not target:
        print("超时：仍未见应用口出现。可能板子还停在下载模式。")
        return 1

    time.sleep(2.0)
    try:
        fd = open_raw(target)
    except OSError as e:
        print("打开 %s 失败: %s" % (target, e))
        return 1
    print("===== 开始抓 %s %.0fs =====" % (target, args.watch))
    buf = b""
    t0 = time.time()
    try:
        while time.time() - t0 < args.watch:
            try:
                d = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.02)
                continue
            except OSError:
                print("!! 端口断开（可能崩溃重启）")
                break
            if d:
                buf += d
                txt = d.decode("utf-8", "replace")
                sys.stdout.write(txt)
                sys.stdout.flush()
    finally:
        os.close(fd)
    print("\n===== 共 %d 字节 =====" % len(buf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
