import re, sys, zlib, struct, math
h = open(sys.argv[1]).read()
pts = [tuple(map(int, m)) for m in re.findall(r'\{(-?\d+), (-?\d+)\},', h.split('MAP_LINES')[0])]
lines = [tuple(map(int, m)) for m in re.findall(r'\{(\d+), (\d+), (\d)\},', h.split('MAP_LINES[]')[1])]
W, H = 500, 436; LAT0, LON0 = 32.95, -117.25; RANGE = 100
ppn = (H / 2 - 20) / RANGE; k = math.cos(math.radians(LAT0)) * 60
img = bytearray(b'\x0a\x12\x20' * W * H)
def px(x, y, c):
    if 0 <= x < W and 0 <= y < H: img[(y * W + x) * 3:(y * W + x) * 3 + 3] = c
def proj(p): return (W / 2 + (p[1] / 1e4 - LON0) * k * ppn, H / 2 - (p[0] / 1e4 - LAT0) * 60 * ppn)
for st, n, b in lines:
    c = b'\xc0\x60\x60' if b else b'\x5a\x8a\xd0'
    for i in range(st, st + n - 1):
        (x0, y0), (x1, y1) = proj(pts[i]), proj(pts[i + 1])
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for t in range(steps + 1):
            px(int(x0 + (x1 - x0) * t / steps), int(y0 + (y1 - y0) * t / steps), c)
for dx in range(-2, 3):
    for dy in range(-2, 3): px(W // 2 + dx, H // 2 + dy, b'\xff\xff\xff')
raw = b''.join(b'\x00' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
