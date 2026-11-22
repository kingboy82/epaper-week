#!/usr/bin/env python3
"""抓一段串口原始输出（纯标准库，不依赖 pyserial）。

用法: serial_dump.py [端口] [秒数]
不依赖 DTR/RTS 复位，设备保持运行；只读取字节流打印到 stdout。
"""
import os
import select
import sys
import termios
import time

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-2110"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                  # iflag: raw
    attrs[1] = 0                                  # oflag: raw
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                  # lflag: raw
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)

    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace").replace("\r", ""))
                sys.stdout.flush()
finally:
    os.close(fd)
