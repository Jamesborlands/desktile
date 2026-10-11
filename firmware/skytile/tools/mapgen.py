# Clip Natural Earth lines to a box around home and emit a C header of polylines.
import json, sys, math
coast, border, out = sys.argv[1:4]  # coast: Overpass JSON (OSM natural=coastline)
LAT0, LON0 = 32.95, -117.25
BOX = (LAT0 - 1.75, LAT0 + 1.75, LON0 - 2.15, LON0 + 2.15)  # ~105 nm each way
TOL = 0.0009  # degrees, simplification tolerance (~0.1 nm)

def inside(p): return BOX[0] <= p[1] <= BOX[1] and BOX[2] <= p[0] <= BOX[3]

def osm_lines(path):
    ways = [[(g['lon'], g['lat']) for g in w['geometry']] for w in json.load(open(path))['elements']]
    # stitch ways that share endpoints so we draw fewer, longer polylines
    merged = True
    while merged:
        merged = False
        ends = {}
        for i, w in enumerate(ways):
            ends.setdefault(w[0], []).append(i)
        for i, w in enumerate(ways):
            nxt = [j for j in ends.get(w[-1], []) if j != i and ways[j]]
            if w and nxt:
                j = nxt[0]
                ways[i] = w + ways[j][1:]; ways[j] = []
                merged = True
                break
        ways = [w for w in ways if w]
    for part in ways:
        run = []
        for i, p in enumerate(part):
            if inside(p) or (i + 1 < len(part) and inside(part[i + 1])) or (run and inside(part[i - 1])):
                run.append(p)
            elif run:
                yield run; run = []
        if len(run) > 1: yield run

def lines(path):
    for f in json.load(open(path))['features']:
        g = f['geometry']
        parts = g['coordinates'] if g['type'] == 'MultiLineString' else [g['coordinates']]
        for part in parts:
            run = []
            for i, p in enumerate(part):
                if inside(p) or (i + 1 < len(part) and inside(part[i + 1])) or (run and inside(part[i - 1])):
                    run.append(p)
                elif run:
                    yield run; run = []
            if len(run) > 1: yield run

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

polys = []
for kind, gen in (('coast', osm_lines(coast)), ('border', lines(border))):
    for run in gen:
        if len(run) > 3 and run[0] == run[-1]:  # closed loop (island): simplify each half
            m = len(run) // 2
            s = rdp(run[:m + 1], TOL)[:-1] + rdp(run[m:], TOL)
        else:
            s = rdp(run, TOL)
        if len(s) > 1: polys.append((kind, s))

with open(out, 'w') as f:
    f.write('// Generated from OpenStreetMap coastline (ODbL, (c) OSM contributors) and Natural Earth border by mapgen.py. Do not edit.\n')
    f.write('#pragma once\n#include <stdint.h>\n\n')
    f.write('// Points are (lat, lon) in 1e-4 degree units.\n')
    f.write('static const int32_t MAP_PTS[][2] = {\n')
    starts, total = [], 0
    for kind, s in polys:
        starts.append((total, len(s), 1 if kind == 'border' else 0))
        for lon, lat in s:
            f.write('  {%d, %d},\n' % (round(lat * 1e4), round(lon * 1e4)))
        total += len(s)
    f.write('};\n\nstruct MapLine { uint16_t start, count; uint8_t border; };\n')
    f.write('static const MapLine MAP_LINES[] = {\n')
    for st, c, b in starts: f.write('  {%d, %d, %d},\n' % (st, c, b))
    f.write('};\nstatic const int MAP_LINE_COUNT = %d;\n' % len(starts))
print(len(polys), 'lines,', sum(len(s) for _, s in polys), 'points')

