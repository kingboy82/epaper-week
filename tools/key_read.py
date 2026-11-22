#!/usr/bin/env python3
"""按键探测读取器：复位芯片到正常启动模式，读取 NVS 里的按键累计值。

用法: key_read.py [端口] [秒数]
"""
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-2110"
SECS = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False          # 打开后立刻释放 DTR，保持 IO0 高
ser.rts = False
ser.open()
try:
    ser.dtr = False
    ser.rts = True       # 拉 EN 复位
    time.sleep(0.12)
    ser.rts = False
    ser.reset_input_buffer()

    buf = bytearray()
    end = time.time() + SECS
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
    sys.stdout.write(buf.decode("utf-8", "replace").replace("\r", ""))
finally:
    ser.close()
