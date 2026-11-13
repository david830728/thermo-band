#!/usr/bin/env python3
"""mon2.py · 同时监听两块板（C3 中枢 + S3 手环），用于对照实验

不触碰 DTR/RTS，所以不会复位任何一块板。

用法:
  python3 tools/mon2.py --secs 20
  python3 tools/mon2.py --secs 20 --send "env\n"     # 给 C3 发命令
"""
import argparse
import os
import re
import select
import sys
import termios
import time
import tty

C3_DEFAULT = "/dev/cu.usbmodem5CFF0557041"
S3_DEFAULT = "/dev/cu.usbmodemB81F3FD775701"


def open_raw(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CRTSCTS
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c3", default=C3_DEFAULT)
    ap.add_argument("--s3", default=S3_DEFAULT)
    ap.add_argument("--secs", type=float, default=15.0)
    ap.add_argument("--send", default=None, help="发给 C3 的命令（含换行）")
    ap.add_argument("--raw", action="store_true", help="打印所有原始行")
    a = ap.parse_args()

    fds = {}
    for tag, port in (("C3", a.c3), ("S3", a.s3)):
        try:
            fds[open_raw(port)] = [tag, b""]
        except Exception as e:
            print("跳过 %s (%s): %s" % (tag, port, e))

    time.sleep(0.3)
    if a.send:
        for fd, (tag, _) in fds.items():
            if tag == "C3":
                os.write(fd, a.send.encode())

    t0 = time.time()
    while time.time() - t0 < a.secs:
        r, _, _ = select.select(list(fds), [], [], 0.2)
        for fd in r:
            tag, buf = fds[fd]
            try:
                chunk = os.read(fd, 8192)
            except Exception:
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("utf-8", "replace").strip()
                if not s:
                    continue
                tt = time.time() - t0
                m = re.search(r"ENV: T=([\d.]+) C.*?→ ([\d.]+) C\)\s+RH=([\d.]+)", s)
                if m:
                    print("%6.1fs  C3-DHT11 T=%5s → 修正 %5s  RH=%s" % (tt, m.group(1), m.group(2), m.group(3)))
                    continue
                m = re.search(r"ENV: T=([\d.]+) C\s+RH=([\d.]+)", s)
                if m:
                    print("%6.1fs  C3-DHT11 T=%5s RH=%s" % (tt, m.group(1), m.group(2)))
                    continue
                m = re.search(r"m601=(-?[\d.]+)C", s)
                if m:
                    print("%6.1fs  S3-M601  T=%5s" % (tt, m.group(1)))
                    continue
                if a.raw:
                    print("%6.1fs  %s | %s" % (tt, tag, s))
            fds[fd][1] = buf          # ★ 必须无条件回存，否则跨 read 的半行会丢

    for fd in fds:
        try:
            os.close(fd)
        except Exception:
            pass


if __name__ == "__main__":
    main()
