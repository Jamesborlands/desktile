# World coastline for the Space screen mini map: Natural Earth 110m, simplified.
# Usage: python3 worldgen.py ne_110m_coastline.geojson ../src/worlddata.h
import json, math, sys
src, out = sys.argv[1:3]
TOL = 0.7  # degrees

def rdp(pts, tol):
    if len(pts) < 3: return pts
    a, b = pts[0], pts[-1]
    dx, dy = b[0] - a[0], b[1] - a[1]
    n = math.hypot(dx, dy) or 1e-12
    dmax, idx = 0, 0
    for i in range(1, len(pts) - 1):
        d = abs(dy * (pts[i][0] - a[0]) - dx * (pts[i][1] - a[1])) / n
        if d > dmax: dmax, idx = d, i
    if dmax > tol: return rdp(pts[:idx + 1], tol)[:-1] + rdp(pts[idx:], tol)
    return [a, b]

lines = []
for f in json.load(open(src))['features']:
    g = f['geometry']
    for part in (g['coordinates'] if g['type'] == 'MultiLineString' else [g['coordinates']]):
        xs = [p[0] for p in part]; ys = [p[1] for p in part]
        if max(xs) - min(xs) < 3 and max(ys) - min(ys) < 3: continue  # tiny islands vanish at this size
        if len(part) > 3 and part[0] == part[-1]:
            m = len(part) // 2
            s = rdp(part[:m + 1], TOL)[:-1] + rdp(part[m:], TOL)
        else:
            s = rdp(part, TOL)
        if len(s) > 1: lines.append(s)

with open(out, 'w') as f:
    f.write('// World coastline (Natural Earth 110m, public domain) simplified by worldgen.py. Do not edit.\n')
    f.write('#pragma once\n#include <stdint.h>\n\n// (lon, lat) in tenths of a degree\nstatic const int16_t WORLD_PTS[][2] = {\n')
    starts, total = [], 0
    for s in lines:
        starts.append((total, len(s)))
        f.write('  ' + ' '.join('{%d,%d},' % (round(x * 10), round(y * 10)) for x, y in s) + '\n')
        total += len(s)
    f.write('};\nstruct WorldLine { uint16_t start, count; };\nstatic const WorldLine WORLD_LINES[] = {\n')
    f.write(''.join('  {%d, %d},\n' % st for st in starts))
    f.write('};\nstatic const int WORLD_LINE_COUNT = %d;\n' % len(starts))
print(len(lines), 'lines,', sum(len(s) for s in lines), 'points')
