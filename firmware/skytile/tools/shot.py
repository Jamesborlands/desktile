#!/usr/bin/env python3
"""Grab a screenshot from the tile over USB serial (firmware answers 'S').

usage: shot.py [port] [out.png]
"""
import glob
import struct
import sys
import time
import zlib

import serial

port = sys.argv[1] if len(sys.argv) > 1 else sorted(glob.glob("/dev/cu.wchusbserial*"))[0]
out = sys.argv[2] if len(sys.argv) > 2 else "shot.png"

s = serial.Serial(port, 115200, timeout=10)
s.dtr = False
s.rts = False
time.sleep(0.2)
s.reset_input_buffer()
s.write(b"S")

w = h = None
rows = {}
deadline = time.time() + 120
while time.time() < deadline:
    line = s.readline().decode(errors="replace").strip()
    if line.startswith("SHOT"):
        w, h = map(int, line.split()[1:3])
    elif line == "END" and w:
        break
    elif line.startswith("R") and w and " " in line:  # other lines are the tile's logs
        y, data = line[1:].split(" ", 1)
        try:
            b = bytes.fromhex(data)
        except ValueError:
            continue
        px = bytearray()
        for i in range(0, len(b) - 2, 3):
            n, c = b[i], b[i + 1] | b[i + 2] << 8
            px += bytes(((c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3)) * n
        if len(px) == w * 3:
            rows[int(y)] = bytes(px)
if not w:
    sys.exit("no reply from the tile")
missing = [y for y in range(h) if y not in rows]
if missing:
    print(f"{len(missing)} rows garbled, shown in magenta")
blank = bytes((255, 0, 255)) * w
raw = b"".join(b"\0" + rows.get(y, blank) for y in range(h))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
with open(out, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
print(f"saved {out}")
